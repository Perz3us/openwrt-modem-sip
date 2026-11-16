// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include "sms.h"

#define TEXT_MAX	(SMS_PARTS_MAX * 160)
#define ESC		0x1b

/* GSM 03.38 default alphabet, by septet value. ESC has no character of its own. */
static const uint16_t gsm7[128] = {
	'@', 0xa3, '$', 0xa5, 0xe8, 0xe9, 0xf9, 0xec, 0xf2, 0xc7, '\n', 0xd8, 0xf8, '\r', 0xc5, 0xe5,
	0x394, '_', 0x3a6, 0x393, 0x39b, 0x3a9, 0x3a0, 0x3a8, 0x3a3, 0x398, 0x39e, 0, 0xc6, 0xe6, 0xdf, 0xc9,
	' ', '!', '"', '#', 0xa4, '%', '&', '\'', '(', ')', '*', '+', ',', '-', '.', '/',
	'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '<', '=', '>', '?',
	0xa1, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
	'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 0xc4, 0xd6, 0xd1, 0xdc, 0xa7,
	0xbf, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
	'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', 0xe4, 0xf6, 0xf1, 0xfc, 0xe0,
};

/* Extension table, sent as ESC followed by the septet. */
static const struct { uint16_t cp; uint8_t s; } gsm7_ext[] = {
	{ 0x0c, 0x0a }, { '^', 0x14 }, { '{', 0x28 }, { '}', 0x29 }, { '\\', 0x2f },
	{ '[', 0x3c }, { '~', 0x3d }, { ']', 0x3e }, { '|', 0x40 }, { 0x20ac, 0x65 },
};

/* Septet for a code point, ESC << 8 | septet for the extension table, or -1. */
static int gsm7_of(uint32_t cp)
{
	size_t i;

	for (i = 0; i < 128; i++)
		if (gsm7[i] == cp && i != ESC)
			return i;
	for (i = 0; i < sizeof(gsm7_ext) / sizeof(gsm7_ext[0]); i++)
		if (gsm7_ext[i].cp == cp)
			return ESC << 8 | gsm7_ext[i].s;
	return -1;
}

/* Strict UTF-8 to code points. Returns the count, -1 for bad UTF-8, -2 when it does not fit. */
static int utf8_decode(const char *s, size_t n, uint32_t *out, int max)
{
	const unsigned char *p = (const unsigned char *)s, *end = p + n;
	int k = 0, len, i;
	uint32_t cp, min;

	while (p < end) {
		if (*p < 0x80) {
			cp = *p;
			len = 1;
			min = 0;
		} else if ((*p & 0xe0) == 0xc0) {
			cp = *p & 0x1f;
			len = 2;
			min = 0x80;
		} else if ((*p & 0xf0) == 0xe0) {
			cp = *p & 0x0f;
			len = 3;
			min = 0x800;
		} else if ((*p & 0xf8) == 0xf0) {
			cp = *p & 0x07;
			len = 4;
			min = 0x10000;
		} else {
			return -1;
		}
		if (end - p < len)
			return -1;
		for (i = 1; i < len; i++) {
			if ((p[i] & 0xc0) != 0x80)
				return -1;
			cp = cp << 6 | (p[i] & 0x3f);
		}
		if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp < 0xe000))
			return -1;
		if (k >= max)
			return -2;
		out[k++] = cp;
		p += len;
	}
	return k;
}

static void hex_put(struct sms_pdu *p, size_t *at, uint8_t v)
{
	static const char h[] = "0123456789ABCDEF";

	p->hex[(*at)++] = h[v >> 4];
	p->hex[(*at)++] = h[v & 15];
	p->hex[*at] = 0;
}

/* Builds one SMS-SUBMIT. ud holds the user data, udl is its length in septets (GSM) or octets. */
static void build(struct sms_pdu *p, const char *digits, int intl, int ucs2, int udhi,
		  const uint8_t *ud, int udn, int udl)
{
	size_t at = 0, nd = strlen(digits), i;
	int k;

	hex_put(p, &at, 0x00);			/* SMSC from the SIM */
	hex_put(p, &at, udhi ? 0x41 : 0x01);	/* SMS-SUBMIT, no validity period */
	hex_put(p, &at, 0x00);			/* message reference, set by the modem */
	hex_put(p, &at, nd);
	hex_put(p, &at, intl ? 0x91 : 0x81);
	for (i = 0; i < nd; i += 2)
		hex_put(p, &at, (i + 1 < nd ? digits[i + 1] - '0' : 0xf) << 4 | (digits[i] - '0'));
	hex_put(p, &at, 0x00);			/* protocol id */
	hex_put(p, &at, ucs2 ? 0x08 : 0x00);	/* data coding */
	hex_put(p, &at, udl);
	for (k = 0; k < udn; k++)
		hex_put(p, &at, ud[k]);
	p->len = at / 2 - 1;
}

