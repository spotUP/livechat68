/* ansi.c - see ansi.h for the interface contract and the reasoning behind
 * buffering, ASCII box drawing, and the DOORMAN-matched palette.
 *
 * C89: no snprintf, so integers are formatted by hand. Every append goes
 * through put_char()/put_str(), which are the only places that touch the
 * buffer bounds - so the overflow check exists once rather than at each of
 * the dozen call sites.
 */

#include <string.h>
#include "ui_ansi.h"

static void put_char(ansi_buf *b, char c)
{
    if (b->overflow) {
        return;
    }
    if (b->len + 1 >= b->cap) {
        b->overflow = 1;
        return;
    }
    b->data[b->len++] = c;
}

static void put_str(ansi_buf *b, const char *s)
{
    while (*s != '\0') {
        put_char(b, *s++);
    }
}

/* Appends a non-negative int in decimal. Values here are screen coordinates
 * and colour codes; a negative one would be a programming error and is
 * clamped to 0 rather than emitting a '-' that would corrupt the escape
 * sequence and everything the terminal read after it. */
static void put_int(ansi_buf *b, int value)
{
    char tmp[12];
    int i;

    if (value < 0) {
        value = 0;
    }
    i = 0;
    if (value == 0) {
        tmp[i++] = '0';
    }
    while (value > 0) {
        tmp[i++] = (char) ('0' + (value % 10));
        value /= 10;
    }
    while (i > 0) {
        put_char(b, tmp[--i]);
    }
}

void ansi_begin(ansi_buf *b, char *storage, long capacity)
{
    b->data = storage;
    b->len = 0;
    b->cap = capacity;
    b->overflow = 0;
    /* Nothing is known about the terminal's colour at the start of a frame:
     * the BBS may have written between this frame and the last. */
    b->last_fg = -1;
    b->last_bg = -1;
    b->last_bold = -1;
    /* Not -1: the frame opens with SGR 0, which clears reverse, so it is
       KNOWN to be off. Leaving it unknown put a redundant ESC[27m at the
       head of every 80-column frame. */
    b->last_reverse = 0;
    b->saved_fg = -1;
    b->saved_bg = -1;
    b->saved_bold = -1;
    b->saved_reverse = -1;
    /* An ANSI terminal until something says otherwise, which is what every
       existing caller has always been. */
    b->cell_backgrounds = 1;
    b->colour = 1;
    b->line_graphics = 0;
    b->palette = 0;
    b->palette_idx = 0;
    b->palette_len = 0;
}

void ansi_flush(ansi_buf *b, ansi_sink_fn sink, void *context)
{
    if (b->len <= 0) {
        return;
    }
    b->data[b->len] = '\0';
    /* Whoever owns the connection writes it. In doorrepo-c this called
       ae_put() directly; a library cannot, because the session is the
       caller's (include/ae_session.h) and a test needs somewhere else for
       the bytes to go. */
    if (sink) sink(context, b->data, b->len);
    b->len = 0;
    b->overflow = 0;
}

void ansi_clear(ansi_buf *b)
{
    /* The reset is not decoration: ESC[2J erases using the CURRENT
     * background colour, so clearing while a coloured bar's attributes are
     * still set paints the WHOLE screen in that colour. Reported from the
     * live BBS as "answer N at the install prompt and the background goes
     * blue" - the confirm bar is white-on-blue, and the redraw that followed
     * inherited it. Prompts restore their own colours too (that is where the
     * ownership belongs), but clearing is the operation that turns a stray
     * attribute into a full-screen one, so it defends itself. */
    put_str(b, "\033[0m\033[2J\033[H");
    b->last_fg = -1;
    b->last_bg = -1;
    b->last_bold = -1;
    /* Not -1: the frame opens with SGR 0, which clears reverse, so it is
       KNOWN to be off. Leaving it unknown put a redundant ESC[27m at the
       head of every 80-column frame. */
    b->last_reverse = 0;
}

