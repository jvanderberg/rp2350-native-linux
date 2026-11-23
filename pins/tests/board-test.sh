#!/bin/sh
# board-test.sh: the pin drivers on the board, with no second board. Uses
# free, unwired pins (GP20, GP21, GP26) and the SPI0 loopback jumper
# (GP19 -> GP16, README.md "Wiring"). Prints PASS/FAIL lines and
# "board-test: N of M passed".
T=/usr/libexec/pins-tests
E=/usr/share/examples
pass=0 total=0
check() {  # NAME then a command; PASS when it exits 0
  name=$1; shift
  total=$((total + 1))
  if "$@"; then pass=$((pass + 1)); echo "PASS $name"; else echo "FAIL $name"; fi
}
has() { grep -q -- "$1"; }
# pinpeek meas, up to 3 tries: it samples the pad from a process, and a
# stretch without the CPU loses edges (reads low)
meas() { $T/pinpeek meas "$@" || $T/pinpeek meas "$@" || $T/pinpeek meas "$@"; }

# ---- GPIO
check gpiodetect sh -c 'gpiodetect | grep -q "rp2350-gpio. (48 lines)"'
gpioinfo -c gpiochip0 > /tmp/gi
check gpioinfo-names grep -q 'line  20:.*"GP20"' /tmp/gi
check gpioinfo-reserved sh -c 'gpioget -c gpiochip0 PSRAM_CS 2>/dev/null; [ $? != 0 ]'
check gpioset-1 sh -c 'gpioset -c gpiochip0 -t0 GP20=1 && $0 regs 20 | grep -q "FUNCSEL 5 (SIO).* in 1 out 1 oe 1"' $T/pinpeek
check gpioset-0 sh -c 'gpioset -c gpiochip0 -t0 GP20=0 && $0 regs 20 | grep -q "in 0 out 0 oe 1"' $T/pinpeek
check gpioget-pull-up sh -c '[ "$(gpioget -c gpiochip0 -b pull-up --numeric GP21)" = 1 ]'
check gpioget-pull-down sh -c '[ "$(gpioget -c gpiochip0 -b pull-down --numeric GP21)" = 0 ]'
check gpio-edge sh -c "$E/pins/gpio-edge 26 4 | tail -1 | grep -q '4 of 4 edges'"
check gpio-blink sh -c "$E/pins/gpio-blink 20 2 50 | grep -c 'pad reads' | grep -q 4"

# ---- I2C: I2C1 (GP6/GP7) has nothing on it: every address NACKs, fast
check i2cdetect-1 sh -c 'i2cdetect -y 1 > /tmp/i2c; r=$?; cat /tmp/i2c; [ $r = 0 ] && [ "$(grep -c -- "-- -- --" /tmp/i2c)" = 8 ]'
check i2cget-nack sh -c '! i2cget -y 1 0x42 0 2>/dev/null'

# ---- SPI0 loopback (GP19 -> GP16)
check spidev_test sh -c 'spidev_test -D /dev/spidev0.0 -s 1000000 -p "\x12\x34\x56" -v 2>&1 | grep -q "RX.*12 34 56"'
check spi-xfer sh -c "$E/pins/spi-xfer 12 34 56 | grep -q 'rx: 12 34 56'"

# ---- ADC
check adc-temp sh -c 'r=$(cat /sys/bus/iio/devices/iio:device0/in_temp_raw) && [ "$r" -gt 600 ] && [ "$r" -lt 1100 ]'
check adc-read sh -c "$E/pins/adc-read | tail -1 | grep -q 'temp raw'"

# ---- PWM: GP26 at 1 kHz 25%, measured from the pad
P=/sys/class/pwm/pwmchip0
echo 26 > $P/export 2>/dev/null
echo 1000000 > $P/pwm26/period && echo 250000 > $P/pwm26/duty_cycle && echo 1 > $P/pwm26/enable
$T/pinpeek regs 26 | grep -q 'FUNCSEL 4 (PWM)'; check pwm-funcsel [ $? = 0 ]
check pwm-1khz-25 meas 26 500 1000 25
check pwm-gpio-busy sh -c '! gpioget -c gpiochip0 GP26 2>/dev/null && $0 regs 26 | grep -q "FUNCSEL 4 (PWM)"' $T/pinpeek
echo 0 > $P/pwm26/enable; echo 26 > $P/unexport
$T/pinpeek regs 26 | grep -q 'FUNCSEL 5 (SIO)'; check pwm-off [ $? = 0 ]

