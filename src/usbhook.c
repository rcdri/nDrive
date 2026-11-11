/* nDrive - share the USB controller with the TI OS. See usbhook.h.
 * Copyright (C) 2026 Rodrigo. GNU GPL v3 or later.
 */
#include "usbhook.h"

#include <string.h>

#include "resident.h"
#include "usbbg.h"

#define USB_BASE 0xb0000000u
#define REG(off) (*(volatile uint32_t *)(USB_BASE + (off)))

/* Bits that belong to the drive: FIFO2/FIFO3 OUT, short-packet and IN
   status in GISR1, DMA channels 3/4 done/error in DMASR/DMAMR. */
#define OUR_GISR1 ((3u << 4) | (3u << 6) | (1u << 18) | (1u << 19))
#define OUR_DMA ((1u << 3) | (1u << 4) | (1u << 19) | (1u << 20))

UsbHookState usbhook;

static volatile uint32_t *read_entry, *write_entry;
static uint32_t setup_words[2];
static unsigned setup_count;
static uint32_t dma0_addr;
/* The extended configuration descriptor is sent from our own buffer: the OS's
   EP0 buffer is only known to hold its own descriptor. */
static uint8_t ep0_buf[64] __attribute__((aligned(32)));

extern void usbhook_read_gate(void);
extern void usbhook_write_gate(void);
extern int resident_publish(volatile uint32_t *, uint32_t *, uint32_t, uint32_t);

/* MTP interface appended to the OS's configuration descriptor: still-image
   class (PTP), bulk IN/OUT plus the interrupt endpoint the class requires
   (nDrive sends no events). Its name string "MTP" is how libmtp recognises it. */
#define STR_MTP 0x10
static const uint8_t mtp_interface[30] = {
    9, 4, 0 /* number, patched */, 0, 3, 0x06, 0x01, 0x01, STR_MTP,
    7, 5, 0x82, 2, 64, 0, 0,
    7, 5, 0x02, 2, 64, 0, 0,
    7, 5, 0x83, 3, 16, 0, 10
};
static const uint8_t mtp_string[8] = { 8, 3, 'M', 0, 'T', 0, 'P', 0 };
static bool string_pending;

/* USB drive: mass storage, SCSI transparent, bulk-only. */
static const uint8_t msc_interface[23] = {
    9, 4, 0 /* number, patched */, 0, 2, 0x08, 0x06, 0x50, 0,
    7, 5, 0x82, 2, 64, 0, 0,
    7, 5, 0x02, 2, 64, 0, 0
};

static bool mtp_mode = true;
void usbhook_set_mtp(bool mtp) { mtp_mode = mtp; }
bool usbhook_is_mtp(void) { return mtp_mode; }

static void cache_clean(const void *p, uint32_t len) {
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)31, end = (uintptr_t)p + len;
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" : : "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" : : "r"(0) : "memory");
}

unsigned usbhook_mps(void) {
    /* What the OS programmed for its own bulk endpoint (EP1 IN). */
    return (REG(0x160) & 0x7ffu) == 512u ? 512u : 64u;
}

/* The OS maps the top megabyte of RAM uncached and keeps its USB DMA buffers
   there; on real hardware DMA from ordinary (cached) memory delivered stale
   data. Part of that megabyte is unused: take it, after checking that it is
   mapped uncached 1:1 and still empty. */
#define POOL_BASE 0x13f40000u

static bool uncached_identity(uintptr_t va) {
    uint32_t ttb;
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(ttb));
    uint32_t e = ((const volatile uint32_t *)(uintptr_t)(ttb & ~0x3fffu))[va >> 20];
    if ((e & 3) != 1) return false;                                 /* coarse table */
    e = ((const volatile uint32_t *)(uintptr_t)(e & ~0x3ffu))[(va >> 12) & 0xff];
    if (e & 0xc) return false;                                      /* cached or buffered */
    if ((e & 3) == 1) return (e & 0xffff0000u) == (va & 0xffff0000u);   /* large page */
    if ((e & 3) == 2) return (e & 0xfffff000u) == (va & 0xfffff000u);   /* small page */
    return false;
}

