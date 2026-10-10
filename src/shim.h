/*
 * libusb341 - internal definitions.
 */
#ifndef LIBUSB341_SHIM_H
#define LIBUSB341_SHIM_H

#include "libusb.h"
#include <stddef.h>
#include <stdio.h>

#define SHIM_VERSION_STR "1.0.0"

/* ---------------------------------------------------------------------- */
/* WCH CH341 parallel/I2C/SPI driver (CH341WDM.SYS / CH341W64.SYS)        */
/* ---------------------------------------------------------------------- */

/* Device interface class registered by the WCH driver. */
#define CH341_GUID_INIT { 0x5446F048, 0x98B4, 0x4EF0, { 0x96, 0xE8, 0x27, 0x99, 0x4B, 0xAC, 0x0D, 0x00 } }

/* CTL_CODE(FILE_DEVICE_UNKNOWN, 0xF34, METHOD_BUFFERED, FILE_ANY_ACCESS) */
#define CH341_IOCTL             0x00223CD0u

/* mWIN32_COMMAND.mFunction values */
#define CH341_FN_GET_VERSION    0x01u   /* driver version */
#define CH341_FN_DEVICE_CTRL    0x04u   /* control pipe: mBuffer = setup packet */
#define CH341_FN_DATA_UP        0x06u   /* bulk IN  (EP2 IN, 0x82) */
#define CH341_FN_DATA_DOWN      0x07u   /* bulk OUT (EP2 OUT, 0x02) */
#define CH341_FN_SET_TIMEOUT    0x09u   /* mBuffer = { ULONG write_ms, ULONG read_ms } */
#define CH341_FN_RESET_DEVICE   0x0Cu
#define CH341_FN_WRITE_READ     0x14u   /* write, then read <times> blocks of <step> bytes */

#define CH341_CMD_HEAD          8u      /* mFunction/mStatus + mLength */
#define CH341_CMD_SMALL         0x28u   /* sizeof(mWIN32_COMMAND) with a 32-byte buffer */
#define CH341_MAX_XFER          0x1000u /* largest data block per driver call */
#define CH341_PACKET            32u     /* EP2 max packet size; one command stream per packet */
#define CH341_TIMEOUT_INFINITE  0xFFFFFFFFu

#define CH341_EP_OUT            0x02
#define CH341_EP_IN             0x82

struct ch341_found {
	WCHAR path[512];
	char instance_id[256];
	uint16_t vid, pid, rev;
	uint32_t address;
};

/* Low-level device access; replaced by the simulator in test builds. */
struct ch341_io_ops {
	const char *name;
	int (*enumerate)(struct ch341_found *out, int max);
	HANDLE (*open)(const WCHAR *path, DWORD *err);
	void (*close)(HANDLE h);
	BOOL (*ioctl)(HANDLE h, void *buf, DWORD inlen, DWORD outlen, DWORD *returned);
};

extern const struct ch341_io_ops ch341_io_real;
#ifdef LIBUSB341_MOCK
extern const struct ch341_io_ops ch341_io_mock;
#endif
extern const struct ch341_io_ops *ch341_io;

/* ---------------------------------------------------------------------- */
/* Shim objects                                                            */
/* ---------------------------------------------------------------------- */

#define MAGIC_CTX       0x43545831u  /* "CTX1" */
#define MAGIC_DEV       0x44455631u
#define MAGIC_HANDLE    0x48444C31u
#define MAGIC_XFER      0x58465231u

struct shim_device;
struct shim_handle;
struct shim_transfer;

struct shim_ctx {
	uint32_t magic;
	libusb_context *orig;       /* context inside the original libusb, if any */
	int orig_ok;                /* orig context was initialized successfully */
	int orig_refs;              /* default context: successful orig libusb_init(NULL) calls */
	int refcnt;                 /* default context: libusb_init(NULL) calls */
	struct shim_device *devs;   /* CH341 device pool */
	struct shim_ctx *next;
};

struct shim_device {
	uint32_t magic;
	LONG refcnt;
	struct shim_ctx *ctx;
	struct shim_device *next;
	WCHAR path[512];
	char instance_id[256];
	uint8_t dev_desc[LIBUSB_DT_DEVICE_SIZE];
	uint8_t *cfg_desc;
	int cfg_len;
	uint8_t port;
	int gone;
};