void ansi_clear_ground(ansi_buf *b, int fg, int bg)
{
    if (bg < 0 || !b->cell_backgrounds) { ansi_clear(b); return; }
    /* Reset, paint the ground, clear once: ESC[2J fills with the current background, so the old
       clear-reset-colour-clear sent a second screen-wide erase for nothing. */
    ansi_reset(b);
    ansi_color(b, fg, bg, 0);
    put_str(b, "\033[2J\033[H");
}

void ansi_goto(ansi_buf *b, int row, int col)
{
    put_str(b, "\033[");
    put_int(b, row);
    put_char(b, ';');
    put_int(b, col);
    put_char(b, 'H');
}

/** The exact colour for a token, or -1 for "not one, or no palette". */
static long token_rgb(const ansi_buf *b, int colour)
{
    int n = colour - UI_TOKEN_BASE;

    if (colour < UI_TOKEN_BASE) return -1;
    if (!b->palette || n < 0 || n >= b->palette_len) return -1;
    return (long) b->palette[n];
}

static void put_rgb(ansi_buf *b, int lead, unsigned long rgb)
{
    put_int(b, lead);
    put_str(b, ";2;");
    put_int(b, (int) ((rgb >> 16) & 0xff));
    put_char(b, ';');
    put_int(b, (int) ((rgb >> 8) & 0xff));
    put_char(b, ';');
    put_int(b, (int) (rgb & 0xff));
}

/* A background, as one SGR parameter. Black goes out as 49, the terminal's
   own default, never as 40 or 48;2;0;0;0: macOS Terminal draws the default
   transparent, so an explicit black cell shows as a black block beside it.
   The TypeScript SDK's bgCode makes the same choice. */
static void put_bg(ansi_buf *b, int bg, long rgb)
{
    if (rgb == 0 || (rgb < 0 && bg == ANSI_BLACK)) put_int(b, 49);
    else if (rgb > 0) put_rgb(b, 48, (unsigned long) rgb);
    else put_int(b, 40 + bg);
}

