#include "cred.h"

static uint16_t uid, gid; /* boots as root */

uint16_t cred_uid(void) { return uid; }
uint16_t cred_gid(void) { return gid; }

void cred_set(uint16_t u, uint16_t g)
{
	uid = u;
	gid = g;
}
