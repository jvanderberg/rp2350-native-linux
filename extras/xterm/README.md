# xterm - a terminal in an X11 window

A terminal in a window on an X server, such as XQuartz on a Mac, built
from the same emulator as `tm` (`../lib/vt`):

    ./build.sh
    DISPLAY=HOST:0 ./xterm [-fs PIXELS] [-g COLSxROWS] [-T TITLE] [-hold]
                           [-e COMMAND [ARGS...]]

It runs your shell, or the command after `-e`, with
`TERM=screen-256color`: vi, less, top and mc work in it. The wheel and
Shift-PgUp/PgDn scroll back through history, unless the program asked
for the mouse. The window can be resized, and only the cells that changed
are sent to the X server.

The terminal itself is `../lib/vt/vtpane.h`, a header for any X11 program
that uses `../lib/x11.h`; `xmc` (in `../mc`) uses it for its terminal tabs.
Its history is kept in RAM, compressed, up to 256 KB a terminal.
