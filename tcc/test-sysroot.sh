#!/bin/sh
# test-sysroot.sh MUSL_TARBALL OUT: a minimal static musl sysroot for testing
# the Thumb-2 tcc on the host (inside the rp2350-native-tcc image), when the
# userland sysroot isn't there: musl built Thumb ARMv7-M soft-float by
# arm-none-eabi-gcc, plus that compiler's thumb/v7-m/nofp libgcc.a.
# The thread pointer comes from the get_tls syscall (0xf0006) as on the
# board, and from cp15 when the kernel (qemu-arm) does not have it.
set -eu
TARBALL=$1 OUT=$2
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT
tar xzf "$TARBALL" -C "$B"
cd "$B"/musl-*
cat > arch/arm/pthread_arch.h <<'EOF'
static inline uintptr_t __get_tp()
{
	/* __ARM_NR_get_tls; r7 is the Thumb frame pointer (see syscall_arch.h) */
	register uintptr_t r0 __asm__("r0");
	uintptr_t tmp;
	__asm__ __volatile__ ("mov %1,r7 ; mov r7,%2 ; svc 0 ; mov r7,%1"
		: "=r"(r0), "=&r"(tmp) : "r"(0xf0006) : "memory");
	if (r0 > -4096UL)
		__asm__ ("mrc p15,0,%0,c13,c0,3" : "=r"(r0));
	return r0;
}

#define TLS_ABOVE_TP
#define GAP_ABOVE_TP 8

#define MC_PC arm_pc
EOF
# __aeabi_read_tp (called by code built with -mtp=soft) the same way
cat > src/thread/arm/__aeabi_read_tp.c <<'EOF'
#include <stdint.h>
#include "pthread_impl.h"
uintptr_t __aeabi_read_tp(void) { return __get_tp(); }
EOF
rm -f src/thread/arm/__aeabi_read_tp.s src/thread/arm/__aeabi_read_tp.S
FLAGS="-mthumb -march=armv7-m -mfloat-abi=soft"
CC="arm-none-eabi-gcc $FLAGS" CROSS_COMPILE=arm-none-eabi- \
  ./configure --target=arm-linux-musleabi --prefix="$OUT" --disable-shared \
  CFLAGS="-Os" >/dev/null
make -j"$(nproc)" >/dev/null
make install >/dev/null
cp "$(arm-none-eabi-gcc $FLAGS -print-libgcc-file-name)" "$OUT/lib/libgcc.a"
# the kernel's UAPI headers (linux/*.h, asm/*.h), from KERNEL_TARBALL
if [ -n "${KERNEL_TARBALL:-}" ]; then
	mkdir "$B/k"
	tar xJf "$KERNEL_TARBALL" -C "$B/k" --strip-components=1
	make -s -C "$B/k" ARCH=arm INSTALL_HDR_PATH="$OUT" headers_install
fi
echo "test sysroot in $OUT"
