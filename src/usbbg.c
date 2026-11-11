/* nDrive - background MTP transport. See usbbg.h.
 *
 * MTP containers on EP2 next to the OS's TI link on EP1. The OS keeps
 * EP0 (descriptors, SET_CONFIGURATION, standard requests) and its own FIFOs;
 * the hooks give EP2 FIFO2 (OUT) and FIFO3 (IN) and hide their status bits
 * from the OS. nDrive moves data with DMA channels 3 and 4 (memory <-> FIFO,
 * as the OS does on channels 1 and 2) and polls their status.
 *
 * Copyright (C) 2026 Rodrigo. GNU GPL v3 or later.
 */
#include "usbbg.h"

#include <string.h>

#include "resident.h"
#include "usbhook.h"

#define USB_BASE 0xb0000000u
#define TIMER_32K (*(volatile uint32_t *)0x900d0024u)
#define RTC_SECONDS (*(volatile uint32_t *)0x90090000u)
#define DATA_BUF 16384u

enum {
    GISR0 = 0x144, GISR1 = 0x148, GISR2 = 0x14c, FIFO2 = 0x1b8, FIFO3 = 0x1bc,
    DMA3 = 0x318, DMA4 = 0x320, DMASR = 0x328, EP2_IN = 0x164, TX0BYTE = 0x154, RX0BYTE = 0x150
};
#define OUT_CH 3u
#define IN_CH 4u

static inline uint32_t rd(unsigned off) { return *(volatile uint32_t *)(USB_BASE + off); }
static inline void wr(unsigned off, uint32_t v) { *(volatile uint32_t *)(USB_BASE + off) = v; }

/* DMA buffers: in the OS's uncached memory when available (see usbhook_pool),
   else these cached ones with explicit cache maintenance. */
static uint8_t data_static[DATA_BUF] __attribute__((aligned(32)));
static uint8_t cbw_static[512] __attribute__((aligned(32)));
static uint8_t csw_static[32] __attribute__((aligned(32)));
static uint8_t *data_buf, *cbw_buf, *csw_buf;
static bool uncached;
#define CBW_BUF 512u

static void buffers(void) {
    if (data_buf) return;
    uint8_t *pool = usbhook_pool();
    uncached = pool != NULL;
    data_buf = pool ? pool : data_static;                   /* 0x0000..0x3fff */
    cbw_buf = pool ? pool + 0x4000 : cbw_static;            /* 0x4000..0x41ff */
    csw_buf = pool ? pool + 0x4200 : csw_static;
}

bool usb_uncached_buffers(void) { buffers(); return uncached; }

static struct {
    uint32_t ms, frac;
    uint16_t last_tick;
    uint16_t stuck;
    bool timer_started;
    bool up;                    /* the counter counts up */
    bool small;                 /* one packet per IN DMA */
    uint32_t rate;              /* counter ticks per second (measured) */
    uint32_t mps;               /* bulk packet size on the bus */
    uint32_t container;         /* bytes sent of the current container */
    Mtp *mtp;
    uint32_t rtc_base, ms_base;
    uint8_t cmd;
    volatile uint32_t resets;
    uint32_t seen_resets;
    UsbBgStatus *status;
} bg;

/* ------------------------------------------------------------------ time */

