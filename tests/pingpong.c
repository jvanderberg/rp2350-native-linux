// SPDX-License-Identifier: MIT
/*
 * pingpong: the forktest pipe round trip between two ordinary (FDPIC, no
 * ATRANS) processes, as the baseline for the cost of an ATRANS switch.
 * nommu has no fork, so the child is vfork + exec of this program.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#define N 500

static unsigned long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000UL + ts.tv_nsec / 1000;
}

int main(int argc, char **argv)
{
	int a[2], b[2], i, st;
	char c = 'x', fa[12], fb[12];
	unsigned long t0, dt;
	pid_t pid;

	if (argc == 4 && !strcmp(argv[1], "child")) {
		int in = atoi(argv[2]), outfd = atoi(argv[3]);

		for (i = 0; i < N; i++) {
			if (read(in, &c, 1) != 1)
				return 1;
			write(outfd, &c, 1);
		}
		return 0;
	}
	if (pipe(a) || pipe(b))
		return 1;
	snprintf(fa, sizeof(fa), "%d", a[0]);
	snprintf(fb, sizeof(fb), "%d", b[1]);
	pid = vfork();
	if (pid == 0) {
		execl("/proc/self/exe", argv[0], "child", fa, fb, (char *)0);
		_exit(99);
	}
	t0 = now_us();
	for (i = 0; i < N; i++) {
		write(a[1], &c, 1);
		if (read(b[0], &c, 1) != 1)
			break;
	}
	dt = now_us() - t0;
	waitpid(pid, &st, 0);
	printf("  pipe round trip (FDPIC, no ATRANS): %lu x0.1 us (2 switches each)\n", dt * 10 / N);
	printf("%s pipe ping-pong between two FDPIC processes\n", i == N && st == 0 ? "PASS" : "FAIL");
	return 0;
}
