// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 perz3us
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "modem.h"

#define QUECTEL_VID	"2c7c"
#define AT_IFACE	2
#define AT_DAEMON	"/etc/init.d/ubus-at-daemon"
#define QMODEM_NET	"/etc/init.d/qmodem_network"
#ifndef MMCLI
#define MMCLI		"/usr/bin/mmcli"
#endif

#ifndef AT_LOCK_DIR
#define AT_LOCK_DIR	"/var/lock"
#endif

static char at_port[64] = "auto";
static char cur[64];

static int read_line(const char *path, char *buf, size_t n)
{
	FILE *f = fopen(path, "r");
	int ok;

	if (!f)
		return -1;
	ok = fgets(buf, n, f) != NULL;
	fclose(f);
	if (!ok)
		return -1;
	buf[strcspn(buf, "\r\n")] = 0;
	return 0;
}

static int write_file(const char *path, const char *val)
{
	FILE *f = fopen(path, "w");

	if (!f)
		return -1;
	fputs(val, f);
	return fclose(f);
}

static int find_tty(int iface, char *tty, size_t tn, char *dev, size_t dn)
{
	char path[PATH_MAX], real[PATH_MAX], val[16];
	struct dirent *e;
	DIR *d;
	int found = 0;

	if (!(d = opendir("/sys/class/tty")))
		return -1;
	while (!found && (e = readdir(d))) {
		if (strncmp(e->d_name, "ttyUSB", 6))
			continue;
		snprintf(path, sizeof(path), "/sys/class/tty/%s/device/..", e->d_name);
		if (!realpath(path, real))
			continue;
		if (iface >= 0) {
			snprintf(path, sizeof(path), "%s/bInterfaceNumber", real);
			if (read_line(path, val, sizeof(val)) || strtol(val, NULL, 16) != iface)
				continue;
		}
		snprintf(path, sizeof(path), "%s/..", real);
		if (!realpath(path, real))
			continue;
		snprintf(path, sizeof(path), "%s/idVendor", real);
		if (read_line(path, val, sizeof(val)) || strcmp(val, QUECTEL_VID))
			continue;
		if (tty)
			snprintf(tty, tn, "/dev/%s", e->d_name);
		if (dev)
			snprintf(dev, dn, "%s", real);
		found = 1;
	}
	closedir(d);
	return found ? 0 : -1;
}

static const char *port(void)
{
	if (strcmp(at_port, "auto"))
		return at_port;
	if ((!*cur || access(cur, F_OK)) && find_tty(AT_IFACE, cur, sizeof(cur), NULL, 0))
		cur[0] = 0;
	return cur;
}

void modem_set_port(const char *p)
{
	snprintf(at_port, sizeof(at_port), "%s", *p ? p : "auto");
	cur[0] = 0;
}

const char *modem_port(void)
{
	return port();
}

int modem_port_ok(void)
{
	const char *p = port();

	return *p && !access(p, F_OK);
}

static int replug(void)
{
	char dev[PATH_MAX], auth[PATH_MAX + 16];

	if (find_tty(-1, NULL, 0, dev, sizeof(dev)))
		return -1;
	snprintf(auth, sizeof(auth), "%s/authorized", dev);
	if (write_file(auth, "0"))
		return -1;
	sleep(2);
	return write_file(auth, "1");
}

static long ms_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

static int at_lock(const char *tty, long until)
{
	char path[128];
	const char *b = strrchr(tty, '/');
	int fd;

	snprintf(path, sizeof(path), AT_LOCK_DIR "/5gmodem-at-%s.lock", b ? b + 1 : tty);
	if ((fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644)) < 0)
		return -1;
	while (flock(fd, LOCK_EX | LOCK_NB)) {
		if (ms_now() >= until) {
			close(fd);
			return -1;
		}
		usleep(20000);
	}
	return fd;
}

static int tty_open(const char *tty)
{
	struct termios t;
	int fd;

	if ((fd = open(tty, O_RDWR | O_NOCTTY | O_NONBLOCK)) < 0)
		return -1;
	if (!tcgetattr(fd, &t)) {
		cfmakeraw(&t);
		cfsetspeed(&t, B115200);
		tcsetattr(fd, TCSANOW, &t);
	}
	tcflush(fd, TCIOFLUSH);
	return fd;
}

