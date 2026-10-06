#ifndef USER_H
#define USER_H

#include <stdint.h>

#define USER_BASE  0x800000u   /* programs are linked (org) here */
#define USER_PAGES 32u         /* 128 KiB: image at the bottom, stack at the top */
#define USER_END   (USER_BASE + USER_PAGES * 4096u)

/* Loads the named program (file on disk first, then built-in) and runs it in ring 3.
   Returns its exit code, or a negative value if it could not be started. */
int  user_run(const char *name);
void user_abort(void) __attribute__((noreturn)); /* called from the fault handler */
void user_exit(int code) __attribute__((noreturn));
int  user_is_active(void);
void user_list_builtin(void);
int  user_install_builtin(const char *name); /* copy a built-in program onto the disk */

/* asm */
int  enter_user(uint32_t entry, uint32_t user_esp);
void user_return(int code) __attribute__((noreturn));

#endif
