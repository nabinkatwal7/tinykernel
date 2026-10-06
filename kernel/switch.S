/* Context switching and the ring 0 -> ring 3 transition. */
.text

/* void switch_context(uint32_t *old_esp, uint32_t new_esp)
   Saves callee-saved registers on the old stack, swaps stacks, restores the new ones. */
.globl _switch_context
_switch_context:
	movl 4(%esp), %eax
	movl 8(%esp), %edx
	pushl %ebp
	pushl %ebx
	pushl %esi
	pushl %edi
	movl %esp, (%eax)
	movl %edx, %esp
	popl %edi
	popl %esi
	popl %ebx
	popl %ebp
	ret

/* int enter_user(uint32_t entry, uint32_t user_esp)
   Runs a ring 3 program. Returns (via user_return) with its exit code. */
.globl _enter_user
_enter_user:
	pushl %ebp
	pushl %ebx
	pushl %esi
	pushl %edi
	pushfl
	movl %esp, _user_saved_esp
	movl 24(%esp), %eax        /* entry */
	movl 28(%esp), %edx        /* user stack */
	movw $0x23, %cx
	movw %cx, %ds
	movw %cx, %es
	movw %cx, %fs
	movw %cx, %gs
	pushl $0x23                /* ss */
	pushl %edx                 /* esp */
	pushl $0x202               /* eflags: IF set */
	pushl $0x1B                /* cs */
	pushl %eax                 /* eip */
	iret

/* void user_return(int code) — abandon whatever kernel stack we are on and resume after
   enter_user() with the given return value. */
.globl _user_return
_user_return:
	movl 4(%esp), %eax
	movl _user_saved_esp, %esp
	popfl
	popl %edi
	popl %esi
	popl %ebx
	popl %ebp
	ret

.data
.globl _user_saved_esp
_user_saved_esp:
	.long 0
