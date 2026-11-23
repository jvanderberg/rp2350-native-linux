// SPDX-License-Identifier: MIT
/*
 * swaptest: whole-process swapping of ATRANS processes
 * (rp2350-atrans-swap.c). Freestanding like forktest, linked at 0x11020000.
 *
 *   swaptest [N [KIB [ROUNDS]]]   (default 4 children, 512 KiB, 3 rounds)
 *
 * Forks N children. Each one grows its heap by KIB KiB, maps 64 KiB, puts
 * 8 KiB of pattern on its stack, fills all of it with a pattern of its own,
 * then sleeps in read() on its own pipe. The parent wakes them in turn,
 * ROUNDS times: each wake-up checks every word of the child's memory
 * against the pattern it left, writes the next generation and goes back to
 * sleep. With more children than fit in RAM, each wake-up swaps the child
 * in and another one out. The parent also reads /proc/<pid>/cmdline of a
 * sleeping (possibly swapped-out) child. Prints the time per wake-up; see
 * /proc/atrans-swap for the swap statistics.
 *
 *   swaptest hold KIB SECONDS     hold KIB KiB of pattern, sleep, check
 *   swaptest wake KIB SECONDS MS  hold it, waking every MS ms (see wake())
 *   swaptest pipes KIB            fill pipes nobody reads (see pipes())
 */
typedef unsigned long ul;

#define SYS_exit		1
#define SYS_fork		2
#define SYS_read		3
#define SYS_write		4
#define SYS_open		5
#define SYS_close		6
#define SYS_getpid		20
#define SYS_kill		37
#define SYS_pipe		42
#define SYS_brk			45
#define SYS_wait4		114
#define SYS_mmap2		192
#define SYS_exit_group		248
#define SYS_clock_gettime64	403
#define SYS_clock_nanosleep64	407

#define WINDOW	0x11000000UL

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

static void out(const char *s) { sc(SYS_write, 1, (long)s, slen(s), 0); }

static void outn(ul v)
{
	char b[12], *p = b + sizeof(b);

	*--p = 0;
	do {
		*--p = '0' + v % 10;
		v /= 10;
	} while (v);
	out(p);
}

static void outx(ul v)
{
	char b[12], *p = b + sizeof(b);

	*--p = 0;
	do {
		*--p = "0123456789abcdef"[v % 16];
		v /= 16;
	} while (v);
	out("0x");
	out(p);
}

static ul num(const char *p)
{
	ul v = 0;

	while (*p >= '0' && *p <= '9')
		v = v * 10 + *p++ - '0';
	return v;
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

	sc(SYS_clock_gettime64, 1, (long)ts, 0, 0);
	return (ul)ts[0] * 1000000 + (ul)ts[1] / 1000;
}

static void sleep_us(ul us)
{
	long long ts[2] = { us / 1000000, (us % 1000000) * 1000 };

	sc(SYS_clock_nanosleep64, 1, 0, (long)ts, 0);
}

/* ---- the memory a child holds ---- */

struct mem {
	ul *heap, heap_words;
	ul *map, map_words;
	ul *stk, stk_words;
};

static inline ul pat(ul seed, ul i)
{
	ul x = seed ^ (i * 2654435761UL);

	x ^= x >> 13;
	return x * 0x9e3779b1UL;
}

static void fill(ul *p, ul n, ul seed)
{
	ul i;

	for (i = 0; i < n; i++)
		p[i] = pat(seed, i);
}

/* number of wrong words; reports the first */
static ul check(const char *what, ul *p, ul n, ul seed)
{
	ul i, bad = 0;

	for (i = 0; i < n; i++)
		if (p[i] != pat(seed, i)) {
			if (!bad) {
				out("  ");
				out(what);
				out(": word at ");
				outx((ul)&p[i]);
				out(" is ");
				outx(p[i]);
				out(", not ");
				outx(pat(seed, i));
				out("\n");
			}
			bad++;
		}
	return bad;
}