/* Response FIFO: data the CH341 returned that libusb IN transfers have not consumed yet.
 * Each unit is what one USB IN transfer would have received. */
struct resp_fifo {
	uint8_t *data;
	size_t cap, head, tail;
	uint32_t *units;
	size_t ucap, uhead, utail;
};

struct shim_handle {
	uint32_t magic;
	struct shim_device *dev;
	struct shim_ctx *ctx;
	HANDLE h;
	uint8_t ic_ver;
	uint32_t claimed;
	ULONG to_wr, to_rd;
	int to_valid;
	int gone;
	struct resp_fifo fifo;
	struct shim_transfer *q_head, *q_tail;  /* queued async transfers */
	struct shim_handle *next;
	uint8_t *io;                            /* driver command buffer */
};

enum { T_IDLE = 0, T_QUEUED, T_ORIG };

struct shim_transfer {
	uint32_t magic;
	int state;
	int cancelled;
	struct shim_handle *h;
	struct shim_transfer *qnext;
	ULONGLONG submit_ms;
	uint32_t stream_id;
	int num_iso;
	struct libusb_transfer *shadow;     /* transfer in the original libusb */
	struct libusb_transfer pub;         /* must be last: iso descriptors follow */
};

#define ST(t) ((struct shim_transfer *)((char *)(t) - offsetof(struct shim_transfer, pub)))

/* ---------------------------------------------------------------------- */

extern CRITICAL_SECTION g_lock;
extern int g_debug;
extern int g_disable_ch341;
extern int g_batch;
extern struct shim_ctx g_default_ctx;
extern struct shim_handle *g_handles;

void shim_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void shim_hexdump(int level, const char *what, const uint8_t *p, int n);
#define LOGE(...) shim_log(1, __VA_ARGS__)
#define LOGW(...) shim_log(2, __VA_ARGS__)
#define LOGI(...) shim_log(3, __VA_ARGS__)
#define LOGD(...) shim_log(4, __VA_ARGS__)

/* ch341.c - driver layer */
int ch341_err_to_libusb(DWORD err, int timed_out);
int ch341_ctrl(struct shim_handle *h, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx,
	uint8_t *data, uint16_t len, int *actual);
int ch341_ctrl_raw(HANDLE dh, uint8_t *io, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx,
	uint8_t *data, uint16_t len, int *actual);
int ch341_set_timeouts(struct shim_handle *h, unsigned int timeout_ms);
int ch341_read_raw(struct shim_handle *h, uint8_t *buf, int len, int *actual, int *overflow);
int ch341_write_raw(struct shim_handle *h, const uint8_t *buf, int len, int *actual);
int ch341_write_read(struct shim_handle *h, const uint8_t *wbuf, int wlen, int pad,
	unsigned step, unsigned times, uint8_t *rbuf, int *ractual);
int ch341_reset(struct shim_handle *h);

/* engine.c - libusb transfer semantics on top of the driver */
unsigned ch341_predict_response(const uint8_t *pkt, unsigned len);
void fifo_free(struct resp_fifo *f);
void fifo_clear(struct resp_fifo *f);
int fifo_empty(const struct resp_fifo *f);
int engine_exec_out(struct shim_handle *h, const uint8_t *data, int len, unsigned timeout, int *actual);
int engine_exec_in(struct shim_handle *h, uint8_t *data, int len, unsigned timeout, int *actual);
int engine_exec_ctrl(struct shim_handle *h, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx,
	uint8_t *data, uint16_t len, unsigned timeout);
int engine_has_pending(struct shim_ctx *ctx);
int engine_run(struct shim_ctx *ctx, DWORD budget_ms, int *completed);
int engine_submit(struct shim_transfer *st);
int engine_cancel(struct shim_transfer *st);
void engine_drop_queue(struct shim_handle *h);
extern HANDLE g_wake;

/* descriptor.c */
int desc_parse_config(const uint8_t *buf, int len, struct libusb_config_descriptor **out);
void desc_free_config(struct libusb_config_descriptor *c);
int desc_is_ours(const void *p);
void desc_track(void *p);
int desc_untrack(void *p);

