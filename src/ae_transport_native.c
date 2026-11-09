/* aedoor_native.c - stdio twin of the AmiExpress XIM door I/O layer.
 *
 * Implements aedoor.h over plain stdio so the DoorRepo door logic (Task 6)
 * can be written once, TDD'd, and run on a dev machine without a live
 * AmiExpress node or an m68k emulator, then cross-compiled unchanged
 * against aedoor_amiga.c for the real target. There is no "#ifdef AMIGA"
 * anywhere in this file -- the choice of THIS file over aedoor_amiga.c,
 * made by the Makefile (Task 6), is the entire platform mechanism.
 *
 * Mapping to the real protocol (see aedoor.h and aedoor_amiga.c for the
 * wire-level citations):
 *   ae_put()      -> stdout, chunked at AE_MAX_LINE like the real backend,
 *                     with the same "bbs:" reroute guard (see
 *                     would_reroute_to_file_display() below) so a caller
 *                     sees byte-identical output on either backend.
 *   ae_get()      -> one line via fgets(), truncated safely at maxlen, and
 *                     draining any unread remainder of an over-long line so
 *                     the next call starts at the following line.
 *   ae_key()      -> one character via getchar(); EOF reports as -1,
 *                     mirroring the real backend's carrier-loss code.
 *   ae_check()    -> always 0: there is no BBS connection to lose.
 *   ae_start()    -> no-op success (nothing to register with).
 *   ae_shutdown() -> no-op that RETURNS (no BBS to notify, no reason to
 *                     end the caller's process early).
 *   ae_fatal()    -> still terminates the process via exit(code); a fatal
 *                     error must stop execution on every backend, even
 *                     though there is nothing to notify here.
 *
 * C89. ASCII only.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include "aedoor.h"
#include "ae_transport_native.h"

/* ---- the stand-in board -------------------------------------------------
 *
 * ae_field_exchange is the one call in aedoor.h that stdio cannot honestly
 * implement: "send a query, get an answer" has no terminal equivalent. So the
 * host backend delegates to whatever a test installed, and answers NOTHING
 * when nothing is installed.
 *
 * WHAT "NO HOOK" MODELS, exactly: a classic AmiExpress being asked a command
 * it has never heard of. Its DEFAULT case (express.e:4221-4228) logs a warning
 * and touches NEITHER msg.String NOR msg.data - so the door gets back the
 * request it wrote into String itself, and the read flag it put in Data. Not
 * an empty reply: its own question, echoed. This function reproduces that
 * exactly, because a door telling "the board has no feed" apart from "the
 * conference is empty" is the difference between the right sentence and a
 * screen that says nobody has ever posted here.
 *
 * ae_field_read/ae_field_write are deliberately still absent from this file:
 * they are reached only through ae_session_bbs.c, which no host test links.
 */
static ae_native_exchange_fn exchange_fn = 0;
static void *exchange_ctx = 0;

void ae_native_set_exchange(ae_native_exchange_fn fn, void *ctx)
{
    exchange_fn = fn;
    exchange_ctx = ctx;
}

int ae_field_exchange(int field, const char *request, char *out, int cap)
{
    int len;

    if (out == 0 || cap <= 0) {
        return -1;
    }
    out[0] = '\0';
    if (request == 0) {
        request = "";
    }
    if (exchange_fn != 0) {
        return exchange_fn(exchange_ctx, field, request, out, cap);
    }

    /* A classic board: the question comes back untouched. */
    for (len = 0; len < cap - 1 && request[len] != '\0'; len++) out[len] = request[len];
    out[len] = '\0';
    return len;
}