static void mem_fill(struct mem *m, ul seed)
{
	fill(m->heap, m->heap_words, seed);
	fill(m->map, m->map_words, seed + 1);
	fill(m->stk, m->stk_words, seed + 2);
}

static ul mem_check(struct mem *m, ul seed)
{
	return check("heap", m->heap, m->heap_words, seed) +
	       check("mmap", m->map, m->map_words, seed + 1) +
	       check("stack", m->stk, m->stk_words, seed + 2);
}

/* heap of kib KiB (brk), 64 KiB mapped; stk is the caller's stack array */
static int mem_get(struct mem *m, ul kib, ul *stk, ul stk_words)
{
	ul cur = sc(SYS_brk, 0, 0, 0, 0), want = cur + kib * 1024;
	long map;

	if ((ul)sc(SYS_brk, want, 0, 0, 0) != want) {
		out("  brk failed (out of memory?)\n");
		return -1;
	}
	map = sc6(SYS_mmap2, 0, 65536, 3, 0x22 /* MAP_PRIVATE|MAP_ANONYMOUS */, -1, 0);
	if (map < 0 && map > -4096) {
		out("  mmap failed\n");
		return -1;
	}
	m->heap = (ul *)cur;
	m->heap_words = kib * 1024 / sizeof(ul);
	m->map = (ul *)map;
	m->map_words = 65536 / sizeof(ul);
	m->stk = stk;
	m->stk_words = stk_words;
	return 0;
}

/* ---- swaptest hold KIB SECONDS ---- */

static int hold(ul kib, ul secs)
{
	ul stk[2048];
	struct mem m;
	ul bad;

	if (mem_get(&m, kib, stk, 2048))
		return 1;
	mem_fill(&m, 0x5eed);
	sleep_us(secs * 1000000);
	bad = mem_check(&m, 0x5eed);
	out(bad ? "swaptest hold: FAIL, " : "swaptest hold: PASS, ");
	outn(bad);
	out(" bad words\n");
	return bad != 0;
}

/* ---- swaptest pipes KIB ---- */

/*
 * Write KIB KiB into pipes that nobody reads, 64 KiB per pipe: kernel
 * pages outside the ATRANS pool, allocated without a fallback, so past
 * what reclaim, lending and swap can free the OOM killer runs. With
 * vm.oom_kill_allocating_task it takes this process ("Out of memory
 * (oom_kill_allocating_task)" in the log), and nothing else. Prints the
 * KiB written so far every 256 KiB.
 */
static int pipes(ul kib)
{
	static char page[4096];
	int fd[2] = { -1, -1 };
	ul done = 0, i;

	for (i = 0; i < sizeof(page); i++)
		page[i] = (char)i;
	while (done < kib) {
		if (done % 64 == 0 && sc(SYS_pipe, (long)fd, 0, 0, 0) < 0) {
			out("swaptest pipes: pipe failed\n");
			break;
		}
		if (sc(SYS_write, fd[1], (long)page, sizeof(page), 0) != sizeof(page)) {
			out("swaptest pipes: write failed\n");
			break;
		}
		done += 4;
		if (done % 256 == 0) {
			out("  ");
			outn(done);
			out(" KiB\n");
		}
	}
	out("swaptest pipes: ");
	outn(done);
	out(" KiB in pipes, not killed\n");
	return 0;
}

/* ---- swaptest wake KIB SECONDS MS ---- */

/*
 * Hold KIB KiB of pattern and wake every MS ms for SECONDS, checking a few
 * words each time and everything every 64 wake-ups. Meant to run alone with
 * background swap at "age 0": the swap worker (a kernel thread, which
 * borrows the mm of the last user task, usually this one) swaps it out
 * while it sleeps, and it wakes in the middle of that I/O (the swap-out is
 * cancelled) or after it (swapped back in). Each way it goes back to user
 * mode through a window the kernel thread changed.
 */
