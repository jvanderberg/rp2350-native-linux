// SPDX-License-Identifier: MIT
/*
 * xgallery.c - what x11.h draws through the RENDER extension, one panel
 * each: TrueType text, smooth shapes, translucency, a graph, a clock, and
 * a button and a text field with their cursors and the clipboard.
 *
 * Build:  ./build.sh xgallery
 * Run:    DISPLAY=HOST:0 ./xgallery
 * Keys:   q or Escape quits. Click the field to type in it; there Ctrl-C
 *         copies its text, Ctrl-V pastes (from any program, the Mac's
 *         clipboard too with XQuartz), Escape leaves it.
 *
 * Where the server has no RENDER (an untrusted ssh -X), each panel draws
 * the plain way instead, and says so.
 */
#include "../lib/x11.h"

#define PW 230                          /* panel size */
#define PH 170
#define GAP 10
#define FX(i) ((x_fixed)(i) << 16)

static int f_small, f_body, f_big, f_bold, render;
static unsigned int c_bg, c_panel, c_text, c_dim, c_accent;
static char field[128] = "Type here";
static int field_len = 9, focus, hover;

static void title(int x, int y, const char *s)
{
    x_text_font(f_bold);
    x_color(c_text);
    x_text(x + 12, y + 8 + x_font_ascent(f_bold), s);
}

/* A panel: a rounded card with a soft shadow. */
static void panel(int x, int y, const char *name)
{
    if (render) {
        x_color(x_rgb(0, 0, 0));
        x_aa_round_rect(FX(x + 1), FX(y + 3), FX(PW), FX(PH), FX(10), 70);
        x_color(c_panel);
        x_aa_round_rect(FX(x), FX(y), FX(PW), FX(PH), FX(10), 255);
    } else {
        x_color(c_panel);
        x_fill_rect(x, y, PW, PH);
    }
    title(x, y, name);
}

static void text_panel(int x, int y)
{
    int ty = y + 50;

    panel(x, y, "TrueType text");
    x_color(c_text);
    x_text_font(f_small);
    x_text(x + 12, ty, "11 px: the quick brown fox");
    x_text_font(f_body);
    x_text(x + 12, ty += 24, "14 px: jumps over the dog");
    x_text_font(f_big);
    x_text(x + 12, ty += 34, "24 px Ag");
    x_text_font(f_body);
    x_color(c_accent);
    x_text_utf8(x + 12, ty += 30, "Grüße — 12 € ✓ ★ π ≈ 3.14");
    x_color(c_dim);
    x_text_font(f_small);
    x_text(x + 12, ty += 22, render ? "anti-aliased, DejaVu Sans" : "no RENDER: core fonts");
}

static void shapes_panel(int x, int y)
{
    int cy = y + 95;
    x_fixed wave[2 * 41];

    panel(x, y, "Smooth shapes");
    x_color(c_accent);
    if (!render || !x_aa_fill_ellipse(FX(x + 16), FX(cy - 30), FX(56), FX(56), 255))
        x_fill_ellipse(x + 16, cy - 30, 56, 56);
    x_color(x_rgb(255, 196, 70));
    if (!render || !x_aa_ellipse(FX(x + 86), FX(cy - 30), FX(56), FX(56), X_FX(3.5), 255))
        x_ellipse(x + 86, cy - 30, 56, 56);
    x_color(x_rgb(120, 220, 150));
    if (!render || !x_aa_round_rect(FX(x + 156), FX(cy - 30), FX(58), FX(56), FX(12), 255))
        x_fill_rect(x + 156, cy - 30, 58, 56);
    /* a wave, 2.5 pixels wide */
    for (int i = 0; i <= 40; i++) {
        wave[2 * i] = FX(x + 16) + i * FX(198) / 40;
        wave[2 * i + 1] = FX(cy + 50) + (x_fixed)((long long)FX(12) * x_sin(i * 65536 / 20) >> 16);
    }
    x_color(c_text);
    if (!render || !x_aa_polyline(wave, 41, X_FX(2.5), 255)) {
        short w[2 * 41];

        for (int i = 0; i <= 40; i++) {
            w[2 * i] = (short)(wave[2 * i] >> 16);
            w[2 * i + 1] = (short)(wave[2 * i + 1] >> 16);
        }
        x_polyline(w, 41);
    }
}

