// SPDX-License-Identifier: MIT
/*
 * xedit.c - a paged text editor in an X11 window, in xmc's style.
 *
 * Build:  ./build.sh          (xedit.c and document.c; needs ../lib/x11.h)
 * Run:    DISPLAY=HOST:0 ./xedit [FILE]
 *
 * The document (document.c) is a rope over the file and a scratch file of
 * additions, read in 4 KiB pages, so a file far larger than RAM opens at
 * once and a jump anywhere costs one screenful. Only the rows on screen
 * are laid out.
 *
 * The window follows x11-direct's patterns: each row, bar and widget is
 * sent only when what it shows changed, and menus and dialogs save what
 * they cover on the server and put it back with one copy when they close.
 * Where the server has RENDER, text is DejaVu, anti-aliased, corners are
 * round and menus and dialogs cast soft shadows.
 */
#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE 700
#define _FILE_OFFSET_BITS 64
#include "document.h"
#include "../lib/x11.h"

#include <ctype.h>
#include <dirent.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define COLS 200
#define ROWS 90
#define HISTORY 128
#define CLIPBOARD (32 * 1024)
#define PAD 10                  /* left of the text */
#define VPAD 6                  /* above the first row */
#define SCROLL_W 12
#define CHECKPOINTS 64
#define CHECKPOINT_INTERVAL (64 * 1024)
#define SEARCH_MAX 256
#define LOOKBACK (2 * DOC_PAGE)  /* how far back previous() looks for a line start */
#define DCLICK_MS 400
#define MAX_ENTRIES 4096
#define LIST_ROWS 60
#define N(a) ((int)(sizeof(a) / sizeof *(a)))

typedef struct { int x, y, w, h; } box;
typedef struct { int r, g, b; } rgb;

/* ====================================================================
 * the document and its view
 * ==================================================================== */

struct row {
    off_t start, next;
    int len, newline, valid;
    char text[COLS + 1];
};

struct line_image {
    char text[COLS + 1];
    int first, last, caret, current, focus;
};

struct snapshot {
    struct root root;
    off_t cursor, anchor;
    off_t changed;          /* where the edit that made this state starts */
};

struct location { off_t pos; uint64_t line, col; };

static struct snapshot doc, saved, undo[HISTORY], redo[HISTORY];
static int nundo, nredo;
static struct row view[ROWS];
static struct line_image drawn[ROWS];
static off_t top;
static int cw, ch, ascent, cols, rows, font, refresh = 1;
static int dragging, running = 1, preferred = -1;
static int zoom = 2;
static char message[300];
static int position_failed;
static struct location location = {0, 1, 1}, checkpoints[CHECKPOINTS];
static unsigned checkpoint_slot;
static unsigned char clipboard[CLIPBOARD];

/* where things are: worked out by resize() */
static int text_top, text_bottom, text_right, status_top;

enum act {
    A_NONE, A_NEW, A_OPEN, A_SAVE, A_SAVE_AS, A_QUIT, A_UNDO, A_REDO, A_CUT, A_COPY,
    A_PASTE, A_SELECT_ALL, A_FIND, A_FIND_NEXT, A_FIND_PREV, A_REPLACE, A_GOTO_LINE,
    A_GOTO_BYTE, A_ZOOM_IN, A_ZOOM_OUT, A_ZOOM_RESET, A_HELP, A_ABOUT
};
static enum act pending;        /* what to do once unsaved changes are dealt with */

static void action(enum act a);
static void paint(void);
static void resize(void);
static void alert(const char *title, const char *fmt, ...);

static off_t min(off_t a, off_t b) { return a < b ? a : b; }
static off_t max(off_t a, off_t b) { return a > b ? a : b; }
static int in_box(box b, int x, int y)
{
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int dirty(void)
{
    return doc.root.size != saved.root.size || doc.root.id != saved.root.id ||
           doc.root.height != saved.root.height;
}

static const char *doc_name(void)
{
    const char *p = doc_path(), *slash;

    if (!p)
        return "Untitled";
    slash = strrchr(p, '/');
    return slash && slash[1] ? slash + 1 : p;
}

static void report(const char *what)
{
    snprintf(message, sizeof message, "%s: %s", what, strerror(errno));
}

static int bytes(off_t pos, void *buf, size_t len)
{
    if (doc_read(doc.root, pos, buf, len) == 0)
        return 1;
    report("Read failed");
    return 0;
}

static void invalidate_position(off_t first)
{
    for (int i = 0; i < CHECKPOINTS; i++)
        if (checkpoints[i].pos > first)
            checkpoints[i] = (struct location){0};
    if (location.pos > first)
        location = (struct location){0, 1, 1};
    position_failed = 0;
}

/* Count in small slices after a long jump. Opening still reads only the
 * viewport, and the UI can handle input while line numbers catch up. */
static int count_position(void)
{
    unsigned char buf[DOC_PAGE];

    if (location.pos > doc.cursor)
        location = (struct location){0, 1, 1};
    if (location.pos == doc.cursor || position_failed)
        return 0;
    for (int i = 0; i < CHECKPOINTS; i++)
        if (checkpoints[i].pos > location.pos && checkpoints[i].pos <= doc.cursor)
            location = checkpoints[i];
    for (int page = 0; page < 8 && location.pos < doc.cursor; page++) {
        off_t boundary = CHECKPOINT_INTERVAL - location.pos % CHECKPOINT_INTERVAL;
        size_t len = (size_t)min(boundary, min(doc.cursor - location.pos, sizeof buf));

        if (!bytes(location.pos, buf, len)) {
            position_failed = 1;
            return 0;
        }
        for (size_t i = 0; i < len; i++) {
            if (buf[i] == '\n') {
                location.line++;
                location.col = 1;
            } else {
                location.col++;
            }
        }
        location.pos += (off_t)len;
        if (location.pos % CHECKPOINT_INTERVAL == 0)
            checkpoints[checkpoint_slot++ % CHECKPOINTS] = location;
    }
    return location.pos != doc.cursor;
}

/* Lay out just one visible row. Very long lines wrap without finding their
 * start or end, so a jump anywhere in one still costs only a viewport. */
static struct row row_at(off_t pos)
{
    struct row row = {.start = pos, .next = pos};
    int n = (int)min(doc.root.size - pos, cols + 1);
    char *end;

    if (!bytes(pos, row.text, (size_t)n))
        return row;
    row.valid = 1;
    end = memchr(row.text, '\n', (size_t)n);
    row.newline = end != NULL;
    row.len = end ? (int)(end - row.text) : (int)min(n, cols);
    row.next = pos + row.len + row.newline;
    for (int i = 0; i < row.len; i++) {
        unsigned char c = (unsigned char)row.text[i];

        row.text[i] = c == '\t' ? ' ' : (c >= 32 && c < 127 ? c : '.');
    }
    row.text[row.len] = 0;
    return row;
}

/* The start of the row before the one at pos. Rows wrap every cols bytes
 * from the start of their line, so find that within LOOKBACK bytes; a
 * longer line just steps back cols bytes, as a jump into it lays out. */
static off_t previous(off_t pos)
{
    unsigned char buf[DOC_PAGE], c;
    off_t end = pos, at, start = -1;

    if (pos <= 0 || !bytes(pos - 1, &c, 1))
        return max(0, pos);
    if (c == '\n')
        end--;
    for (at = end; at > 0 && end - at < LOOKBACK && start < 0; ) {
        size_t n = (size_t)min(at, sizeof buf);

        if (!bytes(at - (off_t)n, buf, n))
            return pos;
        for (size_t i = n; i-- > 0;)
            if (buf[i] == '\n') {
                start = at - (off_t)n + (off_t)i + 1;
                break;
            }
        at -= (off_t)n;
    }
    if (start < 0 && at == 0)
        start = 0;
    if (start < 0)
        return max(0, end - cols);
    if (end == start)
        return start;
    return start + (end - start - 1) / cols * cols;
}

/* The start of the row holding pos. */
static off_t row_start(off_t pos)
{
    unsigned char c;

    if (pos <= 0)
        return 0;
    if (pos >= doc.root.size) {
        pos = doc.root.size;
        if (bytes(pos - 1, &c, 1) && c == '\n')
            return pos;
        return previous(pos);
    }
    return previous(pos + 1);
}

static void layout(void)
{
    off_t pos = top;

    for (int i = 0; i < rows; i++) {
        if (pos < 0) {
            view[i] = (struct row){0};
            continue;
        }
        view[i] = row_at(pos);
        pos = view[i].next;
        if (!view[i].valid || pos == view[i].start ||
            (pos == doc.root.size && !view[i].newline))
            pos = -1;
    }
}

static int cursor_row(void)
{
    for (int i = 0; i < rows && view[i].valid; i++) {
        struct row *r = &view[i];

        if (doc.cursor >= r->start &&
            (doc.cursor < r->next ||
             (doc.cursor == r->next && !r->newline && r->next == doc.root.size)))
            return i;
    }
    return -1;
}

static void reveal(void)
{
    top = min(top, doc.root.size);
    if (cursor_row() >= 0)
        return;
    off_t start = row_start(doc.cursor);
    int back = doc.cursor < top ? 1 : rows - 1;

    while (back-- > 0 && start)
        start = previous(start);
    top = start;
    layout();
    if (cursor_row() < 0 && top != doc.cursor) {
        top = doc.cursor;
        layout();
    }
}

static void push(struct snapshot history[], int *count, struct snapshot s)
{
    if (*count == HISTORY) {
        memmove(history, history + 1, (HISTORY - 1) * sizeof *history);
        (*count)--;
    }
    history[(*count)++] = s;
}

static int replace(off_t start, off_t end, const void *data, size_t len)
{
    struct root edited = doc.root;

    if (start == end && !len)
        return 0;
    if (doc_edit(&edited, start, end - start, data, len) < 0) {
        report("Edit failed");
        return 0;
    }
    push(undo, &nundo, doc);
    nredo = 0;
    doc.root = edited;
    doc.changed = start;
    invalidate_position(start);
    doc.cursor = doc.anchor = start + (off_t)len;
    preferred = -1;
    top = min(top, start);
    layout();
    reveal();
    return 1;
}

static void type_text(const void *data, size_t len)
{
    replace(min(doc.cursor, doc.anchor), max(doc.cursor, doc.anchor), data, len);
}

static void move_to(off_t pos, int extend)
{
    doc.cursor = pos;
    if (!extend)
        doc.anchor = pos;
    reveal();
}

static void history_step(int back)
{
    off_t changed;

    if (back ? !nundo : !nredo)
        return;
    if (back) {
        changed = doc.changed;
        push(redo, &nredo, doc);
        doc = undo[--nundo];
    } else {
        push(undo, &nundo, doc);
        doc = redo[--nredo];
        changed = doc.changed;
    }
    /* line numbers before the edit still hold */
    invalidate_position(changed);
    top = min(top, doc.root.size);
    layout();
    reveal();
}

static void copy(int cut)
{
    off_t first = min(doc.cursor, doc.anchor), last = max(doc.cursor, doc.anchor);

    if (first == last)
        return;
    if (last - first > CLIPBOARD) {
        snprintf(message, sizeof message, "Clipboard limit is %d KiB", CLIPBOARD / 1024);
        return;
    }
    if (bytes(first, clipboard, (size_t)(last - first))) {
        x_set_clipboard((const char *)clipboard, (size_t)(last - first));
        if (cut)
            type_text(NULL, 0);
    }
}

static void reset_view(struct snapshot next)
{
    doc = saved = next;
    nundo = nredo = 0;
    preferred = -1;
    top = 0;
    invalidate_position(0);
    layout();
    refresh = 1;
}

static int open_file(const char *name)
{
    struct snapshot next = {0};

    if (doc_open(name, &next.root) < 0)
        return -1;
    reset_view(next);
    message[0] = 0;
    return 0;
}

static void new_doc(void)
{
    struct snapshot next = {0};

    doc_new(&next.root);
    reset_view(next);
    message[0] = 0;
}

static const char *why(int error)
{
    return error == EINVAL ? "Not a regular file" : strerror(error);
}

static int save(void)
{
    if (!dirty() && doc_exists()) {
        snprintf(message, sizeof message, "No changes to save");
        return 1;
    }
    snprintf(message, sizeof message, "Saving...");
    paint();
    if (doc_save(doc.root) < 0) {
        int error = errno;

        message[0] = 0;
        alert("Couldn't save the file", "%s could not be saved.\n\n%s.", doc_name(),
              error == EBUSY ? "It was changed by another program since it was opened"
                             : why(error));
        return 0;
    }
    saved = doc;
    snprintf(message, sizeof message, "Saved");
    return 1;
}

static void go_line(intmax_t number)
{
    unsigned char buf[DOC_PAGE];
    off_t pos = 0;

    snprintf(message, sizeof message, "Scanning lines...");
    paint();
    message[0] = 0;
    while (number > 1 && pos < doc.root.size) {
        size_t n = (size_t)min(doc.root.size - pos, sizeof buf);

        if (!bytes(pos, buf, n))
            return;
        for (size_t i = 0; i < n; i++)
            if (buf[i] == '\n' && --number == 1) {
                move_to(pos + (off_t)i + 1, 0);
                return;
            }
        pos += (off_t)n;
    }
    if (number == 1)
        move_to(pos, 0);
    else
        snprintf(message, sizeof message, "That line is past the end of the file");
}

/* ====================================================================
 * style: xmc's colours and fonts
 * ==================================================================== */

static const rgb RGB_BG = {13, 17, 23}, RGB_PANEL = {22, 27, 34}, RGB_TEXT = {230, 237, 243};
static const rgb ACC_BLUE = {88, 166, 255}, ACC_AMBER = {227, 179, 65}, ACC_RED = {248, 81, 73};

static unsigned c_bg, c_panel, c_grid, c_track, c_hover, c_text, c_dim, c_faint, c_sel,
    c_sel_dim, c_popup, c_line, c_blue, c_amber, c_red, c_def, c_def_hover;
static int f_ui, f_bold, f_big, smooth, ui_h, menu_h, status_h, item_h;

static unsigned px(rgb c) { return x_rgb(c.r, c.g, c.b); }

static rgb mix(rgb a, rgb b, int t)     /* a + (b - a) * t / 1000 */
{
    rgb c = {a.r + (b.r - a.r) * t / 1000, a.g + (b.g - a.g) * t / 1000,
             a.b + (b.b - a.b) * t / 1000};
    return c;
}

static int font_h(int f) { return x_font_ascent(f) + x_font_descent(f); }

static void round_rect(int x, int y, int w, int h, int r)
{
    if (w <= 0 || h <= 0)
        return;
    if (smooth && r >= 1 && x_aa_round_rect(X_FX(x), X_FX(y), X_FX(w), X_FX(h), X_FX(r), 255))
        return;
    if (r < 1 || w < 2 * r || h < 2 * r) {
        x_fill_rect(x, y, w, h);
        return;
    }
    x_fill_rect(x + r, y, w - 2 * r, h);
    x_fill_rect(x, y + r, w, h - 2 * r);
    x_fill_ellipse(x, y, 2 * r, 2 * r);
    x_fill_ellipse(x + w - 2 * r, y, 2 * r, 2 * r);
    x_fill_ellipse(x, y + h - 2 * r, 2 * r, 2 * r);
    x_fill_ellipse(x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r);
}

/* A line two pixels wide: smooth with RENDER. */
static void stroke(const int *pts, int n)
{
    x_fixed f[8];

    if (smooth) {
        for (int i = 0; i < 2 * n; i++)
            f[i] = X_FX(pts[i]);
        if (x_aa_polyline(f, n, X_FX(1.7), 255))
            return;
    }
    for (int i = 1; i < n; i++) {
        x_line(pts[2 * i - 2], pts[2 * i - 1], pts[2 * i], pts[2 * i + 1]);
        x_line(pts[2 * i - 2] + 1, pts[2 * i - 1], pts[2 * i] + 1, pts[2 * i + 1]);
    }
}

/* The shadow under a menu or dialog at b: see-through where the server
 * can blend, a darker panel colour where it can't. */
static void shadow(box b, int r)
{
    if (smooth) {
        x_color(x_rgb(0, 0, 0));
        x_aa_round_rect(X_FX(b.x - 3), X_FX(b.y - 1), X_FX(b.w + 6), X_FX(b.h + 6), X_FX(r + 3), 40);
        x_aa_round_rect(X_FX(b.x - 1), X_FX(b.y + 1), X_FX(b.w + 2), X_FX(b.h + 3), X_FX(r + 1), 70);
        return;
    }
    x_color(px(mix(RGB_BG, RGB_PANEL, 300)));
    round_rect(b.x - 4, b.y - 2, b.w + 8, b.h + 8, r + 2);
}

static void setup_style(void)
{
    c_bg = px(RGB_BG);
    c_panel = px(RGB_PANEL);
    c_grid = px(mix(RGB_PANEL, RGB_TEXT, 60));
    c_track = px(mix(RGB_PANEL, RGB_TEXT, 90));
    c_hover = px(mix(RGB_PANEL, RGB_TEXT, 150));
    c_text = px(RGB_TEXT);
    c_dim = x_rgb(139, 148, 158);
    c_faint = x_rgb(72, 79, 88);
    c_sel = px(mix(RGB_PANEL, ACC_BLUE, 220));
    c_sel_dim = px(mix(RGB_PANEL, RGB_TEXT, 55));
    c_popup = px(mix(RGB_PANEL, RGB_BG, 400));
    c_line = px(mix(RGB_BG, RGB_PANEL, 650));
    c_blue = px(ACC_BLUE);
    c_amber = px(ACC_AMBER);
    c_red = px(ACC_RED);
    c_def = px(mix(RGB_PANEL, ACC_BLUE, 450));
    c_def_hover = px(mix(RGB_PANEL, ACC_BLUE, 600));
    smooth = x_has_render();
    if ((f_ui = x_font_ttf("DejaVuSans.ttf", 12)) != 0) {
        f_bold = x_font_ttf("DejaVuSans-Bold.ttf", 12);
        f_big = x_font_ttf("DejaVuSans-Bold.ttf", 22);
    } else {
        f_ui = x_font("-adobe-helvetica-medium-r-normal--12-*-*-*-p-*-iso8859-1");
        f_bold = x_font("-adobe-helvetica-bold-r-normal--12-*-*-*-p-*-iso8859-1");
        f_big = x_font("-adobe-helvetica-bold-r-normal--24-*-*-*-p-*-iso8859-1");
    }
    if (!f_bold)
        f_bold = f_ui;
    if (!f_big)
        f_big = f_bold;
    ui_h = font_h(f_ui);
    menu_h = ui_h + 14;
    status_h = ui_h + 12;
    item_h = ui_h + 12;
}

/* ---- text ---- */

/* File names come as UTF-8; the fonts are Latin-1, so the rest shows '?'. */
static void to_latin1(char *out, size_t cap, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t n = 0;

    while (*p && n + 1 < cap) {
        unsigned c = *p++;

        if (c >= 0xc0 && c < 0xe0 && (*p & 0xc0) == 0x80) {
            c = (c & 0x1f) << 6 | (*p++ & 0x3f);
        } else if (c >= 0x80) {
            while ((*p & 0xc0) == 0x80)
                p++;
            c = '?';
        }
        out[n++] = (char)(c < 32 || c == 127 || (c >= 128 && c < 160) ? '?' : c);
    }
    out[n] = 0;
}

enum { TRIM_END, TRIM_START };

/* s shortened with "..." to fit w pixels in font f. */
static void fit(char *out, size_t cap, int f, const char *s, int w, int how)
{
    int n = (int)strlen(s);

    snprintf(out, cap, "%s", s);
    if (x_text_width(f, out) <= w)
        return;
    if (w <= x_text_width(f, "...")) {
        out[0] = 0;
        return;
    }
    if (how == TRIM_START) {
        for (int k = 1; k < n; k++) {
            snprintf(out, cap, "...%s", s + k);
            if (x_text_width(f, out) <= w)
                return;
        }
    } else {
        for (int k = n - 1; k > 0; k--) {
            snprintf(out, cap, "%.*s...", k, s);
            if (x_text_width(f, out) <= w)
                return;
        }
    }
    out[0] = 0;
}

/* Text with its top at y in [x, x + w): align 0 left, 1 right, 2 centre. */
static void text_at(int f, int x, int y, int w, int align, int how, unsigned color, const char *s)
{
    char buf[PATH_MAX];
    int tw;

    fit(buf, sizeof buf, f, s, w, how);
    if (!buf[0])
        return;
    tw = x_text_width(f, buf);
    x_text_font(f);
    x_color(color);
    x_text(align == 1 ? x + w - tw : align == 2 ? x + (w - tw) / 2 : x, y + x_font_ascent(f), buf);
}

static int width_n(int f, const char *s, int n)
{
    char buf[PATH_MAX];

    snprintf(buf, sizeof buf, "%.*s", n, s);
    return x_text_width(f, buf);
}

static void human(char *out, size_t cap, off_t n)
{
    static const char *const unit[] = {"bytes", "KB", "MB", "GB", "TB"};
    intmax_t div = 1024, t;
    int u = 1;

    if (n < 1024) {
        snprintf(out, cap, "%jd byte%s", (intmax_t)n, n == 1 ? "" : "s");
        return;
    }
    while (n / div >= 1024 && u < 4) {
        div *= 1024;
        u++;
    }
    t = (intmax_t)n / (div / 1024) * 10 / 1024;
    snprintf(out, cap, "%jd.%jd %s", t / 10, t % 10, unit[u]);
}

/* ---- change tracking: a hash of what each part shows ---- */

static uint64_t hash_str(const char *s)
{
    uint64_t h = 1469598103934665603ull;

    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ull;
    }
    return h ? h : 1;
}

