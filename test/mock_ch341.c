/*
 * libusb341 - simulated CH341A behind the WCH driver interface (test builds).
 *
 * Emulates the driver's DeviceIoControl protocol and the chip behind it:
 *   - SPI (0xA8 streams, LSB-first on the wire) with a 16 MiB 25-series flash
 *     selected by D0 via UIO streams (0xAB)
 *   - I2C (0xAA streams) with 24C02-style EEPROMs at 0x50 and 0x51
 * The chip only buffers MOCK_RESP_CAP answer packets; a plain driver write that
 * would make it answer more than that without anyone reading stalls (counted
 * as a violation), which is what happens with real hardware.
 *
 * Enabled at runtime with LIBUSB341_MOCK=1 in builds made with -DLIBUSB341_MOCK.
 */
#include "../src/shim.h"
#include <stdlib.h>
#include <string.h>

#define MOCK_RESP_CAP   2
#define MOCK_QMAX       8192
#define FLASH_SIZE      (16u << 20)
#define EE_SIZE         256

struct usb_pkt {
	uint8_t len;
	uint8_t d[32];
};

static struct {
	int init;
	ULONG to_wr, to_rd;
	uint8_t ic_ver;
	/* answer packets the chip has queued on EP2 IN */
	struct usb_pkt q[MOCK_QMAX];
	int qh, qn;
	/* pins */
	uint8_t uio_out, uio_dir;
	int selected;
	/* SPI flash */
	uint8_t *flash;
	uint8_t cmd;
	int nbytes;
	uint32_t addr;
	int wel;
	int pending_erase;      /* 0, 0x20, 0xD8, 0xC7 */
	/* I2C */
	uint8_t ee[2][EE_SIZE];
	int i2c_state;          /* 0 idle, 1 expect address, 2 expect word address, 3 data */
	int i2c_dev, i2c_rd, i2c_ack;
	uint8_t i2c_ptr;
	/* statistics */
	unsigned long n_write, n_read, n_write_read, n_ctrl, violations, spi_bytes, i2c_bytes;
} M;

uint8_t mock_flash_pattern(uint32_t i)
{
	return (uint8_t)((i * 7u) ^ (i >> 8) ^ (i >> 16));
}

uint8_t mock_ee_pattern(int dev, uint32_t i)
{
	return (uint8_t)(0x5A ^ (i * 13u) ^ (uint32_t)dev);
}

static void mock_init(void)
{
	uint32_t i;
	char buf[16];

	if (M.init)
		return;
	M.init = 1;
	M.ic_ver = 0x30;
	if (GetEnvironmentVariableA("LIBUSB341_MOCK_VER", buf, sizeof(buf)))
		M.ic_ver = (uint8_t)strtoul(buf, NULL, 0);
	M.flash = malloc(FLASH_SIZE);
	for (i = 0; i < FLASH_SIZE; i++)
		M.flash[i] = mock_flash_pattern(i);
	for (i = 0; i < EE_SIZE; i++) {
		M.ee[0][i] = mock_ee_pattern(0, i);
		M.ee[1][i] = mock_ee_pattern(1, i);
	}
}

__declspec(dllexport) void libusb341_mock_stats(unsigned long *out)
{
	out[0] = M.n_write;
	out[1] = M.n_read;
	out[2] = M.n_write_read;
	out[3] = M.n_ctrl;
	out[4] = M.violations;
	out[5] = M.spi_bytes;
	out[6] = M.i2c_bytes;
	out[7] = (unsigned long)M.qn;
}

static uint8_t swap_bits(uint8_t x)
{
	x = (uint8_t)(((x >> 1) & 0x55) | ((x << 1) & 0xaa));
	x = (uint8_t)(((x >> 2) & 0x33) | ((x << 2) & 0xcc));
	x = (uint8_t)(((x >> 4) & 0x0f) | ((x << 4) & 0xf0));
	return x;
}

/* ---- SPI flash ---- */

static void flash_deselect(void)
{
	if (M.pending_erase && M.wel) {
		uint32_t base = 0, size = 0;
		if (M.pending_erase == 0x20) { base = M.addr & ~0xFFFu; size = 0x1000; }
		if (M.pending_erase == 0xD8) { base = M.addr & ~0xFFFFu; size = 0x10000; }
		if (M.pending_erase == 0xC7) { base = 0; size = FLASH_SIZE; }
		memset(M.flash + (base % FLASH_SIZE), 0xFF, size);
		M.wel = 0;
	}
	if (M.cmd == 0x02 && M.nbytes > 4)
		M.wel = 0;
	M.pending_erase = 0;
	M.cmd = 0;
	M.nbytes = 0;
}

