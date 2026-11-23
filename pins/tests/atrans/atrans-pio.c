// SPDX-License-Identifier: MIT
/*
 * atrans-pio: /dev/pio from an ATRANS process (static musl, linked in the
 * window; pins/README.md "PIO from ATRANS programs"). Checks:
 *   - pio_open maps PIO1's registers outside the window, and they work: a
 *     state machine echoes words through the FIFOs
 *   - the DMA buffer maps (RAM outside the window) and DMA echoes through it
 *   - /proc/self/maps lists both device ranges
 *   - a forked child uses the parent's mapping; the parent still can after
 *     the child exits
 *   - a child touching PIO registers it hasn't mapped dies with SIGSEGV
 *   - after pio_close (munmap) the old addresses fault too
 * atrans-pio [N] uses PIO block N (1, the default, or 2). Prints PASS/FAIL
 * lines and "atrans-pio: N of M passed"; exit 0 if all.
 */
#include <pio.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int checks, fails;

static void check(int ok, const char *what, const char *detail)
{
	checks++;
	if (!ok)
		fails++;
	printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail ? ": " : "", detail ? detail : "");
	fflush(stdout);
}

static const char *echo =
	".program echo\n"
	".wrap_target\n"
	"    pull\n"
	"    mov isr, ~osr\n"
	"    push\n"
	".wrap\n";

/* n words through state machine 0 (set up by setup()); wrong ones */
static int fifo_echo(PIO pio, int n)
{
	uint32_t x = 12345;
	int bad = 0;

	for (int i = 0; i < n; i++) {
		x = x * 1664525u + 1013904223u;
		pio_sm_put_blocking(pio, 0, x);
		bad += pio_sm_get_blocking(pio, 0) != ~x;
	}
	return bad;
}

static int setup(PIO pio)
{
	char err[128];
	pio_asm_program_t prog;

	if (pio_assemble(echo, "echo", &prog, err, sizeof(err)) < 0)
		return -1;
	int off = pio_add_program(pio, &prog.program);
	pio_sm_config c = pio_asm_default_config(&prog, off);
	pio_sm_init(pio, 0, off, &c);
	pio_sm_set_enabled(pio, 0, true);
	return 0;
}

/* run fn in a child; its wait status */
static int in_child(void (*fn)(void *), void *arg)
{
	int st;
	pid_t p = fork();

	if (p == 0) {
		fn(arg);
		_exit(0);
	}
	if (p < 0 || waitpid(p, &st, 0) != p)
		return -1;
	return st;
}

static volatile uint32_t sink;

static void touch(void *addr)
{
	printf("  child reads %p\n", addr);
	fflush(stdout);
	sink = *(volatile uint32_t *)addr;	/* tcc drops a (void) read */
}

static void child_echo(void *arg)
{
	_exit(fifo_echo((PIO)arg, 1000) ? 1 : 0);
}

int main(int argc, char **argv)
{
	char d[160], dev[16], what[64];
	int st, n = argc > 1 ? atoi(argv[1]) : 1;
	void *regs = (void *)(n == 2 ? 0x50400000 : 0x50300000);

	if (n != 1 && n != 2) {
		fprintf(stderr, "usage: atrans-pio [1|2]\n");
		return 2;
	}
	snprintf(dev, sizeof(dev), "/dev/pio%d", n);

	/* before any mapping: the block's registers are out of reach */
	st = in_child(touch, regs);
	snprintf(d, sizeof(d), "status %#x", st);
	check(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV,
	      "touching unmapped PIO registers is SIGSEGV", d);

	PIO pio = pio_open(n);
	snprintf(d, sizeof(d), "%p", (void *)pio);
	snprintf(what, sizeof(what), "pio_open maps PIO%d's registers at %p", n, regs);
	check(pio && (void *)pio == regs, what, pio ? d : strerror(errno));
	if (!pio)
		goto out;
	check(setup(pio) == 0, "a program loads", NULL);
	int bad = fifo_echo(pio, 10000);
	snprintf(d, sizeof(d), "%d of 10000 wrong", bad);
	check(!bad, "FIFO echo through the mapped registers", d);

	uint32_t *buf = pio_dma_buffer(pio, 8192);
	snprintf(d, sizeof(d), "%p", (void *)buf);
	check(buf && ((uintptr_t)buf < 0x11000000 || (uintptr_t)buf >= 0x11400000),
	      "the DMA buffer maps (outside the window)", buf ? d : strerror(errno));
	if (buf) {
		for (int i = 0; i < 1024; i++)
			buf[i] = 0x9e3779b9u * (i + 1);
		pio_dma_config rx = { 0, true, 4, 4096, 1024, 0, false };
		pio_dma_config tx = { 0, false, 4, 0, 1024, 0, false };
		int rc = pio_dma_start(pio, -1, &rx), tc = pio_dma_start(pio, -1, &tx);
		int ok = rc >= 0 && tc >= 0 && pio_dma_wait(pio, tc, 2000) == 0 &&
			 pio_dma_wait(pio, rc, 2000) == 0;
		bad = 0;
		for (int i = 0; i < 1024; i++)
			bad += buf[1024 + i] != ~buf[i];
		snprintf(d, sizeof(d), "%d of 1024 wrong", bad);
		check(ok && !bad, "DMA echo through the mapped buffer", d);
		pio_dma_stop(pio, rc);
		pio_dma_stop(pio, tc);
	}

	/* /proc/self/maps: both ranges, as rw-s lines naming the device */
	FILE *f = fopen("/proc/self/maps", "r");
	int lines = 0;
	char l[200];
	while (f && fgets(l, sizeof(l), f))
		if (strstr(l, "rw-s") && strstr(l, dev))
			lines++;
	if (f)
		fclose(f);
	snprintf(d, sizeof(d), "%d lines", lines);
	check(lines == (buf ? 2 : 1), "/proc/self/maps lists the device ranges", d);

	/* fork: the child inherits the mapping and uses it */
	st = in_child(child_echo, pio);
	snprintf(d, sizeof(d), "status %#x", st);
	check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "a forked child uses the parent's mapping", d);
	bad = fifo_echo(pio, 1000);
	snprintf(d, sizeof(d), "%d of 1000 wrong", bad);
	check(!bad, "the parent's mapping still works after the child exits", d);

	/* and after munmap the registers are out of reach again */
	pio_close(pio);
	st = in_child(touch, regs);
	snprintf(d, sizeof(d), "status %#x", st);
	check(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "after pio_close the registers fault", d);
	if (buf) {
		st = in_child(touch, buf);
		snprintf(d, sizeof(d), "status %#x", st);
		check(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "after pio_close the buffer faults", d);
	}
	/* the block is free again (pio_close released it) */
	pio = pio_open(n);
	snprintf(what, sizeof(what), "%s opens again", dev);
	check(pio != NULL, what, pio ? NULL : strerror(errno));
	if (pio)
		pio_close(pio);
out:
	printf("atrans-pio: %d of %d passed\n", checks - fails, checks);
	return fails != 0;
}
