/* board-wv.c: writev with an empty second iovec whose base is NULL, as
   musl's stdio does when it flushes at exit (f->write(f, 0, 0)). Linux
   accepts it; prints the result to stderr. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

int main(void)
{
	char a[] = "line written by writev\n";
	struct iovec v[2] = { { a, strlen(a) }, { 0, 0 } };
	ssize_t r = writev(1, v, 2);
	int e = errno;
	char m[80];
	snprintf(m, sizeof m, "writev(buf, NULL/0) = %ld, errno %d\n", (long)r, r < 0 ? e : 0);
	write(2, m, strlen(m));
	return r == (ssize_t)strlen(a) ? 0 : 1;
}
