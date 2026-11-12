/* ui_cells.c - see ui_cells.h. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ui_cells.h"
#include "ui_key.h"

static void set_cell(ui_cell *p, int ch, int fg, int bg, int bold)
{
    p->ch = (unsigned char)ch;
    p->fg = (unsigned char)(fg < 0 ? 7 : fg);
    p->bg = (unsigned char)(bg < 0 ? 0 : bg);
    p->flags = (unsigned char)((bold ? UI_CELLS_BOLD : 0) | (bg < 0 ? UI_CELLS_BG_DEFAULT : 0));
}

int ui_cells_open(ui_cells *c, int cols, int rows)
{
    long n, i;
    memset(c, 0, sizeof *c);
    if (cols <= 0 || rows <= 0) return -1;
    n = (long)cols * rows;
    c->back = (ui_cell *)malloc((size_t)n * sizeof(ui_cell));
    c->front = (ui_cell *)malloc((size_t)n * sizeof(ui_cell));
    if (c->back == 0 || c->front == 0) { ui_cells_close(c); return -1; }
    c->cols = cols;
    c->rows = rows;
    for (i = 0; i < n; i++) { set_cell(&c->back[i], ' ', 7, -1, 0); c->front[i] = c->back[i]; }
    c->full = 1;
    c->fg = 7; c->bg = UI_CELLS_DEFAULT; c->bold = 0;
    ui_cells_forget(c);
    return 0;
}

void ui_cells_close(ui_cells *c)
{
    free(c->back);
    free(c->front);
    c->back = c->front = 0;
    c->cols = c->rows = 0;
}

void ui_cells_forget(ui_cells *c) { c->t_row = c->t_col = -1; c->t_have = 0; c->t_show = -1; }

void ui_cells_invalidate(ui_cells *c) { c->full = 1; ui_cells_forget(c); }

void ui_cells_wide_table(ui_cells *c, const char *const *glyphs, int n)
{
    c->wide = glyphs;
    c->nwide = glyphs == 0 || n < 0 ? 0 : n;
    c->full = 1;
}

void ui_cells_pen(ui_cells *c, int fg, int bg, int bold)
{
    c->fg = fg; c->bg = bg; c->bold = bold;
}

void ui_cells_put(ui_cells *c, int row, int col, int ch)
{
    if (row < 0 || col < 0 || row >= c->rows || col >= c->cols) return;
    set_cell(&c->back[(long)row * c->cols + col], ch, c->fg, c->bg, c->bold);
}

/* The row's first cell, or 0 when the row is off the canvas. The run writers below build the pen's
 * cell once and store it per column instead of calling ui_cells_put (and set_cell) per character:
 * the same cells, without a call, a row bounds check and three pen conditionals per cell. */
static ui_cell *row_at(ui_cells *c, int row)
{
    return row < 0 || row >= c->rows ? 0 : &c->back[(long)row * c->cols];
}

int ui_cells_text(ui_cells *c, int row, int col, const char *s, int max)
{
    int n = 0, k;
    ui_cell pen, *line = row_at(c, row);
    if (max < 0) max = c->cols - col;
    set_cell(&pen, ' ', c->fg, c->bg, c->bold);
    for (; s[n] != '\0' && n < max; n++) {
        k = col + n;
        if (line != 0 && k >= 0 && k < c->cols) { line[k] = pen; line[k].ch = (unsigned char)s[n]; }
    }
    return n;
}

