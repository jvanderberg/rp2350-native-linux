// SPDX-License-Identifier: MIT
/*
 * watchd: run a command when a file's first line changes.
 *
 *   watchd SECONDS FILE COMMAND [FILE COMMAND]...
 *
 * Every SECONDS, reads the first line of each FILE (a sysfs attribute such
 * as /sys/block/mmcblk0/size). When it differs from the last reading, runs
 * "COMMAND VALUE" through /bin/sh and waits for it. A FILE that can't be
 * read reads as "". The first reading only sets the starting value.
 *
 * S42watchd runs it for S03sd and S41linkwatch instead of a "while sleep 2"
 * loop per watch: each such loop keeps a hush process (about 130 KiB with
 * its stack, data and heap) and starts a sleep every 2 s. This is one small
 * process.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAXW 8

static void readline(const char *path, char *buf, size_t n)
{
	int fd = open(path, O_RDONLY);
	ssize_t r = fd < 0 ? -1 : read(fd, buf, n - 1);

	if (fd >= 0)
		close(fd);
	if (r < 0)
		r = 0;
	buf[r] = 0;
	buf[strcspn(buf, "\n")] = 0;
}

int main(int argc, char **argv)
{
	char last[MAXW][32], now[32], cmd[256];
	int n = (argc - 2) / 2, i, secs;

	if (argc < 4 || argc % 2 || n > MAXW || (secs = atoi(argv[1])) <= 0) {
		fprintf(stderr, "usage: watchd SECONDS FILE COMMAND [FILE COMMAND]...\n");
		return 2;
	}
	signal(SIGCHLD, SIG_DFL);
	for (i = 0; i < n; i++)
		readline(argv[2 + 2 * i], last[i], sizeof(last[i]));
	for (;;) {
		sleep(secs);
		for (i = 0; i < n; i++) {
			pid_t pid;

			readline(argv[2 + 2 * i], now, sizeof(now));
			if (!strcmp(now, last[i]))
				continue;
			strcpy(last[i], now);
			snprintf(cmd, sizeof(cmd), "%s '%s'", argv[3 + 2 * i], now);
			pid = vfork();
			if (pid == 0) {
				execl("/bin/sh", "sh", "-c", cmd, (char *)0);
				_exit(127);
			}
			if (pid > 0)
				while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
					;
		}
	}
}