static void update_select(void)
{
	int sel = (M.uio_dir & 1) && !(M.uio_out & 1);

	if (sel && !M.selected) {
		M.cmd = 0;
		M.nbytes = 0;
		M.addr = 0;
	} else if (!sel && M.selected) {
		flash_deselect();
	}
	M.selected = sel;
}

static uint8_t flash_xfer(uint8_t in)
{
	uint8_t out = 0xFF;
	int n = M.nbytes++;

	if (!M.selected)
		return 0xFF;
	if (n == 0) {
		M.cmd = in;
		switch (in) {
		case 0x06: M.wel = 1; break;
		case 0x04: M.wel = 0; break;
		case 0xC7: case 0x60: M.pending_erase = 0xC7; M.cmd = 0xC7; break;
		}
		return 0xFF;
	}
	switch (M.cmd) {
	case 0x9F: {
		static const uint8_t id[3] = { 0xEF, 0x40, 0x18 };
		return n <= 3 ? id[n - 1] : 0xFF;
	}
	case 0x05:
		return (uint8_t)(M.wel ? 0x02 : 0x00);
	case 0x03: case 0x0B: case 0x02: case 0x20: case 0xD8:
		if (n <= 3) {
			M.addr = (M.addr << 8) | in;
			if (n == 3 && (M.cmd == 0x20 || M.cmd == 0xD8))
				M.pending_erase = M.cmd;
			return 0xFF;
		}
		if (M.cmd == 0x0B && n == 4)
			return 0xFF;    /* dummy byte */
		if (M.cmd == 0x03 || M.cmd == 0x0B) {
			out = M.flash[M.addr % FLASH_SIZE];
			M.addr++;
			return out;
		}
		if (M.cmd == 0x02 && M.wel) {
			uint32_t a = M.addr % FLASH_SIZE;
			M.flash[a] &= in;
			M.addr = (M.addr & ~0xFFu) | ((M.addr + 1) & 0xFFu);
		}
		return 0xFF;
	default:
		return 0xFF;
	}
}

/* ---- I2C EEPROMs ---- */

static void i2c_out(uint8_t b)
{
	switch (M.i2c_state) {
	case 1:
		M.i2c_dev = b >> 1;
		M.i2c_rd = b & 1;
		M.i2c_ack = (M.i2c_dev == 0x50 || M.i2c_dev == 0x51);
		M.i2c_state = M.i2c_rd ? 3 : 2;
		break;
	case 2:
		M.i2c_ptr = b;
		M.i2c_state = 3;
		break;
	case 3:
		if (M.i2c_ack && !M.i2c_rd) {
			M.ee[M.i2c_dev - 0x50][M.i2c_ptr] = b;
			M.i2c_ptr++;
		}
		break;
	}
	M.i2c_bytes++;
}

static uint8_t i2c_in(void)
{
	M.i2c_bytes++;
	if (!M.i2c_ack || M.i2c_state != 3)
		return 0xFF;
	return M.ee[M.i2c_dev - 0x50][M.i2c_ptr++];
}

/* ---- chip: execute one 32-byte OUT packet, appending its answer to ans ---- */

static int exec_packet(const uint8_t *p, int n, uint8_t *ans)
{
	int i, r = 0;

	if (n <= 0)
		return 0;
	switch (p[0]) {
	case 0xA8:
		for (i = 1; i < n; i++) {
			ans[r++] = swap_bits(flash_xfer(swap_bits(p[i])));
			M.spi_bytes++;
		}
		break;
	case 0xAA:
		for (i = 1; i < n;) {
			uint8_t c = p[i++];
			int k, len;
			if (c == 0x00)
				break;
			if ((c & 0xC0) == 0xC0) {
				len = (c & 0x3F) ? (c & 0x3F) : 1;
				for (k = 0; k < len; k++)
					ans[r++] = i2c_in();
			} else if ((c & 0xC0) == 0x80) {
				len = c & 0x3F;
				if (len == 0) {
					if (i < n)
						i2c_out(p[i++]);
					ans[r++] = M.i2c_ack ? 0x00 : 0x80;
				} else {
					for (k = 0; k < len && i < n; k++)
						i2c_out(p[i++]);
				}
			} else if (c == 0x74) {
				M.i2c_state = 1;
			} else if (c == 0x75) {
				M.i2c_state = 0;
			}
		}
		break;
	case 0xAB:
		for (i = 1; i < n; i++) {
			uint8_t c = p[i];
			if ((c & 0xE0) == 0x20)
				break;
			switch (c & 0xC0) {
			case 0x00: ans[r++] = (uint8_t)((M.uio_out & 0x3F) | 0x80); break;
			case 0x40: M.uio_dir = c & 0x3F; update_select(); break;
			case 0x80: M.uio_out = c & 0x3F; update_select(); break;
			default: break;
			}
		}
		break;
	default:
		break;
	}
	return r;
}

