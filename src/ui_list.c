/*
 * ui_list - see ui_list.h.
 *
 * Two things worth reading before changing anything here.
 *
 * THE WINDOW FOLLOWS THE SELECTION, not the other way round. Every function
 * that moves the selection ends by calling follow(), which is the only place
 * `offset` is written. A second place that scrolled would eventually
 * disagree with this one, and the symptom is a selected row that is not on
 * screen.
 *
 * THE SCROLL BAR IS PROPORTIONAL AND ALWAYS HAS A THUMB. A bar that rounds
 * its thumb to zero rows on a long list looks like a bar that is broken.
 */

#include "ui_list.h"
#include "ui_theme.h"

#include <string.h>

void ui_list_init(ui_list *list)
{
    if (!list) return;

    memset(list, 0, sizeof(*list));
    list->chrome = ANSI_CYAN;
    list->ink = ANSI_WHITE;
    list->selected_fg = ANSI_WHITE;
    list->selected_bg = ANSI_BLUE;
    list->caret = 0;
    list->drawn_selected = -1;
    list->drawn_offset = -1;
    list->borders = 1;
    list->ground = -1;
    list->scroll_track = UI_TOKEN(UI_T_DIM);
    list->scroll_thumb = UI_TOKEN(UI_T_INK);
}

int ui_list_visible_rows(const ui_list *list)
{
    int rows;

    if (!list) return 0;
    rows = list->borders ? list->height - 2 : list->height;
    return rows > 0 ? rows : 0;
}

/** Keep `offset` showing `selected`, and never past the end. */
static void follow(ui_list *list)
{
    int visible = ui_list_visible_rows(list);
    int max_offset;

    if (visible <= 0) { list->offset = 0; return; }

    {
        int extra = list->follow_extra > 0 ? list->follow_extra : 0;
        int need = list->selected + extra;           /* the last row that must show */
        if (extra >= visible) extra = 0, need = list->selected;

        if (list->selected < list->offset) {
            /* A page back puts the selection (and its extra rows) at the bottom. */
            list->offset = list->page_follow ? need - visible + 1 : list->selected;
        } else if (need >= list->offset + visible) {
            list->offset = list->page_follow ? list->selected : need - visible + 1;
        }
    }

    /* A list shorter than its box does not scroll at all, and no window ever
       shows past the last row. */
    max_offset = list->count - visible;
    if (max_offset < 0) max_offset = 0;
    if (list->offset > max_offset) list->offset = max_offset;
    if (list->offset < 0) list->offset = 0;
}

void ui_list_select(ui_list *list, int index)
{
    if (!list) return;

    if (index < 0) index = 0;
    if (index > list->count - 1) index = list->count - 1;
    if (index < 0) index = 0;              /* an empty list selects nothing */

    list->selected = index;
    follow(list);
}

void ui_list_move(ui_list *list, int delta)
{
    if (!list) return;
    ui_list_select(list, list->selected + delta);
}

/**
 * The scroll bar's thumb: where it starts and how tall it is.
 *
 * Proportional to what is showing, with a floor of one row - a thumb that
 * rounds to nothing reads as a broken widget rather than a long list.
 */
static void thumb_for(const ui_list *list, int visible, int *start, int *size)
{
    int span;

    *size = (visible * visible) / (list->count > 0 ? list->count : 1);
    if (*size < 1) *size = 1;
    if (*size > visible) *size = visible;

    span = list->count - visible;
    if (span <= 0) { *start = 0; *size = visible; return; }

    *start = (list->offset * (visible - *size)) / span;
    if (*start < 0) *start = 0;
    if (*start > visible - *size) *start = visible - *size;
}

