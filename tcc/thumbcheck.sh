#!/bin/sh
# thumbcheck.sh FILE...: check that the code in each object or archive
# (read through its ARM mapping symbols, which tcc's executables keep
# only when linked with -g) is ARMv7-M Thumb: no ARM
# state code ($a), no VFP/NEON, no coprocessor instructions, nothing the
# disassembler does not know.  Prints offenders; exit status 1 if any.
# ALLOW (a regex of 'func: insn' lines) skips known ones.
OBJDUMP=${OBJDUMP:-arm-none-eabi-objdump}
bad=0
for f in "$@"; do
	if $OBJDUMP -t --special-syms "$f" | grep -q ' \$a$\| \$a\.'; then
		echo "$f: ARM state code (\$a mapping symbols)"
		bad=1
	fi
	out=$($OBJDUMP -d "$f" | awk '
		/^[0-9a-f]+ <.*>:$/ { fn = $2 }
		/^ +[0-9a-f]+:\t/ {
			n = split($0, a, "\t")
			insn = a[3]; sub(/[ ;].*/, "", insn)
			if (insn ~ /^(v[a-z]|mrc|mcr|mrrc|mcrr|cdp|ldc|stc|fld|fst|fmrx|fmxr|fmstat|wfi|smc|hvc)/ \
			    || insn ~ /undefined|illegal/ || a[3] ~ /undefined|UNDEFINED|<und>/)
				print fn " " a[3]
		}' | grep -v -E "${ALLOW:-^\$}")
	if [ -n "$out" ]; then
		echo "$f:"; echo "$out" | head -20
		bad=1
	fi
done
exit $bad
