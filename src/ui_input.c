/*
 * ui_input - see ui_input.h. The editing rules are doorrepo's, lifted.
 */

#include "ui_input.h"

#include <string.h>

void ui_input_style_init(ui_input_style *style)
{
    if (!style) return;
    style->row = 1;
    style->left = 1;
    style->cols = 80;
    style->fg = ANSI_WHITE;
    style->bg = ANSI_BLUE;
    style->ink = ANSI_YELLOW;
    style->upper = 0;
    style->mask = 0;
}

/** Draw the prompt row and leave the cursor after the text. */
static void paint(ansi_buf *b, const ui_input_style *st,
                  const char *label, const char *text, int cur)
{
    char line[256];
    unsigned long room = sizeof(line) - 2;

    line[0] = '\0';
    if (label) {
        strncat(line, label, room);
        strncat(line, " ", room - strlen(line));
    }
    if (text && st->mask) {
        unsigned long at = strlen(line), i, n = strlen(text);
        for (i = 0; i < n && at < room; i++) line[at++] = (char) st->mask;
        line[at] = '\0';
    } else if (text) {
        strncat(line, text, room - strlen(line));
    }

    ansi_fill(b, st->row, st->left, st->cols, st->fg, st->bg);
    ansi_color(b, st->ink, st->bg, 1);
    /*
     * RAW, so the text is not padded out to the field width.
     *
     * ansi_fill on the line above has already blanked the row, so padding is
     * bytes spent twice - and it costs more than bytes. The pad leaves the
     * TERMINAL's cursor at the far end of the field, and the goto that puts it
     * back sits at the end of a frame that no longer fits one XIM message
     * (~198 bytes on 68K). The tail then arrives in a second message a fifth
     * of a second later, so every keystroke threw the cursor to the right of
     * the screen and dragged it back. Visible on localhost 2026-09-13, where
     * the slower redraw made the excursion easy to watch.
     */
    ansi_text_raw(b, st->row, st->left + 1, line, st->cols - 1);
    ansi_goto(b, st->row, st->left + 1 + (int) (label ? strlen(label) + 1 : 0) + cur);
    ansi_cursor(b, 1);
}

/** Put back what the prompt changed: cursor hidden, attributes reset. */
static void restore(ansi_buf *b)
{
    ansi_cursor(b, 0);
    ansi_reset(b);
}

int ui_input(ansi_buf *b, const ui_key_source *src, int *pushback,
             const ui_input_style *style, const char *label,
             char *buf, int maxlen)
{
    ui_input_style fallback;
    int len, cur;

    if (!b || !src || !pushback || !buf || maxlen <= 0) return -1;
    if (!style) { ui_input_style_init(&fallback); style = &fallback; }

    len = (int) strlen(buf);
    cur = len;

    for (;;) {
        int key;

        paint(b, style, label, buf, cur);
        /* The prompt is in a buffer until something sends it. */
        if (src->flush) src->flush(src->ctx);
        key = ui_key_read(src, pushback);

        if (key < 0) return -1;              /* the caller went away */

        if (key == UI_KEY_ENTER) {
            restore(b);
            return (buf[0] != '\0') ? 1 : 0;
        }

        /* A cursor key inside a prompt means nothing. Swallowing it is the
           point: letting it through would put an escape sequence into the
           text a door is about to act on. */
        if (key == UI_KEY_ESC || key >= 1000) continue;

        ui_line_key(buf, maxlen, &len, &cur, key, 0, style->upper);
    }
}

int ui_confirm(ansi_buf *b, const ui_key_source *src, int *pushback,
               const ui_input_style *style, const char *question,
               int default_yes)
{
    ui_input_style fallback;
    char line[256];

    if (!b || !src || !pushback) return -1;
    if (!style) { ui_input_style_init(&fallback); style = &fallback; }

    line[0] = '\0';
    strncat(line, question ? question : "", sizeof(line) - 12);
    strncat(line, default_yes ? " [Y/n] " : " [y/N] ", 8);

    for (;;) {
        int key;

        ansi_fill(b, style->row, style->left, style->cols, style->fg, style->bg);
        ansi_color(b, style->ink, style->bg, 1);
        ansi_text_raw(b, style->row, style->left + 1, line, style->cols - 1);
        ansi_cursor(b, 0);

        /* The prompt is in a buffer until something sends it. */
        if (src->flush) src->flush(src->ctx);
        key = ui_key_read(src, pushback);
        if (key < 0) return -1;

        if (key == 'y' || key == 'Y') { restore(b); return 1; }
        if (key == 'n' || key == 'N') { restore(b); return 0; }
        /* ENTER takes the default, and ESC is a no - the safe answer to a
           question somebody backed out of. */
        if (key == UI_KEY_ENTER) { restore(b); return default_yes ? 1 : 0; }
        if (key == UI_KEY_ESC) { restore(b); return 0; }
    }
}

void ui_hist_init(ui_hist *h)
{
    if (!h) return;
    h->n = 0;
    h->at = -1;
    h->draft[0] = '\0';
}

void ui_hist_reset(ui_hist *h)
{
    if (!h) return;
    h->at = -1;
    h->draft[0] = '\0';
}

void ui_hist_add(ui_hist *h, const char *text)
{
    ui_hist_add_id(h, 0, text);
}

