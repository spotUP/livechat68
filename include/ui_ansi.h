/* ansi.h - buffered ANSI screen primitives for the DoorRepo door.
 *
 * Exists so doorrepo.c can render the same full-screen layout DOORMAN (the
 * TypeScript door, Doors/door-manager/app.ts) renders with blessed, without
 * a TUI library that could not exist on a real Amiga.
 *
 * EVERYTHING IS BUFFERED, AND THAT IS THE WHOLE POINT. The first version of
 * this module wrote each escape and each cell straight to ae_put(), which
 * cost roughly a hundred XIM message round trips per frame and was visibly
 * slow to redraw on every keystroke. Now a whole frame - escapes, colours,
 * box edges, text - is composed into one caller-owned buffer and handed to
 * ae_put() in a single call. One frame, one write.
 *
 * Why ANSI is safe to assume: AmiExpress is an ANSI BBS - the catalog's own
 * NAME fields carry scene ANSI art, /X's menus are ANSI, and every terminal
 * that talks to it understands CSI sequences. A sysop with a dumb terminal
 * sets Ansi=no in DoorRepo.cfg and gets the original line-at-a-time
 * renderer instead.
 *
 * Colours match DOORMAN panel for panel: white-on-blue header and footer
 * bars, a cyan-bordered list, a blue-bordered info pane, and a
 * white-on-blue selected row.
 *
 * Box drawing is plain ASCII (+ - |), NOT the Unicode DOORMAN uses: on a
 * real Amiga the terminal is topaz-8 in a Latin-1 world, where a multi-byte
 * U+2500 renders as mojibake.
 *
 * C89. No stdint.h, no snprintf.
 *
 * LIFTED, not rewritten, from examples/doorrepo-c/ansi.h - the module a real
 * C door has been drawing with. One change: ansi_flush() no longer calls
 * ae_put() itself. A library cannot own the board connection (that is
 * ae_session's job now), so the caller says where a finished frame goes -
 * ui_screen_flush() hands it to the session, and a test hands it to a
 * buffer it can read back.
 */

#ifndef UI_ANSI_H
#define UI_ANSI_H

/* Colour codes; values are CSI foreground numbers minus 30, so a background
 * is the same value +10. */
#define ANSI_BLACK   0
#define ANSI_RED     1
#define ANSI_GREEN   2
#define ANSI_YELLOW  3
#define ANSI_BLUE    4
#define ANSI_MAGENTA 5
#define ANSI_CYAN    6
#define ANSI_WHITE   7
/**
 * The bright half. A terminal has sixteen colours, not eight: the bold bit
 * picks the second row, and a theme's ink and dim are usually one of each -
 * white against gray. Reducing both to ANSI_WHITE, which the generated table
 * did until 2026-09-07, drew the mark, the name and the blurb in one colour
 * and made six of the seven themes look like the same grey screen.
 *
 * ansi_color() takes any of these: 8 and up mean bold plus the base colour.
 */
#define ANSI_BRIGHT      8
#define ANSI_GRAY        (ANSI_BRIGHT + ANSI_BLACK)
#define ANSI_BRIGHT_RED  (ANSI_BRIGHT + ANSI_RED)
#define ANSI_BRIGHT_GREEN (ANSI_BRIGHT + ANSI_GREEN)
#define ANSI_BRIGHT_YELLOW (ANSI_BRIGHT + ANSI_YELLOW)
#define ANSI_BRIGHT_BLUE (ANSI_BRIGHT + ANSI_BLUE)
#define ANSI_BRIGHT_MAGENTA (ANSI_BRIGHT + ANSI_MAGENTA)
#define ANSI_BRIGHT_CYAN (ANSI_BRIGHT + ANSI_CYAN)
#define ANSI_BRIGHT_WHITE (ANSI_BRIGHT + ANSI_WHITE)

/* A frame under construction. `data` is caller-owned storage; `len` is how
 * much of it is used; `cap` is its size. Once `len` would exceed `cap` the
 * buffer stops accepting output and sets `overflow` rather than writing past
 * the end - a truncated frame is a cosmetic fault, a smashed stack is not. */
