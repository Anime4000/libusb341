/*
 * libusb341 - access to the WCH CH341 parallel/I2C/SPI driver.
 *
 * The driver (CH341WDM.SYS / CH341W64.SYS, "CH341PAR" package) exposes one
 * device interface per CH341 in EPP/MEM/I2C mode (USB 1A86:5512).  Every
 * operation is a METHOD_BUFFERED DeviceIoControl carrying a mWIN32_COMMAND:
 *
 *     ULONG mFunction;    in: function code / out: NTSTATUS
 *     ULONG mLength;      in: request length / out: data length
 *     UCHAR mBuffer[];    setup packet or data
 *
 * This is the same interface WCH's own CH341DLL.DLL uses; talking to it
 * directly means no 32/64-bit CH341DLL is needed.
 */
#include "shim.h"
#include <setupapi.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* ---------------------------------------------------------------------- */
/* Real backend: SetupAPI + CreateFile + DeviceIoControl                   */
/* ---------------------------------------------------------------------- */

static unsigned parse_hex4(const char *s)
{
	unsigned v = 0;
	int i;
	for (i = 0; i < 4; i++) {
		char c = s[i];
		v <<= 4;
		if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
		else return 0;
	}
	return v;
}

static void parse_hwid(const char *s, struct ch341_found *f)
{
	const char *p;
	if ((p = strstr(s, "VID_")) != NULL) f->vid = (uint16_t)parse_hex4(p + 4);
	if ((p = strstr(s, "PID_")) != NULL) f->pid = (uint16_t)parse_hex4(p + 4);
	if ((p = strstr(s, "REV_")) != NULL) f->rev = (uint16_t)parse_hex4(p + 4);
}

static int real_enumerate(struct ch341_found *out, int max)
{
	static const GUID guid = CH341_GUID_INIT;
	HDEVINFO di;
	DWORD i;
	int n = 0;

	di = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (di == INVALID_HANDLE_VALUE)
		return 0;

	for (i = 0; n < max; i++) {
		SP_DEVICE_INTERFACE_DATA ifd;
		SP_DEVINFO_DATA dev;
		SP_DEVICE_INTERFACE_DETAIL_DATA_W *det;
		DWORD need = 0, addr = 0;
		char hwid[1024];
		struct ch341_found *f = &out[n];

		ifd.cbSize = sizeof(ifd);
		if (!SetupDiEnumDeviceInterfaces(di, NULL, &guid, i, &ifd))
			break;
		SetupDiGetDeviceInterfaceDetailW(di, &ifd, NULL, 0, &need, NULL);
		if (need < sizeof(*det))
			continue;
		det = malloc(need);
		if (!det)
			break;
		det->cbSize = sizeof(*det);
		dev.cbSize = sizeof(dev);
		if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, det, need, NULL, &dev)) {
			free(det);
			continue;
		}
		memset(f, 0, sizeof(*f));
		wcsncpy(f->path, det->DevicePath, (sizeof(f->path) / sizeof(WCHAR)) - 1);
		free(det);
		SetupDiGetDeviceInstanceIdA(di, &dev, f->instance_id, sizeof(f->instance_id), NULL);
		memset(hwid, 0, sizeof(hwid));
		if (SetupDiGetDeviceRegistryPropertyA(di, &dev, SPDRP_HARDWAREID, NULL,
				(BYTE *)hwid, sizeof(hwid) - 2, NULL))
			parse_hwid(hwid, f);
		if (!f->vid)
			parse_hwid(f->instance_id, f);
		if (SetupDiGetDeviceRegistryPropertyA(di, &dev, SPDRP_ADDRESS, NULL,
				(BYTE *)&addr, sizeof(addr), NULL))
			f->address = addr;
		n++;
	}
	SetupDiDestroyDeviceInfoList(di);
	return n;
}

static HANDLE real_open(const WCHAR *path, DWORD *err)
{
	HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, NULL);
	*err = (h == INVALID_HANDLE_VALUE) ? GetLastError() : 0;
	return h;
}

static void real_close(HANDLE h)
{
	CloseHandle(h);
}

static BOOL real_ioctl(HANDLE h, void *buf, DWORD inlen, DWORD outlen, DWORD *returned)
{
	return DeviceIoControl(h, CH341_IOCTL, buf, inlen, buf, outlen, returned, NULL);
}

const struct ch341_io_ops ch341_io_real = {
	"wch", real_enumerate, real_open, real_close, real_ioctl
};

const struct ch341_io_ops *ch341_io = &ch341_io_real;

/* ---------------------------------------------------------------------- */
/* Commands                                                                */
/* ---------------------------------------------------------------------- */