static int differs(uint64_t *slot, const char *fmt, ...)
{
    char buf[PATH_MAX + 512];
    va_list ap;
    uint64_t h;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    h = hash_str(buf);
    if (*slot == h)
        return 0;
    *slot = h;
    return 1;
}

enum {
    K_MENUBAR, K_STATUS, K_SCROLL, K_TITLE, K_FB_BG, K_FB_F1, K_FB_F2, K_FB_PREV, K_FB_NEXT,
    K_FB_CHECK, K_FB_CLOSE, K_FB_REP, K_FB_ALL, NBKEYS
};
static uint64_t bkey[NBKEYS], okey[LIST_ROWS + 20];

/* ====================================================================
 * widgets: text fields, buttons, check boxes
 * ==================================================================== */

struct field { char s[PATH_MAX]; int len, pos, all, cap, edits; };

static void field_set(struct field *f, const char *s, int cap)
{
    f->cap = cap > (int)sizeof f->s ? (int)sizeof f->s : cap;
    snprintf(f->s, (size_t)f->cap, "%s", s);
    f->len = f->pos = (int)strlen(f->s);
    f->all = 0;
    f->edits++;
}

static void field_insert(struct field *f, const char *s, int n)
{
    if (f->all) {
        f->len = f->pos = 0;
        f->s[0] = 0;
        f->all = 0;
    }
    if (n > f->cap - 1 - f->len)
        n = f->cap - 1 - f->len;
    if (n <= 0)
        return;
    memmove(f->s + f->pos + n, f->s + f->pos, (size_t)(f->len - f->pos + 1));
    memcpy(f->s + f->pos, s, (size_t)n);
    f->len += n;
    f->pos += n;
    f->edits++;
}

static void field_paste(struct field *f, const char *s, int n)
{
    int k = 0;

    while (k < n && (unsigned char)s[k] >= 32 && s[k] != 127)
        k++;
    field_insert(f, s, k);
}

/* Keys for a field; 1 if it used the key. */
static int field_key(struct field *f, int k, int mods)
{
    if (mods & X_MOD_CTRL) {
        if (k == 'a' || k == 'A') {
            f->all = f->len > 0;
            f->pos = f->len;
            return 1;
        }
        if (k == 'v' || k == 'V') {
            x_request_paste();
            return 1;
        }
        return k == 'x' || k == 'X' || k == 'c' || k == 'C';
    }
    switch (k) {
    case X_KEY_LEFT: f->pos = f->all ? 0 : (int)max(0, f->pos - 1); f->all = 0; return 1;
    case X_KEY_RIGHT: f->pos = f->all ? f->len : (int)min(f->len, f->pos + 1); f->all = 0; return 1;
    case X_KEY_HOME: f->pos = 0; f->all = 0; return 1;
    case X_KEY_END: f->pos = f->len; f->all = 0; return 1;
    case X_KEY_BACKSPACE: case X_KEY_DELETE:
        if (f->all) {
            f->len = f->pos = 0;
            f->s[0] = 0;
            f->all = 0;
        } else if (k == X_KEY_BACKSPACE && f->pos > 0) {
            memmove(f->s + f->pos - 1, f->s + f->pos, (size_t)(f->len - f->pos + 1));
            f->pos--;
            f->len--;
        } else if (k == X_KEY_DELETE && f->pos < f->len) {
            memmove(f->s + f->pos, f->s + f->pos + 1, (size_t)(f->len - f->pos));
            f->len--;
        } else {
            return 1;
        }
        f->edits++;
        return 1;
    }
    if (k >= 32 && k < 127 && !(mods & X_MOD_ALT)) {
        char c = (char)k;

        field_insert(f, &c, 1);
        return 1;
    }
    return 0;
}

/* The first character shown, so that the caret is in view. */
static int field_scroll(const struct field *f, int room)
{
    int start = 0;

    while (start < f->pos && width_n(f_ui, f->s + start, f->pos - start) > room)
        start++;
    return start;
}

static void field_click(struct field *f, box g, int x)
{
    int start = field_scroll(f, g.w - 20), best = start, dist = INT_MAX;

    for (int i = start; i <= f->len; i++) {
        int d = abs(g.x + 10 + width_n(f_ui, f->s + start, i - start) - x);

        if (d < dist) {
            dist = d;
            best = i;
        }
    }
    f->pos = best;
    f->all = 0;
}

static void draw_field(box g, const struct field *f, int focus, unsigned around)
{
    int room = g.w - 20, ty = g.y + (g.h - ui_h) / 2, start = field_scroll(f, room);
    char shown[PATH_MAX];

    x_color(around);
    x_fill_rect(g.x, g.y, g.w, g.h);
    x_color(focus ? c_blue : c_track);
    round_rect(g.x, g.y, g.w, g.h, 6);
    x_color(c_bg);
    round_rect(g.x + 1, g.y + 1, g.w - 2, g.h - 2, 5);
    snprintf(shown, sizeof shown, "%s", f->s + start);
    if (f->all && focus) {
        x_color(c_sel);
        x_fill_rect(g.x + 8, ty - 1, (int)min(room, x_text_width(f_ui, shown)) + 4, ui_h + 2);
    }
    text_at(f_ui, g.x + 10, ty, room, 0, TRIM_END, c_text, shown);
    if (focus && !f->all) {
        x_color(c_blue);
        x_fill_rect(g.x + 10 + width_n(f_ui, f->s + start, f->pos - start), ty, 1, ui_h);
    }
}

