/* Built-in user programs, assembled by nasm into flat binaries and embedded here. */
.section .rodata
.globl _builtin_hello_start, _builtin_hello_end
_builtin_hello_start:
	.incbin "build/hello.bin"
_builtin_hello_end:

.globl _builtin_counter_start, _builtin_counter_end
_builtin_counter_start:
	.incbin "build/counter.bin"
_builtin_counter_end:

.globl _builtin_fault_start, _builtin_fault_end
_builtin_fault_start:
	.incbin "build/fault.bin"
_builtin_fault_end:
