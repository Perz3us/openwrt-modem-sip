// SPDX-License-Identifier: GPL-2.0
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