uint32_t usb_millis(void) {
    unsigned mask = resident_lock();        /* timer callback and event task */
    uint16_t now = (uint16_t)TIMER_32K;     /* counts down at 32768 Hz */
    if (!bg.timer_started) { bg.last_tick = now; bg.timer_started = true; }
    uint16_t delta = bg.up ? (uint16_t)(now - bg.last_tick) : (uint16_t)(bg.last_tick - now);
    bg.last_tick = now;
    uint32_t rate = bg.rate ? bg.rate : 32768u;
    /* Failsafe: every wait in this driver ends on this clock. If the counter
       ever stood still (thousands of reads without a 30 us tick), count calls
       instead so nothing can wait forever. */
    if (delta) bg.stuck = 0;
    else if (++bg.stuck >= 4000) { bg.stuck = 0; delta = 33; }
    if (rate <= 4000000u) {
        bg.frac += delta * 1000u;
        bg.ms += bg.frac / rate;
        bg.frac %= rate;
    } else {                                /* very fast counter: work in 1/1000 s units of it */
        bg.frac += delta;
        bg.ms += bg.frac / (rate / 1000u);
        bg.frac %= rate / 1000u;
    }
    /* The counter wraps (16 bits); across long gaps between calls the
       real-time clock keeps this clock from falling behind by more than ~1 s. */
    uint32_t rtc = RTC_SECONDS;
    if (!bg.rtc_base || rtc < bg.rtc_base || rtc - bg.rtc_base > 600u) {
        bg.rtc_base = rtc;
        bg.ms_base = bg.ms;
    } else if (rtc - bg.rtc_base >= 2u) {
        uint32_t floor = bg.ms_base + (rtc - bg.rtc_base - 1u) * 1000u;
        if ((int32_t)(floor - bg.ms) > 0) bg.ms = floor;
        if (rtc - bg.rtc_base > 60u) { bg.rtc_base = rtc; bg.ms_base = bg.ms; }
    }
    uint32_t ms = bg.ms;
    resident_unlock(mask);
    return ms;
}

void usb_clock_calibrate(void) {
    const uint32_t limit = 200000000u;
    uint32_t guard = 0, t0 = RTC_SECONDS;
    while (RTC_SECONDS == t0 && ++guard < limit) {}
    if (guard >= limit) return;             /* no real-time clock: keep 32768 Hz */
    t0 = RTC_SECONDS;
    uint16_t last = (uint16_t)TIMER_32K;
    uint32_t down = 0, up = 0, ndown = 0, nup = 0;
    guard = 0;
    while (RTC_SECONDS == t0 && ++guard < limit) {
        uint16_t now = (uint16_t)TIMER_32K;
        uint16_t d = (uint16_t)(last - now), u = (uint16_t)(now - last);
        if (d && d < 0x8000u) { down += d; ++ndown; }
        else if (u && u < 0x8000u) { up += u; ++nup; }
        last = now;
    }
    if (guard >= limit) return;
    unsigned mask = resident_lock();
    bg.up = nup > ndown;
    uint32_t rate = bg.up ? up : down;
    if (rate >= 1000u) bg.rate = rate;
    bg.timer_started = false;
    resident_unlock(mask);
}

uint32_t usb_clock_rate(void) { return bg.rate; }
bool usb_small_dma(void) { return bg.small; }
void usb_set_small_dma(bool small) { bg.small = small; }

void usb_delay(unsigned ms) {
    uint32_t start = usb_millis();
    while (usb_millis() - start < ms) {}
}

/* ----------------------------------------------------------------- cache */

static void cache_clean(const void *p, uint32_t len) {
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)31, end = (uintptr_t)p + len;
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" : : "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" : : "r"(0) : "memory");
}

static void cache_invalidate(const void *p, uint32_t len) {
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)31, end = (uintptr_t)p + len;
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" : : "r"(a) : "memory");
}

/* ------------------------------------------------------------------- DMA */

void usbbg_reset(void) {
    ++bg.resets;
}

static bool reset_seen(void) {
    return bg.resets != bg.seen_resets || !usbhook.configured;
}

#define MIN_POLLS 200000u       /* a timeout also needs this many status polls */

static int dma_why;             /* 0 ok, 1 controller fault, 2 bus reset, 3 timeout */

