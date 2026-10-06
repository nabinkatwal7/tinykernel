/* Memory-mapped files: reads see the file, a shared mapping writes changes back, a private one does not.
   Needs a formatted disk. */
#include "stdio.h"
#include "string.h"
#include "usys.h"

static int fails;

static void check(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s\n", what);
		fails++;
	}
}

static void put_file(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);

	if (fd >= 0) {
		write(fd, text, (int)strlen(text));
		close(fd);
	}
}

static int get_file(const char *path, char *buf, int cap)
{
	int fd = open(path, O_RDONLY), n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, cap - 1);
	close(fd);
	if (n >= 0)
		buf[n] = 0;
	return n;
}

int main(void)
{
	char buf[64], *m;

	put_file("map.txt", "hello mapped world");
	m = mmap("map.txt", 0, MAP_SHARED);
	if (!m) {
		puts("mmaptest: cannot map (is the disk formatted?)");
		return 1;
	}
	check(!strncmp(m, "hello mapped world", 18), "the mapping shows the file contents");
	memcpy(m, "HELLO", 5);                /* modify through memory */
	check(msync(m) == 0, "msync writes the change back");
	get_file("map.txt", buf, sizeof buf);
	check(!strcmp(buf, "HELLO mapped world"), "the file on disk has the change");
	check(munmap(m) == 0, "munmap");
	check(munmap(m) == -1, "unmapping twice fails");

	m = mmap("map.txt", 0, MAP_PRIVATE);
	memcpy(m, "xxxxx", 5);
	munmap(m);
	get_file("map.txt", buf, sizeof buf);
	check(!strcmp(buf, "HELLO mapped world"), "a private mapping never writes back");
	check(mmap("nosuchfile", 0, 0) == 0, "mapping a missing file fails");
	if (!fails)
		puts("mmaptest: all checks passed");
	return fails;
}
