// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 perz3us
#define _GNU_SOURCE
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include "modem.h"
#include "sms.h"
#include "smsw.h"

#define CHECK_SECS	(getenv("MSIP_SMS_CHECK") ? atoi(getenv("MSIP_SMS_CHECK")) : 15)
#define LIST_MAX	256
#define PARTS_MAX	16
#define SEEN_MAX	LIST_MAX

struct msg {
	char key[96];
	int ids[PARTS_MAX], nids;
};

static struct msg msgs[LIST_MAX];
static int nmsgs, mode;
static char seen[SEEN_MAX][96], sent[LIST_MAX][96];
static int nseen, nsent, have_seen;

int smsw_put(int fd, const struct smsw_msg *m)
{
	size_t len = offsetof(struct smsw_msg, text) + m->n;

	return send(fd, m, len, MSG_NOSIGNAL) == (ssize_t)len ? 0 : -1;
}

int smsw_get(int fd, struct smsw_msg *m)
{
	ssize_t got = recv(fd, m, sizeof(*m) - 1, 0);

	if (got < (ssize_t)offsetof(struct smsw_msg, text) || m->n != got - offsetof(struct smsw_msg, text))
		return -1;
	m->text[m->n] = 0;
	m->key[sizeof(m->key) - 1] = m->number[sizeof(m->number) - 1] = m->name[sizeof(m->name) - 1] = 0;
	return 0;
}

static const char *seen_path(void)
{
	const char *p = getenv("MSIP_SMS_SEEN");

	return p ? p : "/etc/modem_sip.sms_seen";
}

static void load_seen(void)
{
	FILE *f = fopen(seen_path(), "r");

	if (!f)
		return;
	have_seen = 1;
	while (nseen < SEEN_MAX && fgets(seen[nseen], sizeof(seen[0]), f)) {
		seen[nseen][strcspn(seen[nseen], "\n")] = 0;
		if (seen[nseen][0])
			nseen++;
	}
	fclose(f);
}

static void save_seen(void)
{
	char tmp[256];
	FILE *f;
	int i;

	snprintf(tmp, sizeof(tmp), "%s.tmp", seen_path());
	if (!(f = fopen(tmp, "w")))
		return;
	for (i = 0; i < nseen; i++)
		fprintf(f, "%s\n", seen[i]);
	if (!fclose(f))
		rename(tmp, seen_path());
	have_seen = 1;
}

static int find(char list[][96], int n, const char *key)
{
	int i;

	for (i = 0; i < n; i++)
		if (!strcmp(list[i], key))
			return i;
	return -1;
}

static void add_seen(const char *key)
{
	if (find(seen, nseen, key) >= 0)
		return;
	if (nseen == SEEN_MAX)
		memmove(seen[0], seen[1], sizeof(seen[0]) * --nseen);
	snprintf(seen[nseen++], sizeof(seen[0]), "%s", key);
}

static int recv_mode(void)
{
	glob_t g = { 0 };
	int off = !glob("/etc/rc.d/S*qmodem-smsd", 0, NULL, &g);

	globfree(&g);
	if (off)
		return 0;
	if (!access("/usr/libexec/rpcd/qmodem", F_OK) || !access("/usr/libexec/aw1000-voice-sms", F_OK) ||
	    !access("/usr/share/5gmodem/sms_tool_mm", F_OK))
		return 1;
	return 2;
}

static void unescape(char *s)
{
	char *d = s;

	for (; *s; s++) {
		if (s[0] == '\\' && (s[1] == 'r' || s[1] == 'n' || s[1] == '"')) {
			*d++ = s[1] == 'r' ? '\r' : s[1] == 'n' ? '\n' : '"';
			s++;
		} else {
			*d++ = *s;
		}
	}
	*d = 0;
}

static struct {
	int id;
	char key[96];
} known[LIST_MAX];
static int nknown;

static void found(int fd, int first, const char *key, const char *number, const char *text, const int *ids, int nids)
{
	static struct smsw_msg out;
	struct msg *m;

	if (nmsgs == LIST_MAX)
		return;
	m = &msgs[nmsgs++];
	snprintf(m->key, sizeof(m->key), "%s", key);
	memcpy(m->ids, ids, sizeof(int) * nids);
	m->nids = nids;
	if (find(seen, nseen, key) >= 0 || find(sent, nsent, key) >= 0)
		return;
	if (first) {
		add_seen(key);
		return;
	}
	memset(&out, 0, offsetof(struct smsw_msg, text));
	out.type = 'M';
	snprintf(out.key, sizeof(out.key), "%s", key);
	snprintf(out.number, sizeof(out.number), "%s", number);
	out.n = snprintf(out.text, sizeof(out.text), "%s", text);
	if (out.n >= sizeof(out.text))
		out.n = sizeof(out.text) - 1;
	if (!smsw_put(fd, &out) && nsent < LIST_MAX)
		snprintf(sent[nsent++], sizeof(sent[0]), "%s", key);
}

