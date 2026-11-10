/*
 * ui_key - see ui_key.h.
 *
 * ui_decode_escape is lifted verbatim from examples/doorrepo-c/flow.c:1553,
 * renamed. ui_key_read is the loop around it that every caller of the
 * original wrote for itself.
 */

#include "ui_key.h"

/* Function key from the n of ESC [ n ~ (xterm/PuTTY/SyncTERM numbering). */
static int fkey_from_vt(int n)
{
    switch (n) {
    case 11: return UI_KEY_F1;
    case 12: return UI_KEY_F2;
    case 13: return UI_KEY_F3;
    case 14: return UI_KEY_F4;
    case 15: return UI_KEY_F5;
    case 17: return UI_KEY_F6;
    case 18: return UI_KEY_F7;
    default: return 0;
    }
}

/*
 * Amiga 8-bit CSI (0x9B): console.device sends F1-F7 as 9B 0~ .. 9B 6~
 * (Devices manual, "Special key report sequences"). Real AmiExpress hands a
 * door the caller's bytes raw, so this arrives here from an Amiga terminal.
 * The 7-bit ESC [ n ~ keeps the VT220 meaning; only the 8-bit form is Amiga.
 * Arrows are A-D as in 7-bit CSI. Anything else is read whole and answered 0.
 */
static int ui_decode_amiga_csi(const ui_key_source *src)
{
    int c, n = -1;

    for (;;) {
        c = src->next(src->ctx);
        if (c < 0) return c;
        if (c >= '0' && c <= '9') { n = (n < 0 ? 0 : n * 10) + (c - '0'); continue; }
        if (c == ';' || c == '?' || c == ' ') continue;
        break;
    }
    switch (c) {
    case 'A': return UI_KEY_UP;
    case 'B': return UI_KEY_DOWN;
    case 'C': return UI_KEY_RIGHT;
    case 'D': return UI_KEY_LEFT;
    case '~':
        switch (n) {
        case 0: return UI_KEY_F1;
        case 1: return UI_KEY_F2;
        case 2: return UI_KEY_F3;
        case 3: return UI_KEY_F4;
        case 4: return UI_KEY_F5;
        case 5: return UI_KEY_F6;
        case 6: return UI_KEY_F7;
        default: return 0;
        }
    default:
        return 0;
    }
}

int ui_decode_escape(const ui_key_source *src, int *pushback)
{
    int c;

    *pushback = -1;

    /* Give the rest of a sequence time to arrive, then ask - without
     * consuming - whether anything did. Nothing queued means the user
     * pressed ESC and nothing else. */
    if (src->settle != (void (*)(void *)) 0) {
        src->settle(src->ctx);
    }
    if (!src->pending(src->ctx)) {
        return UI_KEY_ESC;
    }

    c = src->next(src->ctx);
    if (c < 0) {
        return c;
    }
    if (c != '[' && c != 'O') {
        /* ESC followed by an ordinary key: two keystrokes, not one
         * sequence. The second one is handed back, not eaten. */
        *pushback = c;
        return UI_KEY_ESC;
    }

    c = src->next(src->ctx);
    if (c < 0) {
        return c;
    }
    switch (c) {
    case '<': {
        /*
         * The tail of an SGR mouse report whose ESC [ reached the door in a
         * chunk of its own (the board holds a split report only from ESC [ <).
         * Swallow it up to the final M or m: stray digits and semicolons must
         * never be delivered as keys. Not a key, so 0, like any unknown
         * sequence.
         */
        for (;;) {
            c = src->next(src->ctx);
            if (c < 0) return c;
            if (c == 'M' || c == 'm') break;
            if (!((c >= '0' && c <= '9') || c == ';')) { *pushback = c; break; }
        }
        return 0; }
    case 'A': return UI_KEY_UP;
    case 'B': return UI_KEY_DOWN;
    case 'C': return UI_KEY_RIGHT;
    case 'D': return UI_KEY_LEFT;
    case 'H': return UI_KEY_HOME;
    case 'F': return UI_KEY_END;
    case 'P': return UI_KEY_F1;
    case 'Q': return UI_KEY_F2;
    case 'R': return UI_KEY_F3;
    case 'S': return UI_KEY_F4;
    case 'Z': return UI_KEY_BACKTAB;
    case '[': {
        /* linux console: ESC [ [ A..E are F1-F5. */
        int f = src->next(src->ctx);
        if (f < 0) return f;
        switch (f) {
        case 'A': return UI_KEY_F1;
        case 'B': return UI_KEY_F2;
        case 'C': return UI_KEY_F3;
        case 'D': return UI_KEY_F4;
        case 'E': return UI_KEY_F5;
        default: break;
        }
        *pushback = f;
        return 0; }
    case '5': case '6': case '1': case '4': case '3': {
        /* ESC [ n ~ : the tilde is part of the sequence. */
        int t = src->next(src->ctx);
        if (t < 0) {
            return t;
        }
        if (c == '1' && t == ';') {
            /* ESC [ 1 ; mod X : a modified cursor key (xterm). Shift (mod 2)
             * on Left/Right/Home/End selects; any other modifier or key is
             * read whole and answered as the plain key, never as a stray
             * HOME with the tail left behind as typing. */
            int mod = src->next(src->ctx), fin;
            if (mod < 0) return mod;
            fin = src->next(src->ctx);
            if (fin < 0) return fin;
            if (mod == '2') {
                if (fin == 'D') return UI_KEY_SHIFT_LEFT;
                if (fin == 'C') return UI_KEY_SHIFT_RIGHT;
                if (fin == 'H') return UI_KEY_SHIFT_HOME;
                if (fin == 'F') return UI_KEY_SHIFT_END;
            }
            switch (fin) {
            case 'A': return UI_KEY_UP;
            case 'B': return UI_KEY_DOWN;
            case 'C': return UI_KEY_RIGHT;
            case 'D': return UI_KEY_LEFT;
            case 'H': return UI_KEY_HOME;
            case 'F': return UI_KEY_END;
            default:  return 0;
            }
        }
        if (c == '1' && t >= '0' && t <= '9') {
            /* ESC [ 1 n ~ : F1-F7 (11-15, 17, 18) and the other xterm function
             * keys. Read the tilde too, or it is delivered as a typed '~'. */
            int fin = src->next(src->ctx);
            if (fin < 0) return fin;
            if (fin != '~') { *pushback = fin; return 0; }
            return fkey_from_vt(10 + (t - '0'));
        }
        if (c == '3') return UI_KEY_DELETE;
        if (c == '5') return UI_KEY_PGUP;
        if (c == '6') return UI_KEY_PGDN;
        if (c == '1') return UI_KEY_HOME;
        return UI_KEY_END;
    }
    default:
        return 0;
    }
}

