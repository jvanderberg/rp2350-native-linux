// SPDX-License-Identifier: MIT
/*
 * x11.h - a tiny X11 client for the extras, the window counterpart of ansi.h.
 *
 * It speaks the X11 protocol directly over a socket, so there is no Xlib to
 * build or link: include this file and compile with cc. The board is the
 * client; the screen belongs to an X server elsewhere, such as XQuartz on a
 * Mac.
 *
 *     if (x_open(NULL) < 0)                 // NULL: use $DISPLAY (HOST:0)
 *         return 1;
 *     x_window("demo", 320, 200);
 *     for (;;) {
 *         struct x_event e;
 *
 *         while (x_event(&e))               // never blocks
 *             if (e.type == XE_CLOSE || (e.type == XE_KEY_DOWN && e.key == 'q'))
 *                 return 0;
 *         x_color(x_rgb(0, 0, 80));
 *         x_fill_rect(0, 0, x_width, x_height);
 *         x_color(x_rgb(255, 255, 255));
 *         x_text(10, 20, "hello");
 *         x_present();                      // show the frame
 *         x_wait(33);                       // until input arrives, or 33 ms
 *     }
 *
 * Drawing goes to a back buffer the size of the window and appears only at
 * x_present(), so frames never flicker; when the window is uncovered the
 * back buffer is copied again (in x_event, between frames) without asking
 * the program. When the window is resized it keeps its picture, and new
 * parts show the background (x_background) until the program draws again.
 *
 *   colours   x_rgb(r, g, b) makes a pixel value; x_color() sets the colour
 *             for the drawing calls below. x_line_width() sets line width.
 *   shapes    x_fill_rect, x_rect, x_line, x_fill_ellipse, x_ellipse
 *             (ellipses by bounding box, like rectangles)
 *   text      x_text(x, y, s): y is the baseline; the server's "fixed"
 *             font, usually 6x13 pixels per character, unless x_font has
 *             found another by name and x_text_font chosen it.
 *             x_text_width, x_font_ascent and x_font_descent measure.
 *             x_font_cell opens a fixed-width font cheaply, even a big
 *             Unicode one, and x_text16 draws code points in it;
 *             x_text_utf8 draws UTF-8 (x_text is Latin-1).
 *   batches   x_fill_rects, x_segments: many in one request; x_fill_poly
 *             for a convex polygon, x_polyline for connected lines.
 *             Arrays of shorts (see each function).
 *   images    x_image(x, y, w, h, pixels, stride): 32-bit pixels made by
 *             x_rgb, stride in pixels. Costly over a slow link: 256 KB for
 *             a 320x200 frame, where shapes cost a few bytes each.
 *   canvases  x_canvas(w, h) makes an off-screen image; x_target(c) sends
 *             the drawing calls there (0: back to the frame); x_blit(c, ...)
 *             copies part of it into the frame. Draw something once, then
 *             reuse it for 28 bytes a frame.
 *             x_frame() is the frame itself, to copy from: to save what a
 *             menu covers, or to scroll part of the frame.
 *   masks     x_mask(w, h) makes a 1-bit image, all clear; x_mask_rects
 *             sets rectangles of it. x_blit_masked copies only the pixels
 *             a mask has set, for shapes that aren't rectangles. For a mask
 *             used again and again, make a stencil of it once (x_stencil)
 *             and copy with x_blit_stencil: the server does the costly part
 *             of a masked copy once instead of every time.
 *             x_fill_stippled fills a rectangle only where a small mask,
 *             repeated, is set: a dither, for a see-through tint.
 *             x_fill_poly_tiled fills any polygon with a canvas repeated:
 *             a gradient under a graph, in one request.
 *             x_target_window() draws straight onto the window, over what
 *             x_present() last showed, leaving the back buffer alone.
 *   smooth    with the RENDER extension, which most servers have
 *             (x_has_render; see "RENDER" below): x_font_ttf opens a
 *             TrueType font for anti-aliased text in the calls above (the
 *             board has DejaVu Sans and DejaVu Sans Mono, regular and
 *             bold); with the HIDPI extension (crystalX on a high-density
 *             screen) its glyphs are made at the screen's own resolution,
 *             while sizes stay in X pixels. x_aa_fill_ellipse,
 *             x_aa_ellipse, x_aa_round_rect, x_aa_polygon, x_aa_polyline
 *             and x_aa_triangles draw shapes
 *             with smooth edges at an opacity, in 16.16 fixed point
 *             (X_FX(2.5) is two and a half pixels); x_blend_rect lays a
 *             see-through rectangle over what is there. Without RENDER,
 *             x_font_ttf gives font 0 and the others return 0, drawing
 *             nothing, for the program to draw the plain way.
 *   cursors   x_cursor(X_CURSOR_HAND) over something to click,
 *             X_CURSOR_TEXT over text, X_CURSOR_DEFAULT back again (and
 *             WAIT, RESIZE_H, RESIZE_V): smooth ones with RENDER
 *   clipboard x_set_clipboard(text, len) copies (and sets the primary
 *             selection); x_request_paste() asks for the clipboard, whose
 *             text then arrives as an XE_PASTE event, e.text and e.len
 *   input     x_event() returns key down and up (a real release, unlike a
 *             terminal; a held key repeats as up/down pairs), mouse buttons
 *             and motion, resize (draw again) and close. e.key is ASCII
 *             for printable keys, else an X_KEY_* code; e.mods says which
 *             of Shift, Ctrl and Alt were held (X_MOD_*).
 *             x_pointer() asks where the mouse is, even outside the window.
 *             x_wait_fds(fds, n, ms) sleeps until an event, or one of
 *             the program's own file descriptors can be read.
 *   window    x_background(pixel), before x_window, for what the window
 *             shows where nothing is drawn yet; x_min_size(w, h) after it
 *   other     x_sync() waits until the server has drawn everything sent
 *             (call it after x_present() so a slow link can't build up a
 *             queue of old frames), x_wait(ms), x_flush(), x_fd() and
 *             x_pending() for your own poll(), x_close()
 *   fences    x_fence() after x_present(), and x_fence_wait() before the
 *             next one, instead of x_sync(): the program draws the next
 *             frame while the last one travels, and still never more than
 *             one frame ahead of the server
 *
 * Define X11_TIMING before including this to count the time spent sending
 * in x_write_us (microseconds).
 *
 * Needs a TrueColor display (any current server, XQuartz included) that
 * lets this machine connect. Either log in with `ssh -Y` (or -X), which sets
 * DISPLAY and stores a cookie with xauth, or, on the machine with the
 * screen, allow network clients (XQuartz: Settings > Security) and run
 * `xhost +THIS-ADDRESS`. A MIT-MAGIC-COOKIE-1 for the display in
 * $XAUTHORITY (default ~/.Xauthority) is sent if there is one.
 */
#ifndef DEMO_X11_H
#define DEMO_X11_H

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum {
    XE_NONE, XE_KEY_DOWN, XE_KEY_UP, XE_BUTTON_DOWN, XE_BUTTON_UP,
    XE_MOTION, XE_RESIZE, XE_CLOSE, XE_PASTE
};

/* X keysyms for keys that aren't printable characters */
#define X_KEY_BACKSPACE 0xff08
#define X_KEY_TAB       0xff09
#define X_KEY_RETURN    0xff0d
#define X_KEY_ESCAPE    0xff1b
#define X_KEY_HOME      0xff50
#define X_KEY_LEFT      0xff51
#define X_KEY_UP        0xff52
#define X_KEY_RIGHT     0xff53
#define X_KEY_DOWN      0xff54
#define X_KEY_PAGE_UP   0xff55
#define X_KEY_PAGE_DOWN 0xff56
#define X_KEY_END       0xff57
#define X_KEY_F1        0xffbe          /* F2.. follow on */
#define X_KEY_SHIFT_L   0xffe1
#define X_KEY_SHIFT_R   0xffe2
#define X_KEY_CONTROL_L 0xffe3
#define X_KEY_CONTROL_R 0xffe4
#define X_KEY_DELETE    0xffff
#define X_KEY_INSERT    0xff63
#define X_KEY_KP_ENTER  0xff8d
#define X_KEY_BACKTAB   0xfe20          /* Shift-Tab (ISO_Left_Tab) */

/* x_event.mods: the modifier keys held */
#define X_MOD_SHIFT     1
#define X_MOD_CTRL      4
#define X_MOD_ALT       8               /* Mod1: Alt, or Option on a Mac */

struct x_event {
    int type;                           /* XE_* */
    int key;                            /* keys: ASCII or X_KEY_* */
    int button;                         /* buttons: 1 left, 2 middle, 3 right, 4/5 wheel */
    int x, y;                           /* keys, buttons, motion: mouse in the window */
    int mods;                           /* keys, buttons: X_MOD_* held */
    char *text;                         /* XE_PASTE: the clipboard's text, UTF-8, */
    int len;                            /* len bytes and a NUL; kept until the next x_event */
};

static int x_width, x_height;           /* the window's size now */
static unsigned long long x_sent;       /* bytes sent to the X server so far */

/* ---- internals ---- */

static int xi_fd = -1, xi_dead;
static unsigned char xi_out[16384];
static size_t xi_out_len;
static unsigned int xi_seq;             /* sequence number of the last request */
static unsigned int xi_id_base, xi_id_next;
static unsigned int xi_root, xi_depth, xi_bpp, xi_max_req;
static unsigned int xi_rmask, xi_gmask, xi_bmask;
static int xi_image_msb;                /* server wants image bytes big-endian */
static unsigned int xi_win, xi_back, xi_gc, xi_font, xi_color = 0xffffffffu;
static unsigned int xi_target;          /* canvas the drawing calls go to; 0: the frame */
static unsigned int xi_wm_protocols, xi_wm_delete;
static int xi_min_kc, xi_kc_count, xi_syms_per;
static unsigned int *xi_keysyms;
static struct x_event xi_queue[64];
static int xi_qhead, xi_qlen;

static unsigned int xi_get16(const unsigned char *p) { return p[0] | (unsigned int)p[1] << 8; }
static unsigned int xi_get32(const unsigned char *p) { return xi_get16(p) | xi_get16(p + 2) << 16; }

static void xi_push(int type, int key, int button, int x, int y, int mods)
{
    struct x_event *e;

    if (xi_qlen == (int)(sizeof xi_queue / sizeof xi_queue[0]))
        return;                         /* full: drop it */
    e = &xi_queue[(xi_qhead + xi_qlen++) % (int)(sizeof xi_queue / sizeof xi_queue[0])];
    e->type = type;
    e->key = key;
    e->button = button;
    e->x = x;
    e->y = y;
    e->mods = mods;
    e->text = NULL;
    e->len = 0;
}

/* The connection is gone: say so once, then ignore everything. */
static void xi_lost(void)
{
    if (!xi_dead) {
        xi_dead = 1;
        xi_push(XE_CLOSE, 0, 0, 0, 0, 0);
    }
}

#ifdef X11_TIMING
#include <time.h>
static unsigned long long x_write_us;   /* time spent in write() */
static unsigned long long xi_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000u + (unsigned long long)ts.tv_nsec / 1000u;
}
#endif

static void xi_write(const void *buf, size_t n)
{
    const unsigned char *p = buf;

    while (n > 0 && !xi_dead) {
#ifdef X11_TIMING
        unsigned long long t = xi_us();
        ssize_t w = write(xi_fd, p, n);

        x_write_us += xi_us() - t;
#else
        ssize_t w = write(xi_fd, p, n);
#endif

        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            xi_lost();
            return;
        }
        p += w;
        n -= (size_t)w;
        x_sent += (unsigned long long)w;
    }
}

static inline void x_flush(void)
{
    xi_write(xi_out, xi_out_len);
    xi_out_len = 0;
}

static void xi_put8(unsigned int v) { xi_out[xi_out_len++] = (unsigned char)v; }
static void xi_put16(unsigned int v) { xi_put8(v & 255); xi_put8(v >> 8 & 255); }
static void xi_put32(unsigned int v) { xi_put16(v & 0xffff); xi_put16(v >> 16); }

/* Start a request len 4-byte words long; at most sizeof xi_out is buffered. */
static void xi_req(int opcode, int data, unsigned int len)
{
    if (xi_out_len + len * 4 > sizeof xi_out)
        x_flush();
    xi_put8((unsigned int)opcode);
    xi_put8((unsigned int)data);
    xi_put16(len);
    xi_seq++;
}

static int xi_read(unsigned char *buf, size_t n)
{
    size_t off = 0;

    while (off < n && !xi_dead) {
        ssize_t r = read(xi_fd, buf + off, n - off);

        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            xi_lost();
            return -1;
        }
        off += (size_t)r;
    }
    return xi_dead ? -1 : 0;
}

static unsigned int xi_new_id(void) { return xi_id_base | xi_id_next++; }

static int xi_keysym(int keycode, int state)
{
    unsigned int *ks;
    unsigned int lo, hi;

    if (xi_keysyms == NULL || keycode < xi_min_kc || keycode >= xi_min_kc + xi_kc_count)
        return 0;
    ks = xi_keysyms + (keycode - xi_min_kc) * xi_syms_per;
    lo = ks[0];
    hi = xi_syms_per > 1 && ks[1] != 0 ? ks[1] : lo;
    if (lo >= 'a' && lo <= 'z' && hi == lo)
        hi = lo - 'a' + 'A';
    if (lo >= 'a' && lo <= 'z' && (state & 2))      /* Caps Lock */
        return (int)((state & 1) ? lo : hi);
    return (int)((state & 1) ? hi : lo);             /* Shift */
}

static void xi_copy_back(void)
{
    xi_req(62, 0, 7);                   /* CopyArea */
    xi_put32(xi_back);
    xi_put32(xi_win);
    xi_put32(xi_gc);
    xi_put32(0);
    xi_put32(0);
    xi_put16((unsigned int)x_width);
    xi_put16((unsigned int)x_height);
}

static void xi_forget_picture(unsigned int d);
static void xi_selection_event(const unsigned char *b);

static unsigned int xi_bg;              /* the window's background (x_background) */
static int xi_expose;                   /* the window needs the back buffer again */

/* A back buffer for the window's size now. The old picture is copied into
 * it, on a background for the new part, so that until the program draws
 * again the window shows what it did, not whatever the new pixmap held. */
static void xi_make_back(int old_w, int old_h)
{
    unsigned int old = xi_back;

    xi_back = xi_new_id();
    xi_req(53, (int)xi_depth, 4);       /* CreatePixmap */
    xi_put32(xi_back);
    xi_put32(xi_win);
    xi_put16((unsigned int)x_width);
    xi_put16((unsigned int)x_height);
    if (old == 0)
        return;
    xi_req(56, 0, 3 + 1);               /* ChangeGC: foreground */
    xi_put32(xi_gc);
    xi_put32(0x4);
    xi_put32(xi_bg);
    xi_req(70, 0, 3 + 2);               /* PolyFillRectangle */
    xi_put32(xi_back);
    xi_put32(xi_gc);
    xi_put32(0);
    xi_put16((unsigned int)x_width);
    xi_put16((unsigned int)x_height);
    xi_req(62, 0, 7);                   /* CopyArea: the old picture */
    xi_put32(old);
    xi_put32(xi_back);
    xi_put32(xi_gc);
    xi_put32(0);
    xi_put32(0);
    xi_put16((unsigned int)(old_w < x_width ? old_w : x_width));
    xi_put16((unsigned int)(old_h < x_height ? old_h : x_height));
    xi_req(56, 0, 3 + 1);               /* ChangeGC: the program's colour back */
    xi_put32(xi_gc);
    xi_put32(0x4);
    xi_put32(xi_color);
    xi_forget_picture(old);
    xi_req(54, 0, 2);                   /* FreePixmap */
    xi_put32(old);
}

static unsigned int xi_fence_seq;
static int xi_fence_on;                 /* a fence's reply is still to come */

static void xi_handle(const unsigned char *b)
{
    int x = (short)xi_get16(b + 24), y = (short)xi_get16(b + 26);

    if (xi_fence_on && (b[0] & 0x7f) <= 1 && xi_get16(b + 2) == xi_fence_seq) {
        xi_fence_on = 0;                /* the fence's reply (or error) */
        return;
    }
    switch (b[0] & 0x7f) {
    case 0:
        fprintf(stderr, "x11: error %d from request %d\n", b[1], b[10]);
        break;
    case 2:
        xi_push(XE_KEY_DOWN, xi_keysym(b[1], (int)xi_get16(b + 28)), 0, x, y, (int)xi_get16(b + 28));
        break;
    case 3:
        xi_push(XE_KEY_UP, xi_keysym(b[1], (int)xi_get16(b + 28)), 0, x, y, (int)xi_get16(b + 28));
        break;
    case 4:
        xi_push(XE_BUTTON_DOWN, 0, b[1], x, y, (int)xi_get16(b + 28));
        break;
    case 5:
        xi_push(XE_BUTTON_UP, 0, b[1], x, y, (int)xi_get16(b + 28));
        break;
    case 6:
        xi_push(XE_MOTION, 0, 0, x, y, (int)xi_get16(b + 28));
        break;
    case 12:                            /* Expose: show the back buffer again, */
        xi_expose = 1;                  /* in x_event, between frames, not */
        break;                          /* now, perhaps halfway through one */
    case 22: {                          /* ConfigureNotify */
        int w = (int)xi_get16(b + 20), h = (int)xi_get16(b + 22);

        if (w != x_width || h != x_height) {
            int ow = x_width, oh = x_height;

            x_width = w;
            x_height = h;
            xi_make_back(ow, oh);
            xi_push(XE_RESIZE, 0, 0, w, h, 0);
        }
        break;
    }
    case 29: case 30: case 31:          /* the clipboard */
        xi_selection_event(b);
        break;
    case 33:                            /* ClientMessage: the close button */
        if (b[1] == 32 && xi_get32(b + 8) == xi_wm_protocols &&
            xi_get32(b + 12) == xi_wm_delete)
            xi_push(XE_CLOSE, 0, 0, 0, 0, 0);
        break;
    }
}

/* Read one message. A reply's extra data goes to *extra (malloc'd) if extra
 * is not NULL, otherwise it is skipped. Returns the message type, or -1. */
static int xi_message(unsigned char *b, unsigned char **extra)
{
    size_t n;

    if (xi_read(b, 32) < 0)
        return -1;
    n = ((b[0] & 0x7f) == 1 || (b[0] & 0x7f) == 35) ? (size_t)xi_get32(b + 4) * 4 : 0;
    if (extra != NULL)
        *extra = NULL;
    if (n > 0) {
        unsigned char *p = malloc(n);

        if (p == NULL || xi_read(p, n) < 0) {
            free(p);
            return -1;
        }
        if (extra != NULL)
            *extra = p;
        else
            free(p);
    }
    return b[0] & 0x7f;
}

