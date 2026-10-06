#include "usys.h"

void exit(int code)
{
	syscall3(SYS_EXIT, code, 0, 0);
	for (;;)
		;
}

/* MinGW's gcc emits a call to __main at the top of main() (static constructor hook). */
void __main(void)
{
}