void ui_cells_fill(ui_cells *c, int row, int col, int h, int w, int ch)
{
    int r0 = row < 0 ? 0 : row, r1 = row + h > c->rows ? c->rows : row + h;
    int k0 = col < 0 ? 0 : col, k1 = col + w > c->cols ? c->cols : col + w;
    int stride = c->cols, n = k1 - k0;
    ui_cell cell, *line;
    if (h <= 0 || w <= 0 || r0 >= r1 || k0 >= k1) return;
    set_cell(&cell, ch, c->fg, c->bg, c->bold);
    line = &c->back[(long)r0 * stride + k0];             /* one multiply; the next row is a stride away */
    for (; r0 < r1; r0++, line += stride) {
        ui_cell *p = line;
        int m = n;
        for (; m >= 8; m -= 8, p += 8) {                 /* eight stores per loop test and pointer step */
            p[0] = cell; p[1] = cell; p[2] = cell; p[3] = cell;
            p[4] = cell; p[5] = cell; p[6] = cell; p[7] = cell;
        }
        if (m & 4) { p[0] = cell; p[1] = cell; p[2] = cell; p[3] = cell; p += 4; }
        if (m & 2) { p[0] = cell; p[1] = cell; p += 2; }
        if (m & 1) *p = cell;
    }
}

void ui_cells_box(ui_cells *c, int top, int left, int h, int w, int border_fg, const char *label, int label_fg)
{
    int fg = c->fg, bold = c->bold, at;
    if (h < 2 || w < 2) return;
    c->fg = border_fg; c->bold = 0;
    /* The edges are runs: ui_cells_fill clips like ui_cells_put and builds the cell once. */
    ui_cells_put(c, top, left, '.');
    ui_cells_fill(c, top, left + 1, 1, w - 2, '-');
    ui_cells_put(c, top, left + w - 1, '.');
    ui_cells_fill(c, top + 1, left, h - 2, 1, '|');
    ui_cells_fill(c, top + 1, left + w - 1, h - 2, 1, '|');
    ui_cells_put(c, top + h - 1, left, '`');
    ui_cells_fill(c, top + h - 1, left + 1, 1, w - 2, '-');
    ui_cells_put(c, top + h - 1, left + w - 1, '\'');
    if (label != 0 && label[0] != '\0' && w > 6) {
        c->fg = label_fg;
        ui_cells_put(c, top, left + 2, ' ');
        at = 3 + ui_cells_text(c, top, left + 3, label, w - 6);
        ui_cells_put(c, top, left + at, ' ');
    }
    c->fg = fg; c->bold = bold;
}

void ui_cells_modal(ui_cells *c, int h, int w, int border_fg, const char *label, int label_fg, int *top, int *left)
{
    int t, l;
    if (w > c->cols) w = c->cols;
    if (h > c->rows) h = c->rows;
    l = (c->cols - w) / 2 - 1;
    if (l < 0) l = 0;
    t = (c->rows - h) / 2;
    ui_cells_fill(c, t + 1, l + 1, h - 2, w - 2, ' ');
    ui_cells_box(c, t, l, h, w, border_fg, label, label_fg);
    *top = t; *left = l;
}

int ui_cells_menu(ui_cells *c, int top, int left, const char *const *items, int n, int sel, int border_fg, int fg, int sel_fg)
{
    int i, wide = 0, w, pen = c->fg;
    for (i = 0; i < n; i++) { int l = (int)strlen(items[i]); if (l > wide) wide = l; }
    w = wide + 4;   /* "> " marker at left+1, text at left+3, one column before the border (the TS dropdown) */
    ui_cells_fill(c, top + 1, left + 1, n, w - 2, ' ');
    ui_cells_box(c, top, left, n + 2, w, border_fg, 0, 0);
    for (i = 0; i < n; i++) {
        c->fg = i == sel ? sel_fg : fg;
        ui_cells_text(c, top + 1 + i, left + 1, i == sel ? "> " : "  ", 2);
        ui_cells_text(c, top + 1 + i, left + 3, items[i], w - 4);
    }
    c->fg = pen;
    return w;
}

int ui_cells_marked_cols(const char *s)
{
    int n = 0;
    for (; *s != '\0'; s++) {
        if (*s == UI_CELLS_M_FG || *s == UI_CELLS_M_ATTR) { if (s[1] != '\0') s++; continue; }
        if (*s == UI_CELLS_M_BOLD || *s == UI_CELLS_M_PLAIN || *s == UI_CELLS_M_BASE) continue;
        if (*s == UI_CELLS_M_WIDE) { if (s[1] != '\0') { s++; n += 2; } continue; }
        n++;
    }
    return n;
}