/* One row of the box, exactly as the full draw paints it. */
static void draw_row(ui_list *list, ansi_buf *b, int row, int inner_left,
                     int inner_width, int scrolls, int thumb_start, int thumb_size, int bar)
{
        int index = list->offset + row;
    int screen_row = (list->borders ? list->top + 1 : list->top) + row;
    int chosen = (index == list->selected) && (list->count > 0);
    const char *text = "";
    /* With colour off the board strips the bar along with every other SGR,
       so a list with no caret of its own falls back to ">>": the selected
       row has to be marked by something that survives (ui_ansi.h, colour). */
    const char *caret = list->caret ? list->caret : (b->colour ? 0 : ">>");

    if (index < list->count && list->row) {
    text = list->row(list->context, index);
        if (!text) text = "";
    }

    /* The selected row is a filled bar, not just coloured text: a
       highlight that stops at the end of the word leaves the eye
       hunting for where the selection is. */
    /* A screen with no cell backgrounds (a C64) takes the bar as reverse video, so a caret row there
       is a bar too: the caret alone left the selected row unmarked but for two characters. */
    if (chosen && caret && (list->caret_bar || (b->colour && !b->cell_backgrounds))) {
        /* No separate fill: the caret and the padded text below cover the row once. */
        ansi_color(b, list->selected_fg, list->selected_bg, 1);
        ansi_text_raw(b, screen_row, inner_left, caret,
                      (int) strlen(caret));
    } else if (chosen && caret) {
    /* A caret, and the row in the selection's ink beside it. No
           bar: the caret IS the mark. */
        ansi_color(b, list->ink, list->ground, 1);
        ansi_text_raw(b, screen_row, inner_left, caret,
                      (int) strlen(caret));
    } else if (chosen) {
    /* Ink on a bar. On a screen whose cells cannot carry a
           background, ansi_color turns this into reverse video in the
           bar's colour - the widget does not need to know which kind of
           screen it is on (ui_ansi.h, cell_backgrounds). */
        ansi_fill(b, screen_row, inner_left, inner_width,
                  list->selected_fg, list->selected_bg);
        ansi_color(b, list->selected_fg, list->selected_bg, 1);
    } else {
    /* No background: a list row is text on whatever the screen is,
           which is what blessed draws - `style.item` carries a colour
           and no bg. Painting the ground explicitly made every row a
           filled band in the C door and none in the TypeScript. */
        ansi_color(b, list->ink, list->ground, 0);
    }

    /* ansi_text pads to the width, so a shorter row overwrites what was
       under it without a clearing pass. A caret row starts after the
       caret; every other row starts where the caret would have been, so
       the list does not shuffle sideways as the cursor moves. */
    {
    int lead = caret ? (int) ui_printable_len(caret) : 0;

        /* ERASE THE CARET CELLS ON A ROW THAT NO LONGER CARRIES IT.
         *
         * The text starts AFTER the caret column so the list does not shuffle
         * sideways - which means nothing was ever written over the caret of
         * the row the cursor just left. `ui_list_draw_changed` repaints both
         * rows and the old one kept its ">>" anyway, so every row a caller
         * visited stayed marked: five carets down the theme picker after four
         * keypresses (sysop, 2026-09-07).
         *
         * Spaces, not a fill: a list row is text on whatever ground the screen
         * has, and painting a background here would put a band under the one
         * place the widget is careful not to. */
        if (!chosen && lead > 0) {
            char blanks[16];
            int n = lead < (int) sizeof(blanks) - 1 ? lead : (int) sizeof(blanks) - 1;
            memset(blanks, ' ', (size_t) n);
            blanks[n] = '\0';
            ansi_text_raw(b, screen_row, inner_left, blanks, n);
        }

        {
            int head = list->ink, tail = list->ink, split = 0;
            if (!chosen && list->style && index < list->count)
                split = list->style(list->context, index, &head, &tail);
            if (!chosen && list->style) {
                if (split > 0 && split < (int) strlen(text) && split < inner_width - lead) {
                    ansi_color(b, head, list->ground, 0);
                    ansi_text_raw(b, screen_row, inner_left + lead, text, split);
                    ansi_color(b, tail, list->ground, 0);
                    ansi_text(b, screen_row, inner_left + lead + split, text + split,
                              inner_width - lead - split);
                } else {
                    ansi_color(b, head, list->ground, 0);
                    ansi_text(b, screen_row, inner_left + lead, text, inner_width - lead);
                }
            } else {
                ansi_text(b, screen_row, inner_left + lead, text, inner_width - lead);
            }
        }
    }

    /* The thumb depends on the window and the count only: a repaint at the same
       offset leaves it as it is, and rewriting it was ~40 bytes a row (colour,
       address, glyph) for a cell that cannot have changed. */
    if (scrolls && bar) {
    int on_thumb = (row >= thumb_start) && (row < thumb_start + thumb_size);
        int colour = on_thumb ? list->scroll_thumb : list->scroll_track;
        ansi_bar_cells(b, screen_row, inner_left + inner_width, 1, colour, on_thumb ? '#' : '|');
    }
    }

