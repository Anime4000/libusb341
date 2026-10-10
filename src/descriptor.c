/*
 * libusb341 - configuration descriptor parsing for CH341 devices, and
 * bookkeeping of descriptor objects handed out by the shim (as opposed to the
 * original libusb).
 */
#include "shim.h"
#include <stdlib.h>
#include <string.h>

struct track {
	void *p;
	struct track *next;
};

static struct track *g_track;

void desc_track(void *p)
{
	struct track *t = malloc(sizeof(*t));

	if (!t)
		return;
	EnterCriticalSection(&g_lock);
	t->p = p;
	t->next = g_track;
	g_track = t;
	LeaveCriticalSection(&g_lock);
}

int desc_untrack(void *p)
{
	struct track **pp, *t;
	int found = 0;

	EnterCriticalSection(&g_lock);
	for (pp = &g_track; (t = *pp) != NULL; pp = &t->next) {
		if (t->p == p) {
			*pp = t->next;
			free(t);
			found = 1;
			break;
		}
	}
	LeaveCriticalSection(&g_lock);
	return found;
}

int desc_is_ours(const void *p)
{
	struct track *t;
	int found = 0;

	EnterCriticalSection(&g_lock);
	for (t = g_track; t; t = t->next)
		if (t->p == p) {
			found = 1;
			break;
		}
	LeaveCriticalSection(&g_lock);
	return found;
}

static const unsigned char *dup_extra(const uint8_t *p, int n, int *out_len)
{
	unsigned char *e;

	*out_len = 0;
	if (n <= 0)
		return NULL;
	e = malloc((size_t)n);
	if (!e)
		return NULL;
	memcpy(e, p, (size_t)n);
	*out_len = n;
	return e;
}

/* Skip descriptors until one of type t1 or t2 (or the end); returns new position. */
static int skip_until(const uint8_t *buf, int pos, int end, int t1, int t2)
{
	while (pos + 2 <= end) {
		if (buf[pos] < 2 || pos + buf[pos] > end)
			return end;
		if (buf[pos + 1] == t1 || buf[pos + 1] == t2)
			break;
		pos += buf[pos];
	}
	return pos;
}

void desc_free_config(struct libusb_config_descriptor *c)
{
	int i, a, e;

	if (!c)
		return;
	for (i = 0; c->interface && i < c->bNumInterfaces; i++) {
		struct libusb_interface *itf = (struct libusb_interface *)&c->interface[i];
		for (a = 0; a < itf->num_altsetting; a++) {
			struct libusb_interface_descriptor *alt =
				(struct libusb_interface_descriptor *)&itf->altsetting[a];
			for (e = 0; alt->endpoint && e < alt->bNumEndpoints; e++)
				free((void *)alt->endpoint[e].extra);
			free((void *)alt->endpoint);
			free((void *)alt->extra);
		}
		free((void *)itf->altsetting);
	}
	free((void *)c->interface);
	free((void *)c->extra);
	free(c);
}

int desc_parse_config(const uint8_t *buf, int len, struct libusb_config_descriptor **out)
{
	struct libusb_config_descriptor *c;
	struct libusb_interface *itfs;
	int total, pos, n_if, cur = -1, next;

	*out = NULL;
	if (len < LIBUSB_DT_CONFIG_SIZE || buf[1] != LIBUSB_DT_CONFIG || buf[0] < LIBUSB_DT_CONFIG_SIZE)
		return LIBUSB_ERROR_IO;
	total = buf[2] | (buf[3] << 8);
	if (total > len)
		total = len;

	c = calloc(1, sizeof(*c));
	if (!c)
		return LIBUSB_ERROR_NO_MEM;
	c->bLength = buf[0];
	c->bDescriptorType = buf[1];
	c->wTotalLength = (uint16_t)total;
	c->bNumInterfaces = buf[4];
	c->bConfigurationValue = buf[5];
	c->iConfiguration = buf[6];
	c->bmAttributes = buf[7];
	c->MaxPower = buf[8];
	n_if = buf[4];

	itfs = calloc(n_if ? (size_t)n_if : 1, sizeof(*itfs));
	if (!itfs) {
		free(c);
		return LIBUSB_ERROR_NO_MEM;
	}
	c->interface = itfs;

	pos = buf[0];
	next = skip_until(buf, pos, total, LIBUSB_DT_INTERFACE, -1);
	c->extra = dup_extra(buf + pos, next - pos, &c->extra_length);
	pos = next;

	while (pos + LIBUSB_DT_INTERFACE_SIZE <= total && buf[pos + 1] == LIBUSB_DT_INTERFACE) {
		const uint8_t *d = buf + pos;
		struct libusb_interface_descriptor *alts, *alt;
		struct libusb_endpoint_descriptor *eps;
		int e, n_ep;

		if (d[0] < LIBUSB_DT_INTERFACE_SIZE)
			break;
		if (cur < 0 || itfs[cur].altsetting[0].bInterfaceNumber != d[2]) {
			if (cur + 1 >= n_if)
				break;
			cur++;
		}
		alts = realloc((void *)itfs[cur].altsetting,
			(size_t)(itfs[cur].num_altsetting + 1) * sizeof(*alts));
		if (!alts)
			goto nomem;
		itfs[cur].altsetting = alts;
		alt = &alts[itfs[cur].num_altsetting++];
		memset(alt, 0, sizeof(*alt));
		alt->bLength = d[0];
		alt->bDescriptorType = d[1];
		alt->bInterfaceNumber = d[2];
		alt->bAlternateSetting = d[3];
		alt->bInterfaceClass = d[5];
		alt->bInterfaceSubClass = d[6];
		alt->bInterfaceProtocol = d[7];
		alt->iInterface = d[8];
		n_ep = d[4];
		pos += d[0];

		next = skip_until(buf, pos, total, LIBUSB_DT_ENDPOINT, LIBUSB_DT_INTERFACE);
		alt->extra = dup_extra(buf + pos, next - pos, &alt->extra_length);
		pos = next;

		eps = calloc(n_ep ? (size_t)n_ep : 1, sizeof(*eps));
		if (!eps)
			goto nomem;
		alt->endpoint = eps;
		for (e = 0; e < n_ep; e++) {
			const uint8_t *ed = buf + pos;

			if (pos + LIBUSB_DT_ENDPOINT_SIZE > total || ed[1] != LIBUSB_DT_ENDPOINT
					|| ed[0] < LIBUSB_DT_ENDPOINT_SIZE)
				break;
			eps[e].bLength = ed[0];
			eps[e].bDescriptorType = ed[1];
			eps[e].bEndpointAddress = ed[2];
			eps[e].bmAttributes = ed[3];
			eps[e].wMaxPacketSize = (uint16_t)(ed[4] | (ed[5] << 8));
			eps[e].bInterval = ed[6];
			if (ed[0] >= LIBUSB_DT_ENDPOINT_AUDIO_SIZE) {
				eps[e].bRefresh = ed[7];
				eps[e].bSynchAddress = ed[8];
			}
			pos += ed[0];
			next = skip_until(buf, pos, total, LIBUSB_DT_ENDPOINT, LIBUSB_DT_INTERFACE);
			eps[e].extra = dup_extra(buf + pos, next - pos, &eps[e].extra_length);
			pos = next;
		}
		alt->bNumEndpoints = (uint8_t)e;
	}
	c->bNumInterfaces = (uint8_t)(cur + 1);
	*out = c;
	return 0;

nomem:
	c->bNumInterfaces = (uint8_t)(cur + 1);
	desc_free_config(c);
	return LIBUSB_ERROR_NO_MEM;
}
