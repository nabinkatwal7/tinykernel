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

.globl _builtin_evil_start, _builtin_evil_end
_builtin_evil_start:
	.incbin "build/evil.bin"
_builtin_evil_end:

.globl _builtin_spin_start, _builtin_spin_end
_builtin_spin_start:
	.incbin "build/spin.bin"
_builtin_spin_end:

.globl _builtin_args_start, _builtin_args_end
_builtin_args_start:
	.incbin "build/args.bin"
_builtin_args_end:

.globl _builtin_helloelf_start, _builtin_helloelf_end
_builtin_helloelf_start:
	.incbin "build/helloelf.elf"
_builtin_helloelf_end:
