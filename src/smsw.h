/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 perz3us */
#ifndef SMSW_H
#define SMSW_H

#include <stddef.h>

#define SMSW_TEXT	6144

struct smsw_msg {
	char type;
	unsigned id;
	int code;
	char key[96];
	char number[32];
	char name[24];
	size_t n;
	char text[SMSW_TEXT];
};

int smsw_start(void);
int smsw_put(int fd, const struct smsw_msg *m);
int smsw_get(int fd, struct smsw_msg *m);

#endif
