// SPDX-License-Identifier: MIT
/*
 * forktest: exercises per-process ATRANS address spaces (rp2350-atrans.c),
 * ABI "ATRANS v2" (docs/atrans.md). Freestanding (raw
 * syscalls, no libc), linked at 0x11020000 (atrans.ld): the stack is the
 * 128 KiB below the image.
 *
 *   forktest          all tests
 *   forktest NAME...  only the named tests (see the table at the end)
 *   forktest sleep N  sleep N seconds (to look at it with ps)
 *
 * Every test prints PASS or FAIL; the last line is a summary.
 */
typedef unsigned long ul;
typedef unsigned long long u64;

#define SYS_exit		1
#define SYS_fork		2
#define SYS_read		3
#define SYS_write		4
#define SYS_open		5
#define SYS_close		6
#define SYS_execve		11
#define SYS_getpid		20
#define SYS_kill		37
#define SYS_pipe		42
#define SYS_brk			45
#define SYS_munmap		91
#define SYS_wait4		114
#define SYS_writev		146
#define SYS_clone		120
#define SYS_sched_yield		158
#define SYS_rt_sigreturn	173
#define SYS_rt_sigaction	174
#define SYS_vfork		190
#define SYS_mmap2		192
#define SYS_gettid		224
#define SYS_exit_group		248
#define SYS_clock_gettime64	403	/* the 32-bit-time ones need COMPAT_32BIT_TIME */
#define SYS_clock_nanosleep64	407
#define SYS_futex_time64	422
#define SYS_set_tls		0xf0005
#define SYS_get_tls		0xf0006

#define CLONE_VM		0x00000100
#define CLONE_FS		0x00000200
#define CLONE_FILES		0x00000400
#define CLONE_SIGHAND		0x00000800
#define CLONE_VFORK		0x00004000
#define CLONE_THREAD		0x00010000
#define CLONE_SYSVSEM		0x00040000
#define CLONE_SETTLS		0x00080000
#define CLONE_PARENT_SETTID	0x00100000
#define CLONE_CHILD_CLEARTID	0x00200000
#define SIGCHLD			17

#define WINDOW	0x11000000UL
#define WEND	0x11400000UL
#define TPWORD	(*(volatile ul *)WINDOW)	/* thread pointer, read-only */

