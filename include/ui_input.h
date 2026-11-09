/*
 * ui_input - a single-line editor, and the two dialogs built on it.
 *
 * LIFTED from examples/doorrepo-c/doorrepo.c's ui_text_prompt and ui_confirm,
 * which have been the door's prompts on a real board. What changed is the
 * coupling: those read the key straight from the BBS and flushed straight to
 * it, so the editor could not exist without a session. Here the key source
 * and the frame sink are the caller's (ui_key.h, ui_ansi.h), which is what
 * lets the editing rules be tested keystroke by keystroke.
 *
 * The rules themselves are unchanged, including the two that look like
 * details and are not:
 *
 *  - a cursor key inside a prompt means NOTHING and is swallowed, rather
 *    than inserting an escape sequence into the text;
 *  - the colours and the cursor are put back before returning, or the
 *    caller's next screen is painted in the prompt's blue.
 */

#ifndef UI_INPUT_H
#define UI_INPUT_H

#include "ui_ansi.h"
#include "ui_key.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Where a prompt draws, and in what colours. */
typedef struct {
    int row, left, cols;
    int fg, bg, ink;
    /** Fold typed letters to upper case - what a BBS command prompt wants. */
    int upper;
    /** Non-zero: echo this character for every typed one (getPass2). */
    int mask;
} ui_input_style;

/** Sensible defaults: white on blue, yellow text, no folding. */
void ui_input_style_init(ui_input_style *style);

/**
 * Edit `buf` in place until ENTER.
 *
 * Returns 1 when something was entered, 0 when the line was left empty, and
 * -1 when the caller went away mid-edit - which a door must treat as the end
 * of the session, not as an empty answer.
 */
int ui_input(ansi_buf *b, const ui_key_source *src, int *pushback,
             const ui_input_style *style, const char *label,
             char *buf, int maxlen);

/**
 * Ask a yes/no question. Y and N answer it; ENTER takes `default_yes`.
 *
 * Returns 1 for yes, 0 for no, -1 for a lost caller.
 */
int ui_confirm(ansi_buf *b, const ui_key_source *src, int *pushback,
               const ui_input_style *style, const char *question,
               int default_yes);


/*
 * ui_line: the editing rules of ui_input, one key at a time, for a door that
 * owns its own event loop and screen (livechat) and so cannot hand control to
 * the blocking ui_input. The buffer, length and cursor are the caller's.
 *
 * Keys: printable insert at the cursor; Backspace and Delete; Left, Right,
 * Home, End; CTRL-U clears. With a history, Up and Down walk it (below).
 */

/** Lines kept by a history, and the longest line one holds. */
#define UI_HIST_MAX 100
#define UI_HIST_TEXT 256
#define UI_HIST_ID 48

/** A ring of sent lines with draft recall: Up from a fresh line remembers it, Down past the newest brings it back. */
typedef struct {
    char line[UI_HIST_MAX][UI_HIST_TEXT];
    int n;                       /* lines held, oldest dropped past UI_HIST_MAX */
    int at;                      /* -1 not browsing, else how many back from the newest */
    char draft[UI_HIST_TEXT];    /* what was typed before the first Up */
    char id[UI_HIST_MAX][UI_HIST_ID];   /* each line's message id, "" when it has none (TS input-history entries) */
} ui_hist;

void ui_hist_init(ui_hist *h);
/** Remember a sent line (empty lines are not kept) and stop browsing. */
void ui_hist_add(ui_hist *h, const char *text);
/** ui_hist_add with the message id the line was sent as (TS input-history add(id, text)); `id` may be NULL. */
void ui_hist_add_id(ui_hist *h, const char *id, const char *text);
/** The server named a sent line: entries holding `from` (and an edit in progress) take `to` (TS rekey). */
void ui_hist_rekey(ui_hist *h, const char *from, const char *to);
/** The id of the recalled line while browsing (Enter then edits that message), else NULL (TS getEditingId). */
const char *ui_hist_editing(const ui_hist *h);
/** The id of the newest line, NULL when there is none (TS lastId). */
const char *ui_hist_last_id(const ui_hist *h);
/** Stop browsing (the line was sent or replaced by the caller). */
void ui_hist_reset(ui_hist *h);

/**
 * Apply `key` to `buf` (capacity `cap`, current `*len` / `*cur`).
 * Returns 1 when the key edited or moved, 0 when it is not the line's.
 * Up on an empty line that is not browsing returns 0, so the caller can use it
 * for something else (livechat's message picker). `h` may be NULL.
 * `upper` folds typed letters to upper case.
 */
int ui_line_key(char *buf, int cap, int *len, int *cur, int key, ui_hist *h, int upper);

/**
 * ui_line_key with a selection, the TS textbox's (sdk/engines/ui/blessed/
 * widgets/textbox.ts): Shift+Left/Right/Home/End extend it from `*anchor`
 * (-1 = no selection) to the caret; Backspace and Delete remove it; a typed
 * character replaces it; any other key the line takes clears it. The
 * selection is the text between `*anchor` and `*cur`. ui_line_key alone
 * treats the Shift keys as plain moves.
 */
int ui_line_sel_key(char *buf, int cap, int *len, int *cur, int *anchor, int key, ui_hist *h, int upper);

#ifdef __cplusplus
}
#endif

#endif /* UI_INPUT_H */
