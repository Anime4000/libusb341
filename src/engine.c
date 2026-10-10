/*
 * libusb341 - libusb transfer semantics on top of the WCH CH341 driver.
 *
 * libusb lets an application queue one big bulk OUT transfer plus many bulk
 * IN transfers and lets the host controller interleave them.  The CH341 only
 * buffers the reply to roughly one packet, so a plain driver write of a long
 * command stream would stall.  The WCH driver instead offers "write, then
 * read N blocks of M bytes" (CH341WriteRead), which it interleaves itself.
 *
 * Every 32-byte OUT packet holds exactly one command stream (SPI 0xA8,
 * I2C 0xAA or UIO 0xAB).  From that we can tell how many bytes the chip will
 * answer, batch the packets into WriteRead calls, and keep the answers in a
 * FIFO from which libusb IN transfers are completed - one unit per USB IN
 * transfer, exactly like the short packets the device sends.
 */
#include "shim.h"
#include <stdlib.h>
#include <string.h>

#define MIN_POLL_MS     10
#define MAX_GROUP_WRITE (CH341_MAX_XFER - CH341_PACKET)  /* leave room for the 0x20 pad packet */

HANDLE g_wake;

/* ---------------------------------------------------------------------- */
/* Response prediction                                                     */
/* ---------------------------------------------------------------------- */

unsigned ch341_predict_response(const uint8_t *p, unsigned n)
{
	unsigned i, r = 0;

	if (!n)
		return 0;
	switch (p[0]) {
	case 0xA8:  /* SPI stream: one byte in for every byte shifted out */
		return n - 1;
	case 0xAA:  /* I2C stream */
		for (i = 1; i < n;) {
			uint8_t c = p[i++];
			if (c == 0x00)          /* END */
				break;
			switch (c & 0xC0) {
			case 0xC0:              /* IN: n bytes; 0 = one byte, then NAK */
				r += (c & 0x3F) ? (c & 0x3F) : 1u;
				break;
			case 0x80:              /* OUT: n bytes follow; 0 = one byte, returns ACK status */
				if (c & 0x3F) {
					i += c & 0x3F;
				} else {
					i += 1;
					r += 1;
				}
				break;
			default:                /* STA, STO, SET, US, MS */
				break;
			}
		}
		return r;
	case 0xAB:  /* UIO stream */
		for (i = 1; i < n; i++) {
			uint8_t c = p[i];
			if ((c & 0xE0) == 0x20) /* END */
				break;
			if ((c & 0xE0) == 0x00) /* IN: D7-D0 */
				r++;
		}
		return r;
	default:
		return 0;
	}
}

/* ---------------------------------------------------------------------- */
/* Response FIFO                                                           */
/* ---------------------------------------------------------------------- */

int fifo_empty(const struct resp_fifo *f)
{
	return f->uhead == f->utail;
}

void fifo_clear(struct resp_fifo *f)
{
	f->head = f->tail = 0;
	f->uhead = f->utail = 0;
}

void fifo_free(struct resp_fifo *f)
{
	free(f->data);
	free(f->units);
	memset(f, 0, sizeof(*f));
}

static int fifo_push(struct resp_fifo *f, const uint8_t *p, uint32_t n)
{
	if (f->tail + n > f->cap) {
		if (f->head) {
			memmove(f->data, f->data + f->head, f->tail - f->head);
			f->tail -= f->head;
			f->head = 0;
		}
		if (f->tail + n > f->cap) {
			size_t cap = f->cap ? f->cap * 2 : 16384;
			uint8_t *d;
			while (cap < f->tail + n)
				cap *= 2;
			d = realloc(f->data, cap);
			if (!d)
				return LIBUSB_ERROR_NO_MEM;
			f->data = d;
			f->cap = cap;
		}
	}
	if (f->utail == f->ucap) {
		if (f->uhead) {
			memmove(f->units, f->units + f->uhead, (f->utail - f->uhead) * sizeof(*f->units));
			f->utail -= f->uhead;
			f->uhead = 0;
		}
		if (f->utail == f->ucap) {
			size_t ucap = f->ucap ? f->ucap * 2 : 512;
			uint32_t *u = realloc(f->units, ucap * sizeof(*u));
			if (!u)
				return LIBUSB_ERROR_NO_MEM;
			f->units = u;
			f->ucap = ucap;
		}
	}
	memcpy(f->data + f->tail, p, n);
	f->tail += n;
	f->units[f->utail++] = n;
	return 0;
}

