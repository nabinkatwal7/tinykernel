#include "telnetd.h"

#include "console.h"
#include "cred.h"
#include "ip.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "shell.h"
#include "tcp.h"
#include "users.h"

/*
 * A tiny telnet-like server: one client at a time gets a command line over TCP. Each line is run by the kernel shell with
 * its output captured and sent back. Telnet option negotiation (bytes starting with 0xFF) is stripped, and both CR LF
 * and bare LF end a line. With a user database the client must log in first; commands then run with that user's rights.
 * There is no encryption: use it on a trusted network only.
 */
#define OUT_CAP 8192

static char *txbuf; /* working space for put(): the job stack is only 16 KiB, so it lives in the heap */
#define TX_CAP (OUT_CAP + OUT_CAP / 2)

static int put(int c, const char *s)
{
	uint32_t n = 0;

	for (; *s && n + 2 < TX_CAP; s++) { /* LF -> CR LF, as telnet expects */
		if (*s == '\n')
			txbuf[n++] = '\r';
		txbuf[n++] = *s;
	}
	return n ? tcp_send(c, txbuf, n, 5000) : 0;
}

/* Reads one line (without the terminator), skipping telnet command bytes. 1 on a line, 0 if the peer closed. */
static int get_line(int c, char *line, int cap)
{
	int len = 0, skip = 0;

	for (;;) {
		uint8_t b;
		int n = tcp_recv(c, &b, 1, 120000);

		if (n <= 0)
			return 0;
		if (skip) { /* the two bytes after IAC (option negotiation) */
			skip--;
			continue;
		}
		if (b == 0xFF) {
			skip = 2;
			continue;
		}
		if (b == '\n') {
			line[len] = '\0';
			return 1;
		}
		if (b == '\r' || b == 0)
			continue;
		if (b == 0x08 && len) {
			len--;
			continue;
		}
		if (len < cap - 1)
			line[len++] = (char)b;
	}
}

/* Asks for a user name and password. 1 if the client is who it says (and sets uid/gid), 0 after three failures. */
static int login(int c, uint16_t *uid, uint16_t *gid)
{
	int tries;

	for (tries = 0; tries < 3; tries++) {
		char name[32], pw[64];
		struct user u;

		put(c, "login: ");
		if (!get_line(c, name, sizeof name))
			return 0;
		put(c, "password: ");
		if (!get_line(c, pw, sizeof pw))
			return 0;
		if (!user_find_name(name, &u) && user_verify(&u, pw)) {
			*uid = u.uid;
			*gid = u.gid;
			return 1;
		}
		put(c, "Login incorrect\n");
	}
	return 0;
}

static void session(int c)
{
	uint16_t uid = 0, gid = 0;
	char line[200];
	char *out = kmalloc(OUT_CAP);

	txbuf = kmalloc(TX_CAP);
	if (!out || !txbuf) {
		kfree(out);
		kfree(txbuf);
		return;
	}
	put(c, "Tiny OS telnet server\n");
	if (users_exist() && !login(c, &uid, &gid)) {
		put(c, "Too many failures.\n");
		kfree(out);
		return;
	}
	for (;;) {
		uint16_t saved_uid = cred_uid(), saved_gid = cred_gid();
		int n;

		put(c, "tiny> ");
		if (!get_line(c, line, sizeof line))
			break;
		if (!kstrcmp(line, "exit") || !kstrcmp(line, "quit") || !kstrcmp(line, "logout"))
			break;
		if (!line[0])
			continue;
		if (!kstrncmp(line, "telnetd", 7) || !kstrncmp(line, "reboot", 6) || !kstrncmp(line, "halt", 4)) {
			put(c, "not allowed over telnet\n");
			continue;
		}
		cred_set(uid, gid); /* act as the logged-in user while the command runs */
		console_capture_begin(out, OUT_CAP);
		shell_exec(line);
		n = console_capture_end();
		cred_set(saved_uid, saved_gid);
		out[n] = '\0';
		if (put(c, out))
			break;
	}
	put(c, "Goodbye.\n");
	kfree(out);
}

/* telnetd [PORT [SESSIONS]] : serve shell sessions (default port 2323, until Ctrl+C; run it with & to keep the shell) */
int cmd_telnetd(int argc, char **argv)
{
	uint32_t port = 2323, sessions = 0, served = 0, peer;
	uint16_t pport;
	char s[16];
	int l;

	if ((argc > 1 && (kstrtoul(argv[1], &port) || !port || port > 65535)) || (argc > 2 && kstrtoul(argv[2], &sessions))) {
		console_write("usage: telnetd [PORT [SESSIONS]]\n");
		return 1;
	}
	l = tcp_listen((uint16_t)port);
	if (l < 0) {
		console_write("telnetd: cannot listen (port in use or no network)\n");
		return 1;
	}
	console_printf("telnetd: listening on port %u\n", (uint32_t)port);
	while (!sessions || served < sessions) {
		int c = tcp_accept(l, 1000, &peer, &pport);

		if (c < 0) {
			if (shell_interrupted())
				break;
			continue;
		}
		console_printf("telnetd: session from %s:%u\n", ip_str(peer, s), pport);
		session(c);
		tcp_close(c);
		served++;
	}
	tcp_close(l);
	console_printf("telnetd: stopped after %u session(s)\n", served);
	return 0;
}