static int button_w(const char *label) { return (int)max(84, x_text_width(f_bold, label) + 32); }

static void draw_button(box g, const char *label, int def, int hover, int focus, int on,
                        unsigned around)
{
    x_color(around);
    x_fill_rect(g.x - 2, g.y - 2, g.w + 4, g.h + 4);
    if (focus) {
        x_color(c_blue);
        round_rect(g.x - 2, g.y - 2, g.w + 4, g.h + 4, 8);
        x_color(around);
        round_rect(g.x - 1, g.y - 1, g.w + 2, g.h + 2, 7);
    }
    x_color(!on ? c_panel : def ? (hover ? c_def_hover : c_def) : (hover ? c_hover : c_track));
    round_rect(g.x, g.y, g.w, g.h, 6);
    text_at(f_bold, g.x, g.y + (g.h - font_h(f_bold)) / 2, g.w, 2, TRIM_END,
            on ? c_text : c_faint, label);
}

static int check_w(const char *label) { return 22 + x_text_width(f_ui, label); }

static void draw_check(box g, const char *label, int on, int hover, unsigned around)
{
    int s = 14, y = g.y + (g.h - s) / 2;

    x_color(around);
    x_fill_rect(g.x, g.y, g.w, g.h);
    x_color(on ? c_blue : hover ? c_dim : c_track);
    round_rect(g.x, y, s, s, 3);
    if (on) {
        int mark[] = {g.x + 3, y + 7, g.x + 6, y + 10, g.x + 11, y + 4};

        x_color(c_bg);
        stroke(mark, 3);
    } else {
        x_color(c_bg);
        round_rect(g.x + 1, y + 1, s - 2, s - 2, 2);
    }
    text_at(f_ui, g.x + s + 8, g.y + (g.h - ui_h) / 2, g.w - s - 8, 0, TRIM_END,
            hover ? c_text : c_dim, label);
}

static void draw_close(box g, int hover, unsigned around)
{
    int cx = g.x + g.w / 2, cy = g.y + g.h / 2, r = 4;
    int a[] = {cx - r, cy - r, cx + r, cy + r}, b[] = {cx - r, cy + r, cx + r, cy - r};

    x_color(around);
    x_fill_rect(g.x, g.y, g.w, g.h);
    if (hover) {
        x_color(c_hover);
        round_rect(g.x, g.y, g.w, g.h, 5);
    }
    x_color(hover ? c_text : c_dim);
    stroke(a, 2);
    stroke(b, 2);
}

static void icon_folder(int x, int y)
{
    x_color(c_amber);
    round_rect(x, y, 7, 5, 1);
    round_rect(x, y + 2, 16, 11, 2);
}

static void icon_file(int x, int y, unsigned around)
{
    x_color(c_dim);
    round_rect(x + 2, y, 12, 14, 2);
    x_color(around);
    x_fill_rect(x + 3, y + 1, 10, 12);
    x_color(c_dim);
    x_fill_rect(x + 5, y + 4, 6, 1);
    x_fill_rect(x + 5, y + 7, 6, 1);
    x_fill_rect(x + 5, y + 10, 4, 1);
}

/* ====================================================================
 * overlays: what a menu or dialog covers is saved on the server and put
 * back with one copy when it goes
 * ==================================================================== */

enum { OV_NONE, OV_MENU, OV_DIALOG };
static int ov, ov_frame;        /* ov_frame: its frame is still to be drawn */
static int bar_hover = -1;      /* the menu title under the pointer */
static struct { box b; unsigned save; } under;

static void under_save(box b)
{
    if (b.x < 0) { b.w += b.x; b.x = 0; }
    if (b.y < 0) { b.h += b.y; b.y = 0; }
    if (b.x + b.w > x_width) b.w = x_width - b.x;
    if (b.y + b.h > x_height) b.h = x_height - b.y;
    under.b = b;
    under.save = 0;
    if (b.w <= 0 || b.h <= 0)
        return;
    under.save = x_canvas(b.w, b.h);
    x_target(under.save);
    x_blit(x_frame(), b.x, b.y, b.w, b.h, 0, 0);
    x_target(0);
}

static void under_drop(void)
{
    if (under.save)
        x_free_canvas(under.save);
    under.save = 0;
}

static void under_restore(void)
{
    if (!under.save)
        return;
    x_blit(under.save, 0, 0, under.b.w, under.b.h, under.b.x, under.b.y);
    under_drop();
}

static void overlay_close(void)
{
    under_restore();
    ov = OV_NONE;
    ov_frame = 0;
}

static void overlay_open(int kind)
{
    if (ov)
        overlay_close();
    ov = kind;
    ov_frame = 1;
    bar_hover = -1;
    x_cursor(X_CURSOR_DEFAULT);
}

/* ====================================================================
 * menus
 * ==================================================================== */

struct mitem { const char *label, *keys; enum act act; };
static const struct mitem m_file[] = {
    {"New", "Ctrl+N", A_NEW}, {"Open...", "Ctrl+O", A_OPEN}, {0, 0, A_NONE},
    {"Save", "Ctrl+S", A_SAVE}, {"Save As...", "Ctrl+Shift+S", A_SAVE_AS}, {0, 0, A_NONE},
    {"Quit", "Ctrl+Q", A_QUIT}};
static const struct mitem m_edit[] = {
    {"Undo", "Ctrl+Z", A_UNDO}, {"Redo", "Ctrl+Y", A_REDO}, {0, 0, A_NONE},
    {"Cut", "Ctrl+X", A_CUT}, {"Copy", "Ctrl+C", A_COPY}, {"Paste", "Ctrl+V", A_PASTE},
    {"Select All", "Ctrl+A", A_SELECT_ALL}, {0, 0, A_NONE},
    {"Find...", "Ctrl+F", A_FIND}, {"Find Next", "F3", A_FIND_NEXT},
    {"Find Previous", "Shift+F3", A_FIND_PREV}, {"Replace...", "Ctrl+H", A_REPLACE},
    {0, 0, A_NONE},
    {"Go to Line...", "Ctrl+L", A_GOTO_LINE}, {"Go to Byte Offset...", "Ctrl+G", A_GOTO_BYTE}};
static const struct mitem m_view[] = {
    {"Zoom In", "Ctrl++", A_ZOOM_IN}, {"Zoom Out", "Ctrl+-", A_ZOOM_OUT},
    {"Actual Size", "Ctrl+0", A_ZOOM_RESET}};
static const struct mitem m_help[] = {
    {"Keyboard Shortcuts", "F1", A_HELP}, {0, 0, A_NONE}, {"About xedit", "", A_ABOUT}};

struct menu { const char *title; const struct mitem *items; int n, x, w; };
static struct menu menus[] = {
    {"File", m_file, N(m_file), 0, 0}, {"Edit", m_edit, N(m_edit), 0, 0},
    {"View", m_view, N(m_view), 0, 0}, {"Help", m_help, N(m_help), 0, 0}};

static int menu_open = -1, menu_hover = -1;
static box menu_box;
static int menu_iy[20];

#define ZOOMS 7
static const int zoom_px[ZOOMS] = {10, 11, 13, 15, 17, 20, 24};

static struct {
    int open, replace, focus, match_case, hover, top, h;
    struct field find, repl;
    box f1, f2, prev, next, check, close, rep, all;
} fbar;

static int enabled(enum act a)
{
    switch (a) {
    case A_UNDO: return nundo > 0;
    case A_REDO: return nredo > 0;
    case A_CUT: case A_COPY: return doc.cursor != doc.anchor;
    case A_FIND_NEXT: case A_FIND_PREV: return fbar.find.len > 0;
    case A_ZOOM_IN: return zoom < ZOOMS - 1;
    case A_ZOOM_OUT: return zoom > 0;
    case A_ZOOM_RESET: return zoom != 2;
    default: return 1;
    }
}

static void menu_titles(void)
{
    int x = 6;

    for (int i = 0; i < N(menus); i++) {
        menus[i].x = x;
        menus[i].w = x_text_width(f_ui, menus[i].title) + 22;
        x += menus[i].w;
    }
}

static int title_at(int x, int y)
{
    if (y < 0 || y >= menu_h)
        return -1;
    for (int i = 0; i < N(menus); i++)
        if (x >= menus[i].x && x < menus[i].x + menus[i].w)
            return i;
    return -1;
}

static void menu_layout(void)
{
    const struct menu *m = &menus[menu_open];
    int w = 210, y = 5;

    for (int i = 0; i < m->n; i++) {
        const struct mitem *it = &m->items[i];

        if (it->label)
            w = (int)max(w, 16 + x_text_width(f_ui, it->label) + 44 + x_text_width(f_ui, it->keys) + 16);
        menu_iy[i] = y;
        y += it->label ? item_h : 9;
    }
    menu_box = (box){m->x, menu_h + 2, w, y + 5};
    if (menu_box.x + w > x_width - 6)
        menu_box.x = (int)max(4, x_width - 6 - w);
}

static int menu_step(int menu, int from, int dir)
{
    const struct menu *m = &menus[menu];
    int i = from;

    for (int s = 0; s < m->n; s++) {
        i = i < 0 ? (dir > 0 ? 0 : m->n - 1) : (i + dir + m->n) % m->n;
        if (m->items[i].label && enabled(m->items[i].act))
            return i;
    }
    return from;
}

static void menu_show(int i, int keyboard)
{
    if (ov == OV_DIALOG)
        return;
    menu_open = i;
    menu_hover = keyboard ? menu_step(i, -1, 1) : -1;
    overlay_open(OV_MENU);
    menu_layout();
}

static void menu_hide(void)
{
    if (ov == OV_MENU)
        overlay_close();
    menu_open = menu_hover = -1;
}

static void menu_choose(int i)
{
    enum act a = menus[menu_open].items[i].act;

    menu_hide();
    action(a);
}

static void menu_frame(void)
{
    box b = menu_box;
    const struct menu *m = &menus[menu_open];

    under_save((box){b.x - 4, b.y - 2, b.w + 8, b.h + 8});
    shadow(b, 6);
    x_color(c_track);
    round_rect(b.x - 1, b.y - 1, b.w + 2, b.h + 2, 7);
    x_color(c_popup);
    round_rect(b.x, b.y, b.w, b.h, 6);
    x_color(c_grid);
    for (int i = 0; i < m->n; i++)
        if (!m->items[i].label)
            x_fill_rect(b.x + 10, b.y + menu_iy[i] + 4, b.w - 20, 1);
}

static int menu_parts(void)
{
    const struct menu *m = &menus[menu_open];
    box b = menu_box;
    int changed = 0;

    for (int i = 0; i < m->n; i++) {
        const struct mitem *it = &m->items[i];
        int on = enabled(it->act), hover = i == menu_hover, y = b.y + menu_iy[i];
        int ty = y + (item_h - ui_h) / 2;

        if (!it->label || !differs(&okey[i], "%d|%d", on, hover))
            continue;
        changed = 1;
        x_color(c_popup);
        x_fill_rect(b.x + 4, y, b.w - 8, item_h);
        if (hover) {
            x_color(c_sel);
            round_rect(b.x + 4, y, b.w - 8, item_h, 4);
        }
        text_at(f_ui, b.x + 16, ty, b.w - 32, 0, TRIM_END, on ? c_text : c_faint, it->label);
        text_at(f_ui, b.x + 16, ty, b.w - 32, 1, TRIM_END, on ? c_dim : c_faint, it->keys);
    }
    return changed;
}

static void menu_key(int k)
{
    switch (k) {
    case X_KEY_ESCAPE:
    case X_KEY_F1 + 9:
        menu_hide();
        break;
    case X_KEY_LEFT: case X_KEY_RIGHT:
        menu_show((menu_open + (k == X_KEY_LEFT ? N(menus) - 1 : 1)) % N(menus), 1);
        break;
    case X_KEY_UP: case X_KEY_DOWN:
        menu_hover = menu_step(menu_open, menu_hover, k == X_KEY_UP ? -1 : 1);
        break;
    case X_KEY_RETURN: case X_KEY_KP_ENTER: case ' ':
        if (menu_hover >= 0)
            menu_choose(menu_hover);
        break;
    }
}

static void menu_pointer(const struct x_event *e)
{
    const struct menu *m = &menus[menu_open];
    int title = title_at(e->x, e->y), item = -1;

    if (title >= 0) {
        if (e->type == XE_BUTTON_DOWN && e->button == 1) {
            if (title == menu_open)
                menu_hide();
            else
                menu_show(title, 0);
        } else if (e->type == XE_MOTION && title != menu_open) {
            menu_show(title, 0);
        }
        return;
    }
    if (in_box(menu_box, e->x, e->y))
        for (int i = 0; i < m->n; i++)
            if (m->items[i].label && enabled(m->items[i].act) &&
                e->y >= menu_box.y + menu_iy[i] && e->y < menu_box.y + menu_iy[i] + item_h)
                item = i;
    if (e->type == XE_MOTION)
        menu_hover = item;
    else if (e->type == XE_BUTTON_UP && e->button == 1 && item >= 0)
        menu_choose(item);
    else if (e->type == XE_BUTTON_DOWN && !in_box(menu_box, e->x, e->y))
        menu_hide();
}

/* ====================================================================
 * dialogs
 * ==================================================================== */

enum { D_ALERT, D_ASK_SAVE, D_GOTO, D_HELP, D_ABOUT, D_FILE };
enum { ANS_CANCEL, ANS_OK, ANS_DISCARD };

