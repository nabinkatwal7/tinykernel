#ifndef USER_H
#define USER_H

#include <stdint.h>

#define USER_BASE  0x800000u   /* programs are linked (org) here */
#define USER_PAGES 32u         /* 128 KiB: image at the bottom, stack at the top */
#define STACK_RESERVE (16 * 1024) /* top of the window is the stack; programs may not load there */
#define USER_END   (USER_BASE + USER_PAGES * 4096u)

/* Loads the named program (file on disk first, then built-in) and runs it in ring 3.
   Returns its exit code, or a negative value if it could not be started. */
int  user_run(const char *name);
/* Same, passing argv[0..argc-1] and the environment: [esp]=argc, [esp+4]=argv, [esp+8]=envp. */
int  user_run_args(const char *name, int argc, char **argv);
void user_abort(void) __attribute__((noreturn)); /* called from the fault handler */
void user_exit(int code) __attribute__((noreturn));
int  user_is_active(void);
struct regs;
int  user_exec(struct regs *r, const char *path, char *const *user_argv); /* 0 = switched, -1 = no such program */
uint32_t user_sbrk(int32_t delta); /* grow/shrink the program heap; old break or (uint32_t)-1 */
void user_request_abort(void);     /* checked at every syscall; for Ctrl+C while the program is in the kernel */
int  user_abort_requested(void);
void user_list_builtin(void);
int  user_install_builtin(const char *name); /* copy a built-in program onto the disk */

/* Programs embedded in the kernel image; generated into build/progs_gen.c from user/. */
struct builtin_prog {
	const char *name;
	const uint8_t *start, *end;
};
extern const struct builtin_prog builtin_progs[];
extern const unsigned builtin_nprogs;

/* asm */
int  enter_user(uint32_t entry, uint32_t user_esp);
void user_return(int code) __attribute__((noreturn));

#endif
