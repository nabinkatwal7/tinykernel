#ifndef USERS_H
#define USERS_H

#include <stdint.h>

/*
 * The user database, /etc/passwd on TinyFS (root-only, mode 0600; read and written directly by the kernel):
 *     name:password:uid:gid:home
 * With no database the system runs without logins, as root.
 */
#define USER_NAME_MAX 16
#define USER_HOME_MAX 32
#define USER_PW_MAX   96

struct user {
	char name[USER_NAME_MAX];
	char pw[USER_PW_MAX];          /* stored password field (see users.c) */
	uint16_t uid, gid;
	char home[USER_HOME_MAX];
};

int  users_exist(void);                                   /* is there a user database? */
int  user_find_name(const char *name, struct user *u);    /* 0, or FS_ENOENT */
int  user_find_uid(uint16_t uid, struct user *u);
int  user_add(const char *name, const char *password, uint16_t uid, uint16_t gid, const char *home); /* 0 or FS_E* */
int  user_set_password(const char *name, const char *password); /* 0 or FS_E*; stores a salted hash */
int  user_verify(const struct user *u, const char *password);   /* 1 if the password is right */
const char *user_name_of(uint16_t uid);                   /* "name", or "1000"-style number when unknown (static buffer) */

#endif
