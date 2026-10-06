/* mkdir/rmdir from a program, files inside directories, and the error cases. */
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

static int write_file(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC), n;

	if (fd < 0)
		return fd;
	n = write(fd, text, (int)strlen(text));
	close(fd);
	return n;
}

int main(void)
{
	char buf[32];
	int fd, n;

	if (mkdir("udir") == -6) {
		puts("dirtest: no formatted disk - run 'format' first");
		return 1;
	}
	check(mkdir("udir") == -2, "mkdir on an existing name fails (EEXIST)");
	check(mkdir("udir/inner") == 0, "nested mkdir");
	check(write_file("udir/inner/note.txt", "nested file") == 11, "write into a nested directory");
	fd = open("/udir/inner/note.txt", O_RDONLY);
	check(fd >= 3, "open by absolute path");
	n = read(fd, buf, sizeof buf);
	check(n == 11 && !memcmp(buf, "nested file", 11), "read it back");
	close(fd);
	check(rmdir("udir") == -10, "rmdir of a non-empty directory fails (ENOTEMPTY)");
	check(open("udir", O_RDONLY) < 0, "a directory cannot be opened as a file");
	check(rmdir("udir/inner/note.txt") == -9, "rmdir of a file fails (ENOTDIR)");

	check(kcmd("rm udir/inner/note.txt") == 0, "remove the file via the kernel shell");
	check(rmdir("udir/inner") == 0 && rmdir("udir") == 0, "rmdir bottom-up");
	check(open("udir/inner/note.txt", O_RDONLY) < 0, "everything is gone");

	if (!fails)
		puts("dirtest: all checks passed");
	return fails;
}