static int pack7(uint8_t *ud, int start_bits, const uint8_t *s, int n)
{
	int i, bit, by, sh;

	for (i = 0; i < n; i++) {
		bit = start_bits + 7 * i;
		by = bit / 8;
		sh = bit % 8;
		ud[by] |= s[i] << sh;
		if (sh > 1)
			ud[by + 1] |= s[i] >> (8 - sh);
	}
	return (start_bits + 7 * n + 7) / 8;
}

static int udh(uint8_t *ud, unsigned char ref, int total, int seq)
{
	ud[0] = 5;	/* header length */
	ud[1] = 0;	/* concatenated message, 8-bit reference */
	ud[2] = 3;
	ud[3] = ref;
	ud[4] = total;
	ud[5] = seq;
	return 6;
}

int sms_encode(const char *number, const char *text, size_t n, unsigned char ref,
	       struct sms_pdu *out, int max)
{
	static uint32_t cp[TEXT_MAX];
	static uint8_t sept[2 * TEXT_MAX];
	static uint16_t u16[2 * TEXT_MAX];
	const char *digits = number + (number[0] == '+');
	int ncp, ns = 0, nu = 0, ucs2 = 0, i, v, per, total, part, from, cnt, udn, hl;
	int cut[SMS_PARTS_MAX + 1];
	uint8_t ud[140];

	if (!digits[0] || strlen(digits) > 20 || strspn(digits, "0123456789") != strlen(digits))
		return SMS_BAD_NUMBER;
	if ((ncp = utf8_decode(text, n, cp, TEXT_MAX)) == -2)
		return SMS_TOO_LONG;
	if (ncp <= 0)
		return SMS_BAD_TEXT;

	for (i = 0; i < ncp && !ucs2; i++) {
		if ((v = gsm7_of(cp[i])) < 0)
			ucs2 = 1;
		else if (v > 0x7f) {
			sept[ns++] = ESC;
			sept[ns++] = v & 0x7f;
		} else {
			sept[ns++] = v;
		}
	}
	if (ucs2) {
		for (i = 0; i < ncp; i++) {
			if (cp[i] > 0xffff) {
				u16[nu++] = 0xd800 | ((cp[i] - 0x10000) >> 10);
				u16[nu++] = 0xdc00 | ((cp[i] - 0x10000) & 0x3ff);
			} else {
				u16[nu++] = cp[i];
			}
		}
	}

	/* Split into parts, never between ESC and its septet or inside a surrogate pair. */
	cnt = ucs2 ? nu : ns;
	per = cnt <= (ucs2 ? 70 : 160) ? cnt : (ucs2 ? 67 : 153);
	cut[0] = 0;
	for (total = 0; cut[total] < cnt; total++) {
		if (total >= max || total >= SMS_PARTS_MAX)
			return SMS_TOO_LONG;
		from = cut[total] + per < cnt ? cut[total] + per : cnt;
		if (from < cnt && (ucs2 ? (u16[from - 1] & 0xfc00) == 0xd800 : sept[from - 1] == ESC))
			from--;
		cut[total + 1] = from;
	}

	for (part = 0; part < total; part++) {
		memset(ud, 0, sizeof(ud));
		hl = total > 1 ? udh(ud, ref, total, part + 1) : 0;
		from = cut[part];
		cnt = cut[part + 1] - from;
		if (ucs2) {
			for (i = 0; i < cnt; i++) {
				ud[hl + 2 * i] = u16[from + i] >> 8;
				ud[hl + 2 * i + 1] = u16[from + i] & 0xff;
			}
			udn = hl + 2 * cnt;
			build(&out[part], digits, number[0] == '+', 1, hl > 0, ud, udn, udn);
		} else {
			/* After the header, septets start on the next septet boundary. */
			int start = (hl * 8 + 6) / 7 * 7;

			udn = pack7(ud, start, sept + from, cnt);
			build(&out[part], digits, number[0] == '+', 0, hl > 0, ud, udn, start / 7 + cnt);
		}
	}
	return total;
}

static int ctype_is(const char *ct, size_t n, const char *want)
{
	size_t w = strlen(want);

	while (n && (*ct == ' ' || *ct == '\t')) {
		ct++;
		n--;
	}
	return n >= w && !strncasecmp(ct, want, w) && (n == w || strchr("; \t\r\n", ct[w]));
}

/* Position just after the next empty line, or NULL. */
static const char *after_blank(const char *s, const char *end)
{
	const char *p;

	for (p = s; p < end; p++) {
		if (p + 1 < end && p[0] == '\n' && p[1] == '\n')
			return p + 2;
		if (p + 3 < end && !memcmp(p, "\r\n\r\n", 4))
			return p + 4;
	}
	return NULL;
}