# ---- PIO: squarewave on GP20, measured from the pad
cd $E/pio
./squarewave 20 1000 4 & sleep 1
check pio-squarewave meas 20 500 1000 50
wait
check pio-fifo sh -c './pio-fifo > /tmp/pf 2>&1; r=$?; cat /tmp/pf; [ $r = 0 ]'
check pio-dma sh -c './pio-dma > /tmp/pd 2>&1; r=$?; cat /tmp/pd; [ $r = 0 ]'
check ws2812 sh -c './ws2812 20 10 ff8000; [ $? = 0 ]'

# ---- PIO from ATRANS programs (musl, the board's cc): /dev/pio mapped
# outside the window through spare MPU regions (README.md "PIO from ATRANS
# programs"). atrans-pio covers the negative cases (SIGSEGV without the
# mapping, after munmap) and fork; the kernel must not notice them.
A=$E/pio/atrans
if [ -x $T/atrans-pio ]; then
  check atrans-pio sh -c "$T/atrans-pio > /tmp/ap 2>&1; r=\$?; cat /tmp/ap; exit \$r"
  $A/squarewave 20 1000 4 & sleep 1
  check atrans-squarewave meas 20 500 1000 50
  wait
  check atrans-pio-fifo sh -c "$A/pio-fifo > /tmp/pf 2>&1; r=\$?; cat /tmp/pf; [ \$r = 0 ]"
  check atrans-pio-dma sh -c "$A/pio-dma > /tmp/pd 2>&1; r=\$?; cat /tmp/pd; [ \$r = 0 ]"
  check atrans-ws2812 sh -c "$A/ws2812 20 10 0080ff; [ \$? = 0 ]"
  # two ATRANS processes switching, each with its own block mapped
  $A/squarewave 20 1000 3 & sleep 1
  check atrans-two-blocks sh -c "$T/atrans-pio 2 > /tmp/ap2 2>&1; r=\$?; tail -1 /tmp/ap2; exit \$r"
  wait
fi
# the same built by tcc (`cc prog.c -lpio`)
if [ -x $T/tcc/atrans-pio ]; then
  check tcc-atrans-pio sh -c "$T/tcc/atrans-pio > /tmp/tp 2>&1; r=\$?; cat /tmp/tp; exit \$r"
  $T/tcc/squarewave 20 1000 4 & sleep 1
  check tcc-squarewave meas 20 500 1000 50
  wait
  check tcc-pio-fifo sh -c "$T/tcc/pio-fifo > /tmp/tf 2>&1; r=\$?; cat /tmp/tf; [ \$r = 0 ]"
fi
check no-oops sh -c '! dmesg | grep -E "Oops|Unhandled|BUG:|Kernel panic"'

# ---- MicroPython
check mp-pin micropython -c "from machine import Pin; p=Pin(20, Pin.OUT); p.value(1); assert Pin(20).value()==1; p.value(0); assert Pin(20).value()==0"
check mp-adc micropython -c "from machine import ADC; t=ADC(ADC.CORE_TEMP).read_temp(); print(t); assert 0 < t < 80"
check mp-pio-echo sh -c "micropython $E/pio/pio_echo.py | tee /tmp/pe; grep -q '4 of 4 right' /tmp/pe && grep -q '256 of 256 right' /tmp/pe"
check mp-pin-irq sh -c "micropython $T/irq_test.py 26 | tee /tmp/pi; grep -q '20 of 20' /tmp/pi"
check mp-pwm sh -c "micropython -c 'from machine import Pin, PWM; p=PWM(Pin(26)); p.freq(2000); p.duty_u16(16384); import time; time.sleep(5); p.deinit()' & sleep 2; $T/pinpeek meas 26 300 2000 25 || $T/pinpeek meas 26 300 2000 25 || $T/pinpeek meas 26 300 2000 25; r=\$?; wait; exit \$r"

# ---- the PIO self-tests (pins/tests/pio: GP15, GP20,
# GP21, GP31, GP33). Thread stacks: uClibc's default is RLIMIT_STACK (8 MiB,
# capped at 2 MiB), more than nommu finds in one piece.
check pio-selftest sh -c "ulimit -s 256; $T/pio-selftest > /tmp/ps 2>&1; r=\$?; tail -1 /tmp/ps; grep FAIL /tmp/ps; exit \$r"
check rp2-selftest sh -c "micropython $T/rp2_selftest.py > /tmp/rs 2>&1; r=\$?; tail -1 /tmp/rs; grep FAIL /tmp/rs; exit \$r"
check mp-pio-ring sh -c "micropython $E/pio/pio_ring.py | grep -q '15 of 16'"
echo "board-test: $pass of $total passed"
[ $pass = $total ]
