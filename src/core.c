/*
 * libusb341 - libusb-1.0 API.
 *
 * CH341 chips bound to the WCH driver are served by this DLL; everything else
 * (contexts, other devices, their transfers) is forwarded to the original
 * libusb-1.0.dll when it is present.  Objects belonging to the shim are told
 * apart from the original library's by registry lookup, never by peeking into
 * foreign memory.
 */
#include "shim.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

CRITICAL_SECTION g_lock;
int g_debug;
int g_disable_ch341;
int g_batch = 127;
struct shim_ctx g_default_ctx = { MAGIC_CTX, NULL, 0, 0, 0, NULL, NULL };
struct shim_handle *g_handles;
static struct shim_ctx *g_ctxs;
static struct shim_device *g_devs;
static char g_logpath[MAX_PATH];
static FILE *g_logf;

#define MAX_FOUND 16

/* ---------------------------------------------------------------------- */
/* Logging, startup                                                        */
/* ---------------------------------------------------------------------- */

void shim_log(int level, const char *fmt, ...)
{
	va_list ap;
	FILE *f;

	if (level > g_debug)
		return;
	if (g_logpath[0] && !g_logf) {
		g_logf = fopen(g_logpath, "a");
		if (!g_logf)
			g_logpath[0] = 0;
	}
	f = g_logf ? g_logf : stderr;
	fprintf(f, "[libusb341] ");
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fflush(f);
}

void shim_hexdump(int level, const char *what, const uint8_t *p, int n)
{
	char line[3 * 32 + 1];
	int i, o = 0, shown = n > 128 ? 128 : n;

	if (level > g_debug || n <= 0)
		return;
	shim_log(level, "%s %d bytes%s", what, n, n > shown ? " (first 128)" : "");
	for (i = 0; i < shown; i++) {
		o += sprintf(line + o, "%02x ", p[i]);
		if ((i & 31) == 31 || i == shown - 1) {
			shim_log(level, "  %s", line);
			o = 0;
		}
	}
}

static int env_int(const char *name, int def)
{
	char buf[32];
	DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));

	if (n == 0 || n >= sizeof(buf))
		return def;
	return atoi(buf);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		DWORD n;

		DisableThreadLibraryCalls(inst);
		InitializeCriticalSection(&g_lock);
		g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
		g_debug = env_int("LIBUSB341_DEBUG", 0);
		g_disable_ch341 = env_int("LIBUSB341_DISABLE", 0);
		g_batch = env_int("LIBUSB341_BATCH", 127);
		if (g_batch < 1)
			g_batch = 1;
		if (g_batch > 127)
			g_batch = 127;
		n = GetEnvironmentVariableA("LIBUSB341_LOG", g_logpath, sizeof(g_logpath));
		if (n == 0 || n >= sizeof(g_logpath))
			g_logpath[0] = 0;
		else if (!g_debug)
			g_debug = 3;
#ifdef LIBUSB341_MOCK
		if (env_int("LIBUSB341_MOCK", 0))
			ch341_io = &ch341_io_mock;
#endif
	}
	return TRUE;
}

const char * LIBUSB_CALL libusb341_version(void)
{
	return "libusb341 " SHIM_VERSION_STR " (libusb-1.0 for WCH CH341 driver)";
}

static struct shim_device *find_dev(libusb_device *dev);

/* Diagnostics: 1 and the device instance ID if dev is a CH341 served through
 * the WCH driver, 0 if it belongs to the original libusb. */
int LIBUSB_CALL libusb341_device_info(libusb_device *dev, char *buf, int len)
{
	struct shim_device *d = find_dev(dev);

	if (!d)
		return 0;
	if (buf && len > 0) {
		strncpy(buf, d->instance_id, (size_t)len - 1);
		buf[len - 1] = 0;
	}
	return 1;
}

/* Diagnostics: 1 if the original libusb-1.0.orig.dll is loaded. */
int LIBUSB_CALL libusb341_passthrough(void)
{
	return orig_load();
}

/* ---------------------------------------------------------------------- */
/* Registry lookups (call with g_lock held)                                */
/* ---------------------------------------------------------------------- */

static struct shim_ctx *ctx_get(libusb_context *ctx)
{
	struct shim_ctx *c;

	if (!ctx)
		return &g_default_ctx;
	for (c = g_ctxs; c; c = c->next)
		if ((libusb_context *)c == ctx)
			return c;
	LOGW("unknown context %p, using the default context", (void *)ctx);
	return &g_default_ctx;
}

static struct shim_device *our_dev(libusb_device *dev)
{
	struct shim_device *d;

	for (d = g_devs; d; d = d->next)
		if ((libusb_device *)d == dev)
			return d;
	return NULL;
}

static struct shim_handle *our_handle(libusb_device_handle *hd)
{
	struct shim_handle *h;

	for (h = g_handles; h; h = h->next)
		if ((libusb_device_handle *)h == hd)
			return h;
	return NULL;
}

static struct shim_device *find_dev(libusb_device *dev)
{
	struct shim_device *d;

	EnterCriticalSection(&g_lock);
	d = our_dev(dev);
	LeaveCriticalSection(&g_lock);
	return d;
}

static struct shim_handle *find_handle(libusb_device_handle *hd)
{
	struct shim_handle *h;

	EnterCriticalSection(&g_lock);
	h = our_handle(hd);
	LeaveCriticalSection(&g_lock);
	return h;
}

/* Returns the handle with g_lock held, or NULL (lock released). */
static struct shim_handle *lock_handle(libusb_device_handle *hd)
{
	struct shim_handle *h;

	EnterCriticalSection(&g_lock);
	h = our_handle(hd);
	if (!h)
		LeaveCriticalSection(&g_lock);
	return h;
}

/* Context inside the original libusb that corresponds to c. */
static int orig_ctx(struct shim_ctx *c, libusb_context **oc)
{
	int ok;

	*oc = NULL;
	if (!O.mod)
		return 0;
	EnterCriticalSection(&g_lock);
	if (c == &g_default_ctx) {
		if (!g_default_ctx.orig_ok) {
			/* Default context used without libusb_init(NULL); init it lazily. */
			if (O.libusb_init(NULL) == 0) {
				g_default_ctx.orig_ok = 1;
				g_default_ctx.orig_refs++;
			}
		}
		ok = g_default_ctx.orig_ok;
	} else {
		*oc = c->orig;
		ok = c->orig_ok;
	}
	LeaveCriticalSection(&g_lock);
	return ok;
}

static libusb_context *octx_of(libusb_context *ctx, int *ok)
{
	libusb_context *oc;
	struct shim_ctx *c;

	EnterCriticalSection(&g_lock);
	c = ctx_get(ctx);
	LeaveCriticalSection(&g_lock);
	*ok = orig_ctx(c, &oc);
	return oc;
}

/* ---------------------------------------------------------------------- */
/* Library / context                                                       */
/* ---------------------------------------------------------------------- */

