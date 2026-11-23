# MicroPython Pin.irq with no wiring: an unwired pin's pull flipped 20
# times makes 20 edges (micropython irq_test.py [PIN]).
import sys
import time
from machine import Pin

pin = int(sys.argv[1]) if len(sys.argv) > 1 else 26
n = 0


def h(q):
    global n
    n += 1


p = Pin(pin, Pin.IN, Pin.PULL_DOWN)
time.sleep(0.1)
p.irq(h)
time.sleep(0.2)
for i in range(20):
    p.init(pull=Pin.PULL_UP if i % 2 == 0 else Pin.PULL_DOWN)
    time.sleep(0.05)
time.sleep(0.3)
print("Pin.irq: %d of 20 edges" % n)
p.irq(None)
p.deinit()