/* Pop one unit into dst; returns the unit's full size (may exceed max). */
static uint32_t fifo_pop(struct resp_fifo *f, uint8_t *dst, uint32_t max, uint32_t *copied)
{
	uint32_t n = f->units[f->uhead++];
	uint32_t c = n < max ? n : max;

	if (c)
		memcpy(dst, f->data + f->head, c);
	f->head += n;
	if (f->uhead == f->utail)
		fifo_clear(f);
	*copied = c;
	return n;
}

/* ---------------------------------------------------------------------- */
/* Synchronous operations                                                  */
/* ---------------------------------------------------------------------- */

int engine_exec_out(struct shim_handle *h, const uint8_t *data, int len, unsigned timeout, int *actual)
{
	uint8_t rbuf[CH341_MAX_XFER];
	int npk = (len + (int)CH341_PACKET - 1) / (int)CH341_PACKET;
	int i = 0, r;

	*actual = 0;
	if (h->gone)
		return LIBUSB_ERROR_NO_DEVICE;
	if (len <= 0)
		return 0;
	r = ch341_set_timeouts(h, timeout);
	if (r)
		return r;

	while (i < npk) {
		int j, k, count = 0, last_short = 0;
		unsigned R = 0, rtotal = 0, wbytes = 0;

		/* Group packets: the answering ones must all answer R bytes, except
		 * a last one that may answer fewer (ending in a short packet). */
		for (j = i; j < npk; j++) {
			unsigned off = (unsigned)j * CH341_PACKET;
			unsigned plen = (unsigned)len - off < CH341_PACKET ? (unsigned)len - off : CH341_PACKET;
			unsigned rr = ch341_predict_response(data + off, plen);

			if (wbytes + plen > MAX_GROUP_WRITE)
				break;
			if (rr) {
				if (last_short)
					break;
				if (count == 0) {
					R = rr;
				} else if (rr != R) {
					if (rr < R && (rr % CH341_PACKET) != 0)
						last_short = 1;
					else
						break;
				}
				if ((unsigned)(count + 1) * R > CH341_MAX_XFER || count + 1 > g_batch)
					break;
				count++;
				rtotal += rr;
			}
			wbytes += plen;
		}

		if (count == 0) {
			int w;
			r = ch341_write_raw(h, data + (size_t)i * CH341_PACKET, (int)wbytes, &w);
			*actual += w;
			if (r)
				goto fail;
		} else {
			int got = 0, pad = (h->ic_ver == 0x20 && (wbytes % CH341_PACKET) == 0);
			unsigned off = 0;

			r = ch341_write_read(h, data + (size_t)i * CH341_PACKET, (int)wbytes, pad,
				R, (unsigned)count, rbuf, &got);
			if (r)
				goto fail;
			*actual += (int)wbytes;
			if ((unsigned)got != rtotal)
				LOGW("CH341 answered %d bytes, expected %u", got, rtotal);
			for (k = i; k < j && off < (unsigned)got; k++) {
				unsigned po = (unsigned)k * CH341_PACKET;
				unsigned plen = (unsigned)len - po < CH341_PACKET ? (unsigned)len - po : CH341_PACKET;
				unsigned rr = ch341_predict_response(data + po, plen), take;

				if (!rr)
					continue;
				take = rr < (unsigned)got - off ? rr : (unsigned)got - off;
				r = fifo_push(&h->fifo, rbuf + off, take);
				if (r)
					goto fail;
				off += take;
			}
		}
		i = j;
	}
	return 0;

fail:
	/* The chip's state is unknown now; don't hand out stale answers. */
	fifo_clear(&h->fifo);
	return r;
}