struct button { const char *label; int result; box b; };
static struct {
    int kind;
    enum act purpose;           /* D_GOTO: which; D_FILE: A_OPEN or A_SAVE_AS */
    char title[96], text[700], error[300];
    int warn;                   /* the error line is a question, not a failure */
    struct button btn[3];
    int nbtn, def, cancel;
    int focus;                  /* -1: the field, else a button */
    int hover;                  /* a button, 100 + a list row, 200 check, 201 up */
    int has_field;
    struct field field;
    box b, field_b, error_b;
    int text_y, lines;
    char line[10][160];
} dlg;

struct entry { size_t name; off_t size; int dir; };
static struct {
    char dir[PATH_MAX];
    struct entry *e;
    int n;
    char *pool;
    size_t used, cap;
    int sel, scroll, hidden, list_active, replace_ok, gen, rows, row_h, click_row;
    long long click_ms;
    box up, path, list, check;
} fb;

static const char *const help_rows[][2] = {
    {"Ctrl+N", "New document"}, {"Ctrl+O", "Open a file"}, {"Ctrl+S", "Save"},
    {"Ctrl+Shift+S", "Save under another name"}, {"Ctrl+Q", "Quit"},
    {"Ctrl+Z / Ctrl+Y", "Undo / redo"}, {"Ctrl+X / C / V", "Cut / copy / paste"},
    {"Ctrl+A", "Select all"}, {"Shift+arrows", "Select text (or drag)"},
    {"Ctrl+F", "Find"}, {"F3 / Shift+F3", "Next / previous match"},
    {"Ctrl+H", "Find and replace"}, {"Ctrl+L", "Go to line"}, {"Ctrl+G", "Go to byte offset"},
    {"Ctrl+Home / End", "Start / end of file"}, {"Ctrl++ / Ctrl+-", "Zoom in / out"},
    {"Ctrl+0", "Actual size"}, {"F10, Alt+letter", "Open the menus"},
};

static void dialog_begin(int kind, const char *title)
{
    memset(&dlg, 0, sizeof dlg);
    dlg.kind = kind;
    dlg.hover = -1;
    snprintf(dlg.title, sizeof dlg.title, "%s", title);
}

static void dialog_button(const char *label, int result)
{
    dlg.btn[dlg.nbtn].label = label;
    dlg.btn[dlg.nbtn++].result = result;
}

/* Lines of s that fit w pixels; a newline starts a paragraph. */
static int wrap(const char *s, int f, int w, char out[][160], int maxn)
{
    int n = 0;

    for (const char *p = s; n < maxn;) {
        const char *end = strchr(p, '\n');
        size_t plen = end ? (size_t)(end - p) : strlen(p), i = 0;

        if (plen == 0)
            out[n++][0] = 0;
        while (i < plen && n < maxn) {
            size_t best = 0, j = i;

            for (;;) {
                size_t k = j, len;
                char buf[160];

                while (k < plen && p[k] != ' ')
                    k++;
                len = k - i < sizeof buf ? k - i : sizeof buf - 1;
                memcpy(buf, p + i, len);
                buf[len] = 0;
                if (best && x_text_width(f, buf) > w)
                    break;
                best = len;
                if (k >= plen || len == sizeof buf - 1)
                    break;
                j = k + 1;
            }
            memcpy(out[n], p + i, best);
            out[n++][best] = 0;
            i += best;
            while (i < plen && p[i] == ' ')
                i++;
        }
        if (!end)
            break;
        p = end + 1;
    }
    return n;
}

static void buttons_at(int right, int y)
{
    for (int i = dlg.nbtn - 1; i >= 0; i--) {
        int w = button_w(dlg.btn[i].label);

        right -= w;
        dlg.btn[i].b = (box){right, y, w, 30};
        right -= 8;
    }
}

static void file_layout(void)
{
    int w = (int)min(640, x_width - 40), h = (int)min(480, x_height - 40);
    int L, R, by, ey, ny, lw = x_text_width(f_ui, "File name") + 14;
    box b = {(x_width - w) / 2, (int)max(10, (x_height - h) / 3), w, h};

    dlg.b = b;
    L = b.x + 18;
    R = b.x + w - 18;
    fb.up = (box){L, b.y + 44, 64, 28};
    fb.path = (box){L + 72, b.y + 44, R - L - 72, 28};
    by = b.y + h - 48;
    buttons_at(R, by);
    fb.check = (box){L, by, check_w("Show hidden files") + 4, 30};
    ey = by - 10 - (ui_h + 4);
    dlg.error_b = (box){L, ey, R - L, ui_h + 4};
    ny = ey - 8 - 30;
    dlg.field_b = (box){L + lw, ny, R - L - lw, 30};
    fb.list = (box){L, b.y + 82, R - L, (int)max(40, ny - 12 - (b.y + 82))};
    fb.row_h = ui_h + 10;
    fb.rows = (int)max(1, min(LIST_ROWS, (fb.list.h - 8) / fb.row_h));
    if (fb.sel >= 0 && fb.sel >= fb.scroll + fb.rows)
        fb.scroll = fb.sel - fb.rows + 1;
}

static void dialog_layout(void)
{
    int w, y = 46, lh = ui_h + 6;

    if (dlg.kind == D_FILE) {
        file_layout();
        return;
    }
    if (dlg.kind == D_HELP) {
        int half = (N(help_rows) + 1) / 2;

        w = (int)min(700, x_width - 40);
        dlg.text_y = y + 6;
        y += 6 + half * (ui_h + 8) + 14;
    } else {
        w = (int)min(dlg.kind == D_GOTO ? 380 : 450, x_width - 40);
        if (dlg.kind == D_ABOUT)
            y += font_h(f_big) + 10;
        dlg.lines = wrap(dlg.text, f_ui, w - 36, dlg.line, N(dlg.line));
        dlg.text_y = y;
        y += dlg.lines * lh;
        if (dlg.has_field) {
            dlg.field_b = (box){18, y + 6, w - 36, 30};
            y += 42;
            dlg.error_b = (box){18, y + 2, w - 36, ui_h + 4};
            y += ui_h + 8;
        }
        y += 12;
    }
    dlg.b = (box){(x_width - w) / 2, 0, w, (int)min(y + 48, x_height - 20)};
    dlg.b.y = (int)max(10, (x_height - dlg.b.h) / 3);
    buttons_at(dlg.b.x + w - 18, dlg.b.y + dlg.b.h - 48);
    dlg.field_b.x += dlg.b.x;
    dlg.field_b.y += dlg.b.y;
    dlg.error_b.x += dlg.b.x;
    dlg.error_b.y += dlg.b.y;
}

static void dialog_show(void)
{
    overlay_open(OV_DIALOG);
    dialog_layout();
}

static void dialog_frame(void)
{
    box b = dlg.b;

    under_save((box){b.x - 4, b.y - 2, b.w + 8, b.h + 8});
    shadow(b, 8);
    x_color(c_track);
    round_rect(b.x - 1, b.y - 1, b.w + 2, b.h + 2, 9);
    x_color(c_popup);
    round_rect(b.x, b.y, b.w, b.h, 8);
    x_color(dlg.kind == D_ALERT ? c_red : c_blue);
    round_rect(b.x + 18, b.y + 17, 4, x_font_ascent(f_bold) - 1, 2);
    text_at(f_bold, b.x + 28, b.y + 14, b.w - 46, 0, TRIM_END, c_text, dlg.title);
    if (dlg.kind == D_HELP) {
        int half = (N(help_rows) + 1) / 2, cwid = (b.w - 36 - 24) / 2;

        for (int i = 0; i < N(help_rows); i++) {
            int x = b.x + 18 + (i >= half ? cwid + 24 : 0);
            int y = b.y + dlg.text_y + (i % half) * (ui_h + 8);
            int kw = (int)min(cwid / 2, 130);

            text_at(f_bold, x, y, kw - 8, 0, TRIM_END, c_text, help_rows[i][0]);
            text_at(f_ui, x + kw, y, cwid - kw, 0, TRIM_END, c_dim, help_rows[i][1]);
        }
    }
    if (dlg.kind == D_ABOUT)
        text_at(f_big, b.x + 18, b.y + 46, b.w - 36, 0, TRIM_END, c_text, "xedit");
    for (int i = 0; i < dlg.lines; i++)
        text_at(f_ui, b.x + 18, b.y + dlg.text_y + i * (ui_h + 6), b.w - 36, 0, TRIM_END,
                dlg.kind == D_ABOUT && i > 0 ? c_dim : c_text, dlg.line[i]);
    if (dlg.kind == D_FILE) {
        text_at(f_ui, b.x + 18, dlg.field_b.y + (30 - ui_h) / 2, dlg.field_b.x - b.x - 18, 0,
                TRIM_END, c_text, "File name");
        x_color(c_track);
        round_rect(fb.list.x, fb.list.y, fb.list.w, fb.list.h, 6);
        x_color(c_bg);
        round_rect(fb.list.x + 1, fb.list.y + 1, fb.list.w - 2, fb.list.h - 2, 5);
    }
}

static int file_parts(void)
{
    int changed = 0, bar = fb.n > fb.rows;
    int rx = fb.list.x + 4, rw = fb.list.w - 8 - (bar ? 10 : 0);

    if (differs(&okey[10], "%s", fb.dir)) {
        char t[PATH_MAX];

        changed = 1;
        x_color(c_popup);
        x_fill_rect(fb.path.x, fb.path.y, fb.path.w, fb.path.h);
        x_color(c_panel);
        round_rect(fb.path.x, fb.path.y, fb.path.w, fb.path.h, 6);
        to_latin1(t, sizeof t, fb.dir);
        text_at(f_ui, fb.path.x + 10, fb.path.y + (fb.path.h - ui_h) / 2, fb.path.w - 20, 0,
                TRIM_START, c_dim, t);
    }
    if (differs(&okey[11], "%d|%d", dlg.hover == 201, strcmp(fb.dir, "/") != 0)) {
        changed = 1;
        draw_button(fb.up, "Up", 0, dlg.hover == 201, 0, strcmp(fb.dir, "/") != 0, c_popup);
    }
    if (differs(&okey[12], "%d|%d", fb.hidden, dlg.hover == 200)) {
        changed = 1;
        draw_check(fb.check, "Show hidden files", fb.hidden, dlg.hover == 200, c_popup);
    }
    /* the scroll bar first: rows drawn full width when there is none */
    if (bar && differs(&okey[13], "%d|%d|%d|%d", fb.gen, fb.scroll, fb.n, fb.rows)) {
        box t = {fb.list.x + fb.list.w - 12, fb.list.y + 4, 6, fb.list.h - 8};
        int th = (int)max(20, t.h * fb.rows / fb.n);
        int ty = t.y + (t.h - th) * fb.scroll / (int)max(1, fb.n - fb.rows);

        changed = 1;
        x_color(c_bg);
        x_fill_rect(t.x, t.y, t.w, t.h);
        x_color(c_track);
        round_rect(t.x, ty, t.w, th, 3);
    }
    if (!bar)
        okey[13] = 0;
    for (int r = 0; r < fb.rows; r++) {
        int i = fb.scroll + r, y = fb.list.y + 4 + r * fb.row_h;
        int sel = i == fb.sel, hover = dlg.hover == 100 + i;
        unsigned around = sel ? c_sel : hover ? c_sel_dim : c_bg;

        if (!differs(&okey[20 + r], "%d|%d|%d|%d|%d|%d", fb.gen, i, i < fb.n, sel, hover, rw))
            continue;
        changed = 1;
        x_color(c_bg);
        x_fill_rect(rx, y, rw, fb.row_h);
        if (i >= fb.n) {
            if (i == 0)
                text_at(f_ui, rx, y + (fb.row_h - ui_h) / 2, rw, 2, TRIM_END, c_dim,
                        "This folder is empty");
            continue;
        }
        if (around != c_bg) {
            x_color(around);
            round_rect(rx, y, rw, fb.row_h, 4);
        }
        {
            struct entry *en = &fb.e[i];
            char name[NAME_MAX * 2 + 2], size[32] = "";
            int iy = y + (fb.row_h - 14) / 2, sw;

            if (en->dir)
                icon_folder(rx + 8, iy);
            else
                icon_file(rx + 8, iy, around);
            to_latin1(name, sizeof name, fb.pool + en->name);
            if (!en->dir)
                human(size, sizeof size, en->size);
            sw = size[0] ? x_text_width(f_ui, size) + 16 : 0;
            text_at(f_ui, rx + 32, y + (fb.row_h - ui_h) / 2, rw - 42 - sw, 0, TRIM_END, c_text,
                    name);
            if (size[0])
                text_at(f_ui, rx, y + (fb.row_h - ui_h) / 2, rw - 10, 1, TRIM_END, c_dim, size);
        }
    }
    return changed;
}

static int dialog_parts(void)
{
    int changed = 0;

    for (int i = 0; i < dlg.nbtn; i++) {
        struct button *b = &dlg.btn[i];

        if (!differs(&okey[i], "%d|%d|%d|%s", dlg.hover == i, dlg.focus == i, i == dlg.def,
                     b->label))
            continue;
        changed = 1;
        draw_button(b->b, b->label, i == dlg.def, dlg.hover == i, dlg.focus == i, 1, c_popup);
    }
    if (dlg.has_field && differs(&okey[8], "%d|%d|%d|%s", dlg.focus == -1, dlg.field.pos,
                                 dlg.field.all, dlg.field.s)) {
        changed = 1;
        draw_field(dlg.field_b, &dlg.field, dlg.focus == -1, c_popup);
    }
    if (dlg.has_field && differs(&okey[9], "%d|%s", dlg.warn, dlg.error)) {
        changed = 1;
        x_color(c_popup);
        x_fill_rect(dlg.error_b.x, dlg.error_b.y, dlg.error_b.w, dlg.error_b.h);
        text_at(f_ui, dlg.error_b.x, dlg.error_b.y + 2, dlg.error_b.w, 0, TRIM_END,
                dlg.warn ? c_amber : c_red, dlg.error);
    }
    if (dlg.kind == D_FILE)
        changed |= file_parts();
    return changed;
}

