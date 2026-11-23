// SPDX-License-Identifier: GPL-2.0
/*
 * MemManage, BusFault and UsageFault for ARMv7-M/ARMv8-M nommu Linux.
 *
 * Stock arch/arm/kernel/entry-v7m.S sends these to __invalid_entry, which
 * prints the registers and spins forever: one bad pointer in any user
 * program hangs the machine. apply.sh points vectors 4-6 at __fault_entry
 * instead, which calls v7m_fault() below. A fault from user mode (thread
 * mode on the process stack) gets the usual signal; __fault_entry then
 * raises PendSV, whose exit path (ret_to_user_from_irq) delivers it. A
 * fault in the kernel prints the fault status and the registers, and stops.
 *
 * A fault while the core stacks an exception frame onto a bad user SP
 * (MSTKERR, STKERR: a stack overflow, or any SP outside the process's
 * memory) leaves no frame to read. __fault_entry then points PSP at
 * v7m_stkerr_frame, a harmless frame, before v7m_exception_entry reads it,
 * and the task is killed with SIGSEGV: its registers are lost. Reading the
 * bad SP would fault again in the handler and escalate to a HardFault,
 * which hangs.
 */
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/signal.h>
#include <linux/io.h>
#include <asm/ptrace.h>
#include <asm/rp2350-atrans.h>
#include <asm/v7m.h>

#define SCB_CFSR	0x28
#define SCB_MMFAR	0x34
#define SCB_BFAR	0x38

/* CFSR = UFSR:BFSR:MMFSR */
#define MM_IACCVIOL	BIT(0)
#define MM_MSTKERR	BIT(4)
#define MM_MMARVALID	BIT(7)
#define BF_STKERR	BIT(12)
#define BF_BFARVALID	BIT(15)
#define UF_UNDEFINSTR	BIT(16)
#define UF_INVSTATE	BIT(17)
#define UF_INVPC	BIT(18)
#define UF_NOCP		BIT(19)
#define UF_STKOF	BIT(20)
#define UF_UNALIGNED	BIT(24)
#define UF_DIVBYZERO	BIT(25)

#define SCB_SHCSR	0x24
#define SHCSR_SVCALLPENDED	BIT(15)

asmlinkage void v7m_fault(struct pt_regs *regs, unsigned long exc_return);

/* used by __fault_entry (entry-v7m.S) for a fault while stacking: r0-r3,
 * r12, lr, pc, xPSR (Thumb) */
u32 v7m_stkerr_frame[8] __aligned(8) = { [7] = 0x01000000 };
u32 v7m_stkerr_psp;	/* the bad PSP */

asmlinkage void v7m_fault(struct pt_regs *regs, unsigned long exc_return)
{
	void __iomem *scb = BASEADDR_V7M_SCB;
	u32 cfsr = readl_relaxed(scb + SCB_CFSR);
	u32 mmfar = readl_relaxed(scb + SCB_MMFAR);
	u32 bfar = readl_relaxed(scb + SCB_BFAR);
	unsigned long addr = regs->ARM_pc;
	int sig, code;
	u32 ipsr;

	asm volatile("mrs %0, ipsr" : "=r"(ipsr));
	writel_relaxed(cfsr, scb + SCB_CFSR);	/* write-one-to-clear */

	if ((exc_return & EXC_RET_THREADMODE_PROCESSSTACK) != EXC_RET_THREADMODE_PROCESSSTACK) {
		pr_emerg("\nkernel fault: IPSR %u CFSR %08x MMFAR %08x BFAR %08x LR %08lx\n",
			 ipsr, cfsr, mmfar, bfar, exc_return);
		show_regs(regs);
		for (;;)
			;
	}

	if (cfsr & (MM_MSTKERR | BF_STKERR)) {
		/*
		 * No frame was stacked (regs come from v7m_stkerr_frame). If
		 * this was an SVC, it is still pending: drop it, or it would
		 * run with that frame's arguments. A pending interrupt or
		 * PendSV runs normally; the task dies in PendSV's exit path.
		 */
		writel_relaxed(readl_relaxed(scb + SCB_SHCSR) & ~SHCSR_SVCALLPENDED, scb + SCB_SHCSR);
		pr_info_ratelimited("%s[%d]: bad stack pointer %08x (CFSR %08x), killed\n",
				    current->comm, task_pid_nr(current), v7m_stkerr_psp, cfsr);
		force_fatal_sig(SIGSEGV);
#ifdef CONFIG_RP2350_ATRANS
		{
			unsigned long f = rp2350_atrans_stkerr_frame();

			if (f)	/* a frame user mode can read (MPU on) */
				asm volatile("msr psp, %0" : : "r"(f));
		}
#endif
		return;
	}

	if (cfsr & 0xff) {			/* MemManage */
		sig = SIGSEGV;
		code = SEGV_ACCERR;
		if (cfsr & MM_MMARVALID)
			addr = mmfar;
	} else if (cfsr & 0xff00) {		/* BusFault */
		sig = SIGBUS;
		code = BUS_ADRERR;
		if (cfsr & BF_BFARVALID)
			addr = bfar;
	} else if (cfsr & UF_UNALIGNED) {
		sig = SIGBUS;
		code = BUS_ADRALN;
	} else if (cfsr & UF_DIVBYZERO) {
		sig = SIGFPE;
		code = FPE_INTDIV;
	} else if (cfsr & UF_NOCP) {
		sig = SIGILL;
		code = ILL_COPROC;
	} else if (cfsr & UF_STKOF) {
		sig = SIGSEGV;
		code = SEGV_MAPERR;
	} else {				/* UNDEFINSTR, INVSTATE, INVPC */
		sig = SIGILL;
		code = ILL_ILLOPC;
	}

	pr_info_ratelimited("%s[%d]: %s at %08lx, pc %08lx (CFSR %08x)\n",
			    current->comm, task_pid_nr(current),
			    sig == SIGSEGV ? "segfault" : sig == SIGBUS ? "bus error" :
			    sig == SIGFPE ? "divide by zero" : "illegal instruction",
			    addr, regs->ARM_pc, cfsr);
	force_sig_fault(sig, code, (void __user *)addr);
}
