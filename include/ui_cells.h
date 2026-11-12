/*
 * ui_cells.h - a cell canvas with a diff flush, for doors that paint a
 * windowed screen (menu bar, panes, boxes, modals) and resend only the cells
 * that changed.
 *
 * WHY. ui_ansi writes straight to the wire and colours by theme token
 * (truecolour or 16 colours). A door that must look like a neo-blessed door
 * cell for cell needs blessed's palette (xterm-256 indices) and blessed's
 * SGR shape: every run starts with a reset, so a cell never inherits state
 * from the terminal. The canvas holds what the screen should be (back) and
 * what was last sent (front); ui_cells_flush sends the difference.
 *
 * COLOURS. 0..255 are xterm indices: 0-7 go out as SGR 30-37 / 40-47,
 * 8-15 as 90-97 / 100-107, 16-255 as 38;5;N / 48;5;N. UI_CELLS_DEFAULT (-1)
 * as a background is SGR 49, never 40 (a painted black shows as blocks).
 *
 * MARKED TEXT. ui_cells_marked and ui_cells_wrap take text with inline
 * style bytes: UI_CELLS_M_FG then one byte 1..255 sets the foreground,
 * UI_CELLS_M_BOLD and UI_CELLS_M_PLAIN switch bold, UI_CELLS_M_BASE returns
 * to the pen the call started with, UI_CELLS_M_ATTR then UI_CELLS_ATTR(bits) sets underline, italic and
 * reverse (SGR 4, 3, 7). Markers take no columns, except
 * UI_CELLS_M_WIDE then one byte id 1..255: a two-column glyph, entry id-1 of
 * the table ui_cells_wide_table registered (an emoji, sent as its UTF-8
 * bytes). Only a door that told the board it writes UTF-8 registers one.
 *
 * MEMORY. Two heap arrays of cols*rows cells (4 bytes each): 16000 bytes at
 * 80x25. Nothing large on the stack.
 *
 * BORDERS are ASCII: '.' top corners, '`' bottom left, '\'' bottom right,
 * '-' and '|'. A labelled top edge reads ".- label ----.".
 */
#ifndef UI_CELLS_H
#define UI_CELLS_H

#include "ui_ansi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_CELLS_DEFAULT (-1)
#define UI_CELLS_BOLD 1
#define UI_CELLS_BG_DEFAULT 2
#define UI_CELLS_WIDE 4        /* a two-column glyph: ch is its table id */
#define UI_CELLS_WIDE_TAIL 8   /* the right half of the glyph to its left */
#define UI_CELLS_UNDERLINE 16  /* SGR 4 */
#define UI_CELLS_ITALIC 32     /* SGR 3 (the Amiga console draws it) */
#define UI_CELLS_REVERSE 64    /* SGR 7: foreground and background swapped */

#define UI_CELLS_M_FG    '\001'
#define UI_CELLS_M_BOLD  '\002'
#define UI_CELLS_M_PLAIN '\003'
#define UI_CELLS_M_BASE  '\004'
#define UI_CELLS_M_WIDE  '\005'
#define UI_CELLS_M_ATTR  '\006'   /* then UI_CELLS_ATTR(bits): sets the attributes (replaces; UI_CELLS_ATTR(0) clears) */
#define UI_CELLS_A_UNDERLINE 1
#define UI_CELLS_A_ITALIC 2
#define UI_CELLS_A_REVERSE 4
#define UI_CELLS_ATTR(bits) ('@' + (bits))

typedef struct {
    unsigned char ch;
    unsigned char fg;
    unsigned char bg;
    unsigned char flags;   /* UI_CELLS_BOLD, UI_CELLS_BG_DEFAULT, UI_CELLS_WIDE, UI_CELLS_WIDE_TAIL */
} ui_cell;