int ui_cells_marked(ui_cells *c, int row, int col, const char *s, int from, int to, int max)
{
    int base_fg = c->fg, base_bold = c->bold, base_attr = c->attr, i, n = 0, k;
    ui_cell pen, *line = row_at(c, row);
    set_cell(&pen, ' ', c->fg, c->bg, c->bold);
    pen.flags |= (unsigned char)(c->attr << 4);
    for (i = 0; s[i] != '\0' && i < to; i++) {
        char ch = s[i];
        if ((unsigned char)ch > (unsigned char)UI_CELLS_M_ATTR && i >= from && n < max) {   /* a run of plain characters: the hot path */
            int room = line == 0 ? 0 : c->cols;
            k = col + n;
            do {
                if ((unsigned)k < (unsigned)room) { line[k] = pen; line[k].ch = (unsigned char)ch; }
                k++; n++; i++;
                ch = s[i];
            } while ((unsigned char)ch > (unsigned char)UI_CELLS_M_ATTR && i < to && n < max);
            i--;                                               /* the for step moves on to the character that ended the run */
            continue;
        }
        if (ch == UI_CELLS_M_WIDE) {                                  /* two columns: the glyph, then its tail */
            if (s[i + 1] == '\0') continue;
            if (i++ < from) continue;
            if (n + 2 > max) break;
            k = col + n;
            if (line != 0 && k >= 0 && k + 1 < c->cols) {
                line[k] = pen; line[k].ch = (unsigned char)s[i]; line[k].flags |= UI_CELLS_WIDE;
                line[k + 1] = pen; line[k + 1].flags |= UI_CELLS_WIDE_TAIL;
            }
            n += 2;
            continue;
        }
        if (ch == UI_CELLS_M_ATTR && s[i + 1] != '\0') {
            c->attr = ((unsigned char)s[++i] - '@') & 7;
            set_cell(&pen, ' ', c->fg, c->bg, c->bold); pen.flags |= (unsigned char)(c->attr << 4);
            continue;
        }
        if ((unsigned char)ch <= (unsigned char)UI_CELLS_M_BASE) {   /* a marker: 1..4 (is_marker) */
            if (ch == UI_CELLS_M_FG && s[i + 1] != '\0') { c->fg = (unsigned char)s[++i]; set_cell(&pen, ' ', c->fg, c->bg, c->bold); pen.flags |= (unsigned char)(c->attr << 4); continue; }
            if (ch == UI_CELLS_M_BOLD) { c->bold = 1; set_cell(&pen, ' ', c->fg, c->bg, c->bold); pen.flags |= (unsigned char)(c->attr << 4); continue; }
            if (ch == UI_CELLS_M_PLAIN) { c->bold = 0; set_cell(&pen, ' ', c->fg, c->bg, c->bold); pen.flags |= (unsigned char)(c->attr << 4); continue; }
            if (ch == UI_CELLS_M_BASE) { c->fg = base_fg; c->bold = base_bold; c->attr = base_attr; set_cell(&pen, ' ', c->fg, c->bg, c->bold); pen.flags |= (unsigned char)(c->attr << 4); continue; }
        }
        if (i < from) continue;
        if (n >= max) break;
        k = col + n;
        if (line != 0 && k >= 0 && k < c->cols) { line[k] = pen; line[k].ch = (unsigned char)ch; }
        n++;
    }
    c->fg = base_fg; c->bold = base_bold; c->attr = base_attr;
    return n;
}

/* The four markers are the bytes 1..4, so one range test finds them (ui_cells_marked relies on
 * the same fact). The typedef fails to compile if the header ever moves them. */
