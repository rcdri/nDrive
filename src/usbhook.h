/* nDrive - share the USB controller with the TI OS.
 *
 * The OS keeps driving the controller for the TI link (NavNet, used by
 * TI software and n-link). nDrive hooks the two OS helpers every OS USB
 * register access goes through (32-bit read/write via a bus-space handle) and
 * turns the calculator into a composite device: interface 0 is the OS's TI
 * link (EP1, FIFO0/1), interface 1 is a USB mass storage drive (EP2, FIFO2/3,
 * DMA channels 3/4) served by nDrive in the background.
 *
 * Copyright (C) 2026 Rodrigo. GNU GPL v3 or later.
 */
#ifndef NDRIVE_USBHOOK_H
#define NDRIVE_USBHOOK_H

#include <stdbool.h>
#include <stdint.h>

/* Program context only (patches OS code). Returns NULL or an error. */
const char *usbhook_install(void);
bool usbhook_installed(void);

/* State observed through the hooks. */
typedef struct {
    volatile uint32_t configs;      /* SET_CONFIGURATION(1) count */
    volatile uint32_t resets;       /* times the OS reset the device address */
    volatile bool configured;
    volatile uint32_t descriptors;  /* composite config descriptors sent */
    volatile uint32_t activity;     /* SETUP packets and address/config writes */
} UsbHookState;

extern UsbHookState usbhook;

/* Force a fresh enumeration (soft disconnect through the OS's own handling)
   so a computer that is already connected sees the composite device. */
void usbhook_reconnect(void);

/* Is the 4 KiB page at addr mapped (walks the MMU tables)? */
bool mem_page_mapped(uintptr_t addr);

/* Which interface nDrive adds to the device: MTP, or a USB drive (mass
   storage). Takes effect at the computer's next enumeration (usbhook_reconnect). */
void usbhook_set_mtp(bool mtp);
bool usbhook_is_mtp(void);

/* Memory for DMA buffers, mapped uncached by the OS (which keeps its own USB
   buffers there); NULL if it does not look as expected. USBHOOK_POOL_SIZE bytes. */
#define USBHOOK_POOL_SIZE 0x6000u
uint8_t *usbhook_pool(void);

/* Bulk packet size the bus runs at: 512 (high speed) or 64 (full speed). */
unsigned usbhook_mps(void);

/* Is a cable with power (VBUS, B-session valid) connected? */
bool usbhook_vbus(void);

#endif