/* Writes cmd and its terminator, then reads until ERROR, until (or OK) or ms pass. */
static int at_xfer(int fd, const char *cmd, const char *term, char *resp, size_t n, int ms, const char *until)
{
	struct pollfd pf = { .fd = fd, .events = POLLIN };
	size_t got = 0, len = strlen(cmd), tl = strlen(term);
	long end = ms_now() + ms, left;
	ssize_t k;
	int done = 0;

	resp[0] = 0;
	if (write(fd, cmd, len) != (ssize_t)len || write(fd, term, tl) != (ssize_t)tl)
		return -1;
	while (!done && got + 1 < n && (left = end - ms_now()) > 0 && poll(&pf, 1, left) > 0) {
		if ((k = read(fd, resp + got, n - 1 - got)) <= 0)
			break;
		got += k;
		resp[got] = 0;
		done = strstr(resp, "ERROR") || (until ? strstr(resp, until) : strstr(resp, "OK\r\n"));
	}
	return done ? 0 : -1;
}

static int at_direct_until(const char *tty, const char *cmd, char *resp, size_t n, int ms, const char *until)
{
	int fd, lk, rc;

	if (n < 2)
		return -1;
	lk = at_lock(tty, ms_now() + ms / 2);
	if ((fd = tty_open(tty)) < 0) {
		if (lk >= 0)
			close(lk);
		return -1;
	}
	rc = at_xfer(fd, cmd, "\r", resp, n, ms, until);
	close(fd);
	if (lk >= 0)
		close(lk);
	return rc;
}

/* PDU mode, AT+CMGS, wait for the prompt, then the PDU and Ctrl-Z. Keeps the port locked the
 * whole time so other AT users can't land in the middle, and puts text mode back if it was on. */
static int sms_direct(const char *tty, const char *pdu, int len, char *resp, size_t n)
{
	char cmd[32], r[128];
	int fd, lk, text, rc = -1;

	if (n < 2 || (lk = at_lock(tty, ms_now() + 5000)) < 0)
		return -1;
	if ((fd = tty_open(tty)) < 0) {
		close(lk);
		return -1;
	}
	text = !at_xfer(fd, "AT+CMGF?", "\r", r, sizeof(r), 2000, NULL) && strstr(r, "+CMGF: 1");
	snprintf(cmd, sizeof(cmd), "AT+CMGS=%d", len);
	if (!at_xfer(fd, "AT+CMGF=0", "\r", resp, n, 2000, NULL) && !strstr(resp, "ERROR") &&
	    !at_xfer(fd, cmd, "\r", resp, n, 5000, ">") && !strstr(resp, "ERROR")) {
		/* The network can take a while to accept it. A SIP app gives up after 32 s. */
		rc = at_xfer(fd, pdu, "\x1a", resp, n, 25000, NULL);
		if (!rc && !strstr(resp, "+CMGS:"))
			rc = -1;
	}
	if (text)
		at_xfer(fd, "AT+CMGF=1", "\r", r, sizeof(r), 2000, NULL);
	close(fd);
	close(lk);
	return rc;
}

static int at_direct(const char *tty, const char *cmd, char *resp, size_t n, int ms)
{
	return at_direct_until(tty, cmd, resp, n, ms, NULL);
}

static int at_ready(const char *tty)
{
	char buf[128];

	return !at_direct(tty, "AT", buf, sizeof(buf), 1000) && strstr(buf, "OK");
}

static int wait_back(int secs)
{
	char tty[64];
	int i;

	for (i = 0; i < secs; i++) {
		if (!find_tty(AT_IFACE, tty, sizeof(tty), NULL, 0) && at_ready(tty))
			break;
		sleep(1);
	}
	cur[0] = 0;
	return i < secs ? 0 : -1;
}

void modem_heal(void)
{
	if (!access(AT_DAEMON, X_OK))
		system(AT_DAEMON " restart >/dev/null 2>&1");
	cur[0] = 0;
}

int modem_restart(void)
{
	char before[64], after[64], r[256];
	int atd = !access(AT_DAEMON, X_OK), i;

	if (find_tty(AT_IFACE, before, sizeof(before), NULL, 0))
		before[0] = 0;
	modem_at("AT+CFUN=1,1", r, sizeof(r));
	if (atd)
		system(AT_DAEMON " stop >/dev/null 2>&1");
	for (i = 0; i < 30 && !find_tty(-1, NULL, 0, NULL, 0); i++)
		sleep(1);
	i = wait_back(180);
	if (!i && *before && (find_tty(AT_IFACE, after, sizeof(after), NULL, 0) || strcmp(before, after))) {
		if (!replug())
			i = wait_back(60);
	}
	if (atd)
		system(AT_DAEMON " start >/dev/null 2>&1");
	modem_redial();
	return i || !modem_port_ok() ? -1 : 0;
}