typedef char ui_cells_markers_are_1_to_4[(UI_CELLS_M_FG == 1 && UI_CELLS_M_BOLD == 2 &&
                                          UI_CELLS_M_PLAIN == 3 && UI_CELLS_M_BASE == 4 &&
                                          UI_CELLS_M_WIDE == 5 && UI_CELLS_M_ATTR == 6) ? 1 : -1];
static int is_marker(const char *s, int i)
{
    return ((unsigned char)s[i] >= (unsigned char)UI_CELLS_M_FG && (unsigned char)s[i] <= (unsigned char)UI_CELLS_M_BASE)
        || s[i] == UI_CELLS_M_ATTR;
}

int ui_cells_wrap(const char *s, int width, int *starts, int *ends, int max)
{
    int lines = 0, i = 0;
    if (width < 1) width = 1;
    while (s[i] != '\0' && lines < max) {
        int cols = 0, j = i, brk = -1, brk_next = -1;
        for (;;) {
            unsigned char ch = (unsigned char)s[j];
            if (ch > (unsigned char)' ') {                /* an ordinary printable: only the width can end the row */
                if (cols == width) break;
                cols++;
                j++;
                continue;
            }
            if (ch == '\0') break;
            if (is_marker(s, j)) { j += ((s[j] == UI_CELLS_M_FG || s[j] == UI_CELLS_M_ATTR) && s[j + 1] != '\0') ? 2 : 1; continue; }
            if (s[j] == UI_CELLS_M_WIDE && s[j + 1] != '\0') {   /* a two-column glyph: never split */
                if (cols + 2 > width && cols > 0) break;   /* alone on a too-narrow row it still goes */
                cols += 2;
                j += 2;
                continue;
            }
            if (s[j] == '\n') break;                  /* a hard break: the row ends here */
            if (s[j] == ' ') { brk = j; brk_next = j + 1; }
            if (cols == width) break;
            cols++;
            j++;
        }
        if (s[j] == '\0') { starts[lines] = i; ends[lines] = j; lines++; break; }
        if (s[j] == '\n') { starts[lines] = i; ends[lines] = j; lines++; i = j + 1; continue; }
        if (s[j] == ' ') { brk = j; brk_next = j + 1; }
        if (brk <= i) { brk = j; brk_next = j; }   /* one word wider than the line: cut it */
        starts[lines] = i; ends[lines] = brk; lines++;
        i = brk_next;
    }
    if (lines == 0 && max > 0) { starts[0] = 0; ends[0] = 0; lines = 1; }
    return lines;
}

/* ---- the wire ---------------------------------------------------------- */

static void out_byte(ansi_buf *b, char ch)
{
    if (b->len + 1 > b->cap) { b->overflow = 1; return; }
    b->data[b->len++] = ch;
}

static void out_str(ansi_buf *b, const char *s) { while (*s != '\0') out_byte(b, *s++); }

static void out_int(ansi_buf *b, int v)
{
    char tmp[8];
    int n = 0;
    if (v == 0) { out_byte(b, '0'); return; }
    while (v > 0 && n < 7) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) out_byte(b, tmp[--n]);
}

static void out_colour(ansi_buf *b, int base, int bright, int ext, int v)
{
    if (v < 8) out_int(b, base + v);
    else if (v < 16) out_int(b, bright + v - 8);
    else { out_int(b, ext); out_str(b, ";5;"); out_int(b, v); }
}

/* blessed's shape: ESC[0;[1;]<bg>;<fg>m */
static void out_sgr(ansi_buf *b, const ui_cell *p)
{
    out_str(b, "\033[0;");
    if (p->flags & UI_CELLS_BOLD) out_str(b, "1;");
    if (p->flags & UI_CELLS_ITALIC) out_str(b, "3;");
    if (p->flags & UI_CELLS_UNDERLINE) out_str(b, "4;");
    if (p->flags & UI_CELLS_REVERSE) out_str(b, "7;");
    if (p->flags & UI_CELLS_BG_DEFAULT) out_str(b, "49");
    else out_colour(b, 40, 100, 48, p->bg);
    out_byte(b, ';');
    out_colour(b, 30, 90, 38, p->fg);
    out_byte(b, 'm');
}