void ansi_color(ansi_buf *b, int fg, int bg, int bold)
{
    int pen;
    int want_reverse = 0;
    /* What the CALLER asked for, which is what the cache remembers. Two
       tokens can share a sixteen-colour fallback and still be different
       colours - quiet-phosphor's accent and ink both fall back to 10 - so
       comparing the collapsed value made the second one a no-op and the
       masthead's title came out in the rail's colour. */
    int want_fg = fg;
    int want_bg = bg;

    long fg_rgb = token_rgb(b, fg);
    long bg_rgb = token_rgb(b, bg);

    /* A token with no truecolour behind it still has to be a colour: the
       theme's own nearest-of-sixteen for that token. Without this a door
       that named a token on a plain terminal asked for colour 106. */
    if (fg >= UI_TOKEN_BASE) {
        int n = fg - UI_TOKEN_BASE;
        fg = (b->palette_idx && n < b->palette_len) ? b->palette_idx[n] : ANSI_WHITE;
    }
    if (bg >= UI_TOKEN_BASE) {
        int n = bg - UI_TOKEN_BASE;
        bg = (b->palette_idx && n < b->palette_len) ? b->palette_idx[n] : ANSI_BLACK;
    }

    /* 8 and up is the bright half of the terminal's sixteen: bold, plus the
       base colour. A caller passes a theme token straight in and does not
       have to know which half it landed in.
       Only where the colour is a NUMBER, though: with the exact RGB going
       out there is nothing for bold to brighten, and asking for it anyway
       moved the C64's rendering of that cell onto a different VIC entry
       than the TypeScript's. */
    if (fg >= ANSI_BRIGHT && fg < UI_TOKEN_BASE) { bold = 1; fg -= ANSI_BRIGHT; }
    else if (fg_rgb >= 0) { bold = 0; }
    int bg_bright = bg >= ANSI_BRIGHT && bg < UI_TOKEN_BASE;
    if (bg_bright) { bg -= ANSI_BRIGHT; }
    pen = fg;

    /* Already showing exactly this? Then the sequence is bytes for
     * nothing - and on this door bytes are milliseconds, because every
     * 198 of them is an XIM message costing about 45ms of 68K emulation. */
    if (b->last_fg == want_fg && b->last_bg == want_bg
        && b->last_bold == (bold ? 1 : 0)) {
        return;
    }
    {
    /* ONLY WHAT CHANGED. SGR 0 resets the background too, so the full form
       has to restate it; but when the terminal's colour is KNOWN (every cache
       field >= 0) and we keep a background, the change is just what moved:
       the pen, the background, and bold as SGR 1 / 22. A themed row used to write
       `38;2;r;g;b;48;2;r;g;b` for every pen change - the same ground, again
       and again (THEMEC's live preview was 4.5 KB a move). An unknown cache
       (after a reset, a clear, a palette swap) falls through to the whole
       sequence. */
    int partial = b->cell_backgrounds && b->last_fg >= 0 && b->last_bg >= 0
                  && b->last_bold >= 0
                  && want_bg >= 0 && !want_reverse && b->last_reverse == 0;
    int bold_moved = b->last_bold != (bold ? 1 : 0);
    int fg_moved = b->last_fg != want_fg || bold_moved;   /* bold applies to the pen */
    int bg_moved = b->last_bg != want_bg;
    int lead = 0;

    b->last_fg = want_fg;
    b->last_bg = want_bg;
    b->last_bold = bold ? 1 : 0;
    if (partial) {
        put_str(b, "\033[");
        /* SGR 1 / 22 switch intensity without touching the background (SGR 0
           would, which is why the whole form restates it). */
        if (bold_moved) { put_str(b, bold ? "1" : "22"); lead = 1; }
        if (fg_moved) {
            if (lead) put_char(b, ';');
            if (fg_rgb >= 0) put_rgb(b, 38, (unsigned long) fg_rgb);
            else put_int(b, 30 + pen);
        }
        if (bg_moved) {
            if (fg_moved || lead) put_char(b, ';');
            put_bg(b, bg, bg_rgb);
        }
        put_char(b, 'm');
        return;
    }
    }

    /* INK ON A BAR, WHERE THERE ARE NO BACKGROUNDS.
     *
     * A C64 cell cannot carry one, so a caller asking for dark ink on a
     * bright bar would get the ink and nothing else - black on black under
     * five of the seven themes. Reverse video in the BAR's colour is the
     * same picture: the cell is painted in the pen and the glyph shows
     * through in the screen's background.
     *
     * A background OF the screen colour is not a bar at all, so it asks for
     * no reverse and the ink is the pen, exactly as before. */
    if (!b->cell_backgrounds) {
        if (bg > ANSI_BLACK) {
            /* The pen IS the bar here, in every form it goes out in: the
               exact colour when there is a palette, else the sixteen. */
            pen = bg;
            fg_rgb = bg_rgb;
            bold = bg_rgb >= 0 ? 0 : bg_bright;
            want_reverse = 1;
        }
    }

    if (b->last_reverse != want_reverse) {
        b->last_reverse = want_reverse;
        put_str(b, want_reverse ? "\033[7m" : "\033[27m");
    }

    put_str(b, "\033[");
    put_int(b, bold ? 1 : 0);
    /* The leading 0 is SGR 0, and SGR 0 resets EVERY attribute - reverse
       video with them. So a colour written while the buffer is in reverse
       has to put it back in the same sequence, or the reverse lasts exactly
       until the next colour change. */
    if (!bold && b->last_reverse == 1) {
        put_str(b, ";7");
    }
    put_char(b, ';');
    /* THE EXACT COLOUR when the buffer has the theme's palette, and the
       nearest of sixteen when it has not. `38;2;r;g;b` is what the
       TypeScript writes, so the two implementations put the same shade on
       the same screen; a PETSCII caller gets the nearest VIC either way,
       because the transducer reduces truecolour on its way out. */
    if (fg_rgb >= 0) {
        put_rgb(b, 38, (unsigned long) fg_rgb);
    } else {
        put_int(b, 30 + pen);
    }
    /* Only where a cell can hold one. Asking on a C64 is what gets dropped. */
    if (b->cell_backgrounds && (bg_rgb >= 0 || bg >= 0)) {
        put_char(b, ';');
        put_bg(b, bg, bg_rgb);
    }
    put_char(b, 'm');
}

