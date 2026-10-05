#include "console.h"

void kernel_main(void)
{
	console_clear();
	console_write("Tiny OS\n");
	console_putchar('A');
	console_putchar('\n');
	console_printf("kernel %s boot=%d\n", "ok", 1);

	for (;;) {
		/* idle */
	}
}
