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

/* int fork_snapshot(uint32_t *child_esp, uint32_t child_top, uint32_t parent_top)
   Copies the live part of the caller's stack (saved registers included) to the top of the child
   stack, planting a stub below the return address so the child "returns" 0 from this call.
   The parent gets 1. The child frame matches what switch_context expects to pop. */
.globl _fork_snapshot
_fork_snapshot:
	pushl %ebp
	pushl %ebx
	pushl %esi
	pushl %edi
	movl 20(%esp), %eax        /* child_esp out */
	movl 24(%esp), %edx        /* child_top */
	movl 28(%esp), %ecx        /* parent_top */
	movl %ecx, %ebx
	subl %esp, %ebx            /* ebx = bytes in use: regs + return address + everything above */
	movl %edx, %edi
	subl %ebx, %edi
	subl $4, %edi              /* room for the stub address */
	movl %edi, (%eax)
	movl %esp, %esi
	movl $4, %ecx
	cld
	rep movsl                  /* the four saved registers */
	movl $fork_child_stub, (%edi)
	addl $4, %edi
	movl %ebx, %ecx
	subl $16, %ecx
	shrl $2, %ecx
	rep movsl                  /* return address and the rest of the stack */
	popl %edi
	popl %esi
	popl %ebx
	popl %ebp
	movl $1, %eax
	ret

fork_child_stub:
	xorl %eax, %eax
	ret

/* GCC's stack probe for frames over 4 KiB. Kernel stacks are pre-mapped, so nothing to probe;
   it must preserve every register. */
.globl ___chkstk_ms
___chkstk_ms:
	ret