int LIBUSB_CALL libusb_init_context(libusb_context **ctxp, const struct libusb_init_option options[], int num_options)
{
	int have_orig = orig_load(), r;
	struct shim_ctx *c;

	if (!ctxp) {
		EnterCriticalSection(&g_lock);
		g_default_ctx.refcnt++;
		LeaveCriticalSection(&g_lock);
		if (have_orig) {
			r = O.libusb_init_context ? O.libusb_init_context(NULL, options, num_options) : O.libusb_init(NULL);
			if (r == 0) {
				EnterCriticalSection(&g_lock);
				g_default_ctx.orig_refs++;
				g_default_ctx.orig_ok = 1;
				LeaveCriticalSection(&g_lock);
			} else {
				LOGW("original libusb_init failed: %d", r);
			}
		}
		return LIBUSB_SUCCESS;
	}

	c = calloc(1, sizeof(*c));
	if (!c)
		return LIBUSB_ERROR_NO_MEM;
	c->magic = MAGIC_CTX;
	if (have_orig) {
		r = O.libusb_init_context ? O.libusb_init_context(&c->orig, options, num_options) : O.libusb_init(&c->orig);
		c->orig_ok = (r == 0);
		if (r)
			LOGW("original libusb_init failed: %d", r);
	}
	EnterCriticalSection(&g_lock);
	c->next = g_ctxs;
	g_ctxs = c;
	LeaveCriticalSection(&g_lock);
	*ctxp = (libusb_context *)c;
	return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_init(libusb_context **ctxp)
{
	return libusb_init_context(ctxp, NULL, 0);
}

void LIBUSB_CALL libusb_exit(libusb_context *ctx)
{
	struct shim_ctx *c, **pp;
	struct shim_device *d;
	struct shim_handle *h;
	int do_orig = 0;

	EnterCriticalSection(&g_lock);
	c = ctx_get(ctx);
	if (c == &g_default_ctx) {
		if (c->refcnt > 0)
			c->refcnt--;
		if (c->orig_refs > 0) {
			c->orig_refs--;
			c->orig_ok = c->orig_refs > 0;
			do_orig = 1;
		}
		LeaveCriticalSection(&g_lock);
		if (do_orig)
			O.libusb_exit(NULL);
		return;
	}
	for (pp = &g_ctxs; *pp; pp = &(*pp)->next)
		if (*pp == c) {
			*pp = c->next;
			break;
		}
	/* Objects outliving their context are adopted by the default one. */
	for (d = g_devs; d; d = d->next)
		if (d->ctx == c)
			d->ctx = &g_default_ctx;
	for (h = g_handles; h; h = h->next)
		if (h->ctx == c)
			h->ctx = &g_default_ctx;
	LeaveCriticalSection(&g_lock);
	if (c->orig_ok)
		O.libusb_exit(c->orig);
	c->magic = 0;
	free(c);
}

void LIBUSB_CALL libusb_set_debug(libusb_context *ctx, int level)
{
	int ok;
	libusb_context *oc = octx_of(ctx, &ok);

	if (ok && O.libusb_set_debug)
		O.libusb_set_debug(oc, level);
}

void LIBUSB_CALL libusb_set_log_cb(libusb_context *ctx, libusb_log_cb cb, int mode)
{
	if (!orig_load() || !O.libusb_set_log_cb)
		return;
	if (ctx) {
		int ok;
		libusb_context *oc = octx_of(ctx, &ok);
		if (ok)
			O.libusb_set_log_cb(oc, cb, mode);
	} else {
		O.libusb_set_log_cb(NULL, cb, mode);
	}
}

int LIBUSB_CALLV libusb_set_option(libusb_context *ctx, enum libusb_option option, ...)
{
	va_list ap;
	int ival = 0;
	libusb_context *oc = NULL;

	va_start(ap, option);
	switch (option) {
	case LIBUSB_OPTION_LOG_LEVEL:
		ival = va_arg(ap, int);
		break;
	case LIBUSB_OPTION_USE_USBDK:
	case LIBUSB_OPTION_NO_DEVICE_DISCOVERY:
		break;
	case LIBUSB_OPTION_LOG_CB:
		/* Not forwarded: some callers pass LIBUSB_OPTION_LOG_CB (3) meaning
		 * "log level 3" with no argument, which would hand the original
		 * libusb a garbage function pointer.  Use libusb_set_log_cb(). */
		va_end(ap);
		return LIBUSB_SUCCESS;
	default:
		va_end(ap);
		return LIBUSB_ERROR_INVALID_PARAM;
	}
	va_end(ap);

	if (!orig_load() || !O.libusb_set_option)
		return LIBUSB_SUCCESS;
	if (ctx) {
		int ok;
		oc = octx_of(ctx, &ok);
		if (!ok)
			return LIBUSB_SUCCESS;
	}
	if (option == LIBUSB_OPTION_LOG_LEVEL)
		return O.libusb_set_option(oc, option, ival);
	return O.libusb_set_option(oc, option);
}

const struct libusb_version * LIBUSB_CALL libusb_get_version(void)
{
	static const struct libusb_version v = {
		1, 0, 30, 0, "", "libusb341 " SHIM_VERSION_STR " (WCH CH341 driver shim)"
	};
	return &v;
}

int LIBUSB_CALL libusb_has_capability(uint32_t capability)
{
	if (orig_load() && O.libusb_has_capability)
		return O.libusb_has_capability(capability);
	return capability == LIBUSB_CAP_HAS_CAPABILITY;
}

int LIBUSB_CALL libusb_setlocale(const char *locale)
{
	if (orig_load() && O.libusb_setlocale)
		return O.libusb_setlocale(locale);
	return locale ? LIBUSB_SUCCESS : LIBUSB_ERROR_INVALID_PARAM;
}

const char * LIBUSB_CALL libusb_error_name(int code)
{
	switch (code) {
	case LIBUSB_ERROR_IO: return "LIBUSB_ERROR_IO";
	case LIBUSB_ERROR_INVALID_PARAM: return "LIBUSB_ERROR_INVALID_PARAM";
	case LIBUSB_ERROR_ACCESS: return "LIBUSB_ERROR_ACCESS";
	case LIBUSB_ERROR_NO_DEVICE: return "LIBUSB_ERROR_NO_DEVICE";
	case LIBUSB_ERROR_NOT_FOUND: return "LIBUSB_ERROR_NOT_FOUND";
	case LIBUSB_ERROR_BUSY: return "LIBUSB_ERROR_BUSY";
	case LIBUSB_ERROR_TIMEOUT: return "LIBUSB_ERROR_TIMEOUT";
	case LIBUSB_ERROR_OVERFLOW: return "LIBUSB_ERROR_OVERFLOW";
	case LIBUSB_ERROR_PIPE: return "LIBUSB_ERROR_PIPE";
	case LIBUSB_ERROR_INTERRUPTED: return "LIBUSB_ERROR_INTERRUPTED";
	case LIBUSB_ERROR_NO_MEM: return "LIBUSB_ERROR_NO_MEM";
	case LIBUSB_ERROR_NOT_SUPPORTED: return "LIBUSB_ERROR_NOT_SUPPORTED";
	case LIBUSB_ERROR_OTHER: return "LIBUSB_ERROR_OTHER";
	case LIBUSB_TRANSFER_ERROR: return "LIBUSB_TRANSFER_ERROR";
	case LIBUSB_TRANSFER_TIMED_OUT: return "LIBUSB_TRANSFER_TIMED_OUT";
	case LIBUSB_TRANSFER_CANCELLED: return "LIBUSB_TRANSFER_CANCELLED";
	case LIBUSB_TRANSFER_STALL: return "LIBUSB_TRANSFER_STALL";
	case LIBUSB_TRANSFER_NO_DEVICE: return "LIBUSB_TRANSFER_NO_DEVICE";
	case LIBUSB_TRANSFER_OVERFLOW: return "LIBUSB_TRANSFER_OVERFLOW";
	case 0: return "LIBUSB_SUCCESS / LIBUSB_TRANSFER_COMPLETED";
	default: return "**UNKNOWN**";
	}
}

const char * LIBUSB_CALL libusb_strerror(int code)
{
	switch (code) {
	case LIBUSB_SUCCESS: return "Success";
	case LIBUSB_ERROR_IO: return "Input/Output Error";
	case LIBUSB_ERROR_INVALID_PARAM: return "Invalid parameter";
	case LIBUSB_ERROR_ACCESS: return "Access denied (insufficient permissions)";
	case LIBUSB_ERROR_NO_DEVICE: return "No such device (it may have been disconnected)";
	case LIBUSB_ERROR_NOT_FOUND: return "Entity not found";
	case LIBUSB_ERROR_BUSY: return "Resource busy";
	case LIBUSB_ERROR_TIMEOUT: return "Operation timed out";
	case LIBUSB_ERROR_OVERFLOW: return "Overflow";
	case LIBUSB_ERROR_PIPE: return "Pipe error";
	case LIBUSB_ERROR_INTERRUPTED: return "System call interrupted (perhaps due to signal)";
	case LIBUSB_ERROR_NO_MEM: return "Insufficient memory";
	case LIBUSB_ERROR_NOT_SUPPORTED: return "Operation not supported or unimplemented on this platform";
	case LIBUSB_ERROR_OTHER: return "Other error";
	default: return "Unknown error";
	}
}

/* ---------------------------------------------------------------------- */
/* CH341 devices                                                           */
/* ---------------------------------------------------------------------- */

/* Descriptors of a CH341A in I2C/SPI/parallel mode, used only when the chip
 * cannot be asked (e.g. it is held open exclusively by another program). */
static const uint8_t ch341_cfg_default[] = {
	0x09, 0x02, 0x27, 0x00, 0x01, 0x01, 0x00, 0x80, 0x31,
	0x09, 0x04, 0x00, 0x00, 0x03, 0xff, 0x01, 0x02, 0x00,
	0x07, 0x05, 0x82, 0x02, 0x20, 0x00, 0x00,
	0x07, 0x05, 0x02, 0x02, 0x20, 0x00, 0x00,
	0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x01,
};

static void dev_read_descriptors(struct shim_device *d, const struct ch341_found *f)
{
	HANDLE dh = INVALID_HANDLE_VALUE;
	uint8_t *io = malloc(CH341_CMD_HEAD + CH341_MAX_XFER + 64);
	uint8_t hdr[LIBUSB_DT_CONFIG_SIZE];
	int act = 0, ok_dev = 0;
	DWORD err = 0;

	if (io)
		dh = ch341_io->open(d->path, &err);
	if (dh != INVALID_HANDLE_VALUE) {
		if (ch341_ctrl_raw(dh, io, 0x80, LIBUSB_REQUEST_GET_DESCRIPTOR, LIBUSB_DT_DEVICE << 8, 0,
				d->dev_desc, LIBUSB_DT_DEVICE_SIZE, &act) == 0
				&& act == LIBUSB_DT_DEVICE_SIZE && d->dev_desc[1] == LIBUSB_DT_DEVICE)
			ok_dev = 1;
		if (ch341_ctrl_raw(dh, io, 0x80, LIBUSB_REQUEST_GET_DESCRIPTOR, LIBUSB_DT_CONFIG << 8, 0,
				hdr, sizeof(hdr), &act) == 0 && act == (int)sizeof(hdr) && hdr[1] == LIBUSB_DT_CONFIG) {
			int total = hdr[2] | (hdr[3] << 8);
			if (total >= LIBUSB_DT_CONFIG_SIZE && total <= (int)CH341_MAX_XFER) {
				uint8_t *cfg = malloc((size_t)total);
				if (cfg && ch341_ctrl_raw(dh, io, 0x80, LIBUSB_REQUEST_GET_DESCRIPTOR,
						LIBUSB_DT_CONFIG << 8, 0, cfg, (uint16_t)total, &act) == 0
						&& act == total) {
					d->cfg_desc = cfg;
					d->cfg_len = total;
				} else {
					free(cfg);
				}
			}
		}
		ch341_io->close(dh);
	} else {
		LOGI("cannot open %ls to read descriptors (error %lu); using defaults",
			d->path, (unsigned long)err);
	}
	free(io);

	if (!ok_dev) {
		static const uint8_t def[LIBUSB_DT_DEVICE_SIZE] = {
			0x12, 0x01, 0x10, 0x01, 0xff, 0x00, 0x00, 0x08, 0x86, 0x1a, 0x12, 0x55,
			0x04, 0x03, 0x00, 0x02, 0x00, 0x01
		};
		memcpy(d->dev_desc, def, sizeof(def));
		if (f->vid) {
			d->dev_desc[8] = (uint8_t)f->vid;
			d->dev_desc[9] = (uint8_t)(f->vid >> 8);
		}
		if (f->pid) {
			d->dev_desc[10] = (uint8_t)f->pid;
			d->dev_desc[11] = (uint8_t)(f->pid >> 8);
		}
		if (f->rev) {
			d->dev_desc[12] = (uint8_t)f->rev;
			d->dev_desc[13] = (uint8_t)(f->rev >> 8);
		}
	}
	if (!d->cfg_desc) {
		d->cfg_desc = malloc(sizeof(ch341_cfg_default));
		if (d->cfg_desc) {
			memcpy(d->cfg_desc, ch341_cfg_default, sizeof(ch341_cfg_default));
			d->cfg_len = (int)sizeof(ch341_cfg_default);
		}
	}
}

static void dev_unref(struct shim_device *d)
{
	struct shim_device **pp;
	int free_it = 0;

	EnterCriticalSection(&g_lock);
	if (--d->refcnt == 0) {
		for (pp = &g_devs; *pp; pp = &(*pp)->next)
			if (*pp == d) {
				*pp = d->next;
				break;
			}
		free_it = 1;
	}
	LeaveCriticalSection(&g_lock);
	if (free_it) {
		d->magic = 0;
		free(d->cfg_desc);
		free(d);
	}
}

/* Enumerate CH341s on the WCH driver; each returned device carries one reference. */
static int enum_ch341(struct shim_ctx *c, struct shim_device **out)
{
	struct ch341_found found[MAX_FOUND];
	struct shim_device *d;
	int n, i, k = 0;

	if (g_disable_ch341)
		return 0;
	n = ch341_io->enumerate(found, MAX_FOUND);

	EnterCriticalSection(&g_lock);
	for (d = g_devs; d; d = d->next)
		if (d->ctx == c && !d->gone)
			d->gone = 2;    /* provisional: cleared if still present */
	for (i = 0; i < n; i++) {
		for (d = g_devs; d; d = d->next)
			if (d->ctx == c && d->gone != 1 && _wcsicmp(d->path, found[i].path) == 0)
				break;
		if (d) {
			d->gone = 0;
			d->refcnt++;
		} else {
			d = calloc(1, sizeof(*d));
			if (!d)
				continue;
			d->magic = MAGIC_DEV;
			d->refcnt = 1;
			d->ctx = c;
			wcscpy(d->path, found[i].path);
			strcpy(d->instance_id, found[i].instance_id);
			d->port = (uint8_t)found[i].address;
			dev_read_descriptors(d, &found[i]);
			d->next = g_devs;
			g_devs = d;
			LOGI("found CH341 %04x:%04x at %s", d->dev_desc[8] | (d->dev_desc[9] << 8),
				d->dev_desc[10] | (d->dev_desc[11] << 8), d->instance_id);
		}
		out[k++] = d;
	}
	for (d = g_devs; d; d = d->next)
		if (d->gone == 2)
			d->gone = 1;
	LeaveCriticalSection(&g_lock);
	return k;
}

static int handle_open(struct shim_device *d, struct shim_handle **out)
{
	struct shim_handle *h;
	uint8_t ver[8];
	DWORD err = 0;
	int act = 0;

	*out = NULL;
	if (d->gone == 1)
		return LIBUSB_ERROR_NO_DEVICE;
	h = calloc(1, sizeof(*h));
	if (!h)
		return LIBUSB_ERROR_NO_MEM;
	h->io = malloc(CH341_CMD_HEAD + CH341_MAX_XFER + 64);
	if (!h->io) {
		free(h);
		return LIBUSB_ERROR_NO_MEM;
	}
	h->h = ch341_io->open(d->path, &err);
	if (h->h == INVALID_HANDLE_VALUE) {
		int r = ch341_err_to_libusb(err, 0);
		LOGE("cannot open %ls: error %lu", d->path, (unsigned long)err);
		free(h->io);
		free(h);
		return r;
	}
	h->magic = MAGIC_HANDLE;
	h->dev = d;
	/* Same request CH341DLL uses to read the chip version. */
	if (ch341_ctrl_raw(h->h, h->io, 0xC0, 0x5F, 0, 0, ver, sizeof(ver), &act) == 0 && act >= 1)
		h->ic_ver = ver[0];

	EnterCriticalSection(&g_lock);
	d->refcnt++;
	h->ctx = d->ctx;
	h->next = g_handles;
	g_handles = h;
	LeaveCriticalSection(&g_lock);
	LOGI("opened CH341 %s (IC version 0x%02x)", d->instance_id, h->ic_ver);
	*out = h;
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Device list / devices                                                   */
/* ---------------------------------------------------------------------- */

ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context *ctx, libusb_device ***list)
{
	struct shim_device *ours[MAX_FOUND];
	libusb_device **olist = NULL;
	ssize_t n_orig = 0;
	struct shim_ctx *c;
	libusb_context *oc;
	void **arr;
	int n_ours, i;

	if (!list)
		return LIBUSB_ERROR_INVALID_PARAM;
	EnterCriticalSection(&g_lock);
	c = ctx_get(ctx);
	LeaveCriticalSection(&g_lock);

	n_ours = enum_ch341(c, ours);
	if (orig_ctx(c, &oc)) {
		n_orig = O.libusb_get_device_list(oc, &olist);
		if (n_orig < 0) {
			LOGW("original libusb_get_device_list failed: %d", (int)n_orig);
			n_orig = 0;
			olist = NULL;
		}
	}
	arr = malloc(sizeof(void *) * (size_t)(n_ours + n_orig + 2));
	if (!arr) {
		for (i = 0; i < n_ours; i++)
			dev_unref(ours[i]);
		if (olist)
			O.libusb_free_device_list(olist, 1);
		return LIBUSB_ERROR_NO_MEM;
	}
	arr[0] = olist;     /* hidden slot: the original library's list */
	for (i = 0; i < n_ours; i++)
		arr[1 + i] = ours[i];
	for (i = 0; i < n_orig; i++)
		arr[1 + n_ours + i] = olist[i];
	arr[1 + n_ours + n_orig] = NULL;
	*list = (libusb_device **)(arr + 1);
	return n_ours + n_orig;
}

void LIBUSB_CALL libusb_free_device_list(libusb_device **list, int unref_devices)
{
	void **arr;
	libusb_device **olist;
	int i;

	if (!list)
		return;
	arr = (void **)list - 1;
	olist = arr[0];
	if (unref_devices) {
		for (i = 0; list[i]; i++) {
			struct shim_device *d = find_dev(list[i]);
			if (d)
				dev_unref(d);
		}
	}
	if (olist && O.libusb_free_device_list)
		O.libusb_free_device_list(olist, unref_devices);
	free(arr);
}

libusb_device * LIBUSB_CALL libusb_ref_device(libusb_device *dev)
{
	struct shim_device *d;

	EnterCriticalSection(&g_lock);
	d = our_dev(dev);
	if (d)
		d->refcnt++;
	LeaveCriticalSection(&g_lock);
	if (d)
		return dev;
	return O.libusb_ref_device ? O.libusb_ref_device(dev) : dev;
}

void LIBUSB_CALL libusb_unref_device(libusb_device *dev)
{
	struct shim_device *d;

	if (!dev)
		return;
	d = find_dev(dev);
	if (d)
		dev_unref(d);
	else if (O.libusb_unref_device)
		O.libusb_unref_device(dev);
}

int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *desc)
{
	struct shim_device *d = find_dev(dev);

	if (d) {
		const uint8_t *b = d->dev_desc;
		desc->bLength = b[0];
		desc->bDescriptorType = b[1];
		desc->bcdUSB = (uint16_t)(b[2] | (b[3] << 8));
		desc->bDeviceClass = b[4];
		desc->bDeviceSubClass = b[5];
		desc->bDeviceProtocol = b[6];
		desc->bMaxPacketSize0 = b[7];
		desc->idVendor = (uint16_t)(b[8] | (b[9] << 8));
		desc->idProduct = (uint16_t)(b[10] | (b[11] << 8));
		desc->bcdDevice = (uint16_t)(b[12] | (b[13] << 8));
		desc->iManufacturer = b[14];
		desc->iProduct = b[15];
		desc->iSerialNumber = b[16];
		desc->bNumConfigurations = b[17];
		return 0;
	}
	return O.libusb_get_device_descriptor ? O.libusb_get_device_descriptor(dev, desc) : LIBUSB_ERROR_NOT_SUPPORTED;
}