int ch341_err_to_libusb(DWORD err, int timed_out)
{
	if (timed_out)
		return LIBUSB_ERROR_TIMEOUT;
	switch (err) {
	case ERROR_SEM_TIMEOUT:
	case WAIT_TIMEOUT:
	case ERROR_TIMEOUT:
		return LIBUSB_ERROR_TIMEOUT;
	case ERROR_DEVICE_NOT_CONNECTED:
	case ERROR_DEV_NOT_EXIST:
	case ERROR_NO_SUCH_DEVICE:
	case ERROR_DEVICE_REMOVED:
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_HANDLE:
	case ERROR_BAD_COMMAND:
		return LIBUSB_ERROR_NO_DEVICE;
	case ERROR_ACCESS_DENIED:
		return LIBUSB_ERROR_ACCESS;
	case ERROR_SHARING_VIOLATION:
	case ERROR_BUSY:
		return LIBUSB_ERROR_BUSY;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_NO_SYSTEM_RESOURCES:
		return LIBUSB_ERROR_NO_MEM;
	default:
		return LIBUSB_ERROR_IO;
	}
}

/* Map a failed driver call to a libusb error, treating failures that took
 * (almost) the whole programmed timeout as timeouts. */
static int fail(struct shim_handle *h, const char *what, ULONGLONG t0, ULONG timeout)
{
	DWORD err = GetLastError();
	ULONGLONG el = GetTickCount64() - t0;
	int to = (timeout != CH341_TIMEOUT_INFINITE && timeout && el + 20 >= timeout);
	int r = ch341_err_to_libusb(err, to);
	if (r == LIBUSB_ERROR_NO_DEVICE && h)
		h->gone = 1;
	LOGD("%s failed: win32 error %lu after %llu ms -> %s", what, (unsigned long)err,
		(unsigned long long)el, libusb_error_name(r));
	return r;
}

int ch341_ctrl_raw(HANDLE dh, uint8_t *io, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx,
	uint8_t *data, uint16_t len, int *actual)
{
	DWORD ret = 0, outlen;
	uint32_t n;

	*actual = 0;
	if (!(rt & LIBUSB_ENDPOINT_IN) && len)
		return LIBUSB_ERROR_NOT_SUPPORTED;  /* driver interface has no OUT data stage */
	if (len > CH341_MAX_XFER)
		return LIBUSB_ERROR_INVALID_PARAM;

	memset(io, 0, CH341_CMD_SMALL);
	put32(io, CH341_FN_DEVICE_CTRL);
	put32(io + 4, 8);
	io[8] = rt;
	io[9] = req;
	put16(io + 10, val);
	put16(io + 12, idx);
	put16(io + 14, len);
	outlen = CH341_CMD_HEAD + (len > CH341_PACKET ? len : CH341_PACKET);
	if (!ch341_io->ioctl(dh, io, CH341_CMD_SMALL, outlen, &ret))
		return -1000 - (int)GetLastError();
	if (rt & LIBUSB_ENDPOINT_IN) {
		n = get32(io + 4);
		if (n > len)
			n = len;
		if (n && data)
			memcpy(data, io + CH341_CMD_HEAD, n);
		*actual = (int)n;
	}
	return 0;
}

int ch341_ctrl(struct shim_handle *h, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx,
	uint8_t *data, uint16_t len, int *actual)
{
	ULONGLONG t0 = GetTickCount64();
	int r = ch341_ctrl_raw(h->h, h->io, rt, req, val, idx, data, len, actual);
	if (r <= -1000) {
		SetLastError((DWORD)(-1000 - r));
		r = fail(h, "control transfer", t0, h->to_rd);
		/* A stalled control request is how devices reject unsupported requests. */
		if (r == LIBUSB_ERROR_IO)
			r = LIBUSB_ERROR_PIPE;
	}
	return r;
}

int ch341_set_timeouts(struct shim_handle *h, unsigned int timeout_ms)
{
	ULONG v = timeout_ms ? (ULONG)timeout_ms : CH341_TIMEOUT_INFINITE;
	DWORD ret = 0;
	uint8_t *io = h->io;

	if (h->to_valid && h->to_wr == v && h->to_rd == v)
		return 0;
	memset(io, 0, CH341_CMD_SMALL);
	put32(io, CH341_FN_SET_TIMEOUT);
	put32(io + 4, 8);
	put32(io + 8, v);
	put32(io + 12, v);
	if (!ch341_io->ioctl(h->h, io, CH341_CMD_SMALL, CH341_CMD_SMALL, &ret)) {
		h->to_valid = 0;
		return fail(h, "set timeout", GetTickCount64(), 0);
	}
	h->to_wr = h->to_rd = v;
	h->to_valid = 1;
	return 0;
}