static void blend_panel(int x, int y)
{
    static const unsigned char col[3][3] = { { 255, 80, 90 }, { 80, 200, 120 }, { 80, 140, 255 } };
    static const int at[3][2] = { { 70, 75 }, { 110, 75 }, { 90, 110 } };

    panel(x, y, "Translucency");
    /* stripes, to see through */
    for (int i = 0; i < 6; i++) {
        x_color(i & 1 ? c_panel : x_rgb(70, 76, 92));
        x_fill_rect(x + 150 + i * 12, y + 40, 12, 110);
    }
    for (int i = 0; i < 3; i++) {
        x_color(x_rgb(col[i][0], col[i][1], col[i][2]));
        if (!render || !x_aa_fill_ellipse(FX(x + at[i][0] - 38), FX(y + at[i][1] - 30),
                                         FX(76), FX(76), 150))
            x_fill_ellipse(x + at[i][0] - 38, y + at[i][1] - 30, 76, 76);
    }
    x_color(x_rgb(255, 255, 255));
    x_blend_rect(x + 142, y + 60, 80, 30, 90);
    x_color(x_rgb(0, 0, 0));
    x_blend_rect(x + 142, y + 100, 80, 30, 120);
}

static void graph_panel(int x, int y, int t)
{
    x_fixed line[2 * 49], area[6 * 2 * 48];
    int base = y + PH - 16, n = 0;

    panel(x, y, "A graph");
    x_color(c_dim);
    for (int i = 0; i < 4; i++)
        x_fill_rect(x + 12, y + 50 + i * 30, PW - 24, 1);
    for (int i = 0; i <= 48; i++) {
        int a = i * 65536 / 24 + t * 1200;
        x_fixed v = (x_sin(a) + x_sin(a * 3 + 9000) / 3) / 2;   /* about -0.66 to 0.66 */

        line[2 * i] = FX(x + 12) + i * FX(PW - 24) / 48;
        line[2 * i + 1] = FX(y + 100) - v * 55;
    }
    /* the area under the line, see-through, as triangles */
    for (int i = 0; i < 48; i++) {
        x_fixed x0 = line[2 * i], y0 = line[2 * i + 1], x1 = line[2 * i + 2], y1 = line[2 * i + 3];

        area[n++] = x0; area[n++] = y0; area[n++] = x1; area[n++] = y1; area[n++] = x1; area[n++] = FX(base);
        area[n++] = x0; area[n++] = y0; area[n++] = x1; area[n++] = FX(base); area[n++] = x0; area[n++] = FX(base);
    }
    x_color(c_accent);
    if (render) {
        x_aa_triangles(area, 2 * 48, 60);
        x_aa_polyline(line, 49, FX(2), 255);
    } else {
        short l[2 * 49];

        for (int i = 0; i <= 48; i++) {
            l[2 * i] = (short)(line[2 * i] >> 16);
            l[2 * i + 1] = (short)(line[2 * i + 1] >> 16);
        }
        x_polyline(l, 49);
    }
}

static void clock_panel(int x, int y, int sec)
{
    int cx = x + PW / 2, cy = y + 100, r = 56;

    panel(x, y, "A clock");
    x_color(x_rgb(250, 250, 250));
    if (!render || !x_aa_fill_ellipse(FX(cx - r), FX(cy - r), FX(2 * r), FX(2 * r), 255))
        x_fill_ellipse(cx - r, cy - r, 2 * r, 2 * r);
    x_color(x_rgb(30, 34, 44));
    for (int i = 0; i < 12; i++) {
        int a = i * 65536 / 12;
        x_fixed pts[4] = {
            FX(cx) + (x_fixed)((long long)(r - 10) * x_sin(a)),
            FX(cy) - (x_fixed)((long long)(r - 10) * x_cos(a)),
            FX(cx) + (x_fixed)((long long)(r - 4) * x_sin(a)),
            FX(cy) - (x_fixed)((long long)(r - 4) * x_cos(a)),
        };

        if (!render || !x_aa_polyline(pts, 2, i % 3 ? FX(1) : FX(3), 255))
            x_line(pts[0] >> 16, pts[1] >> 16, pts[2] >> 16, pts[3] >> 16);
    }
    {
        /* hands: hours, minutes, seconds */
        int ang[3] = { (sec / 60 % 720) * 65536 / 720, (sec % 3600) * 65536 / 3600,
                       (sec % 60) * 65536 / 60 };
        int len[3] = { r - 26, r - 12, r - 8 };
        x_fixed wid[3] = { FX(4), X_FX(2.5), X_FX(1.2) };

        for (int k = 0; k < 3; k++) {
            x_fixed pts[4] = { FX(cx), FX(cy),
                               FX(cx) + (x_fixed)((long long)len[k] * x_sin(ang[k])),
                               FX(cy) - (x_fixed)((long long)len[k] * x_cos(ang[k])) };

            x_color(k == 2 ? x_rgb(230, 70, 60) : x_rgb(30, 34, 44));
            if (!render || !x_aa_polyline(pts, 2, wid[k], 255))
                x_line(cx, cy, pts[2] >> 16, pts[3] >> 16);
        }
    }
}

