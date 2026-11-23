/*
 * selftest: checks the ATRANS toolchain and the ATRANS v2 process model
 * through musl: layout and auxv, malloc, fork, exec, vfork, posix_spawn
 * (clone with CLONE_VM on a new stack), threads and TLS, signals,
 * setjmp, mmap, pipes, ptys, 64-bit atomics, floating point.
 *
 *   selftest            all tests
 *   selftest NAME...    only those (layout auxv malloc fork exec vfork spawn
 *                       threads signals jmp mmap pipe pty atomics float time)
 *
 * One PASS/FAIL/SKIP line per test, then a summary. Exit status: failures.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WINDOW_LO 0x11000000UL
#define WINDOW_HI 0x11400000UL
#define TEXT_LO 0x11C00000UL	/* v3: shared text window */
#define TEXT_HI 0x12000000UL

extern char **environ;
extern char __executable_start[], etext[], _end[];

static int passed, failed, skipped;
static const char *self = "/proc/self/exe";

static void result(int ok, const char *name, const char *fmt, ...)
{
	va_list ap;

	printf("%s %s", ok > 0 ? "PASS" : ok == 0 ? "FAIL" : "SKIP", name);
	if (fmt && *fmt) {
		printf(": ");
		va_start(ap, fmt);
		vprintf(fmt, ap);
		va_end(ap);
	}
	printf("\n");
	fflush(stdout);
	if (ok > 0)
		passed++;
	else if (ok == 0)
		failed++;
	else
		skipped++;
}

/* ---- layout: v2 image in the window, stack below it, heap above; v3 the
 * same with text and rodata in the text window ---- */
static void t_layout(void)
{
	int local;
	uintptr_t sp = (uintptr_t)&local, img = (uintptr_t)__executable_start;
	uintptr_t etx = (uintptr_t)etext, dat = (uintptr_t)&passed;
	uintptr_t end = (uintptr_t)_end, brk0 = (uintptr_t)sbrk(0);
	int v3 = img >= TEXT_LO;
	int inwin = v3 ? etx <= TEXT_HI && dat >= WINDOW_LO && end <= WINDOW_HI
		       : img >= WINDOW_LO && end <= WINDOW_HI;
	int stack_below = sp >= WINDOW_LO && sp < (v3 ? dat : img);

	result(inwin, v3 ? "layout image (v3)" : "layout image (v2)",
	       "text %#lx-%#lx, data..end %#lx", (unsigned long)img, (unsigned long)etx,
	       (unsigned long)end);
	/* qemu-user puts the stack where it likes; only the board must match */
	result(stack_below ? 1 : -1, "layout stack below data", "sp %#lx", (unsigned long)sp);
	result(brk0 >= end ? 1 : 0, "layout brk after image", "brk %#lx", (unsigned long)brk0);
}

/* ---- auxv ---- */
static void t_auxv(void)
{
	unsigned long pg = getauxval(AT_PAGESZ), hw = getauxval(AT_HWCAP);
	unsigned long rnd = getauxval(AT_RANDOM), entry = getauxval(AT_ENTRY);
	unsigned long phdr = getauxval(AT_PHDR);

	result(pg == 4096, "auxv AT_PAGESZ", "%lu", pg);
	result(rnd != 0, "auxv AT_RANDOM", "%#lx", rnd);
	result(entry >= (uintptr_t)__executable_start && entry < (uintptr_t)etext,
	       "auxv AT_ENTRY", "%#lx", entry);
	result(phdr >= WINDOW_LO && phdr < WINDOW_HI, "auxv AT_PHDR", "%#lx", phdr);
	/* the board says 0; qemu-user reports its Cortex-A's features */
	result(hw == 0 ? 1 : -1, "auxv AT_HWCAP == 0", "%#lx", hw);
}

/* ---- malloc: small (brk) and large (mmap) ---- */
static void t_malloc(void)
{
	char *small[64], *big;
	int ok = 1, i;

	for (i = 0; i < 64; i++) {
		small[i] = malloc(32 + i * 16);
		if (!small[i]) { ok = 0; break; }
		memset(small[i], i, 32 + i * 16);
	}
	for (i = 0; ok && i < 64; i++)
		ok = small[i][31] == i;
	for (i = 0; i < 64 && small[i]; i++)
		free(small[i]);
	result(ok, "malloc small", NULL);
	big = malloc(512 * 1024);
	if (big) {
		memset(big, 0x5a, 512 * 1024);
		ok = big[512 * 1024 - 1] == 0x5a;
		free(big);
	}
	result(big && ok, "malloc 512 KiB", "%p", (void *)big);
	big = calloc(1, 64 * 1024);
	ok = big != NULL;
	for (i = 0; ok && i < 64 * 1024; i++)
		ok = big[i] == 0;
	free(big);
	result(ok, "calloc zeroed", NULL);
}