void ui_hist_add_id(ui_hist *h, const char *id, const char *text)
{
    if (!h || !text) return;
    if (text[0] != '\0') {
        if (h->n == UI_HIST_MAX) {
            memmove(h->line[0], h->line[1], sizeof(h->line[0]) * (UI_HIST_MAX - 1));
            memmove(h->id[0], h->id[1], sizeof(h->id[0]) * (UI_HIST_MAX - 1));
            h->n--;
        }
        h->line[h->n][0] = '\0';
        strncat(h->line[h->n], text, UI_HIST_TEXT - 1);
        h->id[h->n][0] = '\0';
        if (id) strncat(h->id[h->n], id, UI_HIST_ID - 1);
        h->n++;
    }
    ui_hist_reset(h);
}

void ui_hist_rekey(ui_hist *h, const char *from, const char *to)
{
    int i;
    if (!h || !from || !to || from[0] == '\0') return;
    for (i = 0; i < h->n; i++) {
        if (strcmp(h->id[i], from) == 0) { h->id[i][0] = '\0'; strncat(h->id[i], to, UI_HIST_ID - 1); }
    }
}

const char *ui_hist_editing(const ui_hist *h)
{
    if (!h || h->at < 0 || h->at >= h->n) return 0;
    return h->id[h->n - 1 - h->at][0] != '\0' ? h->id[h->n - 1 - h->at] : 0;
}

const char *ui_hist_last_id(const ui_hist *h)
{
    return h && h->n > 0 ? h->id[h->n - 1] : 0;
}

static void load(char *buf, int cap, int *len, int *cur, const char *text)
{
    buf[0] = '\0';
    strncat(buf, text, (size_t) (cap - 1));
    *len = (int) strlen(buf);
    *cur = *len;
}

/* Latin-1's printable half too (0xA0-0xFF): a caller's own name is typed
   with them, and the board stores them as they are. 0x80-0x9F are C1
   controls (the board's Delete and Shift+cursor bytes live there) and stay out. */
static int printable(int key)
{
    return (key >= 32 && key < 127) || (key >= 160 && key <= 255);
}

/* The plain move a Shift+cursor key makes; the key itself if it is none. */
static int unshifted(int key)
{
    if (key == UI_KEY_SHIFT_LEFT) return UI_KEY_LEFT;
    if (key == UI_KEY_SHIFT_RIGHT) return UI_KEY_RIGHT;
    if (key == UI_KEY_SHIFT_HOME) return UI_KEY_HOME;
    if (key == UI_KEY_SHIFT_END) return UI_KEY_END;
    return key;
}

int ui_line_key(char *buf, int cap, int *len, int *cur, int key, ui_hist *h, int upper)
{
    key = unshifted(key);
    if (key == UI_KEY_LEFT) { if (*cur > 0) (*cur)--; return 1; }
    if (key == UI_KEY_RIGHT) { if (*cur < *len) (*cur)++; return 1; }
    if (key == UI_KEY_HOME) { *cur = 0; return 1; }
    if (key == UI_KEY_END) { *cur = *len; return 1; }
    if (key == UI_KEY_UP && h) {
        /* An empty line that is not browsing is the caller's Up. */
        if (h->at == -1 && *len == 0) return 0;
        if (h->n == 0) return 1;
        if (h->at == -1) { h->draft[0] = '\0'; strncat(h->draft, buf, UI_HIST_TEXT - 1); }
        if (h->at < h->n - 1) { h->at++; load(buf, cap, len, cur, h->line[h->n - 1 - h->at]); }
        return 1;
    }
    if (key == UI_KEY_DOWN && h) {
        if (h->at == -1) return 0;
        h->at--;
        load(buf, cap, len, cur, h->at == -1 ? h->draft : h->line[h->n - 1 - h->at]);
        return 1;
    }
    if (key == UI_KEY_DELETE) {
        if (*cur < *len) { memmove(buf + *cur, buf + *cur + 1, (size_t) (*len - *cur)); (*len)--; }
        return 1;
    }
    if (key >= 1000) return 0;               /* any other cursor key means nothing in a line */
    if (key == 8 || key == 127) {
        if (*cur > 0) { memmove(buf + *cur - 1, buf + *cur, (size_t) (*len - *cur + 1)); (*cur)--; (*len)--; }
        return 1;
    }
    if (key == 21) { *len = 0; *cur = 0; buf[0] = '\0'; return 1; }       /* CTRL-U clears the line */
    if (printable(key) && *len < cap - 1) {
        char c = (char) key;
        if (upper && c >= 'a' && c <= 'z') c = (char) (c - 'a' + 'A');
        memmove(buf + *cur + 1, buf + *cur, (size_t) (*len - *cur + 1));
        buf[(*cur)++] = c;
        (*len)++;
        return 1;
    }
    return 0;
}

int ui_line_sel_key(char *buf, int cap, int *len, int *cur, int *anchor, int key, ui_hist *h, int upper)
{
    int r;
    if (!anchor) return ui_line_key(buf, cap, len, cur, key, h, upper);
    if (*anchor > *len) *anchor = -1;
    if (unshifted(key) != key) {
        if (*anchor < 0) *anchor = *cur;
        r = ui_line_key(buf, cap, len, cur, key, h, upper);
        if (*anchor == *cur) *anchor = -1;
        return r;
    }
    if (*anchor >= 0 && *anchor != *cur
        && (key == 8 || key == 127 || key == UI_KEY_DELETE || printable(key))) {
        int a = *anchor < *cur ? *anchor : *cur, b = *anchor < *cur ? *cur : *anchor;
        memmove(buf + a, buf + b, (size_t) (*len - b + 1));
        *len -= b - a;
        *cur = a;
        *anchor = -1;
        if (!printable(key)) return 1;
    }
    r = ui_line_key(buf, cap, len, cur, key, h, upper);
    if (r) *anchor = -1;
    return r;
}
