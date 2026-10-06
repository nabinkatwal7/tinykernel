; Built-in user program: executes a privileged instruction to prove faults are contained.
bits 32
org 0x800000

start:
	hlt
	jmp $