static long sc(long n, long a, long b, long c, long d)
{
	register long r7 __asm__("r7") = n;
	register long r0 __asm__("r0") = a;
	register long r1 __asm__("r1") = b;
	register long r2 __asm__("r2") = c;
	register long r3 __asm__("r3") = d;

	__asm__ volatile("svc 0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2), "r"(r3) : "memory");
	return r0;
}

static long sc6(long n, long a, long b, long c, long d, long e, long f)
{
	register long r7 __asm__("r7") = n;
	register long r0 __asm__("r0") = a;
	register long r1 __asm__("r1") = b;
	register long r2 __asm__("r2") = c;
	register long r3 __asm__("r3") = d;
	register long r4 __asm__("r4") = e;
	register long r5 __asm__("r5") = f;

	__asm__ volatile("svc 0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
			 : "memory");
	return r0;
}

#define PROT_READ	1
#define PROT_WRITE	2
#define MAP_SHARED	0x01
#define MAP_PRIVATE	0x02
#define MAP_FIXED	0x10
#define MAP_ANONYMOUS	0x20

static long mmap(ul addr, ul len, int prot, int flags, int fd, ul off)
{
	return sc6(SYS_mmap2, addr, len, prot, flags, fd, off >> 12);
}

static long anon(ul len)
{
	return mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

void *memset(void *d, int c, ul n)
{
	unsigned char *p = d;

	while (n--)
		*p++ = c;
	return d;
}

void *memcpy(void *d, const void *s, ul n)
{
	unsigned char *p = d;
	const unsigned char *q = s;

	while (n--)
		*p++ = *q++;
	return d;
}

static ul slen(const char *s)
{
	ul n = 0;

	while (s[n])
		n++;
	return n;
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static int memeq(const char *a, const char *b, ul n)
{
	while (n--)
		if (*a++ != *b++)
			return 0;
	return 1;
}

static int contains(const char *h, ul hn, const char *n)
{
	ul nl = slen(n), i;

	for (i = 0; i + nl <= hn; i++)
		if (memeq(h + i, n, nl))
			return 1;
	return 0;
}

static void out(const char *s) { sc(SYS_write, 1, (long)s, slen(s), 0); }

static void outn(ul v, int base)
{
	char b[12], *p = b + sizeof(b);

	*--p = 0;
	do {
		*--p = "0123456789abcdef"[v % base];
		v /= base;
	} while (v);
	if (base == 16)
		out("0x");
	out(p);
}

static void outd(long v)
{
	if (v < 0) {
		out("-");
		v = -v;
	}
	outn(v, 10);
}

static char *fmtn(char *p, ul v)
{
	char b[12], *q = b + sizeof(b);

	*--q = 0;
	do {
		*--q = '0' + v % 10;
		v /= 10;
	} while (v);
	while (*q)
		*p++ = *q++;
	*p = 0;
	return p;
}

static void __attribute__((noreturn)) die(int code)
{
	sc(SYS_exit_group, code, 0, 0, 0);
	for (;;)
		;
}

static ul now_us(void)
{
	long long ts[2] = { 0, 0 };

	if (sc(SYS_clock_gettime64, 1 /* CLOCK_MONOTONIC */, (long)ts, 0, 0))
		out("  clock_gettime64 failed\n");
	return (ul)ts[0] * 1000000 + (ul)ts[1] / 1000;
}

static void sleep_us(ul us)
{
	long long ts[2] = { us / 1000000, (us % 1000000) * 1000 };

	sc(SYS_clock_nanosleep64, 1, 0, (long)ts, 0);
}

static int wait_child(long pid)
{
	int status = -1;

	sc(SYS_wait4, pid, (long)&status, 0, 0);
	return status;
}

static long get_tls(void) { return sc(SYS_get_tls, 0, 0, 0, 0); }
static void set_tls(ul v) { sc(SYS_set_tls, v, 0, 0, 0); }

static int passed, failed;

static void result(const char *name, int ok)
{
	out(ok ? "PASS " : "FAIL ");
	out(name);
	out("\n");
	if (ok)
		passed++;
	else
		failed++;
}

static void status_line(const char *what, int st)
{
	out("  ");
	out(what);
	if ((st & 0x7f) == 0) {
		out(": exit ");
		outn((st >> 8) & 0xff, 10);
	} else {
		out(": signal ");
		outn(st & 0x7f, 10);
	}
	out("\n");
}

/* ---- startup state ---- */
static long *g_sp0;			/* sp at _start */
static volatile ul g_data = 111;
static volatile ul g_big[16 * 1024 / 4];	/* bss */
extern char __image_start[];		/* atrans.ld */
void _start(void);

#define AT_NULL		0
#define AT_PHDR		3
#define AT_PHENT	4
#define AT_PHNUM	5
#define AT_PAGESZ	6
#define AT_ENTRY	9
#define AT_HWCAP	16
#define AT_RANDOM	25
#define AT_EXECFN	31

struct phdr { ul type, offset, vaddr, paddr, filesz, memsz, flags, align; };

static char *g_self = "/usr/bin/forktest";	/* AT_EXECFN */

/* The initial stack, from the kernel: returns a failure bitmap (0: good). */
static ul check_startup(int verbose)
{
	long argc = g_sp0[0];
	char **argv = (char **)(g_sp0 + 1), **envp = argv + argc + 1;
	ul *av, phdr = 0, phent = 0, phnum = 0, pagesz = 0, entry = 0, hwcap = 1, rnd = 0;
	ul bad = 0, i, n;
	const struct phdr *ph;

	for (av = (ul *)envp; *av; av++)
		;
	for (av++, n = 0; av[0] != AT_NULL && n < 64; av += 2, n++) {
		switch (av[0]) {
		case AT_PHDR: phdr = av[1]; break;
		case AT_PHENT: phent = av[1]; break;
		case AT_PHNUM: phnum = av[1]; break;
		case AT_PAGESZ: pagesz = av[1]; break;
		case AT_ENTRY: entry = av[1]; break;
		case AT_HWCAP: hwcap = av[1]; break;
		case AT_RANDOM: rnd = av[1]; break;
		case AT_EXECFN: g_self = (char *)av[1]; break;
		}
	}
	if ((ul)g_sp0 & 7)
		bad |= 1;			/* sp 8-aligned */
	if ((ul)g_sp0 < WINDOW || (ul)g_sp0 >= (ul)__image_start ||
	    (ul)__image_start - (ul)g_sp0 > 4096)
		bad |= 2;			/* just below the image */
	if (pagesz != 4096)
		bad |= 4;
	if (phent != sizeof(struct phdr) || !phnum || phdr < WINDOW || phdr >= WEND)
		bad |= 8;
	else {
		ph = (const struct phdr *)phdr;
		for (i = 0; i < phnum; i++)
			if (ph[i].type == 1 /* PT_LOAD */ && ph[i].vaddr <= (ul)__image_start &&
			    (ul)__image_start - ph[i].vaddr < ph[i].memsz)
				break;	/* the image's segment (v3: it starts with the ELF headers) */
		if (i == phnum)
			bad |= 16;		/* AT_PHDR points at our headers */
	}
	if ((entry | 1) != ((ul)_start | 1))
		bad |= 32;
	if (hwcap != 0)
		bad |= 64;
	if (rnd < (ul)g_sp0 || rnd + 16 > (ul)__image_start)
		bad |= 128;			/* 16 bytes on the stack */
	else {
		for (i = 0; i < 16 && !((unsigned char *)rnd)[i]; i++)
			;
		if (i == 16)
			bad |= 256;		/* all zero: not random */
	}
	if (av[0] != AT_NULL)
		bad |= 512;
	if (verbose) {
		out("  sp ");
		outn((ul)g_sp0, 16);
		out(", image ");
		outn((ul)__image_start, 16);
		out(", ");
		outn(n, 10);
		out(" auxv entries, AT_PHDR ");
		outn(phdr, 16);
		out(" (");
		outn(phnum, 10);
		out("), AT_ENTRY ");
		outn(entry, 16);
		out(", AT_RANDOM ");
		outn(rnd, 16);
		out("; failures ");
		outn(bad, 16);
		out("\n");
	}
	return bad;
}

static void t_startup(void)
{
	volatile ul local = 0;

	out("  pid ");
	outn(sc(SYS_getpid, 0, 0, 0, 0), 10);
	out("  data ");
	outn((ul)&g_data, 16);
	out("  bss ");
	outn((ul)g_big, 16);
	out("  stack ");
	outn((ul)&local, 16);
	out("  brk ");
	outn(sc(SYS_brk, 0, 0, 0, 0), 16);
	out("\n");
	result("startup: stack below the image, argc/argv/envp/auxv",
	       !check_startup(1) && (ul)&local < (ul)__image_start && (ul)&local > WINDOW);
}

/* Parent and child change the same variables at the same addresses. */
static void t_isolation(void)
{
	volatile ul stackvar = 5;
	long pid;
	int i, ok = 1, st;

	g_data = 111;
	pid = sc(SYS_fork, 0, 0, 0, 0);
	if (pid < 0) {
		out("  fork returned ");
		outd(pid);
		out("\n");
		result("fork", 0);
		return;
	}
	if (pid == 0) {
		g_data = 222;
		stackvar = 6;
		for (i = 0; i < 20; i++) {
			sc(SYS_sched_yield, 0, 0, 0, 0);
			if (g_data != 222 || stackvar != 6)
				die(1);
		}
		die(42);
	}
	for (i = 0; i < 20; i++) {
		sc(SYS_sched_yield, 0, 0, 0, 0);
		if (g_data != 111 || stackvar != 5)
			ok = 0;
	}
	st = wait_child(pid);
	status_line("child", st);
	result("fork: same addresses, separate memory (data, stack)", ok && st == 42 << 8);
}

/* Several children at once, each filling the same 16 KiB bss array. */
static void t_many(void)
{
	enum { N = 4 };
	long pids[N];
	int i, k, bad = 0, st;

	for (k = 0; k < (int)(sizeof(g_big) / 4); k++)
		g_big[k] = 0xaaaa0000UL | k;
	for (i = 0; i < N; i++) {
		pids[i] = sc(SYS_fork, 0, 0, 0, 0);
		if (pids[i] == 0) {
			ul tag = (ul)(i + 1) << 24;
			int round;

			for (k = 0; k < (int)(sizeof(g_big) / 4); k++)
				g_big[k] = tag | k;
			for (round = 0; round < 10; round++) {
				sc(SYS_sched_yield, 0, 0, 0, 0);
				for (k = 0; k < (int)(sizeof(g_big) / 4); k++)
					if (g_big[k] != (tag | k))
						die(1);
			}
			die(0);
		}
	}
	for (i = 0; i < N; i++) {
		st = wait_child(pids[i]);
		if (st != 0)
			bad++;
	}
	for (k = 0; k < (int)(sizeof(g_big) / 4); k++)
		if (g_big[k] != (0xaaaa0000UL | k))
			bad++;
	result("4 concurrent children, each with its own 16 KiB bss", !bad);
}

/* The heap is in the block too: it is copied by fork. */
static void t_heap(void)
{
	ul base = sc(SYS_brk, 0, 0, 0, 0), top;
	volatile ul *h = (volatile ul *)base;
	long pid;
	int i, st;

	top = sc(SYS_brk, base + 32 * 1024, 0, 0, 0);
	if (top != base + 32 * 1024) {
		result("brk grows by 32 KiB", 0);
		return;
	}
	for (i = 0; i < 8 * 1024; i++)
		h[i] = 0x12340000UL | i;
	pid = sc(SYS_fork, 0, 0, 0, 0);
	if (pid == 0) {
		for (i = 0; i < 8 * 1024; i++)
			if (h[i] != (0x12340000UL | i))
				die(1);
		for (i = 0; i < 8 * 1024; i++)
			h[i] = 0;
		die(0);
	}
	st = wait_child(pid);
	for (i = 0; i < 8 * 1024; i++)
		if (h[i] != (0x12340000UL | i))
			st = -1;
	sc(SYS_brk, base, 0, 0, 0);
	result("brk heap copied by fork, child's writes stay in the child", st == 0);
}

/* Growth: brk in steps to 1.5 MiB (required) and on to 3.5 MiB (as far as
 * free memory allows); what was written before each step is still there
 * after it (the block moved, the addresses did not). */
static void t_grow(void)
{
	static const ul steps[] = { 256 << 10, 512 << 10, 1 << 20, 1536 << 10,
				    2 << 20, 3 << 20, 3584 << 10 };
	enum { REQUIRED = 4 };
	ul base = sc(SYS_brk, 0, 0, 0, 0), prev = base, t0, dt, i, k;
	volatile ul *h = (volatile ul *)base;
	volatile ul stackvar = 0x5ac5ac;
	int bad = 0;

	for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		ul want = base + steps[i];

		t0 = now_us();
		if ((ul)sc(SYS_brk, want, 0, 0, 0) != want) {
			out("  brk to ");
			outn(steps[i] >> 10, 10);
			out(" KiB: ENOMEM\n");
			if (i < REQUIRED)
				bad |= 1;
			break;
		}
		dt = now_us() - t0;
		for (k = 0; k < (prev - base) / 4; k += 1024)
			if (h[k] != (0x77000000UL | k))
				bad |= 2;
		for (k = (prev - base) / 4; k < steps[i] / 4; k++)
			if (h[k])
				bad |= 4;		/* new heap zeroed */
		for (k = (prev - base) / 4; k < steps[i] / 4; k++)
			h[k] = 0x77000000UL | k;
		out("  brk to ");
		outn(steps[i] >> 10, 10);
		out(" KiB: ");
		outn(dt, 10);
		out(" us\n");
		prev = want;
	}
	for (k = 0; k < (prev - base) / 4; k++)
		if (h[k] != (0x77000000UL | k))
			bad |= 8;
	if (stackvar != 0x5ac5ac)
		bad |= 16;
	sc(SYS_brk, base, 0, 0, 0);
	result("block grows with brk (to 1.5 MiB at least), contents kept", !bad);
}

/* fork cost against the memory in use (heap touched, so it is copied) */
static void t_fork_size(void)
{
	static const ul sizes[] = { 0, 64 << 10, 256 << 10, 512 << 10, 1 << 20 };
	ul base = sc(SYS_brk, 0, 0, 0, 0), i, k, t0, dt;
	int bad = 0, r;

	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		enum { N = 5 };

		if ((ul)sc(SYS_brk, base + sizes[i], 0, 0, 0) != base + sizes[i]) {
			bad = 1;
			break;
		}
		for (k = 0; k < sizes[i]; k += 4096)
			*(volatile ul *)(base + k) = k;
		t0 = now_us();
		for (r = 0; r < N; r++) {
			long pid = sc(SYS_fork, 0, 0, 0, 0);

			if (pid == 0)
				die(0);
			if (pid < 0 || wait_child(pid) != 0)
				bad = 1;
		}
		dt = (now_us() - t0) / N;
		out("  heap ");
		outn(sizes[i] >> 10, 10);
		out(" KiB: fork + exit + wait ");
		outn(dt, 10);
		out(" us\n");
	}
	sc(SYS_brk, base, 0, 0, 0);
	result("fork time against process size", !bad);
}

/* An access past the block faults: the child dies by a signal. */
static void t_oob(void)
{
	long pid = sc(SYS_fork, 0, 0, 0, 0);
	int st;

	if (pid == 0) {
		(void)*(volatile ul *)(WEND - 4096);
		die(0);
	}
	st = wait_child(pid);
	status_line("load from 0x113ff000 (window, past the block)", st);
	result("load past the block kills only the child (signal)", (st & 0x7f) != 0);
}

/* The MPU confines user mode to the block: a load from anywhere else is a
 * MemManage fault and the child dies by SIGSEGV. */
static void t_mpu(void)
{
	static const struct { ul addr; const char *what; } t[] = {
		{ 0x11400000, "kernel RAM" },
		{ 0x400b0028, "timer register" },
		{ 0x20080000, "SRAM (console ring)" },
		{ 0x10000000, "flash" },
	};
	unsigned i;
	int bad = 0;

	for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		long pid = sc(SYS_fork, 0, 0, 0, 0);
		int st;

		if (pid == 0) {
			(void)*(volatile ul *)t[i].addr;
			die(0);
		}
		st = wait_child(pid);
		out("  load from ");
		outn(t[i].addr, 16);
		out(" (");
		out(t[i].what);
		out("): ");
		if ((st & 0x7f) == 11) {
			out("SIGSEGV\n");
		} else {
			out("status ");
			outn(st, 16);
			out("\n");
			bad++;
		}
	}
	result("MPU: user mode reaches only its block", !bad);
}

