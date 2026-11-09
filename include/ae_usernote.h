/*
 * ae_usernote - the client for the board's AEW_USERNOTE (11200).
 *
 * Adds a sysop note about a user, to the store the account editor's User Notes
 * view reads. Request `NOTE TAB username TAB text` in String (200 bytes; the
 * text is clipped to fit), answer "OK" or "NO". The board side is
 * web/backend/src/amiga-emulation/xim/user-note.ts.
 */
#ifndef AE_USERNOTE_H
#define AE_USERNOTE_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1 = saved, 0 = board refused (unknown user / save failed), -1 = board cannot take notes (no reply). */
int ae_user_note(const char *username, const char *text);

#ifdef __cplusplus
}
#endif

#endif