#define STYLE_FLAGS (UI_CELLS_BOLD | UI_CELLS_BG_DEFAULT | UI_CELLS_UNDERLINE | UI_CELLS_ITALIC | UI_CELLS_REVERSE)
#define ATTR_FLAGS (UI_CELLS_BOLD | UI_CELLS_UNDERLINE | UI_CELLS_ITALIC | UI_CELLS_REVERSE)
static int same_bg(const ui_cell *a, const ui_cell *b)
{
    if ((a->flags ^ b->flags) & UI_CELLS_BG_DEFAULT) return 0;
    return (a->flags & UI_CELLS_BG_DEFAULT) || a->bg == b->bg;
}
static int same_style(const ui_cell *a, const ui_cell *b)
{
    /* UI_CELLS_WIDE / _TAIL say what the cell holds, not how it is drawn. */
    return a->fg == b->fg && (a->flags & ATTR_FLAGS) == (b->flags & ATTR_FLAGS) && same_bg(a, b);
}
/* A blank shows its background and nothing else (no underline, no reverse): it can go out in whatever
 * foreground, bold and italic the terminal holds. A cleared row beside coloured text then costs no SGR. */
static int blank_fits(const ui_cell *pen, const ui_cell *p)
{
    return p->ch == ' '
        && !((pen->flags | p->flags) & (UI_CELLS_UNDERLINE | UI_CELLS_REVERSE | UI_CELLS_WIDE | UI_CELLS_WIDE_TAIL))
        && same_bg(pen, p);
}
/* From the style the terminal holds (`pen`, when `have`) to p's. Only what differs is sent
 * (ESC[32m, not ESC[0;49;32m) as long as nothing has to be switched OFF; an attribute that must go
 * takes the reset-led form, which every terminal understands (22/23/24/27 are not universal). */
static void out_sgr_to(ansi_buf *b, int have, const ui_cell *pen, const ui_cell *p)
{
    int add, sep = 0;
    if (!have || (pen->flags & ATTR_FLAGS & ~p->flags)) { out_sgr(b, p); return; }
    add = p->flags & ~pen->flags & ATTR_FLAGS;
    out_str(b, "\033[");
    if (add & UI_CELLS_BOLD) { out_byte(b, '1'); sep = 1; }
    if (add & UI_CELLS_ITALIC) { if (sep) out_byte(b, ';'); out_byte(b, '3'); sep = 1; }
    if (add & UI_CELLS_UNDERLINE) { if (sep) out_byte(b, ';'); out_byte(b, '4'); sep = 1; }
    if (add & UI_CELLS_REVERSE) { if (sep) out_byte(b, ';'); out_byte(b, '7'); sep = 1; }
    if (!same_bg(pen, p)) {
        if (sep) out_byte(b, ';');
        if (p->flags & UI_CELLS_BG_DEFAULT) out_str(b, "49"); else out_colour(b, 40, 100, 48, p->bg);
        sep = 1;
    }
    if (pen->fg != p->fg) { if (sep) out_byte(b, ';'); out_colour(b, 30, 90, 38, p->fg); }
    out_byte(b, 'm');
}

/* A cell is four bytes; the flush compares it as one 32-bit word. memcpy keeps that legal for any
 * alignment and byte order (equality does not care which byte is high). On the Amiga the arrays come
 * from malloc and every cell is 4 bytes, so each cell is long-aligned and a direct long compare
 * (one cmp.l, no call) is safe. Ports keep the memcpy form. */
typedef char ui_cell_is_four_bytes[sizeof(ui_cell) == 4 ? 1 : -1];
#ifdef AMIGA
long ui_cells_first_diff(const ui_cell *a, const ui_cell *b, long n);   /* ui_cells_68k.s: cmpm.l/dbne */
#else
static int same_cell(const ui_cell *a, const ui_cell *b)
{
    uint32_t x, y;
    memcpy(&x, a, sizeof x);
    memcpy(&y, b, sizeof y);
    return x == y;
}
/* Index of the first cell where a and b differ, or n when all n cells are equal. The flush calls
 * this once per run of unchanged cells; a key that changed three cells skips the other 1997 in one
 * call. The Amiga build replaces it with the assembly in ui_cells_68k.s; this C is the reference. */
