/* nDrive - background MTP transport on EP2 (FIFO2/3, DMA 3/4),
 * next to the OS's TI link. Copyright (C) 2026 Rodrigo. GPL v3+. */
#ifndef NDRIVE_USBBG_H
#define NDRIVE_USBBG_H

#include <stdbool.h>
#include <stdint.h>

#include "msc.h"
#include "mtp.h"

typedef struct {
    uint32_t errors;
    uint32_t last_activity_ms;
    uint32_t commands, last_command_ms, last_write_ms;     /* USB drive mode */
    uint32_t zlps, zlp_timeouts, rx_zlps;
    /* Diagnostics (ctrl+U). */
    uint32_t in_timeouts, out_timeouts, dma_faults, wait_timeouts, aborted;
    uint32_t fail_ch, fail_len, fail_sr, fail_ms, fail_polls, fail_cmd;
    uint32_t bytes_in, bytes_out;
} UsbBgStatus;

/* The computer sent something, or a reply is waiting (cheap; any context). */
bool usbbg_pending(void);
/* Serve the computer until it is quiet for idle_ms or budget_ms passes.
   Task context (reads and writes calculator files). */
void usbbg_service(Mtp *mtp, UsbBgStatus *status, unsigned idle_ms, unsigned budget_ms);

/* The same two for the USB drive (mass storage) personality. */
bool usbbg_drive_pending(void);
void usbbg_drive_service(Msc *msc, UsbBgStatus *status, unsigned idle_ms, unsigned budget_ms);
/* Bus reset / reconfiguration seen by the hooks: drop partial state. */
void usbbg_reset(void);

uint32_t usb_millis(void);
void usb_delay(unsigned ms);

/* Measure the timer against the real-time clock (program context, up to 2 s). */
void usb_clock_calibrate(void);
uint32_t usb_clock_rate(void);      /* timer ticks per second as measured */
bool usb_small_dma(void);           /* one packet per DMA (default) */
void usb_set_small_dma(bool small);
bool usb_uncached_buffers(void);

#endif
