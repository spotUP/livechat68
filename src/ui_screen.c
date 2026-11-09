/*
 * ui_screen - see ui_screen.h.
 *
 * The only thing here with an opinion is where a frame goes: through the
 * session's transport as one write. Everything else is bookkeeping around
 * the ansi_buf.
 */

#include "ui_screen.h"
#include "ui_ansi.h"
#include "aedoor.h"
#include "ae_board_screen.h"
#include "ae_host.h"

#include <string.h>

/**
 * The sink: one finished frame, handed to the board.
 *
 * ae_session has no "write" of its own yet - phase 2's transport lift is
 * what adds it - so this is deliberately the one place that will change when
 * it does, rather than every door.
 */
/*
 * The tier for this width, plus what width cannot know: whether the caller
 * takes colour. That answer was asked once, at open, and is kept across
 * every measure. Without colour a bar is invisible, so a selected row
 * needs its caret at any width.
 */
static void apply_profile(ui_screen *sc, int cols)
{
    int colour = sc->profile.colour;

    sc->profile = ui_profile_for(cols);
    sc->profile.colour = colour;
    if (!colour) sc->profile.caret_selection = 1;
    ansi_set_cell_backgrounds(&sc->buf, sc->profile.cell_backgrounds);
    ansi_set_colour(&sc->buf, colour);
}

static void to_session(void *context, const char *bytes, long len)
{
    ae_session *s = (ae_session *)context;

    if (!s || !bytes || len <= 0) return;
    /* A door whose carrier has gone is drawing into a dropped line; the
       session already knows, and writing anyway would block on a port that
       nobody is reading. */
    if (!ae_carrier(s)) return;

    /* One composed frame, one AEDoor write - the whole reason ui_ansi
       buffers. The 0 is "no line break": the frame positions its own cursor
       and a break here would scroll what it just drew. */
    ae_put(bytes, 0);
}

int ui_screen_open(ui_screen *sc, ae_session *s, char *frame, long cap)
{
    if (!sc || !frame || cap <= 0) return -1;

    memset(sc, 0, sizeof(*sc));
    sc->session = s;
    ansi_begin(&sc->buf, frame, cap);
    /* One question, asked once: M at the prompt is not a resize. No
       session (a test, a sandbox) is a colour caller. */
    sc->profile.colour = s ? ae_user_is_ansi(s) : 1;
    /* A C64 has real line glyphs, and the board's transducer reaches them
       through DEC special graphics; nobody else is sent the switch. */
    ansi_set_line_graphics(&sc->buf, ae_host_info()->client == AE_CLIENT_PETSCII);
    /*
     * THE CALLER'S CURSOR IS NOT PART OF THE DOOR'S DRAWING.
     *
     * A door positions every cell it paints, so wherever the terminal's own
     * cursor lands is an accident of the last write - it sat blinking in the
     * middle of the art, and on a caret-driven screen it competed with the
     * door's own caret. The sysop, 2026-09-15, inside ACHIEVEC: "the cursor
     * should be disabled inside the door".
     *
     * Here rather than in each door: ui_screen_close already turns it back
     * on, so this is the other half of a pair that was only ever half
     * written, and every C door gets it without being edited. The widgets
     * that DO want a cursor - ui_input, ui_doc - turn it on for as long as
     * they own the keyboard and off again after.
     */
    ui_screen_cursor(sc, 0);
    ui_screen_measure(sc);
    return 0;
}

/*
 * The caller's REAL size (plan Decision 26), for a door that lays itself out
 * for any width. ui_screen_open asks BB_SCRWIDTH, which a board answers 80
 * for every ANSI caller because classic doors assume 80; a door that is
 * responsive asks AEW_SCREEN instead and falls back to that answer.
 */
int ui_screen_open_responsive(ui_screen *sc, ae_session *s, char *frame, long cap)
{
    int cols, rows;

    if (ui_screen_open(sc, s, frame, cap) != 0) return -1;
    sc->responsive = 1;
    if (ae_bs_size(&cols, &rows) == 1) {
        sc->cols = cols;
        sc->rows = rows;
        apply_profile(sc, cols);
    }
    return 0;
}

void ui_screen_measure(ui_screen *sc)
{
    int cols, rows;

    if (!sc) return;

    sc->cols = sc->session ? ae_screen_cols(sc->session) : AE_DEFAULT_COLS;
    sc->rows = sc->session ? ae_screen_rows(sc->session) : AE_DEFAULT_ROWS;
    /* A responsive screen keeps the real size when it measures again. */
    if (sc->responsive && ae_bs_size(&cols, &rows) == 1) {
        sc->cols = cols;
        sc->rows = rows;
    }
    /* The tier follows the size, always - a door that asked once at startup
       and then widened would draw an 80-column layout into 40 cells. */
    /* And the writer is told what this screen can paint, so every bar,
       status line and selected row gets it without asking. */
    apply_profile(sc, sc->cols);
}

void ui_screen_cursor(ui_screen *sc, int visible)
{
    if (!sc) return;
    sc->cursor_visible = visible ? 1 : 0;
    ansi_cursor(&sc->buf, sc->cursor_visible);
}

void ui_screen_cursor_reassert(ui_screen *sc)
{
    if (sc && !sc->cursor_visible) ansi_cursor(&sc->buf, 0);
}

int ui_screen_wipe_end(ui_screen *sc)
{
    int r = ae_bs_wipe_end();
    if (r == 1) ui_screen_cursor_reassert(sc);     /* the replay ran: its last frame showed the cursor */
    return r;
}

void ui_screen_flush(ui_screen *sc)
{
    if (!sc) return;
    ansi_flush(&sc->buf, to_session, sc->session);
}

void ui_screen_close(ui_screen *sc)
{
    if (!sc) return;

    /* Reset, THEN clear: a C64 has one screen background, committed at the
       last clear, so a door that leaves without one hands the board its
       last background. ansi_clear writes SGR 0 before the clear, which is
       the transducer's own rule for "back to black". */
    ansi_reset(&sc->buf);
    ansi_clear(&sc->buf);
    ui_screen_cursor(sc, 1);
    ui_screen_flush(sc);
}
