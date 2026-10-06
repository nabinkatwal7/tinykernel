#ifndef CRED_H
#define CRED_H

#include <stdint.h>

/* Who the running shell session acts as. uid 0 is root and may do anything. */
uint16_t cred_uid(void);
uint16_t cred_gid(void);
void     cred_set(uint16_t uid, uint16_t gid);

#endif