/* the button and the field, where they are */
#define BX(x) ((x) + 14)
#define BY(y) ((y) + 46)
#define FLX(x) ((x) + 14)
#define FLY(y) ((y) + 100)

static void controls_panel(int x, int y)
{
    int bw = PW - 28, bh = 34, fw = PW - 28, fh = 30, tw;

    panel(x, y, "Cursors, clipboard");
    x_color(hover == 1 ? x_rgb(110, 175, 255) : c_accent);
    if (!render || !x_aa_round_rect(FX(BX(x)), FX(BY(y)), FX(bw), FX(bh), FX(8), 255))
        x_fill_rect(BX(x), BY(y), bw, bh);
    x_text_font(f_body);
    x_color(x_rgb(255, 255, 255));
    tw = x_text_width(f_body, "Copy the text");
    x_text(BX(x) + (bw - tw) / 2, BY(y) + 22, "Copy the text");

    x_color(focus ? c_accent : c_dim);
    if (!render || !x_aa_round_rect(FX(FLX(x)), FX(FLY(y)), FX(fw), FX(fh), FX(6), 255))
        x_fill_rect(FLX(x), FLY(y), fw, fh);
    x_color(x_rgb(20, 23, 30));
    if (!render || !x_aa_round_rect(FX(FLX(x) + 2), FX(FLY(y) + 2), FX(fw - 4), FX(fh - 4),
                                    FX(5), 255))
        x_fill_rect(FLX(x) + 2, FLY(y) + 2, fw - 4, fh - 4);
    x_color(c_text);
    x_text_utf8(FLX(x) + 8, FLY(y) + 20, field);
    if (focus) {
        x_color(c_accent);
        x_fill_rect(FLX(x) + 9 + x_text_width_utf8(f_body, field), FLY(y) + 7, 2, fh - 14);
    }
    x_text_font(f_small);
    x_color(c_dim);
    x_text(x + 14, y + PH - 14, "Ctrl-C copies, Ctrl-V pastes");
}

static int inside(int px, int py, int x, int y, int w, int h)
{
    return px >= x && py >= y && px < x + w && py < y + h;
}

/* panel (column, row) i's top left */
static int pxof(int i) { return GAP + (i % 3) * (PW + GAP); }
static int pyof(int i) { return GAP + (i / 3) * (PH + GAP); }

static void draw(int sec, int t)
{
    x_color(c_bg);
    x_fill_rect(0, 0, x_width, x_height);
    text_panel(pxof(0), pyof(0));
    shapes_panel(pxof(1), pyof(1));
    blend_panel(pxof(2), pyof(2));
    graph_panel(pxof(3), pyof(3), t);
    clock_panel(pxof(4), pyof(4), sec);
    controls_panel(pxof(5), pyof(5));
    x_present();
}

/* Drop the last UTF-8 character of the field. */
static void backspace(void)
{
    while (field_len > 0 && ((unsigned char)field[--field_len] & 0xc0) == 0x80)
        ;
    field[field_len] = 0;
}

static void add_text(const char *s, int n)
{
    for (int i = 0; i < n && field_len < (int)sizeof field - 1; i++)
        if ((unsigned char)s[i] >= 32)
            field[field_len++] = s[i];
    field[field_len] = 0;
}

