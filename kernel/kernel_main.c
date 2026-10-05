/* First kernel entry: freestanding, no libc. Just hang so we can boot it later. */
void kernel_main(void)
{
	for (;;) {
		/* blank loop */
	}
}