void ansi_set_palette(ansi_buf *b, const unsigned long *rgb,
                      const unsigned char *idx, int count)
{
    if (!b) return;
    b->palette = rgb;
    b->palette_idx = idx;
    b->palette_len = (rgb || idx) ? count : 0;
    /* The colour on screen was written in the old palette's terms. */
    b->last_fg = -1;
    b->last_bg = -1;
    b->last_bold = -1;
}

void ansi_set_cell_backgrounds(ansi_buf *b, int can)
{
    if (!b) return;
    b->cell_backgrounds = can ? 1 : 0;
}

void ansi_set_line_graphics(ansi_buf *b, int on)
{
    if (!b) return;
    b->line_graphics = on ? 1 : 0;
}

void ansi_set_colour(ansi_buf *b, int on)
{
    if (!b) return;
    b->colour = on ? 1 : 0;
}

void ansi_reverse(ansi_buf *b, int on)
{
    if (!b) return;
    if (b->last_reverse == (on ? 1 : 0)) return;
    b->last_reverse = on ? 1 : 0;
    /* The colour cache is now stale in one direction: the NEXT ansi_color
       may ask for exactly what was last written, return early, and never
       re-assert reverse in its SGR 0. Forget the last colour so it writes
       itself out. */
    b->last_bold = -1;
    put_str(b, on ? "\033[7m" : "\033[27m");
}

void ansi_bar_cells(ansi_buf *b, int row, int col, int count, int colour, char plain)
{
    char run[256];
    if (!b || count < 1) return;
    if (count > (int)sizeof run - 1) count = (int)sizeof run - 1;
    memset(run, b->colour ? ' ' : plain, (size_t)count);
    run[count] = '\0';
    if (!b->colour) {
        ansi_text_raw(b, row, col, run, count);
    } else if (b->cell_backgrounds) {
        ansi_color(b, colour, colour, 0);
        ansi_text_raw(b, row, col, run, count);
    } else {
        ansi_color(b, colour, ANSI_BLACK, 0);
        ansi_reverse(b, 1);
        ansi_text_raw(b, row, col, run, count);
        ansi_reverse(b, 0);
    }
}

void ansi_reset(ansi_buf *b)
{
    put_str(b, "\033[0m");
    /* The terminal is back to defaults; nothing is known about its colour
     * any more, so the next ansi_color() must write itself out. */
    b->last_fg = -1;
    b->last_bg = -1;
    b->last_bold = -1;
    /* Not -1: the frame opens with SGR 0, which clears reverse, so it is
       KNOWN to be off. Leaving it unknown put a redundant ESC[27m at the
       head of every 80-column frame. */
    b->last_reverse = 0;
}

void ansi_cursor(ansi_buf *b, int visible)
{
    put_str(b, visible ? "\033[?25h" : "\033[?25l");
}