/* ---- fork ---- */
static int g_data = 111;

static void t_fork(void)
{
	int local = 5, st = 0, i, ok = 1;
	char *heap = malloc(4096);
	pid_t pid, kids[4];

	strcpy(heap, "parent");
	pid = fork();
	if (pid == 0) {
		g_data = 222;
		local = 6;
		strcpy(heap, "child");
		_exit(g_data == 222 && local == 6 && !strcmp(heap, "child") ? 42 : 1);
	}
	if (pid < 0) {
		result(0, "fork", "%s", strerror(errno));
		return;
	}
	waitpid(pid, &st, 0);
	result(WIFEXITED(st) && WEXITSTATUS(st) == 42 && g_data == 111 && local == 5 &&
	       !strcmp(heap, "parent"), "fork: same addresses, separate memory",
	       "status %#x g_data %d", st, g_data);
	free(heap);
	for (i = 0; i < 4; i++) {
		kids[i] = fork();
		if (kids[i] == 0) {
			static char bss[16384];
			memset(bss, i + 1, sizeof bss);
			usleep(20000);
			_exit(bss[16383] == i + 1 ? 10 + i : 1);
		}
	}
	for (i = 0; i < 4; i++) {
		if (kids[i] < 0 || waitpid(kids[i], &st, 0) != kids[i] || WEXITSTATUS(st) != 10 + i)
			ok = 0;
	}
	result(ok, "fork: 4 concurrent children", NULL);
}

/* ---- exec ---- */
static void t_exec(void)
{
	int st = 0;
	pid_t pid = fork();

	if (pid == 0) {
		execl(self, "selftest", "--child", "exec", (char *)0);
		_exit(127);
	}
	waitpid(pid, &st, 0);
	result(WIFEXITED(st) && WEXITSTATUS(st) == 33, "fork + execve + wait", "status %#x", st);
	pid = fork();
	if (pid == 0) {
		execl("/bin/sh", "sh", "-c", "exit 7", (char *)0);
		_exit(127);
	}
	waitpid(pid, &st, 0);
	result(WIFEXITED(st) && WEXITSTATUS(st) == 7, "fork + exec /bin/sh", "status %#x", st);
}

/* ---- vfork ---- */
static void t_vfork(void)
{
	int st = 0;
	pid_t pid = vfork();

	if (pid == 0) {
		execl(self, "selftest", "--child", "exec", (char *)0);
		_exit(127);
	}
	if (pid < 0) {
		result(0, "vfork + exec", "%s", strerror(errno));
		return;
	}
	waitpid(pid, &st, 0);
	result(WIFEXITED(st) && WEXITSTATUS(st) == 33, "vfork + exec", "status %#x", st);
}

/* ---- posix_spawn, system, popen: musl uses clone(CLONE_VM|CLONE_VFORK) ---- */
static void t_spawn(void)
{
	char *argv[] = { "selftest", "--child", "exec", NULL };
	char buf[64] = "";
	int st = 0, r;
	pid_t pid;
	FILE *f;

	r = posix_spawn(&pid, self, NULL, NULL, argv, environ);
	if (r == 0)
		waitpid(pid, &st, 0);
	result(r == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 33, "posix_spawn",
	       "r %d status %#x", r, st);
	r = system("exit 9");
	result(WIFEXITED(r) && WEXITSTATUS(r) == 9, "system()", "%#x", r);
	f = popen("echo popen-ok", "r");
	if (f) {
		if (!fgets(buf, sizeof buf, f))
			buf[0] = 0;
		pclose(f);
	}
	result(f && !strcmp(buf, "popen-ok\n"), "popen()", "%s", buf);
}

/* ---- threads and TLS ---- */
static __thread int tls_var = 7;
static __thread char tls_buf[64];
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int counter, ready;