static int fetch_mm(int fd, int first, char *buf, size_t bn)
{
	static int ids[LIST_MAX];
	static struct sms_in in;
	char args[48], key[96];
	int n, i, k, missed = 0;

	if (modem_mm_cmd("-m any --messaging-list-sms", buf, bn))
		return -1;
	n = sms_mm_list(buf, ids, LIST_MAX);
	for (i = 0; i < n; i++) {
		for (k = 0; k < nknown && known[k].id != ids[i]; k++)
			;
		if (k < nknown && (find(seen, nseen, known[k].key) >= 0 || find(sent, nsent, known[k].key) >= 0)) {
			found(fd, first, known[k].key, "", "", &ids[i], 1);
			continue;
		}
		snprintf(args, sizeof(args), "-s %d -K", ids[i]);
		if (modem_mm_cmd(args, buf, bn) || sms_mm_read(buf, &in)) {
			missed = 1;
			continue;
		}
		snprintf(key, sizeof(key), "%s|%s|%zu", in.number, in.ts, strlen(in.text));
		if (k == nknown && nknown < LIST_MAX)
			nknown++;
		if (k < LIST_MAX) {
			known[k].id = ids[i];
			memcpy(known[k].key, key, sizeof(key));
		}
		found(fd, first, key, in.number, in.text, &ids[i], 1);
	}
	return missed;
}

static int fetch_at(int fd, int first, char *buf, size_t bn)
{
	static struct sms_in parts[LIST_MAX];
	static char text[SMSW_TEXT];
	static int idx[LIST_MAX], used[LIST_MAX];
	int np = 0, i, j, s, ids[PARTS_MAX], nids, missed = 0;
	char key[96];
	const char *p, *pdu;
	size_t len;
	int textmode, rc;

	textmode = !modem_at_wait("AT+CMGF?", buf, bn, 3, NULL) && strstr(buf, "+CMGF: 1");
	modem_at_wait("AT+CMGF=0", buf, bn, 3, NULL);
	rc = modem_at_wait("AT+CMGL=4", buf, bn, 10, NULL) || strstr(buf, "ERROR");
	/* other tools on the port may count on text mode */
	if (textmode) {
		char r[64];

		modem_at_wait("AT+CMGF=1", r, sizeof(r), 3, NULL);
	}
	if (rc)
		return -1;
	unescape(buf);
	for (p = buf; np < LIST_MAX && sms_cmgl_next(&p, &idx[np], &pdu, &len);) {
		if (!sms_decode(pdu, len, &parts[np]))
			used[np++] = 0;
		else
			missed = 1;
	}
	for (i = 0; i < np; i++) {
		if (used[i] || parts[i].total > PARTS_MAX) {
			used[i] = 1;
			continue;
		}
		text[0] = 0;
		nids = 0;
		for (s = 1; s <= parts[i].total; s++) {
			for (j = i; j < np; j++) {
				if (used[j] || parts[j].seq != s || parts[j].ref != parts[i].ref ||
				    parts[j].total != parts[i].total || strcmp(parts[j].number, parts[i].number))
					continue;
				strncat(text, parts[j].text, sizeof(text) - strlen(text) - 1);
				ids[nids++] = idx[j];
				used[j] = 1;
				break;
			}
			if (j == np)
				break;
		}
		if (nids != parts[i].total)
			continue;
		snprintf(key, sizeof(key), "%s|%s|%d", parts[i].number, parts[i].ts, parts[i].ref);
		found(fd, first, key, parts[i].number, text, ids, nids);
	}
	return missed;
}

static void check_inbox(int fd)
{
	/* a full SIM of long Unicode SMS lists to about 80 KB of PDU hex */
	static char buf[131072];
	int i, j, rc, changed, first = !have_seen, before = nseen, present;

	if (!(mode = recv_mode()))
		return;
	nmsgs = 0;
	rc = modem_mm() ? fetch_mm(fd, first, buf, sizeof(buf)) : fetch_at(fd, first, buf, sizeof(buf));
	if (rc < 0)
		return;
	changed = nseen != before;
	for (i = 0; !rc && i < nseen; i++) {
		for (j = 0, present = 0; j < nmsgs && !present; j++)
			present = !strcmp(seen[i], msgs[j].key);
		if (!present) {
			memmove(seen[i], seen[i + 1], sizeof(seen[0]) * (nseen - i - 1));
			nseen--;
			i--;
			changed = 1;
		}
	}
	if (changed || (first && !rc))
		save_seen();
}

