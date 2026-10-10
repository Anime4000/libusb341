/*
 * ch341probe - read-only check of a CH341A through libusb-1.0.dll.
 *
 *   ch341probe          list USB devices, open the CH341A, show descriptors
 *   ch341probe spi      also read the SPI flash JEDEC ID, its first bytes, and
 *                       time a 64 KiB read (same command streams as IMSProg)
 *   ch341probe i2c      also read 16 bytes from the I2C EEPROMs at 0x50/0x51
 *                       (same command streams as SFP-Master)
 *
 * Nothing is ever written to the attached memory chip.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libusb.h"

typedef int (LIBUSB_CALL *info_fn)(libusb_device *, char *, int);
typedef int (LIBUSB_CALL *pt_fn)(void);
typedef const char *(LIBUSB_CALL *ver_fn)(void);

static libusb_device_handle *h;
static int batched;     /* shim: one OUT carrying many packets; libusb: packet by packet */

static uint8_t swap8(uint8_t x)
{
	x = (uint8_t)(((x >> 1) & 0x55) | ((x << 1) & 0xaa));
	x = (uint8_t)(((x >> 2) & 0x33) | ((x << 2) & 0xcc));
	return (uint8_t)(((x >> 4) & 0x0f) | ((x << 4) & 0xf0));
}

static int bulk_out(const uint8_t *b, int n)
{
	int a = 0, r = libusb_bulk_transfer(h, 0x02, (unsigned char *)b, n, &a, 2000);
	if (r)
		printf("  bulk OUT %d bytes: %s\n", n, libusb_error_name(r));
	return r;
}

static int bulk_in(uint8_t *b, int n, int *got)
{
	int r = libusb_bulk_transfer(h, 0x82, b, n, got, 2000);
	if (r)
		printf("  bulk IN %d bytes: %s\n", n, libusb_error_name(r));
	return r;
}

static void hexline(const char *label, const uint8_t *b, int n)
{
	int i;
	printf("  %s", label);
	for (i = 0; i < n; i++)
		printf(" %02X", b[i]);
	printf("\n");
}

/* --- SPI, as IMSProg's ch341a_spi.c does it --- */

static int spi_cs(int low)
{
	const uint8_t b[] = { 0xAB, 0xB7, 0xB7, 0xB7, 0xB7, 0xB7, 0xB6, (uint8_t)(0x40 | (low ? 0x3F : 0x00)), 0x20 };
	return bulk_out(b, sizeof(b));
}

/* Full-duplex SPI transfer of n bytes (tx may be NULL -> 0xFF). */
static int spi_xfer(const uint8_t *tx, uint8_t *rx, int n)
{
	static uint8_t stream[(4096 / 31 + 2) * 32];
	int done = 0;

	while (done < n) {
		int chunk = n - done, pk, i, o = 0, got, left;
		if (chunk > (batched ? 31 * 128 : 31))
			chunk = batched ? 31 * 128 : 31;
		for (pk = 0; pk * 31 < chunk; pk++) {
			int k = chunk - pk * 31 > 31 ? 31 : chunk - pk * 31;
			stream[o++] = 0xA8;
			for (i = 0; i < k; i++)
				stream[o++] = swap8(tx ? tx[done + pk * 31 + i] : 0xFF);
		}
		if (bulk_out(stream, o))
			return -1;
		for (left = chunk; left > 0; left -= got) {
			uint8_t tmp[32];
			if (bulk_in(tmp, left > 31 ? 31 : left, &got) || got <= 0)
				return -1;
			for (i = 0; i < got; i++)
				rx[done + chunk - left + i] = swap8(tmp[i]);
		}
		done += chunk;
	}
	return 0;
}

static int do_spi(void)
{
	static uint8_t buf[65536 + 4];
	const uint8_t cfg[] = { 0xAA, 0x63, 0x00 };   /* stream mode, as IMSProg (750 kHz setting) */
	uint8_t tx[4] = { 0x9F, 0xFF, 0xFF, 0xFF }, rx[4];
	DWORD t0;

	printf("SPI (read-only):\n");
	if (bulk_out(cfg, sizeof(cfg)) || spi_cs(1) || spi_xfer(tx, rx, 4) || spi_cs(0))
		return -1;
	hexline("JEDEC ID:", rx + 1, 3);
	if (rx[1] == 0xFF || rx[1] == 0x00)
		printf("  (no SPI flash answering - is a 25xx chip in the socket?)\n");

	memset(buf, 0xFF, sizeof(buf));
	buf[0] = 0x03; buf[1] = buf[2] = buf[3] = 0;
	t0 = GetTickCount();
	if (spi_cs(1) || spi_xfer(buf, buf, 4 + 65536) || spi_cs(0))
		return -1;
	t0 = GetTickCount() - t0;
	hexline("data @0:", buf + 4, 16);
	printf("  64 KiB read in %lu ms (%.1f KiB/s, %s)\n", (unsigned long)t0,
		t0 ? 64000.0 / t0 : 0.0, batched ? "batched" : "packet by packet");
	return 0;
}

