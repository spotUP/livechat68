/*
 * ae_board_screen - the client for the board's AEW_SCREEN (10800).
 *
 * One conversation any C door can have: the caller's real size, a board wipe
 * around a screen, and mouse events. A request in String, the answer in
 * String, over ae_field_exchange. The board side is
 * web/backend/src/amiga-emulation/xim/board-screen.ts.
 *
 *   SIZE            -> "cols TAB rows"
 *   WIPE IN <c>     -> "OK" (the board now holds the door's next output) | "NO"
 *   WIPE OUT <c>    -> as IN, for the reveal played in reverse
 *   WIPE END        -> "OK" (the held output has been played)
 *   MOUSE ON / OFF  -> "OK" | "NO" (not an ANSI caller)
 *   MOUSE ON B      -> as MOUSE ON, and MOUSE? adds the button as a 4th field
 *   MOUSE?          -> "kind TAB col TAB row" (1-based), "" when none queued;
 *                      after MOUSE ON B: "kind TAB col TAB row TAB button"
 *   SAVE t l r c    -> "OK" (the board holds rows t..t+r-1, columns l..l+c-1,
 *                      1-based, plus the cursor and colours) | "NO" (off the
 *                      screen, or nothing drawn yet)
 *   RESTORE         -> "OK" (the board has repainted the held region and put
 *                      the cursor and colours back, and released it) | "NO"
 *                      (nothing held)
 *
 * A board that does not know the field answers nothing: every call then says
 * "not available" and the door carries on as it did.
 */

#ifndef AE_BOARD_SCREEN_H
#define AE_BOARD_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

#define AE_BS_GONE (-1)

/** kind: P press, R release, M move, U wheel up, D wheel down. col/row are 1-based. */
/* Button of a press/release: SGR code & 3. A motion report with no button
 * held carries AE_MOUSE_NONE; a wheel event and an old board (3-field reply)
 * carry AE_MOUSE_LEFT. */
#define AE_MOUSE_LEFT   0
#define AE_MOUSE_MIDDLE 1
#define AE_MOUSE_RIGHT  2
#define AE_MOUSE_NONE   3
typedef struct { char kind; int col; int row; int button; } ae_mouse_event;

/** 1 answered, 0 silent board (outputs untouched), AE_BS_GONE. */
int ae_bs_size(int *cols, int *rows);
/** The 80x25 / responsive switch (AEW_SCREEN "WIDE ON|OFF"): remembers the call's choice and asks the terminal to
    grow or shrink. SIZE then answers the new size, a moment later for a web caller (ui_screen_wide_step watches).
    1 done, 0 silent board, AE_BS_GONE. */
int ae_bs_wide(int on);
/** The call's saved choice (AEW_SCREEN "WIDE?"): 1 wide, 0 fixed 80x25 (also a silent board), AE_BS_GONE. */
int ae_bs_wide_state(void);
/** 1 capturing (draw now, then ae_bs_wipe_end), 0 no wipe (draw normally), AE_BS_GONE.
    `conceal` 0 plays the screen in, 1 plays it out; `letter` is the board's wipe code, 0 = none. */
int ae_bs_wipe_begin(char letter, int conceal);
/** As ae_bs_wipe_begin, for the board's region slide: only screen rows top..top+rows-1 (1-based) move, the new content
    entering from the right when `forward`, from the left when not. Draw only that region, then ae_bs_wipe_end.
    1 capturing, 0 no slide (draw normally), AE_BS_GONE. */
int ae_bs_slide_begin(int forward, int top, int rows);
/** Ask the board to keep what the caller now sees in the region top..top+rows-1 by left..left+cols-1
    (1-based), so an overlay drawn over it can be taken away with ae_bs_restore. One region is held at a time;
    a new save replaces it. 1 held, 0 not (draw the old screen back yourself), AE_BS_GONE. */
int ae_bs_save(int top, int left, int rows, int cols);
/** Repaint the region held by ae_bs_save, cursor and colours included, and release it.
    1 repainted, 0 nothing held, AE_BS_GONE. */
int ae_bs_restore(void);
/** 1 played, 0 not, AE_BS_GONE. */
int ae_bs_wipe_end(void);
/** 1 on, 0 not available, AE_BS_GONE. */
int ae_bs_mouse(int on);
/** One MOUSE? round trip: 1 event, 0 none, AE_BS_GONE. */
int ae_bs_mouse_next(ae_mouse_event *ev);
/**
 * Everything queued, in order, up to `cap` events (the latest always kept:
 * once full, the last slot is overwritten). Returns the count, or AE_BS_GONE.
 *
 * Asks until the board answers empty and NEVER stops after one event: the
 * wake byte (UI_KEY_MOUSE) re-arms only after the queue has drained, so a
 * door that reads one event leaves the rest unread and is never woken again.
 */
int ae_bs_mouse_drain(ae_mouse_event *evs, int cap);

/** Pure parsers: strict, 0 on anything malformed, outputs untouched on failure. */
int ae_bs_parse_size(const char *reply, int *cols, int *rows);
int ae_bs_parse_mouse(const char *reply, ae_mouse_event *ev);

#ifdef __cplusplus
}
#endif

#endif /* AE_BOARD_SCREEN_H */