typedef struct {
    int cols, rows;
    ui_cell *back;         /* what the screen should show */
    ui_cell *front;        /* what was last sent */
    int full;              /* 1: the next flush resends every cell */
    int fg, bg, bold;      /* the pen */
    int attr;              /* the pen's UI_CELLS_A_* bits; only ui_cells_marked sets them, and restores them */
    long sent;             /* cells sent by the last flush */
    const char *const *wide; /* UI_CELLS_M_WIDE glyphs, id 1 = wide[0]; 0: none */
    int nwide;
    /* What the terminal was left with, so the next ui_cells_present sends no move, colour or cursor
     * sequence it already has. Unknown (-1 / t_have 0) after ui_cells_forget. */
    int t_row, t_col;      /* cursor, 0-based */
    int t_have;            /* 1: t_style is the terminal's current SGR state */
    int t_show;            /* text cursor: 1 shown, 0 hidden */
    ui_cell t_style;
} ui_cells;

/** Registers the two-column glyphs UI_CELLS_M_WIDE refers to (after ui_cells_open). A glyph whose
 *  pair was broken by a later write, or whose id has no entry, is sent as a blank. */
void ui_cells_wide_table(ui_cells *c, const char *const *glyphs, int n);

/** Allocates both buffers; every cell is a blank in fg 7 on the default background. Returns 0 or -1. */
int ui_cells_open(ui_cells *c, int cols, int rows);
void ui_cells_close(ui_cells *c);
/** Forgets what the terminal shows: the next flush sends every cell (after a resize or a clear). */
void ui_cells_invalidate(ui_cells *c);

void ui_cells_pen(ui_cells *c, int fg, int bg, int bold);
/** One cell in the pen. Off-canvas writes are dropped. */
void ui_cells_put(ui_cells *c, int row, int col, int ch);
/** Plain text in the pen, at most `max` columns (max < 0: to the right edge). Returns columns written. */
int ui_cells_text(ui_cells *c, int row, int col, const char *s, int max);
/** A rectangle of `ch` in the pen. */
void ui_cells_fill(ui_cells *c, int row, int col, int h, int w, int ch);
/** An ASCII box in `border_fg` on the pen background; `label` (may be NULL) sits in the top edge in
 *  `label_fg` as ".- label ---.". The inside is not touched. */
void ui_cells_box(ui_cells *c, int top, int left, int h, int w, int border_fg, const char *label, int label_fg);
/** A centred box with a cleared inside: writes its top/left to *top / *left. Clamped to the canvas. */
void ui_cells_modal(ui_cells *c, int h, int w, int border_fg, const char *label, int label_fg, int *top, int *left);
/** A vertical menu inside a box at top/left: the selected item is "> item" in `sel_fg`, the rest
 *  "  item" in `fg`. Width is the longest item + 4 (+2 border). Returns the box width. */
int ui_cells_menu(ui_cells *c, int top, int left, const char *const *items, int n, int sel, int border_fg, int fg, int sel_fg);

/** Visible columns of marked text. */
int ui_cells_marked_cols(const char *s);
/** Marked text bytes [from, to) at row/col, at most `max` columns. Style markers before `from` are
 *  applied but not drawn, so a wrapped line keeps the style it started in. Returns columns written. */
int ui_cells_marked(ui_cells *c, int row, int col, const char *s, int from, int to, int max);
/** Word-wraps marked text to `width` columns. starts[k]/ends[k] get each line's byte range (the space
 *  at a break is dropped; a word longer than the width is cut). Returns the number of lines, at most `max`. */
int ui_cells_wrap(const char *s, int width, int *starts, int *ends, int max);

/** Callback that empties `out` when it is nearly full (ui_screen_flush for a door). */
/* A row of `w` cells: text from the left, then spaces, all in fg/bg/bold (menu bar, label bar, status line). */
void ui_cells_bar(ui_cells *c, int row, int col, int w, const char *text, int fg, int bg, int bold);
/* A three-row bordered button with centred text. */
void ui_cells_button(ui_cells *c, int top, int left, int w, const char *text, int border_fg, int fg);