static long ui_cells_first_diff(const ui_cell *a, const ui_cell *b, long n)
{
    long i;
    for (i = 0; i < n; i++) if (!same_cell(&a[i], &b[i])) break;
    return i;
}
#endif

/* The flush proper. It starts from the cursor position and style the terminal was left with
 * (c->t_*; unknown after ui_cells_forget) and records where it leaves them. */
static void flush_cells(ui_cells *c, ansi_buf *out, ui_cells_drain_fn drain, void *ctx)
{
    long i = 0, n = (long)c->cols * c->rows, last = n - 1;
    int cur_row = c->t_row, cur_col = c->t_col, have_style = c->t_have, cols = c->cols, full = c->full, styled = 0;
    int next_row = 0, next_col = 0;     /* row and column of cell i, stepped rather than divided */
    long done = -1;                     /* the last cell this flush sent */
    const char *wide;
    ui_cell style = c->t_style;
    c->sent = 0;
    while (i < last) {
        ui_cell *p;
        int row, col;
        if (!full) {                    /* skip the run of unchanged cells in one compare loop */
            long skip = ui_cells_first_diff(&c->back[i], &c->front[i], last - i);
            if (skip > 0) {
                i += skip;
                if (i >= last) break;
                next_col += (int)skip;  /* one division per run, not per cell */
                if (next_col >= cols) { next_row += next_col / cols; next_col %= cols; }
            }
        }
        /* Landed on the right half of a glyph: the glyph to its left is resent with it (whole, or as a
         * blank when the pair is broken), or the terminal keeps half an emoji. */
        if (i > 0 && i - 1 > done && (c->back[i - 1].flags & UI_CELLS_WIDE)) {
            i--;
            if (next_col == 0) { next_col = cols - 1; next_row--; } else next_col--;
        }
        p = &c->back[i];
        row = next_row; col = next_col;
        if (++next_col == cols) { next_col = 0; next_row++; }
        wide = 0;
        if (p->flags & UI_CELLS_WIDE) {
            /* Whole only with its tail beside it on the same row, short of the never-sent last cell. */
            if (col + 1 < cols && i + 1 < last && (c->back[i + 1].flags & UI_CELLS_WIDE_TAIL)
                && p->ch >= 1 && p->ch <= c->nwide && c->wide[p->ch - 1] != 0) wide = c->wide[p->ch - 1];
        }
        if (drain != 0 && out->len > out->cap - 64) { drain(ctx); }
        if (row != cur_row || col != cur_col) {
            int gap = row == cur_row ? col - cur_col : 0, k = 0;
            if (gap > 0 && gap <= 4 && have_style) {
                /* A few unchanged cells in the style the terminal holds: resending them (a byte each)
                 * is shorter than any cursor move over them. */
                const ui_cell *q = p - gap;
                while (k < gap && !(q[k].flags & (UI_CELLS_WIDE | UI_CELLS_WIDE_TAIL))
                       && (same_style(&style, &q[k]) || blank_fits(&style, &q[k]))) k++;
                if (k == gap) for (k = 0; k < gap; k++) out_byte(out, (char)q[k].ch);
                else k = 0;
            }
            if (k > 0) { /* bridged */ }
            else if (gap > 0) { out_str(out, "\033["); if (gap > 1) out_int(out, gap); out_byte(out, 'C'); }
            else {
                out_str(out, "\033["); out_int(out, row + 1);
                if (col > 0) { out_byte(out, ';'); out_int(out, col + 1); }   /* ESC[rH: column 1 */
                out_byte(out, 'H');
            }
        }
        if (!have_style || !(same_style(&style, p) || blank_fits(&style, p))) {
            out_sgr_to(out, have_style, &style, p);
            style = *p; style.flags &= STYLE_FLAGS; have_style = 1; styled = 1;
        }
        if (wide != 0) {
            out_str(out, wide);
            c->front[i] = *p;
            c->front[i + 1] = c->back[i + 1];
            i += 2;
            if (++next_col == cols) { next_col = 0; next_row++; }
            c->sent += 2;
            cur_row = row;
            cur_col = col + 2;
        } else {
            /* A broken glyph or tail is a blank: its byte is a table id, not a character. */
            out_byte(out, (p->flags & (UI_CELLS_WIDE | UI_CELLS_WIDE_TAIL)) ? ' ' : (char)p->ch);
            c->front[i] = *p;
            i++;
            c->sent++;
            cur_row = row;
            cur_col = col + 1;
        }
        done = i - 1;
        if (cur_col >= c->cols) cur_row = -1;
    }
    c->front[n - 1] = c->back[n - 1];
    c->t_row = cur_row; c->t_col = cur_col; c->t_style = style; c->t_have = have_style;
    if (styled) {
        /* ansi_color's cache no longer matches the terminal. */
        out->last_fg = -1; out->last_bg = -1; out->last_bold = -1; out->last_reverse = -1;
    }
    c->full = 0;
}