void modem_redial(void)
{
	if (access(QMODEM_NET, X_OK))
		return;
	system("for s in $(uci -q show qmodem | sed -n 's/^qmodem\\.\\([^.=]*\\)=modem-device$/\\1/p'); do "
	       QMODEM_NET " redial \"$s\"; done >/dev/null 2>&1");
}

int modem_data_up(void)
{
	char line[256], dev[32];
	unsigned long dst;
	FILE *f = fopen("/proc/net/route", "r");
	int up = 0;

	if (!f)
		return 0;
	while (!up && fgets(line, sizeof(line), f))
		up = sscanf(line, "%31s %lx", dev, &dst) == 2 && dst == 0 && strcmp(dev, "lo");
	fclose(f);
	return up;
}

static int mm_no_at;

static int mm_reply(const char *out, char *resp, size_t n)
{
	const char *a = strstr(out, "response: '"), *b;

	if (a) {
		a += 11;
		b = strrchr(a, '\'');
		snprintf(resp, n, "%.*s\r\nOK\r\n", (int)(b && b > a ? b - a : 0), a);
		return 0;
	}
	if (strstr(out, "Unauthorized") || strstr(out, "debug mode")) {
		mm_no_at = 1;
		return -1;
	}
	if (!strstr(out, "ERROR"))
		return -1;
	snprintf(resp, n, "%s\r\nERROR\r\n", out);
	return 0;
}

static int mm_owns(void)
{
	static long checked = -100000;
	static int owns;
	char buf[256] = "";
	FILE *p;

	if (mm_no_at)
		return 0;
	if (ms_now() - checked < 10000)
		return owns;
	checked = ms_now();
	owns = 0;
	if (access(MMCLI, X_OK) || !(p = popen(MMCLI " -L 2>/dev/null", "r")))
		return 0;
	owns = fread(buf, 1, sizeof(buf) - 1, p) > 0 && strstr(buf, "/Modem/");
	pclose(p);
	return owns;
}

static int at_mm(const char *cmd, char *resp, size_t n, int secs)
{
	char line[256], out[1024];
	size_t got;
	FILE *p;

	if (strchr(cmd, '\''))
		return -1;
	snprintf(line, sizeof(line), MMCLI " -m any --timeout=%d --command='%s' 2>&1", secs, cmd);
	if (!(p = popen(line, "r")))
		return -1;
	got = fread(out, 1, sizeof(out) - 1, p);
	out[got] = 0;
	pclose(p);
	return mm_reply(out, resp, n);
}

int modem_direct(void)
{
	return access(AT_DAEMON, X_OK) != 0;
}

int modem_at(const char *cmd, char *resp, size_t n)
{
	return modem_at_wait(cmd, resp, n, 3, NULL);
}

int modem_at_wait(const char *cmd, char *resp, size_t n, int secs, const char *until)
{
	const char *ap = port();
	char line[320], esc[128];
	size_t got, k = 0;
	FILE *p;

	if (*ap && modem_direct()) {
		if (mm_owns() && !at_mm(cmd, resp, n, secs))
			return 0;
		return at_direct_until(ap, cmd, resp, n, secs * 1000, until);
	}
	if (!*ap || strpbrk(cmd, "'\\") || strpbrk(ap, "'\"\\"))
		return -1;
	for (; *cmd && k + 3 < sizeof(esc); cmd++) {
		if (*cmd == '"')
			esc[k++] = '\\';
		esc[k++] = *cmd;
	}
	esc[k] = 0;
	snprintf(line, sizeof(line),
		 "ubus call at-daemon sendat '{\"at_port\":\"%s\",\"at_cmd\":\"%s\",\"timeout\":%d}' 2>/dev/null",
		 ap, esc, secs);
	p = popen(line, "r");
	if (!p)
		return -1;
	got = fread(resp, 1, n - 1, p);
	resp[got] = 0;
	return pclose(p) == 0 ? 0 : -1;
}

/* Sending needs the > prompt of AT+CMGS, which only the AT port itself gives. Through the
 * QModem at-daemon or ModemManager this returns -2. */
int modem_sms(const char *pdu, int len, char *resp, size_t n)
{
	const char *ap = port();

	if (n)
		resp[0] = 0;
	if (!*ap || !modem_direct() || mm_owns())
		return -2;
	return sms_direct(ap, pdu, len, resp, n);
}