static int our_config(struct shim_device *d, struct libusb_config_descriptor **config)
{
	int r;

	if (!d->cfg_desc)
		return LIBUSB_ERROR_NOT_FOUND;
	r = desc_parse_config(d->cfg_desc, d->cfg_len, config);
	if (!r)
		desc_track(*config);
	return r;
}

int LIBUSB_CALL libusb_get_active_config_descriptor(libusb_device *dev, struct libusb_config_descriptor **config)
{
	struct shim_device *d = find_dev(dev);

	if (d)
		return our_config(d, config);
	return O.libusb_get_active_config_descriptor ?
		O.libusb_get_active_config_descriptor(dev, config) : LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_get_config_descriptor(libusb_device *dev, uint8_t config_index, struct libusb_config_descriptor **config)
{
	struct shim_device *d = find_dev(dev);

	if (d)
		return config_index == 0 ? our_config(d, config) : LIBUSB_ERROR_NOT_FOUND;
	return O.libusb_get_config_descriptor ?
		O.libusb_get_config_descriptor(dev, config_index, config) : LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_get_config_descriptor_by_value(libusb_device *dev, uint8_t value, struct libusb_config_descriptor **config)
{
	struct shim_device *d = find_dev(dev);

	if (d) {
		if (d->cfg_desc && d->cfg_len > 5 && d->cfg_desc[5] == value)
			return our_config(d, config);
		return LIBUSB_ERROR_NOT_FOUND;
	}
	return O.libusb_get_config_descriptor_by_value ?
		O.libusb_get_config_descriptor_by_value(dev, value, config) : LIBUSB_ERROR_NOT_SUPPORTED;
}

void LIBUSB_CALL libusb_free_config_descriptor(struct libusb_config_descriptor *config)
{
	if (!config)
		return;
	if (desc_untrack(config))
		desc_free_config(config);
	else if (O.libusb_free_config_descriptor)
		O.libusb_free_config_descriptor(config);
}

static int our_iad(struct libusb_interface_association_descriptor_array **iad_array)
{
	struct libusb_interface_association_descriptor_array *a = calloc(1, sizeof(*a));

	if (!a)
		return LIBUSB_ERROR_NO_MEM;
	desc_track(a);
	*iad_array = a;     /* the CH341 has no interface association descriptors */
	return 0;
}

int LIBUSB_CALL libusb_get_interface_association_descriptors(libusb_device *dev, uint8_t config_index,
	struct libusb_interface_association_descriptor_array **iad_array)
{
	struct shim_device *d = find_dev(dev);

	if (d)
		return config_index == 0 ? our_iad(iad_array) : LIBUSB_ERROR_NOT_FOUND;
	return O.libusb_get_interface_association_descriptors ?
		O.libusb_get_interface_association_descriptors(dev, config_index, iad_array) : LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_get_active_interface_association_descriptors(libusb_device *dev,
	struct libusb_interface_association_descriptor_array **iad_array)
{
	struct shim_device *d = find_dev(dev);

	if (d)
		return our_iad(iad_array);
	return O.libusb_get_active_interface_association_descriptors ?
		O.libusb_get_active_interface_association_descriptors(dev, iad_array) : LIBUSB_ERROR_NOT_SUPPORTED;
}

void LIBUSB_CALL libusb_free_interface_association_descriptors(struct libusb_interface_association_descriptor_array *iad_array)
{
	if (!iad_array)
		return;
	if (desc_untrack(iad_array))
		free(iad_array);
	else if (O.libusb_free_interface_association_descriptors)
		O.libusb_free_interface_association_descriptors(iad_array);
}

uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device *dev)
{
	if (find_dev(dev))
		return 0;
	return O.libusb_get_bus_number ? O.libusb_get_bus_number(dev) : 0;
}

uint8_t LIBUSB_CALL libusb_get_port_number(libusb_device *dev)
{
	struct shim_device *d = find_dev(dev);

	if (d)
		return d->port;
	return O.libusb_get_port_number ? O.libusb_get_port_number(dev) : 0;
}

int LIBUSB_CALL libusb_get_port_numbers(libusb_device *dev, uint8_t *port_numbers, int port_numbers_len)
{
	struct shim_device *d = find_dev(dev);

	if (d) {
		if (port_numbers_len < 1)
			return LIBUSB_ERROR_OVERFLOW;
		port_numbers[0] = d->port;
		return 1;
	}
	return O.libusb_get_port_numbers ? O.libusb_get_port_numbers(dev, port_numbers, port_numbers_len) : LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_get_port_path(libusb_context *ctx, libusb_device *dev, uint8_t *path, uint8_t path_length)
{
	if (find_dev(dev))
		return libusb_get_port_numbers(dev, path, path_length);
	if (O.libusb_get_port_path) {
		int ok;
		libusb_context *oc = octx_of(ctx, &ok);
		return O.libusb_get_port_path(oc, dev, path, path_length);
	}
	return LIBUSB_ERROR_NOT_SUPPORTED;
}

libusb_device * LIBUSB_CALL libusb_get_parent(libusb_device *dev)
{
	if (find_dev(dev))
		return NULL;
	return O.libusb_get_parent ? O.libusb_get_parent(dev) : NULL;
}

uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device *dev)
{
	struct shim_device *d = find_dev(dev);

	if (d)
		return d->port ? d->port : 1;
	return O.libusb_get_device_address ? O.libusb_get_device_address(dev) : 0;
}

int LIBUSB_CALL libusb_get_device_speed(libusb_device *dev)
{
	if (find_dev(dev))
		return LIBUSB_SPEED_FULL;
	return O.libusb_get_device_speed ? O.libusb_get_device_speed(dev) : LIBUSB_SPEED_UNKNOWN;
}

static int our_max_packet(struct shim_device *d, unsigned char ep)
{
	int pos = 0;

	while (d->cfg_desc && pos + 2 <= d->cfg_len && d->cfg_desc[pos] >= 2) {
		const uint8_t *p = d->cfg_desc + pos;
		if (p[1] == LIBUSB_DT_ENDPOINT && p[0] >= LIBUSB_DT_ENDPOINT_SIZE
				&& pos + LIBUSB_DT_ENDPOINT_SIZE <= d->cfg_len && p[2] == ep)
			return (p[4] | (p[5] << 8)) & 0x7ff;
		pos += p[0];
	}
	return LIBUSB_ERROR_NOT_FOUND;
}

int LIBUSB_CALL libusb_get_max_packet_size(libusb_device *dev, unsigned char endpoint)
{
	struct shim_device *d = find_dev(dev);

	if (d)
		return our_max_packet(d, endpoint);
	return O.libusb_get_max_packet_size ? O.libusb_get_max_packet_size(dev, endpoint) : LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_get_max_iso_packet_size(libusb_device *dev, unsigned char endpoint)
{
	struct shim_device *d = find_dev(dev);

	if (d)
		return our_max_packet(d, endpoint);
	return O.libusb_get_max_iso_packet_size ? O.libusb_get_max_iso_packet_size(dev, endpoint) : LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_get_max_alt_packet_size(libusb_device *dev, int interface_number, int alternate_setting, unsigned char endpoint)
{
	struct shim_device *d = find_dev(dev);

	if (d)
		return (interface_number == 0 && alternate_setting == 0) ?
			our_max_packet(d, endpoint) : LIBUSB_ERROR_NOT_FOUND;
	return O.libusb_get_max_alt_packet_size ?
		O.libusb_get_max_alt_packet_size(dev, interface_number, alternate_setting, endpoint) : LIBUSB_ERROR_NOT_SUPPORTED;
}

/* BOS-related descriptors only exist on the original library's side. */
int LIBUSB_CALL libusb_get_ss_endpoint_companion_descriptor(libusb_context *ctx,
	const struct libusb_endpoint_descriptor *endpoint, struct libusb_ss_endpoint_companion_descriptor **ep_comp)
{
	int ok;
	libusb_context *oc;

	if (!orig_load() || !O.libusb_get_ss_endpoint_companion_descriptor)
		return LIBUSB_ERROR_NOT_FOUND;
	oc = octx_of(ctx, &ok);
	return O.libusb_get_ss_endpoint_companion_descriptor(oc, endpoint, ep_comp);
}

void LIBUSB_CALL libusb_free_ss_endpoint_companion_descriptor(struct libusb_ss_endpoint_companion_descriptor *ep_comp)
{
	if (ep_comp && O.libusb_free_ss_endpoint_companion_descriptor)
		O.libusb_free_ss_endpoint_companion_descriptor(ep_comp);
}

int LIBUSB_CALL libusb_get_bos_descriptor(libusb_device_handle *dev_handle, struct libusb_bos_descriptor **bos)
{
	if (find_handle(dev_handle))
		return LIBUSB_ERROR_PIPE;   /* USB 1.1 device: no BOS */
	return O.libusb_get_bos_descriptor ? O.libusb_get_bos_descriptor(dev_handle, bos) : LIBUSB_ERROR_NOT_SUPPORTED;
}

void LIBUSB_CALL libusb_free_bos_descriptor(struct libusb_bos_descriptor *bos)
{
	if (bos && O.libusb_free_bos_descriptor)
		O.libusb_free_bos_descriptor(bos);
}

#define BOS_CAP_FWD(getname, freename, type) \
int LIBUSB_CALL getname(libusb_context *ctx, struct libusb_bos_dev_capability_descriptor *dev_cap, type **out) \
{ \
	int ok; \
	libusb_context *oc; \
	if (!orig_load() || !O.getname) \
		return LIBUSB_ERROR_NOT_SUPPORTED; \
	oc = octx_of(ctx, &ok); \
	return O.getname(oc, dev_cap, out); \
} \
void LIBUSB_CALL freename(type *p) \
{ \
	if (p && O.freename) \
		O.freename(p); \
}

BOS_CAP_FWD(libusb_get_usb_2_0_extension_descriptor, libusb_free_usb_2_0_extension_descriptor,
	struct libusb_usb_2_0_extension_descriptor)
BOS_CAP_FWD(libusb_get_ss_usb_device_capability_descriptor, libusb_free_ss_usb_device_capability_descriptor,
	struct libusb_ss_usb_device_capability_descriptor)
BOS_CAP_FWD(libusb_get_container_id_descriptor, libusb_free_container_id_descriptor,
	struct libusb_container_id_descriptor)
BOS_CAP_FWD(libusb_get_platform_descriptor, libusb_free_platform_descriptor,
	struct libusb_platform_descriptor)
BOS_CAP_FWD(libusb_get_ssplus_usb_device_capability_descriptor, libusb_free_ssplus_usb_device_capability_descriptor,
	struct libusb_ssplus_usb_device_capability_descriptor)

/* Read string descriptor idx of a CH341 without an open libusb handle. */
static int our_device_string(struct shim_device *d, uint8_t idx, char *data, int length)
{
	uint8_t tbuf[255], *io;
	HANDLE dh;
	DWORD err = 0;
	int r, n = 0, si, di;

	if (!idx)
		return LIBUSB_ERROR_NOT_FOUND;
	io = malloc(CH341_CMD_HEAD + CH341_MAX_XFER + 64);
	if (!io)
		return LIBUSB_ERROR_NO_MEM;
	dh = ch341_io->open(d->path, &err);
	if (dh == INVALID_HANDLE_VALUE) {
		free(io);
		return ch341_err_to_libusb(err, 0);
	}
	r = ch341_ctrl_raw(dh, io, 0x80, LIBUSB_REQUEST_GET_DESCRIPTOR, LIBUSB_DT_STRING << 8, 0, tbuf, sizeof(tbuf), &n);
	if (!r && n >= 4)
		r = ch341_ctrl_raw(dh, io, 0x80, LIBUSB_REQUEST_GET_DESCRIPTOR, (uint16_t)((LIBUSB_DT_STRING << 8) | idx),
			(uint16_t)(tbuf[2] | (tbuf[3] << 8)), tbuf, sizeof(tbuf), &n);
	else if (!r)
		r = LIBUSB_ERROR_IO;
	ch341_io->close(dh);
	free(io);
	if (r)
		return r <= -1000 ? LIBUSB_ERROR_IO : r;
	if (n < 2 || tbuf[1] != LIBUSB_DT_STRING || tbuf[0] > n)
		return LIBUSB_ERROR_IO;
	for (di = 0, si = 2; si + 1 < tbuf[0] && di < length - 1; si += 2)
		data[di++] = (char)((tbuf[si + 1] || (tbuf[si] & 0x80)) ? '?' : tbuf[si]);
	data[di] = 0;
	return di;
}

int LIBUSB_CALL libusb_get_device_string(libusb_device *dev, enum libusb_device_string_type string_type,
	char *data, int length)
{
	struct shim_device *d = find_dev(dev);

	if (d) {
		if ((int)string_type < 0 || string_type >= LIBUSB_DEVICE_STRING_COUNT || !data || length <= 0)
			return LIBUSB_ERROR_INVALID_PARAM;
		return our_device_string(d, d->dev_desc[14 + string_type], data, length);
	}
	return O.libusb_get_device_string ? O.libusb_get_device_string(dev, string_type, data, length)
		: LIBUSB_ERROR_NOT_SUPPORTED;
}

unsigned long LIBUSB_CALL libusb_get_session_data(libusb_device *dev)
{
	struct shim_device *d = find_dev(dev);

	if (d) {
		/* stable per physical device: FNV-1a of the instance ID */
		unsigned long hsh = 2166136261ul;
		const char *p;
		for (p = d->instance_id; *p; p++)
			hsh = (hsh ^ (unsigned char)*p) * 16777619ul;
		return hsh;
	}
	return O.libusb_get_session_data ? O.libusb_get_session_data(dev) : 0;
}

/* ---------------------------------------------------------------------- */
/* Device handles                                                          */
/* ---------------------------------------------------------------------- */

int LIBUSB_CALL libusb_wrap_sys_device(libusb_context *ctx, intptr_t sys_dev, libusb_device_handle **dev_handle)
{
	int ok;
	libusb_context *oc;

	if (!orig_load() || !O.libusb_wrap_sys_device)
		return LIBUSB_ERROR_NOT_SUPPORTED;
	oc = octx_of(ctx, &ok);
	return O.libusb_wrap_sys_device(oc, sys_dev, dev_handle);
}

int LIBUSB_CALL libusb_open(libusb_device *dev, libusb_device_handle **dev_handle)
{
	struct shim_device *d = find_dev(dev);

	if (d) {
		struct shim_handle *h;
		int r = handle_open(d, &h);
		if (!r)
			*dev_handle = (libusb_device_handle *)h;
		return r;
	}
	return O.libusb_open ? O.libusb_open(dev, dev_handle) : LIBUSB_ERROR_NOT_SUPPORTED;
}

libusb_device_handle * LIBUSB_CALL libusb_open_device_with_vid_pid(libusb_context *ctx, uint16_t vendor_id, uint16_t product_id)
{
	struct shim_device *ours[MAX_FOUND];
	struct shim_handle *h = NULL;
	struct shim_ctx *c;
	libusb_context *oc;
	int n, i;

	EnterCriticalSection(&g_lock);
	c = ctx_get(ctx);
	LeaveCriticalSection(&g_lock);

	n = enum_ch341(c, ours);
	for (i = 0; i < n; i++) {
		const uint8_t *b = ours[i]->dev_desc;
		if (!h && (b[8] | (b[9] << 8)) == vendor_id && (b[10] | (b[11] << 8)) == product_id)
			handle_open(ours[i], &h);
	}
	for (i = 0; i < n; i++)
		dev_unref(ours[i]);
	if (h)
		return (libusb_device_handle *)h;

	if (orig_ctx(c, &oc) && O.libusb_open_device_with_vid_pid)
		return O.libusb_open_device_with_vid_pid(oc, vendor_id, product_id);
	return NULL;
}

void LIBUSB_CALL libusb_close(libusb_device_handle *dev_handle)
{
	struct shim_handle *h, **pp;

	if (!dev_handle)
		return;
	EnterCriticalSection(&g_lock);
	h = our_handle(dev_handle);
	if (h) {
		for (pp = &g_handles; *pp; pp = &(*pp)->next)
			if (*pp == h) {
				*pp = h->next;
				break;
			}
		engine_drop_queue(h);
	}
	LeaveCriticalSection(&g_lock);
	if (!h) {
		if (O.libusb_close)
			O.libusb_close(dev_handle);
		return;
	}
	ch341_io->close(h->h);
	fifo_free(&h->fifo);
	dev_unref(h->dev);
	h->magic = 0;
	free(h->io);
	free(h);
}

libusb_device * LIBUSB_CALL libusb_get_device(libusb_device_handle *dev_handle)
{
	struct shim_handle *h = find_handle(dev_handle);

	if (h)
		return (libusb_device *)h->dev;
	return O.libusb_get_device ? O.libusb_get_device(dev_handle) : NULL;
}

int LIBUSB_CALL libusb_get_configuration(libusb_device_handle *dev, int *config)
{
	if (find_handle(dev)) {
		*config = 1;
		return 0;
	}
	return O.libusb_get_configuration ? O.libusb_get_configuration(dev, config) : LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_set_configuration(libusb_device_handle *dev_handle, int configuration)
{
	if (find_handle(dev_handle))
		return configuration == 1 ? 0 : LIBUSB_ERROR_NOT_SUPPORTED;
	return O.libusb_set_configuration ? O.libusb_set_configuration(dev_handle, configuration) : LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *dev_handle, int interface_number)
{
	struct shim_handle *h = lock_handle(dev_handle);

	if (h) {
		int r = LIBUSB_ERROR_NOT_FOUND;
		if (interface_number == 0) {
			h->claimed |= 1;
			r = 0;
		}
		LeaveCriticalSection(&g_lock);
		return r;
	}
	return O.libusb_claim_interface ? O.libusb_claim_interface(dev_handle, interface_number) : LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_release_interface(libusb_device_handle *dev_handle, int interface_number)
{
	struct shim_handle *h = lock_handle(dev_handle);

	if (h) {
		int r = LIBUSB_ERROR_NOT_FOUND;
		if (interface_number == 0) {
			h->claimed &= ~1u;
			r = 0;
		}
		LeaveCriticalSection(&g_lock);
		return r;
	}
	return O.libusb_release_interface ? O.libusb_release_interface(dev_handle, interface_number) : LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_set_interface_alt_setting(libusb_device_handle *dev_handle, int interface_number, int alternate_setting)
{
	if (find_handle(dev_handle))
		return (interface_number == 0 && alternate_setting == 0) ? 0 : LIBUSB_ERROR_NOT_FOUND;
	return O.libusb_set_interface_alt_setting ?
		O.libusb_set_interface_alt_setting(dev_handle, interface_number, alternate_setting) : LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_clear_halt(libusb_device_handle *dev_handle, unsigned char endpoint)
{
	struct shim_handle *h = lock_handle(dev_handle);

	if (h) {
		int r = engine_exec_ctrl(h, LIBUSB_RECIPIENT_ENDPOINT, LIBUSB_REQUEST_CLEAR_FEATURE, 0,
			endpoint, NULL, 0, 1000);
		LeaveCriticalSection(&g_lock);
		return r < 0 ? r : 0;
	}
	return O.libusb_clear_halt ? O.libusb_clear_halt(dev_handle, endpoint) : LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_reset_device(libusb_device_handle *dev_handle)
{
	struct shim_handle *h = lock_handle(dev_handle);

	if (h) {
		int r = ch341_reset(h);
		fifo_clear(&h->fifo);
		LeaveCriticalSection(&g_lock);
		return r;
	}
	return O.libusb_reset_device ? O.libusb_reset_device(dev_handle) : LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_alloc_streams(libusb_device_handle *dev_handle, uint32_t num_streams, unsigned char *endpoints, int num_endpoints)
{
	if (find_handle(dev_handle))
		return LIBUSB_ERROR_NOT_SUPPORTED;
	return O.libusb_alloc_streams ?
		O.libusb_alloc_streams(dev_handle, num_streams, endpoints, num_endpoints) : LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_free_streams(libusb_device_handle *dev_handle, unsigned char *endpoints, int num_endpoints)
{
	if (find_handle(dev_handle))
		return LIBUSB_ERROR_NOT_SUPPORTED;
	return O.libusb_free_streams ? O.libusb_free_streams(dev_handle, endpoints, num_endpoints) : LIBUSB_ERROR_NOT_SUPPORTED;
}

unsigned char * LIBUSB_CALL libusb_dev_mem_alloc(libusb_device_handle *dev_handle, size_t length)
{
	if (find_handle(dev_handle))
		return NULL;
	return O.libusb_dev_mem_alloc ? O.libusb_dev_mem_alloc(dev_handle, length) : NULL;
}

int LIBUSB_CALL libusb_dev_mem_free(libusb_device_handle *dev_handle, unsigned char *buffer, size_t length)
{
	if (find_handle(dev_handle))
		return LIBUSB_ERROR_NOT_SUPPORTED;
	return O.libusb_dev_mem_free ? O.libusb_dev_mem_free(dev_handle, buffer, length) : LIBUSB_ERROR_NOT_SUPPORTED;
}

/* Kernel driver detaching does not exist on Windows; report it like libusb does there. */
int LIBUSB_CALL libusb_kernel_driver_active(libusb_device_handle *dev_handle, int interface_number)
{
	if (find_handle(dev_handle))
		return LIBUSB_ERROR_NOT_SUPPORTED;
	return O.libusb_kernel_driver_active ? O.libusb_kernel_driver_active(dev_handle, interface_number) : LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_detach_kernel_driver(libusb_device_handle *dev_handle, int interface_number)
{
	if (find_handle(dev_handle))
		return LIBUSB_ERROR_NOT_SUPPORTED;
	return O.libusb_detach_kernel_driver ? O.libusb_detach_kernel_driver(dev_handle, interface_number) : LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_attach_kernel_driver(libusb_device_handle *dev_handle, int interface_number)
{
	if (find_handle(dev_handle))
		return LIBUSB_ERROR_NOT_SUPPORTED;
	return O.libusb_attach_kernel_driver ? O.libusb_attach_kernel_driver(dev_handle, interface_number) : LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_set_auto_detach_kernel_driver(libusb_device_handle *dev_handle, int enable)
{
	if (find_handle(dev_handle))
		return LIBUSB_ERROR_NOT_SUPPORTED;
	return O.libusb_set_auto_detach_kernel_driver ?
		O.libusb_set_auto_detach_kernel_driver(dev_handle, enable) : LIBUSB_ERROR_NOT_SUPPORTED;
}

/* Raw I/O is a WinUSB pipe policy; the WCH driver has nothing comparable. */
int LIBUSB_CALL libusb_endpoint_supports_raw_io(libusb_device_handle *dev_handle, uint8_t endpoint)
{
	if (find_handle(dev_handle))
		return 0;
	return O.libusb_endpoint_supports_raw_io ?
		O.libusb_endpoint_supports_raw_io(dev_handle, endpoint) : LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_endpoint_set_raw_io(libusb_device_handle *dev_handle, uint8_t endpoint, int enable)
{
	if (find_handle(dev_handle))
		return LIBUSB_ERROR_NOT_SUPPORTED;
	return O.libusb_endpoint_set_raw_io ?
		O.libusb_endpoint_set_raw_io(dev_handle, endpoint, enable) : LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_get_max_raw_io_transfer_size(libusb_device_handle *dev_handle, uint8_t endpoint)
{
	if (find_handle(dev_handle))
		return LIBUSB_ERROR_NOT_SUPPORTED;
	return O.libusb_get_max_raw_io_transfer_size ?
		O.libusb_get_max_raw_io_transfer_size(dev_handle, endpoint) : LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_get_string_descriptor_ascii(libusb_device_handle *dev_handle, uint8_t desc_index,
	unsigned char *data, int length)
{
	struct shim_handle *h;
	uint8_t tbuf[255];
	uint16_t langid;
	int r, si, di;

	if (!find_handle(dev_handle))
		return O.libusb_get_string_descriptor_ascii ?
			O.libusb_get_string_descriptor_ascii(dev_handle, desc_index, data, length) : LIBUSB_ERROR_NOT_SUPPORTED;
	if (desc_index == 0 || length <= 0)
		return LIBUSB_ERROR_INVALID_PARAM;

	h = lock_handle(dev_handle);
	if (!h)
		return LIBUSB_ERROR_NO_DEVICE;
	r = engine_exec_ctrl(h, 0x80, LIBUSB_REQUEST_GET_DESCRIPTOR, LIBUSB_DT_STRING << 8, 0, tbuf, sizeof(tbuf), 1000);
	if (r >= 4) {
		langid = (uint16_t)(tbuf[2] | (tbuf[3] << 8));
		r = engine_exec_ctrl(h, 0x80, LIBUSB_REQUEST_GET_DESCRIPTOR, (uint16_t)((LIBUSB_DT_STRING << 8) | desc_index),
			langid, tbuf, sizeof(tbuf), 1000);
	} else if (r >= 0) {
		r = LIBUSB_ERROR_IO;
	}
	LeaveCriticalSection(&g_lock);
	if (r < 0)
		return r;
	if (r < 2 || tbuf[1] != LIBUSB_DT_STRING || tbuf[0] > r)
		return LIBUSB_ERROR_IO;
	for (di = 0, si = 2; si + 1 < tbuf[0] && di < length - 1; si += 2)
		data[di++] = (tbuf[si + 1] || (tbuf[si] & 0x80)) ? '?' : tbuf[si];
	data[di] = 0;
	return di;
}

/* ---------------------------------------------------------------------- */
/* Transfers                                                               */
/* ---------------------------------------------------------------------- */

struct libusb_transfer * LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
	struct shim_transfer *st;
	size_t sz;

	if (iso_packets < 0)
		return NULL;
	sz = offsetof(struct shim_transfer, pub) + sizeof(struct libusb_transfer)
		+ (size_t)iso_packets * sizeof(struct libusb_iso_packet_descriptor);
	st = calloc(1, sz);
	if (!st)
		return NULL;
	st->magic = MAGIC_XFER;
	st->num_iso = iso_packets;
	st->pub.num_iso_packets = iso_packets;
	return &st->pub;
}

void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *transfer)
{
	struct shim_transfer *st;

	if (!transfer)
		return;
	st = ST(transfer);
	if (st->magic != MAGIC_XFER)
		return;
	if (st->state == T_QUEUED) {
		/* Freeing a pending transfer is a caller bug; at least don't leave it queued. */
		struct shim_handle *h;
		EnterCriticalSection(&g_lock);
		for (h = g_handles; h; h = h->next) {
			struct shim_transfer **pp, *prev = NULL;
			for (pp = &h->q_head; *pp; prev = *pp, pp = &(*pp)->qnext)
				if (*pp == st) {
					*pp = st->qnext;
					if (h->q_tail == st)
						h->q_tail = prev;
					break;
				}
		}
		LeaveCriticalSection(&g_lock);
	}
	if ((transfer->flags & LIBUSB_TRANSFER_FREE_BUFFER) && transfer->buffer)
		free(transfer->buffer);
	if (st->shadow && O.libusb_free_transfer)
		O.libusb_free_transfer(st->shadow);
	st->magic = 0;
	free(st);
}

static void LIBUSB_CALL pt_callback(struct libusb_transfer *s)
{
	struct shim_transfer *st = s->user_data;
	struct libusb_transfer *t = &st->pub;
	uint8_t flags = t->flags;
	int i;

	t->status = s->status;
	t->actual_length = s->actual_length;
	for (i = 0; i < t->num_iso_packets && i < s->num_iso_packets; i++) {
		t->iso_packet_desc[i].actual_length = s->iso_packet_desc[i].actual_length;
		t->iso_packet_desc[i].status = s->iso_packet_desc[i].status;
	}
	st->state = T_IDLE;
	InterlockedDecrement(&g_orig_inflight);
	if (t->callback)
		t->callback(t);
	if (flags & LIBUSB_TRANSFER_FREE_TRANSFER)
		libusb_free_transfer(t);
}

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *transfer)
{
	struct shim_transfer *st = ST(transfer);
	struct libusb_transfer *s;
	struct shim_handle *h;
	int r, i;

	if (st->magic != MAGIC_XFER)
		return LIBUSB_ERROR_INVALID_PARAM;
	EnterCriticalSection(&g_lock);
	if (st->state != T_IDLE) {
		LeaveCriticalSection(&g_lock);
		return LIBUSB_ERROR_BUSY;
	}
	h = our_handle(transfer->dev_handle);
	if (h) {
		r = engine_submit(st);
		LeaveCriticalSection(&g_lock);
		return r;
	}
	LeaveCriticalSection(&g_lock);

	/* Passthrough: mirror the transfer into the original libusb. */
	if (!O.libusb_submit_transfer || !O.libusb_alloc_transfer)
		return LIBUSB_ERROR_NO_DEVICE;
	if (transfer->num_iso_packets > st->num_iso)
		return LIBUSB_ERROR_INVALID_PARAM;
	if (!st->shadow) {
		st->shadow = O.libusb_alloc_transfer(st->num_iso);
		if (!st->shadow)
			return LIBUSB_ERROR_NO_MEM;
	}
	s = st->shadow;
	s->dev_handle = transfer->dev_handle;
	s->flags = transfer->flags & (LIBUSB_TRANSFER_SHORT_NOT_OK | LIBUSB_TRANSFER_ADD_ZERO_PACKET);
	s->endpoint = transfer->endpoint;
	s->type = transfer->type;
	s->timeout = transfer->timeout;
	s->length = transfer->length;
	s->buffer = transfer->buffer;
	s->num_iso_packets = transfer->num_iso_packets;
	for (i = 0; i < transfer->num_iso_packets; i++)
		s->iso_packet_desc[i].length = transfer->iso_packet_desc[i].length;
	s->callback = pt_callback;
	s->user_data = st;
	if (transfer->type == LIBUSB_TRANSFER_TYPE_BULK_STREAM && O.libusb_transfer_set_stream_id)
		O.libusb_transfer_set_stream_id(s, st->stream_id);
	st->state = T_ORIG;
	InterlockedIncrement(&g_orig_inflight);
	r = O.libusb_submit_transfer(s);
	if (r) {
		st->state = T_IDLE;
		InterlockedDecrement(&g_orig_inflight);
	}
	return r;
}

int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer *transfer)
{
	struct shim_transfer *st = ST(transfer);
	int r = LIBUSB_ERROR_NOT_FOUND;

	if (st->magic != MAGIC_XFER)
		return LIBUSB_ERROR_INVALID_PARAM;
	EnterCriticalSection(&g_lock);
	if (st->state == T_QUEUED)
		r = engine_cancel(st);
	LeaveCriticalSection(&g_lock);
	if (st->state == T_ORIG && st->shadow && O.libusb_cancel_transfer)
		r = O.libusb_cancel_transfer(st->shadow);
	return r;
}

void LIBUSB_CALL libusb_transfer_set_stream_id(struct libusb_transfer *transfer, uint32_t stream_id)
{
	ST(transfer)->stream_id = stream_id;
}

uint32_t LIBUSB_CALL libusb_transfer_get_stream_id(struct libusb_transfer *transfer)
{
	return ST(transfer)->stream_id;
}

int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *dev_handle, uint8_t request_type, uint8_t bRequest,
	uint16_t wValue, uint16_t wIndex, unsigned char *data, uint16_t wLength, unsigned int timeout)
{
	struct shim_handle *h = lock_handle(dev_handle);

	if (h) {
		int r = engine_exec_ctrl(h, request_type, bRequest, wValue, wIndex, data, wLength, timeout);
		LeaveCriticalSection(&g_lock);
		return r;
	}
	return O.libusb_control_transfer ?
		O.libusb_control_transfer(dev_handle, request_type, bRequest, wValue, wIndex, data, wLength, timeout) :
		LIBUSB_ERROR_NO_DEVICE;
}

int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle *dev_handle, unsigned char endpoint, unsigned char *data,
	int length, int *actual_length, unsigned int timeout)
{
	struct shim_handle *h = lock_handle(dev_handle);

	if (h) {
		int r, a = 0;
		if (endpoint == CH341_EP_OUT)
			r = engine_exec_out(h, data, length, timeout, &a);
		else if (endpoint == CH341_EP_IN)
			r = engine_exec_in(h, data, length, timeout, &a);
		else
			r = LIBUSB_ERROR_NOT_FOUND;
		LeaveCriticalSection(&g_lock);
		if (actual_length)
			*actual_length = a;
		return r;
	}
	return O.libusb_bulk_transfer ?
		O.libusb_bulk_transfer(dev_handle, endpoint, data, length, actual_length, timeout) : LIBUSB_ERROR_NO_DEVICE;
}

int LIBUSB_CALL libusb_interrupt_transfer(libusb_device_handle *dev_handle, unsigned char endpoint, unsigned char *data,
	int length, int *actual_length, unsigned int timeout)
{
	if (find_handle(dev_handle))
		return LIBUSB_ERROR_NOT_SUPPORTED;
	return O.libusb_interrupt_transfer ?
		O.libusb_interrupt_transfer(dev_handle, endpoint, data, length, actual_length, timeout) : LIBUSB_ERROR_NO_DEVICE;
}

/* ---------------------------------------------------------------------- */
/* Event handling                                                          */
/* ---------------------------------------------------------------------- */

static DWORD tv_ms(const struct timeval *tv)
{
	unsigned long long ms;

	if (!tv)
		return 60000;
	if (tv->tv_sec < 0 || tv->tv_usec < 0)
		return 0;
	ms = (unsigned long long)tv->tv_sec * 1000ull + ((unsigned long long)tv->tv_usec + 999ull) / 1000ull;
	return ms > 0x7fffffffull ? 0x7fffffff : (DWORD)ms;
}

static int pending(struct shim_ctx *c)
{
	int p;

	EnterCriticalSection(&g_lock);
	p = engine_has_pending(c);
	LeaveCriticalSection(&g_lock);
	return p;
}

static int handle_events(libusb_context *ctx, struct timeval *tv, int *completed, int locked)
{
	struct shim_ctx *c;
	libusb_context *oc;
	struct timeval zero = { 0, 0 };
	DWORD budget = tv_ms(tv);
	int n = 0, r = 0, have_o, ours;

	if (completed && *completed)
		return 0;
	EnterCriticalSection(&g_lock);
	c = ctx_get(ctx);
	LeaveCriticalSection(&g_lock);
	have_o = orig_ctx(c, &oc) && (locked ? O.libusb_handle_events_locked != NULL
		: O.libusb_handle_events_timeout_completed != NULL);

	ours = pending(c);
	if (ours)
		n = engine_run(c, budget, completed);

	if (have_o && (g_orig_inflight > 0 || !ours)) {
		struct timeval *otv = tv;
		if (n > 0 || pending(c))
			otv = &zero;    /* we did (or still have) work: just poll the original */
		if (!(completed && *completed)) {
			if (locked)
				r = O.libusb_handle_events_locked(oc, otv);
			else
				r = O.libusb_handle_events_timeout_completed(oc, otv, completed);
		}
	} else if (!ours && n == 0 && !(completed && *completed)) {
		/* Nothing to do: behave like libusb and wait (wakes on submit). */
		WaitForSingleObject(g_wake, budget);
	}
	return r;
}

int LIBUSB_CALL libusb_handle_events_timeout_completed(libusb_context *ctx, struct timeval *tv, int *completed)
{
	return handle_events(ctx, tv, completed, 0);
}

int LIBUSB_CALL libusb_handle_events_timeout(libusb_context *ctx, struct timeval *tv)
{
	return handle_events(ctx, tv, NULL, 0);
}

int LIBUSB_CALL libusb_handle_events(libusb_context *ctx)
{
	struct timeval tv = { 60, 0 };
	return handle_events(ctx, &tv, NULL, 0);
}

int LIBUSB_CALL libusb_handle_events_completed(libusb_context *ctx, int *completed)
{
	struct timeval tv = { 60, 0 };
	return handle_events(ctx, &tv, completed, 0);
}

int LIBUSB_CALL libusb_handle_events_locked(libusb_context *ctx, struct timeval *tv)
{
	return handle_events(ctx, tv, NULL, 1);
}

int LIBUSB_CALL libusb_get_next_timeout(libusb_context *ctx, struct timeval *tv)
{
	struct shim_ctx *c;
	libusb_context *oc;

	EnterCriticalSection(&g_lock);
	c = ctx_get(ctx);
	LeaveCriticalSection(&g_lock);
	if (pending(c)) {
		tv->tv_sec = 0;
		tv->tv_usec = 0;
		return 1;
	}
	if (orig_ctx(c, &oc) && O.libusb_get_next_timeout)
		return O.libusb_get_next_timeout(oc, tv);
	return 0;
}

int LIBUSB_CALL libusb_pollfds_handle_timeouts(libusb_context *ctx)
{
	int ok;
	libusb_context *oc = octx_of(ctx, &ok);

	return (ok && O.libusb_pollfds_handle_timeouts) ? O.libusb_pollfds_handle_timeouts(oc) : 1;
}

const struct libusb_pollfd ** LIBUSB_CALL libusb_get_pollfds(libusb_context *ctx)
{
	int ok;
	libusb_context *oc = octx_of(ctx, &ok);

	return (ok && O.libusb_get_pollfds) ? O.libusb_get_pollfds(oc) : NULL;
}

void LIBUSB_CALL libusb_free_pollfds(const struct libusb_pollfd **pollfds)
{
	if (!pollfds)
		return;
	if (O.libusb_free_pollfds)
		O.libusb_free_pollfds(pollfds);
}

void LIBUSB_CALL libusb_set_pollfd_notifiers(libusb_context *ctx, libusb_pollfd_added_cb added_cb,
	libusb_pollfd_removed_cb removed_cb, void *user_data)
{
	int ok;
	libusb_context *oc = octx_of(ctx, &ok);

	if (ok && O.libusb_set_pollfd_notifiers)
		O.libusb_set_pollfd_notifiers(oc, added_cb, removed_cb, user_data);
}

#define CTX_FWD_VOID(name) \
void LIBUSB_CALL name(libusb_context *ctx) \
{ \
	int ok; \
	libusb_context *oc = octx_of(ctx, &ok); \
	if (ok && O.name) \
		O.name(oc); \
}

#define CTX_FWD_INT(name, def) \
int LIBUSB_CALL name(libusb_context *ctx) \
{ \
	int ok; \
	libusb_context *oc = octx_of(ctx, &ok); \
	return (ok && O.name) ? O.name(oc) : (def); \
}

CTX_FWD_INT(libusb_try_lock_events, 0)
CTX_FWD_VOID(libusb_lock_events)
CTX_FWD_VOID(libusb_unlock_events)
CTX_FWD_INT(libusb_event_handling_ok, 1)
CTX_FWD_INT(libusb_event_handler_active, 0)
CTX_FWD_VOID(libusb_lock_event_waiters)
CTX_FWD_VOID(libusb_unlock_event_waiters)

void LIBUSB_CALL libusb_interrupt_event_handler(libusb_context *ctx)
{
	int ok;
	libusb_context *oc = octx_of(ctx, &ok);

	SetEvent(g_wake);
	if (ok && O.libusb_interrupt_event_handler)
		O.libusb_interrupt_event_handler(oc);
}

int LIBUSB_CALL libusb_wait_for_event(libusb_context *ctx, struct timeval *tv)
{
	int ok;
	libusb_context *oc = octx_of(ctx, &ok);

	if (ok && O.libusb_wait_for_event)
		return O.libusb_wait_for_event(oc, tv);
	return WaitForSingleObject(g_wake, tv_ms(tv)) == WAIT_TIMEOUT ? 1 : 0;
}

int LIBUSB_CALL libusb_hotplug_register_callback(libusb_context *ctx, int events, int flags, int vendor_id,
	int product_id, int dev_class, libusb_hotplug_callback_fn cb_fn, void *user_data,
	libusb_hotplug_callback_handle *callback_handle)
{
	int ok;
	libusb_context *oc;

	if (!orig_load() || !O.libusb_hotplug_register_callback)
		return LIBUSB_ERROR_NOT_SUPPORTED;
	oc = octx_of(ctx, &ok);
	return O.libusb_hotplug_register_callback(oc, events, flags, vendor_id, product_id, dev_class,
		cb_fn, user_data, callback_handle);
}

void LIBUSB_CALL libusb_hotplug_deregister_callback(libusb_context *ctx, libusb_hotplug_callback_handle callback_handle)
{
	int ok;
	libusb_context *oc = octx_of(ctx, &ok);

	if (ok && O.libusb_hotplug_deregister_callback)
		O.libusb_hotplug_deregister_callback(oc, callback_handle);
}

void * LIBUSB_CALL libusb_hotplug_get_user_data(libusb_context *ctx, libusb_hotplug_callback_handle callback_handle)
{
	int ok;
	libusb_context *oc = octx_of(ctx, &ok);

	return (ok && O.libusb_hotplug_get_user_data) ? O.libusb_hotplug_get_user_data(oc, callback_handle) : NULL;
}
