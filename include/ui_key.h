/*
 * ui_key - one keystroke, decoded.
 *
 * LIFTED from examples/doorrepo-c/flow.h and flow.c, where this exact
 * decoder has been reading arrows in a real C door. The reasoning below is
 * that file's, unchanged, because the thing it explains cost six debugging
 * rounds and is not obvious from the code.
 *
* ---- Key decoding: the ESC problem -------------------------------------
 *
 * Cursor keys arrive as ESC [ A and friends, one byte per read, and a lone
 * ESC is the natural "back" key. Without a clock the two share a first byte
 * and nothing else: DOORMAN spent six debugging rounds on that ambiguity
 * (handoff.md, 2026-08-17), and DoorRepo's answer was to have NO ESC
 * binding and read the byte after an ESC blocking - so a user's ESC swallowed
 * whatever they pressed next and delivered THAT instead. ESC then Q from a
 * sub-screen went straight out of the door ("q quits doorrepo and it's a
 * stupid button to use for back out - use ESC!", the sysop, 2026-09-01).
 *
 * The clock is the door layer's non-consuming probe (ae_input_pending):
 * settle for a moment, and if nothing is queued the ESC was alone. A byte
 * that turns out not to belong to a sequence is handed BACK, never eaten.
 *
 * The values are what doorrepo.c's UI_KEY_* aliases have always been. */

#ifndef UI_KEY_H
#define UI_KEY_H

