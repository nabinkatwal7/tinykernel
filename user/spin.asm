; Built-in user program: spins forever without any syscall. Only Ctrl+C (or a fault) stops it.
bits 32
org 0x800000

start:
	jmp start
