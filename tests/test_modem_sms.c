// SPDX-License-Identifier: GPL-2.0
#define D		"/tmp/msip-sms-test"
#define MMCLI		D "/mmcli"
#define UBUS		D "/ubus"
#define AT_DAEMON	D "/at-daemon"
#define SMS_TMP		D "/sms.txt"
#define AT_LOCK_DIR	D
#include <assert.h>
#include <pty.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "../src/modem.c"

static void put(const char *path, const char *s)
{
	FILE *f = fopen(path, "w");

	assert(f);
	fputs(s, f);
	fclose(f);
	chmod(path, 0755);
}

static int has(const char *s)
{
	char buf[8192];
	FILE *f = fopen(D "/log", "r");
	size_t n;

	if (!f)
		return 0;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = 0;
	fclose(f);
	return strstr(buf, s) != NULL;
}

/* A modem on a pty that never shows the prompt (prompt = 0) or shows it and never confirms the
 * PDU (prompt = 1). Either way msipd must close the prompt with ESC before it lets go. */
static int direct_send(int prompt)
{
	char r[256], buf[512], name[64];
	size_t got = 0;
	int m, s, st, esc = 0;
	ssize_t k;
	pid_t pid;

	assert(!openpty(&m, &s, name, NULL, NULL));
	if (!(pid = fork())) {
		close(s);
		for (;;) {
			if ((k = read(m, buf + got, sizeof(buf) - 1 - got)) <= 0)
				_exit(esc ? 0 : 1);
			got += k;
			buf[got] = 0;
			if (memchr(buf, 0x1b, got))
				esc = 1;
			if (strstr(buf, "AT+CMGF?\r"))
				(void)!write(m, "\r\n+CMGF: 0\r\nOK\r\n", 16);
			else if (strstr(buf, "AT+CMGF=0\r") || (esc && strstr(buf, "AT\r")))
				(void)!write(m, "\r\nOK\r\n", 6);
			else if (prompt && strstr(buf, "AT+CMGS="))
				(void)!write(m, "\r\n> ", 4);
			else
				continue;
			if (esc)
				_exit(0);
			got = 0;
		}
	}
	modem_set_port(name);
	k = modem_sms("0001", 1, r, sizeof(r));
	close(m);
	close(s);
	assert(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && !WEXITSTATUS(st));
	return k;
}

int main(void)
{
	char r[256];

	assert(!system("rm -rf " D " && mkdir -p " D));
	put(MMCLI, "#!/bin/sh\n"
		   "echo \"mmcli $*\" >> " D "/log\n"
		   "case \"$*\" in\n"
		   "-L) echo '/org/freedesktop/ModemManager1/Modem/0 [Quectel] RG500Q' ;;\n"
		   "*create-sms*) echo \"text $(cat " SMS_TMP ")\" >> " D "/log\n"
		   "  echo 'Successfully created new SMS: /org/freedesktop/ModemManager1/SMS/3' ;;\n"
		   "*--send*) [ -f " D "/fail ] && { echo \"error: couldn't send the SMS: '+CMS ERROR: 38'\"; exit 1; }\n"
		   "  echo 'successfully sent the SMS' ;;\n"
		   "esac\n");

	assert(modem_sms_text("+94771234567", "hi there", 8, r, sizeof(r)) == 0);
	assert(has("number='+94771234567'") && has("text hi there") && has("-s 3 --send"));
	assert(has("--messaging-delete-sms=3") && access(SMS_TMP, F_OK));
	put(D "/fail", "");
	assert(modem_sms_text("0771234567", "x", 1, r, sizeof(r)) == -1 && !strcmp(r, "+CMS ERROR: 38"));
	assert(modem_sms_text("077;reboot", "x", 1, r, sizeof(r)) == -1);

	assert(!system("rm -f " D "/log " D "/fail"));
	put(AT_DAEMON, "#!/bin/sh\n");
	put(UBUS, "#!/bin/sh\n"
		  "echo \"ubus $*\" >> " D "/log\n"
		  "case \"$*\" in\n"
		  "*AT+CMGS=*) printf '%s\\n' '{\"status\": \"success\", \"response\": \"\\r\\n> \"}' ;;\n"
		  "*raw_at_content*1A*) [ -f " D "/fail ] && { printf '%s\\n' '{\"response\": \"\\r\\n+CMS ERROR: 500\\r\\n\"}'; exit 0; }\n"
		  "  printf '%s\\n' '{\"response\": \"\\r\\n+CMGS: 12\\r\\n\\r\\nOK\\r\\n\"}' ;;\n"
		  "*) printf '%s\\n' '{\"response\": \"\\r\\nOK\\r\\n\"}' ;;\n"
		  "esac\n");
	modem_set_port("/dev/ttyUSB2");
	assert(!modem_direct());
	assert(modem_sms("0001", 1, r, sizeof(r)) == 0);
	assert(has("\"at_cmd\":\"AT+CMGF=0\"") && has("\"at_cmd\":\"AT+CMGS=1\",\"end_flag\":\">\""));
	assert(has("\"raw_at_content\":\"303030311A\""));
	put(D "/fail", "");
	assert(modem_sms("0001", 1, r, sizeof(r)) == -1 && !strcmp(r, "+CMS ERROR: 500"));
	assert(!has("\"1B\""));

	/* no answer after the PDU: maybe sent, and the prompt is closed with ESC */
	assert(!system("rm -f " D "/log " D "/fail"));
	put(UBUS, "#!/bin/sh\n"
		  "echo \"ubus $*\" >> " D "/log\n"
		  "case \"$*\" in\n"
		  "*AT+CMGS=*) printf '%s\\n' '{\"response\": \"\\r\\n> \"}' ;;\n"
		  "*raw_at_content*1A*) printf '%s\\n' '{\"response\": \"\"}' ;;\n"
		  "*) printf '%s\\n' '{\"response\": \"\\r\\nOK\\r\\n\"}' ;;\n"
		  "esac\n");
	assert(modem_sms("0001", 1, r, sizeof(r)) == -3 && has("\"raw_at_content\":\"1B\""));

	unlink(AT_DAEMON);
	assert(modem_direct());
	assert(direct_send(0) == -1);
	assert(direct_send(1) == -3);

	assert(!system("rm -rf " D));
	puts("test_modem_sms ok");
	return 0;
}