/* Syscalls refuse pointers outside the block (access_ok, get_user,
 * put_user); stock nommu would use them as they are. */
static void t_efault(void)
{
	char *argv[] = { "/bin/echo", 0 };
	char *envp[] = { 0 };
	long r1, r2, r3, r4, r5, z1, z2, z3, pid;
	ul iov[4] = { (ul)"ok", 2, 0, 0 };	/* musl's stdio flush: { data }, { NULL, 0 } */

	r1 = sc(SYS_write, 1, 0x11400000, 8, 0);			/* copy_from_user */
	r2 = sc(SYS_write, 1, WEND - 4, 8, 0);				/* straddles the end */
	r3 = sc(SYS_execve, (long)argv[0], 0x11400000, (long)envp, 0);	/* get_user */
	pid = sc(SYS_fork, 0, 0, 0, 0);
	if (pid == 0)
		die(0);
	r4 = sc(SYS_wait4, pid, 0x11bffffc, 0, 0);			/* put_user */
	r5 = sc(SYS_read, 0, WINDOW, 4, 0);				/* the TP word */
	/* zero bytes at any pointer are no access: stock Linux returns 0 */
	z1 = sc(SYS_write, 1, 0, 0, 0);
	z2 = sc(SYS_read, 0, 0, 0, 0);
	z3 = sc(SYS_writev, 1, (long)iov, 2, 0);			/* writes "ok" */
	out("  write(kernel ptr) ");
	outd(r1);
	out(", write(past window) ");
	outd(r2);
	out(", execve(kernel argv) ");
	outd(r3);
	out(", wait4(kernel status ptr) ");
	outd(r4);
	out(", read(into the TP word) ");
	outd(r5);
	out(" (-14 = EFAULT)\n  zero length at NULL: write ");
	outd(z1);
	out(", read ");
	outd(z2);
	out(", writev {ok,2},{NULL,0} ");
	outd(z3);
	out("\n");
	(void)argv;
	result("syscalls refuse pointers outside the block (EFAULT), allow zero lengths",
	       r1 == -14 && r2 == -14 && r3 == -14 && r4 == -14 && r5 == -14 &&
	       z1 == 0 && z2 == 0 && z3 == 2);
}