/* AmiExpress doors sometimes deliberately send "bbs:" device/volume paths
 * via JH_SM to trigger file display instead of a printed line: our emulator
 * matches this by trimming the message text and testing a case-insensitive
 * prefix (io.ts:657-666 -- "const trimmed = text.trim(); ... if
 * (trimmed.toLowerCase().startsWith('bbs:')) { ...handleShowFile(...);
 * return; }"). This door only ever wants JH_SM/ae_put to print. A sysop can
 * legitimately configure a download directory as "bbs:doors/", so a wholly
 * ordinary status line like "bbs:doors/FOO.LHA saved" would otherwise be
 * silently rerouted on the Amiga backend -- guarded identically here so the
 * native twin's output matches byte-for-byte and the behaviour is testable
 * without a live emulator.
 *
 * MUST be evaluated PER CHUNK, not once against the caller's whole string:
 * the BBS's reroute check runs on each JH_SM message independently, so any
 * chunk whose OWN first bytes spell "bbs:" is just as vulnerable as the
 * first chunk -- see the matching comment in aedoor_amiga.c's ae_put() for
 * the full reasoning. This is a stdio twin with no real 198-byte send
 * limit, but ae_put() below still walks the same AE_MAX_LINE chunk
 * boundaries as the real backend and re-checks on every iteration, so a
 * caller (and the test suite) sees byte-identical placement of the guard
 * byte on both backends.
 *
 * A leading SPACE does NOT defeat the real check: the BBS trims leading/
 * trailing whitespace BEFORE testing the prefix (text.trim(), io.ts:659),
 * and messages.ts:200 builds that JS string via String.fromCharCode() on
 * the raw bytes, so both an ASCII space (0x20) and a Latin-1 NBSP (0xA0)
 * are still classified as whitespace and stripped by .trim() -- verified
 * against the actual check: " bbs:x".trim().startsWith("bbs:") is true.
 * The guard byte must be printable and non-whitespace to survive trim();
 * a single leading period reliably breaks the match and renders as an
 * ordinary visible character on every terminal. */
static int would_reroute_to_file_display(const char *text, unsigned long n)
{
    unsigned long i;

    i = 0;
    while (i < n && isspace((unsigned char)text[i])) {
        i++;
    }
    if (i + 4 > n) {
        return 0; /* fewer than 4 bytes left in THIS message -- cannot match */
    }
    return (text[i] == 'b' || text[i] == 'B')
        && (text[i + 1] == 'b' || text[i + 1] == 'B')
        && (text[i + 2] == 's' || text[i + 2] == 'S')
        && text[i + 3] == ':';
}

int ae_start(int node)
{
    (void)node; /* nothing to register with when there is no AmiExpress */
    /* Unbuffered stdout for the dev backend only. With a pipe or a file on
     * the other end, stdio buffers 4 KB and the door's abrupt exit paths
     * (carrier loss, ae_fatal) do not flush - so a captured run of a real
     * session came back truncated mid-sentence, and a test could not tell
     * "the door stopped there" from "the buffer was never written". On the
     * Amiga side output goes through AmiExpress, which has no such
     * buffering, so this has no equivalent there. */
    setvbuf(stdout, (char *) 0, _IONBF, 0);
    /* Unbuffered stdin too, for ae_input_pending(): select() sees the file
     * descriptor, not stdio's buffer, so a buffered read would hide the
     * tail of an escape sequence and a lone ESC would be indistinguishable
     * from an arrow key - the exact question that function exists to
     * answer. One byte per read() is a price a scripted run never notices. */
    setvbuf(stdin, (char *) 0, _IONBF, 0);
    return 0;
}

void ae_put(const char *text, int newline)
{
    unsigned long len;
    unsigned long offset;
    unsigned long chunk;
    unsigned long remaining;
    unsigned long check_len;
    unsigned long budget;
    int prefixed;

    if (text == NULL) {
        text = "";
    }

    len = (unsigned long)strlen(text);
    offset = 0;

    /* Chunk at AE_MAX_LINE even though stdout has no such limit, so the
     * boundary logic is identical to -- and testable as a twin of -- the
     * real backend, which genuinely cannot send more than that per call.
     * The bbs: reroute guard is re-evaluated on EVERY iteration against
     * exactly the bytes that iteration is about to write -- see the
     * comment above would_reroute_to_file_display(). */
    while (offset < len) {
        remaining = len - offset;
        check_len = (remaining > (unsigned long)AE_MAX_LINE) ? (unsigned long)AE_MAX_LINE : remaining;
        prefixed = would_reroute_to_file_display(text + offset, check_len) ? 1 : 0;

        budget = (unsigned long)AE_MAX_LINE - (unsigned long)prefixed;
        chunk = remaining;
        if (chunk > budget) {
            chunk = budget;
        }

        if (prefixed) {
            fputc('.', stdout); /* guard byte: breaks the trimmed "bbs:" prefix match, see would_reroute_to_file_display() above */
        }
        fwrite(text + offset, 1, (size_t)chunk, stdout);

        offset += chunk;
    }

    if (newline) {
        fputc('\n', stdout);
    }
}

