# xedit

An X11 text editor for files on the RP2350, styled like `xmc` and `xpulse`.
It has menus, file dialogs, a find and replace bar, and a status bar. From
this directory on the board, run `./build.sh` to build with the bundled `cc`.
The extras release also ships the binary. Start it from a host with an X
server and SSH forwarding enabled:

```sh
ssh -Y root@BOARD /mnt/sd/extras/xedit/xedit            # an untitled document
ssh -Y root@BOARD /mnt/sd/extras/xedit/xedit notes.txt  # open or create a file
```

The file name is optional. Without one, xedit starts an untitled document
and asks for a name when you save. To open files with F4 in `xmc`, set
`XEDITOR=/mnt/sd/extras/xedit/xedit` in its environment.

## Using it

The **File**, **Edit**, **View**, and **Help** menus hold every command,
with its shortcut beside it. Click a menu, or press F10 or Alt with the
menu's first letter, then use the arrow keys.

- **Open and Save As** show a file dialog. Double-click a folder to enter
  it, or click **Up** to go to the parent folder. You can also type a name
  or a path, including `~/`. Backspace in an empty name goes up a folder.
  Save As asks before it replaces an existing file.
- **Find** (Ctrl+F) and **Replace** (Ctrl+H) open a bar above the status
  bar. Enter finds the next match and Shift+Enter the previous one. The
  bar also has **Replace**, **Replace All**, and **Match case**. Esc closes it.
- New, Open, and Quit ask whether to save unsaved changes.
- The status bar shows the file name, whether it has unsaved changes, the
  cursor's line and column, the file size, and the zoom level. After a long
  jump, xedit counts lines in the background while you work.
- Drag the scroll bar, or use the mouse wheel, to move through the file
  without moving the cursor. Double-click a word to select it.

| Key | Action |
|---|---|
| Ctrl+N / Ctrl+O | New document, open a file |
| Ctrl+S / Ctrl+Shift+S | Save, save under another name |
| Ctrl+Q | Quit |
| Ctrl+Z / Ctrl+Y | Undo, redo |
| Ctrl+X / Ctrl+C / Ctrl+V | Cut, copy, paste up to 32 KiB |
| Ctrl+A | Select all |
| Shift+movement, drag | Select text |
| Ctrl+F / Ctrl+H | Find, find and replace |
| F3 / Shift+F3 | Next or previous match |
| Ctrl+L / Ctrl+G | Go to a line number or a byte offset |
| Ctrl+Home / Ctrl+End | Start or end of the file |
| Ctrl++ / Ctrl+- / Ctrl+0 | Zoom in, zoom out, actual size |
| F1 | Keyboard shortcuts |

## Large files

The editor opens a regular file without scanning it. It fetches visible
text in 4 KiB pages and can jump directly to any byte offset, even within
a long line. An on-disk piece tree records which spans to read from the
original file and from appended edits. The page cache, tree cache, and
128-entry undo history use fixed amounts of memory. Search and line-number
navigation scan incrementally. The view and cursor are byte-oriented.
Tabs occupy one cell, and non-ASCII and control bytes display as dots.
The editor preserves those bytes when reading and saving.

Keep large files on the SD card. xedit keeps scratch data in the file's
directory until it exits; an untitled document keeps it in `$TMPDIR`, or
`/tmp`. Save streams the result into a same-directory temporary file, then
replaces the destination after a successful write. The directory needs
enough free space for the scratch data and the full new file during a
save. The original file remains open for undo and may continue using disk
space after a save until you exit.

## Drawing

xedit follows the patterns of `../lib/x11.h`: it sends each row, bar, and
widget only when what it shows changes. Menus and dialogs save the area
they cover on the X server and restore it with one copy when they close.
With the RENDER extension, text uses anti-aliased DejaVu fonts, and menus
and dialogs have rounded corners and soft shadows. Without RENDER, it
falls back to the server's core fonts.