uint8_t *usbhook_pool(void) {
    static int state;                       /* 0 unknown, 1 ok, 2 not usable */
    if (!state) {
        state = 1;
        for (uintptr_t a = POOL_BASE; a < POOL_BASE + USBHOOK_POOL_SIZE; a += 4096)
            if (!uncached_identity(a)) { state = 2; break; }
        if (state == 1) {
            /* Empty, or ours from before a restart (RAM may survive one). */
            volatile uint32_t *p = (volatile uint32_t *)POOL_BASE;
            volatile uint32_t *mark = p + USBHOOK_POOL_SIZE / 4 - 2;
            if (mark[0] != 0x5244446eu || mark[1] != 0x4c4f4f50u)
                for (unsigned i = 0; i < USBHOOK_POOL_SIZE / 4; ++i)
                    if (p[i]) { state = 2; break; }
            if (state == 1) { mark[0] = 0x5244446eu; mark[1] = 0x4c4f4f50u; }
        }
    }
    return state == 1 ? (uint8_t *)POOL_BASE : NULL;
}

static void ep2_setup(void) {
    unsigned mps = usbhook_mps();
    REG(0x164) = 0x1000; REG(0x164) = 0; REG(0x164) = mps;     /* EP2 IN  */
    REG(0x184) = 0x1000; REG(0x184) = 0; REG(0x184) = mps;     /* EP2 OUT */
    REG(0x1b8) = 0x1000;                                       /* FIFO2 */
    REG(0x1bc) = 0x1000;                                       /* FIFO3 */
}

/* The OS is about to DMA EP0 data: extend its configuration descriptor. */
static uint32_t ep0_dma(uint32_t ctrl) {
    if ((ctrl & 3) != 3 || setup_count < 2) return ctrl;
    uint32_t w0 = setup_words[0], w1 = setup_words[1];
    uint32_t wlength = w1 >> 16, len = (ctrl >> 8) & 0x1ffff;
    uint8_t *pool = usbhook_pool();
    uint8_t *b = pool ? pool + 0x5000 : ep0_buf;
    if (string_pending) {
        /* The OS is answering with its product string (see usbhook_read):
           send the interface name instead. */
        string_pending = false;
        memcpy(b, mtp_string, sizeof(mtp_string));
        len = wlength < sizeof(mtp_string) ? wlength : sizeof(mtp_string);
        goto send;
    }
    if ((w0 & 0xffff) != 0x0680 || (w0 >> 24) != 2) return ctrl;     /* GET_DESCRIPTOR(config) */
    const uint8_t *d = (const uint8_t *)(uintptr_t)dma0_addr;
    if (dma0_addr < 0x10000000u || dma0_addr >= 0x14000000u || len < 4) return ctrl;
    if (d[0] != 9 || d[1] != 2) return ctrl;
    uint32_t total = d[2] | d[3] << 8, interfaces = d[4];
    if (total > 64 || interfaces > 3) return ctrl;
    if (len < total) {
        /* Only the header was asked for (or fits): patch length and count. The
           lengths below must match what the full request will give. */
        uint32_t full = total + (mtp_mode ? sizeof(mtp_interface) : sizeof(msc_interface));
        if (full > 64) full -= 3;                                   /* without the OTG descriptor */
        if (full > 64) return ctrl;
        memcpy(b, d, len);
        b[2] = (uint8_t)full;
        b[3] = (uint8_t)(full >> 8);
        if (len >= 5) b[4] = (uint8_t)(interfaces + 1);
        goto send;
    }
    {
        /* Copy the OS's descriptors; EP0 carries one 64-byte packet here, so
           drop the 3-byte OTG descriptor if that is what it takes to fit. */
        const uint8_t *ifc = mtp_mode ? mtp_interface : msc_interface;
        uint32_t ifc_len = mtp_mode ? sizeof(mtp_interface) : sizeof(msc_interface);
        bool drop_otg = total + ifc_len > 64;
        uint32_t n = 0;
        for (uint32_t i = 0; i + 2 <= total && d[i] >= 2 && i + d[i] <= total; i += d[i]) {
            if (drop_otg && d[i + 1] == 9 && d[i] == 3) continue;
            memcpy(b + n, d + i, d[i]);
            n += d[i];
        }
        if (n + ifc_len > 64) return ctrl;
        unsigned mps = usbhook_mps();
        memcpy(b + n, ifc, ifc_len);
        b[n + 2] = (uint8_t)interfaces;
        b[n + 13] = (uint8_t)mps; b[n + 14] = (uint8_t)(mps >> 8);
        b[n + 20] = (uint8_t)mps; b[n + 21] = (uint8_t)(mps >> 8);
        if (mtp_mode) b[n + 29] = mps == 512 ? 6 : 10;              /* interrupt interval */
        n += ifc_len;
        b[2] = (uint8_t)n;
        b[3] = (uint8_t)(n >> 8);
        b[4] = (uint8_t)(interfaces + 1);
        len = wlength < n ? wlength : n;
    }
send:
    if (!pool) cache_clean(b, sizeof(ep0_buf));
    REG(0x304) = (uint32_t)(uintptr_t)b;
    ++usbhook.descriptors;
    return (ctrl & 0xff) | len << 8;
}

