#ifndef UIO_H
#define UIO_H

#include "usys.h"

/* Small helpers for filter programs: read a whole input (a file, or standard input when path is NULL). */
static inline int uio_slurp(const char *path, char *buf, int cap)
{
	int fd = path ? open(path, O_RDONLY) : 0, n = 0, r;

	if (fd < 0)
		return -1;
	while (n < cap - 1 && (r = read(fd, buf + n, cap - 1 - n)) > 0)
		n += r;
	buf[n] = '\0';
	if (path)
		close(fd);
	return n;
}

#endif
