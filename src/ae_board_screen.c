/* ae_board_screen.c - the client for AEW_SCREEN (10800). C89. See ae_board_screen.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aedoor.h"
#include "ae_session.h"
#include "ae_board_screen.h"

/* A board that holds at most 8 presses plus one motion; a bigger count is a broken board. */
#define DRAIN_LIMIT 64

static int ask(const char *request, char *reply, int cap)
{
    return ae_field_exchange(AE_FIELD_SCREEN, request, reply, cap);
}

/* One TAB-separated field as a whole decimal number >= min; returns the start of the next field (or the end), 0 when malformed. */
static const char *number(const char *from, int min, int *out, int last)
{
    char *end;
    long v;

    if (*from < '0' || *from > '9') return 0;
    v = strtol(from, &end, 10);
    if (v < min || v > 100000L) return 0;
    if (last ? *end != '\0' : *end != '\t') return 0;
    *out = (int)v;
    return last ? end : end + 1;
}

int ae_bs_parse_size(const char *reply, int *cols, int *rows)
{
    const char *p;
    int c, r;

    if (!reply || !cols || !rows) return 0;
    if (!(p = number(reply, 1, &c, 0))) return 0;
    if (!number(p, 1, &r, 1)) return 0;
    *cols = c;
    *rows = r;
    return 1;
}

int ae_bs_parse_mouse(const char *reply, ae_mouse_event *ev)
{
    const char *p;
    const char *q;
    int c, r, b = AE_MOUSE_LEFT;
    char k;

    if (!reply || !ev) return 0;
    k = reply[0];
    if (!(k == 'P' || k == 'R' || k == 'M' || k == 'U' || k == 'D') || reply[1] != '\t') return 0;
    if (!(p = number(reply + 2, 1, &c, 0))) return 0;
    /* Row ends the reply (old board, plain MOUSE ON) or a TAB and the button follows. */
    if (!(q = number(p, 1, &r, 1)) && !((q = number(p, 1, &r, 0)) && number(q, 0, &b, 1) && b <= AE_MOUSE_NONE))
        return 0;
    ev->kind = k;
    ev->col = c;
    ev->row = r;
    ev->button = b;
    return 1;
}

int ae_bs_size(int *cols, int *rows)
{
    char reply[AE_MAX_LINE + 1];
    int n = ask("SIZE", reply, (int)sizeof(reply));

    if (n < 0) return AE_BS_GONE;
    if (n == 0) return 0;
    return ae_bs_parse_size(reply, cols, rows);
}

int ae_bs_wide(int on)
{
    char reply[AE_MAX_LINE + 1];
    int n = ask(on ? "WIDE ON" : "WIDE OFF", reply, (int)sizeof(reply));

    if (n < 0) return AE_BS_GONE;
    return strcmp(reply, "OK") == 0 ? 1 : 0;
}

int ae_bs_wide_state(void)
{
    char reply[AE_MAX_LINE + 1];
    int n = ask("WIDE?", reply, (int)sizeof(reply));

    if (n < 0) return AE_BS_GONE;
    return strcmp(reply, "ON") == 0 ? 1 : 0;
}

int ae_bs_wipe_begin(char letter, int conceal)
{
    char req[24], reply[AE_MAX_LINE + 1];
    int n;

    if (letter == 0) return 0;
    sprintf(req, "WIPE %s %c", conceal ? "OUT" : "IN", letter);
    n = ask(req, reply, (int)sizeof(reply));
    if (n < 0) return AE_BS_GONE;
    return strcmp(reply, "OK") == 0 ? 1 : 0;
}

int ae_bs_slide_begin(int forward, int top, int rows)
{
    char req[40], reply[AE_MAX_LINE + 1];
    int n;

    if (top < 1 || rows < 1 || top > 999 || rows > 999) return 0;
    sprintf(req, "WIPE IN P %d %d %c", top, rows, forward ? 'F' : 'B');
    n = ask(req, reply, (int)sizeof(reply));
    if (n < 0) return AE_BS_GONE;
    return strcmp(reply, "OK") == 0 ? 1 : 0;
}

int ae_bs_save(int top, int left, int rows, int cols)
{
    char req[48], reply[AE_MAX_LINE + 1];
    int n;

    if (top < 1 || left < 1 || rows < 1 || cols < 1) return 0;
    if (top > 999 || left > 999 || rows > 999 || cols > 999) return 0;
    sprintf(req, "SAVE %d %d %d %d", top, left, rows, cols);
    n = ask(req, reply, (int)sizeof(reply));
    if (n < 0) return AE_BS_GONE;
    return strcmp(reply, "OK") == 0 ? 1 : 0;
}

int ae_bs_restore(void)
{
    char reply[AE_MAX_LINE + 1];
    int n = ask("RESTORE", reply, (int)sizeof(reply));

    if (n < 0) return AE_BS_GONE;
    return strcmp(reply, "OK") == 0 ? 1 : 0;
}

int ae_bs_wipe_end(void)
{
    char reply[AE_MAX_LINE + 1];
    int n = ask("WIPE END", reply, (int)sizeof(reply));

    if (n < 0) return AE_BS_GONE;
    return strcmp(reply, "OK") == 0 ? 1 : 0;
}

int ae_bs_mouse(int on)
{
    char reply[AE_MAX_LINE + 1];
    int n = ask(on ? "MOUSE ON B" : "MOUSE OFF", reply, (int)sizeof(reply));

    if (n < 0) return AE_BS_GONE;
    return strcmp(reply, "OK") == 0 ? 1 : 0;
}

int ae_bs_mouse_next(ae_mouse_event *ev)
{
    char reply[AE_MAX_LINE + 1];
    int n = ask("MOUSE?", reply, (int)sizeof(reply));

    if (n < 0) return AE_BS_GONE;
    if (n == 0) return 0;
    return ae_bs_parse_mouse(reply, ev);
}

int ae_bs_mouse_drain(ae_mouse_event *evs, int cap)
{
    ae_mouse_event ev;
    int got, count = 0, i;

    if (!evs || cap < 1) return 0;
    for (i = 0; i < DRAIN_LIMIT; i++) {
        got = ae_bs_mouse_next(&ev);
        if (got < 0) return got;
        if (got == 0) break;
        evs[count < cap ? count : cap - 1] = ev;
        count++;
    }
    return count < cap ? count : cap;
}