/* mmap: pages from the top of the block, zeroed or filled from the file;
 * brk stops below them; fork copies them; big ones grow the block. */
static void t_mmap(void)
{
	enum { LEN = 32 * 1024 };
	ul brk0 = sc(SYS_brk, 0, 0, 0, 0), b;
	long a, a2, pid, big, huge, shw;
	volatile ul *p;
	char buf[32], *f;
	int i, st, bad = 0, fd, n;

	a = anon(LEN);
	out("  mmap(32 KiB anon) = ");
	outn(a, 16);
	out(" (brk ");
	outn(brk0, 16);
	out(")\n");
	if (a < 0 || (ul)a < brk0 || (ul)a + LEN > WEND) {
		result("mmap: anonymous pages inside the block", 0);
		return;
	}
	p = (volatile ul *)a;
	for (i = 0; i < LEN / 4; i++)
		if (p[i])
			bad |= 1;			/* not zeroed */
	for (i = 0; i < LEN / 4; i++)
		p[i] = 0x55000000UL | i;
	b = sc(SYS_brk, a + 4096, 0, 0, 0);		/* into the mapping: refused */
	if (b != brk0)
		bad |= 2;
	pid = sc(SYS_fork, 0, 0, 0, 0);
	if (pid == 0) {
		for (i = 0; i < LEN / 4; i++)
			if (p[i] != (0x55000000UL | i))
				die(1);
		for (i = 0; i < LEN / 4; i++)
			p[i] = 0;
		die(0);
	}
	st = wait_child(pid);
	if (st != 0)
		bad |= 4;
	for (i = 0; i < LEN / 4; i++)
		if (p[i] != (0x55000000UL | i))
			bad |= 8;			/* child's writes leaked */
	if (sc(SYS_munmap, a, LEN, 0, 0) != 0)
		bad |= 16;
	a2 = anon(LEN);
	if (a2 < 0)
		bad |= 32;
	p = (volatile ul *)a2;
	for (i = 0; a2 >= 0 && i < LEN / 4; i++)
		if (p[i])
			bad |= 64;		/* reused pages zeroed again */
	sc(SYS_munmap, a2, LEN, 0, 0);

	/* a file, privately */
	fd = sc(SYS_open, (long)"/etc/issue", 0, 0, 0);
	n = sc(SYS_read, fd, (long)buf, sizeof(buf), 0);
	a = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
	sc(SYS_close, fd, 0, 0, 0);
	f = (char *)a;
	if (a < 0 || n <= 0)
		bad |= 128;
	else
		for (i = 0; i < n; i++)
			if (f[i] != buf[i])
				bad |= 128;
	if (a >= 0)
		sc(SYS_munmap, a, 4096, 0, 0);

	/* 1 MiB grows the block; 5 MiB can never fit */
	big = anon(1024 * 1024);
	if (big < 0)
		bad |= 256;
	else {
		p = (volatile ul *)big;
		for (i = 0; i < 1024 * 1024 / 4; i += 1024)
			p[i] = i;
		for (i = 0; i < 1024 * 1024 / 4; i += 1024)
			if (p[i] != (ul)i)
				bad |= 256;
		sc(SYS_munmap, big, 1024 * 1024, 0, 0);
	}
	huge = anon(5 * 1024 * 1024);
	shw = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (huge != -12 || shw != -22)
		bad |= 512;
	out("  1 MiB at ");
	outn(big, 16);
	out(", 5 MiB: ");
	outd(huge);
	out(", writable MAP_SHARED: ");
	outd(shw);
	out("; failures ");
	outn(bad, 16);
	out("\n");
	if (sc(SYS_brk, brk0 + 4096, 0, 0, 0) != brk0 + 4096)	/* room again once unmapped */
		bad |= 1024;
	sc(SYS_brk, brk0, 0, 0, 0);
	result("mmap: anon and file pages, copied by fork, growing the block", !bad);
}

/* fork + exec of a busybox (FDPIC) program from an ATRANS child. */
static void t_exec(void)
{
	long pid = sc(SYS_fork, 0, 0, 0, 0);
	int st;

	if (pid == 0) {
		char *argv[] = { "/bin/echo", "  hello from busybox, exec'd by an ATRANS child", 0 };
		char *envp[] = { 0 };

		sc(SYS_execve, (long)argv[0], (long)argv, (long)envp, 0);
		die(99);
	}
	st = wait_child(pid);
	result("fork + exec of a busybox program", st == 0);
}

/* fork + exec of an ATRANS program (this one), which checks its startup
 * stack, argv and environment */

static void t_exec_atrans(void)
{
	long pid = sc(SYS_fork, 0, 0, 0, 0);
	int st;

	if (pid == 0) {
		char *argv[] = { "forktest", "child-startup", "arg two", 0 };
		char *envp[] = { "FOO=bar", "HOME=/", 0 };

		sc(SYS_execve, (long)g_self, (long)argv, (long)envp, 0);
		die(99);
	}
	st = wait_child(pid);
	status_line("exec'd forktest child-startup", st);
	result("fork + exec of an ATRANS program (argv, envp, auxv)", st == 0);
}

static int child_startup(int argc, char **argv)
{
	char **envp = argv + argc + 1;
	ul bad = check_startup(0);

	if (argc != 3 || !streq(argv[2], "arg two"))
		bad |= 0x1000;
	if (!envp[0] || !streq(envp[0], "FOO=bar") || !envp[1] || !streq(envp[1], "HOME=/") || envp[2])
		bad |= 0x2000;
	if (bad) {
		out("  child-startup failures ");
		outn(bad, 16);
		out("\n");
	}
	return bad ? 1 : 0;
}

