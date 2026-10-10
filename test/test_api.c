/*
 * libusb API behaviour of the shim: contexts, device lists merged with the
 * original libusb (when libusb-1.0.orig.dll is next to the DLL), descriptors,
 * control transfers, transfer life cycle and error paths.
 */
#include <string.h>
#include "libusb.h"
#include "test_util.h"

static int cb_count, cb_status, cb_len;

static void LIBUSB_CALL cb(struct libusb_transfer *t)
{
	cb_count++;
	cb_status = t->status;
	cb_len = t->actual_length;
}

static void list_devices(libusb_context *ctx, int *n_ch341)
{
	libusb_device **list;
	ssize_t n = libusb_get_device_list(ctx, &list);
	ssize_t i;

	*n_ch341 = 0;
	CHECK(n >= 0, "get_device_list %d", (int)n);
	if (n < 0)
		return;
	printf("  %d device(s):\n", (int)n);
	for (i = 0; i < n; i++) {
		struct libusb_device_descriptor d;
		int r = libusb_get_device_descriptor(list[i], &d);
		CHECK(r == 0, "descriptor %d", r);
		printf("    bus %u addr %3u  %04x:%04x\n", libusb_get_bus_number(list[i]),
			libusb_get_device_address(list[i]), d.idVendor, d.idProduct);
		if (d.idVendor == 0x1a86 && d.idProduct == 0x5512)
			(*n_ch341)++;
	}
	CHECK(list[n] == NULL, "list not NULL terminated");
	libusb_free_device_list(list, 1);
}