/* Send what is buffered and wait for the reply to the last request; events
 * that arrive first are queued. Returns 0, or -1 on an error or lost link. */
static int xi_roundtrip(unsigned char *b, unsigned char **extra)
{
    /* Remember which reply: handling an event below can send requests
     * (repainting after Expose, a new back buffer after a resize). */
    unsigned int want = xi_seq & 0xffff;

    x_flush();
    for (;;) {
        int t = xi_message(b, extra);

        if (t < 0)
            return -1;
        if ((t == 0 || t == 1) && xi_get16(b + 2) == want) {
            if (t == 0) {
                xi_handle(b);
                return -1;
            }
            return 0;
        }
        if (extra != NULL) {
            free(*extra);
            *extra = NULL;
        }
        xi_handle(b);
    }
}

static unsigned int xi_atom(const char *name)
{
    unsigned char b[32];
    unsigned int n = (unsigned int)strlen(name);

    xi_req(16, 0, 2 + (n + 3) / 4);     /* InternAtom */
    xi_put16(n);
    xi_put16(0);
    for (unsigned int i = 0; i < n; i++)
        xi_put8((unsigned char)name[i]);
    while (n++ & 3)
        xi_put8(0);
    return xi_roundtrip(b, NULL) < 0 ? 0 : xi_get32(b + 8);
}