static void t_fork_time(void)
{
	enum { N = 20 };
	ul t0 = now_us(), dt;
	int i, bad = 0;

	for (i = 0; i < N; i++) {
		long pid = sc(SYS_fork, 0, 0, 0, 0);

		if (pid == 0)
			die(0);
		if (wait_child(pid) != 0)
			bad++;
	}
	dt = now_us() - t0;
	out("  fork + exit + wait: ");
	outn(dt / N, 10);
	out(" us each (");
	outn(N, 10);
	out(" rounds)\n");
	result("fork timing", !bad);
}

/* Two ATRANS processes passing a byte back and forth: every hop is a
 * switch between blocks (flush + ATRANS write). */
static void t_pingpong(void)
{
	enum { N = 500 };
	int a[2], b[2], i, st;
	char c = 'x';
	long pid;
	ul t0, dt;

	sc(SYS_pipe, (long)a, 0, 0, 0);
	sc(SYS_pipe, (long)b, 0, 0, 0);
	pid = sc(SYS_fork, 0, 0, 0, 0);
	if (pid == 0) {
		for (i = 0; i < N; i++) {
			if (sc(SYS_read, a[0], (long)&c, 1, 0) != 1)
				die(1);
			sc(SYS_write, b[1], (long)&c, 1, 0);
		}
		die(0);
	}
	t0 = now_us();
	for (i = 0; i < N; i++) {
		sc(SYS_write, a[1], (long)&c, 1, 0);
		if (sc(SYS_read, b[0], (long)&c, 1, 0) != 1)
			break;
	}
	dt = now_us() - t0;
	st = wait_child(pid);
	sc(SYS_close, a[0], 0, 0, 0);
	sc(SYS_close, a[1], 0, 0, 0);
	sc(SYS_close, b[0], 0, 0, 0);
	sc(SYS_close, b[1], 0, 0, 0);
	out("  pipe round trip: ");
	outn(dt * 10 / N, 10);
	out(" x0.1 us (2 switches each)\n");
	result("pipe ping-pong between two ATRANS processes", i == N && st == 0);
}

/* ---- clone, vfork, threads ---- */

/* clone(SIGCHLD) without CLONE_VM is fork (musl's fork does this). */
static void t_clone_fork(void)
{
	long pid;
	int st;

	g_data = 111;
	pid = sc(SYS_clone, SIGCHLD, 0, 0, 0);
	if (pid == 0) {
		g_data = 333;
		die(g_data == 333 ? 7 : 1);
	}
	st = wait_child(pid);
	status_line("clone(SIGCHLD) child", st);
	result("clone without CLONE_VM: fork semantics", pid > 0 && st == 7 << 8 && g_data == 111);
}

/* vfork (or clone(CLONE_VM|CLONE_VFORK)): the child shares memory and the
 * parent waits until it exits. The child path touches no stack. */
static long __attribute__((naked, noinline)) vfork_set(long use_clone, volatile ul *flag)
{
	__asm__ volatile(
		"push	{r7}\n"
		"mov	r2, r1\n"
		"cmp	r0, #0\n"
		"ite	eq\n"
		"moveq	r7, #190\n"		/* vfork */
		"movne	r7, #120\n"		/* clone */
		"movw	r0, #0x4111\n"		/* CLONE_VM | CLONE_VFORK | SIGCHLD */
		"mov	r1, #0\n"
		"svc	0\n"
		"cmp	r0, #0\n"
		"bne	1f\n"
		"mov	r3, #1\n"
		"str	r3, [r2]\n"
		"mov	r0, #3\n"
		"mov	r7, #1\n"		/* exit */
		"svc	0\n"
		"1:	pop	{r7}\n"
		"bx	lr\n");
}

static volatile ul g_vflag;

static void t_vfork(void)
{
	int k, bad = 0;

	for (k = 0; k < 2; k++) {
		long pid;
		int st;

		g_vflag = 0;
		pid = vfork_set(k, &g_vflag);
		st = wait_child(pid);
		out(k ? "  clone(CLONE_VM|CLONE_VFORK)" : "  vfork");
		out(": child ");
		outd(pid);
		out(", status ");
		outn(st, 16);
		out(", flag set by child ");
		outn(g_vflag, 10);
		out("\n");
		if (pid <= 0 || st != 3 << 8 || g_vflag != 1)
			bad = 1;
	}
	result("vfork and clone(CLONE_VM|CLONE_VFORK) share memory", !bad);
}

/* clone for threads: fn(arg) on a new stack, then exit (the thread only).
 * The new stack holds arg and fn. */
static long __attribute__((naked, noinline)) clone_thread(ul flags, ul stack, int *ptid, ul tls, int *ctid)
{
	__asm__ volatile(
		"push	{r4, r7}\n"
		"ldr	r4, [sp, #8]\n"		/* ctid, the 5th argument */
		"mov	r7, #120\n"
		"svc	0\n"
		"cmp	r0, #0\n"
		"bne	1f\n"
		"pop	{r0, r1}\n"		/* child: arg, fn from its new stack */
		"blx	r1\n"
		"mov	r7, #1\n"		/* exit (this thread) */
		"svc	0\n"
		"1:	pop	{r4, r7}\n"
		"bx	lr\n");
}

static long futex(volatile int *addr, int op, int val, long long *ts)
{
	return sc6(SYS_futex_time64, (long)addr, op, val, (long)ts, 0, 0);
}
#define FUTEX_WAIT	0
#define FUTEX_WAKE	1

static volatile ul g_counter;

static void atomic_inc(volatile ul *p)
{
	ul t, f;

	__asm__ volatile("1: ldrex %0, [%2]\n"
			 "add %0, %0, #1\n"
			 "strex %1, %0, [%2]\n"
			 "cmp %1, #0\n"
			 "bne 1b\n"
			 : "=&r"(t), "=&r"(f) : "r"(p) : "cc", "memory");
}

struct tharg {
	ul tls;
	volatile ul bad;
	volatile ul done;
};

static void thread_fn(void *v)
{
	struct tharg *a = v;
	int i;

	for (i = 0; i < 200; i++) {
		atomic_inc(&g_counter);
		if ((ul)get_tls() != a->tls || TPWORD != a->tls)
			a->bad |= 1;
		if (i % 20 == 0)
			sc(SYS_sched_yield, 0, 0, 0, 0);
	}
	set_tls(a->tls + 1);			/* only this thread's */
	sc(SYS_sched_yield, 0, 0, 0, 0);
	if ((ul)get_tls() != a->tls + 1 || TPWORD != a->tls + 1)
		a->bad |= 2;
	a->done = 1;
}