static void *thr(void *arg)
{
	int id = (int)(intptr_t)arg, i, ok = tls_var == 7;

	tls_var = 100 + id;
	snprintf(tls_buf, sizeof tls_buf, "thread %d", id);
	errno = 1000 + id;
	for (i = 0; i < 1000; i++) {
		pthread_mutex_lock(&mtx);
		counter++;
		pthread_mutex_unlock(&mtx);
		if (!(i & 127))
			sched_yield();
	}
	pthread_mutex_lock(&mtx);
	ready++;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mtx);
	ok = ok && tls_var == 100 + id && errno == 1000 + id;
	snprintf(tls_buf + 20, 20, "%d", id);
	return (void *)(intptr_t)(ok && atoi(tls_buf + 20) == id ? id + 1 : -1);
}

static void t_threads(void)
{
	pthread_t t[4];
	void *ret;
	int i, ok = 1, r;

	tls_var = 1;
	errno = 0;
	for (i = 0; i < 4; i++) {
		r = pthread_create(&t[i], NULL, thr, (void *)(intptr_t)i);
		if (r) {
			result(0, "pthread_create", "%s", strerror(r));
			return;
		}
	}
	pthread_mutex_lock(&mtx);
	while (ready < 4)
		pthread_cond_wait(&cond, &mtx);
	pthread_mutex_unlock(&mtx);
	for (i = 0; i < 4; i++) {
		pthread_join(t[i], &ret);
		if ((intptr_t)ret != i + 1)
			ok = 0;
	}
	result(ok, "pthread create/join, TLS and errno per thread", NULL);
	result(counter == 4000 && tls_var == 1, "mutex, condvar, main thread's TLS kept",
	       "counter %d tls %d", counter, tls_var);
	result(pthread_self() != 0 && (uintptr_t)pthread_self() >= WINDOW_LO ? 1 : -1,
	       "thread pointer in the window", "%p", (void *)pthread_self());
}

/* ---- signals and setjmp ---- */
static sigjmp_buf sjb;
static volatile sig_atomic_t got;

static void on_sig(int s)
{
	got = s;
}

static void on_segv(int s)
{
	siglongjmp(sjb, s);
}

static void t_signals(void)
{
	struct sigaction sa = { .sa_handler = on_sig };
	volatile int *bad = (int *)8;
	int st = 0;
	pid_t pid;

	sigaction(SIGUSR1, &sa, NULL);
	raise(SIGUSR1);
	result(got == SIGUSR1, "signal handler", NULL);
	sa.sa_handler = on_segv;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	if (sigsetjmp(sjb, 1) == 0) {
		(void)*bad;
		result(0, "SIGSEGV on a bad load, siglongjmp out", "no fault");
	} else {
		result(1, "SIGSEGV on a bad load, siglongjmp out", NULL);
	}
	signal(SIGSEGV, SIG_DFL);
	signal(SIGBUS, SIG_DFL);
	pid = fork();
	if (pid == 0) {
		pause();
		_exit(0);
	}
	usleep(50000);
	kill(pid, SIGTERM);
	waitpid(pid, &st, 0);
	result(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "kill a child", "status %#x", st);
}

static jmp_buf jb;

static void jump(int n)
{
	longjmp(jb, n);
}

static void t_jmp(void)
{
	volatile int n = 0;
	int r = setjmp(jb);

	if (r < 3) {
		n++;
		jump(r + 1);
	}
	result(r == 3 && n == 3, "setjmp/longjmp", "r %d n %d", r, n);
}

/* ---- mmap ---- */
static void t_mmap(void)
{
	size_t len = 64 * 1024;
	char *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	char buf[32];
	int ok, fd;

	ok = p != MAP_FAILED;
	if (ok) {
		ok = p[0] == 0 && p[len - 1] == 0;
		memset(p, 1, len);
		ok = ok && munmap(p, len) == 0;
	}
	result(ok, "mmap anonymous 64 KiB", "%p", (void *)p);
	fd = open(self, O_RDONLY);
	p = fd >= 0 ? mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
	ok = p != MAP_FAILED && pread(fd, buf, 4, 0) == 4 && !memcmp(p, buf, 4) && !memcmp(p, "\177ELF", 4);
	if (p != MAP_FAILED)
		munmap(p, 4096);
	if (fd >= 0)
		close(fd);
	result(ok, "mmap a file", NULL);
}