static int xi_connect_unix(const char *path)
{
    struct sockaddr_un sa;
    int s;

    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof sa.sun_path || (s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
        return -1;
    strcpy(sa.sun_path, path);
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static int xi_connect_tcp(const char *host, int port)
{
    struct addrinfo hints, *res, *ai;
    char service[16];
    int s = -1, one = 1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof service, "%d", port);
    if (getaddrinfo(host, service, &hints, &res) != 0)
        return -1;
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        if ((s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) < 0)
            continue;
        if (connect(s, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(s);
        s = -1;
    }
    freeaddrinfo(res);
    if (s >= 0)
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return s;
}

/* DISPLAY is HOST:N[.S] (TCP port 6000+N), :N or unix:N (the local socket),
 * or a socket path such as XQuartz's /private/tmp/.../org.xquartz:0. */
static int xi_connect(const char *d)
{
    char host[256], path[64];
    const char *colon = strrchr(d, ':');
    size_t hl;

    if (d[0] == '/')
        return xi_connect_unix(d);
    if (colon == NULL || (hl = (size_t)(colon - d)) >= sizeof host)
        return -1;
    memcpy(host, d, hl);
    host[hl] = '\0';
    if (hl == 0 || strcmp(host, "unix") == 0) {
        snprintf(path, sizeof path, "/tmp/.X11-unix/X%d", atoi(colon + 1));
        return xi_connect_unix(path);
    }
    return xi_connect_tcp(host, 6000 + atoi(colon + 1));
}

/* The MIT-MAGIC-COOKIE-1 for display d (connected on xi_fd) from the
 * authority file, looked up as Xlib does: a local or loopback connection
 * matches family Local with this host's name, a TCP one family Internet
 * (IPv4) or Internet6 with the server's address; family Wild matches any.
 * DISPLAY=THIS-HOST'S-NAME:N counts as local, as xauth stores it so.
 * Returns its length (at most 64 bytes, into cookie), or 0 if none. */
static size_t xi_cookie(const char *d, unsigned char *cookie)
{
    static const char proto[] = "MIT-MAGIC-COOKIE-1";
    const char *colon = strrchr(d, ':'), *file = getenv("XAUTHORITY"), *home;
    char path[512], self[256] = "";
    unsigned char peer[16], f[4][256];
    unsigned int flen[4], peerlen = 0, peerfamily = 0;
    size_t numlen, found = 0;
    int local = 1;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    FILE *fp;

    if (colon == NULL)
        return 0;
    for (numlen = 0; colon[1 + numlen] >= '0' && colon[1 + numlen] <= '9'; numlen++)
        ;
    if (getpeername(xi_fd, (struct sockaddr *)&ss, &sl) == 0) {
        if (ss.ss_family == AF_INET) {
            memcpy(peer, &((struct sockaddr_in *)&ss)->sin_addr, 4);
            peerlen = 4;
            local = peer[0] == 127;
        } else if (ss.ss_family == AF_INET6) {
            const struct in6_addr *a = &((struct sockaddr_in6 *)&ss)->sin6_addr;

            if (IN6_IS_ADDR_V4MAPPED(a)) {
                memcpy(peer, a->s6_addr + 12, 4);
                peerlen = 4;
                local = peer[0] == 127;
            } else {
                memcpy(peer, a->s6_addr, 16);
                peerlen = 16;
                peerfamily = 6;
                local = IN6_IS_ADDR_LOOPBACK(a);
            }
        }
    }
    gethostname(self, sizeof self - 1);
    if (!local && (size_t)(colon - d) == strlen(self) && memcmp(d, self, strlen(self)) == 0)
        local = 1;
    if (file == NULL || *file == '\0') {
        if ((home = getenv("HOME")) == NULL)
            return 0;
        snprintf(path, sizeof path, "%s/.Xauthority", home);
        file = path;
    }
    if ((fp = fopen(file, "rb")) == NULL)
        return 0;
    for (;;) {
        int hi = getc(fp), lo = getc(fp), i;
        unsigned int family;

        if (hi == EOF || lo == EOF)
            break;
        family = (unsigned int)(hi << 8 | lo);
        /* address, display number, protocol name, data */
        for (i = 0; i < 4; i++) {
            unsigned int n;

            if ((hi = getc(fp)) == EOF || (lo = getc(fp)) == EOF)
                break;
            n = (unsigned int)(hi << 8 | lo);
            flen[i] = n;
            if (n > sizeof f[i] ? fseek(fp, (long)n, SEEK_CUR) != 0 : fread(f[i], 1, n, fp) != n)
                break;
        }
        if (i < 4)
            break;
        if (flen[1] != numlen || memcmp(f[1], colon + 1, numlen) != 0 ||
            flen[2] != sizeof proto - 1 || memcmp(f[2], proto, sizeof proto - 1) != 0 || flen[3] > 64)
            continue;
        if (family == 65535 ||
            (local && family == 256 && flen[0] == strlen(self) && memcmp(f[0], self, flen[0]) == 0) ||
            (!local && peerlen && family == peerfamily && flen[0] == peerlen &&
             memcmp(f[0], peer, peerlen) == 0)) {
            memcpy(cookie, f[3], flen[3]);
            found = flen[3];
            break;
        }
    }
    fclose(fp);
    return found;
}

/* Pick what we need out of the set-up reply. */
static int xi_parse_setup(const unsigned char *b, size_t len)
{
    const unsigned char *s, *p, *end = b + len;
    unsigned int nformats = b[21], visual;

    xi_id_base = xi_get32(b + 4);
    xi_max_req = xi_get16(b + 18);
    xi_image_msb = b[22];
    xi_min_kc = b[26];
    xi_kc_count = b[27] - b[26] + 1;
    p = b + 32 + ((xi_get16(b + 16) + 3) & ~3u);    /* after the vendor string */
    s = p + 8 * nformats;                           /* the first screen */
    if (s + 40 > end)
        return -1;
    xi_root = xi_get32(s);
    visual = xi_get32(s + 32);
    xi_depth = s[38];
    for (unsigned int i = 0; i < nformats; i++)
        if (p[i * 8] == xi_depth)
            xi_bpp = p[i * 8 + 1];
    /* find the root visual among the screen's depths */
    p = s + 40;
    for (int d = 0; d < s[39] && p + 8 <= end; d++) {
        int nv = (int)xi_get16(p + 2);

        p += 8;
        for (int v = 0; v < nv && p + 24 <= end; v++, p += 24)
            if (xi_get32(p) == visual) {
                if (p[4] != 4)          /* TrueColor */
                    return -1;
                xi_rmask = xi_get32(p + 8);
                xi_gmask = xi_get32(p + 12);
                xi_bmask = xi_get32(p + 16);
            }
    }
    return xi_rmask != 0 ? 0 : -1;
}

/* ---- the API ---- */

/* Connect to display (NULL: $DISPLAY). Prints the reason and returns -1 on failure. */
static inline int x_open(const char *display)
{
    unsigned char head[8], b[32], *setup, *syms = NULL, cookie[64];
    size_t len, clen;
    const char *d = display != NULL ? display : getenv("DISPLAY");

    if (d == NULL || *d == '\0') {
        fprintf(stderr, "x11: set DISPLAY to the X server, e.g. DISPLAY=192.168.1.2:0\n");
        return -1;
    }
    if ((xi_fd = xi_connect(d)) < 0) {
        fprintf(stderr, "x11: can't connect to the X server at %s\n", d);
        return -1;
    }
    clen = xi_cookie(d, cookie);
    xi_put8('l');                       /* little-endian, protocol 11.0 */
    xi_put8(0);
    xi_put16(11);
    xi_put16(0);
    xi_put16(clen ? 18 : 0);            /* MIT-MAGIC-COOKIE-1, if there is one */
    xi_put16((unsigned int)clen);
    xi_put16(0);
    if (clen) {
        for (const char *p = "MIT-MAGIC-COOKIE-1"; *p != '\0'; p++)
            xi_put8((unsigned char)*p);
        xi_put8(0);                     /* 18 bytes, padded to 20 */
        xi_put8(0);
        for (size_t i = 0; i < clen; i++)
            xi_put8(cookie[i]);
        while (xi_out_len & 3)
            xi_put8(0);
    }
    x_flush();
    if (xi_read(head, 8) < 0) {
        fprintf(stderr, "x11: the X server at %s hung up\n", d);
        return -1;
    }
    len = (size_t)xi_get16(head + 6) * 4;
    setup = malloc(len ? len : 1);
    if (setup == NULL || xi_read(setup, len) < 0) {
        fprintf(stderr, "x11: no set-up reply from %s\n", d);
        return -1;
    }
    if (head[0] != 1) {
        fprintf(stderr, "x11: %s refused the connection: %.*s\n"
                "x11: log in with ssh -Y, or on that machine try: xhost +ADDRESS-OF-THIS-ONE\n",
                d, head[0] == 0 ? head[1] : (int)len, (char *)setup);
        free(setup);
        return -1;
    }
    if (xi_parse_setup(setup, len) < 0 || xi_bpp != 32) {
        fprintf(stderr, "x11: %s is not a 24/32-bit TrueColor display\n", d);
        free(setup);
        return -1;
    }
    free(setup);

    xi_req(101, 0, 2);                  /* GetKeyboardMapping */
    xi_put8((unsigned int)xi_min_kc);
    xi_put8((unsigned int)xi_kc_count);
    xi_put16(0);
    if (xi_roundtrip(b, &syms) == 0 && syms != NULL) {
        size_t n = (size_t)xi_get32(b + 4);

        xi_syms_per = b[1];
        xi_keysyms = malloc(n * sizeof *xi_keysyms);
        if (xi_keysyms != NULL)
            for (size_t i = 0; i < n; i++)
                xi_keysyms[i] = xi_get32(syms + i * 4);
    }
    free(syms);

    xi_font = xi_new_id();
    xi_req(45, 0, 3 + 2);               /* OpenFont "fixed" */
    xi_put32(xi_font);
    xi_put16(5);
    xi_put16(0);
    xi_put32('f' | 'i' << 8 | 'x' << 16 | (unsigned int)'e' << 24);
    xi_put32('d');
    xi_wm_protocols = xi_atom("WM_PROTOCOLS");
    xi_wm_delete = xi_atom("WM_DELETE_WINDOW");
    return xi_dead ? -1 : 0;
}

static void xi_ttf_free(void);
static char *xi_clip;                   /* what we copied (x_set_clipboard), or NULL */

static inline void x_close(void)
{
    if (xi_fd >= 0) {
        x_flush();
        close(xi_fd);                   /* the server removes the window */
    }
    xi_fd = -1;
    xi_dead = 1;
    free(xi_keysyms);
    xi_keysyms = NULL;
    xi_ttf_free();
    free(xi_clip);
    xi_clip = NULL;
}

/* A pixel value for a colour, each component 0-255. */
static inline unsigned int x_rgb(int r, int g, int b)
{
    unsigned int c[3] = { (unsigned int)r & 255, (unsigned int)g & 255, (unsigned int)b & 255 };
    unsigned int m[3] = { xi_rmask, xi_gmask, xi_bmask }, v = 0;

    for (int i = 0; i < 3; i++) {
        unsigned int mask = m[i], shift = 0, bits = 0;

        while (mask != 0 && !(mask & 1)) {
            mask >>= 1;
            shift++;
        }
        while (mask & 1) {
            mask >>= 1;
            bits++;
        }
        v |= (bits >= 8 ? c[i] << (bits - 8) : c[i] >> (8 - bits)) << shift;
    }
    return v;
}

/* Open the program's one window and show it; it starts out black. */
static inline unsigned int x_window(const char *title, int w, int h)
{
    unsigned int n = (unsigned int)strlen(title);

    x_width = w;
    x_height = h;
    xi_win = xi_new_id();
    xi_req(1, 0, 8 + 3);                /* CreateWindow, depth from the parent */
    xi_put32(xi_win);
    xi_put32(xi_root);
    xi_put16(0);
    xi_put16(0);
    xi_put16((unsigned int)w);
    xi_put16((unsigned int)h);
    xi_put16(0);                        /* border */
    xi_put16(1);                        /* InputOutput */
    xi_put32(0);                        /* visual from the parent */
    xi_put32(0x2 | 0x10 | 0x800);       /* background-pixel, bit-gravity, event-mask */
    xi_put32(xi_bg);
    xi_put32(1);                        /* NorthWest: a resize keeps the picture */
    xi_put32(0x1 | 0x2 | 0x4 | 0x8 | 0x40 | 0x8000 | 0x20000);
    /* keys, buttons, motion, exposure, structure */

    xi_req(18, 0, 6 + (n + 3) / 4);     /* ChangeProperty WM_NAME */
    xi_put32(xi_win);
    xi_put32(39);                       /* WM_NAME */
    xi_put32(31);                       /* STRING */
    xi_put32(8);                        /* format 8, 3 unused */
    xi_put32(n);
    for (unsigned int i = 0; i < n; i++)
        xi_put8((unsigned char)title[i]);
    while (n++ & 3)
        xi_put8(0);

    if (xi_wm_protocols != 0 && xi_wm_delete != 0) {
        xi_req(18, 0, 6 + 1);           /* WM_PROTOCOLS = WM_DELETE_WINDOW */
        xi_put32(xi_win);
        xi_put32(xi_wm_protocols);
        xi_put32(4);                    /* ATOM */
        xi_put32(32);
        xi_put32(1);
        xi_put32(xi_wm_delete);
    }

    xi_gc = xi_new_id();
    xi_req(55, 0, 4 + 3);               /* CreateGC */
    xi_put32(xi_gc);
    xi_put32(xi_win);
    xi_put32(0x4 | 0x4000 | 0x10000);   /* foreground, font, graphics-exposures */
    xi_put32(x_rgb(255, 255, 255));
    xi_put32(xi_font);
    xi_put32(0);
    xi_color = x_rgb(255, 255, 255);

    xi_make_back(0, 0);
    xi_req(8, 0, 2);                    /* MapWindow */
    xi_put32(xi_win);
    x_flush();
    return xi_win;
}

static void xi_change_gc(unsigned int mask, unsigned int value)
{
    xi_req(56, 0, 3 + 1);               /* ChangeGC */
    xi_put32(xi_gc);
    xi_put32(mask);
    xi_put32(value);
}

static inline void x_color(unsigned int pixel)
{
    if (pixel != xi_color) {
        xi_change_gc(0x4, pixel);
        xi_color = pixel;
    }
}

static inline void x_line_width(int w)
{
    xi_change_gc(0x10, (unsigned int)w);
}

static unsigned int xi_dst(void) { return xi_target != 0 ? xi_target : xi_back; }

static void xi_box(int opcode, int x, int y, int w, int h, int arc)
{
    if (w <= 0 || h <= 0)
        return;
    xi_req(opcode, 0, arc ? 3 + 3 : 3 + 2);
    xi_put32(xi_dst());
    xi_put32(xi_gc);
    xi_put16((unsigned int)x);
    xi_put16((unsigned int)y);
    xi_put16((unsigned int)w);
    xi_put16((unsigned int)h);
    if (arc) {
        xi_put16(0);                    /* from 0 degrees */
        xi_put16(360 * 64);             /* all the way round, in 1/64 degree */
    }
}

static inline void x_fill_rect(int x, int y, int w, int h) { xi_box(70, x, y, w, h, 0); }
static inline void x_rect(int x, int y, int w, int h) { xi_box(67, x, y, w, h, 0); }
static inline void x_fill_ellipse(int x, int y, int w, int h) { xi_box(71, x, y, w, h, 1); }
static inline void x_ellipse(int x, int y, int w, int h) { xi_box(68, x, y, w, h, 1); }

static inline void x_line(int x1, int y1, int x2, int y2)
{
    xi_req(66, 0, 3 + 2);               /* PolySegment */
    xi_put32(xi_dst());
    xi_put32(xi_gc);
    xi_put16((unsigned int)x1);
    xi_put16((unsigned int)y1);
    xi_put16((unsigned int)x2);
    xi_put16((unsigned int)y2);
}

/* Text in the current colour; (x, y) is the left end of the baseline. */
static int xi_text_ttf(int x, int y, const unsigned char *s, int n);

static inline void x_text(int x, int y, const char *s)
{
    unsigned int n = (unsigned int)strlen(s);

    if (n == 0 || xi_text_ttf(x, y, (const unsigned char *)s, (int)n))
        return;
    if (n > 254)
        n = 254;
    xi_req(74, 0, 4 + (n + 2 + 3) / 4); /* PolyText8 */
    xi_put32(xi_dst());
    xi_put32(xi_gc);
    xi_put16((unsigned int)x);
    xi_put16((unsigned int)y);
    xi_put8(n);                         /* one text item: length, delta, chars */
    xi_put8(0);
    for (unsigned int i = 0; i < n; i++)
        xi_put8((unsigned char)s[i]);
    for (n += 2; n & 3; n++)
        xi_put8(0);
}

/* ---- fonts ----
 * x_text starts out in font 0, the server's "fixed". x_font finds another
 * by XLFD pattern, such as
 *     "-adobe-helvetica-bold-r-normal--24-*-*-*-p-*-iso8859-1",
 * and gives back its number for x_text_font; 0 if the server has none that
 * match, so the program still runs, in "fixed". x_text_width measures a
 * string, x_font_ascent and x_font_descent give the font's height above
 * and below the baseline. Up to X_FONTS fonts. */
#define X_FONTS 16
struct xi_ttf;
static struct {
    unsigned int id;
    int ascent, descent, minc, maxc, defw;
    short *w;                           /* widths of minc..maxc, or NULL */
    unsigned int glyphset;              /* a TrueType font's RENDER glyph set, or 0 */
    struct xi_ttf *ttf;                 /* and its face, for more characters */
} xi_fonts[X_FONTS];
static int xi_nfonts = 1, xi_cur_font;

/* Read a font's metrics (QueryFont) into slot f. */
static void xi_font_metrics(int f)
{
    unsigned char b[32], *x = NULL;

    xi_req(47, 0, 2);                   /* QueryFont */
    xi_put32(xi_fonts[f].id);
    if (xi_roundtrip(b, &x) != 0 || x == NULL) {
        xi_fonts[f].ascent = 11;
        xi_fonts[f].descent = 2;
        xi_fonts[f].defw = 6;
        free(x);
        return;
    }
    /* x is the reply from byte 32 on: max-bounds' width is at 28 of the
     * reply, so in b; the rest of the fixed part follows in x. */
    xi_fonts[f].defw = (short)xi_get16(b + 28);
    xi_fonts[f].minc = (int)xi_get16(x + 8);
    xi_fonts[f].maxc = (int)xi_get16(x + 10);
    xi_fonts[f].ascent = (short)xi_get16(x + 20);
    xi_fonts[f].descent = (short)xi_get16(x + 22);
    {
        unsigned int nprops = xi_get16(x + 14), m = xi_get32(x + 24);
        const unsigned char *ci = x + 28 + 8 * nprops;

        if (m > 0 && m == (unsigned int)(xi_fonts[f].maxc - xi_fonts[f].minc + 1) &&
            (xi_fonts[f].w = malloc(m * sizeof *xi_fonts[f].w)) != NULL)
            for (unsigned int i = 0; i < m; i++)
                xi_fonts[f].w[i] = (short)xi_get16(ci + 12 * i + 4);
    }
    free(x);
}

static inline int x_font(const char *pattern)
{
    unsigned char b[32], *x = NULL;
    unsigned int n = (unsigned int)strlen(pattern), found;
    int f;

    if (xi_nfonts >= X_FONTS || n > 255)
        return 0;
    xi_req(49, 0, 2 + (n + 3) / 4);     /* ListFonts: is there one? */
    xi_put16(1);
    xi_put16(n);
    for (unsigned int i = 0; i < n; i++)
        xi_put8((unsigned char)pattern[i]);
    for (; n & 3; n++)
        xi_put8(0);
    found = xi_roundtrip(b, &x) == 0 ? xi_get16(b + 8) : 0;
    free(x);
    if (found == 0)
        return 0;
    f = xi_nfonts++;
    xi_fonts[f].id = xi_new_id();
    n = (unsigned int)strlen(pattern);
    xi_req(45, 0, 3 + (n + 3) / 4);     /* OpenFont, the first that matches */
    xi_put32(xi_fonts[f].id);
    xi_put16(n);
    xi_put16(0);
    for (unsigned int i = 0; i < n; i++)
        xi_put8((unsigned char)pattern[i]);
    for (; n & 3; n++)
        xi_put8(0);
    xi_font_metrics(f);
    return f;
}

/* A fixed-width font, for a grid of characters (a terminal), which may be
 * a large Unicode one: QueryFont would send the metrics of every glyph,
 * which can be hundreds of KB, so this measures one "M" instead
 * (QueryTextExtents). All its characters are then that wide. */
static inline int x_font_cell(const char *pattern)
{
    unsigned char b[32], *x = NULL;
    unsigned int n = (unsigned int)strlen(pattern), found;
    char name[256];
    int f;

    if (xi_nfonts >= X_FONTS || n > 255)
        return 0;
    xi_req(49, 0, 2 + (n + 3) / 4);     /* ListFonts: the first that matches */
    xi_put16(1);
    xi_put16(n);
    for (unsigned int i = 0; i < n; i++)
        xi_put8((unsigned char)pattern[i]);
    for (; n & 3; n++)
        xi_put8(0);
    found = xi_roundtrip(b, &x) == 0 ? xi_get16(b + 8) : 0;
    if (found == 0 || x == NULL) {
        free(x);
        return 0;
    }
    /* Open it by the name the server gave: a scalable font won't open
     * from a pattern with wildcards. */
    n = x[0];
    memcpy(name, x + 1, n);
    name[n] = 0;
    free(x);
    x = NULL;
    f = xi_nfonts++;
    xi_fonts[f].id = xi_new_id();
    xi_req(45, 0, 3 + (n + 3) / 4);     /* OpenFont */
    xi_put32(xi_fonts[f].id);
    xi_put16(n);
    xi_put16(0);
    for (unsigned int i = 0; i < n; i++)
        xi_put8((unsigned char)name[i]);
    for (; n & 3; n++)
        xi_put8(0);
    xi_req(48, 1, 2 + 1);               /* QueryTextExtents of "M" (odd length) */
    xi_put32(xi_fonts[f].id);
    xi_put16('M');
    xi_put16(0);
    if (xi_roundtrip(b, &x) != 0 || (int)xi_get32(b + 16) <= 0) {
        free(x);
        xi_nfonts--;                    /* listed, but it won't open: none */
        return 0;
    }
    xi_fonts[f].ascent = (short)xi_get16(b + 8);
    xi_fonts[f].descent = (short)xi_get16(b + 10);
    xi_fonts[f].defw = (int)xi_get32(b + 16);
    free(x);
    xi_fonts[f].minc = 1;               /* no table: every glyph is defw wide */
    xi_fonts[f].maxc = 0;
    return f;
}

/* Slot 0 is "fixed", opened by x_open; its metrics are read when needed. */
static int xi_font_ready(int f)
{
    if (f < 0 || f >= xi_nfonts)
        f = 0;
    if (f == 0 && xi_fonts[0].id == 0) {
        xi_fonts[0].id = xi_font;
        xi_font_metrics(0);
    }
    return f;
}

static inline void x_text_font(int f)
{
    f = xi_font_ready(f);
    if (f != xi_cur_font) {
        if (xi_fonts[f].glyphset == 0)
            xi_change_gc(0x4000, xi_fonts[f].id);
        xi_cur_font = f;
    }
}

static inline int x_text_width(int f, const char *s)
{
    int w = 0;

    f = xi_font_ready(f);
    for (; *s; s++) {
        int c = (unsigned char)*s;

        if (xi_fonts[f].w != NULL && c >= xi_fonts[f].minc && c <= xi_fonts[f].maxc)
            w += xi_fonts[f].w[c - xi_fonts[f].minc];
        else
            w += xi_fonts[f].defw;
    }
    return w;
}

static inline int x_font_ascent(int f) { return xi_fonts[xi_font_ready(f)].ascent; }
static inline int x_font_descent(int f) { return xi_fonts[xi_font_ready(f)].descent; }

/* ---- RENDER: anti-aliased text and shapes, translucency, cursors ----
 * Core X draws with hard edges. Most servers (XQuartz and Xorg among them)
 * also have the RENDER extension, which blends: these calls use it, and
 * x_has_render() says whether it is there. Untrusted ssh -X clients don't
 * get it, so each call below says what it does without it.
 *
 * There is no floating point in any of it: positions are x_fixed, 16.16
 * fixed point as RENDER itself takes them (X_FX(10) is 10 pixels,
 * X_FX(2.5) two and a half, i << 16 pixel i), and the TrueType rasterizer
 * works in the same units. The board's C has no floating-point hardware. */

typedef int x_fixed;
#define X_FX(v) ((x_fixed)((v) * 65536))

/* BEGIN TABLES (tools/x11-tables.py) */
static const int xi_sin_q[257] = {
    0, 402, 804, 1206, 1608, 2010, 2412, 2814, 3216, 3617,
    4019, 4420, 4821, 5222, 5623, 6023, 6424, 6824, 7224, 7623,
    8022, 8421, 8820, 9218, 9616, 10014, 10411, 10808, 11204, 11600,
    11996, 12391, 12785, 13180, 13573, 13966, 14359, 14751, 15143, 15534,
    15924, 16314, 16703, 17091, 17479, 17867, 18253, 18639, 19024, 19409,
    19792, 20175, 20557, 20939, 21320, 21699, 22078, 22457, 22834, 23210,
    23586, 23961, 24335, 24708, 25080, 25451, 25821, 26190, 26558, 26925,
    27291, 27656, 28020, 28383, 28745, 29106, 29466, 29824, 30182, 30538,
    30893, 31248, 31600, 31952, 32303, 32652, 33000, 33347, 33692, 34037,
    34380, 34721, 35062, 35401, 35738, 36075, 36410, 36744, 37076, 37407,
    37736, 38064, 38391, 38716, 39040, 39362, 39683, 40002, 40320, 40636,
    40951, 41264, 41576, 41886, 42194, 42501, 42806, 43110, 43412, 43713,
    44011, 44308, 44604, 44898, 45190, 45480, 45769, 46056, 46341, 46624,
    46906, 47186, 47464, 47741, 48015, 48288, 48559, 48828, 49095, 49361,
    49624, 49886, 50146, 50404, 50660, 50914, 51166, 51417, 51665, 51911,
    52156, 52398, 52639, 52878, 53114, 53349, 53581, 53812, 54040, 54267,
    54491, 54714, 54934, 55152, 55368, 55582, 55794, 56004, 56212, 56418,
    56621, 56823, 57022, 57219, 57414, 57607, 57798, 57986, 58172, 58356,
    58538, 58718, 58896, 59071, 59244, 59415, 59583, 59750, 59914, 60075,
    60235, 60392, 60547, 60700, 60851, 60999, 61145, 61288, 61429, 61568,
    61705, 61839, 61971, 62101, 62228, 62353, 62476, 62596, 62714, 62830,
    62943, 63054, 63162, 63268, 63372, 63473, 63572, 63668, 63763, 63854,
    63944, 64031, 64115, 64197, 64277, 64354, 64429, 64501, 64571, 64639,
    64704, 64766, 64827, 64884, 64940, 64993, 65043, 65091, 65137, 65180,
    65220, 65259, 65294, 65328, 65358, 65387, 65413, 65436, 65457, 65476,
    65492, 65505, 65516, 65525, 65531, 65535, 65536,
};
static const unsigned char xi_gamma[257] = {
    0, 3, 5, 7, 8, 10, 12, 13, 15, 16, 18, 19, 21, 22, 24, 25,
    26, 28, 29, 30, 32, 33, 34, 35, 37, 38, 39, 40, 42, 43, 44, 45,
    46, 48, 49, 50, 51, 52, 53, 55, 56, 57, 58, 59, 60, 61, 62, 64,
    65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 77, 78, 79, 80, 81,
    82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97,
    98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113,
    114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129,
    129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144,
    144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 155, 156, 157, 158,
    159, 160, 161, 162, 163, 164, 165, 165, 166, 167, 168, 169, 170, 171, 172, 173,
    173, 174, 175, 176, 177, 178, 179, 180, 181, 181, 182, 183, 184, 185, 186, 187,
    188, 188, 189, 190, 191, 192, 193, 194, 195, 195, 196, 197, 198, 199, 200, 201,
    201, 202, 203, 204, 205, 206, 207, 207, 208, 209, 210, 211, 212, 213, 213, 214,
    215, 216, 217, 218, 218, 219, 220, 221, 222, 223, 224, 224, 225, 226, 227, 228,
    229, 229, 230, 231, 232, 233, 234, 234, 235, 236, 237, 238, 239, 239, 240, 241,
    242, 243, 244, 244, 245, 246, 247, 248, 248, 249, 250, 251, 252, 253, 253, 254,
    255,
};
/* 17 x 20: grey, alpha */
static const unsigned char xi_cur_hand[680] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 73, 0, 227, 0, 242, 0, 113,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 11, 0, 190, 160, 255, 204, 255,
    0, 230, 0, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 27, 0, 188, 161, 255,
    249, 255, 0, 251, 0, 37, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 24, 0, 169,
    134, 255, 255, 255, 22, 255, 0, 62, 0, 0, 0, 0, 0, 0, 0, 12,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 19,
    0, 150, 108, 255, 255, 255, 48, 255, 0, 113, 0, 200, 0, 187, 0, 219,
    11, 255, 0, 182, 0, 73, 0, 55, 0, 0, 0, 0, 0, 0, 0, 137,
    0, 186, 0, 143, 81, 255, 255, 255, 75, 255, 0, 248, 178, 255, 150, 255,
    139, 255, 255, 255, 83, 255, 64, 255, 36, 255, 0, 138, 0, 0, 0, 147,
    83, 255, 177, 255, 46, 255, 54, 255, 255, 255, 101, 255, 38, 255, 255, 255,
    201, 255, 108, 255, 255, 255, 81, 255, 236, 255, 218, 255, 0, 233, 0, 23,
    0, 199, 177, 255, 255, 255, 196, 255, 28, 255, 255, 255, 128, 255, 102, 255,
    255, 255, 201, 255, 108, 255, 255, 255, 80, 255, 235, 255, 230, 255, 0, 237,
    0, 38, 0, 146, 58, 255, 255, 255, 255, 255, 139, 255, 255, 255, 255, 255,
    255, 255, 209, 255, 255, 255, 209, 255, 255, 255, 209, 255, 255, 255, 230, 255,
    0, 237, 0, 38, 0, 66, 0, 189, 115, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 31, 255, 255, 255, 31, 255, 255, 255, 31, 255, 255, 255,
    229, 255, 0, 237, 0, 38, 0, 35, 0, 81, 0, 230, 172, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 31, 255, 255, 255, 31, 255, 255, 255, 31, 255,
    255, 255, 229, 255, 0, 237, 0, 38, 0, 2, 0, 47, 0, 122, 22, 255,
    229, 255, 255, 255, 255, 255, 255, 255, 31, 255, 255, 255, 31, 255, 255, 255,
    31, 255, 255, 255, 220, 255, 0, 234, 0, 38, 0, 0, 0, 15, 0, 59,
    0, 163, 79, 255, 255, 255, 255, 255, 255, 255, 31, 255, 255, 255, 31, 255,
    255, 255, 31, 255, 230, 255, 25, 255, 0, 159, 0, 38, 0, 0, 0, 0,
    0, 27, 0, 71, 0, 204, 136, 255, 255, 255, 255, 255, 158, 255, 255, 255,
    158, 255, 255, 255, 158, 255, 230, 255, 0, 237, 0, 70, 0, 23, 0, 0,
    0, 0, 0, 0, 0, 39, 0, 96, 0, 236, 159, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 205, 255, 0, 229, 0, 38, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 7, 0, 51, 0, 119, 0, 251, 76, 255,
    76, 255, 76, 255, 76, 255, 76, 255, 76, 255, 22, 255, 0, 148, 0, 37,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16, 0, 58, 0, 90,
    0, 126, 0, 126, 0, 126, 0, 126, 0, 126, 0, 126, 0, 103, 0, 67,
    0, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 21,
    0, 49, 0, 60, 0, 60, 0, 60, 0, 60, 0, 60, 0, 60, 0, 53,
    0, 27, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 5, 0, 5, 0, 5, 0, 5, 0, 5, 0, 5,
    0, 0, 0, 0, 0, 0, 0, 0,
};
/* 9 x 21: grey, alpha */
static const unsigned char xi_cur_text[378] = {
    149, 162, 255, 255, 255, 255, 255, 255, 149, 162, 255, 255, 255, 255, 255, 255,
    149, 162, 255, 255, 0, 255, 0, 255, 0, 255, 180, 255, 0, 255, 0, 255,
    0, 255, 255, 255, 149, 179, 255, 255, 255, 255, 180, 255, 0, 255, 180, 255,
    255, 255, 255, 255, 149, 179, 0, 71, 0, 71, 0, 71, 255, 255, 0, 255,
    255, 255, 0, 71, 0, 71, 0, 71, 0, 31, 0, 44, 0, 44, 255, 255,
    0, 255, 255, 255, 0, 44, 0, 44, 0, 31, 0, 0, 0, 0, 0, 44,
    255, 255, 0, 255, 255, 255, 0, 44, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 44, 255, 255, 0, 255, 255, 255, 0, 44, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 44, 255, 255, 0, 255, 255, 255, 0, 44, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 44, 255, 255, 0, 255, 255, 255, 0, 44, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 44, 255, 255, 0, 255, 255, 255, 0, 44,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 44, 255, 255, 0, 255, 255, 255,
    0, 44, 0, 0, 0, 0, 0, 0, 0, 0, 0, 44, 255, 255, 0, 255,
    255, 255, 0, 44, 0, 0, 0, 0, 0, 0, 0, 0, 0, 44, 255, 255,
    0, 255, 255, 255, 0, 44, 0, 0, 0, 0, 0, 0, 0, 0, 0, 44,
    255, 255, 0, 255, 255, 255, 0, 44, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 44, 255, 255, 0, 255, 255, 255, 0, 44, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 44, 255, 255, 0, 255, 255, 255, 0, 44, 0, 0, 0, 0,
    149, 162, 255, 255, 255, 255, 180, 255, 0, 255, 180, 255, 255, 255, 255, 255,
    149, 162, 255, 255, 0, 255, 0, 255, 0, 255, 180, 255, 0, 255, 0, 255,
    0, 255, 255, 255, 149, 179, 255, 255, 255, 255, 255, 255, 149, 179, 255, 255,
    255, 255, 255, 255, 149, 179, 0, 71, 0, 71, 0, 71, 0, 71, 0, 71,
    0, 71, 0, 71, 0, 71, 0, 71, 0, 31, 0, 44, 0, 44, 0, 44,
    0, 31, 0, 44, 0, 44, 0, 44, 0, 31,
};
/* END TABLES */

static int xi_render_state;             /* 0: not asked yet, 1: there, -1: not */
static int xi_rmajor;                   /* its opcode */
static unsigned int xi_pf_a8, xi_pf_rgb, xi_pf_argb;   /* picture formats */
static int xi_hidpi_op;                 /* the HIDPI extension's opcode, or 0 */
static int xi_hidpi = 1;                /* its scale: device pixels per X pixel */

static unsigned int xi_isqrt(unsigned int v)
{
    unsigned int r = 0, bit = 1u << 30;

    while (bit > v)
        bit >>= 2;
    while (bit != 0) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* The length of (dx, dy), 16.16, by a 32-bit square root: each 64-bit
 * operation is a call on the board. To 1/256 of a pixel, or better. */
static x_fixed xi_hypot(x_fixed dx, x_fixed dy)
{
    unsigned int a = (unsigned int)(dx < 0 ? -dx : dx), b = (unsigned int)(dy < 0 ? -dy : dy);
    int sh = 8;

    a >>= 8;
    b >>= 8;
    while (a > 32767 || b > 32767) {
        a >>= 1;
        b >>= 1;
        sh++;
    }
    return (x_fixed)(xi_isqrt(a * a + b * b) << sh);
}

/* Find RENDER (0.10 or later, for solid fills) and the picture formats for
 * coverage (A8), for this display's pixels and for cursors (ARGB32). */
static int xi_render_init(void)
{
    unsigned char b[32], *x = NULL;
    unsigned int n, len;

    xi_req(98, 0, 2 + 2);               /* QueryExtension "RENDER" */
    xi_put16(6);
    xi_put16(0);
    for (const char *p = "RENDER"; *p != '\0'; p++)
        xi_put8((unsigned char)*p);
    xi_put16(0);
    if (xi_roundtrip(b, &x) != 0 || !b[8]) {
        free(x);
        return 0;
    }
    free(x);
    x = NULL;
    xi_rmajor = b[9];
    xi_req(xi_rmajor, 0, 3);            /* RenderQueryVersion 0.11 */
    xi_put32(0);
    xi_put32(11);
    if (xi_roundtrip(b, &x) != 0) {
        free(x);
        return 0;
    }
    free(x);
    x = NULL;
    if (xi_get32(b + 8) == 0 && xi_get32(b + 12) < 10)
        return 0;
    xi_req(xi_rmajor, 1, 1);            /* RenderQueryPictFormats */
    if (xi_roundtrip(b, &x) != 0 || x == NULL) {
        free(x);
        return 0;
    }
    n = xi_get32(b + 8);
    len = xi_get32(b + 4) * 4;
    for (unsigned int i = 0; i < n && 28 * i + 28 <= len; i++) {
        const unsigned char *f = x + 28 * i;
        unsigned int id = xi_get32(f), depth = f[5];
        unsigned int rs = xi_get16(f + 8), rm = xi_get16(f + 10);
        unsigned int gs = xi_get16(f + 12), gm = xi_get16(f + 14);
        unsigned int bs = xi_get16(f + 16), bm = xi_get16(f + 18);
        unsigned int as = xi_get16(f + 20), am = xi_get16(f + 22);

        if (f[4] != 1)
            continue;                   /* not Direct */
        if (depth == 8 && am == 0xff && rm == 0 && gm == 0 && bm == 0)
            xi_pf_a8 = id;
        if (depth == 32 && as == 24 && am == 0xff && rs == 16 && rm == 0xff &&
            gs == 8 && gm == 0xff && bs == 0 && bm == 0xff)
            xi_pf_argb = id;
        if (depth == xi_depth && rm << rs == xi_rmask && gm << gs == xi_gmask &&
            bm << bs == xi_bmask)
            xi_pf_rgb = id;
    }
    free(x);
    return xi_pf_a8 != 0 && xi_pf_rgb != 0;
}

/* HIDPI, an extension of servers that draw at a finer resolution than X
 * pixels (crystalX on a 2x screen): QueryVersion gives that scale, and
 * SetGlyphSetScale lets a glyph set's images, origins and advances be in
 * 1/scale X pixels, so TrueType text is rendered at the screen's own
 * resolution instead of enlarged. X11_HIDPI=0 leaves it alone. */
static void xi_hidpi_init(void)
{
    unsigned char b[32], *x = NULL;
    const char *env = getenv("X11_HIDPI");
    int op;

    xi_hidpi_op = 0;
    xi_hidpi = 1;
    if (env != NULL && strcmp(env, "0") == 0)
        return;
    xi_req(98, 0, 2 + 2);               /* QueryExtension "HIDPI" */
    xi_put16(5);
    xi_put16(0);
    for (const char *p = "HIDPI"; *p != '\0'; p++)
        xi_put8((unsigned char)*p);
    xi_put8(0);
    xi_put16(0);
    if (xi_roundtrip(b, &x) != 0 || !b[8]) {
        free(x);
        return;
    }
    free(x);
    x = NULL;
    op = b[9];
    xi_req(op, 0, 1);                   /* HIDPIQueryVersion */
    if (xi_roundtrip(b, &x) == 0 && xi_get16(b + 8) == 1) {
        unsigned int s = xi_get32(b + 12);

        if (s >= 2 && s <= 8) {
            xi_hidpi_op = op;
            xi_hidpi = (int)s;
        }
    }
    free(x);
}

/* Whether the server has RENDER, so that the calls below can draw. */
static inline int x_has_render(void)
{
    if (xi_render_state == 0) {
        xi_render_state = xi_fd >= 0 && !xi_dead && xi_render_init() ? 1 : -1;
        if (xi_render_state > 0)
            xi_hidpi_init();
    }
    return xi_render_state > 0;
}

/* The pictures (RENDER's handle for a drawable) made so far, a few. */
static struct {
    unsigned int d, p;
} xi_picts[16];
static int xi_npicts, xi_pict_next;

/* Free the picture made for drawable d, if any: it is going away. */
static void xi_forget_picture(unsigned int d)
{
    for (int i = 0; i < xi_npicts; i++)
        if (xi_picts[i].d == d) {
            xi_req(xi_rmajor, 7, 2);    /* RenderFreePicture */
            xi_put32(xi_picts[i].p);
            xi_picts[i] = xi_picts[--xi_npicts];
            return;
        }
}

/* The picture for drawing on drawable d. */
static unsigned int xi_picture(unsigned int d)
{
    int i;

    for (i = 0; i < xi_npicts; i++)
        if (xi_picts[i].d == d)
            return xi_picts[i].p;
    if (xi_npicts == (int)(sizeof xi_picts / sizeof xi_picts[0])) {
        i = xi_pict_next++ % xi_npicts; /* full: let one go, it's made again if needed */
        xi_req(xi_rmajor, 7, 2);        /* RenderFreePicture */
        xi_put32(xi_picts[i].p);
    } else {
        i = xi_npicts++;
    }
    xi_picts[i].d = d;
    xi_picts[i].p = xi_new_id();
    xi_req(xi_rmajor, 4, 5);            /* RenderCreatePicture, no attributes */
    xi_put32(xi_picts[i].p);
    xi_put32(d);
    xi_put32(xi_pf_rgb);
    xi_put32(0);
    return xi_picts[i].p;
}

/* A pixel as a RENDER colour: 16 bits each, premultiplied by alpha. */
static void xi_premul(unsigned int pixel, int alpha, unsigned int c[4])
{
    unsigned int m[3] = { xi_rmask, xi_gmask, xi_bmask }, a = (unsigned int)(alpha & 255) * 0x101;

    for (int i = 0; i < 3; i++) {
        unsigned int mask = m[i], shift = 0, v;

        while (mask != 0 && !(mask & 1)) {
            mask >>= 1;
            shift++;
        }
        v = pixel >> shift & mask;
        c[i] = (mask ? v * 0xffff / mask : 0) * a / 0xffff;
    }
    c[3] = a;
}

/* Pictures that are one colour everywhere, by (pixel, alpha), a few. */
static struct {
    unsigned int pixel, p;
    int alpha;
} xi_solids[48];
static int xi_nsolids, xi_solid_next;

static unsigned int xi_solid(unsigned int pixel, int alpha)
{
    unsigned int c[4];
    int i;

    for (i = 0; i < xi_nsolids; i++)
        if (xi_solids[i].pixel == pixel && xi_solids[i].alpha == alpha)
            return xi_solids[i].p;
    if (xi_nsolids == (int)(sizeof xi_solids / sizeof xi_solids[0])) {
        i = xi_solid_next++ % xi_nsolids;
        xi_req(xi_rmajor, 7, 2);        /* RenderFreePicture */
        xi_put32(xi_solids[i].p);
    } else {
        i = xi_nsolids++;
    }
    xi_solids[i].pixel = pixel;
    xi_solids[i].alpha = alpha;
    xi_solids[i].p = xi_new_id();
    xi_premul(pixel, alpha, c);
    xi_req(xi_rmajor, 33, 4);           /* RenderCreateSolidFill */
    xi_put32(xi_solids[i].p);
    for (int k = 0; k < 4; k++)
        xi_put16(c[k]);
    return xi_solids[i].p;
}

/* ---- TrueType fonts, for x_font_ttf ----
 * A small reader for .ttf files (glyf outlines; a .ttc's first face) and
 * an anti-aliasing rasterizer: signed-area accumulation, as in font-rs,
 * in 16.16 fixed point. Each edge adds the area it covers to the cells it
 * crosses, and a running sum over the image gives the coverage. The parts
 * an edge adds in a row always sum to exactly its height there, so the
 * sum comes back to zero after each row: no rounding error builds up. No
 * hinting, no kerning, no CFF (.otf) outlines. */

struct xi_ttf {
    unsigned char *d;                   /* the file, shared by the sizes made from it */
    size_t n;
    char *name;                         /* as x_font_ttf was given it */
    int borrowed;                       /* d belongs to another size's face */
    unsigned int glyf, loca, hmtx, cmap;
    int loca_long, cmap_fmt, nhm, nglyphs, upem, ascent, descent, px, qmark;
    int gs;                             /* glyph pixels per X pixel (HIDPI), 1 without */
    int cell_w, cell_asc, cell_desc;    /* x_font_ttf_cell: glyphs cut to this box, */
                                        /* in glyph pixels */
    unsigned char *have[256];           /* which code points the glyph set has: bit pages */
};

static unsigned int xi_be16(const struct xi_ttf *f, size_t o)
{
    return o + 2 <= f->n ? (unsigned int)f->d[o] << 8 | f->d[o + 1] : 0;
}

static unsigned int xi_be32(const struct xi_ttf *f, size_t o)
{
    return xi_be16(f, o) << 16 | xi_be16(f, o + 2);
}

static int xi_ttf_parse(struct xi_ttf *f)
{
    unsigned int off = 0, head = 0, hhea = 0, maxp = 0, ntab, best = 0;

    if (f->n >= 16 && memcmp(f->d, "ttcf", 4) == 0)
        off = xi_be32(f, 12);           /* a collection: its first face */
    ntab = xi_be16(f, off + 4);
    for (unsigned int i = 0; i < ntab; i++) {
        size_t r = off + 12 + 16 * (size_t)i;
        unsigned int o = xi_be32(f, r + 8);

        if (r + 16 > f->n)
            return -1;
        if (memcmp(f->d + r, "head", 4) == 0) head = o;
        else if (memcmp(f->d + r, "hhea", 4) == 0) hhea = o;
        else if (memcmp(f->d + r, "maxp", 4) == 0) maxp = o;
        else if (memcmp(f->d + r, "cmap", 4) == 0) f->cmap = o;
        else if (memcmp(f->d + r, "glyf", 4) == 0) f->glyf = o;
        else if (memcmp(f->d + r, "loca", 4) == 0) f->loca = o;
        else if (memcmp(f->d + r, "hmtx", 4) == 0) f->hmtx = o;
    }
    if (!head || !hhea || !maxp || !f->cmap || !f->glyf || !f->loca || !f->hmtx)
        return -1;
    f->loca_long = xi_be16(f, head + 50) != 0;
    f->upem = (int)xi_be16(f, head + 18);
    if (f->upem == 0)
        f->upem = 1;
    f->ascent = (short)xi_be16(f, hhea + 4);
    f->descent = (short)xi_be16(f, hhea + 6);
    f->nhm = (int)xi_be16(f, hhea + 34);
    f->nglyphs = (int)xi_be16(f, maxp + 4);
    /* the best Unicode subtable: format 12, else format 4 */
    for (unsigned int i = 0, n = xi_be16(f, f->cmap + 2); i < n; i++) {
        unsigned int r = f->cmap + 4 + 8 * i, p = xi_be16(f, r), e = xi_be16(f, r + 2);
        unsigned int sub = f->cmap + xi_be32(f, r + 4), fmt = xi_be16(f, sub);

        if ((p == 0 || (p == 3 && (e == 1 || e == 10))) &&
            (fmt == 12 || (fmt == 4 && best == 0))) {
            best = sub;
            f->cmap_fmt = (int)fmt;
        }
    }
    if (best == 0)
        return -1;
    f->cmap = best;
    return 0;
}

/* The glyph for code point c, 0 if the font has none. */
static int xi_ttf_glyph(const struct xi_ttf *f, unsigned int c)
{
    unsigned int s = f->cmap;

    if (f->cmap_fmt == 12) {
        for (unsigned int i = 0, n = xi_be32(f, s + 12); i < n; i++) {
            unsigned int r = s + 16 + 12 * i, lo = xi_be32(f, r), hi = xi_be32(f, r + 4);

            if (c >= lo && c <= hi)
                return (int)((xi_be32(f, r + 8) + c - lo) & 0xffff);
        }
        return 0;
    }
    if (c > 0xffff)
        return 0;
    {
        unsigned int seg2 = xi_be16(f, s + 6), ends = s + 14, starts = ends + seg2 + 2;
        unsigned int deltas = starts + seg2, ranges = deltas + seg2;

        for (unsigned int i = 0; i < seg2; i += 2) {
            unsigned int start, delta, ro, gi;

            if (c > xi_be16(f, ends + i))
                continue;
            start = xi_be16(f, starts + i);
            if (c < start)
                return 0;
            delta = xi_be16(f, deltas + i);
            ro = xi_be16(f, ranges + i);
            if (ro == 0)
                return (int)((c + delta) & 0xffff);
            gi = xi_be16(f, ranges + i + ro + 2 * (c - start));
            return gi == 0 ? 0 : (int)((gi + delta) & 0xffff);
        }
    }
    return 0;
}

/* How far the pen moves after glyph g, in font units. */
static int xi_ttf_advance(const struct xi_ttf *f, int g)
{
    int i = g < f->nhm ? g : f->nhm - 1;

    return i < 0 ? 0 : (int)xi_be16(f, f->hmtx + 4 * (unsigned int)i);
}

/* A glyph's outline: points in font units, y up; ends[k] is one past the
 * last point of contour k. */
struct xi_outline {
    int *x, *y, *ends;
    unsigned char *on;
    int n, cap, nc, ccap;
};

static int xi_outline_grow(struct xi_outline *o, int pts, int contours)
{
    if (o->n + pts > o->cap) {
        int cap = (o->n + pts) * 2;
        int *x = realloc(o->x, (size_t)cap * sizeof *x), *y;
        unsigned char *on;

        if (x == NULL)
            return -1;
        o->x = x;
        if ((y = realloc(o->y, (size_t)cap * sizeof *y)) == NULL)
            return -1;
        o->y = y;
        if ((on = realloc(o->on, (size_t)cap)) == NULL)
            return -1;
        o->on = on;
        o->cap = cap;
    }
    if (o->nc + contours > o->ccap) {
        int cap = (o->nc + contours) * 2;
        int *e = realloc(o->ends, (size_t)cap * sizeof *e);

        if (e == NULL)
            return -1;
        o->ends = e;
        o->ccap = cap;
    }
    return 0;
}

/* Scratch space for rendering glyphs, kept from one glyph to the next: on
 * the board a malloc and free cost about as much as rendering the glyph. */
static struct {
    struct xi_outline o;                /* the outline */
    int *ends, *xs, *ys, tmp_cap;       /* a simple glyph's contours and coordinates */
    unsigned char *fl;                  /* and flags */
    int *ras, ras_cap;                  /* the coverage cells */
    unsigned char *alpha;               /* the image */
} xi_sc;

static int xi_sc_tmp(int n)
{
    if (n > xi_sc.tmp_cap) {
        int cap = n * 2, *e, *x, *y;
        unsigned char *f;

        if ((e = realloc(xi_sc.ends, (size_t)cap * sizeof *e)) == NULL)
            return -1;
        xi_sc.ends = e;
        if ((x = realloc(xi_sc.xs, (size_t)cap * sizeof *x)) == NULL)
            return -1;
        xi_sc.xs = x;
        if ((y = realloc(xi_sc.ys, (size_t)cap * sizeof *y)) == NULL)
            return -1;
        xi_sc.ys = y;
        if ((f = realloc(xi_sc.fl, (size_t)cap)) == NULL)
            return -1;
        xi_sc.fl = f;
        xi_sc.tmp_cap = cap;
    }
    return 0;
}

/* Add glyph g's contours to o, each point moved by m: x' = m0 x + m2 y + m4,
 * y' = m1 x + m3 y + m5, m0..m3 in 2.14 (a composite glyph's parts). */
static int xi_ttf_outline(const struct xi_ttf *f, int g, int depth, struct xi_outline *o,
                          const int m[6])
{
    size_t s, p;
    unsigned int a, b;
    int nc, npts, *ends, *xs, *ys, ok = -1;
    unsigned char *fl;

    if (g >= f->nglyphs || depth > 6)
        return 0;
    if (f->loca_long) {
        a = xi_be32(f, f->loca + 4 * (size_t)g);
        b = xi_be32(f, f->loca + 4 * (size_t)g + 4);
    } else {
        a = xi_be16(f, f->loca + 2 * (size_t)g) * 2;
        b = xi_be16(f, f->loca + 2 * (size_t)g + 2) * 2;
    }
    if (b <= a)
        return 0;                       /* no outline: a space */
    s = f->glyf + (size_t)a;
    nc = (short)xi_be16(f, s);
    if (nc < 0) {                       /* a composite of other glyphs */
        p = s + 10;
        for (;;) {
            unsigned int flags = xi_be16(f, p);
            int part = (int)xi_be16(f, p + 2), dx, dy, t[4] = { 16384, 0, 0, 16384 }, mm[6];

            p += 4;
            if (flags & 1) {
                dx = (short)xi_be16(f, p);
                dy = (short)xi_be16(f, p + 2);
                p += 4;
            } else {
                dx = p < f->n ? (signed char)f->d[p] : 0;
                dy = p + 1 < f->n ? (signed char)f->d[p + 1] : 0;
                p += 2;
            }
            if (flags & 8) {
                t[0] = t[3] = (short)xi_be16(f, p);
                p += 2;
            } else if (flags & 0x40) {
                t[0] = (short)xi_be16(f, p);
                t[3] = (short)xi_be16(f, p + 2);
                p += 4;
            } else if (flags & 0x80) {
                t[0] = (short)xi_be16(f, p);
                t[1] = (short)xi_be16(f, p + 2);
                t[2] = (short)xi_be16(f, p + 4);
                t[3] = (short)xi_be16(f, p + 6);
                p += 8;
            }
            if (!(flags & 2))
                dx = dy = 0;            /* only offsets; point matching is rare */
            mm[0] = (int)(((long long)m[0] * t[0] + (long long)m[2] * t[1]) >> 14);
            mm[1] = (int)(((long long)m[1] * t[0] + (long long)m[3] * t[1]) >> 14);
            mm[2] = (int)(((long long)m[0] * t[2] + (long long)m[2] * t[3]) >> 14);
            mm[3] = (int)(((long long)m[1] * t[2] + (long long)m[3] * t[3]) >> 14);
            mm[4] = (int)(((long long)m[0] * dx + (long long)m[2] * dy) >> 14) + m[4];
            mm[5] = (int)(((long long)m[1] * dx + (long long)m[3] * dy) >> 14) + m[5];
            if (xi_ttf_outline(f, part, depth + 1, o, mm) < 0)
                return -1;
            if (!(flags & 0x20) || p >= f->n)
                return 0;
        }
    }
    if (nc == 0)
        return 0;
    if (xi_sc_tmp(nc + 1) < 0)
        return -1;
    ends = xi_sc.ends;
    for (int i = 0; i < nc; i++)
        ends[i] = (int)xi_be16(f, s + 10 + 2 * (size_t)i) + 1;
    npts = ends[nc - 1];
    p = s + 12 + 2 * (size_t)nc + xi_be16(f, s + 10 + 2 * (size_t)nc);
    if (xi_sc_tmp(npts > nc ? npts + 1 : nc + 1) < 0)
        return -1;
    ends = xi_sc.ends;
    fl = xi_sc.fl;
    xs = xi_sc.xs;
    ys = xi_sc.ys;
    for (int i = 0; i < npts;) {
        unsigned char c;

        if (p >= f->n)
            goto out;
        c = f->d[p++];
        fl[i++] = c;
        if (c & 8) {
            int rep = p < f->n ? f->d[p++] : 0;

            while (rep-- > 0 && i < npts)
                fl[i++] = c;
        }
    }
    for (int axis = 0; axis < 2; axis++) {
        unsigned char sh = axis ? 4 : 2, same = axis ? 32 : 16;
        int v = 0, *out = axis ? ys : xs;

        for (int i = 0; i < npts; i++) {
            if (fl[i] & sh) {
                int d = p < f->n ? f->d[p] : 0;

                p++;
                v += fl[i] & same ? d : -d;
            } else if (!(fl[i] & same)) {
                v += (short)xi_be16(f, p);
                p += 2;
            }
            out[i] = v;
        }
    }
    if (xi_outline_grow(o, npts, nc) < 0)
        goto out;
    for (int k = 0, i = 0; k < nc; k++) {
        int e = ends[k] < npts ? ends[k] : npts;

        for (; i < e; i++) {
            if (m[0] == 16384 && m[1] == 0 && m[2] == 0 && m[3] == 16384) {
                o->x[o->n] = xs[i] + m[4];
                o->y[o->n] = ys[i] + m[5];
            } else {
                o->x[o->n] = (int)(((long long)m[0] * xs[i] + (long long)m[2] * ys[i]) >> 14) + m[4];
                o->y[o->n] = (int)(((long long)m[1] * xs[i] + (long long)m[3] * ys[i]) >> 14) + m[5];
            }
            o->on[o->n++] = fl[i] & 1;
        }
        o->ends[o->nc++] = o->n;
    }
    ok = 0;
out:
    return ok;
}

/* The coverage image being drawn: w x h cells of 16.16 area, and a spare
 * row's worth at the end for the cells just right of the image. */
struct xi_raster {
    int w, h, *a;
};

/* An edge from (ax, ay) to (bx, by), in 16.16 pixels from the image's top left. */
static void xi_ras_line(struct xi_raster *r, int ax, int ay, int bx, int by)
{
    int dir = 1, x, y, yend;
    long long dxdy;

    if (ay == by)
        return;
    if (ay > by) {
        int t = ax;

        ax = bx;
        bx = t;
        t = ay;
        ay = by;
        by = t;
        dir = -1;
    }
    dxdy = ((long long)(bx - ax) << 16) / (by - ay);
    x = ax;
    if (ay < 0) {
        x -= (int)((long long)ay * dxdy >> 16);
        ay = 0;
    }
    yend = (by + 0xffff) >> 16;
    if (yend > r->h)
        yend = r->h;
    for (y = ay >> 16; y < yend; y++) {
        int top = y << 16, bot = (y + 1) << 16;
        int dy = (by < bot ? by : bot) - (ay > top ? ay : top);
        int xn = x + (int)(dxdy * dy >> 16), d = dy * dir;
        int lo = x < xn ? x : xn, hi = x < xn ? xn : x;
        int x0f = lo >> 16, x1c = (hi + 0xffff) >> 16;
        int x0i = x0f < 0 ? 0 : x0f > r->w ? r->w : x0f;
        int x1i = x1c < 0 ? 0 : x1c > r->w ? r->w : x1c;
        int *row = r->a + (size_t)y * (size_t)r->w, used, part;

        if (x1i <= x0i + 1) {
            int xm = ((x + xn) >> 1) - (x0f << 16);

            part = (int)((long long)d * xm >> 16);
            row[x0i] += d - part;
            row[x0i + 1] += part;
        } else {
            /* 1 / the width; a 32-bit divide, which the CPU has (64-bit is a call) */
            long long s = 0xffffffffu / (unsigned int)(hi - lo);
            long long f0 = 65536 - (lo - (x0f << 16));      /* 1 - x0's fraction */
            long long f1 = hi - (x1c << 16) + 65536;
            long long a0 = s * (f0 * f0 >> 16) >> 17;       /* s (1 - f)^2 / 2 */
            long long am = s * (f1 * f1 >> 16) >> 17;

            used = (int)(d * a0 >> 16);
            row[x0i] += used;
            if (x1i == x0i + 2) {
                part = (int)(d * (65536 - a0 - am) >> 16);
                row[x0i + 1] += part;
                used += part;
            } else {
                long long a1 = s * (f0 + 32768) >> 16;      /* s (1.5 - f) */
                long long a2 = a1 + (long long)(x1i - x0i - 3) * s;

                part = (int)(d * (a1 - a0) >> 16);
                row[x0i + 1] += part;
                used += part;
                part = (int)(d * s >> 16);
                for (int xi = x0i + 2; xi < x1i - 1; xi++) {
                    row[xi] += part;
                    used += part;
                }
                part = (int)(d * (65536 - a2 - am) >> 16);
                row[x1i - 1] += part;
                used += part;
            }
            row[x1i] += d - used;       /* the rest: the row's parts sum to d */
        }
        x = xn;
    }
}

/* A quadratic curve, as lines: more of them the more it bends. */
static void xi_ras_quad(struct xi_raster *r, int x0, int y0, int x1, int y1, int x2, int y2)
{
    long long ddx = (long long)x0 - 2LL * x1 + x2, ddy = (long long)y0 - 2LL * y1 + y2;
    long long dev = (ddx * ddx + ddy * ddy) >> 16;           /* 16.16 pixels squared */
    int n, px = x0, py = y0;

    if (dev < 21845) {                  /* under 1/3 of a pixel */
        xi_ras_line(r, x0, y0, x2, y2);
        return;
    }
    /* 1 + (3 dev)^(1/4), as font-rs: the largest k with k^4 <= 3 dev */
    {
        long long d3 = 3 * dev;
        int k = 1;

        while (k < 64 && ((long long)(k + 1) * (k + 1) * (k + 1) * (k + 1) << 16) <= d3)
            k++;
        n = 1 + k;
    }
    for (int i = 1; i <= n; i++) {
        long long t = (i << 16) / n, u = 65536 - t;
        int qx = (int)((u * u * x0 + 2 * u * t * x1 + t * t * x2) >> 32);
        int qy = (int)((u * u * y0 + 2 * u * t * y1 + t * t * y2) >> 32);

        xi_ras_line(r, px, py, qx, qy);
        px = qx;
        py = qy;
    }
}

/* A rendered glyph: coverage 0-255, w x h; its top left is (left, top)
 * from the pen, y down. */
struct xi_glyph {
    int w, h, left, top, adv;
    unsigned char *alpha;
};

/* In glyph pixels, f->gs to the X pixel: the advance is the X-pixel one
 * (what x_text_width counts) times gs, so text lands where it was measured. */
static void xi_ttf_render(const struct xi_ttf *f, int g, struct xi_glyph *out)
{
    static const int ident[6] = { 16384, 0, 0, 16384, 0, 0 };
    struct xi_outline o;
    struct xi_raster r;
    long long sc = ((long long)(f->px * f->gs) << 24) / f->upem;   /* font units to 16.16 pixels, << 8 */
    int minx = 0x7fffffff, miny = 0x7fffffff, maxx = -0x7fffffff, maxy = -0x7fffffff;

    o = xi_sc.o;
    o.n = o.nc = 0;
    memset(out, 0, sizeof *out);
    out->adv = (xi_ttf_advance(f, g) * f->px * 2 + f->upem) / (2 * f->upem) * f->gs;
    if (xi_ttf_outline(f, g, 0, &o, ident) < 0 || o.n == 0)
        goto done;
    /* to 16.16 pixels, y down */
    for (int i = 0; i < o.n; i++) {
        o.x[i] = (int)(o.x[i] * sc >> 8);
        o.y[i] = (int)(-o.y[i] * sc >> 8);
        minx = o.x[i] < minx ? o.x[i] : minx;
        maxx = o.x[i] > maxx ? o.x[i] : maxx;
        miny = o.y[i] < miny ? o.y[i] : miny;
        maxy = o.y[i] > maxy ? o.y[i] : maxy;
    }
    out->left = minx >> 16;
    out->top = miny >> 16;
    r.w = ((maxx + 0xffff) >> 16) - out->left + 1;
    r.h = ((maxy + 0xffff) >> 16) - out->top + 1;
    if (r.h * ((r.w + 3) & ~3) > 11000)
        goto done;                      /* too big for one request: nothing */
    {
        int need = r.w * r.h + r.w + 4;

        if (need > xi_sc.ras_cap) {
            int *a = realloc(xi_sc.ras, (size_t)need * 2 * sizeof *a);
            unsigned char *al;

            if (a == NULL)
                goto done;
            xi_sc.ras = a;
            if ((al = realloc(xi_sc.alpha, (size_t)need * 2)) == NULL)
                goto done;
            xi_sc.alpha = al;
            xi_sc.ras_cap = need * 2;
        }
        r.a = xi_sc.ras;
        memset(r.a, 0, (size_t)need * sizeof *r.a);
        out->alpha = xi_sc.alpha;       /* until the next glyph */
    }
    for (int i = 0; i < o.n; i++) {
        o.x[i] -= out->left << 16;
        o.y[i] -= out->top << 16;
    }
    for (int k = 0, s = 0; k < o.nc; s = o.ends[k++]) {
        int n = o.ends[k] - s, first = -1, sx, sy, cx, cy, kx = 0, ky = 0, ctrl = 0;

        if (n < 2)
            continue;
        /* start on an on-curve point (or between two off-curve ones) */
        for (int i = 0; i < n && first < 0; i++)
            if (o.on[s + i])
                first = i;
        if (first >= 0) {
            sx = o.x[s + first];
            sy = o.y[s + first];
        } else {
            sx = (o.x[s] + o.x[s + 1]) / 2;
            sy = (o.y[s] + o.y[s + 1]) / 2;
            first = 0;
        }
        cx = sx;
        cy = sy;
        for (int j = 1; j <= n; j++) {
            int q = s + (first + j) % n, qx = o.x[q], qy = o.y[q];

            if (o.on[q]) {
                if (ctrl)
                    xi_ras_quad(&r, cx, cy, kx, ky, qx, qy);
                else
                    xi_ras_line(&r, cx, cy, qx, qy);
                ctrl = 0;
                cx = qx;
                cy = qy;
            } else if (ctrl) {
                int mx = (kx + qx) / 2, my = (ky + qy) / 2;

                xi_ras_quad(&r, cx, cy, kx, ky, mx, my);
                cx = mx;
                cy = my;
                kx = qx;
                ky = qy;
            } else {
                kx = qx;
                ky = qy;
                ctrl = 1;
            }
        }
        if (ctrl)
            xi_ras_quad(&r, cx, cy, kx, ky, sx, sy);
        else
            xi_ras_line(&r, cx, cy, sx, sy);
    }
    {
        int acc = 0;

        for (int i = 0; i < r.w * r.h; i++) {
            int v;

            acc += r.a[i];
            v = acc < 0 ? -acc : acc;
            out->alpha[i] = xi_gamma[(v > 65536 ? 65536 : v) >> 8];
        }
    }
    out->w = r.w;
    out->h = r.h;
    if (f->cell_w > 0) {                /* cut to the cell: nothing spills into the next */
        int x0 = out->left < 0 ? -out->left : 0;
        int y0 = out->top < -f->cell_asc ? -f->cell_asc - out->top : 0;
        int x1 = out->left + out->w > f->cell_w ? f->cell_w - out->left : out->w;
        int y1 = out->top + out->h > f->cell_desc ? f->cell_desc - out->top : out->h;

        if (x1 <= x0 || y1 <= y0) {
            out->w = out->h = 0;
        } else {
            for (int y = y0; y < y1; y++)
                memmove(out->alpha + (y - y0) * (x1 - x0), out->alpha + y * out->w + x0,
                        (size_t)(x1 - x0));
            out->left += x0;
            out->top += y0;
            out->w = x1 - x0;
            out->h = y1 - y0;
        }
    }
done:
    xi_sc.o = o;
}

/* The images of the glyphs being sent, rows padded to 4 bytes. */
static unsigned char xi_batch_px[12288];

/* Send k glyphs, their images in xi_batch_px (data bytes), to font fi's
 * glyph set. */
static void xi_glyphs_send(int fi, const struct xi_glyph *gl, const unsigned int *id, int k,
                           int data)
{
    xi_req(xi_rmajor, 20, (unsigned int)(3 + 4 * k + data / 4));   /* RenderAddGlyphs */
    xi_put32(xi_fonts[fi].glyphset);
    xi_put32((unsigned int)k);
    for (int j = 0; j < k; j++)
        xi_put32(id[j]);
    for (int j = 0; j < k; j++) {
        xi_put16((unsigned int)gl[j].w);
        xi_put16((unsigned int)gl[j].h);
        xi_put16((unsigned int)-gl[j].left);    /* where the pen is, in the image */
        xi_put16((unsigned int)-gl[j].top);
        xi_put16((unsigned int)gl[j].adv);
        xi_put16(0);
    }
    memcpy(xi_out + xi_out_len, xi_batch_px, (size_t)data);
    xi_out_len += (size_t)data;
}

/* Render code points cps[0..n) of TrueType font fi (or '?' for those it
 * hasn't) into its glyph set, as glyphs whose ids are the code points, up
 * to 64 glyphs and 12 KB a request. */
static void xi_ttf_add(int fi, const unsigned int *cps, int n)
{
    struct xi_ttf *f = xi_fonts[fi].ttf;
    struct xi_glyph gl[64];
    unsigned int id[64];
    int k = 0, used = 0;

    for (int i = 0; i < n; i++) {
        int g = xi_ttf_glyph(f, cps[i]), stride, bytes;
        struct xi_glyph one;

        xi_ttf_render(f, g != 0 ? g : f->qmark, &one);
        stride = (one.w + 3) & ~3;
        bytes = one.h * stride;
        if (k > 0 && (k == 64 || used + bytes > (int)sizeof xi_batch_px)) {
            xi_glyphs_send(fi, gl, id, k, used);
            k = 0;
            used = 0;
        }
        for (int y = 0; y < one.h; y++) {
            unsigned char *row = xi_batch_px + used + y * stride;

            memcpy(row, one.alpha + y * one.w, (size_t)one.w);
            memset(row + one.w, 0, (size_t)(stride - one.w));
        }
        used += bytes;
        one.alpha = NULL;
        gl[k] = one;
        id[k++] = cps[i];
    }
    if (k > 0)
        xi_glyphs_send(fi, gl, id, k, used);
}

static int xi_ttf_has(const struct xi_ttf *f, unsigned int c)
{
    return f->have[c >> 8] != NULL && (f->have[c >> 8][(c >> 3) & 31] >> (c & 7) & 1);
}

static void xi_ttf_mark(struct xi_ttf *f, unsigned int c)
{
    if (f->have[c >> 8] == NULL && (f->have[c >> 8] = calloc(32, 1)) == NULL)
        return;
    f->have[c >> 8][(c >> 3) & 31] |= (unsigned char)(1u << (c & 7));
}

/* Read a font file: a path, or a bare name looked for in $X11_FONT_DIR,
 * the board's /usr/share/fonts/dejavu and the usual Linux places. */
static unsigned char *xi_read_font(const char *name, size_t *len)
{
    static const char *const dirs[] = {
        "", "/usr/share/fonts/dejavu/", "/usr/share/fonts/truetype/dejavu/",
        "/usr/share/fonts/TTF/", "/usr/share/fonts/",
    };
    const char *env = getenv("X11_FONT_DIR");
    char path[512];

    for (int i = -1; i < (int)(sizeof dirs / sizeof dirs[0]); i++) {
        FILE *fp;
        unsigned char *d = NULL;
        size_t n = 0, cap = 0;

        if (i < 0 && (env == NULL || strchr(name, '/') != NULL))
            continue;
        if (i > 0 && strchr(name, '/') != NULL)
            break;
        snprintf(path, sizeof path, "%s%s%s", i < 0 ? env : dirs[i], i < 0 ? "/" : "", name);
        if ((fp = fopen(path, "rb")) == NULL)
            continue;
        for (;;) {
            size_t r;

            if (n == cap) {
                unsigned char *nd = realloc(d, cap = cap ? cap * 2 : 65536);

                if (nd == NULL || cap > (16u << 20)) {
                    free(nd ? nd : d);
                    fclose(fp);
                    return NULL;
                }
                d = nd;
            }
            if ((r = fread(d + n, 1, cap - n, fp)) == 0)
                break;
            n += r;
        }
        fclose(fp);
        *len = n;
        return d;
    }
    return NULL;
}

/* A TrueType font, px pixels to the em, from a .ttf file: a path, or a
 * name such as "DejaVuSans.ttf" (the board has DejaVuSans, -Bold,
 * DejaVuSansMono and -Bold in /usr/share/fonts/dejavu). Its text is
 * anti-aliased, in x_text and x_text16, through x_text_font as any other
 * font. Its Latin-1 characters are rendered here and sent to the server
 * once (on the board, at 13 pixels: 15 ms built with gcc -O2, 70 with
 * tcc, and about 24 KB); others the first time x_text16 draws them. 0 ("fixed") if the server has no RENDER or the file
 * can't be read. */
static int xi_font_ttf(const char *file, int px, int cell)
{
    struct xi_ttf *f;
    unsigned int cps[224];
    int n = 0, fi;

    if (xi_nfonts >= X_FONTS || px < 4 || px > 96 || !x_has_render())
        return 0;
    if ((f = calloc(1, sizeof *f)) == NULL)
        return 0;
    for (int i = 1; i < xi_nfonts && f->d == NULL; i++) {
        const struct xi_ttf *o = xi_fonts[i].ttf;

        if (o != NULL && !o->borrowed && strcmp(o->name, file) == 0) {
            f->d = o->d;                /* the same file at another size: one copy */
            f->n = o->n;
            f->borrowed = 1;
        }
    }
    if (f->d == NULL)
        f->d = xi_read_font(file, &f->n);
    if (f->d == NULL || xi_ttf_parse(f) < 0 || (f->name = strdup(file)) == NULL ||
        (xi_fonts[xi_nfonts].w = calloc(256, sizeof(short))) == NULL) {
        if (!f->borrowed)
            free(f->d);
        free(f->name);
        free(f);
        return 0;
    }
    f->px = px;
    /* at the server's scale, but no glyph bigger than a 96-pixel font's:
     * one image has to fit a request */
    for (f->gs = xi_hidpi; f->gs > 1 && px * f->gs > 96; f->gs--)
        ;
    f->qmark = xi_ttf_glyph(f, '?');
    fi = xi_nfonts++;
    xi_fonts[fi].id = 0;
    xi_fonts[fi].ttf = f;
    xi_fonts[fi].glyphset = xi_new_id();
    xi_fonts[fi].ascent = (f->ascent * px + f->upem - 1) / f->upem;
    xi_fonts[fi].descent = (-f->descent * px + f->upem - 1) / f->upem;
    xi_fonts[fi].minc = 0;
    xi_fonts[fi].maxc = 255;
    xi_req(xi_rmajor, 17, 3);           /* RenderCreateGlyphSet */
    xi_put32(xi_fonts[fi].glyphset);
    xi_put32(xi_pf_a8);
    if (f->gs > 1) {
        xi_req(xi_hidpi_op, 1, 3);      /* HIDPISetGlyphSetScale */
        xi_put32(xi_fonts[fi].glyphset);
        xi_put32((unsigned int)f->gs);
    }
    for (unsigned int c = 0; c < 256; c++) {
        int g = xi_ttf_glyph(f, c);

        xi_fonts[fi].w[c] = (short)((xi_ttf_advance(f, g ? g : f->qmark) * px * 2 + f->upem) /
                                    (2 * f->upem));
        if ((c >= 32 && c < 127) || c >= 160) {
            cps[n++] = c;
            xi_ttf_mark(f, c);
        }
    }
    xi_fonts[fi].defw = xi_fonts[fi].w['?'];
    if (cell) {
        f->cell_w = xi_fonts[fi].w['M'] * f->gs;
        f->cell_asc = xi_fonts[fi].ascent * f->gs;
        f->cell_desc = xi_fonts[fi].descent * f->gs;
    }
    xi_ttf_add(fi, cps, n);
    return fi;
}

static inline int x_font_ttf(const char *file, int px) { return xi_font_ttf(file, px, 0); }

/* The same for a grid of characters (a terminal), from a fixed-width font:
 * each glyph is cut to its cell, as wide as "M" and the font's height, so
 * that none reaches into the next cell, where it would stay when only that
 * cell is drawn again. */
static inline int x_font_ttf_cell(const char *file, int px) { return xi_font_ttf(file, px, 1); }

/* Whether font f has a glyph for code point c: a TrueType font's own
 * answer; for the server's fonts, 1 (it doesn't say). */
static inline int x_font_has(int f, unsigned int c)
{
    f = f >= 0 && f < xi_nfonts ? f : 0;
    if (xi_fonts[f].ttf == NULL)
        return 1;
    return c < 0x10000 && xi_ttf_glyph(xi_fonts[f].ttf, c) != 0;
}

/* Text in a TrueType font: RenderCompositeGlyphs8 or 16, 252 glyphs an item. */
static void xi_glyph_text(int x, int y, const unsigned char *s8, const unsigned int *s16, int n)
{
    int fi = xi_cur_font, items = 0, wide = s16 != NULL;
    struct xi_ttf *f = xi_fonts[fi].ttf;

    if (n > 2000)
        n = 2000;
    if (n <= 0 || f == NULL)
        return;
    if (wide) {                         /* characters it hasn't drawn yet */
        unsigned int add[252];
        int k = 0;

        for (int i = 0; i < n; i++) {
            unsigned int c = s16[i];

            if (c >= 32 && c <= 0xffff && !(c >= 127 && c < 160) && !xi_ttf_has(f, c)) {
                xi_ttf_mark(f, c);
                add[k++] = c;
                if (k == 252) {
                    xi_ttf_add(fi, add, k);
                    k = 0;
                }
            }
        }
        if (k > 0)
            xi_ttf_add(fi, add, k);
    }
    for (int i = 0; i < n; i += 252) {
        int k = n - i < 252 ? n - i : 252;

        items += (8 + k * (wide ? 2 : 1) + 3) & ~3;
    }
    {
        unsigned int dst = xi_picture(xi_dst()), src = xi_solid(xi_color, 255);

        xi_req(xi_rmajor, wide ? 24 : 23, 7 + (unsigned int)items / 4);  /* CompositeGlyphs */
        xi_put8(3);                     /* PictOpOver */
        xi_put8(0);
        xi_put16(0);
        xi_put32(src);
        xi_put32(dst);
        xi_put32(0);                    /* no mask format: each glyph blended on its own */
        xi_put32(xi_fonts[fi].glyphset);
        xi_put16(0);
        xi_put16(0);
    }
    for (int i = 0; i < n; i += 252) {
        int k = n - i < 252 ? n - i : 252, len = 8;

        xi_put8((unsigned int)k);
        xi_put8(0);
        xi_put16(0);
        xi_put16(i == 0 ? (unsigned int)x : 0);     /* later items go on from the pen */
        xi_put16(i == 0 ? (unsigned int)y : 0);
        for (int j = i; j < i + k; j++) {
            unsigned int c = wide ? s16[j] : s8[j];

            if (c < 32 || c > 0xffff || (c >= 127 && c < 160))
                c = '?';
            if (wide) {
                xi_put16(c);
                len += 2;
            } else {
                xi_put8(c);
                len++;
            }
        }
        for (; len & 3; len++)
            xi_put8(0);
    }
}

static int xi_text_ttf(int x, int y, const unsigned char *s, int n)
{
    if (xi_fonts[xi_cur_font].glyphset == 0)
        return 0;
    xi_glyph_text(x, y, s, NULL, n);
    return 1;
}

/* ---- anti-aliased shapes and translucency ----
 * These draw in the current colour at an opacity (alpha, 0-255), with
 * smooth edges, onto the current target. Each returns 0, drawing nothing,
 * when the server has no RENDER, so that the program can draw the plain
 * way instead (x_fill_ellipse, x_fill_poly, x_fill_stippled ...). Shapes
 * of one colour can go in one request: put their triangles in an array
 * for x_aa_triangles. Overlapping parts of one call don't add up. */

static int xi_tri_n;                    /* triangles in the open request */
static size_t xi_tri_at;                /* where it starts in xi_out */

#define XI_TRI_MAX 500

static void xi_tri_open(unsigned int src, unsigned int dst)
{
    xi_req(xi_rmajor, 11, 6 + 6 * XI_TRI_MAX);  /* RenderTriangles; length set at the end */
    xi_tri_at = xi_out_len - 4;
    xi_put8(3);                         /* Over */
    xi_put8(0);
    xi_put16(0);
    xi_put32(src);
    xi_put32(dst);
    xi_put32(xi_pf_a8);                 /* a mask: smooth edges, overlaps don't add up */
    xi_put16(0);
    xi_put16(0);
    xi_tri_n = 0;
}

static void xi_tri_close(void)
{
    unsigned int len = 6 + 6 * (unsigned int)xi_tri_n;

    if (xi_tri_n == 0) {                /* nothing in it: take it back */
        xi_out_len = xi_tri_at;
        xi_seq--;
        return;
    }
    xi_out[xi_tri_at + 2] = (unsigned char)(len & 255);
    xi_out[xi_tri_at + 3] = (unsigned char)(len >> 8);
}

static unsigned int xi_tri_src, xi_tri_dst;

static int xi_tri_begin(int alpha)
{
    if (!x_has_render())
        return 0;
    xi_tri_dst = xi_picture(xi_dst());
    xi_tri_src = xi_solid(xi_color, alpha & 255);
    xi_tri_open(xi_tri_src, xi_tri_dst);
    return 1;
}

static void xi_tri(x_fixed ax, x_fixed ay, x_fixed bx, x_fixed by, x_fixed cx, x_fixed cy)
{
    if (xi_tri_n == XI_TRI_MAX) {
        xi_tri_close();
        xi_tri_open(xi_tri_src, xi_tri_dst);
    }
    xi_put32((unsigned int)ax);
    xi_put32((unsigned int)ay);
    xi_put32((unsigned int)bx);
    xi_put32((unsigned int)by);
    xi_put32((unsigned int)cx);
    xi_put32((unsigned int)cy);
    xi_tri_n++;
}

/* sin and cos of a, in 65536ths of a turn (16384: a right angle), 16.16 */
static inline x_fixed x_sin(int a)
{
    unsigned int u = (unsigned int)a & 0xffff, r = u & 0x3fff, i;
    int s;

    if (u & 0x4000)
        r = 0x4000 - r;                 /* the second and fourth quarters run back */
    i = r >> 6;
    s = i >= 256 ? xi_sin_q[256]
                 : xi_sin_q[i] + (int)(((xi_sin_q[i + 1] - xi_sin_q[i]) * (int)(r & 63)) >> 6);
    return u & 0x8000 ? -s : s;
}

static inline x_fixed x_cos(int a) { return x_sin(a + 0x4000); }

/* Points around the ellipse in the box (x, y, w, h), as x, y pairs in out
 * (room for max points): more of them for a bigger ellipse, so the curve
 * stays round. Returns how many. */
static int xi_ellipse_steps(x_fixed w, x_fixed h)
{
    int n = (int)(((long long)(w > h ? w : h) * 3 / 4) >> 16);

    return n < 16 ? 16 : n > 240 ? 240 : n;
}

static void xi_ellipse_pt(x_fixed x, x_fixed y, x_fixed w, x_fixed h, int i, int n, x_fixed *px,
                          x_fixed *py)
{
    int a = (int)(((long long)i << 16) / n);
    x_fixed rx = w / 2, ry = h / 2;

    *px = x + rx + (x_fixed)((long long)rx * x_cos(a) >> 16);
    *py = y + ry + (x_fixed)((long long)ry * x_sin(a) >> 16);
}

static inline int x_ellipse_points(x_fixed x, x_fixed y, x_fixed w, x_fixed h, x_fixed *out, int max)
{
    int n = xi_ellipse_steps(w, h);

    if (n > max)
        n = max;
    for (int i = 0; i < n; i++)
        xi_ellipse_pt(x, y, w, h, i, n, &out[2 * i], &out[2 * i + 1]);
    return n;
}

/* The outline of a rounded rectangle, corners of radius rad: 28 points
 * (56 values) in out. A menu, a dialog, a button or its shadow. */
static inline int x_rounded_points(x_fixed x, x_fixed y, x_fixed w, x_fixed h, x_fixed rad,
                                   x_fixed out[56])
{
    static const int start[4] = { -16384, 0, 16384, 32768 };    /* -90, 0, 90, 180 degrees */
    int k = 0;

    if (rad > w / 2)
        rad = w / 2;
    if (rad > h / 2)
        rad = h / 2;
    if (rad < 0)
        rad = 0;
    for (int c = 0; c < 4; c++) {
        x_fixed cx = c == 0 || c == 1 ? x + w - rad : x + rad;
        x_fixed cy = c == 1 || c == 2 ? y + h - rad : y + rad;

        for (int i = 0; i <= 6; i++) {
            int a = start[c] + i * 65536 / 24;             /* 15 degree steps */

            out[k++] = cx + (x_fixed)((long long)rad * x_cos(a) >> 16);
            out[k++] = cy + (x_fixed)((long long)rad * x_sin(a) >> 16);
        }
    }
    return 28;
}

/* n triangles, six values each (x0, y0, x1, y1, x2, y2). */
static inline int x_aa_triangles(const x_fixed *t, int n, int alpha)
{
    if (!xi_tri_begin(alpha))
        return 0;
    for (int i = 0; i < n; i++, t += 6)
        xi_tri(t[0], t[1], t[2], t[3], t[4], t[5]);
    xi_tri_close();
    return 1;
}

/* A convex polygon of n points (x, y pairs). */
static inline int x_aa_polygon(const x_fixed *p, int n, int alpha)
{
    if (!xi_tri_begin(alpha))
        return 0;
    for (int i = 1; i + 1 < n; i++)
        xi_tri(p[0], p[1], p[2 * i], p[2 * i + 1], p[2 * i + 2], p[2 * i + 3]);
    xi_tri_close();
    return 1;
}

/* Connected lines through n points, width pixels wide. */
static inline int x_aa_polyline(const x_fixed *p, int n, x_fixed width, int alpha)
{
    if (!xi_tri_begin(alpha))
        return 0;
    for (int i = 0; i + 1 < n; i++) {
        long long dx = p[2 * i + 2] - p[2 * i], dy = p[2 * i + 3] - p[2 * i + 1];
        long long len = xi_hypot((x_fixed)dx, (x_fixed)dy);
        x_fixed ux, uy, ax, ay, bx, by;

        if (len < 64)
            continue;                   /* under 1/1000 of a pixel */
        /* half a width further at each end, so the joins have no gaps */
        ux = (x_fixed)(dx * (width / 2) / len);
        uy = (x_fixed)(dy * (width / 2) / len);
        ax = p[2 * i] - ux;
        ay = p[2 * i + 1] - uy;
        bx = p[2 * i + 2] + ux;
        by = p[2 * i + 3] + uy;
        xi_tri(ax - uy, ay + ux, bx - uy, by + ux, bx + uy, by - ux);
        xi_tri(ax - uy, ay + ux, bx + uy, by - ux, ax + uy, ay - ux);
    }
    xi_tri_close();
    return 1;
}

/* A filled ellipse in the box (x, y, w, h). */
static inline int x_aa_fill_ellipse(x_fixed x, x_fixed y, x_fixed w, x_fixed h, int alpha)
{
    int n = xi_ellipse_steps(w, h);
    x_fixed x0, y0, xa, ya, xb, yb;

    if (!xi_tri_begin(alpha))
        return 0;
    xi_ellipse_pt(x, y, w, h, 0, n, &x0, &y0);
    xi_ellipse_pt(x, y, w, h, 1, n, &xa, &ya);
    for (int i = 2; i < n; i++) {
        xi_ellipse_pt(x, y, w, h, i, n, &xb, &yb);
        xi_tri(x0, y0, xa, ya, xb, yb);
        xa = xb;
        ya = yb;
    }
    xi_tri_close();
    return 1;
}

/* An ellipse's outline, width pixels wide, inside the box (x, y, w, h). */
static inline int x_aa_ellipse(x_fixed x, x_fixed y, x_fixed w, x_fixed h, x_fixed width, int alpha)
{
    int n = xi_ellipse_steps(w, h);
    x_fixed iw = w - 2 * width > 0 ? w - 2 * width : 0, ih = h - 2 * width > 0 ? h - 2 * width : 0;
    x_fixed ox, oy, ix, iy, ox2, oy2, ix2, iy2;

    if (!xi_tri_begin(alpha))
        return 0;
    xi_ellipse_pt(x, y, w, h, 0, n, &ox, &oy);
    xi_ellipse_pt(x + width, y + width, iw, ih, 0, n, &ix, &iy);
    for (int i = 1; i <= n; i++) {
        xi_ellipse_pt(x, y, w, h, i % n, n, &ox2, &oy2);
        xi_ellipse_pt(x + width, y + width, iw, ih, i % n, n, &ix2, &iy2);
        xi_tri(ox, oy, ox2, oy2, ix2, iy2);
        xi_tri(ox, oy, ix2, iy2, ix, iy);
        ox = ox2;
        oy = oy2;
        ix = ix2;
        iy = iy2;
    }
    xi_tri_close();
    return 1;
}

/* A filled rectangle with corners of radius rad. */
static inline int x_aa_round_rect(x_fixed x, x_fixed y, x_fixed w, x_fixed h, x_fixed rad, int alpha)
{
    x_fixed p[56];

    return x_aa_polygon(p, x_rounded_points(x, y, w, h, rad, p), alpha);
}

/* A rectangle in the current colour that lets what's under it show
 * through: alpha 0 is not there at all, 255 is x_fill_rect. */
static inline int x_blend_rect(int x, int y, int w, int h, int alpha)
{
    unsigned int c[4], dst;

    if (!x_has_render() || w <= 0 || h <= 0)
        return x_has_render();
    dst = xi_picture(xi_dst());
    xi_premul(xi_color, alpha, c);
    xi_req(xi_rmajor, 26, 5 + 2);       /* RenderFillRectangles */
    xi_put8(3);                         /* Over */
    xi_put8(0);
    xi_put16(0);
    xi_put32(dst);
    for (int k = 0; k < 4; k++)
        xi_put16(c[k]);
    xi_put16((unsigned int)x);
    xi_put16((unsigned int)y);
    xi_put16((unsigned int)w);
    xi_put16((unsigned int)h);
    return 1;
}

/* ---- the mouse pointer ----
 * x_cursor(X_CURSOR_HAND) over something to click, X_CURSOR_TEXT over
 * text to type in, X_CURSOR_DEFAULT back to the usual arrow. With RENDER,
 * the hand and the I-beam are smooth full-colour images, otherwise they
 * and the rest come from the server's cursor font. x_define_cursor gives
 * one a picture of the program's own. */
enum {
    X_CURSOR_DEFAULT, X_CURSOR_HAND, X_CURSOR_TEXT, X_CURSOR_WAIT,
    X_CURSOR_RESIZE_H, X_CURSOR_RESIZE_V, X_CURSORS
};

static unsigned int xi_cursor_font, xi_cursors[X_CURSORS];
static int xi_cursor_now, xi_cursor_art_tried[X_CURSORS];

static inline void x_cursor(int c);

/* Cursor c from now on: w x h pixels of premultiplied ARGB (0xAARRGGBB),
 * pointing at (hx, hy). Returns 0 (and c keeps its look) without RENDER. */
static inline int x_define_cursor(int c, int w, int h, const unsigned int *argb, int hx, int hy)
{
    unsigned int pix, gc, pict, cid;

    if (c <= X_CURSOR_DEFAULT || c >= X_CURSORS || xi_win == 0 || w <= 0 || h <= 0 ||
        (unsigned int)(w * h) > 4000 || !x_has_render() || xi_pf_argb == 0)
        return 0;
    pix = xi_new_id();
    gc = xi_new_id();
    pict = xi_new_id();
    cid = xi_new_id();
    xi_req(53, 32, 4);                  /* CreatePixmap, depth 32 */
    xi_put32(pix);
    xi_put32(xi_win);
    xi_put16((unsigned int)w);
    xi_put16((unsigned int)h);
    xi_req(55, 0, 4);                   /* CreateGC for it */
    xi_put32(gc);
    xi_put32(pix);
    xi_put32(0);
    xi_req(72, 2, 6 + (unsigned int)(w * h));   /* PutImage, ZPixmap */
    xi_put32(pix);
    xi_put32(gc);
    xi_put16((unsigned int)w);
    xi_put16((unsigned int)h);
    xi_put16(0);
    xi_put16(0);
    xi_put8(0);                         /* left pad */
    xi_put8(32);                        /* depth */
    xi_put16(0);
    for (int i = 0; i < w * h; i++) {
        unsigned int p = argb[i];

        if (xi_image_msb) {
            xi_put8(p >> 24);
            xi_put8(p >> 16 & 255);
            xi_put8(p >> 8 & 255);
            xi_put8(p & 255);
        } else {
            xi_put32(p);
        }
    }
    xi_req(xi_rmajor, 4, 5);            /* RenderCreatePicture */
    xi_put32(pict);
    xi_put32(pix);
    xi_put32(xi_pf_argb);
    xi_put32(0);
    xi_req(xi_rmajor, 27, 4);           /* RenderCreateCursor */
    xi_put32(cid);
    xi_put32(pict);
    xi_put16((unsigned int)hx);
    xi_put16((unsigned int)hy);
    xi_req(xi_rmajor, 7, 2);            /* RenderFreePicture */
    xi_put32(pict);
    xi_req(60, 0, 2);                   /* FreeGC */
    xi_put32(gc);
    xi_req(54, 0, 2);                   /* FreePixmap */
    xi_put32(pix);
    if (xi_cursors[c] != 0) {
        xi_req(95, 0, 2);               /* FreeCursor: the window keeps it while it shows it */
        xi_put32(xi_cursors[c]);
    }
    xi_cursors[c] = cid;
    xi_cursor_art_tried[c] = 1;
    if (xi_cursor_now == c) {
        xi_cursor_now = X_CURSOR_DEFAULT;
        x_cursor(c);                    /* the new look, now */
    }
    x_flush();
    return 1;
}

/* The smooth hand or I-beam, from the tables (grey, alpha). */
static void xi_cursor_art(int c)
{
    unsigned int px[17 * 21];
    const unsigned char *t = c == X_CURSOR_HAND ? xi_cur_hand : xi_cur_text;
    int w = c == X_CURSOR_HAND ? 17 : 9, h = c == X_CURSOR_HAND ? 20 : 21;

    xi_cursor_art_tried[c] = 1;
    for (int i = 0; i < w * h; i++)
        px[i] = (unsigned int)t[2 * i + 1] << 24 | (unsigned int)t[2 * i] * 0x010101u;
    if (c == X_CURSOR_HAND)
        x_define_cursor(c, w, h, px, 5, 1);
    else
        x_define_cursor(c, w, h, px, 4, 9);
}

/* The pointer's shape over the window. Sends nothing if it is unchanged. */
static inline void x_cursor(int c)
{
    /* glyphs of the standard cursor font (cursorfont.h) */
    static const unsigned short glyph[X_CURSORS] = { 0, 60, 152, 150, 108, 116 };

    if (c < 0 || c >= X_CURSORS || c == xi_cursor_now || xi_win == 0)
        return;
    if ((c == X_CURSOR_HAND || c == X_CURSOR_TEXT) && !xi_cursor_art_tried[c])
        xi_cursor_art(c);
    xi_cursor_now = c;
    if (c != X_CURSOR_DEFAULT && xi_cursors[c] == 0) {
        if (xi_cursor_font == 0) {
            xi_cursor_font = xi_new_id();
            xi_req(45, 0, 3 + 2);       /* OpenFont "cursor" */
            xi_put32(xi_cursor_font);
            xi_put16(6);
            xi_put16(0);
            for (const char *p = "cursor"; *p != '\0'; p++)
                xi_put8((unsigned char)*p);
            xi_put16(0);
        }
        xi_cursors[c] = xi_new_id();
        xi_req(94, 0, 8);               /* CreateGlyphCursor: the glyph and its mask */
        xi_put32(xi_cursors[c]);
        xi_put32(xi_cursor_font);
        xi_put32(xi_cursor_font);
        xi_put16(glyph[c]);
        xi_put16(glyph[c] + 1u);
        xi_put16(0);                    /* black, */
        xi_put16(0);
        xi_put16(0);
        xi_put16(0xffff);               /* outlined in white */
        xi_put16(0xffff);
        xi_put16(0xffff);
    }
    xi_req(2, 0, 4);                    /* ChangeWindowAttributes: cursor */
    xi_put32(xi_win);
    xi_put32(0x4000);
    xi_put32(c == X_CURSOR_DEFAULT ? 0 : xi_cursors[c]);  /* None: the parent's */
    x_flush();
}

/* ---- the clipboard ----
 * x_set_clipboard(text) makes this window the owner of CLIPBOARD (and of
 * PRIMARY, for a middle-click paste); other programs then ask it for the
 * text, which x_event answers. x_request_paste() asks the owner for its
 * text, which comes back as an XE_PASTE event: e.text, e.len bytes of
 * UTF-8. XQuartz copies CLIPBOARD to and from the Mac's pasteboard. */

static unsigned int xi_a_clipboard, xi_a_utf8, xi_a_targets, xi_a_text, xi_a_paste;
static size_t xi_clip_len;
static int xi_own_sel;                  /* which of them we own: 1 CLIPBOARD, 2 PRIMARY */
static unsigned int xi_paste_prop;      /* a pasted property to read in x_event */

static void xi_clip_atoms(void)
{
    if (xi_a_clipboard != 0)
        return;
    xi_a_clipboard = xi_atom("CLIPBOARD");
    xi_a_utf8 = xi_atom("UTF8_STRING");
    xi_a_targets = xi_atom("TARGETS");
    xi_a_text = xi_atom("TEXT");
    xi_a_paste = xi_atom("X11_H_PASTE");
}

/* Put len bytes of UTF-8 text on the clipboard (and the primary selection). */
static inline void x_set_clipboard(const char *text, size_t len)
{
    char *t;

    if (len > 240000)
        len = 240000;                   /* what fits in one request */
    t = malloc(len + 1);
    if (t == NULL || xi_win == 0) {
        free(t);
        return;
    }
    xi_clip_atoms();
    memcpy(t, text, len);
    t[len] = 0;
    free(xi_clip);
    xi_clip = t;
    xi_clip_len = len;
    xi_own_sel = 3;
    for (int i = 0; i < 2; i++) {
        xi_req(22, 0, 4);               /* SetSelectionOwner (1 is PRIMARY) */
        xi_put32(xi_win);
        xi_put32(i == 0 ? xi_a_clipboard : 1);
        xi_put32(0);                    /* CurrentTime */
    }
    x_flush();
}

static void xi_convert_selection(unsigned int target)
{
    xi_req(24, 0, 6);                   /* ConvertSelection */
    xi_put32(xi_win);
    xi_put32(xi_a_clipboard);
    xi_put32(target);
    xi_put32(xi_a_paste);
    xi_put32(0);
}

/* Ask for the clipboard's text: it arrives as an XE_PASTE event (none if
 * the clipboard is empty). */
static inline void x_request_paste(void)
{
    if (xi_win == 0)
        return;
    xi_clip_atoms();
    xi_convert_selection(xi_a_utf8);
    x_flush();
}

static void xi_change_property(unsigned int win, unsigned int prop, unsigned int type,
                               unsigned int format, const unsigned char *data, size_t bytes)
{
    xi_req(18, 0, 6 + (unsigned int)((bytes + 3) / 4));    /* ChangeProperty, Replace */
    xi_put32(win);
    xi_put32(prop);
    xi_put32(type);
    xi_put32(format);
    xi_put32((unsigned int)(bytes / (format / 8)));
    for (size_t i = 0; i < bytes; i++) {
        if (xi_out_len == sizeof xi_out)
            x_flush();                  /* a big one: it goes out in pieces */
        xi_put8(data[i]);
    }
    for (size_t i = bytes; i & 3; i++)
        xi_put8(0);
}

/* Another program wants what we copied: put it in the property it named,
 * in the form it asked for, and tell it (SelectionNotify). */
static void xi_answer_selection(const unsigned char *b)
{
    unsigned int time = xi_get32(b + 4), req = xi_get32(b + 12), sel = xi_get32(b + 16);
    unsigned int target = xi_get32(b + 20), prop = xi_get32(b + 24), ok = 0;

    if (prop == 0)
        prop = target;
    if (!(xi_own_sel & (sel == xi_a_clipboard ? 1 : sel == 1 ? 2 : 0)))
        target = 0;                     /* not one we hold: say no */
    if (xi_clip != NULL && target == xi_a_targets) {
        unsigned char list[16];
        unsigned int t[4] = { xi_a_targets, xi_a_utf8, 31, xi_a_text };

        for (int i = 0; i < 16; i++)
            list[i] = (unsigned char)(t[i / 4] >> (8 * (i % 4)));
        xi_change_property(req, prop, 4, 32, list, 16);
        ok = 1;
    } else if (xi_clip != NULL && (target == xi_a_utf8 || target == xi_a_text)) {
        xi_change_property(req, prop, xi_a_utf8, 8, (const unsigned char *)xi_clip, xi_clip_len);
        ok = 1;
    } else if (xi_clip != NULL && target == 31) {
        /* STRING is Latin-1 */
        unsigned char *l = calloc(xi_clip_len + 1, 1);
        size_t n = 0;

        if (l != NULL) {
            for (size_t i = 0; i < xi_clip_len; i++) {
                unsigned char c = (unsigned char)xi_clip[i];

                if (c < 0x80) {
                    l[n++] = c;
                } else if ((c & 0xe0) == 0xc0 && i + 1 < xi_clip_len) {
                    unsigned int u = (c & 0x1fu) << 6 | ((unsigned char)xi_clip[++i] & 0x3fu);

                    l[n++] = u < 256 ? (unsigned char)u : '?';
                } else if ((c & 0xc0) != 0x80) {
                    l[n++] = '?';       /* a longer sequence: its continuation bytes are skipped */
                }
            }
            xi_change_property(req, prop, 31, 8, l, n);
            free(l);
            ok = 1;
        }
    }
    xi_req(25, 0, 11);                  /* SendEvent: SelectionNotify to the requestor */
    xi_put32(req);
    xi_put32(0);
    xi_put8(31);
    xi_put8(0);
    xi_put16(0);
    xi_put32(time);
    xi_put32(req);
    xi_put32(sel);
    xi_put32(target);
    xi_put32(ok ? prop : 0);
    xi_put32(0);
    xi_put32(0);
    x_flush();
}

/* SelectionClear, SelectionRequest and SelectionNotify, from xi_handle. */
static void xi_selection_event(const unsigned char *b)
{
    switch (b[0] & 0x7f) {
    case 29:                            /* SelectionClear: another program owns it now */
        if (xi_get32(b + 12) == xi_a_clipboard)
            xi_own_sel &= ~1;
        else if (xi_get32(b + 12) == 1)
            xi_own_sel &= ~2;
        if (xi_own_sel == 0) {          /* both gone: the text is no one's */
            free(xi_clip);
            xi_clip = NULL;
        }
        break;
    case 30:                            /* SelectionRequest: someone pastes what we copied */
        xi_answer_selection(b);
        break;
    case 31:                            /* SelectionNotify: the owner answered */
        if (xi_get32(b + 20) != 0)
            xi_paste_prop = xi_get32(b + 20);
        else if (xi_get32(b + 16) == xi_a_utf8)
            xi_convert_selection(31);   /* no UTF-8: try STRING (Latin-1) */
        break;
    }
}

/* The pasted text, from the property the owner put it in, as UTF-8. */
static void xi_read_paste(void)
{
    unsigned char b[32], *x = NULL;
    unsigned int prop = xi_paste_prop, n, type;
    char *t;
    size_t k = 0;

    xi_paste_prop = 0;
    xi_req(20, 1, 6);                   /* GetProperty, deleting it */
    xi_put32(xi_win);
    xi_put32(prop);
    xi_put32(0);                        /* any type */
    xi_put32(0);
    xi_put32(1u << 16);                 /* up to 256 KB */
    if (xi_roundtrip(b, &x) != 0 || x == NULL || b[1] != 8) {
        free(x);                        /* INCR (too big), or not text */
        return;
    }
    n = xi_get32(b + 16);
    type = xi_get32(b + 8);
    if (n > xi_get32(b + 4) * 4 || (t = malloc(2 * (size_t)n + 1)) == NULL) {
        free(x);
        return;
    }
    for (unsigned int i = 0; i < n; i++) {
        unsigned char c = x[i];

        if (type == 31 && c >= 0x80) {  /* STRING is Latin-1 */
            t[k++] = (char)(0xc0 | c >> 6);
            t[k++] = (char)(0x80 | (c & 0x3f));
        } else {
            t[k++] = (char)c;
        }
    }
    t[k] = 0;
    free(x);
    xi_push(XE_PASTE, 0, 0, 0, 0, 0);
    if (xi_qlen > 0) {
        struct x_event *e = &xi_queue[(xi_qhead + xi_qlen - 1) %
                                      (int)(sizeof xi_queue / sizeof xi_queue[0])];

        if (e->type == XE_PASTE && e->text == NULL) {
            e->text = t;
            e->len = (int)k;
            return;
        }
    }
    free(t);
}

/* Read and handle everything the server has sent, without blocking. */
static void xi_drain(void)
{
    unsigned char b[32];

    while (!xi_dead) {
        struct pollfd p;

        p.fd = xi_fd;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, 0) <= 0)
            break;
        if (xi_message(b, NULL) >= 0)
            xi_handle(b);
    }
}

