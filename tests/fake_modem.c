// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 perz3us
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/modem.h"

static const char *dir(void)
{
	const char *d = getenv("MSIP_FAKE");

	return d ? d : "/tmp/msip-fake";
}

int modem_at_wait(const char *cmd, char *resp, size_t n, int secs, const char *until)
{
	char path[256];
	FILE *f;
	size_t got = 0;

	(void)secs;
	(void)until;
	snprintf(path, sizeof(path), "%s/at.log", dir());
	if (strcmp(cmd, "AT+CLCC") && (f = fopen(path, "a"))) {
		fprintf(f, "%s\n", cmd);
		fclose(f);
	}
	resp[0] = 0;
	if (!strcmp(cmd, "AT+CLCC") || !strcmp(cmd, "AT+CMGL=4")) {
		snprintf(path, sizeof(path), "%s/%s", dir(), cmd[3] == 'C' && cmd[4] == 'L' ? "clcc" : "cmgl");
		if ((f = fopen(path, "r"))) {
			got = fread(resp, 1, n - 8, f);
			fclose(f);
		}
	}
	snprintf(resp + got, n - got, "OK\r\n");
	return 0;
}

int modem_at(const char *cmd, char *resp, size_t n)
{
	return modem_at_wait(cmd, resp, n, 3, NULL);
}

int modem_sms(const char *pdu, int len, char *resp, size_t n)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s/at.log", dir());
	if ((f = fopen(path, "a"))) {
		fprintf(f, "SMS %d %s\n", len, pdu);
		fclose(f);
	}
	snprintf(resp, n, "+CMGS: 1\r\nOK\r\n");
	return 0;
}

int modem_sms_text(const char *number, const char *text, size_t len, char *resp, size_t n)
{
	(void)number;
	(void)text;
	(void)len;
	if (n)
		resp[0] = 0;
	return -2;
}

int modem_mm(void)
{
	return 0;
}

int modem_mm_cmd(const char *args, char *out, size_t n)
{
	(void)args;
	if (n)
		out[0] = 0;
	return -1;
}
