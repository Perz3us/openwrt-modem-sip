// SPDX-License-Identifier: GPL-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/sms.h"

static struct sms_pdu pdu[SMS_PARTS_MAX];

static int enc(const char *num, const char *text)
{
	return sms_encode(num, text, strlen(text), 7, pdu, SMS_PARTS_MAX);
}

int main(void)
{
	char buf[1300];
	const char *t;
	size_t tn;

	assert(enc("+94771234567", "hi") == 1);
	assert(!strcmp(pdu[0].hex, "0001000B914977214365F7000002E834"));
	assert(pdu[0].len == 15);

	assert(enc("0771234567", "\xe0\xb6\x85") == 1);
	assert(!strcmp(pdu[0].hex, "0001000A8170173254760008020D85"));

	memset(buf, 'a', 161);
	buf[161] = 0;
	assert(enc("+94771234567", buf) == 2);
	assert(!strncmp(pdu[0].hex, "0041", 4) && !strncmp(pdu[0].hex + 26, "A0", 2));
	assert(!strncmp(pdu[1].hex + 26, "0F", 2));

	memset(buf, 'a', 152);
	strcpy(buf + 152, "\xe2\x82\xac" "aaaaaaaaaa");
	assert(enc("+94771234567", buf) == 2);
	assert(!strncmp(pdu[0].hex + 26, "9F", 2));

	memset(buf, 'a', 1250);
	buf[1250] = 0;
	assert(enc("+94771234567", buf) == SMS_TOO_LONG);
	assert(enc("alice", "hi") == SMS_BAD_NUMBER);
	assert(enc("+", "hi") == SMS_BAD_NUMBER);
	assert(enc("0771234567", "") == SMS_BAD_TEXT);
	assert(enc("0771234567", "\xff") == SMS_BAD_TEXT);

	assert(sms_body("text/plain; charset=utf-8", 25, "hello", 5, &t, &tn) == 1 && tn == 5);
	strcpy(buf, "From: <sip:a@b>\r\nTo: <sip:c@d>\r\n\r\nContent-Type: text/plain;charset=utf-8\r\n\r\nhello");
	assert(sms_body("message/cpim", 12, buf, strlen(buf), &t, &tn) == 1);
	assert(tn == 5 && !memcmp(t, "hello", 5));
	assert(sms_body("application/im-iscomposing+xml", 30, "<x/>", 4, &t, &tn) == 0);
	assert(sms_body("image/png", 9, "x", 1, &t, &tn) == -1);

	{
		struct sms_in in;
		const char *p, *pdu;
		size_t len;
		int idx, ids[4];
		const char *h = "07911326040000F0040B911346610089F60000208062917314080CC8F71D14969741F977FD07";

		assert(!sms_decode(h, strlen(h), &in));
		assert(!strcmp(in.number, "+31641600986") && !strcmp(in.text, "How are you?"));
		assert(!strcmp(in.ts, "20806291731408"));
		assert(in.total == 1 && in.seq == 1);

		h = "00040A81701732547600086211310100000002" "0D85";
		assert(!sms_decode(h, strlen(h), &in));
		assert(!strcmp(in.number, "0771234567") && !strcmp(in.text, "\xe0\xb6\x85"));

		h = "00440A8170173254760000621131010000000905000307020" "1D069";
		assert(!sms_decode(h, strlen(h), &in));
		assert(!strcmp(in.text, "hi") && in.ref == 7 && in.total == 2 && in.seq == 1);

		h = "00040CD0C47498FD3E03000062113101000000" "02E834";
		assert(!sms_decode(h, strlen(h), &in));
		assert(!strcmp(in.number, "Dialog") && !strcmp(in.text, "hi"));

		assert(sms_decode("0001000B91", 10, &in) == -1);
		assert(sms_decode("zz", 2, &in) == -1);

		p = "AT+CMGL=4\r\n+CMGL: 3,1,,24\r\n0791ABCD\r\n+CMGL: 12,0,,30\r\n00AA\r\n\r\nOK\r\n";
		assert(sms_cmgl_next(&p, &idx, &pdu, &len) && idx == 3 && len == 8 && !strncmp(pdu, "0791ABCD", 8));
		assert(sms_cmgl_next(&p, &idx, &pdu, &len) && idx == 12 && len == 4);
		assert(!sms_cmgl_next(&p, &idx, &pdu, &len));

		assert(sms_mm_list("    /org/freedesktop/ModemManager1/SMS/4 (received)\n"
				   "    /org/freedesktop/ModemManager1/SMS/5 (sent)\n"
				   "    /org/freedesktop/ModemManager1/SMS/9 (received)\n", ids, 4) == 2);
		assert(ids[0] == 4 && ids[1] == 9);

		assert(!sms_mm_read("sms.dbus-path                : /org/freedesktop/ModemManager1/SMS/4\n"
				    "sms.content.number           : +94771234567\n"
				    "sms.content.text             : line one\nline two\n"
				    "sms.content.data             : --\n"
				    "sms.properties.timestamp     : 2026-11-13T10:00:00+05:30\n", &in));
		assert(!strcmp(in.number, "+94771234567") && !strcmp(in.text, "line one\nline two"));
		assert(!strcmp(in.ts, "2026-11-13T10:00:00+05:30"));
	}

	puts("test_sms ok");
	return 0;
}
