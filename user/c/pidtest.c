/* getpid()/getppid(): a program runs inside a task, so it reports that task's ids. */
#include "stdio.h"
#include "usys.h"

int main(void)
{
	int pid = getpid(), ppid = getppid();

	printf("pid=%d ppid=%d\n", pid, ppid);
	return pid >= 0 && ppid >= 0 ? 0 : 1;
}