void ui_cells_flush(ui_cells *c, ansi_buf *out, ui_cells_drain_fn drain, void *ctx)
{
    ui_cells_forget(c);                 /* the caller may have written to the terminal itself */
    flush_cells(c, out, drain, ctx);
    ui_cells_forget(c);
}

void ui_cells_present(ui_cells *c, ansi_buf *out, ui_cells_drain_fn drain, void *ctx, int caret, int cr, int cc)
{
    caret = caret ? 1 : 0;
    flush_cells(c, out, drain, ctx);
    if (c->t_show != caret) { out_str(out, caret ? "\033[?25h" : "\033[?25l"); c->t_show = caret; }
    if (caret && (c->t_row != cr || c->t_col != cc)) {
        out_str(out, "\033["); out_int(out, cr + 1); out_byte(out, ';'); out_int(out, cc + 1); out_byte(out, 'H');
        c->t_row = cc < c->cols ? cr : -1; c->t_col = cc;
    }
}

/* One row of `w` cells in one colour: `text` from the left, spaces after it. The menu bar, the
 * chat pane's label bar and the status line are all this, in different colours. */
void ui_cells_bar(ui_cells *c, int row, int col, int w, const char *text, int fg, int bg, int bold)
{
    int pfg = c->fg, pbg = c->bg, pbold = c->bold, n = 0;
    ui_cells_pen(c, fg, bg, bold);
    if (text != 0) n = ui_cells_text(c, row, col, text, w);
    if (n < w) ui_cells_fill(c, row, col + n, 1, w - n, ' ');
    ui_cells_pen(c, pfg, pbg, pbold);
}

/* A bordered button three rows high: the border in `border_fg`, `text` centred inside in `fg`. */
void ui_cells_button(ui_cells *c, int top, int left, int w, const char *text, int border_fg, int fg)
{
    int pfg = c->fg, len = (int)strlen(text), at;
    if (w < 3) return;
    ui_cells_box(c, top, left, 3, w, border_fg, 0, 0);
    c->fg = fg;
    ui_cells_fill(c, top + 1, left + 1, 1, w - 2, ' ');
    at = (w - 2 - len) / 2;
    if (at < 0) at = 0;
    ui_cells_text(c, top + 1, left + 1 + at, text, w - 2);
    c->fg = pfg;
}

void ui_hit_reset(ui_hit *h) { h->n = 0; }

void ui_hit_add(ui_hit *h, int id, int top, int left, int rows, int cols)
{
    if (h->n >= UI_HIT_MAX || rows < 1 || cols < 1) return;
    h->r[h->n].id = id; h->r[h->n].top = top; h->r[h->n].left = left;
    h->r[h->n].rows = rows; h->r[h->n].cols = cols;
    h->n++;
}