/* Queue an answer as the USB packets the chip would send. */
static void queue_answer(const uint8_t *a, int n)
{
	while (n > 0) {
		int k = n > 32 ? 32 : n, idx;
		if (M.qn >= MOCK_QMAX) {
			M.violations++;
			return;
		}
		idx = (M.qh + M.qn) % MOCK_QMAX;
		M.q[idx].len = (uint8_t)k;
		memcpy(M.q[idx].d, a, (size_t)k);
		M.qn++;
		a += k;
		n -= k;
		if (n == 0 && k == 32)
			break;
	}
}

/* One USB IN transfer of up to req bytes: packets until full or short. */
static int usb_read(uint8_t *dst, uint32_t req, uint32_t *got)
{
	*got = 0;
	if (!M.qn)
		return 0;
	while (M.qn) {
		struct usb_pkt *pk = &M.q[M.qh];
		if (*got + pk->len > req)
			return -1;  /* babble */
		memcpy(dst + *got, pk->d, pk->len);
		*got += pk->len;
		M.qh = (M.qh + 1) % MOCK_QMAX;
		M.qn--;
		if (pk->len < 32 || *got == req)
			break;
	}
	return 1;
}

static int write_packets(const uint8_t *d, uint32_t n, int driver_reads_concurrently)
{
	uint8_t ans[256];
	uint32_t off;

	for (off = 0; off < n; off += 32) {
		int plen = (int)(n - off < 32 ? n - off : 32);
		int r;
		if (!driver_reads_concurrently && M.qn >= MOCK_RESP_CAP) {
			/* chip NAKs further OUT packets: the write times out */
			M.violations++;
			return -1;
		}
		r = exec_packet(d + off, plen, ans);
		queue_answer(ans, r);
	}
	return 0;
}

/* ---- driver ---- */

static int mock_enumerate(struct ch341_found *out, int max)
{
	mock_init();
	if (max < 1)
		return 0;
	memset(out, 0, sizeof(*out));
	wcscpy(out->path, L"\\\\?\\usb#vid_1a86&pid_5512#mock#{5446f048-98b4-4ef0-96e8-27994bac0d00}");
	strcpy(out->instance_id, "USB\\VID_1A86&PID_5512\\MOCK0");
	out->vid = 0x1A86;
	out->pid = 0x5512;
	out->rev = 0x0304;
	out->address = 3;
	return 1;
}

static HANDLE mock_open(const WCHAR *path, DWORD *err)
{
	(void)path;
	mock_init();
	*err = 0;
	return (HANDLE)(intptr_t)0x341;
}

static void mock_close(HANDLE h)
{
	(void)h;
}

static const uint8_t dev_desc[18] = {
	0x12, 0x01, 0x10, 0x01, 0xff, 0x00, 0x00, 0x08, 0x86, 0x1a, 0x12, 0x55,
	0x04, 0x03, 0x00, 0x02, 0x00, 0x01
};
static const uint8_t cfg_desc[39] = {
	0x09, 0x02, 0x27, 0x00, 0x01, 0x01, 0x00, 0x80, 0x31,
	0x09, 0x04, 0x00, 0x00, 0x03, 0xff, 0x01, 0x02, 0x00,
	0x07, 0x05, 0x82, 0x02, 0x20, 0x00, 0x00,
	0x07, 0x05, 0x02, 0x02, 0x20, 0x00, 0x00,
	0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x01,
};

static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static BOOL fail(DWORD e)
{
	SetLastError(e);
	return FALSE;
}