int main(void)
{
	libusb_context *ctx = NULL;
	libusb_device_handle *h;
	libusb_device *dev;
	struct libusb_device_descriptor dd;
	struct libusb_config_descriptor *cfg;
	struct libusb_transfer *t;
	unsigned char s[128], b[64];
	int r, n341, cfgval;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("  %s\n", libusb_get_version()->describe);
	CHECK(strcmp(libusb_error_name(LIBUSB_ERROR_TIMEOUT), "LIBUSB_ERROR_TIMEOUT") == 0, "error_name");
	CHECK(strcmp(libusb_error_name(LIBUSB_TRANSFER_CANCELLED), "LIBUSB_TRANSFER_CANCELLED") == 0, "status name");

	/* SFP-Master passes option 3 (LOG_CB) with no argument; must be harmless */
	r = libusb_set_option(NULL, 3);
	CHECK(r == 0, "set_option(NULL, 3) = %d", r);

	r = libusb_init(&ctx);
	CHECK(r == 0 && ctx, "init %d", r);
	libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, 0);
	list_devices(ctx, &n341);
	CHECK(n341 >= 1, "simulated CH341 not listed");

	h = libusb_open_device_with_vid_pid(ctx, 0x1a86, 0x5512);
	CHECK(h != NULL, "open_device_with_vid_pid");
	if (!h)
		return finish("test_api");

	dev = libusb_get_device(h);
	CHECK(libusb_get_device_descriptor(dev, &dd) == 0 && dd.bcdDevice == 0x0304 && dd.iProduct == 2,
		"device descriptor bcdDevice %04x", dd.bcdDevice);
	CHECK(libusb_get_device_speed(dev) == LIBUSB_SPEED_FULL, "speed");
	CHECK(libusb_get_max_packet_size(dev, 0x82) == 32 && libusb_get_max_packet_size(dev, 0x81) == 8,
		"max packet");

	r = libusb_get_active_config_descriptor(dev, &cfg);
	CHECK(r == 0, "config descriptor %d", r);
	if (!r) {
		const struct libusb_interface_descriptor *alt = &cfg->interface[0].altsetting[0];
		CHECK(cfg->bNumInterfaces == 1 && alt->bNumEndpoints == 3 && alt->endpoint[1].bEndpointAddress == 0x02,
			"config layout");
		libusb_free_config_descriptor(cfg);
	}

	r = libusb_get_descriptor(h, LIBUSB_DT_DEVICE, 0, b, 18);
	CHECK(r == 18 && b[8] == 0x86 && b[9] == 0x1a, "get_descriptor via control transfer: %d", r);
	r = libusb_get_string_descriptor_ascii(h, 2, s, sizeof(s));
	CHECK(r > 0, "string descriptor %d", r);
	if (r > 0)
		printf("  product string: \"%s\"\n", s);
	{
		char ds[128];
		int r2 = libusb_get_device_string(dev, LIBUSB_DEVICE_STRING_PRODUCT, ds, sizeof(ds));
		CHECK(r2 == r && strcmp(ds, (char *)s) == 0, "get_device_string %d", r2);
		CHECK(libusb_get_device_string(dev, LIBUSB_DEVICE_STRING_SERIAL_NUMBER, ds, sizeof(ds))
			== LIBUSB_ERROR_NOT_FOUND, "no serial string");
		CHECK(libusb_get_session_data(dev) != 0, "session data");
		CHECK(libusb_endpoint_supports_raw_io(h, 0x82) == 0, "raw io");
	}
	CHECK(libusb_get_configuration(h, &cfgval) == 0 && cfgval == 1, "configuration");
	CHECK(libusb_claim_interface(h, 0) == 0, "claim 0");
	CHECK(libusb_claim_interface(h, 1) == LIBUSB_ERROR_NOT_FOUND, "claim 1");
	CHECK(libusb_kernel_driver_active(h, 0) == LIBUSB_ERROR_NOT_SUPPORTED, "kernel driver");
	CHECK(libusb_control_transfer(h, 0x40, 0x9A, 0, 0, b, 4, 100) == LIBUSB_ERROR_NOT_SUPPORTED,
		"OUT control with data must be refused");

	/* async: IN before OUT (I2C read of 16 bytes at A0h:00) */
	{
		static uint8_t out[] = { 0xAA, 0x74, 0x82, 0xA0, 0x00, 0x74, 0x81, 0xA1, 0xCF, 0xC0, 0x75, 0x00 };
		struct libusb_transfer *tin = libusb_alloc_transfer(0), *tout = libusb_alloc_transfer(0);
		struct timeval tv = { 0, 100 };
		int i, k;

		libusb_fill_bulk_transfer(tin, h, 0x82, b, 64, cb, NULL, 300);
		libusb_fill_bulk_transfer(tout, h, 0x02, out, sizeof(out), cb, NULL, 300);
		cb_count = 0;
		CHECK(libusb_submit_transfer(tin) == 0, "submit in");
		CHECK(libusb_submit_transfer(tin) == LIBUSB_ERROR_BUSY, "double submit");
		CHECK(libusb_submit_transfer(tout) == 0, "submit out");
		for (k = 0; k < 100 && cb_count < 2; k++)
			libusb_handle_events_timeout(ctx, &tv);
		CHECK(cb_count == 2 && tin->status == 0 && tin->actual_length == 16, "async i2c read: %d cb, len %d",
			cb_count, tin->actual_length);
		for (i = 0; i < 16 && b[i] == ee_pattern(0, (uint32_t)i); i++)
			;
		CHECK(i == 16, "async data mismatch at %d", i);

		/* cancel a pending IN */
		cb_count = 0;
		CHECK(libusb_submit_transfer(tin) == 0, "submit in 2");
		CHECK(libusb_cancel_transfer(tin) == 0, "cancel");
		libusb_handle_events_timeout(ctx, &tv);
		CHECK(cb_count == 1 && cb_status == LIBUSB_TRANSFER_CANCELLED, "cancel callback status %d", cb_status);
		CHECK(libusb_cancel_transfer(tin) == LIBUSB_ERROR_NOT_FOUND, "cancel idle");

		/* IN with nothing to read times out */
		cb_count = 0;
		tin->timeout = 50;
		CHECK(libusb_submit_transfer(tin) == 0, "submit in 3");
		for (k = 0; k < 50 && !cb_count; k++)
			libusb_handle_events_timeout(ctx, &tv);
		CHECK(cb_count == 1 && cb_status == LIBUSB_TRANSFER_TIMED_OUT, "timeout status %d", cb_status);

		libusb_free_transfer(tin);
		libusb_free_transfer(tout);
	}

	/* sync IN with nothing pending */
	{
		int got = -1;
		r = libusb_bulk_transfer(h, 0x82, b, 32, &got, 50);
		CHECK(r == LIBUSB_ERROR_TIMEOUT && got == 0, "empty read: %d", r);
		r = libusb_bulk_transfer(h, 0x05, b, 32, &got, 50);
		CHECK(r == LIBUSB_ERROR_NOT_FOUND, "bad endpoint: %d", r);
	}

	/* FREE_TRANSFER flag */
	t = libusb_alloc_transfer(0);
	{
		static uint8_t cfgcmd[] = { 0xAA, 0x61, 0x00 };
		struct timeval tv = { 0, 1000 };
		libusb_fill_bulk_transfer(t, h, 0x02, cfgcmd, 3, cb, NULL, 100);
		t->flags = LIBUSB_TRANSFER_FREE_TRANSFER;
		cb_count = 0;
		CHECK(libusb_submit_transfer(t) == 0, "submit free-transfer");
		libusb_handle_events_timeout(ctx, &tv);
		CHECK(cb_count == 1 && cb_status == 0 && cb_len == 3, "free-transfer callback");
	}

	CHECK(libusb_release_interface(h, 0) == 0, "release");
	libusb_close(h);

	/* default context too */
	CHECK(libusb_init(NULL) == 0, "init default");
	list_devices(NULL, &n341);
	h = libusb_open_device_with_vid_pid(NULL, 0x1a86, 0x5512);
	CHECK(h != NULL, "open on default context");
	if (h)
		libusb_close(h);
	CHECK(libusb_open_device_with_vid_pid(NULL, 0x1234, 0x5678) == NULL, "absent device");
	libusb_exit(NULL);
	libusb_exit(ctx);
	return finish("test_api");
}
