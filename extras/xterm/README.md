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

Dragging selects text and copies it to the clipboard; the middle button,
Shift-Insert and Ctrl-Shift-V paste. With XQuartz that is the Mac's
clipboard too. When the program has the mouse, as mc does, hold Shift to
select.

Where the X server has the RENDER extension (XQuartz and Xorg do; an
untrusted `ssh -X` login doesn't), the text is DejaVu Sans Mono,
anti-aliased, from `/usr/share/fonts/dejavu`: 12 pixels, or `-fs`. Without
it, xterm uses the server's own fonts: misc-fixed, DejaVu Sans Mono or
Lucida Typewriter, 13 pixels.

The terminal itself is `../lib/vt/vtpane.h`, a header for any X11 program
that uses `../lib/x11.h`; `xmc` (in `../mc`) uses it for its terminal tabs.
Its history is kept in RAM, compressed, up to 256 KB a terminal.