static void t_threads(void)
{
	enum { N = 3, STACK = 8192 };
	static struct tharg args[N];
	static volatile int ctid[N];
	int ptid[N], i, bad = 0;
	long stacks, tid[N];
	ul flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
		   CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID;

	set_tls(0x7a1a0000);
	g_counter = 0;
	stacks = anon(N * STACK);
	if (stacks < 0) {
		result("threads", 0);
		return;
	}
	for (i = 0; i < N; i++) {
		ul *top = (ul *)(stacks + (i + 1) * STACK);

		args[i].tls = 0x7b000000UL + (i << 8);
		args[i].bad = args[i].done = 0;
		top[-2] = (ul)&args[i];
		top[-1] = (ul)thread_fn;
		ctid[i] = -1;
		tid[i] = clone_thread(flags, (ul)(top - 2), &ptid[i], args[i].tls, (int *)&ctid[i]);
		if (tid[i] <= 0 || ptid[i] != tid[i])
			bad |= 1;
	}
	for (i = 0; i < N; i++) {
		int v;

		while (tid[i] > 0 && (v = ctid[i]) != 0)	/* CLONE_CHILD_CLEARTID: 0 + wake */
			futex(&ctid[i], FUTEX_WAIT, v, 0);
		if (!args[i].done || args[i].bad)
			bad |= 2;
	}
	if (g_counter != N * 200)
		bad |= 4;
	if ((ul)get_tls() != 0x7a1a0000 || TPWORD != 0x7a1a0000)
		bad |= 8;
	out("  ");
	outn(N, 10);
	out(" threads, counter ");
	outn(g_counter, 10);
	out(", main tls ");
	outn(get_tls(), 16);
	out("; failures ");
	outn(bad, 16);
	out("\n");
	sc(SYS_munmap, stacks, N * STACK, 0, 0);
	result("threads: CLONE_VM|CLONE_THREAD|CLONE_SETTLS, CHILD_CLEARTID futex wake", !bad);
}

/* TLS: set_tls/get_tls and the read-only thread pointer word survive
 * context switches and fork; user mode can't write the word. */
static void t_tls(void)
{
	long pid;
	int i, bad = 0, st;

	set_tls(0x12345678);
	for (i = 0; i < 20; i++) {
		sc(SYS_sched_yield, 0, 0, 0, 0);
		if ((ul)get_tls() != 0x12345678 || TPWORD != 0x12345678)
			bad |= 1;
	}
	pid = sc(SYS_fork, 0, 0, 0, 0);
	if (pid == 0) {
		if ((ul)get_tls() != 0x12345678 || TPWORD != 0x12345678)
			die(1);		/* inherited */
		set_tls(0xabcd0000);
		for (i = 0; i < 20; i++) {
			sc(SYS_sched_yield, 0, 0, 0, 0);
			if ((ul)get_tls() != 0xabcd0000 || TPWORD != 0xabcd0000)
				die(2);
		}
		die(0);
	}
	for (i = 0; i < 20; i++) {
		sc(SYS_sched_yield, 0, 0, 0, 0);
		if ((ul)get_tls() != 0x12345678 || TPWORD != 0x12345678)
			bad |= 2;
	}
	st = wait_child(pid);
	if (st != 0)
		bad |= 4;
	pid = sc(SYS_fork, 0, 0, 0, 0);
	if (pid == 0) {
		TPWORD = 1;		/* read-only: SIGSEGV */
		die(0);
	}
	st = wait_child(pid);
	if ((st & 0x7f) != 11)
		bad |= 8;
	out("  tls ");
	outn(get_tls(), 16);
	out(", word at 0x11000000 ");
	outn(TPWORD, 16);
	out(", writing the word: ");
	outn(st & 0x7f ? st & 0x7f : 0, 10);
	out(" (signal); failures ");
	outn(bad, 16);
	out("\n");
	result("TLS: set_tls/get_tls, fast-path word, kept across switches and fork", !bad);
}

/* Private futexes are keyed by mm too: two processes at the same window
 * address don't see each other's waiters (stock nommu keys by address). */
static volatile int g_fut;

static void t_futex(void)
{
	long pid, woken;
	int st;

	g_fut = 0;
	pid = sc(SYS_fork, 0, 0, 0, 0);
	if (pid == 0) {
		long long ts[2] = { 0, 300 * 1000 * 1000 };	/* 300 ms */
		long r = futex(&g_fut, FUTEX_WAIT, 0, ts);

		die(r == -110 ? 0 : 1);		/* ETIMEDOUT: nobody woke us */
	}
	sleep_us(100 * 1000);
	woken = futex(&g_fut, FUTEX_WAKE, 1, 0);
	st = wait_child(pid);
	out("  wake from another process woke ");
	outd(woken);
	out("; ");
	status_line("child (exit 0 = timed out)", st);
	result("futex keys are per process", woken == 0 && st == 0);
}

/* ---- /proc for ATRANS processes ---- */

static long read_file(const char *path, char *buf, long n)
{
	long fd = sc(SYS_open, (long)path, 0, 0, 0), got = 0, r;

	if (fd < 0)
		return fd;
	while (got < n && (r = sc(SYS_read, fd, (long)buf + got, n - got, 0)) > 0)
		got += r;
	sc(SYS_close, fd, 0, 0, 0);
	return got;
}

static void t_proc(void)
{
	char path[40], buf[512], *p;
	int pfd[2], bad = 0, st;
	long pid, n, m;

	/* self: the block is loaded (read through the window) */
	n = read_file("/proc/self/cmdline", buf, sizeof(buf));
	if (n <= 0 || !contains(buf, n, "forktest"))
		bad |= 1;
	/* a child blocked on a pipe: not loaded (read through the uncached alias) */
	sc(SYS_pipe, (long)pfd, 0, 0, 0);
	pid = sc(SYS_fork, 0, 0, 0, 0);
	if (pid == 0) {
		char c;

		sc(SYS_close, pfd[1], 0, 0, 0);
		sc(SYS_read, pfd[0], (long)&c, 1, 0);
		die(0);
	}
	sc(SYS_close, pfd[0], 0, 0, 0);
	sleep_us(20 * 1000);
	p = fmtn(memcpy(path, "/proc/", 7) + 6, pid);
	memcpy(p, "/cmdline", 9);
	n = read_file(path, buf, sizeof(buf));
	out("  /proc/");
	outn(pid, 10);
	out("/cmdline: ");
	outd(n);
	out(" bytes");
	if (n <= 0 || !contains(buf, n, "forktest"))
		bad |= 2;
	memcpy(p, "/maps", 6);
	m = read_file(path, buf, sizeof(buf) - 1);
	if (m <= 0 || !contains(buf, m, "[stack]") || !contains(buf, m, "forktest") ||
	    !contains(buf, m, "[block"))
		bad |= 4;
	memcpy(p, "/stat", 6);
	n = read_file(path, buf, sizeof(buf));
	if (n <= 0)
		bad |= 8;
	out(", maps ");
	outd(m);
	out(" bytes; failures ");
	outn(bad, 16);
	out("\n");
	sc(SYS_write, pfd[1], (long)"x", 1, 0);
	sc(SYS_close, pfd[1], 0, 0, 0);
	st = wait_child(pid);
	result("/proc/<pid>/cmdline and maps of ATRANS processes", !bad && st == 0);
}