int engine_exec_in(struct shim_handle *h, uint8_t *data, int len, unsigned timeout, int *actual)
{
	int r, ovf = 0;

	*actual = 0;
	if (!fifo_empty(&h->fifo)) {
		uint32_t c, n = fifo_pop(&h->fifo, data, (uint32_t)(len > 0 ? len : 0), &c);
		*actual = (int)c;
		return n > c ? LIBUSB_ERROR_OVERFLOW : 0;
	}
	if (h->gone)
		return LIBUSB_ERROR_NO_DEVICE;
	r = ch341_set_timeouts(h, timeout);
	if (r)
		return r;
	r = ch341_read_raw(h, data, len, actual, &ovf);
	if (!r && ovf)
		r = LIBUSB_ERROR_OVERFLOW;
	return r;
}

/* Returns bytes transferred (data stage) or a libusb error. */
int engine_exec_ctrl(struct shim_handle *h, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx,
	uint8_t *data, uint16_t len, unsigned timeout)
{
	int r, actual = 0;

	if (h->gone)
		return LIBUSB_ERROR_NO_DEVICE;
	/* Requests that would disturb the vendor driver's view of the device. */
	if ((rt & 0x60) == LIBUSB_REQUEST_TYPE_STANDARD) {
		if (rt == 0x00 && req == LIBUSB_REQUEST_SET_CONFIGURATION)
			return val == 1 ? 0 : LIBUSB_ERROR_NOT_SUPPORTED;
		if (rt == 0x01 && req == LIBUSB_REQUEST_SET_INTERFACE)
			return (val == 0 && idx == 0) ? 0 : LIBUSB_ERROR_NOT_SUPPORTED;
		if (rt == 0x80 && req == LIBUSB_REQUEST_GET_CONFIGURATION) {
			if (len >= 1 && data) {
				data[0] = 1;
				return 1;
			}
			return 0;
		}
		if (rt == 0x00 && req == LIBUSB_REQUEST_SET_ADDRESS)
			return LIBUSB_ERROR_NOT_SUPPORTED;
	}
	r = ch341_set_timeouts(h, timeout);
	if (r)
		return r;
	r = ch341_ctrl(h, rt, req, val, idx, data, len, &actual);
	return r ? r : actual;
}

/* ---------------------------------------------------------------------- */
/* Asynchronous transfers                                                  */
/* ---------------------------------------------------------------------- */

static int is_in_xfer(const struct shim_transfer *st)
{
	return st->pub.type != LIBUSB_TRANSFER_TYPE_CONTROL && (st->pub.endpoint & LIBUSB_ENDPOINT_IN);
}

int engine_submit(struct shim_transfer *st)
{
	struct shim_handle *h = (struct shim_handle *)st->pub.dev_handle;
	struct libusb_transfer *t = &st->pub;

	if (h->gone)
		return LIBUSB_ERROR_NO_DEVICE;
	switch (t->type) {
	case LIBUSB_TRANSFER_TYPE_CONTROL:
		if (t->length < (int)LIBUSB_CONTROL_SETUP_SIZE || !t->buffer)
			return LIBUSB_ERROR_INVALID_PARAM;
		break;
	case LIBUSB_TRANSFER_TYPE_BULK:
		if (t->endpoint != CH341_EP_OUT && t->endpoint != CH341_EP_IN)
			return LIBUSB_ERROR_NOT_FOUND;
		if (t->length < 0 || (t->length && !t->buffer))
			return LIBUSB_ERROR_INVALID_PARAM;
		break;
	default:
		return LIBUSB_ERROR_NOT_SUPPORTED;
	}
	st->state = T_QUEUED;
	st->cancelled = 0;
	st->h = h;
	st->qnext = NULL;
	st->submit_ms = GetTickCount64();
	t->status = LIBUSB_TRANSFER_COMPLETED;
	t->actual_length = 0;
	if (h->q_tail)
		h->q_tail->qnext = st;
	else
		h->q_head = st;
	h->q_tail = st;
	if (g_wake)
		SetEvent(g_wake);
	return 0;
}

