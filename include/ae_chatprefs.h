/*
 * ae_chatprefs - the client for the board's AEW_CHATPREFS (11300).
 *
 * Reads and writes the caller's LiveChat prefs in the SAME per-user store the
 * TS door uses (db.chatPrefs()). The board side is
 * web/backend/src/amiga-emulation/xim/chat-prefs.ts.
 *
 * Wire (request and reply are each one String, AE_MAX_LINE = 198 bytes):
 *   GET            -> "showLogins=1;showFileActivity=0;...;pinned=a,b"
 *                     (stored keys only, bools 0/1, pinned last as whole names
 *                     that fit; empty = nothing stored)
 *   GET TAB pinned -> "a,b,c" (the full pinned list)
 *   SET TAB key TAB value -> "OK" / "NO"
 */
#ifndef AE_CHATPREFS_H
#define AE_CHATPREFS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Fills `out` with the compact prefs line. Returns its length, or -1 when the board has no command / nothing stored (out is ""). */
int ae_chat_prefs_get(char *out, int outlen);

/* Fills `out` with the full comma-separated pinned channel list. Same return as ae_chat_prefs_get. */
int ae_chat_prefs_get_pinned(char *out, int outlen);

/* Saves ONE key (booleans "0"/"1"; "pinnedChannels" a comma list). 1 saved, 0 refused, -1 board cannot take prefs. */
int ae_chat_prefs_set(const char *key, const char *value);

/* The 0/1 value of `key` in a GET line, or `dflt` when the key is absent. */
int ae_chat_prefs_bool(const char *line, const char *key, int dflt);

/* Copies the value of `key` (up to the next ';') into `out`. Returns `out`, or NULL when absent. */
const char *ae_chat_prefs_str(const char *line, const char *key, char *out, int outlen);

#ifdef __cplusplus
}
#endif

#endif