static void alert(const char *title, const char *fmt, ...)
{
    va_list ap;

    if (ov == OV_MENU)
        menu_hide();
    dialog_begin(D_ALERT, title);
    va_start(ap, fmt);
    vsnprintf(dlg.text, sizeof dlg.text, fmt, ap);
    va_end(ap);
    dialog_button("OK", ANS_OK);
    dialog_show();
}

static void ask_save(enum act next)
{
    pending = next;
    dialog_begin(D_ASK_SAVE, "Save changes?");
    snprintf(dlg.text, sizeof dlg.text,
             "Do you want to save the changes you made to \"%s\"?\n\n"
             "Your changes will be lost if you don't save them.", doc_name());
    dialog_button("Don't Save", ANS_DISCARD);
    dialog_button("Cancel", ANS_CANCEL);
    dialog_button("Save", ANS_OK);
    dlg.def = dlg.focus = 2;
    dlg.cancel = 1;
    dialog_show();
}

static void goto_dialog(enum act what)
{
    char now[32];

    dialog_begin(D_GOTO, what == A_GOTO_LINE ? "Go to Line" : "Go to Byte Offset");
    dlg.purpose = what;
    if (what == A_GOTO_LINE) {
        snprintf(dlg.text, sizeof dlg.text, "Line number:");
        if (location.pos == doc.cursor)
            snprintf(now, sizeof now, "%" PRIu64, location.line);
        else
            now[0] = 0;
    } else {
        snprintf(dlg.text, sizeof dlg.text, "Byte offset, from 0 to %jd:", (intmax_t)doc.root.size);
        snprintf(now, sizeof now, "%jd", (intmax_t)doc.cursor);
    }
    dlg.has_field = 1;
    dlg.focus = -1;
    field_set(&dlg.field, now, 24);
    dlg.field.all = dlg.field.len > 0;
    dialog_button("Cancel", ANS_CANCEL);
    dialog_button("Go", ANS_OK);
    dlg.def = 1;
    dialog_show();
}

static void help_dialog(void)
{
    dialog_begin(D_HELP, "Keyboard Shortcuts");
    dialog_button("Close", ANS_OK);
    dialog_show();
}

static void about_dialog(void)
{
    dialog_begin(D_ABOUT, "About");
    snprintf(dlg.text, sizeof dlg.text,
             "A paged text editor for X11.\n"
             "It opens files larger than memory, reading 4 KiB pages as they are "
             "shown. Edits wait in a scratch file until you save.\n"
             "Part of the rp2350-native-linux extras. MIT License.");
    dialog_button("OK", ANS_OK);
    dialog_show();
}

/* ---- the file dialog ---- */