/* orig.c - the original libusb-1.0.dll */
#define ORIG_FUNCS(X) \
	X(libusb_init) X(libusb_init_context) X(libusb_exit) X(libusb_set_debug) X(libusb_set_log_cb) \
	X(libusb_has_capability) X(libusb_setlocale) X(libusb_set_option) \
	X(libusb_get_device_list) X(libusb_free_device_list) X(libusb_ref_device) X(libusb_unref_device) \
	X(libusb_get_configuration) X(libusb_get_device_descriptor) X(libusb_get_active_config_descriptor) \
	X(libusb_get_config_descriptor) X(libusb_get_config_descriptor_by_value) X(libusb_free_config_descriptor) \
	X(libusb_get_ss_endpoint_companion_descriptor) X(libusb_free_ss_endpoint_companion_descriptor) \
	X(libusb_get_bos_descriptor) X(libusb_free_bos_descriptor) \
	X(libusb_get_usb_2_0_extension_descriptor) X(libusb_free_usb_2_0_extension_descriptor) \
	X(libusb_get_ss_usb_device_capability_descriptor) X(libusb_free_ss_usb_device_capability_descriptor) \
	X(libusb_get_container_id_descriptor) X(libusb_free_container_id_descriptor) \
	X(libusb_get_platform_descriptor) X(libusb_free_platform_descriptor) \
	X(libusb_get_bus_number) X(libusb_get_port_number) X(libusb_get_port_numbers) X(libusb_get_port_path) \
	X(libusb_get_parent) X(libusb_get_device_address) X(libusb_get_device_speed) \
	X(libusb_get_max_packet_size) X(libusb_get_max_iso_packet_size) X(libusb_get_max_alt_packet_size) \
	X(libusb_get_interface_association_descriptors) X(libusb_get_active_interface_association_descriptors) \
	X(libusb_free_interface_association_descriptors) \
	X(libusb_wrap_sys_device) X(libusb_open) X(libusb_close) X(libusb_get_device) \
	X(libusb_set_configuration) X(libusb_claim_interface) X(libusb_release_interface) \
	X(libusb_open_device_with_vid_pid) X(libusb_set_interface_alt_setting) X(libusb_clear_halt) \
	X(libusb_reset_device) X(libusb_alloc_streams) X(libusb_free_streams) \
	X(libusb_dev_mem_alloc) X(libusb_dev_mem_free) \
	X(libusb_kernel_driver_active) X(libusb_detach_kernel_driver) X(libusb_attach_kernel_driver) \
	X(libusb_set_auto_detach_kernel_driver) \
	X(libusb_alloc_transfer) X(libusb_submit_transfer) X(libusb_cancel_transfer) X(libusb_free_transfer) \
	X(libusb_transfer_set_stream_id) X(libusb_transfer_get_stream_id) \
	X(libusb_control_transfer) X(libusb_bulk_transfer) X(libusb_interrupt_transfer) \
	X(libusb_get_string_descriptor_ascii) \
	X(libusb_try_lock_events) X(libusb_lock_events) X(libusb_unlock_events) X(libusb_event_handling_ok) \
	X(libusb_event_handler_active) X(libusb_interrupt_event_handler) X(libusb_lock_event_waiters) \
	X(libusb_unlock_event_waiters) X(libusb_wait_for_event) \
	X(libusb_handle_events_timeout) X(libusb_handle_events_timeout_completed) X(libusb_handle_events) \
	X(libusb_handle_events_completed) X(libusb_handle_events_locked) X(libusb_pollfds_handle_timeouts) \
	X(libusb_get_next_timeout) X(libusb_get_pollfds) X(libusb_free_pollfds) X(libusb_set_pollfd_notifiers) \
	X(libusb_hotplug_register_callback) X(libusb_hotplug_deregister_callback) X(libusb_hotplug_get_user_data) \
	X(libusb_get_ssplus_usb_device_capability_descriptor) X(libusb_free_ssplus_usb_device_capability_descriptor) \
	X(libusb_get_device_string) X(libusb_get_session_data) \
	X(libusb_endpoint_supports_raw_io) X(libusb_endpoint_set_raw_io) X(libusb_get_max_raw_io_transfer_size)

struct orig_api {
	HMODULE mod;
#define ORIG_PTR(name) __typeof__(&name) name;
	ORIG_FUNCS(ORIG_PTR)
#undef ORIG_PTR
};

extern struct orig_api O;
int orig_load(void);    /* returns 1 if the original libusb is available */
extern volatile LONG g_orig_inflight;

#endif /* LIBUSB341_SHIM_H */