int ch341_read_raw(struct shim_handle *h, uint8_t *buf, int len, int *actual, int *overflow)
{
	/* The driver must be asked for at least one full packet (as CH341DLL does),
	 * otherwise a packet larger than the request would overflow. */
	uint32_t req = (uint32_t)len < CH341_PACKET ? CH341_PACKET : (uint32_t)len;
	uint32_t n;
	DWORD ret = 0;
	ULONGLONG t0 = GetTickCount64();
	uint8_t *io = h->io;

	*actual = 0;
	*overflow = 0;
	if (req > CH341_MAX_XFER)
		req = CH341_MAX_XFER;
	put32(io, CH341_FN_DATA_UP);
	put32(io + 4, req);
	if (!ch341_io->ioctl(h->h, io, CH341_CMD_HEAD, CH341_CMD_HEAD + req, &ret))
		return fail(h, "bulk read", t0, h->to_rd);
	n = get32(io + 4);
	if (n > req)
		n = req;
	if (n > (uint32_t)len) {
		*overflow = 1;
		n = (uint32_t)len;
	}
	memcpy(buf, io + CH341_CMD_HEAD, n);
	*actual = (int)n;
	shim_hexdump(4, "IN", buf, (int)n);
	return 0;
}

int ch341_write_raw(struct shim_handle *h, const uint8_t *buf, int len, int *actual)
{
	uint8_t *io = h->io;

	*actual = 0;
	while (len > 0) {
		uint32_t n = (uint32_t)len > CH341_MAX_XFER ? CH341_MAX_XFER : (uint32_t)len;
		uint32_t w;
		DWORD ret = 0;
		ULONGLONG t0 = GetTickCount64();

		put32(io, CH341_FN_DATA_DOWN);
		put32(io + 4, n);
		memcpy(io + CH341_CMD_HEAD, buf, n);
		shim_hexdump(4, "OUT", buf, (int)n);
		if (!ch341_io->ioctl(h->h, io, CH341_CMD_HEAD + n, CH341_CMD_HEAD, &ret))
			return fail(h, "bulk write", t0, h->to_wr);
		w = get32(io + 4);
		if (w > n)
			w = n;
		*actual += (int)w;
		if (w < n)
			return LIBUSB_ERROR_IO;
		buf += n;
		len -= (int)n;
	}
	return 0;
}

int ch341_write_read(struct shim_handle *h, const uint8_t *wbuf, int wlen, int pad,
	unsigned step, unsigned times, uint8_t *rbuf, int *ractual)
{
	uint32_t total = step * times, n;
	DWORD ret = 0;
	ULONGLONG t0 = GetTickCount64();
	uint8_t *io = h->io;
	int plen = pad ? (int)CH341_PACKET : 0;

	*ractual = 0;
	if (!total || total > CH341_MAX_XFER || wlen <= 0 || (uint32_t)(wlen + plen) > CH341_MAX_XFER)
		return LIBUSB_ERROR_INVALID_PARAM;
	put32(io, CH341_FN_WRITE_READ);
	memcpy(io + CH341_CMD_HEAD, wbuf, (size_t)wlen);
	shim_hexdump(4, "OUT", wbuf, wlen);
	/* CH341DLL appends an all-zero (no-op) packet for IC version 0x20 when the
	 * stream ends on a packet boundary; do the same. */
	if (plen) {
		memset(io + CH341_CMD_HEAD + wlen, 0, (size_t)plen);
		wlen += plen;
	}
	put32(io + 4, (uint32_t)wlen + 8);
	put32(io + CH341_CMD_HEAD + wlen, step);
	put32(io + CH341_CMD_HEAD + wlen + 4, times);
	LOGD("write_read: %d bytes out, %u x %u bytes in", wlen, times, step);
	if (!ch341_io->ioctl(h->h, io, CH341_CMD_HEAD + (DWORD)wlen + 8, CH341_CMD_HEAD + total, &ret))
		return fail(h, "write/read", t0, h->to_rd);
	n = get32(io + 4);
	if (n > total)
		n = total;
	memcpy(rbuf, io + CH341_CMD_HEAD, n);
	*ractual = (int)n;
	shim_hexdump(4, "IN", rbuf, (int)n);
	return 0;
}

int ch341_reset(struct shim_handle *h)
{
	DWORD ret = 0;
	uint8_t *io = h->io;

	memset(io, 0, CH341_CMD_SMALL);
	put32(io, CH341_FN_RESET_DEVICE);
	if (!ch341_io->ioctl(h->h, io, CH341_CMD_SMALL, CH341_CMD_SMALL, &ret))
		return fail(h, "reset", GetTickCount64(), 0);
	h->to_valid = 0;
	return 0;
}