typedef struct {
    char *data;
    long len;
    long cap;
    int overflow;
    /* What ansi_color() last wrote, so it can skip repeating itself. A
     * captured session showed 19 of 25 colour sequences in one screen
     * paint asking for the colour already in effect. -1 means "unknown",
     * which is the state at the start of every frame and after any reset:
     * the terminal may have been changed by something else in between, so
     * the next colour is always written. */
    int last_fg;
    int last_bg;
    int last_bold;
    /** Reverse video as last emitted; -1 after a reset, so it re-emits. */
    int last_reverse;
    /** The colour state at ansi_save_cursor, handed back by ansi_restore_cursor. */
    int saved_fg;
    int saved_bg;
    int saved_bold;
    int saved_reverse;
    /**
     * A cell on this terminal may carry its own background colour.
     *
     * 1 for an ANSI terminal, 0 for a C64: the VIC-II has one screen
     * background and per-cell background is dropped on the way out
     * (sdk/petscii/ansi-to-petscii.ts). Where it is 0, ansi_color() paints
     * "ink on a coloured bar" as REVERSE VIDEO in the bar's colour, which
     * is the same picture by the only means a C64 has.
     *
     * It lives here, on the writer, rather than in each widget: a bar, a
     * status line, a box label and a selected row all have the same
     * problem, and one of them remembering to handle it is how five of the
     * seven themes ended up drawing a black bar on a black screen.
     */
    int cell_backgrounds;
    /**
     * The caller takes colour (1) or the board strips every SGR (0).
     *
     * Like cell_backgrounds it lives on the writer, so every widget that
     * draws into this buffer can see it: with 0 a selection bar, a reverse
     * thumb or an inverted headline is invisible, and the widget marks the
     * state with characters (">>", "#") instead. Default 1.
     */
    int colour;
    /**
     * The terminal draws real lines: borders and rules go out as DEC
     * special graphics (`ESC ( 0` ... `ESC ( B`) instead of `+ - |`.
     *
     * 1 only for a C64 caller (AE_CLIENT=petscii, set by ui_screen_open).
     * The board's PETSCII transducer maps the DEC letters onto the ROM's
     * own line glyphs (sdk/petscii/unicode-to-petscii.ts,
     * DEC_SPECIAL_GRAPHICS); an Amiga ANSI terminal has no such set, so
     * every other caller keeps the ASCII it always had. Default 0.
     */
    int line_graphics;
    /**
     * The theme's exact colours, or NULL for the terminal's sixteen.
     *
     * When it is set, a colour of UI_TOKEN(n) or a UI_INK marker naming a
     * token is written as `38;2;r;g;b` - the same bytes the TypeScript
     * writes, which is the only way the two implementations can put the
     * same shade on a screen. Without it every colour is one of sixteen,
     * which is what a door had before and what a plain terminal gets.
     */
    const unsigned long *palette;
    /** The same tokens as plain colours, for a terminal with no truecolour. */
    const unsigned char *palette_idx;
    int palette_len;
} ansi_buf;

/**
 * INLINE COLOUR, the C SDK's answer to a blessed tag.
 *
 * A door's row is not one colour: the TypeScript writes
 * `${s.accent('[*]')} ${s.ink(name)} ${s.dim(blurb)}` and the C wrote the
 * whole line in one pen, so every theme but the green one came out grey
 * (sysop, 2026-09-06, with a screenshot of six identical rows).
 *
 * A marker is UI_INK followed by ONE byte:
 *   '0'-'7'  that ANSI colour becomes the pen
 *   'B'/'b'  bold on / off
 *   'R'      back to the colour the caller set before the text
 *
 * Two markers take a THIRD byte:
 *   'T' <hex>   a theme token rather than a terminal colour (below)
 *   'H' '3'/'4'/'5'  this ROW is the top half / bottom half of a
 *               double-height line, or back to normal - DECDHL, which a
 *               teletext headline is made of. Written to the wire as
 *               `ESC # 3`, the escape a VT100 has always understood;
 *               terminals that do not implement it ignore it, which is why
 *               a door may write it without asking first.
 *
 * Markers cost NO columns: ansi_text pads and clips by what reaches the
 * screen, so a row with four of them still fills its width exactly. Two
 * bytes each, no parser, no allocation - which is what a 68K door can
 * afford and a tag syntax is not.
 */
#define UI_INK '\001'

