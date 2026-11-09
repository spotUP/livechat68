/*
 * ae_chunk - see ae_chunk.h. Lifted from flow.c with its reasoning.
 */

#include "ae_chunk.h"

static unsigned long esc_safe_chunk(const char *text, unsigned long len,
                                    unsigned long budget)
{
    unsigned long take;
    unsigned long i;
    long esc = -1;

    if (text == (const char *) 0 || len == 0 || budget == 0) {
        return 0;
    }
    take = (len < budget) ? len : budget;

    /* The last ESC in what is about to be sent. Anything earlier has been
     * terminated already, or this one would not be the last. */
    for (i = 0; i < take; i++) {
        if (text[i] == '\033') {
            esc = (long) i;
        }
    }
    if (esc < 0) {
        return take;
    }

    /* Terminated inside the chunk? Three shapes end differently:
     *   CSI (ESC [): on a final byte, 0x40..0x7e, after the '['.
     *   a designation (ESC ( ) * + # % or space, then one more byte): three bytes in all.
     *   anything else (ESC 7, ESC 8, ESC c, ESC D ...): the byte after the ESC IS the whole sequence.
     * The last shape used to be looked for a CSI final byte too, and found none: an "ESC 8" ending a write was
     * "unterminated", so the rail tick left as a 109-byte message plus a lone ESC 8. */
    if ((unsigned long) esc + 1 >= take) {
        goto unterminated;           /* the ESC is the last byte: its second byte is not here yet */
    }
    {
        unsigned char next = (unsigned char) text[esc + 1];
        if (next != '[') {
            if (next == '(' || next == ')' || next == '*' || next == '+' || next == '#' || next == '%' || next == ' ') {
                if ((unsigned long) esc + 2 < take) {
                    return take;
                }
                goto unterminated;
            }
            return take;
        }
    }
    for (i = (unsigned long) esc + 2; i < take; i++) {
        unsigned char c = (unsigned char) text[i];
        if (c >= 0x40 && c <= 0x7e) {
            return take;
        }
    }

unterminated:
    /* Unterminated: cut before it and let the next message carry it whole. */
    if (esc == 0) {
        return take;                 /* longer than a whole message: send on */
    }
    return (unsigned long) esc;
}

/* Do not cut a UTF-8 sequence in half either. A door that declared UTF-8
 * output (AEW_CHARSET) sends emoji as 3-4 byte sequences, and the board
 * decodes each message on its own: a torn sequence would arrive as latin1
 * garbage. Back the cut off to the lead byte when the bytes before the cut
 * are an unfinished sequence. A latin1 door's bytes may look like a lead
 * byte too; moving them to the next message changes no byte the caller
 * gets, only which message carries it. */
unsigned long ae_safe_chunk(const char *text, unsigned long len,
                            unsigned long budget)
{
    unsigned long take = esc_safe_chunk(text, len, budget);
    unsigned long back;

    if (take == 0 || take >= len) {
        return take;
    }
    for (back = 1; back <= 3 && back <= take; back++) {
        unsigned char c = (unsigned char) text[take - back];
        unsigned long need;

        if ((c & 0xc0) == 0x80) {
            continue;                /* a continuation byte: look further back */
        }
        if (c >= 0xc2 && c <= 0xdf) need = 2;
        else if (c >= 0xe0 && c <= 0xef) need = 3;
        else if (c >= 0xf0 && c <= 0xf4) need = 4;
        else return take;            /* ASCII or not a lead byte: nothing torn */
        if (back < need && take - back > 0) {
            return take - back;      /* unfinished: the whole sequence goes next */
        }
        return take;
    }
    return take;
}
