/* A stand-in for libusb (LD_PRELOAD) that connects libmtp - the library KDE,
 * GNOME and the mtp-tools use - straight to nDrive's MTP responder, with a
 * directory (NDRIVE_ROOT) as the calculator's file system. USB packets are
 * modelled: 512-byte packets, a transfer ends on a short or zero-length one. */
#define _GNU_SOURCE
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mtp.h"
#include "posix_fs.h"

#define MPS 512u

static Mtp *mtp;
static int dev_token, handle_token;
static uint8_t packet[MPS];
static uint32_t packet_n, packet_pos, container_bytes;
static int have_packet, zlp_pending;
static FILE *trace;

static void received(void *user, const char *path) {
    (void)user;
    const char *log = getenv("NDRIVE_RECEIVED_LOG");
    if (!log) return;
    FILE *f = fopen(log, "a");
    if (f) { fprintf(f, "%s\n", path); fclose(f); }
}

static void start(void) {
    if (mtp) return;
    const char *root = getenv("NDRIVE_ROOT");
    posix_fs_root(root ? root : "/tmp/ndrive-root");
    if (getenv("NDRIVE_FAIL_WRITES")) posix_fs_fail_writes_after = (unsigned)atoi(getenv("NDRIVE_FAIL_WRITES"));
    if (getenv("NDRIVE_TRACE")) trace = fopen(getenv("NDRIVE_TRACE"), "a");
    mtp = mtp_create(&posix_fs, &posix_mem, received, NULL);
}

int libusb_init(libusb_context **ctx) { if (ctx) *ctx = (libusb_context *)&dev_token; start(); return 0; }
void libusb_exit(libusb_context *ctx) { (void)ctx; }
int libusb_set_option(libusb_context *ctx, enum libusb_option option, ...) { (void)ctx; (void)option; return 0; }
void libusb_set_debug(libusb_context *ctx, int level) { (void)ctx; (void)level; }

ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list) {
    (void)ctx;
    libusb_device **l = calloc(2, sizeof(*l));
    l[0] = (libusb_device *)&dev_token;
    *list = l;
    return 1;
}
void libusb_free_device_list(libusb_device **list, int unref) { (void)unref; free(list); }
uint8_t libusb_get_bus_number(libusb_device *d) { (void)d; return 1; }
uint8_t libusb_get_device_address(libusb_device *d) { (void)d; return 9; }
libusb_device *libusb_get_device(libusb_device_handle *h) { (void)h; return (libusb_device *)&dev_token; }

int libusb_get_device_descriptor(libusb_device *d, struct libusb_device_descriptor *desc) {
    (void)d;
    memset(desc, 0, sizeof(*desc));
    desc->bLength = 18; desc->bDescriptorType = 1; desc->bcdUSB = 0x0200; desc->bMaxPacketSize0 = 64;
    desc->idVendor = 0x0451; desc->idProduct = 0xe022; desc->bcdDevice = 0x0201;
    desc->iManufacturer = 2; desc->iProduct = 1; desc->bNumConfigurations = 1;
    return 0;
}

static struct libusb_endpoint_descriptor ep(uint8_t addr, uint8_t attr, uint16_t mps) {
    struct libusb_endpoint_descriptor e;
    memset(&e, 0, sizeof(e));
    e.bLength = 7; e.bDescriptorType = 5; e.bEndpointAddress = addr; e.bmAttributes = attr;
    e.wMaxPacketSize = mps; e.bInterval = attr == 3 ? 6 : 0;
    return e;
}

int libusb_get_config_descriptor(libusb_device *d, uint8_t index, struct libusb_config_descriptor **out) {
    (void)d;
    if (index) return LIBUSB_ERROR_NOT_FOUND;
    struct libusb_config_descriptor *c = calloc(1, sizeof(*c));
    struct libusb_interface *ifs = calloc(2, sizeof(*ifs));
    struct libusb_interface_descriptor *alt = calloc(2, sizeof(*alt));
    struct libusb_endpoint_descriptor *eps = calloc(5, sizeof(*eps));
    eps[0] = ep(0x81, 2, MPS); eps[1] = ep(0x01, 2, MPS);
    eps[2] = ep(0x82, 2, MPS); eps[3] = ep(0x02, 2, MPS); eps[4] = ep(0x83, 3, 16);
    alt[0].bLength = 9; alt[0].bDescriptorType = 4; alt[0].bInterfaceNumber = 0; alt[0].bNumEndpoints = 2;
    alt[0].bInterfaceClass = 0xff; alt[0].bInterfaceSubClass = 2; alt[0].endpoint = eps;
    alt[1].bLength = 9; alt[1].bDescriptorType = 4; alt[1].bInterfaceNumber = 1; alt[1].bNumEndpoints = 3;
    alt[1].bInterfaceClass = 6; alt[1].bInterfaceSubClass = 1; alt[1].bInterfaceProtocol = 1;
    alt[1].iInterface = 5; alt[1].endpoint = eps + 2;
    ifs[0].altsetting = alt; ifs[0].num_altsetting = 1;
    ifs[1].altsetting = alt + 1; ifs[1].num_altsetting = 1;
    c->bLength = 9; c->bDescriptorType = 2; c->wTotalLength = 62; c->bNumInterfaces = 2;
    c->bConfigurationValue = 1; c->bmAttributes = 0x80; c->MaxPower = 250; c->interface = ifs;
    *out = c;
    return 0;
}
int libusb_get_active_config_descriptor(libusb_device *d, struct libusb_config_descriptor **out) {
    return libusb_get_config_descriptor(d, 0, out);
}
void libusb_free_config_descriptor(struct libusb_config_descriptor *c) {
    if (!c) return;
    free((void *)c->interface[0].altsetting[0].endpoint);
    free((void *)c->interface[0].altsetting);
    free((void *)c->interface);
    free(c);
}