void ui_list_draw(ui_list *list, ansi_buf *b)
{
    int visible, row, inner_left, inner_width;
    int thumb_start = 0, thumb_size = 0;
    int scrolls;

    if (!list || !b || list->width < 3 || list->height < 1) return;

    visible = ui_list_visible_rows(list);
    if (visible <= 0) return;

    if (list->borders) {
        ansi_box(b, list->top, list->left, list->height, list->width,
                 list->chrome, list->label);
    }

    inner_left = list->borders ? list->left + 1 : list->left;
    inner_width = list->borders ? list->width - 2 : list->width;

    /* The bar takes a column from the text, and only when there is
       something to scroll: a list that fits should not lose a column to a
       bar that would never move. */
    scrolls = list->count > visible;
    if (scrolls) inner_width -= 1;
    if (inner_width < 1) return;

    if (scrolls) thumb_for(list, visible, &thumb_start, &thumb_size);

    for (row = 0; row < visible; row++) {
        draw_row(list, b, row, inner_left, inner_width, scrolls, thumb_start, thumb_size, 1);
    }

    list->drawn_selected = list->selected;
    list->drawn_offset = list->offset;
    ansi_color(b, list->ink, list->ground, 0);
}

static int geometry(const ui_list *list, int *inner_left, int *inner_width,
                    int *scrolls, int *thumb_start, int *thumb_size)
{
    int visible = ui_list_visible_rows(list);
    *inner_left = list->borders ? list->left + 1 : list->left;
    *inner_width = list->borders ? list->width - 2 : list->width;
    *scrolls = list->count > visible;
    if (*scrolls) *inner_width -= 1;
    *thumb_start = 0; *thumb_size = 0;
    if (*scrolls) thumb_for(list, visible, thumb_start, thumb_size);
    return *inner_width >= 1 ? visible : 0;
}

void ui_list_draw_changed(ui_list *list, ansi_buf *b, int rows_changed)
{
    int visible;
    int inner_left;
    int inner_width;
    int scrolls;
    int thumb_start, thumb_size;
    int was, now;

    if (!list || !b) return;

    /* Never drawn, scrolled, or the rows themselves moved: the whole box. */
    if (list->drawn_selected < 0 || rows_changed
        || list->drawn_offset != list->offset) {
        ui_list_draw(list, b);
        return;
    }
    if (list->drawn_selected == list->selected) return;   /* nothing moved */

    visible = geometry(list, &inner_left, &inner_width, &scrolls, &thumb_start, &thumb_size);
    if (visible == 0) return;

    was = list->drawn_selected - list->offset;
    now = list->selected - list->offset;
    if (was >= 0 && was < visible) {
        draw_row(list, b, was, inner_left, inner_width, scrolls, thumb_start, thumb_size, 0);
    }
    if (now >= 0 && now < visible) {
        draw_row(list, b, now, inner_left, inner_width, scrolls, thumb_start, thumb_size, 0);
    }
    ansi_color(b, list->ink, list->ground, 0);

    list->drawn_selected = list->selected;
}

void ui_list_draw_row(ui_list *list, ansi_buf *b, int index)
{
    int inner_left, inner_width, scrolls, thumb_start, thumb_size, visible, row;

    if (!list || !b) return;
    if (list->drawn_selected < 0 || list->drawn_offset != list->offset) {
        ui_list_draw(list, b);
        return;
    }
    visible = geometry(list, &inner_left, &inner_width, &scrolls, &thumb_start, &thumb_size);
    row = index - list->offset;
    if (row < 0 || row >= visible) return;
    draw_row(list, b, row, inner_left, inner_width, scrolls, thumb_start, thumb_size, 0);
    ansi_color(b, list->ink, list->ground, 0);
}