/* Is this a typing or delivery notice rather than a message? */
static int notice(const char *ct, size_t n)
{
	return ctype_is(ct, n, "application/im-iscomposing+xml") || ctype_is(ct, n, "message/imdn+xml");
}

/* Finds the text in a SIP MESSAGE body. Returns 1 with text set, 0 for a typing or delivery
 * notice that needs no SMS, or -1 for a content type that can't be sent as SMS. */
int sms_body(const char *ctype, size_t ctn, const char *body, size_t n, const char **text, size_t *tn)
{
	const char *end = body + n, *mime, *payload, *p, *eol;

	if (ctype_is(ctype, ctn, "text/plain")) {
		*text = body;
		*tn = n;
		return 1;
	}
	if (notice(ctype, ctn))
		return 0;
	if (!ctype_is(ctype, ctn, "message/cpim"))
		return -1;

	/* RFC 3862: message headers, a blank line, MIME headers, a blank line, the content. */
	if (!(mime = after_blank(body, end)) || !(payload = after_blank(mime, end)))
		return -1;
	for (p = mime; p < payload; p = eol + 1) {
		if (!(eol = memchr(p, '\n', payload - p)))
			break;
		if ((size_t)(eol - p) > 13 && !strncasecmp(p, "Content-Type:", 13)) {
			if (ctype_is(p + 13, eol - p - 13, "text/plain")) {
				*text = payload;
				*tn = end - payload;
				return 1;
			}
			return notice(p + 13, eol - p - 13) ? 0 : -1;
		}
	}
	return -1;
}

static size_t utf8_put(char *out, size_t at, size_t max, uint32_t cp)
{
	int k = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4, i;

	if (at + k >= max)
		return at;
	if (k == 1) {
		out[at++] = cp;
	} else {
		out[at++] = (0xf00 >> k) | (cp >> (6 * (k - 1)));
		for (i = k - 2; i >= 0; i--)
			out[at++] = 0x80 | ((cp >> (6 * i)) & 0x3f);
	}
	out[at] = 0;
	return at;
}

static int septet(const uint8_t *b, size_t n, int bit)
{
	size_t by = bit / 8;
	int sh = bit % 8, v;

	if (by >= n)
		return -1;
	v = b[by] >> sh;
	if (sh > 1 && by + 1 < n)
		v |= b[by + 1] << (8 - sh);
	return v & 0x7f;
}

static void gsm7_text(const uint8_t *b, size_t n, int bit, int count, char *out, size_t max)
{
	size_t at = 0, e;
	uint32_t cp;
	int i, v;

	out[0] = 0;
	for (i = 0; i < count; i++) {
		if ((v = septet(b, n, bit + 7 * i)) < 0)
			break;
		cp = gsm7[v];
		if (v == ESC) {
			if (++i >= count || (v = septet(b, n, bit + 7 * i)) < 0)
				break;
			cp = ' ';
			for (e = 0; e < sizeof(gsm7_ext) / sizeof(gsm7_ext[0]); e++)
				if (gsm7_ext[e].s == v)
					cp = gsm7_ext[e].cp;
		}
		at = utf8_put(out, at, max, cp);
	}
}

