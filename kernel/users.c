#include "users.h"

#include "fs.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "sha256.h"
#include "timer.h"

#define PASSWD_PATH "/etc/passwd"
#define PASSWD_MAX  4096

int users_exist(void)
{
	return fs_mounted() && fs_size(PASSWD_PATH) > 0;
}

/* Reads the file into a fresh NUL-terminated buffer (caller kfree), or NULL. */
static char *load(void)
{
	int n = fs_mounted() ? fs_size(PASSWD_PATH) : -1;
	char *buf;

	if (n < 0 || n > PASSWD_MAX)
		return 0;
	buf = kmalloc((size_t)n + 1);
	if (!buf)
		return 0;
	n = n ? fs_read(PASSWD_PATH, buf, (uint32_t)n) : 0;
	if (n < 0) {
		kfree(buf);
		return 0;
	}
	buf[n] = '\0';
	return buf;
}

/* Splits one "name:pw:uid:gid:home" line (modified in place). */
static int parse_line(char *line, struct user *u)
{
	char *f[5];
	int i;
	uint32_t uid, gid;

	for (i = 0; i < 5; i++) {
		f[i] = line;
		if (i < 4) {
			line = f[i];
			while (*line && *line != ':')
				line++;
			if (!*line)
				return -1;
			*line++ = '\0';
		}
	}
	if (kstrtoul(f[2], &uid) || kstrtoul(f[3], &gid))
		return -1;
	kstrlcpy(u->name, f[0], sizeof u->name);
	kstrlcpy(u->pw, f[1], sizeof u->pw);
	u->uid = (uint16_t)uid;
	u->gid = (uint16_t)gid;
	kstrlcpy(u->home, f[4], sizeof u->home);
	return 0;
}

static int find(const char *name, int uid, struct user *out)
{
	char *buf = load(), *p, *eol;
	int rc = FS_ENOENT;

	if (!buf)
		return FS_ENOENT;
	for (p = buf; *p; p = eol + 1) {
		struct user u;
		int more;

		for (eol = p; *eol && *eol != '\n'; eol++)
			;
		more = *eol != '\0';
		*eol = '\0';
		if (!parse_line(p, &u) && (name ? !kstrcmp(u.name, name) : u.uid == uid)) {
			*out = u;
			rc = FS_OK;
			break;
		}
		if (!more)
			break;
	}
	kfree(buf);
	return rc;
}

int user_find_name(const char *name, struct user *u)
{
	return find(name, 0, u);
}

int user_find_uid(uint16_t uid, struct user *u)
{
	return find(0, uid, u);
}

const char *user_name_of(uint16_t uid)
{
	static char buf[USER_NAME_MAX];
	struct user u;

	if (!user_find_uid(uid, &u))
		kstrlcpy(buf, u.name, sizeof buf);
	else
		ksnprintf(buf, sizeof buf, "%u", uid);
	return buf;
}

/*
 * Stored passwords look like  $s$<8 hex digits of salt>$<64 hex digits>  : SHA-256 applied 2000 times to
 * salt + password (each round hashes the previous digest followed by the salt and password again), so equal
 * passwords differ and guessing is slowed down. A field without the prefix is an old plain-text password:
 * it still works and is replaced by a hash the next time the password is set.
 */
#define HASH_ROUNDS 2000

static void to_hex(const uint8_t *in, int n, char *out)
{
	static const char hex[] = "0123456789abcdef";
	int i;

	for (i = 0; i < n; i++) {
		out[i * 2] = hex[in[i] >> 4];
		out[i * 2 + 1] = hex[in[i] & 15];
	}
	out[n * 2] = '\0';
}

static void hash_password(const char *salt_hex, const char *password, char *out, size_t size)
{
	uint8_t digest[32], buf[32 + 8 + USER_PW_MAX];
	char hex[65], salt8[9];
	size_t plen = kstrlen(password), n;
	int i;

	if (plen > USER_PW_MAX - 1)
		plen = USER_PW_MAX - 1;
	memcpy(buf, salt_hex, 8);
	memcpy(buf + 8, password, plen);
	sha256(buf, 8 + plen, digest);
	for (i = 1; i < HASH_ROUNDS; i++) {
		memcpy(buf, digest, 32);
		memcpy(buf + 32, salt_hex, 8);
		memcpy(buf + 40, password, plen);
		n = 40 + plen;
		sha256(buf, n, digest);
	}
	to_hex(digest, 32, hex);
	memcpy(salt8, salt_hex, 8);
	salt8[8] = 0;
	ksnprintf(out, size, "$s$%s$%s", salt8, hex);
}

