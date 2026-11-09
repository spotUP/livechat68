/* ae_dos.c - the platform-free half of ae_dos: byte order.
 *
 * Compiled on BOTH backends. There is nothing here but arithmetic, and that is
 * the point: the four accessors below are the only place in the SDK that turns
 * bytes on disk into numbers, so if AmiExpress's on-disk order is ever read
 * wrongly it is wrong in one file rather than in every caller.
 *
 * Why not a struct cast, which on 68K would be free: because the host is
 * little-endian and 68K is big-endian, so a cast produces correct numbers in
 * the emulator and garbage in the tests - or, far worse, the other way round,
 * where the tests pass and the board reads somebody's mail out of the wrong
 * offset. Shifting is a few instructions on a 68000 and it is the same few on
 * every machine.
 *
 * ae_dos_get_long sign-extends explicitly. `long` is 32 bits under vbcc and 64
 * on the host; a value with the top bit set must mean the same negative number
 * on both, and letting the type's width decide is how it would not.
 *
 * C89. ASCII only.
 */

#include "ae_dos.h"

long ae_dos_get_long(const unsigned char *p)
{
    unsigned long v;

    v  = ((unsigned long)p[0]) << 24;
    v |= ((unsigned long)p[1]) << 16;
    v |= ((unsigned long)p[2]) << 8;
    v |= ((unsigned long)p[3]);

    /* Sign-extend from 32 bits, whatever `long` happens to be here. */
    if (v & 0x80000000UL) {
        return -(long)((~v + 1UL) & 0xFFFFFFFFUL);
    }
    return (long)v;
}

int ae_dos_get_word(const unsigned char *p)
{
    unsigned int v;

    v  = ((unsigned int)p[0]) << 8;
    v |= ((unsigned int)p[1]);

    if (v & 0x8000U) {
        return -(int)((~v + 1U) & 0xFFFFU);
    }
    return (int)v;
}

void ae_dos_put_long(unsigned char *p, long value)
{
    unsigned long v = (unsigned long)value;

    p[0] = (unsigned char)((v >> 24) & 0xFF);
    p[1] = (unsigned char)((v >> 16) & 0xFF);
    p[2] = (unsigned char)((v >> 8) & 0xFF);
    p[3] = (unsigned char)(v & 0xFF);
}

void ae_dos_put_word(unsigned char *p, int value)
{
    unsigned int v = (unsigned int)value;

    p[0] = (unsigned char)((v >> 8) & 0xFF);
    p[1] = (unsigned char)(v & 0xFF);
}

/* The short-read loop, written once.
 *
 * It lives here rather than in each backend because it is not platform work -
 * both AmigaDOS Read() and stdio fread() may return fewer bytes than asked for
 * without anything being wrong, and a caller reading fixed-size records has to
 * loop or eventually read a record that is half of one record and half of the
 * next. Doing that in one place means no caller gets it wrong.
 *
 * The three-way return is deliberate. "Nothing left" and "half a record left"
 * are completely different facts about a file: the first ends a walk normally,
 * the second means the file is truncated and the door must say so. Collapsing
 * them into a byte count would leave every caller re-deriving the distinction.
 */
int ae_dos_read_full(ae_file *f, void *buf, long len)
{
    unsigned char *at = (unsigned char *)buf;
    long remaining = len;
    long got;

    if (f == 0 || buf == 0 || len < 0) return -1;
    if (len == 0) return 1;

    while (remaining > 0) {
        got = ae_dos_read(f, at, remaining);
        if (got < 0) return -1;
        if (got == 0) {
            /* Clean end of file: a normal stop only if we had taken nothing. */
            return (remaining == len) ? 0 : -1;
        }
        at += got;
        remaining -= got;
    }
    return 1;
}
