#include "users.h"

#include "fs.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"

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

int user_verify(const struct user *u, const char *password)
{
	return !kstrcmp(u->pw, password); /* plain text for now */
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
	int n, rc;

	if (!valid_name(name) || has_char(password, ':') || has_char(password, '\n') || has_char(home, ':')) {
		kfree(old);
		return FS_EINVAL;
	}
	if (!user_find_name(name, &u)) {
		kfree(old);
		return FS_EEXIST;
	}
	out = kmalloc(oldlen + 160);
	if (!out) {
		kfree(old);
		return FS_ENOSPC;
	}
	if (old)
		memcpy(out, old, oldlen);
	n = ksnprintf(out + oldlen, 160, "%s:%s:%u:%u:%s\n", name, password, uid, gid, home);
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