int libusb_open(libusb_device *d, libusb_device_handle **h) { (void)d; start(); *h = (libusb_device_handle *)&handle_token; return 0; }
void libusb_close(libusb_device_handle *h) { (void)h; }
int libusb_kernel_driver_active(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
int libusb_detach_kernel_driver(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
int libusb_claim_interface(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
int libusb_release_interface(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
int libusb_set_configuration(libusb_device_handle *h, int c) { (void)h; (void)c; return 0; }
int libusb_set_interface_alt_setting(libusb_device_handle *h, int i, int a) { (void)h; (void)i; (void)a; return 0; }
int libusb_clear_halt(libusb_device_handle *h, unsigned char e) { (void)h; (void)e; return 0; }
int libusb_reset_device(libusb_device_handle *h) { (void)h; mtp_reset(mtp); have_packet = zlp_pending = 0; return 0; }

int libusb_get_string_descriptor_ascii(libusb_device_handle *h, uint8_t index, unsigned char *data, int length) {
    (void)h;
    const char *s = index == 5 ? "MTP" : index == 1 ? "TI-Nspire(tm) CX II Handheld" :
                    index == 2 ? "Texas Instruments Incorporated" : NULL;
    if (!s) return LIBUSB_ERROR_PIPE;
    snprintf((char *)data, (size_t)length, "%s", s);
    return (int)strlen((char *)data);
}

int libusb_control_transfer(libusb_device_handle *h, uint8_t type, uint8_t request, uint16_t value,
                            uint16_t index, unsigned char *data, uint16_t length, unsigned int timeout) {
    (void)h; (void)value; (void)index; (void)timeout;
    if (type == 0x82 && request == 0 && length >= 2) { data[0] = data[1] = 0; return 2; }   /* endpoint status */
    if (type == 0xa1 && request == 0x67 && length >= 4) { data[0] = 4; data[1] = 0; data[2] = 0x01; data[3] = 0x20; return 4; }
    return LIBUSB_ERROR_PIPE;               /* the calculator's OS stalls what it does not know */
}

int libusb_bulk_transfer(libusb_device_handle *h, unsigned char endpoint, unsigned char *data, int length,
                         int *transferred, unsigned int timeout) {
    (void)h; (void)timeout;
    if (transferred) *transferred = 0;
    if (!(endpoint & 0x80)) {
        if (trace) { fprintf(trace, "OUT %d:", length); for (int i = 0; i < length && i < 40; ++i) fprintf(trace, " %02x", data[i]); fprintf(trace, "\n"); fflush(trace); }
        if (length) mtp_rx(mtp, data, (uint32_t)length);
        if (transferred) *transferred = length;
        return 0;
    }
    int got = 0, any = 0;
    for (;;) {
        if (zlp_pending) { zlp_pending = 0; any = 1; break; }       /* zero-length packet ends the transfer */
        if (!have_packet) {
            bool end;
            if (!mtp_tx_pending(mtp)) break;
            packet_n = mtp_tx(mtp, packet, MPS, &end);
            packet_pos = 0;
            have_packet = 1;
            container_bytes += packet_n;
            if (end) { if (container_bytes % MPS == 0) zlp_pending = 1; container_bytes = 0; }
        }
        uint32_t take = packet_n - packet_pos;
        if (take > (uint32_t)(length - got)) {
            if (packet_pos == 0 && got == length) break;            /* buffer full on a packet boundary */
            take = (uint32_t)(length - got);                        /* (a real host would see babble) */
        }
        memcpy(data + got, packet + packet_pos, take);
        got += (int)take;
        packet_pos += take;
        any = 1;
        int was_short = packet_n < MPS;
        if (packet_pos == packet_n) have_packet = 0;
        if (was_short || got == length) break;
    }
    if (trace) { fprintf(trace, "IN %d/%d:", got, length); for (int i = 0; i < got && i < 40; ++i) fprintf(trace, " %02x", data[i]); fprintf(trace, "\n"); fflush(trace); }
    if (transferred) *transferred = got;
    return any ? 0 : LIBUSB_ERROR_TIMEOUT;
}

/* Events (interrupt endpoint): nDrive sends none. */
struct libusb_transfer *libusb_alloc_transfer(int iso) { (void)iso; return calloc(1, sizeof(struct libusb_transfer)); }
void libusb_free_transfer(struct libusb_transfer *t) { free(t); }
int libusb_submit_transfer(struct libusb_transfer *t) { (void)t; return LIBUSB_ERROR_NOT_SUPPORTED; }
int libusb_cancel_transfer(struct libusb_transfer *t) { (void)t; return 0; }
int libusb_handle_events_timeout_completed(libusb_context *c, struct timeval *tv, int *completed) {
    (void)c; (void)tv; (void)completed;
    return 0;
}