/**
 * A colour that names a THEME TOKEN rather than a terminal colour.
 *
 * `ansi_color(b, UI_TOKEN(UI_T_ACCENT), ...)` writes the theme's exact
 * accent when the buffer has a palette, and the nearest of sixteen when it
 * has not. Values below this are plain ANSI colours, so every existing
 * caller keeps working unchanged.
 */
#define UI_TOKEN_BASE 100
#define UI_TOKEN(n)   (UI_TOKEN_BASE + (n))

/** The marker for one ANSI colour, into a caller's two-byte buffer. */
void ui_ink(char *out, int colour);

/**
 * The line-attribute markers, three bytes each and zero columns wide.
 *
 * A double-height row is sent TWICE - once under UI_LINE_TOP and once under
 * UI_LINE_BOTTOM - exactly as it would be to a VT100. The caller's terminal
 * draws the halves; nothing here knows how.
 */
#define UI_LINE_TOP    "\001H3"
#define UI_LINE_BOTTOM "\001H4"
#define UI_LINE_NORMAL "\001H5"

/**
 * How many COLUMNS a string reaches the screen as: its length with the
 * markers taken out.
 *
 * Anything laying text out has to measure with this rather than strlen, or
 * a coloured string is charged for bytes nobody sees. ui_footer_build did
 * exactly that the first time and dropped the "Ent: Use" hint off a
 * 40-column screen that had room for it.
 */
unsigned long ui_printable_len(const char *text);

/* Binds a buffer to caller storage and empties it. */
void ansi_begin(ansi_buf *b, char *storage, long capacity);

/* Writes the composed frame out in one ae_put() call and empties the
 * buffer. Safe to call on an empty buffer (does nothing). */
/** Where a finished frame goes: the board, or a test's buffer. */
typedef void (*ansi_sink_fn)(void *context, const char *bytes, long len);

void ansi_flush(ansi_buf *b, ansi_sink_fn sink, void *context);

/* Clears the whole screen and homes the cursor. */
void ansi_clear(ansi_buf *b);

/* Clears the whole screen to a GROUND colour and homes the cursor: the erase
 * takes the current background, so the colours are set first. A negative
 * `bg` (or a terminal whose cells carry no background) is a plain ansi_clear. */
void ansi_clear_ground(ansi_buf *b, int fg, int bg);

/* Moves the cursor. Rows and columns are 1-based, as in the escape itself. */
void ansi_goto(ansi_buf *b, int row, int col);

/* Sets foreground/background. Pass -1 for background to leave it at the
 * terminal default; `bold` brightens the foreground. */
void ansi_color(ansi_buf *b, int fg, int bg, int bold);

/* Restores the terminal's default attributes. Always emit this before
 * handing control back to the BBS. */
void ansi_reset(ansi_buf *b);

/**
 * Give the buffer the theme's exact colours (ui_theme.rgb), or NULL to go
 * back to the terminal's sixteen. `count` is UI_TOKEN_COUNT.
 */
void ansi_set_palette(ansi_buf *b, const unsigned long *rgb,
                      const unsigned char *idx, int count);

/* Say whether this terminal's cells can carry a background (see the field).
 * Call once, after ansi_begin, from whatever knows the screen. */
void ansi_set_cell_backgrounds(ansi_buf *b, int can);

/** Draw borders and rules as DEC line graphics (see ansi_buf.line_graphics). */
void ansi_set_line_graphics(ansi_buf *b, int on);

/** A horizontal rule of `count` cells at (row, col) in the current pen: a line where the terminal has one, `-` else. */
void ansi_hline(ansi_buf *b, int row, int col, int count);

/* Say whether this caller takes colour (see the field). ui_screen calls it
 * wherever it applies the profile. */
void ansi_set_colour(ansi_buf *b, int on);

/* Reverse video on or off (SGR 7 / 27).
 *
 * The highlight a C64 has. Per-cell BACKGROUND is dropped on the way to a
 * PETSCII caller - the VIC-II has one screen background - so a selected row
 * painted as a coloured bar arrives with nothing marking it at all. These
 * two reach the caller as $12 and $92 (sdk/petscii/ansi-to-petscii.ts). */
void ansi_reverse(ansi_buf *b, int on);

