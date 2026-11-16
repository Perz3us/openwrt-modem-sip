/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 perz3us */
#ifndef SIP_H
#define SIP_H

#include <stddef.h>

struct sip_cb {
	void (*answered)(const char *name);
	void (*dial)(const char *name, const char *number);
	void (*hangup)(void);
	void (*digit)(char d);
	int (*busy)(void);
	int (*hd)(void);
	void (*sms)(unsigned id, const char *name, const char *number, const char *text, size_t n);
	void (*sms_delivered)(const char *key);
};

int sip_start(const char *bind_ip, int port, int rtp_from, int rtp_to, const struct sip_cb *cb);
void sip_ring_all(const char *number);
void sip_cancel_all(void);
void sip_connected(void);
void sip_bye(void);
void sip_stop(void);
void sip_sms_result(unsigned id, int code);
void sip_sms_deliver(const char *key, const char *number, const char *text, size_t n);

#endif
