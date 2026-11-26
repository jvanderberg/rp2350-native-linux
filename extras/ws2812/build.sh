#!/bin/sh
# Build ws2812, WS2812 LEDs driven by PIO. Needs pioasm and libpio, which
# are on the board.
#   ./build.sh              with cc (tcc on the board)
#   CC=gcc ./build.sh       with another compiler
cd "$(dirname "$0")" || exit 1
CC=${CC:-cc}
pioasm ws2812.pio ws2812.pio.h || exit 1
# Link to a temporary name, then rename: safe while the old one runs.
$CC -o ws2812.new ws2812.c -lpio && mv ws2812.new ws2812 && echo "built ws2812"
