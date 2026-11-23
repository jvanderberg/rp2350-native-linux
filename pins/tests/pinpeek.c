// SPDX-License-Identifier: MIT
/*
 * pinpeek: look at a pin's hardware state from user space, to check the
 * pin drivers on the board without a second board. An FDPIC program: it
 * runs with the MPU off, so it reads the chip's registers directly (read
 * only; it writes nothing).
 *
 *   pinpeek regs PIN        IO_BANK0 CTRL (FUNCSEL), the pad, SIO in/out/oe
 *   pinpeek meas PIN MS [HZ DUTY]
 *                           samples the pad's level for MS ms: edges,
 *                           frequency and duty (the duty to about 10 kHz:
 *                           above that, reading the clock at each edge
 *                           takes a visible part of the period);
 *                           with HZ and DUTY (%), exits 1 unless within 1%
 *                           and 2.5 points of them
 *
 * meas is a process: while it doesn't have the CPU (another process, a
 * switch) it misses edges. So it times each period it sees (rise to rise)
 * and its high time, and drops the periods more than 2% (or 2 us, the
 * clock's steps) off their median (a gap in them). The frequency is the
 * mean of the rest, which averages out the clock's 1 us steps; the duty
 * the median of their high fractions, which a stall between an edge and
 * its timestamp doesn't move.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAXP 4096	/* periods kept */
static float per_s[MAXP], per_high[MAXP], sorted[MAXP];

static int cmp_float(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;

	return x < y ? -1 : x > y;
}

static double median(const float *v, unsigned n)
{
	memcpy(sorted, v, n * sizeof(*v));
	qsort(sorted, n, sizeof(*v), cmp_float);
	return n % 2 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
}

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define IO_CTRL(n) REG(0x40028000 + 8 * (n) + 4)
#define IO_STATUS(n) REG(0x40028000 + 8 * (n))
#define PAD(n) REG(0x40038000 + 4 + 4 * (n))
#define SIO_IN(n) REG(0xd0000004 + ((n) >= 32 ? 4 : 0))
#define SIO_OUT(n) REG(0xd0000010 + ((n) >= 32 ? 4 : 0))
#define SIO_OE(n) REG(0xd0000030 + ((n) >= 32 ? 4 : 0))

static const char *func_name(unsigned f)
{
	static const char *n[] = { "HSTX", "SPI", "UART", "I2C", "PWM", "SIO", "PIO0", "PIO1",
				   "PIO2", "GPCK", "USB", "UART_AUX" };
	return f < 12 ? n[f] : f == 0x1f ? "NULL" : "?";
}

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
	if (argc < 3)
		goto usage;
	unsigned n = atoi(argv[2]), b = 1u << (n % 32);

	if (n >= 48)
		goto usage;
	if (!strcmp(argv[1], "regs")) {
		uint32_t c = IO_CTRL(n), p = PAD(n), st = IO_STATUS(n);

		printf("GP%u: FUNCSEL %u (%s) ctrl 0x%08x status 0x%08x pad 0x%03x%s%s%s%s%s in %d out %d oe %d\n",
		       n, c & 0x1f, func_name(c & 0x1f), c, st, p, p & 0x100 ? " ISO" : "",
		       p & 0x80 ? " OD" : "", p & 0x40 ? " IE" : "", p & 8 ? " PUE" : "",
		       p & 4 ? " PDE" : "", !!(SIO_IN(n) & b), !!(SIO_OUT(n) & b), !!(SIO_OE(n) & b));
		return 0;
	}
	if (!strcmp(argv[1], "meas") && argc > 3) {
		double ms = atof(argv[3]), t0 = now(), end = t0 + ms / 1000, t, first = 0, last = 0;
		unsigned long rises = 0, falls = 0, samples = 0, high = 0;
		unsigned np = 0;
		double hi = -1;	/* the current period's high time (rise to fall) */
		int prev = !!(SIO_IN(n) & b), v;

		do {
			for (int k = 0; k < 256; k++) {
				v = !!(SIO_IN(n) & b);
				samples++;
				high += v;
				if (v != prev) {
					if (v) {
						rises++;
						t = now();
						if (!first) {
							first = t;
						} else if (np < MAXP && hi >= 0 && t > last) {
							/* the period that ended here; its edges
							 * timed alike, so the delay to see one
							 * cancels out */
							per_s[np] = t - last;
							per_high[np] = hi;
							np++;
						}
						last = t;
						hi = -1;
					} else {
						falls++;
						if (first)
							hi = now() - last;
					}
					prev = v;
				}
			}
		} while (now() < end);
		double hz = rises > 1 ? (rises - 1) / (last - first) : 0.0;
		double duty = samples ? 100.0 * high / samples : 0.0;

		if (np >= 3) {
			double m = median(per_s, np), sp = 0;
			double tol = m * 0.02 > 2e-6 ? m * 0.02 : 2e-6;
			unsigned kept = 0;

			for (unsigned i = 0; i < np; i++) {
				if (per_s[i] < m - tol || per_s[i] > m + tol)
					continue;
				sp += per_s[i];
				per_high[kept++] = per_high[i] / per_s[i];	/* now a fraction */
			}
			hz = kept / sp;
			duty = 100.0 * median(per_high, kept);
		}

		printf("GP%u: rises=%lu falls=%lu hz=%.1f duty=%.1f samples=%lu\n", n, rises, falls,
		       hz, duty, samples);
		if (argc > 5) {
			/* within 1% and 2.5 points of HZ and DUTY */
			double want = atof(argv[4]), wd = atof(argv[5]);

			return hz > want * 0.99 && hz < want * 1.01 && duty > wd - 2.5 && duty < wd + 2.5 ? 0 : 1;
		}
		return 0;
	}
usage:
	fprintf(stderr, "usage: pinpeek regs PIN | pinpeek meas PIN MS\n");
	return 2;
}