static void xi_ttf_free(void)
{
    for (int i = 0; i < xi_nfonts; i++) {
        struct xi_ttf *f = xi_fonts[i].ttf;

        if (f != NULL) {
            for (int k = 0; k < 256; k++)
                free(f->have[k]);
            if (!f->borrowed)
                free(f->d);
            free(f->name);
            free(f);
            xi_fonts[i].ttf = NULL;
        }
        xi_fonts[i].glyphset = 0;
        if (i > 0) {
            free(xi_fonts[i].w);
            xi_fonts[i].w = NULL;
        }
    }
    /* the connection's fonts, pictures, colours, cursors and atoms are
     * gone with it: an x_open after x_close starts again */
    xi_nfonts = 1;
    xi_cur_font = 0;
    xi_render_state = 0;
    xi_npicts = xi_pict_next = 0;
    xi_nsolids = xi_solid_next = 0;
    xi_cursor_font = 0;
    xi_cursor_now = 0;
    memset(xi_cursors, 0, sizeof xi_cursors);
    memset(xi_cursor_art_tried, 0, sizeof xi_cursor_art_tried);
    xi_paste_prop = 0;
    xi_own_sel = 0;
    xi_a_clipboard = 0;
    free(xi_sc.o.x);
    free(xi_sc.o.y);
    free(xi_sc.o.on);
    free(xi_sc.o.ends);
    free(xi_sc.ends);
    free(xi_sc.xs);
    free(xi_sc.ys);
    free(xi_sc.fl);
    free(xi_sc.ras);
    free(xi_sc.alpha);
    memset(&xi_sc, 0, sizeof xi_sc);
}