static void delivered(const char *key)
{
	char cmd[48], r[256];
	int i, k;

	if ((i = find(sent, nsent, key)) >= 0) {
		memmove(sent[i], sent[i + 1], sizeof(sent[0]) * (nsent - i - 1));
		nsent--;
	}
	add_seen(key);
	save_seen();
	if (mode != 2)
		return;
	for (i = 0; i < nmsgs; i++) {
		if (strcmp(msgs[i].key, key))
			continue;
		for (k = 0; k < msgs[i].nids; k++) {
			if (modem_mm()) {
				snprintf(cmd, sizeof(cmd), "-m any --messaging-delete-sms=%d", msgs[i].ids[k]);
				modem_mm_cmd(cmd, r, sizeof(r));
			} else {
				snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", msgs[i].ids[k]);
				modem_at(cmd, r, sizeof(r));
			}
		}
	}
}

static int send_sms(const struct smsw_msg *m)
{
	static struct sms_pdu pdu[SMS_PARTS_MAX];
	static unsigned char ref;
	char r[256];
	int parts, i, rc;

	if (!ref)
		ref = time(NULL);
	if ((parts = sms_encode(m->number, m->text, m->n, ref++, pdu, SMS_PARTS_MAX)) < 0) {
		if (parts == SMS_TOO_LONG)
			syslog(LOG_WARNING, "%s: SMS not sent, longer than %d parts", m->name, SMS_PARTS_MAX);
		else
			syslog(LOG_WARNING, "%s: SMS not sent, %s", m->name,
			       parts == SMS_BAD_NUMBER ? "not a phone number" : "the text is empty or not UTF-8");
		return parts;
	}
	if ((rc = modem_sms_text(m->number, m->text, m->n, r, sizeof(r))) != -2) {
		if (rc) {
			syslog(LOG_WARNING, "%s: SMS not sent through ModemManager (%s)", m->name, *r ? r : "no answer");
			return SMS_FAILED;
		}
	} else {
		for (i = 0; i < parts; i++) {
			if ((rc = modem_sms(pdu[i].hex, pdu[i].len, r, sizeof(r)))) {
				r[strcspn(r, "\r\n")] = 0;
				/* Answer as sent: a retry from the app would send it twice. */
				if (rc == -3) {
					syslog(LOG_WARNING, "%s: SMS part %d of %d went out but the modem never confirmed it",
					       m->name, i + 1, parts);
					return SMS_UNSURE;
				}
				if (rc == -2)
					syslog(LOG_WARNING, "%s: SMS not sent, no modem AT port found", m->name);
				else
					syslog(LOG_WARNING, "%s: SMS not sent, part %d of %d failed (%s)", m->name, i + 1,
					       parts, *r ? r : "no answer from the modem");
				return rc == -2 ? SMS_NO_PORT : SMS_FAILED;
			}
		}
	}
	syslog(LOG_INFO, "%s sent an SMS in %d part%s", m->name, parts, parts > 1 ? "s" : "");
	return SMS_OK;
}

static void worker(int fd)
{
	static struct smsw_msg m;
	struct pollfd pf = { .fd = fd, .events = POLLIN };
	time_t next = 0, now;
	int ms;

	prctl(PR_SET_PDEATHSIG, SIGTERM);
	signal(SIGHUP, SIG_IGN);
	signal(SIGINT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	load_seen();
	for (;;) {
		now = time(NULL);
		ms = next > now ? (int)(next - now) * 1000 : 0;
		if (poll(&pf, 1, ms) > 0) {
			if (pf.revents & (POLLHUP | POLLERR) || smsw_get(fd, &m))
				_exit(0);
			if (m.type == 'S') {
				m.code = send_sms(&m);
				m.type = 'R';
				m.n = 0;
				smsw_put(fd, &m);
			} else if (m.type == 'D') {
				delivered(m.key);
			}
			continue;
		}
		next = time(NULL) + CHECK_SECS;
		check_inbox(fd);
	}
}

int smsw_start(void)
{
	int sv[2];
	pid_t pid;

	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv))
		return -1;
	if ((pid = fork()) < 0) {
		close(sv[0]);
		close(sv[1]);
		return -1;
	}
	if (!pid) {
		close(sv[0]);
		worker(sv[1]);
		_exit(0);
	}
	close(sv[1]);
	return sv[0];
}