int engine_cancel(struct shim_transfer *st)
{
	if (st->state != T_QUEUED || st->cancelled)
		return LIBUSB_ERROR_NOT_FOUND;
	st->cancelled = 1;
	/* An abandoned read means the answers in flight are no longer wanted. */
	if (is_in_xfer(st))
		fifo_clear(&st->h->fifo);
	if (g_wake)
		SetEvent(g_wake);
	return 0;
}

void engine_drop_queue(struct shim_handle *h)
{
	struct shim_transfer *st = h->q_head, *n;

	for (; st; st = n) {
		n = st->qnext;
		st->qnext = NULL;
		st->state = T_IDLE;
		st->h = NULL;
	}
	h->q_head = h->q_tail = NULL;
}

int engine_has_pending(struct shim_ctx *ctx)
{
	struct shim_handle *h;

	for (h = g_handles; h; h = h->next)
		if (h->ctx == ctx && h->q_head)
			return 1;
	return 0;
}

static enum libusb_transfer_status err_to_status(int r)
{
	switch (r) {
	case 0: return LIBUSB_TRANSFER_COMPLETED;
	case LIBUSB_ERROR_TIMEOUT: return LIBUSB_TRANSFER_TIMED_OUT;
	case LIBUSB_ERROR_NO_DEVICE: return LIBUSB_TRANSFER_NO_DEVICE;
	case LIBUSB_ERROR_PIPE: return LIBUSB_TRANSFER_STALL;
	case LIBUSB_ERROR_OVERFLOW: return LIBUSB_TRANSFER_OVERFLOW;
	default: return LIBUSB_TRANSFER_ERROR;
	}
}

static void exec_one(struct shim_handle *h, struct shim_transfer *st)
{
	struct libusb_transfer *t = &st->pub;
	int r, actual = 0;

	if (t->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
		uint8_t *s = t->buffer;
		uint16_t val = (uint16_t)(s[2] | (s[3] << 8));
		uint16_t idx = (uint16_t)(s[4] | (s[5] << 8));
		uint16_t len = (uint16_t)(s[6] | (s[7] << 8));

		if ((int)len > t->length - (int)LIBUSB_CONTROL_SETUP_SIZE)
			len = (uint16_t)(t->length - (int)LIBUSB_CONTROL_SETUP_SIZE);
		r = engine_exec_ctrl(h, s[0], s[1], val, idx, s + LIBUSB_CONTROL_SETUP_SIZE, len, t->timeout);
		if (r >= 0) {
			actual = r;
			r = 0;
		}
	} else if (t->endpoint & LIBUSB_ENDPOINT_IN) {
		r = engine_exec_in(h, t->buffer, t->length, t->timeout, &actual);
	} else {
		r = engine_exec_out(h, t->buffer, t->length, t->timeout, &actual);
	}
	t->actual_length = actual;
	t->status = err_to_status(r);
}

/* Unlink st from h's queue given the pointer that points to it. */
static void unlink_at(struct shim_handle *h, struct shim_transfer **pp, struct shim_transfer *prev)
{
	struct shim_transfer *st = *pp;

	*pp = st->qnext;
	if (h->q_tail == st)
		h->q_tail = prev;
	st->qnext = NULL;
}