/* dir/name into out; 0, or -1 if it would not fit. */
static int join(char *out, size_t cap, const char *dir, const char *name)
{
    int n = snprintf(out, cap, "%s/%s", strcmp(dir, "/") ? dir : "", name);

    if (n < 0 || (size_t)n >= cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static void fb_free(void)
{
    free(fb.e);
    free(fb.pool);
    fb.e = NULL;
    fb.pool = NULL;
    fb.n = 0;
    fb.used = fb.cap = 0;
}

static int entry_cmp(const void *pa, const void *pb)
{
    const struct entry *a = pa, *b = pb;
    const char *x = fb.pool + a->name, *y = fb.pool + b->name;
    int c;

    if (!strcmp(x, ".."))
        return -1;
    if (!strcmp(y, ".."))
        return 1;
    if (a->dir != b->dir)
        return b->dir - a->dir;
    c = strcasecmp(x, y);
    return c ? c : strcmp(x, y);
}

static int fb_load(const char *where)
{
    char *real = realpath(where, NULL), full[PATH_MAX];
    struct dirent *de;
    DIR *d;
    int n = 0;

    if (!real)
        return -1;
    d = opendir(real);
    if (!d) {
        int error = errno;

        free(real);
        errno = error;
        return -1;
    }
    fb_free();
    snprintf(fb.dir, sizeof fb.dir, "%s", real);
    free(real);
    while ((de = readdir(d)) != NULL && fb.n < MAX_ENTRIES) {
        const char *name = de->d_name;
        size_t len = strlen(name) + 1;
        struct stat st;
        struct entry en = {0};

        if (!strcmp(name, "."))
            continue;
        if (!strcmp(name, "..") ? !strcmp(fb.dir, "/") : name[0] == '.' && !fb.hidden)
            continue;
        if (fb.n == n) {
            struct entry *e = realloc(fb.e, (size_t)(n = n ? 2 * n : 64) * sizeof *e);

            if (!e)
                break;
            fb.e = e;
        }
        if (fb.used + len > fb.cap) {
            size_t cap = fb.cap ? 2 * fb.cap : 4096;
            char *p;

            while (cap < fb.used + len)
                cap *= 2;
            if (!(p = realloc(fb.pool, cap)))
                break;
            fb.pool = p;
            fb.cap = cap;
        }
        if (join(full, sizeof full, fb.dir, name) == 0 && stat(full, &st) == 0) {
            en.dir = S_ISDIR(st.st_mode);
            en.size = st.st_size;
        }
        en.name = fb.used;
        memcpy(fb.pool + fb.used, name, len);
        fb.used += len;
        fb.e[fb.n++] = en;
    }
    closedir(d);
    if (fb.n)
        qsort(fb.e, (size_t)fb.n, sizeof *fb.e, entry_cmp);
    fb.sel = -1;
    fb.scroll = 0;
    fb.gen++;
    return 0;
}

static void fb_touch(void)
{
    fb.list_active = 0;
    dlg.error[0] = 0;
    if (fb.replace_ok) {
        fb.replace_ok = 0;
        dlg.btn[1].label = "Save";
    }
}

static void fb_visible(void)
{
    if (fb.sel >= 0 && fb.sel < fb.scroll)
        fb.scroll = fb.sel;
    if (fb.sel >= fb.scroll + fb.rows)
        fb.scroll = fb.sel - fb.rows + 1;
    fb.scroll = (int)max(0, min(fb.scroll, fb.n - fb.rows));
}

static void fb_select(int i)
{
    if (!fb.n)
        return;
    fb.sel = (int)max(0, min(fb.n - 1, i));
    fb_touch();
    fb.list_active = 1;
    if (!fb.e[fb.sel].dir)
        field_set(&dlg.field, fb.pool + fb.e[fb.sel].name, PATH_MAX);
    fb_visible();
}

static void fb_go(const char *target, const char *child)
{
    if (fb_load(target) < 0) {
        snprintf(dlg.error, sizeof dlg.error, "Can't open that folder: %s", strerror(errno));
        dlg.warn = 0;
        return;
    }
    fb_touch();
    if (dlg.purpose == A_OPEN)
        field_set(&dlg.field, "", PATH_MAX);
    for (int i = 0; child && i < fb.n; i++)
        if (!strcmp(fb.pool + fb.e[i].name, child)) {
            fb.sel = i;
            fb_visible();
        }
}

static void fb_enter(const char *name)
{
    char target[PATH_MAX], child[NAME_MAX + 1] = "";

    if (!strcmp(name, "..")) {
        const char *slash = strrchr(fb.dir, '/');

        if (!strcmp(fb.dir, "/"))
            return;
        snprintf(child, sizeof child, "%s", slash ? slash + 1 : "");
    }
    if (join(target, sizeof target, fb.dir, name) < 0) {
        snprintf(dlg.error, sizeof dlg.error, "That path is too long");
        dlg.warn = 0;
        return;
    }
    fb_go(target, child[0] ? child : NULL);
}

static void file_dialog(enum act purpose)
{
    char start[PATH_MAX];
    const char *path = doc_path();

    if (ov == OV_MENU)
        menu_hide();
    dialog_begin(D_FILE, purpose == A_OPEN ? "Open File" : "Save As");
    dlg.purpose = purpose;
    dlg.has_field = 1;
    dlg.focus = -1;
    dialog_button("Cancel", ANS_CANCEL);
    dialog_button(purpose == A_OPEN ? "Open" : "Save", ANS_OK);
    dlg.def = 1;
    field_set(&dlg.field, "", PATH_MAX);
    if (path) {
        char *slash;

        snprintf(start, sizeof start, "%s", path);
        slash = strrchr(start, '/');
        if (slash)
            *(slash == start ? slash + 1 : slash) = 0;
        else
            snprintf(start, sizeof start, ".");
    } else if (!getcwd(start, sizeof start)) {
        snprintf(start, sizeof start, "/");
    }
    if (fb_load(start) < 0 && fb_load(".") < 0 && fb_load("/") < 0) {
        fb.dir[0] = 0;
        snprintf(dlg.error, sizeof dlg.error, "Can't read the folder: %s", strerror(errno));
    }
    if (purpose == A_SAVE_AS) {
        field_set(&dlg.field, path ? doc_name() : "untitled.txt", PATH_MAX);
        dlg.field.all = 1;
        for (int i = 0; i < fb.n; i++)
            if (!strcmp(fb.pool + fb.e[i].name, dlg.field.s))
                fb.sel = i;
    }
    fb.click_row = -1;
    fb.list_active = fb.replace_ok = 0;
    dialog_show();
    fb_visible();
}

static void dialog_close(void)
{
    int file = dlg.kind == D_FILE;

    overlay_close();
    if (file)
        fb_free();
}

static void run_pending(void)
{
    enum act a = pending;

    pending = A_NONE;
    if (a == A_NEW)
        new_doc();
    else if (a == A_OPEN)
        file_dialog(A_OPEN);
    else if (a == A_QUIT)
        running = 0;
}

static void fb_accept(void)
{
    char target[PATH_MAX];
    const char *s = dlg.field.s, *home = getenv("HOME");
    struct stat st;
    int rc = 0;

    if (fb.list_active && fb.sel >= 0 && fb.e[fb.sel].dir) {
        fb_enter(fb.pool + fb.e[fb.sel].name);
        return;
    }
    if (!dlg.field.len) {
        snprintf(dlg.error, sizeof dlg.error, "Choose a file, or type its name");
        dlg.warn = 1;
        return;
    }
    if (s[0] == '/')
        snprintf(target, sizeof target, "%s", s);
    else if (s[0] == '~' && (s[1] == '/' || !s[1]) && home && *home)
        rc = join(target, sizeof target, home, s[1] ? s + 2 : "");
    else
        rc = join(target, sizeof target, fb.dir, s);
    if (rc < 0) {
        snprintf(dlg.error, sizeof dlg.error, "That path is too long");
        dlg.warn = 0;
        return;
    }
    if (stat(target, &st) == 0 && S_ISDIR(st.st_mode)) {
        fb_go(target, NULL);
        field_set(&dlg.field, "", PATH_MAX);
        return;
    }
    dlg.warn = 0;
    if (dlg.purpose == A_OPEN) {
        if (open_file(target) < 0) {
            snprintf(dlg.error, sizeof dlg.error, "Can't open \"%.120s\": %s", s, why(errno));
            return;
        }
        dialog_close();
        return;
    }
    if (!fb.replace_ok && lstat(target, &st) == 0) {
        fb.replace_ok = 1;
        dlg.btn[1].label = "Replace";
        dlg.warn = 1;
        snprintf(dlg.error, sizeof dlg.error, "\"%.120s\" already exists. Replace it?", s);
        return;
    }
    snprintf(message, sizeof message, "Saving...");
    if (doc_save_as(doc.root, target, fb.replace_ok) < 0) {
        message[0] = 0;
        snprintf(dlg.error, sizeof dlg.error, "Can't save \"%.120s\": %s", s,
                 errno == EBUSY ? "it changed on disk" : why(errno));
        return;
    }
    saved = doc;
    refresh = 1;
    snprintf(message, sizeof message, "Saved");
    dialog_close();
    run_pending();
}

static void goto_accept(void)
{
    char *end;
    intmax_t value;

    errno = 0;
    value = strtoimax(dlg.field.s, &end, 10);
    if (!dlg.field.len || errno || *end || value < (dlg.purpose == A_GOTO_LINE)) {
        snprintf(dlg.error, sizeof dlg.error, "Enter a %s",
                 dlg.purpose == A_GOTO_LINE ? "line number, from 1" : "byte offset, from 0");
        return;
    }
    dialog_close();
    if (dlg.purpose == A_GOTO_LINE)
        go_line(value);
    else
        move_to((off_t)min(value, doc.root.size), 0);
}

static void dialog_finish(int result)
{
    int kind = dlg.kind;

    if (kind == D_FILE && result == ANS_OK) {
        fb_accept();
        return;
    }
    if (kind == D_GOTO && result == ANS_OK) {
        goto_accept();
        return;
    }
    dialog_close();
    if (kind == D_FILE) {
        pending = A_NONE;
    } else if (kind == D_ASK_SAVE) {
        if (result == ANS_CANCEL)
            pending = A_NONE;
        else if (result == ANS_DISCARD)
            run_pending();
        else if (!doc_path())
            file_dialog(A_SAVE_AS);
        else if (save())
            run_pending();
        else
            pending = A_NONE;
    }
}

static int file_key(int k, int mods)
{
    if ((mods & X_MOD_ALT) && k == X_KEY_UP) {
        fb_enter("..");
        return 1;
    }
    switch (k) {
    case X_KEY_UP: fb_select(fb.sel <= 0 ? 0 : fb.sel - 1); return 1;
    case X_KEY_DOWN: fb_select(fb.sel < 0 ? 0 : fb.sel + 1); return 1;
    case X_KEY_PAGE_UP: fb_select(fb.sel - fb.rows); return 1;
    case X_KEY_PAGE_DOWN: fb_select(fb.sel < 0 ? fb.rows - 1 : fb.sel + fb.rows); return 1;
    case X_KEY_BACKSPACE:
        if (dlg.focus == -1 && !dlg.field.len) {
            fb_enter("..");
            return 1;
        }
        return 0;
    }
    return 0;
}

static void dialog_key(int k, int mods)
{
    int order[4], n = 0, at = 0;

    if (k == X_KEY_ESCAPE) {
        dialog_finish(dlg.btn[dlg.cancel].result);
        return;
    }
    if (dlg.has_field)
        order[n++] = -1;
    for (int i = 0; i < dlg.nbtn; i++)
        order[n++] = i;
    for (int i = 0; i < n; i++)
        if (order[i] == dlg.focus)
            at = i;
    if (k == X_KEY_TAB || k == X_KEY_BACKTAB) {
        dlg.focus = order[(at + (k == X_KEY_TAB && !(mods & X_MOD_SHIFT) ? 1 : n - 1)) % n];
        return;
    }
    if (k == X_KEY_RETURN || k == X_KEY_KP_ENTER || (k == ' ' && dlg.focus >= 0)) {
        dialog_finish(dlg.btn[dlg.focus >= 0 ? dlg.focus : dlg.def].result);
        return;
    }
    if (dlg.focus >= 0 && (k == X_KEY_LEFT || k == X_KEY_RIGHT)) {
        int i = dlg.focus + (k == X_KEY_LEFT ? -1 : 1);

        if (i >= 0 && i < dlg.nbtn)
            dlg.focus = i;
        return;
    }
    if (dlg.kind == D_FILE && file_key(k, mods))
        return;
    if (dlg.has_field) {
        int edits = dlg.field.edits;

        if (field_key(&dlg.field, k, mods)) {
            dlg.focus = -1;
            if (dlg.field.edits != edits) {
                if (dlg.kind == D_FILE)
                    fb_touch();
                else
                    dlg.error[0] = 0;
            }
        }
        return;
    }
    if (k > 32 && k < 127 && !(mods & (X_MOD_CTRL | X_MOD_ALT)))
        for (int i = 0; i < dlg.nbtn; i++)
            if (tolower((unsigned char)dlg.btn[i].label[0]) == tolower(k)) {
                dialog_finish(dlg.btn[i].result);
                return;
            }
}

static int dialog_hit(int x, int y)
{
    for (int i = 0; i < dlg.nbtn; i++)
        if (in_box(dlg.btn[i].b, x, y))
            return i;
    if (dlg.kind != D_FILE)
        return -1;
    if (in_box(fb.check, x, y))
        return 200;
    if (in_box(fb.up, x, y) && strcmp(fb.dir, "/"))
        return 201;
    if (in_box((box){fb.list.x, fb.list.y + 4, fb.list.w, fb.rows * fb.row_h}, x, y)) {
        int i = fb.scroll + (y - fb.list.y - 4) / fb.row_h;

        return i < fb.n ? 100 + i : -1;
    }
    return -1;
}

static void dialog_pointer(const struct x_event *e)
{
    int hit = dialog_hit(e->x, e->y);

    if (e->type == XE_MOTION) {
        dlg.hover = hit;
        x_cursor(dlg.has_field && in_box(dlg.field_b, e->x, e->y) ? X_CURSOR_TEXT
                                                                   : X_CURSOR_DEFAULT);
        return;
    }
    if (e->type != XE_BUTTON_DOWN)
        return;
    if ((e->button == 4 || e->button == 5) && dlg.kind == D_FILE &&
        in_box(fb.list, e->x, e->y)) {
        fb.scroll = (int)max(0, min(fb.n - fb.rows, fb.scroll + (e->button == 4 ? -3 : 3)));
        dlg.hover = dialog_hit(e->x, e->y);
        return;
    }
    if (e->button != 1)
        return;
    if (hit >= 0 && hit < 100) {
        dialog_finish(dlg.btn[hit].result);
    } else if (dlg.has_field && in_box(dlg.field_b, e->x, e->y)) {
        dlg.focus = -1;
        field_click(&dlg.field, dlg.field_b, e->x);
    } else if (hit == 200) {
        fb.hidden = !fb.hidden;
        fb_go(fb.dir, NULL);
    } else if (hit == 201) {
        fb_enter("..");
    } else if (hit >= 100) {
        int i = hit - 100;
        long long now = now_ms();

        if (i == fb.click_row && now - fb.click_ms < DCLICK_MS) {
            fb.click_row = -1;
            if (fb.e[i].dir)
                fb_enter(fb.pool + fb.e[i].name);
            else
                fb_accept();
            return;
        }
        fb.click_row = i;
        fb.click_ms = now;
        fb_select(i);
    }
}

/* ====================================================================
 * search and the find bar
 * ==================================================================== */

static int fold(int c) { return fbar.match_case ? c : tolower(c); }

/* Chunked KMP keeps matches crossing a page boundary and only visits
 * bytes up to the first match. A failed search can scan the whole file. */
static int find_range(const char *pat, size_t len, off_t start, off_t limit, off_t *match)
{
    unsigned char buf[DOC_PAGE];
    size_t prefix[SEARCH_MAX], matched = 0;

    prefix[0] = 0;
    for (size_t i = 1, j = 0; i < len; i++) {
        while (j && fold((unsigned char)pat[i]) != fold((unsigned char)pat[j]))
            j = prefix[j - 1];
        if (fold((unsigned char)pat[i]) == fold((unsigned char)pat[j]))
            j++;
        prefix[i] = j;
    }
    while (start < limit) {
        size_t n = (size_t)min(limit - start, sizeof buf);

        if (!bytes(start, buf, n))
            return -1;
        for (size_t i = 0; i < n; i++) {
            int c = fold(buf[i]);

            while (matched && c != fold((unsigned char)pat[matched]))
                matched = prefix[matched - 1];
            if (c == fold((unsigned char)pat[matched]))
                matched++;
            if (matched == len) {
                *match = start + (off_t)i - (off_t)(len - 1);
                return 1;
            }
        }
        start += (off_t)n;
    }
    return 0;
}

/* The last match in [from, limit): the same search run backwards from
 * limit with the pattern reversed, so it reads only back to that match. */
static int find_last(const char *pat, size_t len, off_t from, off_t limit, off_t *match)
{
    unsigned char buf[DOC_PAGE];
    size_t prefix[SEARCH_MAX], matched = 0;
#define RPAT(i) fold((unsigned char)pat[len - 1 - (i)])

    prefix[0] = 0;
    for (size_t i = 1, j = 0; i < len; i++) {
        while (j && RPAT(i) != RPAT(j))
            j = prefix[j - 1];
        if (RPAT(i) == RPAT(j))
            j++;
        prefix[i] = j;
    }
    while (limit > from) {
        size_t n = (size_t)min(limit - from, sizeof buf);

        if (!bytes(limit - (off_t)n, buf, n))
            return -1;
        for (size_t i = n; i-- > 0;) {
            int c = fold(buf[i]);

            while (matched && c != RPAT(matched))
                matched = prefix[matched - 1];
            if (c == RPAT(matched))
                matched++;
            if (matched == len) {
                *match = limit - (off_t)n + (off_t)i;
                return 1;
            }
        }
        limit -= (off_t)n;
    }
    return 0;
#undef RPAT
}

static void fbar_open(int with_replace)
{
    off_t a = min(doc.cursor, doc.anchor), b = max(doc.cursor, doc.anchor);
    char sel[SEARCH_MAX];

    if (!fbar.find.cap)
        field_set(&fbar.find, "", SEARCH_MAX);
    if (!fbar.repl.cap)
        field_set(&fbar.repl, "", SEARCH_MAX);
    if (b > a && b - a < SEARCH_MAX && bytes(a, sel, (size_t)(b - a))) {
        sel[b - a] = 0;
        if (!memchr(sel, '\n', (size_t)(b - a)) && strlen(sel) == (size_t)(b - a))
            field_set(&fbar.find, sel, SEARCH_MAX);
    }
    fbar.find.all = fbar.find.len > 0;
    fbar.focus = 1;
    if (!fbar.open || fbar.replace != with_replace) {
        fbar.open = 1;
        fbar.replace = with_replace;
        resize();
    }
}

static void fbar_close(void)
{
    fbar.open = fbar.focus = fbar.hover = 0;
    resize();
}

static void find_dir(int dir)
{
    const char *pat = fbar.find.s;
    size_t len = strlen(pat);
    off_t m, a = min(doc.cursor, doc.anchor), b = max(doc.cursor, doc.anchor);
    int r, wrapped = 0;

    if (!len) {
        fbar_open(fbar.open && fbar.replace);
        return;
    }
    snprintf(message, sizeof message, "Searching...");
    paint();
    message[0] = 0;
    if (dir > 0) {
        r = find_range(pat, len, b, doc.root.size, &m);
        if (r == 0 && b) {
            r = find_range(pat, len, 0, min(doc.root.size, b + (off_t)len - 1), &m);
            wrapped = 1;
        }
    } else {
        r = find_last(pat, len, 0, a, &m);
        if (r == 0) {
            r = find_last(pat, len, a, doc.root.size, &m);
            wrapped = 1;
        }
    }
    if (r > 0) {
        doc.anchor = m + (off_t)len;
        move_to(m, 1);
        if (wrapped)
            snprintf(message, sizeof message, "Search wrapped to the %s",
                     dir > 0 ? "start" : "end");
    } else if (r == 0) {
        snprintf(message, sizeof message, "No matches for \"%.120s\"", pat);
    }
}

static int selection_matches(void)
{
    off_t a = min(doc.cursor, doc.anchor), b = max(doc.cursor, doc.anchor);
    size_t len = strlen(fbar.find.s);
    char buf[SEARCH_MAX];

    if (!len || b - a != (off_t)len || !bytes(a, buf, len))
        return 0;
    for (size_t i = 0; i < len; i++)
        if (fold((unsigned char)buf[i]) != fold((unsigned char)fbar.find.s[i]))
            return 0;
    return 1;
}

static void replace_one(void)
{
    if (selection_matches())
        type_text(fbar.repl.s, strlen(fbar.repl.s));
    find_dir(1);
}

static void replace_all(void)
{
    const char *pat = fbar.find.s, *with = fbar.repl.s;
    size_t len = strlen(pat), wlen = strlen(with);
    struct snapshot before = doc;
    off_t pos = 0, m, first = -1;
    long count = 0;

    if (!len)
        return;
    snprintf(message, sizeof message, "Replacing...");
    paint();
    message[0] = 0;
    while (find_range(pat, len, pos, doc.root.size, &m) > 0) {
        if (doc_edit(&doc.root, m, (off_t)len, with, wlen) < 0) {
            report("Replace failed");
            break;
        }
        if (first < 0)
            first = m;
        pos = m + (off_t)wlen;
        count++;
    }
    if (count) {
        push(undo, &nundo, before);
        nredo = 0;
        doc.changed = first;
        invalidate_position(first);
        doc.cursor = doc.anchor = min(doc.cursor, doc.root.size);
        top = min(top, doc.root.size);
        layout();
        reveal();
    }
    if (message[0])
        return;
    if (count)
        snprintf(message, sizeof message, "Replaced %ld match%s", count, count == 1 ? "" : "es");
    else
        snprintf(message, sizeof message, "No matches for \"%.120s\"", pat);
}

/* ====================================================================
 * painting the editor
 * ==================================================================== */

static void fbar_layout(void)
{
    int lw = (int)max(x_text_width(f_ui, "Find"), x_text_width(f_ui, "Replace")) + 16;
    int x0 = 12, right, fw, x;

    fbar.h = fbar.replace ? 74 : 40;
    fbar.top = status_top - fbar.h;
    right = button_w("Previous") + 6 + button_w("Next") + 16 + check_w("Match case") + 16 + 24;
    fw = (int)max(100, x_width - x0 - lw - 8 - right - 12);
    fbar.f1 = (box){x0 + lw, fbar.top + 6, fw, 28};
    x = fbar.f1.x + fw + 8;
    fbar.prev = (box){x, fbar.top + 6, button_w("Previous"), 28};
    x += fbar.prev.w + 6;
    fbar.next = (box){x, fbar.top + 6, button_w("Next"), 28};
    x += fbar.next.w + 16;
    fbar.check = (box){x, fbar.top + 6, check_w("Match case") + 4, 28};
    fbar.close = (box){x_width - 12 - 24, fbar.top + 8, 24, 24};
    fbar.f2 = (box){x0 + lw, fbar.top + 40, fw, 28};
    x = fbar.f2.x + fw + 8;
    fbar.rep = (box){x, fbar.top + 40, button_w("Replace"), 28};
    fbar.all = (box){x + fbar.rep.w + 6, fbar.top + 40, button_w("Replace All"), 28};
}

static int fbar_hit(int x, int y)
{
    const box *b[] = {&fbar.prev, &fbar.next, &fbar.check, &fbar.close, &fbar.rep, &fbar.all,
                      &fbar.f1, &fbar.f2};

    for (int i = 0; i < N(b); i++)
        if ((fbar.replace || (i != 4 && i != 5 && i != 7)) && in_box(*b[i], x, y))
            return i + 1;
    return 0;
}

static int paint_fbar(void)
{
    int changed = 0, on = fbar.find.len > 0;

    if (differs(&bkey[K_FB_BG], "%d|%d|%d", fbar.replace, x_width, fbar.top)) {
        changed = 1;
        x_color(c_panel);
        x_fill_rect(0, fbar.top, x_width, fbar.h);
        x_color(c_grid);
        x_fill_rect(0, fbar.top, x_width, 1);
        text_at(f_ui, 12, fbar.f1.y + (28 - ui_h) / 2, 80, 0, TRIM_END, c_dim, "Find");
        if (fbar.replace)
            text_at(f_ui, 12, fbar.f2.y + (28 - ui_h) / 2, 80, 0, TRIM_END, c_dim, "Replace");
    }
    if (differs(&bkey[K_FB_F1], "%d|%d|%d|%s", fbar.focus == 1, fbar.find.pos, fbar.find.all,
                fbar.find.s)) {
        changed = 1;
        draw_field(fbar.f1, &fbar.find, fbar.focus == 1, c_panel);
    }
    if (differs(&bkey[K_FB_PREV], "%d|%d", fbar.hover == 1, on)) {
        changed = 1;
        draw_button(fbar.prev, "Previous", 0, fbar.hover == 1, 0, on, c_panel);
    }
    if (differs(&bkey[K_FB_NEXT], "%d|%d", fbar.hover == 2, on)) {
        changed = 1;
        draw_button(fbar.next, "Next", 1, fbar.hover == 2, 0, on, c_panel);
    }
    if (differs(&bkey[K_FB_CHECK], "%d|%d", fbar.hover == 3, fbar.match_case)) {
        changed = 1;
        draw_check(fbar.check, "Match case", fbar.match_case, fbar.hover == 3, c_panel);
    }
    if (differs(&bkey[K_FB_CLOSE], "%d", fbar.hover == 4)) {
        changed = 1;
        draw_close(fbar.close, fbar.hover == 4, c_panel);
    }
    if (!fbar.replace)
        return changed;
    if (differs(&bkey[K_FB_F2], "%d|%d|%d|%s", fbar.focus == 2, fbar.repl.pos, fbar.repl.all,
                fbar.repl.s)) {
        changed = 1;
        draw_field(fbar.f2, &fbar.repl, fbar.focus == 2, c_panel);
    }
    if (differs(&bkey[K_FB_REP], "%d|%d", fbar.hover == 5, on)) {
        changed = 1;
        draw_button(fbar.rep, "Replace", 0, fbar.hover == 5, 0, on, c_panel);
    }
    if (differs(&bkey[K_FB_ALL], "%d|%d", fbar.hover == 6, on)) {
        changed = 1;
        draw_button(fbar.all, "Replace All", 0, fbar.hover == 6, 0, on, c_panel);
    }
    return changed;
}

static int paint_menubar(void)
{
    int open = ov == OV_MENU ? menu_open : -1;

    if (!differs(&bkey[K_MENUBAR], "%d|%d|%d", open, bar_hover, x_width))
        return 0;
    x_color(c_panel);
    x_fill_rect(0, 0, x_width, menu_h);
    x_color(c_grid);
    x_fill_rect(0, menu_h - 1, x_width, 1);
    for (int i = 0; i < N(menus); i++) {
        if (i == open || (open < 0 && i == bar_hover)) {
            x_color(i == open ? c_sel : c_sel_dim);
            round_rect(menus[i].x, 4, menus[i].w, menu_h - 9, 5);
        }
        text_at(f_ui, menus[i].x, (menu_h - 1 - ui_h) / 2, menus[i].w, 2, TRIM_END, c_text,
                menus[i].title);
    }
    return 1;
}

static int paint_rows(void)
{
    off_t first = min(doc.cursor, doc.anchor), last = max(doc.cursor, doc.anchor);
    int current = cursor_row(), changed = 0, focus = !(fbar.open && fbar.focus);
    int lead = (ch - ascent - x_font_descent(font)) / 2;

    for (int i = 0; i < rows; i++) {
        const struct row *r = &view[i];
        struct line_image line = {.caret = -1, .focus = focus};
        int y = text_top + VPAD + i * ch;

        if (r->valid) {
            int extent = r->len + r->newline;

            memcpy(line.text, r->text, (size_t)r->len);
            line.first = (int)max(0, min(extent, first - r->start));
            line.last = (int)max(0, min(extent, last - r->start));
            if (line.first == line.last)
                line.first = line.last = 0;
            if (i == current) {
                line.caret = (int)(doc.cursor - r->start);
                line.current = 1;
            }
        }
        if (!refresh && !memcmp(&drawn[i].first, &line.first, 5 * sizeof(int)) &&
            strcmp(drawn[i].text, line.text) == 0)
            continue;
        drawn[i] = line;
        changed = 1;
        x_color(line.current && first == last ? c_line : c_bg);
        x_fill_rect(0, y, text_right, ch);
        if (line.last > line.first) {
            x_color(c_sel);
            x_fill_rect(PAD + line.first * cw, y, (line.last - line.first) * cw, ch);
        }
        if (line.text[0]) {
            x_text_font(font);
            x_color(c_text);
            x_text(PAD, y + lead + ascent, line.text);
        }
        if (line.caret >= 0) {
            x_color(focus ? c_blue : c_dim);
            x_fill_rect(PAD + line.caret * cw, y + 1, 2, ch - 2);
        }
    }
    return changed;
}

static int sb_hover, sb_drag, sb_grab;

static void thumb(box *track, int *ty, int *th, off_t *span)
{
    off_t end = top, size = doc.root.size;

    for (int i = 0; i < rows && view[i].valid; i++)
        end = view[i].next;
    *track = (box){text_right, text_top, SCROLL_W, text_bottom - text_top};
    *span = end - top;
    *th = *ty = 0;
    if (size <= 0 || (top == 0 && end >= size))
        return;
    *th = (int)max(24, min(track->h, (off_t)track->h * *span / size));
    *ty = track->y + (int)((off_t)(track->h - *th) * min(top, max(1, size - *span)) /
                           max(1, size - *span));
}

static int paint_scroll(void)
{
    box t;
    int ty, th;
    off_t span;

    thumb(&t, &ty, &th, &span);
    if (!differs(&bkey[K_SCROLL], "%d|%d|%d|%d|%d", ty, th, t.x, t.h, sb_hover || sb_drag))
        return 0;
    x_color(c_bg);
    x_fill_rect(t.x, t.y, t.w, t.h);
    if (th) {
        x_color(sb_hover || sb_drag ? c_dim : c_track);
        round_rect(t.x + 3, ty + 2, t.w - 6, th - 4, 3);
    }
    return 1;
}

static int paint_status(void)
{
    char pos[64], size[32], zoomed[16], left[400];
    const char *seg[3];
    int x, ty = status_top + (status_h - ui_h) / 2, d = dirty();

    if (location.pos == doc.cursor)
        snprintf(pos, sizeof pos, "Ln %" PRIu64 ", Col %" PRIu64, location.line, location.col);
    else if (position_failed)
        snprintf(pos, sizeof pos, "Ln ?, Col ?");
    else
        snprintf(pos, sizeof pos, "Counting lines...");
    human(size, sizeof size, doc.root.size);
    snprintf(zoomed, sizeof zoomed, "%d%%", zoom_px[zoom] * 100 / 13);
    snprintf(left, sizeof left, "%s", message[0] ? message : doc_name());
    if (!differs(&bkey[K_STATUS], "%s|%d|%d|%s|%s|%s|%d", left, !!message[0], d, pos, size,
                 zoomed, x_width))
        return 0;
    x_color(c_panel);
    x_fill_rect(0, status_top, x_width, status_h);
    x_color(c_grid);
    x_fill_rect(0, status_top, x_width, 1);
    seg[0] = zoomed;
    seg[1] = size;
    seg[2] = pos;
    x = x_width - 12;
    for (int i = 0; i < 3; i++) {
        int w = x_text_width(f_ui, seg[i]);

        x -= w;
        text_at(f_ui, x, ty, w, 0, TRIM_END, i == 2 ? c_text : c_dim, seg[i]);
        x -= 12;
        x_color(c_grid);
        x_fill_rect(x, status_top + 6, 1, status_h - 11);
        x -= 12;
    }
    if (message[0]) {
        text_at(f_ui, 12, ty, x - 24, 0, TRIM_END, c_amber, left);
    } else {
        int w = (int)min(x - 24 - (d ? 90 : 0), x_text_width(f_ui, left));

        text_at(f_ui, 12, ty, w, 0, TRIM_END, c_text, left);
        if (d) {
            x_color(c_amber);
            x_fill_ellipse(12 + w + 10, status_top + status_h / 2 - 3, 6, 6);
            text_at(f_ui, 12 + w + 22, ty, 70, 0, TRIM_END, c_dim, "Modified");
        }
    }
    return 1;
}

static void paint_title(void)
{
    char t[PATH_MAX + 32];

    if (!differs(&bkey[K_TITLE], "%s|%d", doc_name(), dirty()))
        return;
    to_latin1(t, sizeof t, doc_name());
    snprintf(t + strlen(t), sizeof t - strlen(t), "%s - xedit", dirty() ? " *" : "");
    xi_change_property(xi_win, 39, 31, 8, (const unsigned char *)t, strlen(t)); /* WM_NAME */
    x_flush();
}

static int paint_base(void)
{
    int changed = 0;

    if (refresh) {
        memset(bkey, 0, sizeof bkey);
        x_color(c_bg);
        x_fill_rect(0, 0, x_width, x_height);
        changed = 1;
    }
    changed |= paint_menubar();
    changed |= paint_rows();
    changed |= paint_scroll();
    if (fbar.open)
        changed |= paint_fbar();
    changed |= paint_status();
    paint_title();
    return changed;
}

static void paint(void)
{
    int changed = 0;

    if (!ov || refresh || ov_frame)
        changed = paint_base();
    if (ov) {
        if (refresh || ov_frame) {
            under_drop();
            memset(okey, 0, sizeof okey);
            if (ov == OV_MENU) {
                menu_layout();
                menu_frame();
            } else {
                dialog_layout();
                dialog_frame();
            }
            ov_frame = 0;
            changed = 1;
        }
        changed |= ov == OV_MENU ? menu_parts() : dialog_parts();
    }
    if (changed)
        x_present();
    refresh = 0;
}

static void resize(void)
{
    status_top = x_height - status_h;
    fbar_layout();
    text_top = menu_h;
    text_bottom = fbar.open ? fbar.top : status_top;
    text_right = x_width - SCROLL_W;
    cols = (int)max(1, min(COLS, (text_right - 2 * PAD) / cw));
    rows = (int)max(1, min(ROWS, (text_bottom - text_top - VPAD) / ch));
    refresh = 1;
    layout();
    reveal();
    if (ov == OV_MENU)
        menu_layout();
    else if (ov == OV_DIALOG)
        dialog_layout();
}

static void set_font(int level)
{
    static const char *const core[ZOOMS] = {"6x10", "6x10", "fixed", "8x13", "9x15", "10x20", "12x24"};
    static int cached[ZOOMS];

    if (!cached[level]) {
        int f = x_font_ttf_cell("DejaVuSansMono.ttf", zoom_px[level]);

        if (!f)
            f = x_font_cell(core[level]);
        cached[level] = f ? f : -1;
    }
    if (cached[level] < 0) {
        snprintf(message, sizeof message, "That font size is not available");
        return;
    }
    zoom = level;
    font = cached[level];
    cw = (int)max(1, x_text_width(font, "M"));
    ascent = x_font_ascent(font);
    ch = (int)max(1, ascent + x_font_descent(font) + 4);
    resize();
}

/* ====================================================================
 * commands and input
 * ==================================================================== */

static void action(enum act a)
{
    switch (a) {
    case A_NONE: break;
    case A_NEW: case A_OPEN: case A_QUIT:
        if (dirty()) {
            ask_save(a);
        } else {
            pending = a;
            run_pending();
        }
        break;
    case A_SAVE:
        if (doc_path())
            save();
        else
            file_dialog(A_SAVE_AS);
        break;
    case A_SAVE_AS: file_dialog(A_SAVE_AS); break;
    case A_UNDO: history_step(1); break;
    case A_REDO: history_step(0); break;
    case A_CUT: copy(1); break;
    case A_COPY: copy(0); break;
    case A_PASTE: x_request_paste(); break;
    case A_SELECT_ALL: doc.anchor = 0; move_to(doc.root.size, 1); break;
    case A_FIND: fbar_open(0); break;
    case A_REPLACE: fbar_open(1); break;
    case A_FIND_NEXT: find_dir(1); break;
    case A_FIND_PREV: find_dir(-1); break;
    case A_GOTO_LINE: case A_GOTO_BYTE: goto_dialog(a); break;
    case A_ZOOM_IN: if (zoom < ZOOMS - 1) set_font(zoom + 1); break;
    case A_ZOOM_OUT: if (zoom > 0) set_font(zoom - 1); break;
    case A_ZOOM_RESET: set_font(2); break;
    case A_HELP: help_dialog(); break;
    case A_ABOUT: about_dialog(); break;
    }
}

/* Keys that work anywhere outside menus and dialogs. */
static int command_key(int k, int mods)
{
    int ctrl = mods & X_MOD_CTRL, shift = mods & X_MOD_SHIFT;

    if ((mods & X_MOD_ALT) && !ctrl && k < 127) {
        for (int i = 0; i < N(menus); i++)
            if (tolower((unsigned char)menus[i].title[0]) == tolower(k)) {
                menu_show(i, 1);
                return 1;
            }
        return 0;
    }
    if (k == X_KEY_F1) {
        action(A_HELP);
        return 1;
    }
    if (k == X_KEY_F1 + 2) {
        action(shift ? A_FIND_PREV : A_FIND_NEXT);
        return 1;
    }
    if (k == X_KEY_F1 + 9) {
        menu_show(0, 1);
        return 1;
    }
    if (!ctrl)
        return 0;
    if (k >= 'A' && k <= 'Z')
        k += 'a' - 'A';
    switch (k) {
    case 'n': action(A_NEW); break;
    case 'o': action(A_OPEN); break;
    case 's': action(shift ? A_SAVE_AS : A_SAVE); break;
    case 'q': action(A_QUIT); break;
    case 'f': action(A_FIND); break;
    case 'h': action(A_REPLACE); break;
    case 'l': action(A_GOTO_LINE); break;
    case 'g': action(A_GOTO_BYTE); break;
    case 'z': action(shift ? A_REDO : A_UNDO); break;
    case 'y': action(A_REDO); break;
    case 'x': action(A_CUT); break;
    case 'c': action(A_COPY); break;
    case 'v': action(A_PASTE); break;
    case 'a': action(A_SELECT_ALL); break;
    case '+': case '=': action(A_ZOOM_IN); break;
    case '-': case '_': action(A_ZOOM_OUT); break;
    case '0': action(A_ZOOM_RESET); break;
    case X_KEY_HOME: move_to(0, shift); break;
    case X_KEY_END: move_to(doc.root.size, shift); break;
    }
    return 1;
}

static int fbar_key(int k, int mods)
{
    struct field *f = fbar.focus == 2 ? &fbar.repl : &fbar.find;

    switch (k) {
    case X_KEY_ESCAPE:
        fbar_close();
        return 1;
    case X_KEY_TAB: case X_KEY_BACKTAB:
        if (fbar.replace)
            fbar.focus = 3 - fbar.focus;
        return 1;
    case X_KEY_RETURN: case X_KEY_KP_ENTER:
        if (fbar.focus == 2)
            replace_one();
        else
            find_dir(mods & X_MOD_SHIFT ? -1 : 1);
        return 1;
    }
    return field_key(f, k, mods);
}

static void vertical(int step, int count, int extend)
{
    struct row row;
    int i;

    reveal();
    i = cursor_row();
    if (i < 0)
        return;
    row = view[i];
    if (preferred < 0)
        preferred = (int)(doc.cursor - row.start);
    while (count-- > 0) {
        off_t pos = step < 0 ? (i > 0 ? view[i - 1].start : previous(row.start))
                             : row.next;

        if (pos == row.start || (step > 0 && pos == doc.root.size && !row.newline))
            break;
        i += step;
        if (i < 0 || i >= rows || !view[i].valid) {
            row = row_at(pos);
            /* Further steps use the file once we move beyond the viewport. */
        } else {
            row = view[i];
        }
        if (!row.valid)
            break;
    }
    if (row.valid) {
        int end = row.len - (!row.newline && row.next < doc.root.size && row.len > 0);

        move_to(row.start + min(preferred, end), extend);
    }
}

static void editor_key(int k, int mods)
{
    int extend = mods & X_MOD_SHIFT;
    int i;
    char c;

    if (k != X_KEY_UP && k != X_KEY_DOWN && k != X_KEY_PAGE_UP && k != X_KEY_PAGE_DOWN)
        preferred = -1;
    switch (k) {
    case X_KEY_ESCAPE:
        if (fbar.open)
            fbar_close();
        return;
    case X_KEY_LEFT:
        move_to(!extend && doc.cursor != doc.anchor ? min(doc.cursor, doc.anchor)
                                                    : max(0, doc.cursor - 1), extend);
        return;
    case X_KEY_RIGHT:
        move_to(!extend && doc.cursor != doc.anchor ? max(doc.cursor, doc.anchor)
                                                    : doc.cursor + (doc.cursor < doc.root.size), extend);
        return;
    case X_KEY_UP: vertical(-1, 1, extend); return;
    case X_KEY_DOWN: vertical(1, 1, extend); return;
    case X_KEY_PAGE_UP: vertical(-1, (int)max(1, rows - 1), extend); return;
    case X_KEY_PAGE_DOWN: vertical(1, (int)max(1, rows - 1), extend); return;
    case X_KEY_HOME: case X_KEY_END:
        reveal();
        i = cursor_row();
        if (i >= 0) {
            const struct row *r = &view[i];
            /* a wrapped row's end is the next row's start: stop before it */
            int end = r->len - (!r->newline && r->next < doc.root.size && r->len > 0);

            move_to(r->start + (k == X_KEY_HOME ? 0 : end), extend);
        }
        return;
    case X_KEY_BACKSPACE: case X_KEY_DELETE:
        if (doc.cursor != doc.anchor)
            type_text(NULL, 0);
        else if (k == X_KEY_BACKSPACE && doc.cursor)
            replace(doc.cursor - 1, doc.cursor, NULL, 0);
        else if (k == X_KEY_DELETE && doc.cursor < doc.root.size)
            replace(doc.cursor, doc.cursor + 1, NULL, 0);
        return;
    case X_KEY_RETURN: case X_KEY_KP_ENTER: c = '\n'; break;
    case X_KEY_TAB: c = '\t'; break;
    default:
        if (k < 32 || k >= 127 || (mods & X_MOD_ALT))
            return;
        c = (char)k;
    }
    type_text(&c, 1);
}

static void keyboard(int k, int mods)
{
    if (ov == OV_MENU) {
        menu_key(k);
        return;
    }
    if (ov == OV_DIALOG) {
        dialog_key(k, mods);
        return;
    }
    message[0] = 0;
    if (fbar.open && fbar.focus && fbar_key(k, mods))
        return;
    if (command_key(k, mods) || (fbar.open && fbar.focus))
        return;
    editor_key(k, mods);
}

static off_t text_pos(int x, int y)
{
    int row = (int)max(0, min(rows - 1, (y - text_top - VPAD) / ch));

    if (!view[row].valid)
        return doc.root.size;
    return view[row].start + max(0, min(view[row].len, (x - PAD + cw / 2) / cw));
}

static void scroll_by(int n)
{
    for (int i = 0; i < (n < 0 ? -n : n); i++) {
        struct row r = row_at(top);

        if (n < 0)
            top = previous(top);
        else if (r.valid && (r.next < doc.root.size || r.newline))
            top = r.next;
    }
    layout();
}

static void select_word(off_t pos)
{
    for (int i = 0; i < rows && view[i].valid; i++) {
        const struct row *r = &view[i];
        int at, s, e;

        if (pos < r->start || pos > r->start + r->len)
            continue;
        at = (int)(pos - r->start);
        s = e = at;
        while (s > 0 && (isalnum((unsigned char)r->text[s - 1]) || r->text[s - 1] == '_'))
            s--;
        while (e < r->len && (isalnum((unsigned char)r->text[e]) || r->text[e] == '_'))
            e++;
        if (s < e) {
            doc.anchor = r->start + s;
            doc.cursor = r->start + e;
        }
        return;
    }
}

static void base_pointer(const struct x_event *e)
{
    static long long click_ms;
    static int click_x, click_y;
    int in_text = e->y >= text_top && e->y < text_bottom && e->x < text_right;
    int in_bar = fbar.open && e->y >= fbar.top && e->y < status_top;
    box t;
    int ty, th;
    off_t span;

    if (e->type == XE_MOTION && !dragging && !sb_drag) {
        int hit = in_bar ? fbar_hit(e->x, e->y) : 0;

        x_cursor(in_text || hit == 7 || hit == 8 ? X_CURSOR_TEXT : X_CURSOR_DEFAULT);
        bar_hover = title_at(e->x, e->y);
        fbar.hover = hit;
        sb_hover = e->y >= text_top && e->y < text_bottom && e->x >= text_right;
        return;
    }
    if (sb_drag) {
        thumb(&t, &ty, &th, &span);
        if (e->type == XE_BUTTON_UP && e->button == 1) {
            sb_drag = 0;
        } else if (e->type == XE_MOTION && t.h > th) {
            off_t room = max(1, doc.root.size - span);
            off_t pos = (off_t)max(0, min(t.h - th, e->y - sb_grab - t.y)) * room / (t.h - th);

            top = e->y - sb_grab - t.y >= t.h - th ? row_start(room) : row_start(pos);
            layout();
        }
        return;
    }
    if (dragging) {
        if (e->type == XE_BUTTON_UP && e->button == 1) {
            dragging = 0;
        } else if (e->type == XE_MOTION) {
            if (e->y < text_top + VPAD)
                scroll_by(-1);
            else if (e->y >= text_bottom)
                scroll_by(1);
            doc.cursor = text_pos(e->x, e->y);
        }
        return;
    }
    if (e->type != XE_BUTTON_DOWN)
        return;
    if (e->button == 1 && title_at(e->x, e->y) >= 0) {
        menu_show(title_at(e->x, e->y), 0);
        return;
    }
    if (in_bar) {
        if (e->button != 1)
            return;
        switch (fbar_hit(e->x, e->y)) {
        case 1: find_dir(-1); break;
        case 2: find_dir(1); break;
        case 3: fbar.match_case = !fbar.match_case; break;
        case 4: fbar_close(); break;
        case 5: replace_one(); break;
        case 6: replace_all(); break;
        case 7: fbar.focus = 1; field_click(&fbar.find, fbar.f1, e->x); break;
        case 8: fbar.focus = 2; field_click(&fbar.repl, fbar.f2, e->x); break;
        }
        return;
    }
    if (e->y < text_top || e->y >= text_bottom)
        return;
    if (e->button == 4 || e->button == 5) {
        scroll_by(e->button == 4 ? -3 : 3);
        return;
    }
    if (e->button != 1)
        return;
    message[0] = 0;
    if (e->x >= text_right) {
        thumb(&t, &ty, &th, &span);
        if (!th)
            return;
        if (e->y >= ty && e->y < ty + th) {
            sb_drag = 1;
            sb_grab = e->y - ty;
        } else {
            scroll_by(e->y < ty ? -(int)max(1, rows - 1) : (int)max(1, rows - 1));
        }
        return;
    }
    fbar.focus = 0;
    preferred = -1;
    {
        off_t pos = text_pos(e->x, e->y);
        long long now = now_ms();

        if (now - click_ms < DCLICK_MS && abs(e->x - click_x) < 4 && abs(e->y - click_y) < 4) {
            click_ms = 0;
            select_word(pos);
            return;
        }
        click_ms = now;
        click_x = e->x;
        click_y = e->y;
        if (!(e->mods & X_MOD_SHIFT))
            doc.anchor = pos;
        doc.cursor = pos;
        dragging = 1;
    }
}

static void pointer(const struct x_event *e)
{
    if (ov == OV_MENU)
        menu_pointer(e);
    else if (ov == OV_DIALOG)
        dialog_pointer(e);
    else
        base_pointer(e);
}

static void paste(const struct x_event *e)
{
    struct field *f = NULL;

    if (e->len <= 0)
        return;
    if (ov == OV_DIALOG && dlg.has_field)
        f = &dlg.field;
    else if (ov == OV_NONE && fbar.open && fbar.focus)
        f = fbar.focus == 2 ? &fbar.repl : &fbar.find;
    if (f) {
        field_paste(f, e->text, e->len);
        if (ov == OV_DIALOG) {
            dlg.focus = -1;
            if (dlg.kind == D_FILE)
                fb_touch();
        }
    } else if (ov == OV_NONE) {
        if (e->len <= CLIPBOARD)
            type_text(e->text, (size_t)e->len);
        else
            snprintf(message, sizeof message, "Clipboard limit is %d KiB", CLIPBOARD / 1024);
    }
}

int main(int argc, char **argv)
{
    struct x_event event;
    int rc;

    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("usage: xedit [--] [FILE]\n");
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--")) {
        argc--;
        argv++;
    } else if (argc > 1 && argv[1][0] == '-' && argv[1][1]) {
        fprintf(stderr, "usage: xedit [--] [FILE]\n");
        return 2;
    }
    if (argc > 2) {
        fprintf(stderr, "usage: xedit [--] [FILE]\n");
        return 2;
    }
    rc = argc == 2 ? doc_open(argv[1], &doc.root) : doc_new(&doc.root);
    if (rc < 0) {
        fprintf(stderr, "xedit: %s: %s\n", argv[1], why(errno));
        doc_close();
        return 1;
    }
    if (x_open(NULL) < 0) {
        fprintf(stderr, "xedit: cannot open an X11 window on DISPLAY\n");
        doc_close();
        return 1;
    }
    x_background(px(RGB_BG));
    if (!x_window("xedit", 820, 560)) {
        fprintf(stderr, "xedit: cannot open an X11 window on DISPLAY\n");
        doc_close();
        return 1;
    }
    saved = doc;
    x_min_size(520, 380);
    setup_style();
    menu_titles();
    status_top = x_height - status_h;
    set_font(zoom);
    paint();
    while (running && !xi_dead) {
        int batch = 64;

        while (running && batch-- && x_event(&event)) {
            switch (event.type) {
            case XE_KEY_DOWN: keyboard(event.key, event.mods); break;
            case XE_BUTTON_DOWN: case XE_BUTTON_UP: case XE_MOTION: pointer(&event); break;
            case XE_RESIZE: resize(); break;
            case XE_CLOSE:
                if (ov == OV_MENU)
                    menu_hide();
                if (ov == OV_NONE)
                    action(A_QUIT);
                break;
            case XE_PASTE: paste(&event); break;
            }
        }
        if (running) {
            int counting = count_position();

            paint();
            if (batch >= 0)
                x_wait(counting ? 0 : -1);
        }
    }
    x_close();
    doc_close();
    return 0;
}
