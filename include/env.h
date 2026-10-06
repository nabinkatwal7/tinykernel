#ifndef ENV_H
#define ENV_H

#define ENV_MAX      16
#define ENV_NAME_MAX 24
#define ENV_VAL_MAX  64

/* Process environment: a small NAME=value table inherited by every program the shell runs. */
int         env_set(const char *name, const char *value);  /* 0 on success, -1 bad name / full */
const char *env_get(const char *name);                      /* NULL if unset */
int         env_unset(const char *name);                    /* 0 if it existed */
int         env_count(void);
/* Entry i as "NAME=value" in buf (NUL-terminated); returns 0, or -1 if i is out of range. */
int         env_entry(int i, char *buf, int size);

#endif