int main(int argc, char **argv)
{
    int t = 0, last = -1, dirty = 1;

    if (x_open(argc > 1 ? argv[1] : NULL) < 0)
        return 1;
    c_bg = x_rgb(22, 25, 33);
    c_panel = x_rgb(40, 45, 58);
    c_text = x_rgb(228, 232, 240);
    c_dim = x_rgb(128, 136, 152);
    c_accent = x_rgb(70, 140, 240);
    x_background(c_bg);
    x_window("xgallery", 3 * PW + 4 * GAP, 2 * PH + 3 * GAP);
    render = x_has_render();
    f_small = x_font_ttf("DejaVuSans.ttf", 11);
    f_body = x_font_ttf("DejaVuSans.ttf", 14);
    f_big = x_font_ttf("DejaVuSans.ttf", 24);
    f_bold = x_font_ttf("DejaVuSans-Bold.ttf", 13);
    if (f_body == 0) {                  /* no RENDER, or no fonts: core ones */
        f_small = x_font("-*-helvetica-medium-r-normal--11-*-*-*-p-*-iso8859-1");
        f_body = x_font("-*-helvetica-medium-r-normal--14-*-*-*-p-*-iso8859-1");
        f_big = x_font("-*-helvetica-medium-r-normal--24-*-*-*-p-*-iso8859-1");
        f_bold = x_font("-*-helvetica-bold-r-normal--12-*-*-*-p-*-iso8859-1");
    }
    for (;;) {
        struct x_event e;
        struct timespec ts;
        int sec;

        while (x_event(&e)) {
            int bx = BX(pxof(5)), by = BY(pyof(5)), fx = FLX(pxof(5)), fy = FLY(pyof(5));

            switch (e.type) {
            case XE_CLOSE:
                return 0;
            case XE_RESIZE:
                dirty = 1;
                break;
            case XE_MOTION: {
                int h = inside(e.x, e.y, bx, by, PW - 28, 34) ? 1
                        : inside(e.x, e.y, fx, fy, PW - 28, 30) ? 2 : 0;

                if (h != hover) {
                    hover = h;
                    x_cursor(h == 1 ? X_CURSOR_HAND : h == 2 ? X_CURSOR_TEXT : X_CURSOR_DEFAULT);
                    dirty = 1;
                }
                break;
            }
            case XE_BUTTON_DOWN:
                if (e.button == 1 && inside(e.x, e.y, bx, by, PW - 28, 34))
                    x_set_clipboard(field, (size_t)field_len);
                if (e.button == 1)
                    focus = inside(e.x, e.y, fx, fy, PW - 28, 30);
                if (e.button == 2 && inside(e.x, e.y, fx, fy, PW - 28, 30))
                    x_request_paste();
                dirty = 1;
                break;
            case XE_PASTE:
                add_text(e.text, e.len);
                dirty = 1;
                break;
            case XE_KEY_DOWN:
                if (!focus) {
                    if (e.key == 'q' || e.key == X_KEY_ESCAPE) {
                        x_close();
                        return 0;
                    }
                } else if (e.key == X_KEY_ESCAPE) {
                    focus = 0;
                } else if (e.mods & X_MOD_CTRL && (e.key == 'c' || e.key == 'C')) {
                    x_set_clipboard(field, (size_t)field_len);
                } else if (e.mods & X_MOD_CTRL && (e.key == 'v' || e.key == 'V')) {
                    x_request_paste();
                } else if (e.key == X_KEY_BACKSPACE) {
                    backspace();
                } else if (e.key >= 32 && e.key < 256) {
                    char u[2];
                    int n = 0;

                    if (e.key < 128) {
                        u[n++] = (char)e.key;
                    } else {
                        u[n++] = (char)(0xc0 | e.key >> 6);
                        u[n++] = (char)(0x80 | (e.key & 0x3f));
                    }
                    add_text(u, n);
                }
                dirty = 1;
                break;
            }
        }
        clock_gettime(CLOCK_REALTIME, &ts);
        sec = (int)(ts.tv_sec % 43200);
        if (sec != last) {
            last = sec;
            dirty = 1;
        }
        if (dirty) {
            draw(sec, t++);
            x_sync();
            dirty = 0;
        }
        x_wait(1000);
    }
}