uint32_t usbhook_read(const uint32_t *handle, uint32_t off) {
    uint32_t base = handle[2];
    uint32_t v = *(volatile uint32_t *)(base + off);
    if (base != USB_BASE) return v;
    switch (off) {
    case 0x148: v &= ~OUR_GISR1; break;
    case 0x328: v &= ~OUR_DMA; break;
    case 0x1b8: case 0x1bc: v = 0; break;
    case 0x1d0:
        if (setup_count < 2) setup_words[setup_count] = v;
        if (!setup_count) {
            /* GET_DESCRIPTOR(string STR_MTP): the OS does not have it. Let it
               answer for its product string; ep0_dma swaps the contents. */
            string_pending = mtp_mode && (v & 0xffffu) == 0x0680u && (v >> 16) == (0x0300u | STR_MTP);
            if (string_pending) v = (v & 0xff00ffffu) | 1u << 16;
        }
        ++setup_count;
        ++usbhook.activity;                         /* a host is talking */
        break;
    }
    return v;
}

void usbhook_write(const uint32_t *handle, uint32_t off, uint32_t v) {
    uint32_t base = handle[2];
    volatile uint32_t *reg = (volatile uint32_t *)(base + off);
    if (base != USB_BASE) { *reg = v; return; }
    switch (off) {
    case 0x1b8: case 0x1bc: return;                 /* our FIFOs */
    case 0x148: v &= ~OUR_GISR1; break;
    case 0x328: v &= ~OUR_DMA; break;
    case 0x32c: v |= OUR_DMA; break;                /* we poll; keep masked */
    case 0x138: v |= OUR_GISR1; break;
    case 0x1a8: v = (v & 0xffffu) | 0x12u << 24 | 0x02u << 16; break;   /* FIFO3 EP2 IN, FIFO2 EP2 OUT */
    case 0x1ac: v = (v & 0xffffu) | 0x22220000u; break;                /* bulk, enabled */
    case 0x1a0:
        /* EP2: IN FIFO3, OUT FIFO2. EP3 (the interrupt endpoint, never
           used): FIFO2, which is an OUT FIFO, so a poll finds nothing to send. */
        *reg = (v & 0xffu) | 0x2300u | (mtp_mode ? 0x220000u : 0u);
        ep2_setup();
        usbbg_reset();
        return;
    case 0x1c0:
        if (v == 0x10) setup_count = 0;             /* OS starts reading a SETUP */
        break;
    case 0x104: {
        uint32_t old = *reg;
        ++usbhook.activity;
        if ((v & 0x80) && !(old & 0x80)) { ++usbhook.configs; usbhook.configured = true; }
        if (!(v & 0x80)) usbhook.configured = false;
        if (!(v & 0x7f) && (old & 0x7f)) { ++usbhook.resets; usbbg_reset(); }
        break;
    }
    case 0x160:                                     /* OS sets its bulk packet size: ours follows */
        *reg = v;
        if ((v & 0x7ffu) == 64u || (v & 0x7ffu) == 512u) {
            REG(0x164) = (REG(0x164) & ~0x7ffu) | (v & 0x7ffu);
            REG(0x184) = (REG(0x184) & ~0x7ffu) | (v & 0x7ffu);
        }
        return;
    case 0x304: dma0_addr = v; break;
    case 0x300: v = ep0_dma(v); break;
    }
    *reg = v;
}