int ui_hit_at(const ui_hit *h, int row, int col, int *dr, int *dc)
{
    int i;
    for (i = h->n - 1; i >= 0; i--) {
        const ui_hit_rect *r = &h->r[i];
        if (row < r->top || row >= r->top + r->rows || col < r->left || col >= r->left + r->cols) continue;
        if (dr) *dr = row - r->top;
        if (dc) *dc = col - r->left;
        return r->id;
    }
    return -1;
}

static int popup_sep(const ui_popup *p, int i) { return strcmp(p->items[i], "---") == 0; }

static void popup_step(ui_popup *p, int dir)
{
    int i, at = p->sel;
    for (i = 0; i < p->n; i++) {
        at = (at + dir + p->n) % p->n;
        if (!popup_sep(p, at)) { p->sel = at; return; }
    }
}

void ui_popup_open(ui_popup *p, const char *const *items, int n, unsigned long alert, int row, int col, int rows, int cols)
{
    int i;
    if (n > UI_POPUP_MAX) n = UI_POPUP_MAX;
    p->n = n; p->alert = alert; p->w = 24;
    for (i = 0; i < n; i++) {
        int l = (int)strlen(items[i]) + 4;
        p->items[i] = items[i];
        if (l > p->w) p->w = l;
    }
    p->top = row < rows - (n + 4) ? row : rows - (n + 4);
    if (p->top < 0) p->top = 0;
    p->left = col < cols - 26 ? col : cols - 26;
    if (p->left < 0) p->left = 0;
    p->sel = -1;
    popup_step(p, 1);
    p->open = n > 0 && p->sel >= 0;
}

int ui_popup_key(ui_popup *p, int key)
{
    if (!p->open) return UI_POPUP_CLOSE;
    switch (key) {
    case UI_KEY_UP: popup_step(p, -1); return UI_POPUP_STAY;
    case UI_KEY_DOWN: popup_step(p, 1); return UI_POPUP_STAY;
    case UI_KEY_ENTER: case '\r': case '\n': p->open = 0; return p->sel;
    case UI_KEY_ESC: p->open = 0; return UI_POPUP_CLOSE;
    default: return UI_POPUP_STAY;
    }
}

int ui_popup_click(ui_popup *p, int row, int col)
{
    int i = row - p->top - 1;
    if (!p->open) return UI_POPUP_CLOSE;
    if (row < p->top || row >= p->top + p->n + 2 || col < p->left || col >= p->left + p->w) {
        p->open = 0;
        return UI_POPUP_CLOSE;
    }
    if (i < 0 || i >= p->n || col == p->left || col == p->left + p->w - 1 || popup_sep(p, i)) return UI_POPUP_STAY;
    p->sel = i;
    p->open = 0;
    return i;
}

void ui_popup_draw(ui_cells *c, const ui_popup *p, int border_fg, int fg, int sel_fg, int alert_fg)
{
    int i, pen = c->fg;
    if (!p->open) return;
    ui_cells_fill(c, p->top + 1, p->left + 1, p->n, p->w - 2, ' ');
    ui_cells_box(c, p->top, p->left, p->n + 2, p->w, border_fg, 0, 0);
    for (i = 0; i < p->n; i++) {
        int row = p->top + 1 + i, k;
        if (popup_sep(p, i)) {
            c->fg = border_fg;
            for (k = 1; k < p->w - 1; k++) ui_cells_put(c, row, p->left + k, '-');
            continue;
        }
        c->fg = i == p->sel ? sel_fg : (p->alert >> i) & 1UL ? alert_fg : fg;
        ui_cells_text(c, row, p->left + 1, i == p->sel ? "> " : "  ", 2);
        ui_cells_text(c, row, p->left + 3, p->items[i], p->w - 4);
    }
    c->fg = pen;
}