static void put_b64(ansi_buf *b, const unsigned char *in, int n)
{
    static const char abc[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned long v = 0;
    int i;
    for (i = 0; i < 3; i++) v = (v << 8) | (i < n ? in[i] : 0);
    put_char(b, abc[(v >> 18) & 63]);
    put_char(b, abc[(v >> 12) & 63]);
    put_char(b, n > 1 ? abc[(v >> 6) & 63] : '=');
    put_char(b, n > 2 ? abc[v & 63] : '=');
}

void ansi_clipboard(ansi_buf *b, const char *latin1)
{
    unsigned char group[3];
    int have = 0;
    const unsigned char *p = (const unsigned char *)latin1;
    put_str(b, "\033]52;c;");
    for (; *p; p++) {
        unsigned char utf[2];
        int n = 0, i;
        if (*p < 0x80) utf[n++] = *p;
        else { utf[n++] = (unsigned char)(0xC0 | (*p >> 6)); utf[n++] = (unsigned char)(0x80 | (*p & 0x3F)); }
        for (i = 0; i < n; i++) {
            group[have++] = utf[i];
            if (have == 3) { put_b64(b, group, 3); have = 0; }
        }
    }
    if (have) put_b64(b, group, have);
    put_char(b, '\007');
}

/* DECSC saves the cursor AND the terminal's colour/reverse state; DECRC puts
   all of it back. The colour cache has to follow, or it describes a colour
   the terminal no longer shows: the next ansi_color() asking for the colour
   written inside the save..restore span is judged redundant and skipped, and
   the text lands in the caller's colour (the masthead rail went grey on every
   idle tick after the first). */
void ansi_save_cursor(ansi_buf *b)
{
    b->saved_fg = b->last_fg;
    b->saved_bg = b->last_bg;
    b->saved_bold = b->last_bold;
    b->saved_reverse = b->last_reverse;
    put_str(b, "\0337");
}

void ansi_erase_to_eol(ansi_buf *b)
{
    put_str(b, "\033[K");
}

void ansi_restore_cursor(ansi_buf *b)
{
    b->last_fg = b->saved_fg;
    b->last_bg = b->saved_bg;
    b->last_bold = b->saved_bold;
    b->last_reverse = b->saved_reverse;
    put_str(b, "\0338");
}

/* Does the SGR whose parameters run [p, end) leave the default background
 * behind? A reset (`0`, or no parameters at all) and `49` do; one that names
 * its own background does not - that is the caller's colour, and it stays. */
static int sgr_drops_ground(const char *p, const char *end)
{
    int v[16], n = 0, i, drops = (p == end);

    while (p <= end && n < 16) {
        int x = 0;
        while (p < end && *p >= '0' && *p <= '9') x = x * 10 + (*p++ - '0');
        v[n++] = x;                  /* an empty parameter is 0, which is a reset */
        if (p >= end) break;
        p++;                         /* ';' or ':' */
    }
    for (i = 0; i < n; i++) {
        if (v[i] == 38) { i += (i + 1 < n && v[i + 1] == 5) ? 2 : 4; continue; }   /* 38;5;n / 38;2;r;g;b */
        if (v[i] == 0 || v[i] == 49) drops = 1;
        else if ((v[i] >= 40 && v[i] <= 48) || (v[i] >= 100 && v[i] <= 107)) return 0;
    }
    return drops;
}

/* The background alone, as ansi_color would write it. */
static void put_ground(ansi_buf *b, int bg)
{
    long rgb = token_rgb(b, bg);

    if (bg >= UI_TOKEN_BASE) {
        int n = bg - UI_TOKEN_BASE;
        bg = (b->palette_idx && n < b->palette_len) ? b->palette_idx[n] : ANSI_BLACK;
    }
    if (bg >= ANSI_BRIGHT && bg < UI_TOKEN_BASE) bg -= ANSI_BRIGHT;
    put_str(b, "\033[");
    put_bg(b, bg, rgb);
    put_char(b, 'm');
}

int ansi_escape_len(const char *p)
{
    int n;

    if (p[0] != 0x1b || p[1] != '[') return 0;
    n = 2;
    while (p[n] && ((unsigned char)p[n] < 0x40 || (unsigned char)p[n] > 0x7e)) n++;
    return p[n] ? n + 1 : n;
}

int ansi_text_cols(const char *text)
{
    int cols = 0, esc;

    while (*text) {
        if ((esc = ansi_escape_len(text)) > 0) { text += esc; continue; }
        cols++;
        text++;
    }
    return cols;
}

int ansi_text_bytes(const char *text, int cols)
{
    int at = 0, used = 0, esc;

    while (text[at]) {
        if ((esc = ansi_escape_len(text + at)) > 0) { at += esc; continue; }
        if (used == cols) break;
        used++;
        at++;
    }
    return at;
}

static void write_text(ansi_buf *b, int row, int col, const char *text, int maxlen, int pad)
{
    int n;            /* columns written - markers do not count */
    const char *at = text;
    int base_fg = b->last_fg;
    int base_bg = b->last_bg;
    int base_bold = b->last_bold;

    if (maxlen <= 0) {
        return;
    }
    ansi_goto(b, row, col);
    n = 0;
    while (*at != '\0' && n < maxlen) {
        char c = *at;

        /* A pen change, and it costs the row no columns (ui_ansi.h). */
        if (c == UI_INK && at[1] != '\0') {
            char what = at[1];
            if (what == 'H' && at[2] != '\0') {
                /* A LINE attribute, not a pen: `ESC # 3` says the row the
                   cursor is on is the top half of a double-height line.
                   Written through raw, because it is the terminal's own
                   escape and costs the row no columns - a teletext headline
                   is the same forty characters, drawn twice. */
                put_char(b, 27);
                put_char(b, '#');
                put_char(b, at[2]);
                at += 3;
                continue;
            }
            if (what == 'T' && at[2] != '\0') {
                /* A token: the palette decides what colour that is. */
                char d = at[2];
                int n = (d >= '0' && d <= '9') ? d - '0' : 10 + (d - 'a');
                ansi_color(b, UI_TOKEN(n), base_bg, b->last_bold);
                at += 3;
                continue;
            }
            if (what >= '0' && what <= '9') {
                ansi_color(b, what - '0', base_bg, b->last_bold);
            } else if (what >= 'a' && what <= 'f') {
                /* The bright half; ansi_color turns it into bold + base. */
                ansi_color(b, 10 + (what - 'a'), base_bg, b->last_bold);
            } else if (what == 'B') {
                ansi_color(b, b->last_fg, base_bg, 1);
            } else if (what == 'b') {
                ansi_color(b, b->last_fg, base_bg, 0);
            } else if (what == 'R') {
                ansi_color(b, base_fg, base_bg, base_bold);
            }
            /* The colour moved the cursor nowhere, but ansi_color wrote an
               escape into the stream - so the next glyph still lands where
               it should. */
            at += 2;
            continue;
        }

        /* A CALLER'S OWN COLOUR goes through as colour. A message body, a
         * bulletin, the Conftop results are raw ANSI, and this turned their
         * ESC into a space, so `ESC[32m` reached the screen as ` [32m`
         * text. The board then patched a bare `[32m` back into colour one
         * XIM message at a time - and where a message boundary fell between
         * the `[` and the `32m` the patch missed and the caller read
         * `[32m17.` on the last row of a Conftop page (SUBS, 2026-09-28).
         * An SGR moves no cursor and costs no columns (ui_doc_wrap already
         * counts it so), so it is safe here; the colour it leaves is not
         * the cached one, so the cache is forgotten. Any other sequence
         * could move the cursor, and is dropped whole rather than printed. */
        if (c == 27 && at[1] == '[') {
            const char *q = at + 2;
            while (*q && ((unsigned char)*q < 0x40 || (unsigned char)*q > 0x7e)) q++;
            if (*q == 'm') {
                const char *p;
                for (p = at; p <= q; p++) put_char(b, *p);
                b->last_fg = -1;
                b->last_bg = -1;
                b->last_bold = -1;
                /* A reset takes the row's ground with it: the board prompt
                   opens with `ESC[0m`, and on a themed row that punched the
                   terminal's own black under every coloured word. The
                   caller's colour stays; the ground comes back. */
                if (sgr_drops_ground(at + 2, q) && b->cell_backgrounds && base_bg >= 0) put_ground(b, base_bg);
            }
            at = *q ? q + 1 : q;
            continue;
        }

        /* Control bytes become spaces: high-bit Latin-1 art passes through
         * one byte per column, but a raw ESC or CR from the server would
         * move the cursor and corrupt the entire layout. A line-at-a-time
         * renderer can forward those; a cursor-addressed one cannot. */
        put_char(b, (c >= 0 && c < 32) ? ' ' : c);
        n++;
        at++;
    }
    if (pad && n < maxlen) {
        /* Padding is the CALLER's colour, not whatever the last marker left
           behind: a row that ends mid-accent would trail a coloured bar.
           A row the text already filled has no padding, and a pen sent for
           none is 19 bytes of nothing (the palette's rail ends in its
           title at the box's last column). */
        ansi_color(b, base_fg, base_bg, base_bold);
        while (n < maxlen) {
            put_char(b, ' ');
            n++;
        }
    }
}

unsigned long ui_printable_len(const char *text)
{
    unsigned long n = 0;

    if (!text) return 0;
    while (*text) {
        if (*text == UI_INK && text[1]) {
            /* Two bytes for a colour, three for a token or a line attribute. */
            text += ((text[1] == 'T' || text[1] == 'H') && text[2]) ? 3 : 2;
            continue;
        }
        n++;
        text++;
    }
    return n;
}

void ui_ink(char *out, int colour)
{
    int c;

    if (!out) return;

    /* A THEME TOKEN, which the palette turns into the exact colour: three
       bytes, `UI_INK 'T' <hex>`. A door with a palette wants these, because
       the sixteen-colour form can only be the nearest shade. */
    if (colour >= UI_TOKEN_BASE) {
        int n = (colour - UI_TOKEN_BASE) & 15;
        out[0] = UI_INK;
        out[1] = 'T';
        out[2] = (char) (n < 10 ? '0' + n : 'a' + (n - 10));
        out[3] = '\0';
        return;
    }

    c = colour & 15;
    out[0] = UI_INK;
    /* A hex digit, so the bright half fits: '0'-'7' are the base colours and
       '8'-'f' their bold twins. */
    out[1] = (char) (c < 10 ? '0' + c : 'a' + (c - 10));
    out[2] = '\0';
}

void ansi_text(ansi_buf *b, int row, int col, const char *text, int maxlen)
{
    write_text(b, row, col, text, maxlen, 1);
}

void ansi_text_raw(ansi_buf *b, int row, int col, const char *text, int maxlen)
{
    write_text(b, row, col, text, maxlen, 0);
}

void ansi_fill(ansi_buf *b, int row, int col, int width, int fg, int bg)
{
    int i;

    if (width <= 0) {
        return;
    }
    ansi_color(b, fg, bg, 0);
    ansi_goto(b, row, col);
    for (i = 0; i < width; i++) {
        put_char(b, ' ');
    }
}

/* A box that HIDES what is behind it.
 *
 * ansi_box() draws a frame and nothing else, which is right on a screen
 * that was just cleared and wrong for a dialog: the "Not installed" notice
 * opened over the browser and the archive list and the ANSI art behind it
 * read straight through the middle of it (screenshot, 2026-08-31).
 *
 * Painting the rectangle first is the whole difference. Every overlay wants
 * it, so it lives here rather than as a fill loop copied into each caller -
 * and a caller that draws on a cleared screen keeps using ansi_box, which
 * writes fewer bytes down a modem line.
 */
void ansi_panel(ansi_buf *b, int top, int left, int height, int width,
                int fg, int bg, const char *label)
{
    int row;

    if (width < 2 || height < 2) {
        return;
    }
    for (row = 0; row < height; row++) {
        ansi_fill(b, top + row, left, width, fg, bg);
    }
    ansi_box(b, top, left, height, width, fg, label);
}

/*
 * ONE PLACE PICKS A LINE GLYPH.
 *
 * `dec` is the DEC special graphics letter for the piece (q across, x down,
 * l k m j the corners), `ascii` what an Amiga ANSI terminal gets instead.
 * On a line-graphics terminal the first piece of a run selects the DEC set
 * and line_end() releases it, so a run of rule costs six bytes of switching
 * however long it is. A cursor move or a pen change between pieces does not
 * touch G0, so a whole frame is one run.
 */
static void put_line(ansi_buf *b, char dec, char ascii, int *in_dec)
{
    if (!b->line_graphics) {
        put_char(b, ascii);
        return;
    }
    if (!*in_dec) {
        put_str(b, "\033(0");
        *in_dec = 1;
    }
    put_char(b, dec);
}

static void line_end(ansi_buf *b, int *in_dec)
{
    if (*in_dec) {
        put_str(b, "\033(B");
        *in_dec = 0;
    }
}

void ansi_hline(ansi_buf *b, int row, int col, int count)
{
    int i;
    int in_dec = 0;

    if (!b || count <= 0) return;
    ansi_goto(b, row, col);
    for (i = 0; i < count; i++) {
        put_line(b, 'q', '-', &in_dec);
    }
    line_end(b, &in_dec);
}

void ansi_box(ansi_buf *b, int top, int left, int height, int width, int fg, const char *label)
{
    int in_dec = 0;
    int i;
    int inner;
    int labellen;

    if (width < 2 || height < 2) {
        return;
    }
    inner = width - 2;
    labellen = 0;
    if (label != (const char *) 0) {
        labellen = (int) strlen(label);
        if (labellen > inner - 3) {
            labellen = inner - 3;
        }
        if (labellen < 0) {
            labellen = 0;
        }
    }

    ansi_color(b, fg, -1, 0);

    /* Top edge, with the label written into it the way blessed does. */
    ansi_goto(b, top, left);
    put_line(b, 'l', '+', &in_dec);
    if (labellen > 0) {
        put_line(b, 'q', '-', &in_dec);
        line_end(b, &in_dec);
        put_char(b, ' ');
        for (i = 0; i < labellen; i++) {
            put_char(b, label[i]);
        }
        put_char(b, ' ');
        for (i = labellen + 3; i < inner; i++) {
            put_line(b, 'q', '-', &in_dec);
        }
    } else {
        for (i = 0; i < inner; i++) {
            put_line(b, 'q', '-', &in_dec);
        }
    }
    put_line(b, 'k', '+', &in_dec);
    line_end(b, &in_dec);
    ansi_box_open(b, top, left, height, width, fg);
}

void ansi_box_open(ansi_buf *b, int top, int left, int height, int width, int fg)
{
    int i;
    int inner;
    int in_dec = 0;

    if (width < 2 || height < 2) {
        return;
    }
    inner = width - 2;
    ansi_color(b, fg, -1, 0);

    /* Sides only - the interior is written by whoever owns it, so redrawing
     * the frame never erases the content inside it. */
    for (i = 1; i < height - 1; i++) {
        ansi_goto(b, top + i, left);
        put_line(b, 'x', '|', &in_dec);
        ansi_goto(b, top + i, left + width - 1);
        put_line(b, 'x', '|', &in_dec);
    }

    ansi_goto(b, top + height - 1, left);
    put_line(b, 'm', '+', &in_dec);
    for (i = 0; i < inner; i++) {
        put_line(b, 'q', '-', &in_dec);
    }
    put_line(b, 'j', '+', &in_dec);
    line_end(b, &in_dec);
}

void ansi_center(ansi_buf *b, int row, int col, int width, const char *text)
{
    int len;
    int pad;

    len = (int) strlen(text);
    if (len > width) {
        len = width;
    }
    pad = (width - len) / 2;
    ansi_text_raw(b, row, col + pad, text, len);
}