/* --- I2C, as SFP-Master's ch34x_i2c.c does it --- */

static int do_i2c(void)
{
	const uint8_t cfg[] = { 0xAA, 0x60, 0x00 };   /* 20 kHz, as SFP-Master */
	int dev;

	printf("I2C (read-only):\n");
	if (bulk_out(cfg, sizeof(cfg)))
		return -1;
	for (dev = 0x50; dev <= 0x51; dev++) {
		uint8_t cmd[] = { 0xAA, 0x74, 0x82, (uint8_t)(dev << 1), 0x00, 0x74, 0x81, (uint8_t)((dev << 1) | 1),
			0xCF, 0xC0, 0x75, 0x00 };
		uint8_t b[64];
		int got = 0;
		char label[32];

		if (bulk_out(cmd, sizeof(cmd)) || bulk_in(b, sizeof(b), &got))
			return -1;
		snprintf(label, sizeof(label), "0x%02X @0 (%d bytes):", dev, got);
		hexline(label, b, got);
	}
	return 0;
}

int main(int argc, char **argv)
{
	HMODULE m = GetModuleHandleA("libusb-1.0.dll");
	ver_fn ver = m ? (ver_fn)(void *)GetProcAddress(m, "libusb341_version") : NULL;
	info_fn info = m ? (info_fn)(void *)GetProcAddress(m, "libusb341_device_info") : NULL;
	pt_fn pt = m ? (pt_fn)(void *)GetProcAddress(m, "libusb341_passthrough") : NULL;
	libusb_device **list;
	ssize_t n, i;
	int r, a, want_spi = 0, want_i2c = 0;

	for (a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "spi"))
			want_spi = 1;
		else if (!strcmp(argv[a], "i2c"))
			want_i2c = 1;
		else {
			printf("usage: %s [spi] [i2c]\n", argv[0]);
			return 2;
		}
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("libusb-1.0.dll: %s\n", ver ? ver() : "original libusb (no CH341 shim)");

	r = libusb_init(NULL);
	if (r) {
		printf("libusb_init: %s\n", libusb_error_name(r));
		return 1;
	}
	if (pt)
		printf("passthrough to libusb-1.0.orig.dll: %s\n", pt() ? "yes" : "no (only WCH-driver CH341s visible)");

	n = libusb_get_device_list(NULL, &list);
	printf("%d USB device(s):\n", (int)n);
	for (i = 0; i < n; i++) {
		struct libusb_device_descriptor d;
		char id[256] = "";
		int ours = info ? info(list[i], id, sizeof(id)) : 0;

		libusb_get_device_descriptor(list[i], &d);
		printf("  %04x:%04x  %s%s\n", d.idVendor, d.idProduct,
			ours ? "WCH CH341 driver: " : "libusb", ours ? id : "");
	}

	h = libusb_open_device_with_vid_pid(NULL, 0x1a86, 0x5512);
	if (!h) {
		printf("No CH341A (1a86:5512) could be opened.\n");
		libusb_free_device_list(list, 1);
		libusb_exit(NULL);
		return 1;
	}
	{
		libusb_device *dev = libusb_get_device(h);
		struct libusb_device_descriptor d;
		unsigned char s[128], v[8];

		batched = info && info(dev, NULL, 0);
		libusb_get_device_descriptor(dev, &d);
		printf("Opened CH341A via %s\n", batched ? "the WCH driver (shim)" : "libusb (WinUSB/libusbK)");
		printf("  bcdUSB %x.%02x, bcdDevice %x.%02x, iProduct %u\n", d.bcdUSB >> 8, d.bcdUSB & 0xff,
			d.bcdDevice >> 8, d.bcdDevice & 0xff, d.iProduct);
		if (d.iProduct && libusb_get_string_descriptor_ascii(h, d.iProduct, s, sizeof(s)) > 0)
			printf("  product: \"%s\"\n", s);
		r = libusb_control_transfer(h, 0xC0, 0x5F, 0, 0, v, sizeof(v), 1000);
		if (r > 0)
			printf("  IC version: 0x%02X\n", v[0]);
		else
			printf("  IC version: %s\n", libusb_error_name(r));
	}
	r = libusb_claim_interface(h, 0);
	if (r)
		printf("claim interface: %s\n", libusb_error_name(r));

	if (want_spi && do_spi())
		printf("SPI test failed\n");
	if (want_i2c && do_i2c())
		printf("I2C test failed\n");

	libusb_release_interface(h, 0);
	libusb_close(h);
	libusb_free_device_list(list, 1);
	libusb_exit(NULL);
	return 0;
}