int engine_run(struct shim_ctx *ctx, DWORD budget_ms, int *completed)
{
	struct shim_transfer *done = NULL, **dtail = &done, *st, *next;
	struct shim_handle *h;
	int ndone = 0;

	EnterCriticalSection(&g_lock);
	for (h = g_handles; h; h = h->next) {
		struct shim_transfer **pp, *prev;
		int read_timed_out = 0;

		if (h->ctx != ctx || !h->q_head)
			continue;

		/* Pass 1: OUT and control transfers in submission order.  Their
		 * answers land in the FIFO, so IN transfers queued earlier (as
		 * SFP-Master does) still see them. */
		for (pp = &h->q_head, prev = NULL; (st = *pp) != NULL;) {
			if (!st->cancelled && is_in_xfer(st)) {
				prev = st;
				pp = &st->qnext;
				continue;
			}
			unlink_at(h, pp, prev);
			if (st->cancelled)
				st->pub.status = LIBUSB_TRANSFER_CANCELLED;
			else
				exec_one(h, st);
			*dtail = st;
			dtail = &st->qnext;
		}

		/* Pass 2: IN transfers, in order. */
		while ((st = h->q_head) != NULL) {
			struct libusb_transfer *t = &st->pub;
			ULONGLONG now = GetTickCount64(), el = now - st->submit_ms;

			if (!st->cancelled && fifo_empty(&h->fifo)) {
				DWORD wait;
				int r, actual = 0, ovf = 0;

				if (read_timed_out)
					break;
				if (t->timeout && el >= t->timeout) {
					t->status = LIBUSB_TRANSFER_TIMED_OUT;
					goto complete;
				}
				/* Nothing predicted: ask the chip directly, but only for as long
				 * as the caller is willing to wait in this event loop round. */
				wait = budget_ms < MIN_POLL_MS ? MIN_POLL_MS : budget_ms;
				if (t->timeout && wait > t->timeout - el)
					wait = (DWORD)(t->timeout - el);
				r = ch341_set_timeouts(h, wait);
				if (!r)
					r = ch341_read_raw(h, t->buffer, t->length, &actual, &ovf);
				if (r == LIBUSB_ERROR_TIMEOUT) {
					read_timed_out = 1;
					if (t->timeout && GetTickCount64() - st->submit_ms >= t->timeout) {
						t->status = LIBUSB_TRANSFER_TIMED_OUT;
						goto complete;
					}
					break;
				}
				t->actual_length = actual;
				t->status = r ? err_to_status(r) :
					(ovf ? LIBUSB_TRANSFER_OVERFLOW : LIBUSB_TRANSFER_COMPLETED);
				goto complete;
			}
			if (st->cancelled) {
				t->status = LIBUSB_TRANSFER_CANCELLED;
			} else {
				exec_one(h, st);
			}
complete:
			unlink_at(h, &h->q_head, NULL);
			*dtail = st;
			dtail = &st->qnext;
		}
	}
	LeaveCriticalSection(&g_lock);

	for (st = done; st; st = next) {
		struct libusb_transfer *t = &st->pub;
		uint8_t flags = t->flags;

		next = st->qnext;
		st->qnext = NULL;
		st->state = T_IDLE;
		st->h = NULL;
		if (t->status == LIBUSB_TRANSFER_COMPLETED && (flags & LIBUSB_TRANSFER_SHORT_NOT_OK)
				&& t->actual_length < t->length - (t->type == LIBUSB_TRANSFER_TYPE_CONTROL ?
					(int)LIBUSB_CONTROL_SETUP_SIZE : 0))
			t->status = LIBUSB_TRANSFER_ERROR;
		LOGD("transfer %p ep 0x%02x done: status %d, %d/%d bytes", (void *)t, t->endpoint,
			t->status, t->actual_length, t->length);
		ndone++;
		if (t->callback)
			t->callback(t);
		if (flags & LIBUSB_TRANSFER_FREE_TRANSFER)
			libusb_free_transfer(t);
	}
	(void)completed;
	return ndone;
}
