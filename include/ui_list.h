/*
 * ui_list - a bordered list with a selection and a scroll bar.
 *
 * The widget the catalogue actually needs: nearly every door in the door
 * list is "show these rows, let somebody pick one". DOORMAN draws it with
 * blessed; this draws the same thing in a C door, at eight colours and in
 * plain ASCII, because on a real Amiga the terminal is topaz-8 and a
 * multi-byte box character renders as mojibake.
 *
 * The list OWNS the scrolling arithmetic, which is the part every hand-rolled
 * copy gets wrong: a selection that walks off the bottom has to pull the
 * window with it, a window must never show past the end, and a list shorter
 * than its box must not scroll at all.
 */

#ifndef UI_LIST_H
#define UI_LIST_H

#include "ui_ansi.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Where the rows come from: `index` is 0-based, the return is the text. */
typedef const char *(*ui_list_row_fn)(void *context, int index);

/**
 * How a row's text is inked in two segments: `*head` up to column `split`
 * of the text, `*tail` from there on. Return 0 for no split (the whole row is
 * in `*head`). Only unselected rows ask; the selected row keeps the
 * selection's colours across its whole width.
 */
typedef int (*ui_list_style_fn)(void *context, int index, int *head, int *tail);

typedef struct {
    /** Where the box sits, 1-based like every ANSI coordinate. */
    int top, left, height, width;
    /** How many rows exist, and which one is chosen. */
    int count;
    int selected;
    /** The first visible row - the widget maintains it. */
    int offset;
    /** Border colour, row colour, and the selected row's two. */
    int chrome, ink, selected_fg, selected_bg;
    /** Drawn in the top border, or 0 for none. */
    const char *label;
    /** Draw the border at all. False at 40 columns (ui_profile.h). */
    int borders;
    /**
     * Mark the selected row with this instead of painting a bar.
     *
     * NULL for a bar, which is what a wide screen shows. At 40 columns the
     * TypeScript picker marks the cursor with blessed's own ">>" and paints
     * no bar at all, and the two doors are meant to be the same screen -
     * the sysop put them side by side on 2026-09-06.
     *
     * The row is drawn in the selection's ink, so the caret is not the only
     * thing that moves.
     */
    const char *caret;
    /**
     * Non-zero: leaving the window scrolls a PAGE (the selection lands at the
     * edge it came from), not one row. A one-row scroll repaints the whole
     * box on every key past the edge; a page costs one box per page.
     */
    int page_follow;
    /**
     * Rows below the selection that must stay inside the window with it: 1
     * for a door whose row has its value on the line under it.
     */
    int follow_extra;
    ui_list_row_fn row;
    void *context;
    /** Optional per-segment inks for an unselected row; NULL = all in `ink`. */
    ui_list_style_fn style;
    /**
     * The background every cell of the list is drawn on, or -1 for the
     * terminal's own (what ui_list_init sets). A door that paints its ground
     * sets this so the rows, the caret blanks and the scroll bar sit on it.
     */
    int ground;
    /**
     * Non-zero with `caret`: the selected row is ALSO a bar in
     * selected_fg/selected_bg across its whole width, caret included.
     */
    int caret_bar;
    /**
     * What is on the glass right now, so a cursor move repaints two rows
     * instead of the whole box. -1 until ui_list_draw() has run once.
     * Every byte a 68K door writes is an XIM message: a full repaint of
     * this list is ~1,900 bytes, ten messages, half a second - per
     * keypress (sysop, 2026-09-07: "its slow"). Two rows is ~150.
     */
    int drawn_selected;
    int drawn_offset;
    /**
     * THE ONE SCROLLBAR (sysop, 2026-10-15): solid colour bars, no glyphs.
     * The track is a space on `scroll_track`, the thumb a space on
     * `scroll_thumb`; ui_list_init sets them to the theme's DIM and INK
     * tokens, the same roles Element.renderScrollbar reads
     * (sdk/engines/ui/theme/styles.ts, scrollbarRoles). A door does not pick
     * a glyph or a colour for its bar. On a C64 (no per-cell background)
     * ansi_color() turns each into a reverse-video space.
     */
    int scroll_track, scroll_thumb;
} ui_list;

/**
 * Sensible defaults: white on black, a cyan frame, borders on, nothing
 * selected. A caller then sets what it cares about.
 */
void ui_list_init(ui_list *list);

/** Rows visible inside the box, which is the height minus its frame. */
int ui_list_visible_rows(const ui_list *list);

/**
 * Move the selection by `delta` and pull the window after it.
 *
 * Clamps at both ends rather than wrapping: a list that jumps from the last
 * row to the first on one keypress is how somebody loses their place.
 */
void ui_list_move(ui_list *list, int delta);

/** Select an exact row, clamped, window following. */
void ui_list_select(ui_list *list, int index);

/** Draw it. Composes into `b`; the caller flushes. */
void ui_list_draw(ui_list *list, ansi_buf *b);

/**
 * Repaint only what changed since the last draw: the row the cursor left
 * and the row it landed on. Falls back to a full ui_list_draw() when the
 * window scrolled, the list has never been drawn, or the rows themselves
 * may have changed (pass `rows_changed`). The smart path every door should
 * take on a keypress - RULES.md, "Doors draw smart".
 */
void ui_list_draw_changed(ui_list *list, ansi_buf *b, int rows_changed);

/** Repaint exactly row `index` if it is visible; a full draw if the list was never drawn or scrolled. */
void ui_list_draw_row(ui_list *list, ansi_buf *b, int index);

#ifdef __cplusplus
}
#endif

#endif /* UI_LIST_H */