/* `count` cells of a bar (a scrollbar's track or thumb, a pager's position
 * line), one run: spaces on a solid `colour`, so Topaz and latin1 agree. With per-cell
 * backgrounds the cell is that colour; a C64 gets the colour as the pen with
 * reverse video on (the theme's DIM is dark, and ansi_color() only turns a
 * light background into reverse). With colour off each cell is the ASCII `plain`. */
void ansi_bar_cells(ansi_buf *b, int row, int col, int count, int colour, char plain);

/* Hides/shows the hardware cursor. */
void ansi_cursor(ansi_buf *b, int visible);

/* Puts `latin1` on the caller's clipboard: OSC 52 (ESC ] 52 ; c ; <base64> BEL),
 * the text converted to UTF-8 first, as the TypeScript door's osc52Copy sends
 * it. A terminal without OSC 52 ignores the sequence. */
void ansi_clipboard(ansi_buf *b, const char *latin1);

/**
 * Save and restore where the TERMINAL's cursor is (DECSC/DECRC, ESC 7 / 8).
 *
 * For chrome that repaints itself while a caller is typing. An animated
 * masthead rail ticks five times a second, and each repaint leaves the cursor
 * wherever that paint ended - the top right of the screen - so the caller
 * types into a field while the cursor sits in the banner. Reported against
 * REQUESTS with a screenshot (sysop, 2026-09-15: "the cursor is in the upper
 * right corner of the screen when i type in the input").
 *
 * Two bytes each on the wire, which matters on a 68K where a frame over ~198
 * bytes costs a second XIM message.
 */
void ansi_save_cursor(ansi_buf *b);

/**
 * Erase from the cursor to the end of the row (CSI 0 K, EL - standard ANSI, and the Amiga console.device has it).
 * The erased cells take the CURRENT background, so set the pen whose background the tail should have first. Three
 * bytes for what would be dozens of spaces; callers gate it on ui_profile.line_erase.
 */
void ansi_erase_to_eol(ansi_buf *b);
void ansi_restore_cursor(ansi_buf *b);

/* Writes `text` at (row, col), truncated to `maxlen` columns and padded
 * with spaces to exactly that width, so a row always overwrites whatever
 * was under it without a separate clearing pass.
 *
 * Control bytes are replaced with spaces. High-bit Latin-1 passes through
 * untouched (one byte per column), which is what carries the catalog's
 * scene art. */
void ansi_text(ansi_buf *b, int row, int col, const char *text, int maxlen);

/* Same, but does NOT pad - use where trailing cells must keep their
 * existing contents. */
void ansi_text_raw(ansi_buf *b, int row, int col, const char *text, int maxlen);

/* CSI is `ESC [`, then any number of parameter/intermediate bytes, then one
 * final byte in 0x40-0x7E. Returns its length in bytes, 0 when `p` does not
 * start one. A lone `ESC` that starts nothing is one byte and one column,
 * which is what it looks like on screen. An unterminated run is all of it. */
int ansi_escape_len(const char *p);

/* The columns `text` fills on screen: a CSI escape costs none. The board's
 * own prompt carries its colours (express.e:28417), so its length is not
 * strlen. */
int ansi_text_cols(const char *text);

/* The bytes of `text` that fill at most `cols` columns, escapes inside the
 * cut kept whole - so a clipped coloured string never ends half an escape. */
int ansi_text_bytes(const char *text, int cols);

/* Fills `width` columns at (row, col) with spaces in the given colours. */
void ansi_fill(ansi_buf *b, int row, int col, int width, int fg, int bg);

/* Draws a single-line ASCII box. `label`, when non-NULL and non-empty, is
 * written into the top edge as " label ", where blessed puts a panel label. */
void ansi_box(ansi_buf *b, int top, int left, int height, int width, int fg, const char *label);

/* The same box without its top edge: the sides and the bottom. For a box whose
 * top line is something else (the search palette's rail), so that row is
 * written once, not drawn as an edge and then written over. */
void ansi_box_open(ansi_buf *b, int top, int left, int height, int width, int fg);

/* ansi_box that paints its interior first, so a dialog drawn over the
 * browser hides what is behind it instead of letting it read through. */
void ansi_panel(ansi_buf *b, int top, int left, int height, int width,
                int fg, int bg, const char *label);

/* Writes `text` centred within `width` columns starting at (row, col). */
void ansi_center(ansi_buf *b, int row, int col, int width, const char *text);

#endif /* UI_ANSI_H */