static void fresh_salt(char salt_hex[9])
{
	static uint32_t counter;
	uint8_t raw[16], digest[32];
	uint32_t t = timer_ticks();

	memcpy(raw, &t, 4);
	counter++;
	memcpy(raw + 4, &counter, 4);
	memcpy(raw + 8, "tinysalt", 8);
	sha256(raw, sizeof raw, digest);
	to_hex(digest, 4, salt_hex);
}

static void make_stored(const char *password, char *out, size_t size)
{
	char salt[9];

	fresh_salt(salt);
	hash_password(salt, password, out, size);
}

int user_verify(const struct user *u, const char *password)
{
	char again[USER_PW_MAX];

	if (kstrncmp(u->pw, "$s$", 3))
		return !kstrcmp(u->pw, password); /* legacy plain text */
	if (kstrlen(u->pw) < 12 || u->pw[11] != '$')
		return 0;
	hash_password(u->pw + 3, password, again, sizeof again);
	return !kstrcmp(again, u->pw);
}

static int has_char(const char *s, char c)
{
	for (; *s; s++)
		if (*s == c)
			return 1;
	return 0;
}

static int valid_name(const char *n)
{
	size_t i, len = kstrlen(n);

	if (!len || len >= USER_NAME_MAX)
		return 0;
	for (i = 0; i < len; i++)
		if (!((n[i] >= 'a' && n[i] <= 'z') || (n[i] >= '0' && n[i] <= '9') || n[i] == '_'))
			return 0;
	return 1;
}

int user_add(const char *name, const char *password, uint16_t uid, uint16_t gid, const char *home)
{
	struct user u;
	char *old = load(), *out;
	size_t oldlen = old ? kstrlen(old) : 0;
	char hashed[USER_PW_MAX];
	int n, rc;

	if (!valid_name(name) || has_char(password, ':') || has_char(password, '\n') || has_char(home, ':')) {
		kfree(old);
		return FS_EINVAL;
	}
	if (!user_find_name(name, &u)) {
		kfree(old);
		return FS_EEXIST;
	}
	make_stored(password, hashed, sizeof hashed);
	out = kmalloc(oldlen + 200);
	if (!out) {
		kfree(old);
		return FS_ENOSPC;
	}
	if (old)
		memcpy(out, old, oldlen);
	n = ksnprintf(out + oldlen, 200, "%s:%s:%u:%u:%s\n", name, hashed, uid, gid, home);
	if (!fs_mounted()) {
		rc = FS_ENOMOUNT;
	} else {
		fs_mkdir("/etc"); /* fine if it exists */
		rc = fs_write(PASSWD_PATH, out, (uint32_t)(oldlen + (size_t)n));
		if (!rc)
			fs_chmod(PASSWD_PATH, 0600); /* the password field is not for everybody */
	}
	kfree(old);
	kfree(out);
	return rc;
}

/* Replace one user's password field (hashing the new password) by rewriting the whole file. */
int user_set_password(const char *name, const char *password)
{
	char *buf = load(), *out, *p, *eol;
	char hashed[USER_PW_MAX];
	size_t pos = 0;
	int found = 0, rc;

	if (!buf)
		return FS_ENOENT;
	if (has_char(password, ':') || has_char(password, '\n')) {
		kfree(buf);
		return FS_EINVAL;
	}
	out = kmalloc(kstrlen(buf) + 200);
	if (!out) {
		kfree(buf);
		return FS_ENOSPC;
	}
	make_stored(password, hashed, sizeof hashed);
	for (p = buf; *p; p = eol + 1) {
		size_t nlen = kstrlen(name);
		int more;

		for (eol = p; *eol && *eol != '\n'; eol++)
			;
		more = *eol != '\0';
		*eol = '\0';
		if (!kstrncmp(p, name, nlen) && p[nlen] == ':') { /* "name:oldpw:rest" -> "name:newpw:rest" */
			char *rest = p + nlen + 1;

			while (*rest && *rest != ':')
				rest++;
			pos += (size_t)ksnprintf(out + pos, 200 + kstrlen(rest), "%s:%s%s\n", name, hashed, rest);
			found = 1;
		} else {
			pos += (size_t)ksnprintf(out + pos, kstrlen(p) + 2, "%s\n", p);
		}
		if (!more)
			break;
	}
	rc = found ? fs_write(PASSWD_PATH, out, (uint32_t)pos) : FS_ENOENT;
	if (!rc)
		fs_chmod(PASSWD_PATH, 0600);
	kfree(buf);
	kfree(out);
	return rc;
}