typedef void (*ui_cells_drain_fn)(void *ctx);
/** Sends the cells that differ from what was last sent, in as few bytes as it can: no cursor move
 *  between neighbours, ESC[nC or the cells themselves over a short unchanged gap, and on a style change
 *  only the part that differs (the reset-led ESC[0;... form when an attribute has to go). A blank goes
 *  out in whatever foreground the terminal holds. The bottom-right cell is never sent (it would scroll
 *  some terminals). Assumes nothing about the terminal's cursor or colours before, and leaves nothing
 *  assumed after: the caller may write to the terminal itself between flushes. */
void ui_cells_flush(ui_cells *c, ansi_buf *out, ui_cells_drain_fn drain, void *ctx);
/** One frame of a door that owns the whole screen: the flush, then the text cursor shown at (cr, cc)
 *  (0-based) when `caret`, hidden when not. Unlike ui_cells_flush it REMEMBERS where it left the
 *  terminal's cursor, colours and cursor visibility, and the next call sends only what differs: a key
 *  typed at the caret is one byte on the wire. A door that writes anything else to the terminal
 *  (a picture, a clipboard sequence, ansi_* calls) calls ui_cells_forget after it. */
void ui_cells_present(ui_cells *c, ansi_buf *out, ui_cells_drain_fn drain, void *ctx, int caret, int cr, int cc);
/** The terminal's cursor and colours are no longer what ui_cells_present left. ui_cells_invalidate does this too. */
void ui_cells_forget(ui_cells *c);

/* ---- Hit-testing: rectangles with an id, the last one added wins (it is drawn on top). ---- */
#define UI_HIT_MAX 32
typedef struct { int id, top, left, rows, cols; } ui_hit_rect;
typedef struct { ui_hit_rect r[UI_HIT_MAX]; int n; } ui_hit;
void ui_hit_reset(ui_hit *h);
/** Add a region (0-based canvas cells). Full list: the region is dropped. */
void ui_hit_add(ui_hit *h, int id, int top, int left, int rows, int cols);
/** The id of the topmost region holding row/col (0-based), -1 none; dr and dc (may be 0) get the offset inside it. */
int ui_hit_at(const ui_hit *h, int row, int col, int *dr, int *dc);

/* ---- Popup list (a context menu): opens at a click, clamped to the screen as the TS door's
 *      context-menus.ts does, "---" items are separators the selection skips, bit i of `alert`
 *      draws item i in the alert colour. ---- */
#define UI_POPUP_MAX   24
#define UI_POPUP_STAY  (-1)    /* key/hit used, popup stays open */
#define UI_POPUP_CLOSE (-2)    /* dismissed: Esc, or a click outside */
typedef struct {
    const char *items[UI_POPUP_MAX];
    int n, sel, top, left, w;
    unsigned long alert;
    int open;
} ui_popup;
/** Open at 0-based row/col on a rows x cols screen: top = max(0, min(row, rows - (n + 4))),
 *  left = max(0, min(col, cols - 26)), 24 columns wide (wider when an item needs it). */
void ui_popup_open(ui_popup *p, const char *const *items, int n, unsigned long alert, int row, int col, int rows, int cols);
/** Up/Down move (skipping separators), Enter picks: returns the item index and closes; Esc
 *  returns UI_POPUP_CLOSE and closes; any other key UI_POPUP_STAY. */
int ui_popup_key(ui_popup *p, int key);
/** A click at 0-based row/col: the item index (popup closes), UI_POPUP_STAY on the border or a
 *  separator, UI_POPUP_CLOSE outside (popup closes). */
int ui_popup_click(ui_popup *p, int row, int col);
void ui_popup_draw(ui_cells *c, const ui_popup *p, int border_fg, int fg, int sel_fg, int alert_fg);

#ifdef __cplusplus
}
#endif

#endif /* UI_CELLS_H */
