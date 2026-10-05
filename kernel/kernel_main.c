/* 32-bit kernel entry — reached after bootloader loads us and enters PM. */
void kernel_main(void)
{
	volatile unsigned short *vga = (unsigned short *)0xB8000;
	const char *msg = "kernel ok";
	int i;

	/* Row 1 (leave BIOS "Tiny OS" on row 0). White on black. */
	for (i = 0; msg[i]; i++)
		vga[80 + i] = (unsigned short)(msg[i] | 0x0F00);

	for (;;) {
		/* booted successfully — hang */
	}
}