static bool dma(unsigned ch, void *buf, uint32_t len, bool to_host, unsigned timeout_ms) {
    if (!uncached) cache_clean(buf, len);
    unsigned reg = ch == OUT_CH ? DMA3 : DMA4;
    wr(reg + 4, (uint32_t)(uintptr_t)buf);
    wr(reg, len << 8 | (to_host ? 3u : 1u));
    uint32_t start = usb_millis(), polls = 0, s, now = start;
    dma_why = 0;
    for (;;) {
        s = rd(DMASR);
        if (s & (1u << ch)) break;
        if (s & (1u << (16 + ch))) { dma_why = 1; break; }
        ++polls;
        now = usb_millis();
        if (reset_seen()) { dma_why = 2; break; }
        if (polls >= MIN_POLLS && now - start > timeout_ms) { dma_why = 3; break; }
    }
    wr(DMASR, 1u << ch | 1u << (16 + ch));
    if (dma_why) {
        wr(reg, 0);
        wr(ch == OUT_CH ? FIFO2 : FIFO3, 0x1000);
        UsbBgStatus *st = bg.status;
        if (st) {
            ++st->errors;
            if (dma_why == 1) ++st->dma_faults;
            else if (dma_why == 2) ++st->aborted;
            else if (to_host) ++st->in_timeouts;
            else ++st->out_timeouts;
            if (dma_why != 2) {
                st->fail_ch = ch; st->fail_len = len; st->fail_sr = s;
                st->fail_ms = now - start; st->fail_polls = polls; st->fail_cmd = bg.cmd;
            }
        }
    } else if (bg.status) {
        if (to_host) bg.status->bytes_in += len; else bg.status->bytes_out += len;
    }
    if (!to_host && !uncached) cache_invalidate(buf, len);
    return !dma_why;
}

/* Data to the computer. By default one packet per DMA: that is the
   shape of transfer known to work on real hardware. "dma=big" in the settings
   file allows one DMA per chunk; if one of those times out, it is switched off. */
static bool dma_in(void *buf, uint32_t len, unsigned timeout_ms) {
    uint32_t mps = bg.mps ? bg.mps : 64u;
    if (!bg.small || len <= mps) {
        bool ok = dma(IN_CH, buf, len, true, timeout_ms);
        if (!ok && dma_why == 3 && len > mps) bg.small = true;
        return ok;
    }
    for (uint32_t off = 0; off < len; off += mps) {
        uint32_t n = len - off < mps ? len - off : mps;
        if (!dma(IN_CH, (uint8_t *)buf + off, n, true, timeout_ms)) return false;
    }
    return true;
}

static uint32_t out_available(void) {
    if (!(rd(GISR1) & (3u << 4))) return 0;
    return rd(FIFO2) & 0x7ff;
}

/* ---------------------------------------------- USB drive (bulk-only, SCSI) */

static void command(Msc *m, uint32_t n) {
    if (n > CBW_BUF) n = CBW_BUF;
    if (!dma(OUT_CH, cbw_buf, n, false, 1000)) return;
    if (!msc_command(m, cbw_buf, n)) return;         /* not a CBW: ignore it */
    bg.cmd = m->cdb[0];
    if (bg.status) {
        ++bg.status->commands;
        bg.status->last_command_ms = usb_millis();
    }
    if (m->dir == MSC_IN) {
        while (m->done < m->xfer) {
            uint32_t c = msc_in(m, data_buf, DATA_BUF);
            if (!c || !dma_in(data_buf, c, 5000)) return;
        }
    } else if (m->dir == MSC_OUT) {
        uint32_t fill = 0, received = 0;
        while (received < m->xfer) {
            uint32_t start = usb_millis(), avail;
            while (!(avail = out_available())) {
                if (reset_seen() || usb_millis() - start > 5000) {
                    if (bg.status) { ++bg.status->errors; ++bg.status->wait_timeouts; }
                    return;
                }
            }
            if (avail > m->xfer - received) avail = m->xfer - received;
            if (fill + avail > DATA_BUF) { msc_out(m, data_buf, fill); fill = 0; }
            if (!dma(OUT_CH, data_buf + fill, avail, false, 1000)) return;
            fill += avail;
            received += avail;
        }
        if (fill) msc_out(m, data_buf, fill);
        if (bg.status && m->rw) bg.status->last_write_ms = usb_millis();
    }
    msc_status(m, csw_buf);
    dma_in(csw_buf, MSC_CSW_LEN, 5000);
}

bool usbbg_drive_pending(void) {
    return usbhook.configured && out_available() != 0;
}