static int wake(ul kib, ul secs, ul ms)
{
	ul stk[2048];
	struct mem m;
	ul bad = 0, n = 0, t_end, t0, dt, dmax = 0, i;

	if (mem_get(&m, kib, stk, 2048))
		return 1;
	mem_fill(&m, 0x3a3e);
	t_end = now_us() + secs * 1000000;
	while (now_us() < t_end && !bad) {
		t0 = now_us();
		sleep_us(ms * 1000);
		dt = now_us() - t0;
		if (dt > dmax)
			dmax = dt;
		for (i = 0; i < m.heap_words; i += m.heap_words / 8)
			bad += m.heap[i] != pat(0x3a3e, i);
		if (!(++n & 63))
			bad += mem_check(&m, 0x3a3e);
	}
	bad += mem_check(&m, 0x3a3e);
	out(bad ? "swaptest wake: FAIL, " : "swaptest wake: PASS, ");
	outn(bad);
	out(" bad words, ");
	outn(n);
	out(" wake-ups, longest sleep ");
	outn(dmax / 1000);
	out(" ms\n");
	return bad != 0;
}

/* ---- children woken in turn ---- */

#define MAXCH	16

/* child: wait for a byte on cmd, check, refill, report on ack */
static void __attribute__((noreturn)) child(int idx, ul kib, int cmd, int ack)
{
	ul stk[2048];		/* 8 KiB of the stack */
	struct mem m;
	ul gen = 0, bad;
	char c, r;

	if (mem_get(&m, kib, stk, 2048)) {
		r = 'M';
		sc(SYS_write, ack, (long)&r, 1, 0);
		die(1);
	}
	mem_fill(&m, idx * 0x1000193UL + gen);
	r = 'R';
	sc(SYS_write, ack, (long)&r, 1, 0);
	for (;;) {
		if (sc(SYS_read, cmd, (long)&c, 1, 0) != 1 || c == 'q')
			die(0);
		bad = mem_check(&m, idx * 0x1000193UL + gen);
		gen++;
		mem_fill(&m, idx * 0x1000193UL + gen);
		r = bad ? 'B' : 'K';
		sc(SYS_write, ack, (long)&r, 1, 0);
	}
}

static int proc_cmdline_ok(long pid)
{
	char path[32] = "/proc/", buf[64], *p = path + 6;
	char d[12], *q = d + sizeof(d);
	long fd, n;

	*--q = 0;
	do {
		*--q = '0' + pid % 10;
		pid /= 10;
	} while (pid);
	while (*q)
		*p++ = *q++;
	memcpy(p, "/cmdline", 9);
	fd = sc(SYS_open, (long)path, 0, 0, 0);
	if (fd < 0)
		return 0;
	n = sc(SYS_read, fd, (long)buf, sizeof(buf) - 1, 0);
	sc(SYS_close, fd, 0, 0, 0);
	if (n < 8)
		return 0;
	buf[n] = 0;
	/* "/usr/bin/swaptest" or "swaptest" */
	for (p = buf; *p; p++)
		if (p[0] == 's' && p[1] == 'w' && p[2] == 'a' && p[3] == 'p' && p[4] == 't')
			return 1;
	return 0;
}