/* The back buffer as a canvas to copy from: x_blit(x_frame(), ...) into a
 * canvas saves part of the frame (to put back after a menu, say), and into
 * the frame itself moves part of it (scrolling a list). It changes when the
 * window is resized. */
static inline unsigned int x_frame(void) { return xi_back; }

/* Text as n Unicode code points (up to U+FFFF; others show as '?'), for
 * a font that covers them, such as an iso10646 one. (x, y) is the left
 * end of the baseline. */
static inline void x_text16(int x, int y, const unsigned int *s, int n)
{
    if (xi_fonts[xi_cur_font].glyphset != 0) {
        xi_glyph_text(x, y, NULL, s, n);
        return;
    }
    while (n > 0) {
        int k = n > 254 ? 254 : n;

        xi_req(75, 0, 4 + (unsigned int)(2 + 2 * k + 3) / 4);  /* PolyText16 */
        xi_put32(xi_dst());
        xi_put32(xi_gc);
        xi_put16((unsigned int)x);
        xi_put16((unsigned int)y);
        xi_put8((unsigned int)k);       /* one item: length, delta, chars */
        xi_put8(0);
        for (int i = 0; i < k; i++) {
            unsigned int c = s[i] > 0xffff ? '?' : s[i];

            xi_put8(c >> 8);            /* CHAR2B: high byte first */
            xi_put8(c & 0xff);
        }
        for (int m = 2 + 2 * k; m & 3; m++)
            xi_put8(0);
        x += k * xi_fonts[xi_cur_font].defw;
        s += k;
        n -= k;
    }
}

