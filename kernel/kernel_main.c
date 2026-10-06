#include "console.h"
#include "keyboard.h"

void kernel_main(void)
{
	console_clear();
	console_write("Tiny OS\n");
	console_putchar('A');
	console_putchar('\n');
	console_printf("kernel %s boot=%d\n", "ok", 1);

	console_write("Type something:\n");
	for (;;)
		console_putchar(keyboard_getchar());
}
