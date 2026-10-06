/*
 * User-program entry (C runtime start-up). The kernel leaves  [esp]=argc  [esp+4]=argv
 * [esp+8]=envp  and starts us here. Calls main(argc, argv, envp) and exits with its result.
 */
.text
.globl _start
_start:
	xorl %ebp, %ebp            /* end of the frame-pointer chain */
	movl (%esp), %eax          /* argc */
	movl 4(%esp), %ecx         /* argv */
	movl 8(%esp), %edx         /* envp */
	pushl %edx
	pushl %ecx
	pushl %eax
	call _main
	pushl %eax                 /* exit(main's return value) */
	call _exit
1:	jmp 1b
