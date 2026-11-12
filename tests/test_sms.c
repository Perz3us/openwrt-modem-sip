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

	puts("test_sms ok");
	return 0;
}