/* UTF-8 to at most max code points (a bad byte is U+FFFD). */
static int xi_utf8(const char *s, unsigned int *out, int max)
{
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;

    while (*p && n < max) {
        unsigned int c = *p++, more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;

        if (c >= 0x80 && more == 0) {
            out[n++] = 0xfffd;          /* a stray continuation byte */
            continue;
        }
        c &= 0x7fu >> more;
        while (more-- > 0) {
            if ((*p & 0xc0) != 0x80) {
                c = 0xfffd;
                break;
            }
            c = c << 6 | (*p++ & 0x3fu);
        }
        out[n++] = c;
    }
    return n;
}

/* Text as UTF-8, in a TrueType font or a Unicode (iso10646) core font;
 * in an iso8859-1 one, characters past U+00FF don't show. */
static inline void x_text_utf8(int x, int y, const char *s)
{
    unsigned int b[512];

    x_text16(x, y, b, xi_utf8(s, b, 512));
}

/* How wide n code points are in font f, in pixels. */
static inline int x_text_width16(int f, const unsigned int *s, int n)
{
    int w = 0;

    f = xi_font_ready(f);
    for (int i = 0; i < n; i++) {
        unsigned int c = s[i];
        struct xi_ttf *t = xi_fonts[f].ttf;

        if (c < 256 && xi_fonts[f].w != NULL && (int)c >= xi_fonts[f].minc &&
            (int)c <= xi_fonts[f].maxc) {
            w += xi_fonts[f].w[c - (unsigned int)xi_fonts[f].minc];
        } else if (t != NULL) {
            int g = c >= 32 && c <= 0xffff ? xi_ttf_glyph(t, c) : 0;

            w += (xi_ttf_advance(t, g ? g : t->qmark) * t->px * 2 + t->upem) / (2 * t->upem);
        } else {
            w += xi_fonts[f].defw;
        }
    }
    return w;
}