/* ---- signals ---- */

struct ksigaction {
	void *handler;
	ul flags;
	void *restorer;
	ul mask[2];
};
#define SA_SIGINFO	4
#define SIGUSR1		10
#define SIGSEGV		11

static long sigaction(int sig, void *handler, ul flags)
{
	struct ksigaction sa = { handler, flags, 0, { 0, 0 } };

	return sc(SYS_rt_sigaction, sig, (long)&sa, 0, 8);
}

static volatile ul g_sig, g_sigaddr, g_sigsp;

static void on_usr1(int sig)
{
	volatile ul local;

	g_sig = sig;
	g_sigsp = (ul)&local;
}

/* SIGSEGV with SA_SIGINFO: record si_addr and skip the faulting 16-bit
 * load by moving the saved pc (uc_mcontext.arm_pc) */
static void on_segv(int sig, void *si, void *uc)
{
	g_sig = sig;
	g_sigaddr = ((ul *)si)[3];
	((ul *)uc)[23] += 2;		/* uc_mcontext (+20 bytes) .arm_pc (+72) */
}

static void t_signals(void)
{
	register ul r1v __asm__("r1");
	int bad = 0;
	long r;

	r = sigaction(SIGUSR1, (void *)on_usr1, 0);	/* no restorer: return code on the stack */
	g_sig = 0;
	sc(SYS_kill, sc(SYS_getpid, 0, 0, 0, 0), SIGUSR1, 0, 0);
	if (r || g_sig != SIGUSR1 || g_sigsp < WINDOW || g_sigsp >= (ul)__image_start)
		bad |= 1;

	r = sigaction(SIGSEGV, (void *)on_segv, SA_SIGINFO);
	g_sig = 0;
	r1v = 0x11400000;
	__asm__ volatile("ldr r0, [r1]" : : "r"(r1v) : "r0", "memory");	/* 16-bit encoding */
	if (r || g_sig != SIGSEGV || g_sigaddr != 0x11400000)
		bad |= 2;
	sigaction(SIGSEGV, 0, 0);			/* SIG_DFL */
	out("  SIGUSR1 handler ran on the stack at ");
	outn(g_sigsp, 16);
	out(", SIGSEGV handler saw si_addr ");
	outn(g_sigaddr, 16);
	out("; failures ");
	outn(bad, 16);
	out("\n");
	result("signal handlers: delivery, sigreturn, SA_SIGINFO fault recovery", !bad);
}

/* ---- bad stack pointers: the kernel survives, the process dies ---- */

#pragma GCC diagnostic ignored "-Winfinite-recursion"
static ul recurse(ul n)
{
	volatile char buf[1024];

	buf[0] = (char)n;
	return recurse(n + 1) + buf[0];
}

static void __attribute__((naked, noinline)) svc_with_sp(ul sp)
{
	__asm__ volatile("mov sp, r0\n"
			 "mov r7, #20\n"		/* getpid */
			 "svc 0\n"
			 "b .\n");
}

static void __attribute__((naked, noinline)) push_with_sp(ul sp)
{
	__asm__ volatile("mov sp, r0\n"
			 "push {r0-r3}\n"
			 "b .\n");
}

static void __attribute__((naked, noinline)) sigreturn_with_frame(ul frame)
{
	__asm__ volatile("mov sp, r0\n"
			 "mov r7, #173\n"		/* rt_sigreturn */
			 "svc 0\n"
			 "b .\n");
}

static void t_badsp(void)
{
	static const char *what[] = {
		"stack overflow (recursion below 0x11000000)",
		"svc with sp = 0x30000000",
		"svc with sp = 0x11480000 (kernel RAM)",
		"push with sp past the block",
		"rt_sigreturn to sp = 0x20000000",
	};
	unsigned i;
	int bad = 0;
	long r;

	for (i = 0; i < sizeof(what) / sizeof(what[0]); i++) {
		long pid = sc(SYS_fork, 0, 0, 0, 0);
		int st;

		if (pid == 0) {
			ul frame[64] __attribute__((aligned(8)));

			switch (i) {
			case 0: recurse(0); break;
			case 1: svc_with_sp(0x30000000); break;
			case 2: svc_with_sp(0x11480000); break;
			case 3: push_with_sp(WEND - 4096); break;
			case 4:
				memset(frame, 0, sizeof(frame));
				/* rt_sigframe: siginfo (128 bytes), then ucontext:
				 * uc_mcontext at +20, arm_sp at +20 + 64 */
				frame[(128 + 20 + 64) / 4] = 0x20000000;
				frame[(128 + 20 + 68) / 4] = 0x11020001;	/* lr */
				frame[(128 + 20 + 72) / 4] = 0x11020000;	/* pc */
				frame[(128 + 20 + 76) / 4] = 0x01000000;	/* xPSR: Thumb */
				sigreturn_with_frame((ul)frame);
				break;
			}
			die(0);
		}
		st = wait_child(pid);
		out("  ");
		out(what[i]);
		out(": ");
		if ((st & 0x7f) == 11)
			out("SIGSEGV\n");
		else {
			out("status ");
			outn(st, 16);
			out("\n");
			bad |= 1 << i;
		}
	}
	/* clone with a stack outside the block: refused before the kernel
	 * would write an exception frame there */
	r = sc(SYS_clone, CLONE_VM | SIGCHLD, WEND - 4096, 0, 0);
	if (r == 0)
		die(0);		/* must not happen */
	out("  clone(newsp past the block) = ");
	outd(r);
	out(" (-22 = EINVAL)\n");
	if (r != -22)
		bad |= 0x100;
	result("bad stack pointers kill the process, not the kernel", !bad);
}

/* ---- out of memory ---- */

/* Children grab 1.5 MiB each and hold it: free RAM can't serve them all,
 * so some get ENOMEM from brk (exit 2), none is killed, and the memory
 * comes back. */