void usbbg_drive_service(Msc *m, UsbBgStatus *status, unsigned idle_ms, unsigned budget_ms) {
    buffers();
    bg.status = status;
    bg.seen_resets = bg.resets;
    uint32_t start = usb_millis(), last = start;
    for (;;) {
        uint32_t now = usb_millis();
        if (now - start > budget_ms || now - last > idle_ms) break;
        if (reset_seen()) break;
        uint32_t n = out_available();
        if (!n) continue;
        bg.mps = usbhook_mps();
        m->mps = bg.mps;
        command(m, n);
        last = usb_millis();
        if (status) status->last_activity_ms = last;
    }
}

/* ------------------------------------------------------------------ MTP */

/* A zero-length packet on EP2 IN: ends a transfer whose length is a multiple
   of the packet size. The controller sends it when TX0BYTE is set. */
static void zlp(void) {
    wr(EP2_IN, rd(EP2_IN) | 0x8000u);
    uint32_t start = usb_millis(), polls = 0;
    while ((rd(EP2_IN) & 0x8000u) && !(rd(TX0BYTE) & 2u)) {
        ++polls;
        if (reset_seen()) break;
        if (polls >= MIN_POLLS && usb_millis() - start > 1000) {
            if (bg.status) { ++bg.status->errors; ++bg.status->zlp_timeouts; }
            break;
        }
    }
    wr(EP2_IN, rd(EP2_IN) & ~0x8000u);
    wr(TX0BYTE, rd(TX0BYTE) & ~2u);
    if (bg.status) ++bg.status->zlps;
}

/* Take what the computer sent. Commands arrive as one short packet; file data
   is collected into larger pieces so the file is written in few calls. */
static bool receive(Mtp *m) {
    uint32_t fill = 0;
    for (;;) {
        uint32_t avail = out_available();
        if (!avail) {
            if (!fill || !mtp_receiving(m)) break;
            uint32_t start = usb_millis(), polls = 0;
            while (!(avail = out_available()) && (++polls < 2000u || usb_millis() - start < 3)) {}
            if (!avail) break;
        }
        if (avail > DATA_BUF - fill) break;
        if (!dma(OUT_CH, data_buf + fill, avail, false, 1000)) return false;
        fill += avail;
        if (avail < bg.mps) break;                  /* short packet: end of the transfer */
    }
    if (fill) mtp_rx(m, data_buf, fill);
    return true;
}

static bool transmit(Mtp *m) {
    bool end;
    uint32_t n = mtp_tx(m, data_buf, DATA_BUF, &end);
    if (n && !dma_in(data_buf, n, 5000)) return false;
    bg.container += n;
    if (end) {
        if (bg.container % bg.mps == 0) zlp();
        bg.container = 0;
    }
    return true;
}

bool usbbg_pending(void) {
    return usbhook.configured && (out_available() != 0 || (bg.mtp && mtp_tx_pending(bg.mtp)));
}

void usbbg_service(Mtp *m, UsbBgStatus *status, unsigned idle_ms, unsigned budget_ms) {
    buffers();
    bg.status = status;
    bg.mtp = m;
    if (bg.resets != bg.seen_resets) {              /* new connection: nothing carries over */
        bg.seen_resets = bg.resets;
        bg.container = 0;
        mtp_reset(m);
    }
    uint32_t start = usb_millis(), last = start;
    for (;;) {
        uint32_t now = usb_millis();
        if (now - start > budget_ms || now - last > idle_ms) break;
        if (reset_seen()) break;
        bg.mps = usbhook_mps();
        bool busy = false, ok = true;
        if (mtp_tx_pending(m)) { ok = transmit(m); busy = true; }
        else if (out_available()) { ok = receive(m); busy = true; }
        /* A zero-length packet from the computer (end of a transfer that is a
           multiple of the packet size) carries no data: the controller only
           flags it. Acknowledge the flag. */
        if (rd(RX0BYTE) & 2u) {
            wr(RX0BYTE, rd(RX0BYTE) & ~2u);
            if (status) ++status->rx_zlps;
        }
        if (!ok) { mtp_reset(m); bg.container = 0; }
        if (busy) {
            last = usb_millis();
            if (status) status->last_activity_ms = last;
            bg.cmd = mtp_stats(m)->last_op;
        }
    }
}