#ifdef __cplusplus
extern "C" {
#endif

/* The bytes AmiExpress hands a door for the cursor keys.
 *
 * The board converts them before the door is called (express.e:7514-7528,
 * ported in web/backend/src/amiga-emulation/xim/io.ts): a door that waits
 * for ESC [ A will wait for ever on a real board, because the escape
 * sequence never arrives. */
#define AE_ARROW_LEFT  2
#define AE_ARROW_RIGHT 3
#define AE_ARROW_UP    4
#define AE_ARROW_DOWN  5

/* Home, End, Page Up and Page Down arrive the same way: one byte, converted by
 * the board (web/backend/src/amiga-emulation/xim/types.ts NavigationKeyCodes -
 * keep both in step). express.e has no code for them, so these are the
 * board's own control bytes. */
#define AE_KEY_HOME 0x1c
#define AE_KEY_END  0x1d
#define AE_KEY_PGUP 0x1f
#define AE_KEY_PGDN 0x19

/* Delete and the Shift+cursor keys (text selection in a line editor). The
 * control range has no free bytes left, so the board uses C1 bytes 0x80-0x84:
 * the Amiga font draws nothing there and ui_line_key accepts printable text
 * only from 32-126 and 160-255. SDK doors only, like Home/End above. */
#define AE_KEY_DELETE      0x80
#define AE_KEY_SHIFT_LEFT  0x81
#define AE_KEY_SHIFT_RIGHT 0x82
#define AE_KEY_SHIFT_HOME  0x83
#define AE_KEY_SHIFT_END   0x84
/* F5-F7 (ESC [ 1 5 ~, 1 7 ~, 1 8 ~): the board drops other CSI function keys, so these get C1 bytes too. */
#define AE_KEY_F5          0x85
#define AE_KEY_F6          0x86
#define AE_KEY_F7          0x87
/* F1-F4 followed (the board maps every F1-F7 terminal spelling to these bytes). */
#define AE_KEY_F1          0x88
#define AE_KEY_F2          0x89
#define AE_KEY_F3          0x8a
#define AE_KEY_F4          0x8b

#define UI_KEY_UP    1000
#define UI_KEY_DOWN  1001
#define UI_KEY_PGUP  1002
#define UI_KEY_PGDN  1003
#define UI_KEY_HOME  1004
#define UI_KEY_END   1005
#define UI_KEY_ENTER 1006
#define UI_KEY_ESC   1007
/*
 * LEFT and RIGHT arrive as themselves now.
 *
 * They used to be decoded straight to UI_KEY_PGUP/UI_KEY_PGDN, because every
 * widget that read them wanted paging and nothing wanted the direction. A
 * teletext reader does: its arrows mean "the page before / after this one".
 * The widgets that wanted paging map them at their own switch, so their
 * behaviour is unchanged - see ui_doc.c and ui_list.c.
 */
#define UI_KEY_LEFT  1008
#define UI_KEY_RIGHT 1009

/*
 * A mouse event is waiting on the board (AEW_SCREEN MOUSE?). The board
 * absorbs the SGR reports and wakes the door with ONE byte, 0x1E, when its
 * mouse slot goes from empty to non-empty; ui_key_read turns that byte into
 * UI_KEY_MOUSE and the door fetches the events with ae_bs_mouse_drain.
 */
#define UI_KEY_MOUSE 1010
/** ESC O P: F1 (VT100 PF1). A door that has a help screen binds it. */
#define UI_KEY_F1    1011
/** ESC O Q / R / S: F2, F3, F4 (VT100 PF2-PF4). */
#define UI_KEY_F2    1012
#define UI_KEY_F3    1013
#define UI_KEY_F4    1014
/** ESC [ Z: Shift+Tab. */
#define UI_KEY_BACKTAB 1015
/** ESC [ 3 ~: the Delete key. */
#define UI_KEY_DELETE 1016
/** Shift+Left/Right/Home/End (xterm ESC [ 1 ; 2 D/C/H/F): extend a selection. */
#define UI_KEY_SHIFT_LEFT  1017
#define UI_KEY_SHIFT_RIGHT 1018
#define UI_KEY_SHIFT_HOME  1019
#define UI_KEY_SHIFT_END   1020
/** F5: the board's AE_KEY_F5 byte, or ESC [ 1 5 ~ straight from a terminal. */
#define UI_KEY_F5          1021
/** F6, F7: the board's AE_KEY_F6/F7 bytes, or ESC [ 1 7 ~ / ESC [ 1 8 ~. */
#define UI_KEY_F6          1022
#define UI_KEY_F7          1023
/* A raw 0x1E read is UI_KEY_MOUSE: Ctrl-^ collides with the wake byte while the board's mouse absorber is armed. */
#define UI_MOUSE_WAKE 0x1E

/**
 * Where a door's keys come from.
 *
 * SET EVERY FIELD. Callers build this on the stack and assign field by
 * field, so a member left alone holds whatever was there - and this struct
 * is all function pointers, which means calling one. `idle` was added on
 * 2026-09-06 and segfaulted the SDK's own test suite the moment it landed,
 * for exactly that reason. Anything optional takes 0.
 */
typedef struct ui_key_source {
    int  (*next)(void *ctx);     /* blocking read of one byte; < 0 = user gone */
    int  (*pending)(void *ctx);  /* non-zero if a byte is queued; must NOT consume */
    void (*settle)(void *ctx);   /* wait long enough for a sequence's tail to land; may be NULL */
    /**
     * Called over and over while no key is waiting; may be NULL.
     *
     * Where a door's animation lives. A 68K door has one thread and blocks
     * in ae_key(), so a masthead that slides has to slide HERE - there is
     * no timer to hang it on. The door decides how long each call takes and
     * whether to draw at all, so a screen that bans decorative motion (the
     * 40-column tier) simply does not pass one.
     *
     * Requires `pending`: without it there is no way to know when to stop.
     */
    void (*idle)(void *ctx);
    /**
     * Push everything drawn so far to the caller; may be NULL.
     *
     * A widget that PAINTS AND THEN BLOCKS - ui_input, ui_confirm, ui_doc -
     * only ever receives an `ansi_buf`, which is memory. Without this it drew
     * its prompt into that buffer and then waited for a key that the caller
     * had no way of knowing was wanted: on the live board, pressing P in SUBS
     * looked like it did nothing, and the next keystroke silently took the
     * confirm's default (2026-09-08).
     *
     * Set it to whatever sends the door's screen - ui_screen_flush - and the
     * widgets will call it after painting, before every blocking read. NULL
     * keeps the old behaviour, which is only correct for a caller that never
     * blocks on its own drawing.
     */
    void (*flush)(void *ctx);
    void *ctx;
} ui_key_source;

/* Decodes what follows an ESC that the caller has already read. Returns
 * UI_KEY_ESC for a lone ESC, a UI_KEY_* for a recognised CSI/SS3
 * sequence, 0 for one it does not know, or the negative value next()
 * reported for a lost user. *pushback receives a byte that was read but
 * belongs to the NEXT key (ESC then Q, typed fast), else -1; the caller
 * must deliver it before reading again. */
int ui_decode_escape(const ui_key_source *src, int *pushback);



/*
 * The whole loop: read one key, decoding an escape sequence if that is what
 * arrives. `pushback` is the caller's one-byte holding pen - initialise it
 * to -1 and hand the same pointer back every time.
 */
int ui_key_read(const ui_key_source *src, int *pushback);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* UI_KEY_H */