static inline int x_text_width_utf8(int f, const char *s)
{
    unsigned int b[512];

    return x_text_width16(f, b, xi_utf8(s, b, 512));
}

/* Copy w x h pixels (from x_rgb) to (x, y); stride is the row length in pixels.
 * Large images go in several requests, rows at a time. */
static inline void x_image(int x, int y, int w, int h, const unsigned int *pixels, int stride)
{
    unsigned int one = 1;
    int native = (*(unsigned char *)&one == 1) != xi_image_msb;
    int rows_max;

    if (w <= 0 || h <= 0)
        return;
    rows_max = (int)((xi_max_req * 4 - 24) / ((unsigned int)w * 4));
    if (rows_max < 1)
        return;                         /* wider than one request can hold */
    while (h > 0 && !xi_dead) {
        int rows = h < rows_max ? h : rows_max;

        xi_req(72, 2, 6 + (unsigned int)(w * rows));   /* PutImage, ZPixmap */
        xi_put32(xi_dst());
        xi_put32(xi_gc);
        xi_put16((unsigned int)w);
        xi_put16((unsigned int)rows);
        xi_put16((unsigned int)x);
        xi_put16((unsigned int)y);
        xi_put8(0);                     /* left pad */
        xi_put8(xi_depth);
        xi_put16(0);
        x_flush();
        for (int r = 0; r < rows; r++) {
            const unsigned int *row = pixels + (size_t)r * (size_t)stride;

            if (native) {
                xi_write(row, (size_t)w * 4);
            } else {
                for (int i = 0; i < w; i++) {
                    unsigned int v = row[i];

                    if (xi_out_len + 4 > sizeof xi_out)
                        x_flush();
                    xi_put8(v >> 24);
                    xi_put8(v >> 16 & 255);
                    xi_put8(v >> 8 & 255);
                    xi_put8(v & 255);
                }
            }
        }
        pixels += (size_t)rows * (size_t)stride;
        y += rows;
        h -= rows;
    }
}