int ui_key_read(const ui_key_source *src, int *pushback)
{
    int c;

    if (!src || !src->next || !pushback) return -1;

    /* A byte handed back by the last decode comes first, or an ESC that was
       followed by an ordinary key would lose that key. */
    if (*pushback >= 0) {
        c = *pushback;
        *pushback = -1;
        return c;
    }

    /* Nothing typed yet: let the door do something with the wait. */
    if (src->idle && src->pending) {
        while (!src->pending(src->ctx)) {
            src->idle(src->ctx);
        }
    }

    c = src->next(src->ctx);
    if (c < 0) return c;

    /* CR and LF both mean ENTER: a telnet client sends CR, a raw socket LF,
       and a door that only knew one of them would look dead to half its
       callers. */
    if (c == '\r' || c == '\n') return UI_KEY_ENTER;

    /* THE ARROWS A BOARD ACTUALLY SENDS.
     *
     * AmiExpress converts cursor keys before a door ever sees them:
     * express.e:7514-7528 turns ESC[A/B/C/D into 4, 5, 3 and 2, and JH_HK
     * hands the door that single byte (xim/io.ts:755-781 ports it). A door
     * decoding ESC sequences is decoding something the board already ate,
     * which is why THEMEC's list did not move under the arrow keys on a real
     * board while its unit tests passed (sysop, 2026-09-06).
     *
     * The ESC path below stays for the rawArrow case and for a door driven
     * from a terminal directly. */
    if (c == AE_ARROW_LEFT)  return UI_KEY_LEFT;
    if (c == AE_ARROW_RIGHT) return UI_KEY_RIGHT;
    if (c == AE_ARROW_UP)    return UI_KEY_UP;
    if (c == AE_ARROW_DOWN)  return UI_KEY_DOWN;
    if (c == AE_KEY_HOME)    return UI_KEY_HOME;
    if (c == AE_KEY_END)     return UI_KEY_END;
    if (c == AE_KEY_PGUP)    return UI_KEY_PGUP;
    if (c == AE_KEY_PGDN)    return UI_KEY_PGDN;
    if (c == AE_KEY_DELETE)      return UI_KEY_DELETE;
    if (c == AE_KEY_SHIFT_LEFT)  return UI_KEY_SHIFT_LEFT;
    if (c == AE_KEY_SHIFT_RIGHT) return UI_KEY_SHIFT_RIGHT;
    if (c == AE_KEY_SHIFT_HOME)  return UI_KEY_SHIFT_HOME;
    if (c == AE_KEY_SHIFT_END)   return UI_KEY_SHIFT_END;
    if (c == AE_KEY_F5)          return UI_KEY_F5;
    if (c == AE_KEY_F6)          return UI_KEY_F6;
    if (c == AE_KEY_F7)          return UI_KEY_F7;
    if (c == AE_KEY_F1)          return UI_KEY_F1;
    if (c == AE_KEY_F2)          return UI_KEY_F2;
    if (c == AE_KEY_F3)          return UI_KEY_F3;
    if (c == AE_KEY_F4)          return UI_KEY_F4;

    if (c == UI_MOUSE_WAKE) return UI_KEY_MOUSE;

    if (c == 0x1b) return ui_decode_escape(src, pushback);
    if (c == 0x9b) return ui_decode_amiga_csi(src);

    return c;
}