bool mem_page_mapped(uintptr_t addr) {
    uint32_t ttb;
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(ttb));
    const volatile uint32_t *l1 = (const volatile uint32_t *)(uintptr_t)(ttb & ~0x3fffu);
    uint32_t e = l1[addr >> 20];
    switch (e & 3) {
    case 2: return true;
    case 1: return (((const volatile uint32_t *)(uintptr_t)(e & ~0x3ffu))[(addr >> 12) & 0xff] & 3) != 0;
    case 3: return (((const volatile uint32_t *)(uintptr_t)(e & ~0xfffu))[(addr >> 10) & 0x3ff] & 3) != 0;
    default: return false;
    }
}

/* The OS's six bus-space helpers sit together: read8/16/32, write8/16/32,
   each "ldr r3,[r0,#8]; <access>; bx lr". */
static volatile uint32_t *find_helpers(void) {
    static const uint32_t ops[6] = {
        0xe7d10003u, 0xe19100b3u, 0xe7910003u, 0xe7c12003u, 0xe18120b3u, 0xe7812003u
    };
    volatile uint32_t *found = NULL;
    for (uintptr_t page = 0x10000000u; page < 0x11000000u; page += 4096) {
        if (!mem_page_mapped(page) || !mem_page_mapped(page + 4096)) continue;
        volatile uint32_t *p = (volatile uint32_t *)page;
        for (unsigned i = 0; i < 1024; ++i, ++p) {
            if (p[0] != 0xe5903008u || p[1] != ops[0]) continue;
            unsigned k;
            for (k = 0; k < 6; ++k)
                if (p[3 * k] != 0xe5903008u || p[3 * k + 1] != ops[k] || p[3 * k + 2] != 0xe12fff1eu) break;
            if (k != 6) continue;
            if (found) return NULL;                 /* not unique: refuse */
            found = p;
        }
    }
    return found;
}

bool usbhook_installed(void) {
    return read_entry && read_entry[0] == 0xe51ff004u && read_entry[1] == (uint32_t)(uintptr_t)usbhook_read_gate;
}

const char *usbhook_install(void) {
    if (usbhook_installed()) return NULL;
    volatile uint32_t *block = find_helpers();
    if (!block) return "OS USB helpers not found";
    read_entry = block + 6;
    write_entry = block + 15;
    /* Hooks are only safe while the OS has not configured anything yet or
       with our EP2 bits applied right away: apply them now. */
    if (!resident_publish(write_entry, (uint32_t *)(uintptr_t)usbhook_write_gate, 0xe5903008u, 0xe7812003u))
        return "OS USB write helper changed";
    if (!resident_publish(read_entry, (uint32_t *)(uintptr_t)usbhook_read_gate, 0xe5903008u, 0xe7910003u))
        return "OS USB read helper changed";
    unsigned mask = resident_lock();
    REG(0x138) |= OUR_GISR1;
    REG(0x32c) |= OUR_DMA;
    REG(0x1a8) = (REG(0x1a8) & 0xffffu) | 0x12u << 24 | 0x02u << 16;
    REG(0x1ac) = (REG(0x1ac) & 0xffffu) | 0x22220000u;
    REG(0x1a0) = (REG(0x1a0) & 0xffu) | 0x2300u | (mtp_mode ? 0x220000u : 0u);
    ep2_setup();
    usbhook.configured = (REG(0x104) & 0x80) != 0;
    resident_unlock(mask);
    return NULL;
}

void usbhook_reconnect(void) {
    REG(0x114) |= 1;
    usb_delay(400);
    REG(0x114) &= ~1u;
}

bool usbhook_vbus(void) {
    return (REG(0x80) & (1u << 17)) != 0;           /* OTGCS: B-session valid */
}