static void t_oom(void)
{
	enum { N = 4 };
	ul want = 1536 << 10;
	int pfd[2], i, ok = 0, nomem = 0, other = 0;
	long pids[N];

	sc(SYS_pipe, (long)pfd, 0, 0, 0);
	for (i = 0; i < N; i++) {
		pids[i] = sc(SYS_fork, 0, 0, 0, 0);
		if (pids[i] == 0) {
			ul base = sc(SYS_brk, 0, 0, 0, 0), k;
			char c;

			sc(SYS_close, pfd[1], 0, 0, 0);
			if ((ul)sc(SYS_brk, base + want, 0, 0, 0) != base + want)
				die(2);
			for (k = 0; k < want; k += 4096)
				*(volatile ul *)(base + k) = k;
			sc(SYS_read, pfd[0], (long)&c, 1, 0);	/* hold until the parent closes */
			for (k = 0; k < want; k += 4096)
				if (*(volatile ul *)(base + k) != k)
					die(3);
			die(0);
		}
		sleep_us(200 * 1000);
	}
	sc(SYS_close, pfd[0], 0, 0, 0);
	sc(SYS_close, pfd[1], 0, 0, 0);
	for (i = 0; i < N; i++) {
		int st = wait_child(pids[i]);

		if (st == 0)
			ok++;
		else if (st == 2 << 8)
			nomem++;
		else
			other++;
	}
	out("  ");
	outn(N, 10);
	out(" children wanting 1.5 MiB each: ");
	outn(ok, 10);
	out(" got it, ");
	outn(nomem, 10);
	out(" ENOMEM, ");
	outn(other, 10);
	out(" other\n");
	result("out of memory: brk fails with ENOMEM, nothing is killed", !other && nomem > 0);
}

/* ---- main ---- */

/* ---- shared text (v3 layout: forktest3) ---- */

#define TEXTWIN	0x11C00000UL

/* The text window is read-only for user mode and for the kernel's write
 * paths, readable by syscalls, shared by fork, shown in maps. */
static void t_text(void)
{
	char buf[1024];
	int pfd[2], bad = 0, st;
	long pid, r, n;

	if ((ul)_start < TEXTWIN) {
		result("shared text (skipped: v2 binary)", 1);
		return;
	}
	pid = sc(SYS_fork, 0, 0, 0, 0);
	if (pid == 0) {
		*(volatile ul *)((ul)t_text & ~1UL) = 0;	/* store into code */
		die(0);
	}
	st = wait_child(pid);
	if ((st & 0x7f) != 11)
		bad |= 1;
	pid = sc(SYS_fork, 0, 0, 0, 0);
	if (pid == 0) {
		(void)*(volatile ul *)(0x12000000UL - 4096);	/* past the text block */
		die(0);
	}
	st = wait_child(pid);
	if ((st & 0x7f) != 11)
		bad |= 2;
	/* the kernel writing into text: refused (a pipe, so read() doesn't block) */
	sc(SYS_pipe, (long)pfd, 0, 0, 0);
	sc(SYS_write, pfd[1], (long)"abcd", 4, 0);
	r = sc(SYS_read, pfd[0], (long)t_text & ~1UL, 4, 0);
	if (r != -14)
		bad |= 4;
	if (sc(SYS_clock_gettime64, 1, (long)t_text & ~1UL, 0, 0) != -14)
		bad |= 8;
	sc(SYS_close, pfd[0], 0, 0, 0);
	sc(SYS_close, pfd[1], 0, 0, 0);
	/* the kernel reading from text: a string literal (rodata, text window) */
	if (sc(SYS_write, 1, (long)"  (this line is in the text window)\n", 36, 0) != 36)
		bad |= 16;
	/* fork shares it: the child's maps say 2 users */
	pid = sc(SYS_fork, 0, 0, 0, 0);
	if (pid == 0) {
		n = read_file("/proc/self/maps", buf, sizeof(buf) - 1);
		if (n > 0)
			sc(SYS_write, 1, (long)buf, n, 0);
		die(n > 0 && contains(buf, n, "r-xs") && contains(buf, n, "2 users]") ? 0 : 1);
	}
	st = wait_child(pid);
	if (st != 0)
		bad |= 32;
	out("  read() into text ");
	outd(r);
	out("; failures ");
	outn(bad, 16);
	out("\n");
	result("shared text: read-only, readable by syscalls, shared by fork", !bad);
}

static const struct { const char *name; void (*fn)(void); } tests[] = {
	{ "startup", t_startup },
	{ "isolation", t_isolation },
	{ "many", t_many },
	{ "heap", t_heap },
	{ "grow", t_grow },
	{ "oob", t_oob },
	{ "mpu", t_mpu },
	{ "efault", t_efault },
	{ "mmap", t_mmap },
	{ "exec", t_exec },
	{ "execatrans", t_exec_atrans },
	{ "clone", t_clone_fork },
	{ "vfork", t_vfork },
	{ "tls", t_tls },
	{ "threads", t_threads },
	{ "futex", t_futex },
	{ "proc", t_proc },
	{ "signals", t_signals },
	{ "badsp", t_badsp },
	{ "forktime", t_fork_time },
	{ "forksize", t_fork_size },
	{ "pingpong", t_pingpong },
	{ "text", t_text },
	{ "oom", t_oom },
};

void __attribute__((noreturn, used)) cmain(long *sp)
{
	long argc = sp[0];
	char **argv = (char **)(sp + 1);
	unsigned i;
	int a;

	g_sp0 = sp;
	check_startup(0);	/* sets g_self */
	if (argc > 1 && streq(argv[1], "child-startup"))
		die(child_startup(argc, argv));
	if (argc > 2 && streq(argv[1], "sleep")) {
		ul s = 0;
		char *p = argv[2];

		while (*p >= '0' && *p <= '9')
			s = s * 10 + *p++ - '0';
		sleep_us(s * 1000000);
		die(0);
	}
	if (argc > 1) {
		for (a = 1; a < argc; a++) {
			for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
				if (streq(argv[a], tests[i].name))
					break;
			if (i == sizeof(tests) / sizeof(tests[0])) {
				out("forktest: no test ");
				out(argv[a]);
				out("\n");
				die(2);
			}
			tests[i].fn();
		}
	} else {
		for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
			tests[i].fn();
	}
	out(failed ? "forktest: FAIL (" : "forktest: PASS (");
	outn(passed, 10);
	out(" passed, ");
	outn(failed, 10);
	out(" failed)\n");
	die(failed != 0);
}

void __attribute__((naked, noreturn, section(".text.start"))) _start(void)
{
	__asm__ volatile("mov r0, sp\n\tbl cmain\n");
}
