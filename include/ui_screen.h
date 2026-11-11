/*
 * ui_screen - the door's frame, and where it goes.
 *
 * One frame, one write. Everything a door draws lands in a caller-owned
 * buffer and reaches the caller in a single ae_put - the measurement behind
 * that is in ui_ansi.h: the first version of this wrote each escape and each
 * cell separately, about a hundred round trips per frame, and redraw was
 * visibly slow on every keystroke.
 *
 * THE SCREEN OWNS THE CURSOR STATE; BOARD-DRAWN WRITES RE-ASSERT IT. The board's wipe replay ends with ESC[?25h behind the
 * door's back: ui_screen_wipe_end puts the remembered state back (see ui_screen_cursor).
 *
 * The screen owns its size, which it asks the session for once. A door that
 * redraws on a resize asks again with ui_screen_measure().
 */

#ifndef UI_SCREEN_H
#define UI_SCREEN_H

#include "ae_session.h"
#include "ui_ansi.h"
#include "ui_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int rows;
    int cols;
    ansi_buf buf;
    ae_session *session;
    /** The tier this size belongs to, refreshed with the size. */
    ui_profile profile;
    /** Set by ui_screen_open_responsive: measuring asks AEW_SCREEN, not BB_SCRWIDTH. */
    int responsive;
    /** Whether the caller's cursor is shown. THE SCREEN OWNS THE CURSOR STATE; board-drawn writes re-assert it. */
    int cursor_visible;
    /** The wide switch (ui_screen_wide_*): the choice this door last asked for, and the polls left to watch a size change settle. */
    int wide;
    int wide_watch;
    int wide_tick;
} ui_screen;

/**
 * Bind a screen to a session and a frame buffer.
 *
 * Asks the board how big the caller's terminal is, so `cols`, `rows` and
 * `profile` are usable immediately. Returns 0, or -1 when the arguments
 * cannot work. Allocates nothing.
 */
int ui_screen_open(ui_screen *sc, ae_session *s, char *frame, long cap);

/**
 * ui_screen_open for a door that lays itself out for any width.
 *
 * ui_screen_open asks BB_SCRWIDTH, which the board answers 80 for every ANSI
 * caller because classic doors assume 80. This asks AEW_SCREEN for the
 * caller's REAL size and falls back to ui_screen_open's answer when the board
 * is silent.
 */
int ui_screen_open_responsive(ui_screen *sc, ae_session *s, char *frame, long cap);

/** Ask the board for the size again - after a resize, or on a hunch. */
void ui_screen_measure(ui_screen *sc);

/*
 * The 80x25 / responsive switch for a responsive door (the port of sdk/utils/terminal-mode.ts). Three calls:
 *
 *   ui_screen_open_responsive(...);
 *   ui_screen_wide_restore(&sc);              once at start: the call's saved choice is asked of the terminal
 *   ...
 *   key = ui_key_read(&keys, &pushback);      in the input loop
 *   if (ui_screen_wide_step(&sc, &key, &pushback)) relayout();    key is 0 when Alt+Enter was eaten
 *   (and in the idle hook)  if (ui_screen_wide_step(&sc, 0, 0)) relayout();
 *
 * relayout() is the door's own: reopen what took the size at construction, keep the content, repaint.
 * ui_screen_wide_step returns 1 when sc->cols / sc->rows changed since the last call. Alt+Enter reaches the
 * door as ESC CR (UI_KEY_ESC with '\r' pushed back); the step toggles and swallows it, so a door gets no stray
 * Enter. A silent board changes nothing.
 */
void ui_screen_wide_restore(ui_screen *sc);
int ui_screen_wide_step(ui_screen *sc, int *key, int *pushback);

/**
 * Show or hide the caller's cursor, and REMEMBER which (ui_screen_open hides it).
 *
 * The screen owns the cursor state. A write the BOARD makes into the door's output path (a wipe replay, a logo) can
 * flip the terminal's cursor behind the door's back - the board's wipe replay ends its last frame with ESC[?25h - so
 * those writes go through ui_screen_wipe_end / ui_screen_cursor_reassert, which put the remembered state back.
 */
void ui_screen_cursor(ui_screen *sc, int visible);

/** Queue the remembered state again after a board-drawn write: a hidden cursor is hidden again, a visible one needs nothing. */
void ui_screen_cursor_reassert(ui_screen *sc);

/**
 * End a board wipe (ae_bs_wipe_end) and re-assert the cursor state the replay overrode. The re-assert is queued in the
 * frame buffer: the next ui_screen_flush sends it. Returns what ae_bs_wipe_end returned.
 */
int ui_screen_wipe_end(ui_screen *sc);

/** Send what has been drawn. One write, then the buffer is empty again. */
void ui_screen_flush(ui_screen *sc);

/**
 * Put the terminal back: attributes reset, cursor shown, and flushed.
 *
 * A door that skips this leaves the caller's next prompt painted in the
 * door's last colour, which reads as the BOARD being broken.
 */
void ui_screen_close(ui_screen *sc);

#ifdef __cplusplus
}
#endif

#endif /* UI_SCREEN_H */