static BOOL mock_ioctl(HANDLE h, void *vbuf, DWORD inlen, DWORD outlen, DWORD *returned)
{
	uint8_t *b = vbuf;
	uint32_t fn = get32(b), len = get32(b + 4);

	(void)h;
	*returned = 0;
	if (inlen < CH341_CMD_HEAD)
		return fail(ERROR_INVALID_PARAMETER);
	switch (fn) {
	case CH341_FN_GET_VERSION:
		b[8] = 0x22;
		*returned = CH341_CMD_SMALL;
		return TRUE;
	case CH341_FN_SET_TIMEOUT:
		if (inlen < 16 || len != 8)
			return fail(ERROR_INVALID_PARAMETER);
		M.to_wr = get32(b + 8);
		M.to_rd = get32(b + 12);
		*returned = CH341_CMD_SMALL;
		return TRUE;
	case CH341_FN_RESET_DEVICE:
		M.qh = M.qn = 0;
		*returned = CH341_CMD_SMALL;
		return TRUE;
	case CH341_FN_DEVICE_CTRL: {
		uint8_t rt = b[8], req = b[9];
		uint16_t val = (uint16_t)(b[10] | (b[11] << 8)), wl = (uint16_t)(b[14] | (b[15] << 8));
		const uint8_t *src = NULL;
		uint8_t tmp[64];
		uint32_t n = 0;

		M.n_ctrl++;
		if (inlen < CH341_CMD_SMALL || len != 8 || outlen < CH341_CMD_HEAD + wl)
			return fail(ERROR_INVALID_PARAMETER);
		if (rt == 0x80 && req == 6) {
			switch (val >> 8) {
			case 1: src = dev_desc; n = sizeof(dev_desc); break;
			case 2: src = cfg_desc; n = sizeof(cfg_desc); break;
			case 3:
				if ((val & 0xff) == 0) {
					static const uint8_t lang[4] = { 4, 3, 0x09, 0x04 };
					src = lang; n = 4;
				} else if ((val & 0xff) == 2) {
					const char *s = "USB2.0-EPP/I2C/SPI (mock)";
					int k;
					tmp[0] = (uint8_t)(2 + 2 * strlen(s));
					tmp[1] = 3;
					for (k = 0; s[k]; k++) {
						tmp[2 + 2 * k] = (uint8_t)s[k];
						tmp[3 + 2 * k] = 0;
					}
					src = tmp; n = tmp[0];
				}
				break;
			}
			if (!src)
				return fail(ERROR_GEN_FAILURE);
		} else if (rt == 0xC0 && req == 0x5F) {
			tmp[0] = M.ic_ver;
			tmp[1] = 0x00;
			src = tmp; n = 2;
		} else if (rt == 0x02 && req == 1) {
			n = 0;
		} else {
			return fail(ERROR_GEN_FAILURE);
		}
		if (n > wl)
			n = wl;
		if (src)
			memcpy(b + 8, src, n);
		put32(b + 4, n);
		*returned = CH341_CMD_HEAD + n;
		return TRUE;
	}
	case CH341_FN_DATA_DOWN:
		M.n_write++;
		if (len > CH341_MAX_XFER || inlen != CH341_CMD_HEAD + len)
			return fail(ERROR_INVALID_PARAMETER);
		if (write_packets(b + 8, len, 0) < 0)
			return fail(ERROR_SEM_TIMEOUT);
		put32(b + 4, len);
		*returned = CH341_CMD_HEAD;
		return TRUE;
	case CH341_FN_DATA_UP: {
		uint32_t got = 0;
		int r;

		M.n_read++;
		if (len > CH341_MAX_XFER || outlen < CH341_CMD_HEAD + len || len < 32)
			return fail(ERROR_INVALID_PARAMETER);
		r = usb_read(b + 8, len, &got);
		if (r == 0) {
			/* nothing to send: the driver waits for its read timeout */
			Sleep(M.to_rd == CH341_TIMEOUT_INFINITE ? 1000 : (M.to_rd > 1000 ? 1000 : M.to_rd));
			return fail(ERROR_SEM_TIMEOUT);
		}
		if (r < 0)
			return fail(ERROR_GEN_FAILURE);
		put32(b + 4, got);
		*returned = CH341_CMD_HEAD + got;
		return TRUE;
	}
	case CH341_FN_WRITE_READ: {
		uint32_t wl, step, times, t, total = 0;
		uint8_t out[CH341_MAX_XFER];

		M.n_write_read++;
		if (len < 8 || inlen != CH341_CMD_HEAD + len)
			return fail(ERROR_INVALID_PARAMETER);
		wl = len - 8;
		step = get32(b + 8 + wl);
		times = get32(b + 12 + wl);
		if (!wl || wl > CH341_MAX_XFER || !step || !times || step * times > CH341_MAX_XFER
				|| outlen < CH341_CMD_HEAD + step * times)
			return fail(ERROR_INVALID_PARAMETER);
		/* the driver keeps IN requests pending while writing */
		write_packets(b + 8, wl, 1);
		for (t = 0; t < times; t++) {
			uint32_t got = 0;
			int r = usb_read(out + total, step, &got);
			if (r == 0) {
				M.violations++;     /* more answers predicted than the chip gave */
				return fail(ERROR_SEM_TIMEOUT);
			}
			if (r < 0) {
				M.violations++;
				return fail(ERROR_GEN_FAILURE);
			}
			total += got;
		}
		if (M.qn)
			M.violations++;         /* answers left behind: prediction too small */
		memcpy(b + 8, out, total);
		put32(b + 4, total);
		*returned = CH341_CMD_HEAD + total;
		return TRUE;
	}
	default:
		return fail(ERROR_INVALID_FUNCTION);
	}
}

const struct ch341_io_ops ch341_io_mock = {
	"mock", mock_enumerate, mock_open, mock_close, mock_ioctl
};
