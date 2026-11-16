/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SMS_H
#define SMS_H

#include <stddef.h>

#define SMS_PARTS_MAX	8

enum sms_err {
	SMS_OK = 0,
	SMS_BAD_NUMBER = -1,
	SMS_BAD_TEXT = -2,
	SMS_TOO_LONG = -3,
	SMS_NO_PORT = -4,
	SMS_FAILED = -5,
	SMS_UNSURE = -6,
};

/* One SMS-SUBMIT for AT+CMGS in PDU mode. hex starts with the empty SMSC field ("00"),
 * len is the length in octets without it. */
struct sms_pdu {
	char hex[2 * 160 + 1];
	int len;
};

int sms_encode(const char *number, const char *text, size_t n, unsigned char ref,
	       struct sms_pdu *out, int max);
int sms_body(const char *ctype, size_t ctn, const char *body, size_t n, const char **text, size_t *tn);

struct sms_in {
	char number[32];
	char ts[32];
	int ref, total, seq;
	char text[720];
};

int sms_decode(const char *hex, size_t n, struct sms_in *out);
int sms_cmgl_next(const char **p, int *index, const char **pdu, size_t *len);
int sms_mm_list(const char *out, int *ids, int max);
int sms_mm_read(const char *out, struct sms_in *in);

#endif