/* ---- pipes ---- */
static void t_pipe(void)
{
	int a[2], b[2], st = 0, i, ok = 1;
	char c;
	struct pollfd pfd;
	pid_t pid;

	if (pipe(a) || pipe(b)) {
		result(0, "pipe", "%s", strerror(errno));
		return;
	}
	pid = fork();
	if (pid == 0) {
		close(a[1]);
		close(b[0]);
		while (read(a[0], &c, 1) == 1) {
			c++;
			if (write(b[1], &c, 1) != 1)
				break;
		}
		_exit(0);
	}
	close(a[0]);
	close(b[1]);
	for (i = 0, c = 0; i < 50 && ok; i++) {
		char d = c;
		ok = write(a[1], &d, 1) == 1;
		pfd = (struct pollfd){ .fd = b[0], .events = POLLIN };
		ok = ok && poll(&pfd, 1, 2000) == 1 && read(b[0], &c, 1) == 1 && c == d + 1;
	}
	close(a[1]);
	waitpid(pid, &st, 0);
	close(b[0]);
	result(ok, "pipe ping-pong with a child, poll", NULL);
}

/* ---- ptys (tmux, dropbear) ---- */
static void t_pty(void)
{
	int m = posix_openpt(O_RDWR | O_NOCTTY), s = -1, ok = 0;
	char buf[16] = "";

	if (m < 0) {
		result(-1, "pty", "posix_openpt: %s", strerror(errno));
		return;
	}
	if (grantpt(m) == 0 && unlockpt(m) == 0)
		s = open(ptsname(m), O_RDWR | O_NOCTTY);
	if (s >= 0 && write(s, "pty\n", 4) == 4) {
		usleep(10000);
		ok = read(m, buf, sizeof buf - 1) > 0 && !strncmp(buf, "pty", 3);
	}
	result(ok, "pty open and echo", "%s", s >= 0 ? ptsname(m) : strerror(errno));
	if (s >= 0)
		close(s);
	close(m);
}

/* ---- atomics: 32-bit inline, 64-bit through libgcc (no kuser helpers) ---- */
static void t_atomics(void)
{
	static volatile long long v64 = 1;
	static atomic_int a32;
	int old;

	atomic_store(&a32, 5);
	old = atomic_fetch_add(&a32, 3);
	result(old == 5 && atomic_load(&a32) == 8, "atomics 32-bit (ldrex/strex)", NULL);
	__sync_fetch_and_add(&v64, 0x100000000LL);
	result(__sync_val_compare_and_swap(&v64, 0x100000001LL, 42) == 0x100000001LL && v64 == 42,
	       "atomics 64-bit (__sync, libgcc)", "%lld", (long long)v64);
}

/* ---- soft-float ---- */
static void t_float(void)
{
	volatile double x = 2.0;
	char buf[32];

	snprintf(buf, sizeof buf, "%.6f %g", sqrt(x), strtod("1e-3", NULL) * 1000);
	result(!strcmp(buf, "1.414214 1"), "soft-float libm and printf", "%s", buf);
}

/* ---- time ---- */
static void t_time(void)
{
	struct timespec a, b;

	clock_gettime(CLOCK_MONOTONIC, &a);
	usleep(20000);
	clock_gettime(CLOCK_MONOTONIC, &b);
	long us = (b.tv_sec - a.tv_sec) * 1000000 + (b.tv_nsec - a.tv_nsec) / 1000;
	result(us >= 19000 && us < 2000000, "clock_gettime, usleep", "%ld us", us);
}

static const struct { const char *name; void (*fn)(void); } tests[] = {
	{ "layout", t_layout }, { "auxv", t_auxv }, { "malloc", t_malloc },
	{ "fork", t_fork }, { "exec", t_exec }, { "vfork", t_vfork },
	{ "spawn", t_spawn }, { "threads", t_threads }, { "signals", t_signals },
	{ "jmp", t_jmp }, { "mmap", t_mmap }, { "pipe", t_pipe }, { "pty", t_pty },
	{ "atomics", t_atomics }, { "float", t_float }, { "time", t_time },
};

int main(int argc, char **argv)
{
	unsigned i;
	int j;

	if (argc >= 3 && !strcmp(argv[1], "--child"))
		return !strcmp(argv[2], "exec") ? 33 : 1;
	if (access(self, X_OK) != 0)
		self = argv[0];
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (i = 0; i < sizeof tests / sizeof tests[0]; i++) {
		if (argc > 1) {
			for (j = 1; j < argc; j++)
				if (!strcmp(argv[j], tests[i].name))
					break;
			if (j == argc)
				continue;
		}
		tests[i].fn();
	}
	printf("selftest: %s (%d passed, %d failed, %d skipped)\n", failed ? "FAIL" : "PASS",
	       passed, failed, skipped);
	return failed;
}