static int hexv(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 :
	       c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

int sms_decode(const char *hex, size_t hn, struct sms_in *o)
{
	uint8_t b[200];
	const uint8_t *ud;
	size_t n, un, k, t = 0;
	int at, fo, ol, toa, dcs, udl, udhl = 0, alpha = 0, i, skip;
	uint32_t u, l;
	char *d;

	memset(o, 0, sizeof(*o));
	o->total = o->seq = 1;
	if (hn % 2 || hn / 2 > sizeof(b) || hn < 2)
		return -1;
	for (n = 0; n < hn / 2; n++) {
		if (hexv(hex[2 * n]) < 0 || hexv(hex[2 * n + 1]) < 0)
			return -1;
		b[n] = hexv(hex[2 * n]) << 4 | hexv(hex[2 * n + 1]);
	}
	at = 1 + b[0];
	if ((size_t)at + 3 > n)
		return -1;
	fo = b[at++];
	if (fo & 3)
		return -1;
	ol = b[at++];
	toa = b[at++];
	if ((size_t)(at + (ol + 1) / 2 + 10) > n)
		return -1;
	if ((toa & 0x70) == 0x50) {
		gsm7_text(b + at, (ol + 1) / 2, 0, ol * 4 / 7, o->number, sizeof(o->number));
	} else {
		d = o->number;
		if ((toa & 0x70) == 0x10)
			*d++ = '+';
		for (i = 0; i < ol && d < o->number + sizeof(o->number) - 1; i++) {
			int v = (b[at + i / 2] >> (i % 2 ? 4 : 0)) & 0xf;

			if (v > 9)
				break;
			*d++ = '0' + v;
		}
		*d = 0;
	}
	at += (ol + 1) / 2 + 1;
	dcs = b[at++];
	for (i = 0; i < 7; i++)
		sprintf(o->ts + 2 * i, "%02X", b[at + i]);
	at += 7;
	udl = b[at++];
	if ((dcs & 0xc0) == 0 || (dcs & 0xc0) == 0x40)
		alpha = (dcs >> 2) & 3;
	else if ((dcs & 0xf0) == 0xf0)
		alpha = dcs & 4 ? 1 : 0;
	else if ((dcs & 0xf0) == 0xe0)
		alpha = 2;
	ud = b + at;
	un = n - at;
	if (fo & 0x40) {
		if (!un || (size_t)(udhl = ud[0]) + 1 > un)
			return -1;
		for (i = 1; i + 1 <= udhl; i += 2 + ud[i + 1]) {
			if (ud[i] == 0 && ud[i + 1] == 3 && i + 4 <= udhl) {
				o->ref = ud[i + 2];
				o->total = ud[i + 3];
				o->seq = ud[i + 4];
			} else if (ud[i] == 8 && ud[i + 1] == 4 && i + 5 <= udhl) {
				o->ref = ud[i + 2] << 8 | ud[i + 3];
				o->total = ud[i + 4];
				o->seq = ud[i + 5];
			}
		}
	}
	if (alpha == 0) {
		skip = fo & 0x40 ? ((udhl + 1) * 8 + 6) / 7 : 0;
		gsm7_text(ud, un, skip * 7, udl - skip, o->text, sizeof(o->text));
	} else if (alpha == 2) {
		size_t e = (size_t)udl < un ? (size_t)udl : un;

		for (k = fo & 0x40 ? (size_t)udhl + 1 : 0; k + 1 < e; k += 2) {
			u = ud[k] << 8 | ud[k + 1];
			if (u >= 0xd800 && u < 0xdc00 && k + 3 < e) {
				l = ud[k + 2] << 8 | ud[k + 3];
				if (l >= 0xdc00 && l < 0xe000) {
					u = 0x10000 + ((u - 0xd800) << 10) + (l - 0xdc00);
					k += 2;
				}
			}
			t = utf8_put(o->text, t, sizeof(o->text), u);
		}
	} else {
		snprintf(o->text, sizeof(o->text), "[a data message that can't be shown]");
	}
	if (o->total < 1 || o->seq < 1 || o->seq > o->total)
		o->total = o->seq = 1;
	return 0;
}

int sms_cmgl_next(const char **p, int *index, const char **pdu, size_t *len)
{
	const char *q = strstr(*p, "+CMGL:");

	if (!q)
		return 0;
	*index = atoi(q + 6);
	q += strcspn(q, "\r\n");
	q += strspn(q, "\r\n");
	*pdu = q;
	*len = strspn(q, "0123456789ABCDEFabcdef");
	*p = q + *len;
	return 1;
}

int sms_mm_list(const char *out, int *ids, int max)
{
	const char *p = out, *eol;
	int n = 0;

	while (n < max && (p = strstr(p, "/SMS/"))) {
		eol = p + strcspn(p, "\n");
		if (memmem(p, eol - p, "(received)", 10))
			ids[n++] = atoi(p + 5);
		p = eol;
	}
	return n;
}

static void kv(const char *out, const char *key, char *val, size_t n)
{
	const char *p = strstr(out, key), *v, *end, *nl;

	val[0] = 0;
	if (!p || !(v = strchr(p, ':')))
		return;
	for (v++; *v == ' '; v++)
		;
	for (end = v; (nl = strchr(end, '\n')) && nl[1] && strncmp(nl + 1, "sms.", 4); end = nl + 1)
		;
	if (!nl)
		end += strlen(end);
	else
		end = nl;
	while (end > v && (end[-1] == '\r' || end[-1] == ' '))
		end--;
	if (end - v == 2 && !strncmp(v, "--", 2))
		return;
	snprintf(val, n, "%.*s", (int)(end - v), v);
}

int sms_mm_read(const char *out, struct sms_in *in)
{
	memset(in, 0, sizeof(*in));
	in->total = in->seq = 1;
	kv(out, "sms.content.number", in->number, sizeof(in->number));
	kv(out, "sms.content.text", in->text, sizeof(in->text));
	kv(out, "sms.properties.timestamp", in->ts, sizeof(in->ts));
	return in->number[0] ? 0 : -1;
}
