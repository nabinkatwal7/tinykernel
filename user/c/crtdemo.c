/* First C user program: proves crt0 passes argc/argv/envp and exit(main()) works. */
#include "usys.h"

static void put_num(int n)
{
	char buf[12];
	int i = 0;

	if (n == 0)
		buf[i++] = '0';
	while (n > 0) {
		buf[i++] = (char)('0' + n % 10);
		n /= 10;
	}
	while (i--)
		putchar(buf[i]);
}

int main(int argc, char **argv, char **envp)
{
	int envc = 0;

	(void)argv;
	write("C runtime: argc=", 16);
	put_num(argc);
	while (envp[envc])
		envc++;
	write(" envc=", 6);
	put_num(envc);
	putchar('\n');
	return 3;
}