/* Many shapes of one kind in as few requests as fit the buffer: each item
 * is per shorts from v (4 for rectangles and segments). */
static void xi_batch(int opcode, const short *v, int n, int per)
{
    int most = (int)((sizeof xi_out - 12) / ((size_t)per * 2));

    while (n > 0) {
        int k = n < most ? n : most;

        xi_req(opcode, 0, 3 + (unsigned int)(k * per / 2));
        xi_put32(xi_dst());
        xi_put32(xi_gc);
        for (int i = 0; i < k * per; i++)
            xi_put16((unsigned int)v[i]);
        v += k * per;
        n -= k;
    }
}

/* n rectangles: x, y, w, h each. */
static inline void x_fill_rects(const short *r, int n) { xi_batch(70, r, n, 4); }

/* n line segments: x1, y1, x2, y2 each. */
static inline void x_segments(const short *s, int n) { xi_batch(66, s, n, 4); }

/* A filled convex polygon of n points: x, y each. */
static inline void x_fill_poly(const short *pts, int n)
{
    if (n < 3 || n > 1000)
        return;
    xi_req(69, 0, 4 + (unsigned int)n);  /* FillPoly */
    xi_put32(xi_dst());
    xi_put32(xi_gc);
    xi_put8(2);                         /* Convex */
    xi_put8(0);                         /* coordinates relative to the origin */
    xi_put16(0);
    for (int i = 0; i < 2 * n; i++)
        xi_put16((unsigned int)pts[i]);
}

/* An off-screen image w x h; its contents start undefined, so fill it. */
static inline unsigned int x_canvas(int w, int h)
{
    unsigned int c = xi_new_id();

    xi_req(53, (int)xi_depth, 4);       /* CreatePixmap */
    xi_put32(c);
    xi_put32(xi_win);
    xi_put16((unsigned int)(w > 0 ? w : 1));
    xi_put16((unsigned int)(h > 0 ? h : 1));
    return c;
}

static inline void x_free_canvas(unsigned int c)
{
    if (c == 0)
        return;
    if (xi_target == c)
        xi_target = 0;
    xi_forget_picture(c);
    xi_req(54, 0, 2);                   /* FreePixmap */
    xi_put32(c);
}

/* Where the drawing calls go: a canvas, or 0 for the frame. */
static inline void x_target(unsigned int c) { xi_target = c; }

/* Draw straight onto the window, over the last frame x_present showed;
 * the back buffer is left as it was. x_target(0) goes back to it. */
static inline void x_target_window(void) { xi_target = xi_win; }

/* Copy w x h from (sx, sy) of canvas c to (dx, dy) of the current target. */
static inline void x_blit(unsigned int c, int sx, int sy, int w, int h, int dx, int dy)
{
    if (w <= 0 || h <= 0)
        return;
    xi_req(62, 0, 7);                   /* CopyArea */
    xi_put32(c);
    xi_put32(xi_dst());
    xi_put32(xi_gc);
    xi_put16((unsigned int)sx);
    xi_put16((unsigned int)sy);
    xi_put16((unsigned int)dx);
    xi_put16((unsigned int)dy);
    xi_put16((unsigned int)w);
    xi_put16((unsigned int)h);
}

/* ---- masks: 1-bit images that say which pixels of a canvas to copy ---- */

static unsigned int xi_gc1, xi_gc1_fg = 2;

static void xi_mask_fg(unsigned int m, unsigned int fg)
{
    if (xi_gc1 == 0) {
        xi_gc1 = xi_new_id();
        xi_req(55, 0, 4 + 1);           /* CreateGC, for depth-1 drawables */
        xi_put32(xi_gc1);
        xi_put32(m);
        xi_put32(0x10000);              /* graphics-exposures */
        xi_put32(0);
    }
    if (fg != xi_gc1_fg) {
        xi_req(56, 0, 3 + 1);           /* ChangeGC: foreground */
        xi_put32(xi_gc1);
        xi_put32(0x4);
        xi_put32(fg);
        xi_gc1_fg = fg;
    }
}

/* A w x h mask, all clear (nothing copied). Free it with x_free_canvas. */
static inline unsigned int x_mask(int w, int h)
{
    unsigned int m = xi_new_id();

    xi_req(53, 1, 4);                   /* CreatePixmap, depth 1 */
    xi_put32(m);
    xi_put32(xi_win);
    xi_put16((unsigned int)(w > 0 ? w : 1));
    xi_put16((unsigned int)(h > 0 ? h : 1));
    xi_mask_fg(m, 0);
    xi_req(70, 0, 3 + 2);               /* PolyFillRectangle: clear it */
    xi_put32(m);
    xi_put32(xi_gc1);
    xi_put32(0);
    xi_put16((unsigned int)(w > 0 ? w : 1));
    xi_put16((unsigned int)(h > 0 ? h : 1));
    return m;
}

/* Set n rectangles (x, y, w, h each) of mask m: those pixels get copied. */
static inline void x_mask_rects(unsigned int m, const short *r, int n)
{
    int most = (int)((sizeof xi_out - 12) / 8);

    xi_mask_fg(m, 1);
    while (n > 0) {
        int k = n < most ? n : most;

        xi_req(70, 0, 3 + (unsigned int)(k * 2));
        xi_put32(m);
        xi_put32(xi_gc1);
        for (int i = 0; i < k * 4; i++)
            xi_put16((unsigned int)r[i]);
        r += k * 4;
        n -= k;
    }
}

/* x_blit, copying only the pixels set in mask m (which lines up with c). */
static inline void x_blit_masked(unsigned int c, unsigned int m, int sx, int sy, int w, int h,
                                 int dx, int dy)
{
    if (w <= 0 || h <= 0)
        return;
    xi_req(56, 0, 3 + 3);               /* ChangeGC: clip origin and mask */
    xi_put32(xi_gc);
    xi_put32(0x20000 | 0x40000 | 0x80000);
    xi_put32((unsigned int)(dx - sx));
    xi_put32((unsigned int)(dy - sy));
    xi_put32(m);
    x_blit(c, sx, sy, w, h, dx, dy);
    xi_change_gc(0x80000, 0);           /* clip mask: None */
}

/*
 * A fence: x_fence marks the point reached, and x_fence_wait waits until
 * the server has dealt with everything up to the last mark. Waiting for
 * the previous frame's fence instead of x_sync keeps one frame in flight,
 * so the board draws the next frame while the last one travels.
 */
static inline void x_fence_wait(void)
{
    unsigned char b[32];

    x_flush();
    while (xi_fence_on && !xi_dead) {
        if (xi_message(b, NULL) < 0)
            break;
        xi_handle(b);
    }
    xi_fence_on = 0;
}

static inline void x_fence(void)
{
    x_fence_wait();                     /* only one at a time */
    xi_req(43, 0, 1);                   /* GetInputFocus: any request with a reply */
    xi_fence_seq = xi_seq & 0xffff;
    xi_fence_on = 1;
    x_flush();
}

/* Fill a rectangle in the current colour, but only where the mask m,
 * repeated across it like wallpaper, is set: a dither, for tints. */
/* A polygon (not necessarily convex, but not crossing itself) filled with
 * canvas c repeated, its top left at (ox, oy): a gradient, say. */
static inline void x_fill_poly_tiled(unsigned int c, int ox, int oy, const short *pts, int n)
{
    if (n < 3 || n > 2000)
        return;
    xi_req(56, 0, 3 + 4);               /* ChangeGC: fill-style, tile, ts origin */
    xi_put32(xi_gc);
    xi_put32(0x100 | 0x400 | 0x1000 | 0x2000);
    xi_put32(1);                        /* FillTiled */
    xi_put32(c);
    xi_put32((unsigned int)ox);
    xi_put32((unsigned int)oy);
    xi_req(69, 0, 4 + (unsigned int)n);  /* FillPoly */
    xi_put32(xi_dst());
    xi_put32(xi_gc);
    xi_put8(1);                         /* Nonconvex */
    xi_put8(0);
    xi_put16(0);
    for (int i = 0; i < 2 * n; i++)
        xi_put16((unsigned int)pts[i]);
    xi_change_gc(0x100, 0);             /* FillSolid */
}

/* Connected lines through n points, in the current colour and width. */
static inline void x_polyline(const short *pts, int n)
{
    if (n < 2 || n > 2000)
        return;
    xi_req(65, 0, 3 + (unsigned int)n);  /* PolyLine, coordinates from the origin */
    xi_put32(xi_dst());
    xi_put32(xi_gc);
    for (int i = 0; i < 2 * n; i++)
        xi_put16((unsigned int)pts[i]);
}

/* The window's background: what the server fills new parts of the window
 * with when it grows, until the program draws them. Call before x_window
 * (it can't change after). */
static inline void x_background(unsigned int pixel) { xi_bg = pixel; }

/* The smallest size the window manager should let the window be made. */
static inline void x_min_size(int w, int h)
{
    xi_req(18, 0, 6 + 18);              /* ChangeProperty WM_NORMAL_HINTS */
    xi_put32(xi_win);
    xi_put32(40);                       /* WM_NORMAL_HINTS */
    xi_put32(41);                       /* WM_SIZE_HINTS */
    xi_put32(32);                       /* format 32, 3 unused */
    xi_put32(18);
    xi_put32(16);                       /* flags: PMinSize */
    for (int i = 1; i < 18; i++)
        xi_put32(i == 5 ? (unsigned int)w : i == 6 ? (unsigned int)h : 0);
}

static inline void x_fill_stippled(unsigned int m, int x, int y, int w, int h)
{
    xi_req(56, 0, 3 + 4);               /* ChangeGC: fill-style, stipple, ts origin */
    xi_put32(xi_gc);
    xi_put32(0x100 | 0x800 | 0x1000 | 0x2000);
    xi_put32(2);                        /* FillStippled */
    xi_put32(m);
    xi_put32(0);
    xi_put32(0);
    xi_box(70, x, y, w, h, 0);
    xi_change_gc(0x100, 0);             /* FillSolid */
}

/*
 * A stencil: a mask set up once for repeated masked copies. Setting a mask
 * makes the server turn the bitmap into a clip region, which is slow for
 * big masks; a stencil does that once, and each x_blit_stencil only moves
 * it. Free it with x_free_stencil (before freeing the mask).
 */
static inline unsigned int x_stencil(unsigned int m)
{
    unsigned int g = xi_new_id();

    xi_req(55, 0, 4 + 2);               /* CreateGC */
    xi_put32(g);
    xi_put32(xi_win);
    xi_put32(0x10000 | 0x80000);        /* graphics-exposures, clip-mask */
    xi_put32(0);
    xi_put32(m);
    return g;
}

static inline void x_free_stencil(unsigned int g)
{
    if (g == 0)
        return;
    xi_req(60, 0, 2);                   /* FreeGC */
    xi_put32(g);
}

/* x_blit through stencil g, made from the mask that lines up with c. */
static inline void x_blit_stencil(unsigned int g, unsigned int c, int sx, int sy, int w, int h,
                                  int dx, int dy)
{
    if (w <= 0 || h <= 0)
        return;
    xi_req(56, 0, 3 + 2);               /* ChangeGC: clip origin */
    xi_put32(g);
    xi_put32(0x20000 | 0x40000);
    xi_put32((unsigned int)(dx - sx));
    xi_put32((unsigned int)(dy - sy));
    xi_req(62, 0, 7);                   /* CopyArea */
    xi_put32(c);
    xi_put32(xi_dst());
    xi_put32(g);
    xi_put16((unsigned int)sx);
    xi_put16((unsigned int)sy);
    xi_put16((unsigned int)dx);
    xi_put16((unsigned int)dy);
    xi_put16((unsigned int)w);
    xi_put16((unsigned int)h);
}

/* Wait until the server has dealt with everything sent so far. */
static inline void x_sync(void)
{
    unsigned char b[32];

    xi_req(43, 0, 1);                   /* GetInputFocus: any request with a reply */
    xi_roundtrip(b, NULL);
}

/* Show what has been drawn since the last x_present(). */
static inline void x_present(void)
{
    xi_copy_back();
    x_flush();
}

/* The next event, if there is one; never blocks. */
static inline int x_event(struct x_event *e)
{
    static char *given;                 /* the last XE_PASTE's text */
    unsigned char b[32];

    free(given);
    given = NULL;
    while (xi_qlen == 0 && !xi_dead) {
        struct pollfd p;

        p.fd = xi_fd;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, 0) <= 0)
            break;
        if (xi_message(b, NULL) >= 0)
            xi_handle(b);
    }
    if (xi_expose && xi_back != 0) {
        xi_expose = 0;
        xi_copy_back();
        x_flush();
    }
    /* a paste arrived: read it here, not in xi_handle, which may be in
     * the middle of another round trip */
    if (xi_paste_prop != 0 && !xi_dead)
        xi_read_paste();
    if (xi_qlen == 0)
        return 0;
    *e = xi_queue[xi_qhead];
    given = e->type == XE_PASTE ? e->text : NULL;
    xi_qhead = (xi_qhead + 1) % (int)(sizeof xi_queue / sizeof xi_queue[0]);
    xi_qlen--;
    return 1;
}

static inline int x_pending(void);

/* Send what is buffered, then sleep until an event arrives, or one of
 * fds[0..n) (stdin, a pipe, a pseudo-terminal) can be read, or ms pass
 * (-1: no limit). Returns which of fds can be read, bit i for fds[i] (n up
 * to 31), or 0. It returns at once if events are waiting. Messages from
 * the server that aren't events for the program (a fence's reply) are
 * dealt with and the sleep goes on, so a loop paced by it keeps its pace. */
static inline int x_wait_fds(const int *fds, int n, int ms)
{
    struct pollfd p[32];
    struct timespec t0, t;

    if (n > 31)
        n = 31;
    x_flush();
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        int left = ms, mask = 0, r;

        if (x_pending() || xi_dead)
            return 0;
        if (ms > 0) {
            long long el;

            clock_gettime(CLOCK_MONOTONIC, &t);
            el = (long long)(t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
            left = el >= ms ? 0 : ms - (int)el;
        }
        p[0].fd = xi_fd;
        p[0].events = POLLIN;
        p[0].revents = 0;
        for (int i = 0; i < n; i++) {
            p[i + 1].fd = fds[i];
            p[i + 1].events = POLLIN;
            p[i + 1].revents = 0;
        }
        r = poll(p, (nfds_t)n + 1, left);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return 0;                   /* the time is up */
        for (int i = 0; i < n; i++)
            if (p[i + 1].revents)
                mask |= 1 << i;
        if (mask)
            return mask;
        xi_drain();                     /* the server: events, or a fence's reply */
        if (left == 0)
            return 0;
    }
}

/* Send what is buffered, then sleep until an event arrives or ms pass. */
static inline void x_wait(int ms) { x_wait_fds(NULL, 0, ms); }

/* Where the mouse is, relative to the window, even outside it. Returns 0
 * if it is on another screen or the server didn't answer. */
static inline int x_pointer(int *x, int *y)
{
    unsigned char b[32];

    xi_req(38, 0, 2);                   /* QueryPointer */
    xi_put32(xi_win);
    if (xi_roundtrip(b, NULL) < 0 || !b[1])
        return 0;
    *x = (short)xi_get16(b + 20);
    *y = (short)xi_get16(b + 22);
    return 1;
}

static inline int x_fd(void) { return xi_fd; }

/* Is anything waiting that x_event would return (or a repaint it would
 * do)? Check it before blocking in your own poll() on x_fd(): x_sync and
 * other round trips read events off the socket into a queue, where poll()
 * can't see them. */
static inline int x_pending(void) { return xi_qlen > 0 || xi_expose || xi_paste_prop != 0; }

#endif /* DEMO_X11_H */