static int run(int n, ul kib, int rounds)
{
	int cmd[MAXCH][2], ack[2], i, r, bad = 0, nok = 0;
	long pid[MAXCH];
	ul t0, dt, tsum = 0, tmax = 0, ttot;
	char c;

	if (sc(SYS_pipe, (long)ack, 0, 0, 0))
		return 1;
	out("swaptest: ");
	outn(n);
	out(" children x (");
	outn(kib);
	out(" KiB heap + 64 KiB mmap + 8 KiB stack), ");
	outn(rounds);
	out(" rounds\n");
	ttot = now_us();
	for (i = 0; i < n; i++) {
		if (sc(SYS_pipe, (long)cmd[i], 0, 0, 0))
			return 1;
		t0 = now_us();
		pid[i] = sc(SYS_fork, 0, 0, 0, 0);
		if (pid[i] == 0) {
			sc(SYS_close, cmd[i][1], 0, 0, 0);
			sc(SYS_close, ack[0], 0, 0, 0);
			child(i, kib, cmd[i][0], ack[1]);
		}
		if (pid[i] < 0) {
			out("  fork failed: ");
			outn(-pid[i]);
			out("\n");
			return 1;
		}
		sc(SYS_close, cmd[i][0], 0, 0, 0);
		if (sc(SYS_read, ack[0], (long)&c, 1, 0) != 1 || c != 'R') {
			out("  child ");
			outn(i);
			out(" could not get its memory\n");
			bad++;
			n = i + 1;
			break;
		}
		out("  child ");
		outn(i);
		out(" (pid ");
		outn(pid[i]);
		out(") ready in ");
		outn((now_us() - t0) / 1000);
		out(" ms\n");
	}
	if (!bad) {
		for (r = 0; r < rounds; r++) {
			for (i = 0; i < n; i++) {
				c = 'w';
				t0 = now_us();
				sc(SYS_write, cmd[i][1], (long)&c, 1, 0);
				if (sc(SYS_read, ack[0], (long)&c, 1, 0) != 1)
					c = '?';
				dt = now_us() - t0;
				tsum += dt;
				if (dt > tmax)
					tmax = dt;
				out("  round ");
				outn(r);
				out(" child ");
				outn(i);
				out(c == 'K' ? ": ok, " : ": BAD, ");
				outn(dt / 1000);
				out(" ms\n");
				if (c != 'K')
					bad++;
			}
			/* everyone asleep: read a child's cmdline (from swap
			 * if it is swapped out) */
			if (r == 0)
				for (i = 0; i < n; i++)
					nok += proc_cmdline_ok(pid[i]);
		}
		out("  /proc/<pid>/cmdline readable for ");
		outn(nok);
		out(" of ");
		outn(n);
		out(" sleeping children\n");
		if (nok != n)
			bad++;
	}
	for (i = 0; i < n; i++) {
		c = 'q';
		sc(SYS_write, cmd[i][1], (long)&c, 1, 0);
	}
	for (i = 0; i < n; i++) {
		int st = -1;

		sc(SYS_wait4, pid[i], (long)&st, 0, 0);
		if (st != 0) {
			out("  child ");
			outn(i);
			out(" exit status ");
			outx(st);
			out("\n");
			bad++;
		}
	}
	ttot = now_us() - ttot;
	out("  wake-ups: avg ");
	outn(rounds * n ? tsum / (rounds * n) / 1000 : 0);
	out(" ms, max ");
	outn(tmax / 1000);
	out(" ms; total ");
	outn(ttot / 1000);
	out(" ms\n");
	out(bad ? "swaptest: FAIL\n" : "swaptest: PASS\n");
	return bad != 0;
}

void __attribute__((noreturn, used)) cmain(long *sp)
{
	long argc = sp[0];
	char **argv = (char **)(sp + 1);
	int n = 4, rounds = 3;
	ul kib = 512;

	if (argc > 3 && streq(argv[1], "hold"))
		die(hold(num(argv[2]), num(argv[3])));
	if (argc > 2 && streq(argv[1], "pipes"))
		die(pipes(num(argv[2])));
	if (argc > 4 && streq(argv[1], "wake"))
		die(wake(num(argv[2]), num(argv[3]), num(argv[4])));
	if (argc > 1)
		n = num(argv[1]);
	if (argc > 2)
		kib = num(argv[2]);
	if (argc > 3)
		rounds = num(argv[3]);
	if (n < 1 || n > MAXCH)
		n = 4;
	die(run(n, kib, rounds));
}

void __attribute__((naked, noreturn, section(".text.start"))) _start(void)
{
	__asm__ volatile("mov r0, sp\n\tbl cmain\n");
}