void ae_get(char *buf, int maxlen)
{
    char *newline;
    int c;

    if (buf == NULL || maxlen <= 0) {
        return;
    }

    /* fgets() never writes more than maxlen bytes (including the NUL), so
     * this cannot overflow buf regardless of how long the actual input
     * line is. */
    if (fgets(buf, maxlen, stdin) == NULL) {
        buf[0] = '\0';
        return;
    }

    newline = strchr(buf, '\n');
    if (newline != NULL) {
        *newline = '\0';
        return;
    }

    /* No newline landed in buf: the input line was longer than maxlen-1
     * chars and its remainder is still queued on stdin. Drain it here so
     * the NEXT ae_get() call starts cleanly at the following line instead
     * of reading leftovers from this one -- otherwise a single over-long
     * answer desynchronises every prompt that follows it. Stops at EOF too,
     * so this can never spin if the stream simply ends mid-line. */
    do {
        c = getchar();
    } while (c != '\n' && c != EOF);
}

/* No wait here: the pending check below carries its own short grace
 * period, and a scripted run always has its next byte (or its end) ready,
 * so a sleep would only slow the tests down. */
long ae_native_delay_calls, ae_native_delay_total;     /* tests: how often and how long the door slept */
int ae_native_delay_max;

void ae_delay_ticks(int ticks)
{
    ae_native_delay_calls++;
    ae_native_delay_total += ticks;
    if (ticks > ae_native_delay_max) ae_native_delay_max = ticks;
    /* Nothing to wait for: this backend never reports pending input, so a
     * quiet-period wait here would only slow a scripted run down. */
    (void) ticks;
}

/* "Has the next byte already arrived?" - answered the way the BBS answers
 * GETKEY: without consuming it. This used to report a flat 0, which was
 * honest for a line-mode terminal but made the native twin diverge from the
 * Amiga backend the moment the door started reading a lone ESC as a key:
 * every scripted arrow sequence would have decoded as ESC, ESC, ESC.
 *
 * select() waits up to two ticks for the descriptor - the same grace the
 * door gives the real BBS - then one byte is peeked with getchar()/ungetc()
 * so that end-of-file reads as "nothing pending" rather than as a byte.
 * ungetc() of one character is guaranteed by C89, and stdin is unbuffered
 * (ae_start) so the descriptor and the stream agree.
 *
 * END OF FILE IS A LOST CARRIER. A scripted run whose keys ran out has no
 * caller left, exactly as on the board when carrier drops: the Amiga backend
 * then answers with an error from ae_key(), and ui_key_read's idle loop
 * (`while (!pending) idle()`) must be allowed to reach that read. If EOF
 * reported "nothing pending" forever, a key script that did not end in a
 * leaving key spun in that loop for ever and hung `make test`. So the first
 * EOF answers 0 - a lone ESC at the end of a script still reads as ESC, the
 * "did anything follow it" question - and every later EOF answers 1, so the
 * next ae_key() returns -1. Only the native build; the Amiga path is
 * untouched. */
static int eof_seen;

int ae_input_pending(void)
{
    fd_set readable;
    struct timeval grace;
    int fd = fileno(stdin);
    int c;

    FD_ZERO(&readable);
    FD_SET(fd, &readable);
    grace.tv_sec = 0;
    grace.tv_usec = 40000;                     /* 2 ticks of 1/50 s */
    if (select(fd + 1, &readable, (fd_set *) 0, (fd_set *) 0, &grace) <= 0) {
        return 0;
    }
    c = getchar();
    if (c == EOF) {
        if (!eof_seen) {
            eof_seen = 1;
            return 0;
        }
        return 1;
    }
    eof_seen = 0;
    ungetc(c, stdin);
    return 1;
}

int ae_key(void)
{
    int c;

    c = getchar();
    if (c == EOF) {
        return -1;
    }
    eof_seen = 0;
    return c;
}

void ae_raw_arrows(int on)
{
    /* No BBS on this backend: a terminal delivers whatever it delivers, and
     * ui_read_key() already understands both the single-byte codes and the
     * escape sequences. */
    (void) on;
}

void ae_return_command(const char *command)
{
    /* No BBS to hand it to on this backend. Printed rather than silently
     * dropped, so the native build shows what a real board would have been
     * asked to run - the probe harness and a developer at a terminal both
     * read this. */
    if (command != NULL && command[0] != '\0') {
        printf("[ae_return_command] %s\n", command);
    }
}

int ae_check(void)
{
    return 0;
}

void ae_shutdown(void)
{
    /* no-op: nothing to notify, and no reason to end the caller's process
     * early when there is no BBS on the other end. */
}

void ae_fatal(int code)
{
    exit(code);
}
