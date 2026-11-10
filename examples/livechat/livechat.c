/*
 * LIVECHAT68 - the federated chat room, in C, on a 68K Amiga.
 *
 * One chat shared by every board with a key, served by the door server at
 * /chat/v1 (see CHAT-API.md). This door is a plain client: it holds no room
 * state of its own beyond the lines on screen, and everything it shows came
 * back from the server as `|`-separated Latin-1 rows.
 *
 * THE SCREEN is the TS LIVECHAT door's, cell for cell (livechat_ui.c draws it
 * from an lc_model). Every key changes the model, lc_draw paints the whole
 * frame into the canvas' back buffer, and ui_cells_flush sends only the cells
 * that differ from what the terminal already shows. A typed letter costs one
 * or two cells on the wire, never a repaint.
 *
 * WHO TALKS. The caller's handle is read from the session at startup and is
 * NEVER typed, so nobody speaks as somebody else.
 *
 * HOW IT STAYS LIVE. There is no thread and no timer on a 68K door. The idle
 * hook of ui_key_read runs while no key is waiting: it sleeps half a second,
 * and every POLL_STEPS sleeps asks /events.txt with wait=0. Keys and mouse
 * wakes do not reset that count, so moving the mouse never starves the poll. A send is
 * followed by one poll at once, and that poll is the only way the caller's
 * own line reaches the pane, so there is one display path, deduplicated by
 * message id.
 *
 * KEYS (as the TS door): Tab / Shift-Tab cycle input, sidebar and menu bar;
 * Enter sends, joins the sidebar room or opens the DM; F1 help, F2 sidebar,
 * F3 next channel; PgUp/PgDn scroll; Ctrl+Q quits (Ctrl+C is the board break key, K09). Commands are the
 * TS ones (/join /leave /create /rooms /who /msg /reply /react /thread /clear /help
 * /quit) and the old short forms (/j /n /np /l /w /d /r /t /? /q).
 *
 * Everything the TS door has is ported. Voice, microphone, video and drawing
 * answer "not on Amiga yet"; Files behaves as the TS door's does (the hub has
 * no file store yet).
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aedoor.h"
#include "ae_board_screen.h"
#include "ae_chat.h"
#include "ae_chatprefs.h"
#include "ae_host.h"
#include "ae_http.h"
#include "ae_net.h"
#include "ae_request.h"
#include "ae_session.h"
#include "ae_usernote.h"
#include "ui_ansi.h"
#include "ui_cells.h"
#include "ui_input.h"
#include "ui_key.h"
#include "ui_screen.h"
#include "ui_theme.h"
#include "livechat_ui.h"

/* Where the key file sits; the host test points it at a file of its own. */
#ifndef LIVECHAT_CFG_PATH
#define LIVECHAT_CFG_PATH "PROGDIR:LIVECHAT.cfg"
#endif

/* Marks a parameter used. vbcc warns on a (void) cast ("statement has no effect"); this does not. */
#define IGNORE(p) do { if ((p) != 0) { } } while (0)

static char frame_buffer[16384];
static char session_storage[AE_SESSION_MIN_STORAGE];
static ui_screen screen;
static ui_key_source keys;
static int pushback = -1;
static ui_cells cells;
static lc_model M;

static ae_chat chat;
static char handle[32];
static int is_self(const char *user);

/* The door sleeps IDLE_SLEEP_TICKS (1/50 s) per idle call, so a key waits at most that long before
   the key loop sees it. It used to sleep 25 ticks (half a second) and every key paid for it. The
   network poll cadence is kept in wall time: a "poll step" is POLL_STEP_TICKS (25 ticks, half a
   second), and the door polls every poll_steps of them: 12 steps is six seconds. The door's only
   clock is the ticks it has slept, so idle_ticks counts those. */
#define IDLE_SLEEP_TICKS 2
#define POLL_STEP_TICKS 25
#define POLL_STEPS_DEFAULT 12
static int poll_steps = POLL_STEPS_DEFAULT;
static int poll_step_ticks = POLL_STEP_TICKS;
static int idle_ticks;

static char room_id[48];
static char room_name[48];
static char room_topic[80];
static char room_motd[512];   /* the room's message of the day (TS chat-socket-handlers caches it on join and room:motd) */
static int room_members;
static char dm_thread[32];
static char dm_peer[24];
static int msg_number;
static int help_shown;          /* times the help overlay opened (the host test reads it) */
static int is_sysop;            /* the caller's level is 255: sysop items in the context menus */
static int user_level;          /* the caller's level: the TS commands' minSecLevel gates */
static int mouse_events;        /* mouse events dispatched (a test sentinel) */
static int bar_drag;            /* left button went down on the chat scrollbar and is still held (R1a) */
static int keep_lines;          /* join_room adds to the pane instead of starting it afresh */

/* The room list from the last rooms.txt, for the sidebar and F3. */
#define ROOMS_MAX 24
static char rooms_id[ROOMS_MAX][48];
static char rooms_name[ROOMS_MAX][40];
static char rooms_kind[ROOMS_MAX][8];     /* text, voice or art (TS commands/chan-admin.ts ROOM_KINDS) */
static int rooms_count;

/* /create's name, topic, visibility and motd while the channel-type picker is open. */
static char pending_room[40];
static char pending_topic[100];
static char pending_motd[128];
static const char *pending_vis = "public";

enum { PROMPT_JOIN, PROMPT_CREATE, PROMPT_NOTE };
static int prompt_kind;
static char note_target[32];                               /* PROMPT_NOTE: who the note is about */

static int door_key(void *ctx) { IGNORE(ctx); return ae_key(); }
static int door_pending(void *ctx) { IGNORE(ctx); return ae_input_pending(); }
static void door_settle(void *ctx) { IGNORE(ctx); ae_delay_ticks(1); }
static void door_flush(void *ctx) { IGNORE(ctx); ui_screen_flush(&screen); }
static void drain(void *ctx) { IGNORE(ctx); ui_screen_flush(&screen); }

static void copy(char *dst, size_t cap, const char *src)
{
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

/** Text from the wire is Latin-1 for the screen: no control bytes, so no canvas marker either. */
static void clean(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    for (; *src != '\0' && n + 1 < cap; src++) {
        unsigned char ch = (unsigned char)*src;
        dst[n++] = (ch < 32 || ch == 127) ? ' ' : (char)ch;
    }
    /* Cut short: never leave half a UTF-8 sequence or half a {colour} / ~effect~ tag at the end. */
    if (*src != '\0') {
        size_t k = n, floor = n > 40 ? n - 40 : 0;
        while (k > floor && dst[k - 1] != '{' && dst[k - 1] != '}') k--;           /* an unclosed {tag} */
        if (k > floor && dst[k - 1] == '{') n = k - 1;
        else {
            size_t t = n;
            while (t > floor && (dst[t - 1] == '-' || (dst[t - 1] >= 'a' && dst[t - 1] <= 'z') || (dst[t - 1] >= '0' && dst[t - 1] <= '9'))) t--;
            if (t > floor && dst[t - 1] == '~') {
                size_t open = 0, i;
                for (i = 0; i < t; i++) if (dst[i] == '~') open++;
                if (open % 2 == 1) n = t - 1;                                       /* an unclosed ~effect */
            }
        }
        {
            size_t lead = n;
            while (lead > 0 && n - lead < 4 && ((unsigned char)dst[lead - 1] & 0xC0) == 0x80) lead--;   /* continuation bytes */
            if (lead > 0 && n - lead < 4) {
                unsigned char b = (unsigned char)dst[lead - 1];
                size_t want = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
                if (want > 1 && n - (lead - 1) < want) n = lead - 1;
            }
        }
    }
    dst[n] = '\0';
}

/** "hh:mm" in local time for an epoch the server sent, seconds or milliseconds. */
static void hhmm_of(char *out, const char *epoch)
{
    char digits[12];
    time_t t;
    struct tm *tm;
    size_t n = strlen(epoch);
    if (n > 10) n = 10;                       /* milliseconds: the first ten digits are the seconds */
    memcpy(digits, epoch, n);
    digits[n] = '\0';
    t = (time_t)atol(digits);
    tm = localtime(&t);
    if (n == 0 || tm == 0) { strcpy(out, "--:--"); return; }
    sprintf(out, "%02d:%02d", tm->tm_hour % 24, tm->tm_min % 60);
}

static void hhmm_now(char *out)
{
    time_t t = time(0);
    struct tm *tm = localtime(&t);
    if (tm == 0) { strcpy(out, "--:--"); return; }
    sprintf(out, "%02d:%02d", tm->tm_hour % 24, tm->tm_min % 60);
}

/* ---- the pane's lines --------------------------------------------------- */

static void lines_clear(void)
{
    M.nlines = 0;
    lc_lines_changed(&M);
    M.scroll = 0;
    msg_number = 0;
}

static lc_line *line_by_id(const char *id)
{
    int i;
    for (i = 0; i < M.nlines; i++) {
        if (M.lines[i].id[0] != '\0' && strcmp(M.lines[i].id, id) == 0) return &M.lines[i];
    }
    return 0;
}

static lc_line *line_by_number(int number)
{
    int i;
    for (i = 0; i < M.nlines; i++) if (M.lines[i].number == number && number > 0) return &M.lines[i];
    return 0;
}

/** Takes the message `id` out of the pane (TS removeMessage: no line says so). */
static void line_remove(const char *id)
{
    int i, j = 0;
    for (i = 0; i < M.nlines; i++) {
        if (M.lines[i].id[0] != '\0' && strcmp(M.lines[i].id, id) == 0) continue;
        if (i != j) M.lines[j] = M.lines[i];
        j++;
    }
    M.nlines = j;
    lc_lines_changed(&M);
}

/** The caller's newest message in the pane (TS lastOwnMessageId), or 0. */
static lc_line *own_last(void)
{
    int i;
    for (i = M.nlines - 1; i >= 0; i--) {
        lc_line *l = &M.lines[i];
        if (l->number > 0 && l->id[0] != '\0' && is_self(l->nick)) return l;
    }
    return 0;
}

/** The newest message in the pane (TS lastRoomMessageId), or 0. */
static lc_line *room_last(void)
{
    int i;
    for (i = M.nlines - 1; i >= 0; i--) {
        if (M.lines[i].number > 0 && M.lines[i].id[0] != '\0') return &M.lines[i];
    }
    return 0;
}

/** "nick: snippet" as TS reply-quote builds it: whitespace collapsed, trimmed, 60 characters kept. */
static void quote_of(const lc_line *p, char *out, int cap)
{
    char snip[61];
    const char *s = p->text;
    int n = 0, gap = 0;
    while (*s == ' ') s++;
    for (; *s != '\0' && n < 60; s++) {
        if (*s == ' ' || *s == '\t') { gap = 1; continue; }
        if (gap && n > 0) { snip[n++] = ' '; if (n == 60) break; }
        gap = 0;
        snip[n++] = *s;
    }
    snip[n] = '\0';
    snprintf(out, (size_t)cap, "%.24s: %s", p->nick, snip);
}

/** Opens a slot at `at`, dropping the oldest line when the pane is full. */
static lc_line *line_slot(int at)
{
    int i;
    lc_lines_changed(&M);                   /* every add and shift of the pane passes here; the caller fills the slot before the next draw */
    if (M.nlines == LC_LINES) {
        for (i = 1; i < M.nlines; i++) M.lines[i - 1] = M.lines[i];
        M.nlines--;
        if (at > 0) at--;
    }
    for (i = M.nlines; i > at; i--) M.lines[i] = M.lines[i - 1];
    M.nlines++;
    memset(&M.lines[at], 0, sizeof M.lines[at]);
    return &M.lines[at];
}

/* ---- ignore and mute: the server stores the ignore list; hiding the lines is this door's job. ---- */
#define HIDE_MAX 24
static char ignored[HIDE_MAX][32];      /* nicks (no @BOARD) the caller ignores: lines and DMs hidden */
static int nignored;
static char muted[HIDE_MAX][32];        /* nicks muted for this run only (TS mute-list): room lines hidden */
static int nmuted;
static char blocked[HIDE_MAX][32];      /* nicks blocked (TS mute-list block): hidden everywhere, server-ignored */
static int nblocked;

/** "alice@BOARD" -> "alice". */
static void nick_of(char *out, size_t cap, const char *user)
{
    size_t n = 0;
    if (*user == '@') user++;
    for (; *user != '\0' && *user != '@' && n + 1 < cap; user++) out[n++] = *user;
    out[n] = '\0';
}

static int same_nick(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

/** The caller, as hub rows name it ("handle@BOARD", chat.user) or as the door's local echo
 *  names it (bare handle). Hub rows never carry a bare name, so the second test only
 *  matches the local echo. */
static int is_self(const char *user)
{
    return (chat.user[0] != '\0' && same_nick(user, chat.user)) || same_nick(user, handle);
}

static int hide_find(char list[][32], int n, const char *nick)
{
    int i;
    for (i = 0; i < n; i++) if (same_nick(list[i], nick)) return i;
    return -1;
}

static void hide_add(char list[][32], int *n, const char *nick)
{
    if (hide_find(list, *n, nick) >= 0 || *n == HIDE_MAX) return;
    nick_of(list[*n], 32, nick);
    (*n)++;
}

static void hide_remove(char list[][32], int *n, const char *nick)
{
    int i = hide_find(list, *n, nick);
    if (i < 0) return;
    for (; i + 1 < *n; i++) memcpy(list[i], list[i + 1], 32);
    (*n)--;
}

/** Lines from someone the caller ignores or muted are not shown (never the caller's own). */
static int hidden_nick(const char *nick)
{
    char n[32];
    nick_of(n, sizeof n, nick);
    if (same_nick(n, handle)) return 0;
    return hide_find(ignored, nignored, n) >= 0 || hide_find(muted, nmuted, n) >= 0
        || hide_find(blocked, nblocked, n) >= 0;
}

/** Reads the server's ignore list (IGNORES rows user|at) into the local one. */
static void load_ignores(void)
{
    char *f[AE_CHAT_ROW_FIELDS];
    nignored = 0;
    if (ae_chat_ignores(&chat) != AE_CHAT_OK) return;
    while (ae_chat_next_row(&chat, f, AE_CHAT_ROW_FIELDS) >= 1) {
        char n[32];
        nick_of(n, sizeof n, f[0]);
        if (hide_find(blocked, nblocked, n) < 0) hide_add(ignored, &nignored, n);   /* a block is server-ignored too */
    }
}

/** A message, inserted by seq among the messages; one already shown (same id) is not shown twice. */
static lc_line *add_message(int kind, long seq, const char *id, const char *nick, const char *at, const char *text,
                            const char *reply_to)
{
    int pos = M.nlines;
    lc_line *l;
    char quote[sizeof l->quote];
    const lc_line *parent;
    if (id[0] != '\0' && line_by_id(id) != 0) return 0;
    if (hidden_nick(nick)) return 0;
    quote[0] = '\0';                                /* TS draws the quote only when the parent is loaded */
    if (reply_to[0] != '\0' && (parent = line_by_id(reply_to)) != 0) quote_of(parent, quote, (int)sizeof quote);
    while (pos > 0 && (M.lines[pos - 1].kind == LC_HISTORY || M.lines[pos - 1].kind == LC_LIVE)
           && M.lines[pos - 1].seq > seq) pos--;
    l = line_slot(pos);
    l->kind = kind;
    l->seq = seq;
    l->number = ++msg_number;
    clean(l->id, sizeof l->id, id);
    clean(l->nick, sizeof l->nick, nick);
    clean(l->text, sizeof l->text, text);
    hhmm_of(l->time, at);
    copy(l->quote, sizeof l->quote, quote);
    M.scroll = 0;
    return l;
}

/** "*** text ***" in grey, or "[hh:mm] text" in yellow: lines of the door's own, at the bottom. */
static void add_note(int kind, const char *text)
{
    lc_line *l = line_slot(M.nlines);
    l->kind = kind;
    l->seq = chat.last_seq;
    clean(l->text, sizeof l->text, text);
    if (kind == LC_ACTIVITY_LINE) hhmm_now(l->time);
    M.scroll = 0;
}

static void system_line(const char *text) { add_note(LC_SYSTEM, text); }

/** TS addSystemMessage("{accentAlt}[MOTD] x{/accentAlt}"): a system line whose text is in the motd colour. */
static void motd_line(const char *text)
{
    add_note(LC_SYSTEM, text);
    M.lines[M.nlines - 1].tone = LC_TK_ACCENT_ALT;
}

/** TS addSystemMessage("{accent}text{/accent}"): a system line in the accent colour. */
static void accent_line(const char *text)
{
    add_note(LC_SYSTEM, text);
    M.lines[M.nlines - 1].tone = LC_TK_ACCENT;
}

/** TS acm(formatDmLine(d)): "[DM to x]: text" in magenta when sent, "[DM from x]: text" in cyan
    when received; a group DM says "Group DM". */
static void dm_line(int sent, int group, const char *who, const char *text)
{
    char out[LC_TEXT_MAX + 1];
    sprintf(out, "[%s%s %.120s]: ", group ? "Group DM" : "DM", sent ? " to" : " from", who);
    strncat(out, text, sizeof out - strlen(out) - 1);
    add_note(LC_NOTE, out);
    M.lines[M.nlines - 1].tone = sent ? LC_DM_SENT : LC_DM_RECEIVED;
}

/** TS addSystemMessage("{alert}text{/alert}"): a system line in the alert colour. */
static void alert_line(const char *text)
{
    add_note(LC_SYSTEM, text);
    M.lines[M.nlines - 1].tone = LC_TK_ALERT;
}

/* ---- modals ------------------------------------------------------------- */

static void message_modal(const char *title, const char *text)
{
    M.modal = LC_MODAL_MESSAGE;
    copy(M.modal_title, sizeof M.modal_title, title);
    copy(M.modal_text, sizeof M.modal_text, text);
}

/** Errors waiting behind the open modal, '|'-separated: TS opens one room:error modal per error,
 *  so each shows after the one before it is closed. */
static char modal_next[200];

static void modal_close(void)
{
    char one[96];
    char *bar;
    int n;
    M.modal = LC_MODAL_NONE;
    if (modal_next[0] == '\0') return;
    bar = strchr(modal_next, '|');
    n = bar != 0 ? (int)(bar - modal_next) : (int)strlen(modal_next);
    if (n > (int)sizeof one - 1) n = (int)sizeof one - 1;
    memcpy(one, modal_next, (size_t)n);
    one[n] = '\0';
    if (bar != 0) memmove(modal_next, bar + 1, strlen(bar + 1) + 1); else modal_next[0] = '\0';
    message_modal("Error", one);
}

/** What the server's error code means to a caller, as the TS door's error modal. */
static void show_error(int rc)
{
    const char *t;
    if (rc == AE_CHAT_ERR_TRANSPORT) t = "Cannot reach the chat server.";
    else if (strcmp(chat.error, "muted") == 0) t = "You are muted in this room.";
    else if (strcmp(chat.error, "moderated") == 0) t = "Room is moderated (+m); you need voice (+v) or op (+o) to speak";
    else if (strcmp(chat.error, "banned") == 0) t = "You are banned from this room.";
    else if (strcmp(chat.error, "not_member") == 0) t = "Join the room first.";
    else if (strcmp(chat.error, "not_found") == 0) t = "Not found.";
    else if (strcmp(chat.error, "invite_required") == 0) t = "That room is private.";
    else if (strcmp(chat.error, "too_long") == 0) t = "Message too long.";
    else if (strcmp(chat.error, "rate_limited") == 0) t = "Slow down a moment.";
    else if (strcmp(chat.error, "forbidden") == 0) t = "Not allowed.";
    else if (strcmp(chat.error, "conflict") == 0) t = "Already sent.";
    else if (chat.detail[0] != '\0') t = chat.detail;    /* the hub's own reason (bad_request and the rest) */
    else t = "The chat server refused that.";
    message_modal("Error", t);
}

static void open_prompt(int kind, const char *question)
{
    prompt_kind = kind;
    M.modal = LC_MODAL_PROMPT;
    copy(M.modal_title, sizeof M.modal_title, question);
    M.prompt[0] = '\0';
    M.prompt_len = 0;
}

/* TS context-menus.ts Add Note: the board stores the note (BBSApi.addUserNote, here AEW user-note). */
static void note_typed(const char *text)
{
    char line[96];
    char note[sizeof M.prompt];
    int r, n;
    while (*text == ' ') text++;                            /* TS (val || '').trim() */
    copy(note, sizeof note, text);
    for (n = (int)strlen(note); n > 0 && note[n - 1] == ' '; ) note[--n] = '\0';
    if (note[0] == '\0') return;
    r = ae_user_note(note_target, note);
    if (r > 0) { sprintf(line, "Note saved for %.30s.", note_target); system_line(line); }
    else if (r == 0) { sprintf(line, "Could not save note for %.30s.", note_target); alert_line(line); }
    else alert_line("Notes are unavailable.");
}

static void open_help(void)
{
    M.help = 1;
    M.help_top = 0;
    help_shown++;
}

/* ---- the screen --------------------------------------------------------- */

static void present(void)
{
    int cr = 0, cc = 0;
    if (M.ntyping > 0) hhmm_now(M.typing_time);
    int caret = M.focus == LC_FOCUS_INPUT && !M.help && !M.view && M.menu < 0 && !M.settings && !M.theme
                && (M.modal == LC_MODAL_NONE || M.modal == LC_MODAL_PROMPT);
    lc_draw(&cells, &M, &cr, &cc);
    ui_cells_flush(&cells, &screen.buf, drain, 0);
    ui_screen_cursor(&screen, caret);
    if (caret) ansi_goto(&screen.buf, cr + 1, cc + 1);
    ui_screen_flush(&screen);
}

/* ---- per-room sidebar state: TS expandedChannels, collapsedChannels, pinned channels, unreadCount ---- */
typedef struct { char id[ROOMS_MAX][48]; int count[ROOMS_MAX]; int n; } id_set;
static id_set pinned_rooms, expanded_rooms, collapsed_rooms, unread_rooms;

static int idset_find(const id_set *s, const char *id)
{
    int i;
    for (i = 0; i < s->n; i++) if (strcmp(s->id[i], id) == 0) return i;
    return -1;
}

static int idset_put(id_set *s, const char *id)
{
    int i = idset_find(s, id);
    if (i >= 0 || s->n == ROOMS_MAX) return i;
    copy(s->id[s->n], sizeof s->id[0], id);
    s->count[s->n] = 0;
    return s->n++;
}

static void idset_drop(id_set *s, const char *id)
{
    int i = idset_find(s, id);
    if (i < 0) return;
    for (; i + 1 < s->n; i++) { memcpy(s->id[i], s->id[i + 1], sizeof s->id[0]); s->count[i] = s->count[i + 1]; }
    s->n--;
}

static int room_unread(const char *id)
{
    int i = idset_find(&unread_rooms, id);
    return i < 0 ? 0 : unread_rooms.count[i];
}

/** TS channelsToShow: pinned rooms first, the order kept inside each group. */
static int room_order(int *ord)
{
    int n = 0, i, pass;
    for (pass = 1; pass >= 0; pass--)
        for (i = 0; i < rooms_count; i++)
            if ((idset_find(&pinned_rooms, rooms_id[i]) >= 0) == pass) ord[n++] = i;
    return n;
}

/* ---- the sidebar -------------------------------------------------------- */

static void side_add(int kind, int joined, const char *label, const char *id)
{
    lc_side *s;
    if (M.nside >= LC_SIDE_MAX) return;
    s = &M.side[M.nside++];
    memset(s, 0, sizeof *s);
    s->kind = kind;
    s->joined = joined;
    clean(s->label, sizeof s->label, label);
    clean(s->id, sizeof s->id, id);
}

/** Presence word -> the sidebar's status letter. */
static char status_char(const char *word)
{
    return strcmp(word, "away") == 0 ? 'a' : strcmp(word, "busy") == 0 ? 'b' : strcmp(word, "dnd") == 0 ? 'd' : 0;
}

static void side_add_user(const char *nick, const char *status, const char *text)
{
    lc_side *s;
    side_add(LC_SIDE_USER, 0, nick, nick);
    if (M.nside == 0 || M.side[M.nside - 1].kind != LC_SIDE_USER) return;
    s = &M.side[M.nside - 1];
    s->status = status_char(status);
    clean(s->note, sizeof s->note, text);
}

static int side_selectable(int i)
{
    return i >= 0 && i < M.nside && M.side[i].kind != LC_SIDE_HEAD && M.side[i].kind != LC_SIDE_BLANK;
}

static void load_sidebar(void);

/** F3 and a click on the status line: the sidebar shows the channels or the online users (TS switchSidebarTab). */
static void switch_tab(int users)
{
    M.side_tab = users;
    load_sidebar();
    system_line(users ? "Switched to users view" : "Switched to channels view");
}

/** Rebuilds the sidebar as the TS door has it: channels, the current room's people under it, then DMs. */
static void load_sidebar(void)
{
    char *f[AE_CHAT_ROW_FIELDS];
    char users[16][24], ustat[16][8], utext[16][28];
    int nusers = 0, i, j, k, nord, ord[ROOMS_MAX], keep = M.side_sel;
    if (room_id[0] != '\0' && ae_chat_who(&chat) == AE_CHAT_OK) {
        while (ae_chat_next_row(&chat, f, AE_CHAT_ROW_FIELDS) >= 2) {     /* user|room|sysop|roleColor|idle */
            if (strcmp(f[1], room_id) == 0 && nusers < 16) {
                clean(ustat[nusers], sizeof ustat[0], f[5]);
                clean(utext[nusers], sizeof utext[0], f[6]);
                clean(users[nusers++], sizeof users[0], f[0]);
            }
        }
    }
    M.nside = 0;
    if (M.side_tab) {                       /* TS users tab: everyone online, not just this room */
        side_add(LC_SIDE_HEAD, 0, "ONLINE USERS", "");
        if (ae_chat_who(&chat) == AE_CHAT_OK) {
            while (ae_chat_next_row(&chat, f, AE_CHAT_ROW_FIELDS) >= 2) {
                char nm[24];
                clean(nm, sizeof nm, f[0]);
                side_add_user(nm, f[5], f[6]);
            }
        }
        M.side_sel = 0;
        return;
    }
    side_add(LC_SIDE_HEAD, 0, "TEXT CHANNELS", "");
    nord = room_order(ord);
    for (k = 0; k < nord; k++) {
        int here, open;
        i = ord[k];
        here = strcmp(rooms_id[i], room_id) == 0;
        /* TS lists Voice rooms under VOICE CHANNELS, a section a telnet caller does not get. */
        if (strcmp(rooms_kind[i], "voice") == 0) continue;
        /* TS isExpanded: a collapsed room is shut; otherwise the active room or one opened by hand. */
        open = idset_find(&collapsed_rooms, rooms_id[i]) >= 0 ? 0
             : (here || idset_find(&expanded_rooms, rooms_id[i]) >= 0);
        side_add(LC_SIDE_ROOM, here, rooms_name[i], rooms_id[i]);
        M.side[M.nside - 1].art = strcmp(rooms_kind[i], "art") == 0;
        M.side[M.nside - 1].open = (char)open;
        M.side[M.nside - 1].unread = room_unread(rooms_id[i]);
        if (here && open) for (j = 0; j < nusers; j++) side_add_user(users[j], ustat[j], utext[j]);
    }
    if (ae_chat_dm_list(&chat) == AE_CHAT_OK) {
        int first = 1;
        while (ae_chat_next_row(&chat, f, AE_CHAT_ROW_FIELDS) >= 2) {     /* threadId|peer|lastAt|unread */
            if (first) { side_add(LC_SIDE_BLANK, 0, "", ""); side_add(LC_SIDE_HEAD, 0, "DIRECT MESSAGES", ""); first = 0; }
            side_add(LC_SIDE_DM, 0, f[1], f[0]);
        }
    }
    M.side_sel = keep;
    if (!side_selectable(M.side_sel)) {
        for (i = 0; i < M.nside; i++) if (M.side[i].kind == LC_SIDE_ROOM && M.side[i].joined) M.side_sel = i;
        if (!side_selectable(M.side_sel)) M.side_sel = 1;
    }
}

static int load_rooms(void)
{
    char *f[AE_CHAT_ROW_FIELDS];
    int rc = ae_chat_rooms(&chat);
    rooms_count = 0;
    if (rc != AE_CHAT_OK) return rc;
    for (;;) {                                                         /* id|name|topic|visibility|members|owner|kind */
        int n = ae_chat_next_row(&chat, f, AE_CHAT_ROW_FIELDS);
        if (n < 5) break;
        if (rooms_count == ROOMS_MAX) continue;
        clean(rooms_id[rooms_count], sizeof rooms_id[0], f[0]);
        clean(rooms_name[rooms_count], sizeof rooms_name[0], f[1]);
        /* A hub from before room types sends no kind: every room was text. */
        clean(rooms_kind[rooms_count], sizeof rooms_kind[0],
              n >= 7 && (strcmp(f[6], "voice") == 0 || strcmp(f[6], "art") == 0) ? f[6] : "text");
        rooms_count++;
    }
    return AE_CHAT_OK;
}

/* ---- events ------------------------------------------------------------- */

static void side_user(const char *user, int add)
{
    int i, j;
    for (i = 0; i < M.nside; i++) {
        if (M.side[i].kind == LC_SIDE_USER && strcmp(M.side[i].id, user) == 0) {
            if (add) return;
            for (j = i + 1; j < M.nside; j++) M.side[j - 1] = M.side[j];
            M.nside--;
            if (M.side_sel >= M.nside) M.side_sel = M.nside - 1;
            return;
        }
    }
    if (!add) return;
    for (i = 0; i < M.nside; i++) {
        if (M.side[i].kind == LC_SIDE_ROOM && M.side[i].joined && M.nside < LC_SIDE_MAX) {
            for (j = M.nside; j > i + 1; j--) M.side[j] = M.side[j - 1];
            M.nside++;
            memset(&M.side[i + 1], 0, sizeof M.side[i + 1]);
            M.side[i + 1].kind = LC_SIDE_USER;
            clean(M.side[i + 1].label, sizeof M.side[i + 1].label, user);
            clean(M.side[i + 1].id, sizeof M.side[i + 1].id, user);
            return;
        }
    }
}

/** Replaces a message's reaction counts with the server's (reactions.txt rows emoji|count|mine): the
 *  TS door gets the full counts with every chat:reaction and keeps them per message the same way. */
static char reactions_loaded[24];      /* the id load_reactions last read: react_on skips a second read */

static void load_reactions(const char *id)
{
    char *f[AE_CHAT_ROW_FIELDS];
    lc_line *l = line_by_id(id);
    if (l == 0 || ae_chat_reactions(&chat, id) != AE_CHAT_OK) return;
    clean(reactions_loaded, sizeof reactions_loaded, id);
    lc_lines_changed(&M);
    l->nreact = 0;
    while (ae_chat_next_row(&chat, f, AE_CHAT_ROW_FIELDS) >= 3) {
        lc_react *r;
        if (atoi(f[1]) <= 0 || l->nreact == LC_REACT_MAX) continue;
        r = &l->react[l->nreact++];
        clean(r->emoji, sizeof r->emoji, f[0]);
        r->count = atoi(f[1]);
        r->mine = atoi(f[2]) != 0;
    }
}

/** A presence event: `text` is "status!statusText" on the wire (CHAT-API.md). The status word never holds a
    '!', so the first one splits it; a '!' in the note stays a '!'. */
static void side_presence(const char *user, const char *text)
{
    char nick[32], status[8], note[28];
    const char *bar = strchr(text, '!');
    size_t n = bar != 0 ? (size_t)(bar - text) : strlen(text);
    int i;
    if (n >= sizeof status) n = sizeof status - 1;
    memcpy(status, text, n);
    status[n] = '\0';
    clean(note, sizeof note, bar != 0 ? bar + 1 : "");
    nick_of(nick, sizeof nick, user);
    for (i = 0; i < M.nside; i++) {
        char id[32];
        if (M.side[i].kind != LC_SIDE_USER) continue;
        nick_of(id, sizeof id, M.side[i].id);
        if (!same_nick(id, nick)) continue;
        M.side[i].status = status_char(status);
        copy(M.side[i].note, sizeof M.side[i].note, note);
    }
}

/** Applies the rows of an events poll (seq|type|room|user|ref|at|color|text). */
static int event_hidden(const char *type);
static void thread_view(const char *msg_id);

/** One event line: `at` (null: no time), `head` in `tone`, `body` in `body_tone` (0: the pen). */
static void add_event(const char *at, int tone, const char *head, int body_tone, const char *body, int pb)
{
    lc_line *l = line_slot(M.nlines);
    l->kind = LC_EVENT;
    l->seq = chat.last_seq;
    if (at != 0) hhmm_of(l->time, at);
    clean(l->head, sizeof l->head, head);
    clean(l->text, sizeof l->text, body);
    l->tone = tone;
    l->body_tone = body_tone;
    l->pb = pb;
    M.scroll = 0;
}

/** " (12.3KB)" for a byte count, "" for none: TS (fileSize / 1024).toFixed(1), in whole numbers for the 68K. */
static void kb_text(char *out, const char *size)
{
    long n = atol(size), tenths = (n * 10 + 512) / 1024;
    if (n <= 0) { out[0] = '\0'; return; }
    sprintf(out, " (%ld.%ldKB)", tenths / 10, tenths % 10);
}

/** A hub "bbs" row (label = node, user, ref = the board's event type, text = its data, form-encoded):
 *  the TS door's two bbs:event listeners. The legacy one (chat-socket-handlers.ts:81) is registered
 *  first (server.ts:2504) and adds the "[EVENT]" feed line and a system line for logins, under the
 *  logins filter; BBSEventHandler.formatEvent (handlers/bbs-event.handler.ts) adds one line for every
 *  type, unfiltered. Guest is skipped by both. */
static void take_bbs_event(const char *at, const char *node, const char *user, const char *type, const char *data)
{
    char head[48], body[LC_TEXT_MAX], a[204], b[204], c[204], kb[24];
    int login = strcmp(type, "user_login") == 0, logout = strcmp(type, "user_logout") == 0;
    if (strcmp(user, "Guest") == 0) return;
    if ((login || logout) && !event_hidden(type)) {
        if (login) sprintf(body, "%.30s has logged in (Node %.6s)", user, node);
        else sprintf(body, "%.30s has logged off", user);
        add_event(0, LC_TK_DIM, "[EVENT] ", login ? LC_EV_GREEN : LC_EV_RED, body, 0);
        system_line(body);
    }
    sprintf(head, "* %.40s", user);
    if (login) {
        ae_req_form_value(data, "location", a, sizeof a);
        sprintf(body, " logged in from %.120s (Node %.6s)", a[0] != '\0' ? a : "Unknown", node);
        add_event(at, LC_EV_OK, head, 0, body, 0);
    } else if (logout) {
        long d;
        ae_req_form_value(data, "duration", a, sizeof a);
        d = atol(a);
        if (d != 0) sprintf(b, " after %ldm %lds", d / 60, d % 60); else b[0] = '\0';
        sprintf(body, " logged out%s (Node %.6s)", b, node);
        add_event(at, LC_TK_ACCENT_ALT, head, 0, body, 0);
    } else if (strcmp(type, "upload") == 0 || strcmp(type, "download") == 0) {
        int up = type[0] == 'u';
        ae_req_form_value(data, "fileName", a, sizeof a);
        ae_req_form_value(data, "fileSize", b, sizeof b);
        kb_text(kb, b);
        ae_req_form_value(data, "conferenceName", c, sizeof c);
        sprintf(body, " %s %.100s%s%s%.80s", up ? "uploaded" : "downloaded", a, kb,
                c[0] == '\0' ? "" : up ? " to " : " from ", c);
        add_event(at, up ? LC_EV_BAR : LC_TK_ACCENT_ALT, head, 0, body, 0);
    } else if (strcmp(type, "door_activity") == 0) {
        int entered;
        ae_req_form_value(data, "action", a, sizeof a);
        entered = strcmp(a, "entered") == 0;
        ae_req_form_value(data, "doorName", b, sizeof b);
        sprintf(body, " %s door %.120s", entered ? "entered" : "exited", b);
        add_event(at, entered ? LC_EV_GREEN : LC_EV_YELLOW, head, 0, body, 0);
    } else if (strcmp(type, "custom_door_event") == 0) {
        int tone = LC_EV_WHITE, score, pb = 0;
        ae_req_form_value(data, "doorName", a, sizeof a);
        ae_req_form_value(data, "eventType", b, sizeof b);
        ae_req_form_value(data, "message", c, sizeof c);
        if (b[0] == '\0') strcpy(b, "event");
        score = strcmp(b, "score") == 0 || strcmp(b, "score_submitted") == 0;
        if (score) tone = LC_EV_YELLOW;
        else if (strstr(b, "create") || strstr(b, "add") || strstr(b, "new")) tone = LC_EV_GREEN;
        else if (strstr(b, "delete") || strstr(b, "remove")) tone = LC_EV_RED;
        else if (strstr(b, "complete") || strstr(b, "done") || strstr(b, "finish")) tone = LC_EV_BLUE;
        else if (strstr(b, "achievement") || strstr(b, "unlock")) tone = LC_EV_YELLOW;
        else if (strstr(b, "update") || strstr(b, "edit") || strstr(b, "move")) tone = LC_EV_CYAN;
        sprintf(head, "[%.40s]", a[0] != '\0' ? a : "Unknown");
        if (strcmp(b, "match_result") == 0) tone = LC_TK_ACCENT_ALT;
        sprintf(body, " %.30s%s%.150s", user, strcmp(b, "match_result") == 0 ? " " : ": ", c[0] != '\0' ? c : "No message");
        if (score) {
            char flag[8];
            ae_req_form_value(data, "isPersonalBest", flag, sizeof flag);
            pb = strcmp(flag, "true") == 0;
        }
        add_event(at, tone, head, 0, body, pb);
    } else {
        sprintf(head, "* Unknown event: %.20s", type);
        add_event(at, LC_TK_DIM, head, 0, "", 0);
    }
}

/* ---- D3 typing indicators (TS chat:keystroke / chat:typing over the hub) ----
   Sender: the whole input buffer goes to POST /rooms/:id/typing, throttled with a leading and a
   trailing edge (TS 400 ms); "" (submit, clear, room switch, exit) always goes at once.
   Receiver: a typing event sets the typer's row, "" drops it, 5 s with no update drops it (TS
   TYPING_STALE_MS). The caller's own buffer is a local echo row, as the TS door draws it. */
static int show_typing = 1;                      /* TS showTypingIndicators: never stored by the TS door either */
#define TYPING_THROTTLE_TICKS 20                 /* 400 ms at 50 ticks/s */
#define TYPING_STALE_TICKS 250                   /* 5000 ms */
#define TYPING_BLINK_TICKS 26                    /* 530 ms */
static char typing_sent[LC_INPUT_MAX];           /* the last buffer the hub has */
static char typing_room[48];                     /* the room it was sent to */
static char typing_seen[LC_INPUT_MAX];           /* the input as typing_sync last saw it */
static int typing_wait, typing_pending, typing_blink_t;
static int typing_age[LC_TYPING_MAX];

static void typing_set(const char *nick, const char *text)
{
    int i, j;
    for (i = 0; i < M.ntyping; i++) if (strcmp(M.typing[i].nick, nick) == 0) break;
    if (text[0] == '\0') {
        if (i == M.ntyping) return;
        for (j = i; j + 1 < M.ntyping; j++) { M.typing[j] = M.typing[j + 1]; typing_age[j] = typing_age[j + 1]; }
        M.ntyping--;
        return;
    }
    if (i == M.ntyping) {
        if (M.ntyping == LC_TYPING_MAX) return;   /* full: the newest typer waits for a free row */
        M.ntyping++;
        copy(M.typing[i].nick, sizeof M.typing[i].nick, nick);
    }
    clean(M.typing[i].text, sizeof M.typing[i].text, text);
    typing_age[i] = 0;
}

static void typing_send(const char *text)
{
    ae_chat_typing(&chat, room_id, text);
    copy(typing_sent, sizeof typing_sent, text);
    copy(typing_room, sizeof typing_room, room_id);
    typing_wait = TYPING_THROTTLE_TICKS;
    typing_pending = 0;
}

/** After each key: the input changed, so update the own row and send (leading edge) or hold (trailing). */
static void typing_sync(void)
{
    if (strcmp(M.input, typing_seen) == 0) return;
    copy(typing_seen, sizeof typing_seen, M.input);
    if (show_typing) typing_set(handle, M.input);
    if (dm_thread[0] != '\0' || room_id[0] == '\0') return;
    if (strcmp(M.input, typing_sent) == 0) { typing_pending = 0; return; }
    if (M.input[0] == '\0' || typing_wait <= 0) typing_send(M.input);
    else typing_pending = 1;
}

/** The idle clock: trailing edge, stale rows, cursor blink. 1 = the frame changed. */
static int typing_tick(int ticks)
{
    int i, changed = 0;
    if (typing_wait > 0) typing_wait -= ticks;
    if (typing_pending && typing_wait <= 0 && dm_thread[0] == '\0' && room_id[0] != '\0') typing_send(M.input);
    for (i = 0; i < M.ntyping; ) {
        if (!is_self(M.typing[i].nick) && (typing_age[i] += ticks) >= TYPING_STALE_TICKS) {
            typing_set(M.typing[i].nick, "");
            changed = 1;
        } else i++;
    }
    if (M.ntyping > 0 && (typing_blink_t += ticks) >= TYPING_BLINK_TICKS) {
        typing_blink_t = 0;
        M.typing_blink = !M.typing_blink;
        changed = 1;
    }
    return changed;
}

/** Exit: the hub hears "stopped" if it still holds a buffer. */
static void typing_stop(void)
{
    if (typing_sent[0] != '\0' && typing_room[0] != '\0') ae_chat_typing(&chat, typing_room, "");
}

/** Room switch: stopped, and the rows belong to the old room. */
static void typing_reset(void)
{
    typing_stop();
    typing_sent[0] = '\0';
    typing_seen[0] = '\0';
    typing_pending = 0;
    M.ntyping = 0;
}

static void take_events(void)
{
    char *f[AE_CHAT_ROW_FIELDS];
    char line[LC_TEXT_MAX];
    char reacted[8][24];       /* messages whose counts changed; fetched after the rows, which share the reply buffer */
    int nreacted = 0, i;
    while (ae_chat_next_row(&chat, f, AE_CHAT_ROW_FIELDS) >= AE_CHAT_ROW_MIN) {
        long seq = atol(f[0]);
        const char *type = f[1], *room = f[2], *user = f[3], *ref = f[4];
        int in_room = dm_thread[0] == '\0' && room_id[0] != '\0' && strcmp(room, room_id) == 0;
        if (event_hidden(type)) continue;
        if (strcmp(type, "message") == 0 && in_room) {
            add_message(LC_LIVE, seq, ref, user, f[5], f[7], f[8]);
        } else if (strcmp(type, "dm") == 0) {
            /* the hub's label column is the bare thread id: "dm<hex>" 1:1, "dg<hex>" group */
            if (dm_thread[0] != '\0' && strcmp(room, dm_thread) == 0) {
                add_message(LC_LIVE, seq, ref, user, f[5], f[7], "");
            } else if (!is_self(user) && !hidden_nick(user)) {
                /* TS chat:dm received: the DM line, then the activity line (group threads are "dg...") */
                int group = strncmp(room, "dg", 2) == 0;
                dm_line(0, group, user, f[7]);
                sprintf(line, "%s from %.24s", group ? "Group DM" : "DM", user);
                add_note(LC_ACTIVITY_LINE, line);
            }
        } else if ((strcmp(type, "react") == 0 || strcmp(type, "unreact") == 0) && in_room) {
            /* TS chat-socket-handlers chat:reaction: an accent chat line and an activity line */
            sprintf(line, "[%.24s %s %.12s]", user, type[0] == 'u' ? "removed" : "reacted", f[7]);
            add_note(LC_NOTE, line);
            sprintf(line, "%.24s: %.12s", user, f[7]);
            add_note(LC_ACTIVITY_LINE, line);
            for (i = 0; i < nreacted && strcmp(reacted[i], ref) != 0; i++) {}
            if (i == nreacted && nreacted < 8 && line_by_id(ref) != 0) clean(reacted[nreacted++], sizeof reacted[0], ref);
        } else if ((strcmp(type, "joined") == 0 || strcmp(type, "left") == 0) && in_room) {
            int joined = type[0] == 'j';
            sprintf(line, "%.24s %s the room", user, joined ? "joined" : "left");
            system_line(line);
            sprintf(line, "%.24s %s", user, joined ? "joined" : "left");
            add_note(LC_ACTIVITY_LINE, line);
            if (!is_self(user)) side_user(user, joined);
        } else if (strcmp(type, "presence") == 0) {
            side_presence(user, f[7]);          /* TS chat:presence: the dot and the text change in the user list */
        } else if (strcmp(type, "edit") == 0) {
            lc_line *l = line_by_id(ref);       /* TS chat:edited: the rows of that id now read the new text */
            if (l != 0) { clean(l->text, sizeof l->text, f[7]); l->edited = 1; lc_lines_changed(&M); }
        } else if (strcmp(type, "delete") == 0) {
            line_remove(ref);                   /* TS chat:deleted: gone, nothing printed */
        } else if (strcmp(type, "topic") == 0 && in_room) {
            clean(room_topic, sizeof room_topic, f[7]);
            sprintf(M.pane_label, "%.40s - %.50s", room_name, room_topic);
            sprintf(line, "%.24s set the topic: %.200s", user[0] != '\0' ? user : "Someone", room_topic);
            system_line(line);
        } else if (strcmp(type, "room_motd") == 0 && in_room) {
            /* TS room:motd: cache it, then "[MOTD] x" or "[MOTD] cleared" */
            clean(room_motd, sizeof room_motd, f[7]);
            if (room_motd[0] != '\0') sprintf(line, "[MOTD] %.200s", room_motd);
            else strcpy(line, "[MOTD] cleared");
            motd_line(line);
        } else if (strcmp(type, "typing") == 0 && in_room) {
            if (!is_self(user) && show_typing) typing_set(user, f[7]);   /* own events: the local echo has them */
        } else if (strcmp(type, "mode") == 0 && in_room && f[7][0] != '\0') {
            /* TS room:mode: {accentAlt}[by] set mode <applied>{/accentAlt} */
            sprintf(line, "[%.24s] set mode %.160s", user[0] != '\0' ? user : "?", f[7]);
            motd_line(line);
        } else if ((strcmp(type, "op") == 0 || strcmp(type, "deop") == 0) && in_room) {
            sprintf(line, "%.24s is %s a room operator", user[0] != '\0' ? user : "Someone", type[0] == 'd' ? "no longer" : "now");
            system_line(line);
        } else if ((strcmp(type, "kicked") == 0 || strcmp(type, "banned") == 0) && is_self(user) && in_room) {
            /* TS chat:kicked / chat:banned reach only the target; the server's kick is per room, so leave the room */
            sprintf(line, "You have been %s%s%.120s", type[0] == 'k' ? "kicked" : "banned", f[7][0] != '\0' ? ": " : "", f[7]);
            system_line(line);
            system_line("Disconnecting...");
            room_id[0] = '\0';
            M.room[0] = '\0';
        } else if (strcmp(type, "muted") == 0 && is_self(user) && in_room) {
            system_line("You have been muted");
        } else if (strcmp(type, "bbs") == 0) {
            take_bbs_event(f[5], room, user, ref, f[7]);
        } else if (strcmp(type, "board_joined") == 0 && f[7][0] != '\0') {
            /* hub board_joined (no room): a new board registered; web chat says the same (chat-session-bridge) */
            sprintf(line, "%.80s joined the federation", f[7]);
            system_line(line);
        }
    }
    for (i = 0; i < nreacted; i++) load_reactions(reacted[i]);
}

/* TS server.ts disconnection modal: MAX is doorSettings.reconnectAttempts (default 3); the C door has no such setting. */
#define RECONNECT_MAX 3
#define RECONNECT_REASON "Cannot reach the chat server."

static void reconnect_open(const char *reason, int attempt)
{
    M.modal = LC_MODAL_RECONNECT;
    copy(M.modal_text, sizeof M.modal_text, reason);
    M.modal_attempt = attempt;
    M.modal_max = RECONNECT_MAX;
}

/** A poll that fails opens the disconnection modal; polling stays off while it is open. */
static void do_poll(void)
{
    int rc;
    if (M.modal == LC_MODAL_RECONNECT) return;
    rc = ae_chat_poll(&chat, 0);
    if (rc == AE_CHAT_OK) take_events();
    else if ((rc == AE_CHAT_ERR_TRANSPORT || rc == AE_CHAT_ERR_STATUS)
             && (M.modal == LC_MODAL_NONE || M.modal == LC_MODAL_MESSAGE)) reconnect_open(RECONNECT_REASON, 1);
}

/** Retry: one poll. Returns 1 when the door should end (past the maximum). */
static int reconnect_retry(void)
{
    int rc;
    system_line("Attempting to reconnect...");
    rc = ae_chat_poll(&chat, 0);
    if (rc == AE_CHAT_OK) {
        M.modal = LC_MODAL_NONE;
        take_events();
        add_note(LC_SYSTEM, "Reconnected to server!");
        M.lines[M.nlines - 1].tone = LC_TK_OK;
        return 0;
    }
    if (M.modal_attempt >= M.modal_max) {
        M.modal = LC_MODAL_NONE;
        alert_line("Maximum reconnection attempts reached. Please restart LiveChat.");
        present();
        ae_delay_ticks(100);                         /* TS exits after 2 s */
        return 1;
    }
    reconnect_open("Reconnection failed", M.modal_attempt + 1);
    return 0;
}

/** Reacts and shows the result. The server keeps one row per user, emoji and message and sends a
 *  react event only when it added one, so a repeat comes back OK with no event; the counts are read
 *  back here when the poll did not already read them, else nothing on screen moves. */
static int react_on(const char *msg_id, const char *emoji)
{
    char id[24];
    int count = 0, rc;
    clean(id, sizeof id, msg_id);            /* the poll may move M.lines under the caller's row */
    rc = ae_chat_react(&chat, id, emoji, &count);
    if (rc != AE_CHAT_OK) return rc;
    reactions_loaded[0] = '\0';
    do_poll();
    if (strcmp(reactions_loaded, id) != 0) load_reactions(id);
    return AE_CHAT_OK;
}

/* ---- rooms and DMs ------------------------------------------------------ */

/** Reads history rows (seq|id|user|at|replyTo|threadId|roleColor|text), after a "--- n earlier ---" marker. */
static void take_history(void)
{
    char *f[AE_CHAT_ROW_FIELDS];
    char note[48];
    int marker = M.nlines, n = 0;
    system_line("--- 0 earlier messages ---");
    while (ae_chat_next_row(&chat, f, AE_CHAT_ROW_FIELDS) >= AE_CHAT_ROW_MIN) {
        if (add_message(LC_HISTORY, atol(f[0]), f[1], f[2], f[3], f[7], f[4]) != 0) n++;
    }
    if (n == 0) {                                  /* nothing earlier: no marker */
        int i;
        for (i = marker + 1; i < M.nlines; i++) M.lines[i - 1] = M.lines[i];
        M.nlines--;
        lc_lines_changed(&M);
        return;
    }
    sprintf(note, "--- %d earlier message%s ---", n, n == 1 ? "" : "s");
    copy(M.lines[marker].text, sizeof M.lines[marker].text, note);
    lc_lines_changed(&M);
}

/** Joins a room by id and shows it; the room left is the one before, once the new join worked. */
/** Room names compare without case, as TS findRoom does. */
static int same_name(const char *a, const char *b)
{
    while (*a != '\0' && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return tolower((unsigned char)*a) == tolower((unsigned char)*b);
}

/** The type of a listed room, by id or name; "text" for one the list does not hold. */
static const char *room_kind_of(const char *id)
{
    int i;
    for (i = 0; i < rooms_count; i++)
        if (strcmp(rooms_id[i], id) == 0 || same_name(rooms_name[i], id)) return rooms_kind[i];
    return "text";
}

#define LC_JOIN_REFUSED (-100)   /* a Voice or Art room: the door has neither yet */

/** TS opens the voice grid or the drawing canvas; telnet has neither yet (spot 2026-11-09). */
static int refuse_kind(const char *kind)
{
    system_line(strcmp(kind, "voice") == 0 ? "Voice channels are not on Amiga yet." : "Art channels are not on Amiga yet.");
    return LC_JOIN_REFUSED;
}

static int join_room(const char *id)
{
    char name[48], topic[80], motd[512], line[128], old[48];
    int members = 0, rc;
    const char *kind = room_kind_of(id);
    if (strcmp(kind, "text") != 0) return refuse_kind(kind);
    copy(old, sizeof old, room_id);
    rc = ae_chat_join(&chat, id, name, sizeof name, topic, sizeof topic, motd, sizeof motd, &members);
    if (rc != AE_CHAT_OK) { show_error(rc); return rc; }
    if (old[0] != '\0' && strcmp(old, id) != 0) { typing_reset(); ae_chat_leave(&chat, old); }
    clean(room_id, sizeof room_id, id);
    clean(room_name, sizeof room_name, name);
    clean(room_topic, sizeof room_topic, topic);
    clean(room_motd, sizeof room_motd, motd);      /* TS join caches the motd and prints nothing */
    room_members = members;
    dm_thread[0] = '\0';
    copy(M.room, sizeof M.room, room_name);
    if (room_topic[0] != '\0') sprintf(M.pane_label, "%.40s - %.50s", room_name, room_topic);
    else copy(M.pane_label, sizeof M.pane_label, room_name);
    if (!keep_lines) lines_clear();
    sprintf(line, "Joined #%.40s (%d users)", room_name, room_members);
    system_line(line);
    sprintf(line, "Joined #%.40s", room_name);
    add_note(LC_ACTIVITY_LINE, line);
    if (ae_chat_history(&chat, room_id, 0, 30) == AE_CHAT_OK) take_history();
    idset_drop(&unread_rooms, room_id);            /* the room is read once it is open */
    load_sidebar();
    return AE_CHAT_OK;
}

static void leave_room(void)
{
    char line[80];
    if (room_id[0] == '\0') { system_line("You are not in a channel."); return; }
    ae_chat_leave(&chat, room_id);
    sprintf(line, "Left #%.40s", room_name);
    room_id[0] = '\0';
    M.room[0] = '\0';
    M.pane_label[0] = '\0';
    lines_clear();
    system_line(line);
    load_sidebar();
}

static void open_dm(const char *thread, const char *peer)
{
    char line[64];
    clean(dm_thread, sizeof dm_thread, thread);
    clean(dm_peer, sizeof dm_peer, peer);
    sprintf(M.pane_label, "@%.30s - Direct message", dm_peer);
    lines_clear();
    sprintf(line, "Direct messages with %.24s", dm_peer);
    system_line(line);
    if (ae_chat_dm_history(&chat, dm_thread, 0, 30) == AE_CHAT_OK) take_history();
}

static void next_channel(void)
{
    int i;
    if (rooms_count == 0) return;
    for (i = 0; i < rooms_count; i++) if (strcmp(rooms_id[i], room_id) == 0) break;
    join_room(rooms_id[(i + 1) % rooms_count]);
}

/** Makes a room of `kind` and walks into it. The server joins by id, so the id comes from the create reply.
 *  TS room:create carries the motd; the hub sets it with PUT /rooms/:id/motd once the room exists. */
static void new_room(const char *name, const char *topic, const char *visibility, const char *kind, const char *motd)
{
    char id[sizeof room_id], line[80];
    int rc = ae_chat_room_create(&chat, name, topic, visibility, kind, id, (int)sizeof id);
    if (rc != AE_CHAT_OK) { show_error(rc); return; }
    if (motd[0] != '\0' && (rc = ae_chat_room_motd(&chat, id, motd)) != AE_CHAT_OK) show_error(rc);
    load_rooms();
    /* The kind is known here; a stale room list must not let a Voice or Art room be joined. */
    rc = strcmp(kind, "text") != 0 ? refuse_kind(kind) : join_room(id);
    if (rc == AE_CHAT_OK) sprintf(line, "Created #%.40s", room_name);
    else if (rc == LC_JOIN_REFUSED) sprintf(line, "Created #%.40s", name);
    else return;
    system_line(line);
}

static void ctx_open(int kind, int target, int row, int col);
static void kind_cancelled(void);
static void arg_word(const char *text, char *out, size_t cap);
static void usage(const char *text);

/** TS server.ts pickRoomKind: every new channel gets a type, asked before it reaches the hub. */
static void ask_room_kind(const char *name, const char *topic, const char *visibility, const char *motd)
{
    clean(pending_room, sizeof pending_room, name);
    clean(pending_topic, sizeof pending_topic, topic);
    clean(pending_motd, sizeof pending_motd, motd);
    pending_vis = visibility;
    system_line("Channel type: Text, Voice or Art?");
    ctx_open(LC_CTX_KIND, 0, M.rows / 2 - 2, M.cols / 2 - 6);
}

/** /n, /np and the New room prompt: the whole text is the name (it may have spaces), --type skips the question. */
static void create_typed(const char *text, const char *visibility)
{
    char name[sizeof pending_room], kind[16];
    const char *flag = strstr(text, "--type");
    int n;
    kind[0] = '\0';
    if (flag != 0) {
        arg_word(flag + 6, kind, sizeof kind);
        if (strcmp(kind, "text") != 0 && strcmp(kind, "voice") != 0 && strcmp(kind, "art") != 0) {
            usage("/create #name [--type text|voice|art]");
            return;
        }
    }
    n = flag != 0 ? (int)(flag - text) : (int)strlen(text);
    if (n > (int)sizeof name - 1) n = (int)sizeof name - 1;
    memcpy(name, text, (size_t)n);
    name[n] = '\0';
    while (n > 0 && name[n - 1] == ' ') name[--n] = '\0';
    if (name[0] == '#') memmove(name, name + 1, strlen(name));
    if (name[0] == '\0') { usage("/create #name [--type text|voice|art]"); return; }
    if (kind[0] != '\0') new_room(name, "", visibility, kind, "");
    else ask_room_kind(name, "", visibility, "");
}

#define CREATE_USAGE "/create #name [topic] [--type text|voice|art] [--private] [--invite-only] [--motd \"text\"]"

/** TS commands/chan-admin.ts createCmd: the first word is the name, the other plain words the topic.
 *  --private and --invite-only both make the room members-only (the hub has only public and private),
 *  --type skips the question, --motd takes the words up to the next flag. */
static void create_flags(const char *text)
{
    char name[sizeof pending_room], topic[sizeof pending_topic], motd[sizeof pending_motd], kind[16], w[128];
    const char *p = text, *vis = "public";
    arg_word(p, w, sizeof w);
    if (w[0] == '\0') { usage(CREATE_USAGE); return; }
    clean(name, sizeof name, w[0] == '#' ? w + 1 : w);
    topic[0] = motd[0] = kind[0] = '\0';
    for (;;) {
        while (*p == ' ') p++;
        while (*p != '\0' && *p != ' ') p++;                   /* past the word just read */
        arg_word(p, w, sizeof w);
        if (w[0] == '\0') break;
        if (strcmp(w, "--private") == 0 || strcmp(w, "--invite-only") == 0) vis = "private";
        else if (strcmp(w, "--type") == 0) {
            int i;
            while (*p == ' ') p++;
            while (*p != '\0' && *p != ' ') p++;
            arg_word(p, kind, sizeof kind);
            for (i = 0; kind[i] != '\0'; i++) kind[i] = (char)tolower((unsigned char)kind[i]);
            if (strcmp(kind, "text") != 0 && strcmp(kind, "voice") != 0 && strcmp(kind, "art") != 0) {
                system_line("Error: Channel type must be text, voice or art.");
                return;
            }
        } else if (strcmp(w, "--motd") == 0) {
            const char *q;
            size_t n;
            while (*p == ' ') p++;
            while (*p != '\0' && *p != ' ') p++;
            while (*p == ' ') p++;
            q = strstr(p, " --");
            n = q != 0 ? (size_t)(q - p) : strlen(p);
            if (n > sizeof motd - 1) n = sizeof motd - 1;
            memcpy(motd, p, n);
            motd[n] = '\0';
            while (n > 0 && motd[n - 1] == ' ') motd[--n] = '\0';
            if (n > 0 && (motd[n - 1] == '"' || motd[n - 1] == '\'')) motd[--n] = '\0';
            if (motd[0] == '"' || motd[0] == '\'') memmove(motd, motd + 1, strlen(motd));
            p += q != 0 ? (size_t)(q - p) : strlen(p);
            continue;
        } else if (strlen(topic) + strlen(w) + 2 <= sizeof topic) {
            if (topic[0] != '\0') strcat(topic, " ");
            strcat(topic, w);
        }
    }
    if (kind[0] != '\0') {
        sprintf(w, "Creating #%.40s...", name);
        system_line(w);
        new_room(name, topic, vis, kind, motd);
    } else ask_room_kind(name, topic, vis, motd);
}

static void list_rooms(void)
{
    char line[96];
    int k, i, ord[ROOMS_MAX], rc = load_rooms();
    if (rc != AE_CHAT_OK) { show_error(rc); return; }
    sprintf(line, "Found %d room%s", rooms_count, rooms_count == 1 ? "" : "s");
    system_line(line);
    for (k = room_order(ord), i = 0; i < k; i++) {
        sprintf(line, "#%.30s (%.40s)", rooms_name[ord[i]], rooms_id[ord[i]]);
        system_line(line);
    }
    load_sidebar();
}

static void list_who(void)
{
    char *f[AE_CHAT_ROW_FIELDS];
    char line[128];
    int rc = ae_chat_who(&chat), n = 0;
    if (rc != AE_CHAT_OK) { show_error(rc); return; }
    system_line("Who is on");
    while (ae_chat_next_row(&chat, f, AE_CHAT_ROW_FIELDS) >= 5) {
        sprintf(line, "%.24s in #%.24s%s, idle %ds", f[0], f[1], atoi(f[2]) ? " (sysop)" : "", atoi(f[4]));
        system_line(line);
        n++;
    }
    if (n == 0) system_line("Nobody is on");
}

/* ---- commands ----------------------------------------------------------- */

/** Returns the rest of `text` after one word, or "". */
static const char *after_word(const char *text)
{
    while (*text != '\0' && *text != ' ') text++;
    while (*text == ' ') text++;
    return text;
}

static void arg_word(const char *text, char *out, size_t cap)
{
    size_t n = 0;
    while (*text == ' ') text++;
    while (*text != '\0' && *text != ' ' && n + 1 < cap) out[n++] = *text++;
    out[n] = '\0';
}

static int is_cmd(const char *cmd, const char *a, const char *b)
{
    return strcmp(cmd, a) == 0 || (b != 0 && strcmp(cmd, b) == 0);
}

static void join_typed(const char *text)
{
    char a[48];
    arg_word(text, a, sizeof a);
    if (a[0] == '#') memmove(a, a + 1, strlen(a));
    if (a[0] == '\0') { system_line("Usage: /join room"); return; }
    join_room(a);
}

/* ---- B2: message edit/delete, topic, moderation, invites, reactions (TS commands/) ---- */

static int digits(const char *s)
{
    if (*s == '\0') return 0;
    for (; *s != '\0'; s++) if (*s < '0' || *s > '9') return 0;
    return 1;
}

/** TS commands return `Usage: ...` as an error: the door prints `Error: <text>`. */
static void usage(const char *text)
{
    char t[120];
    sprintf(t, "Error: Usage: %.100s", text);
    system_line(t);
}

/** TS minSecLevel: below it the command answers `Permission denied. Need level N`. */
static int need_level(int level)
{
    char t[64];
    if (user_level >= level) return 1;
    sprintf(t, "Error: Permission denied. Need level %d", level);
    system_line(t);
    return 0;
}

/** `@name` / `name` as the server names a user: handle@BOARD, our own board when none is given.
 *  `shown` (may be NULL) gets the name without the `@` prefix, as the TS lines print it. */
static void target_user(const char *arg, char *out, size_t cap, char *shown, size_t shown_cap)
{
    const char *suffix = strchr(chat.user, '@');
    if (*arg == '@') arg++;
    if (shown != 0) copy(shown, shown_cap, arg);
    if (strchr(arg, '@') != 0 || suffix == 0) copy(out, cap, arg);
    else { copy(out, cap, arg); if (strlen(out) + strlen(suffix) < cap) strcat(out, suffix); }
}

/* ---- presence, whois, ignore (TS commands/user.ts, commands/ui.ts, whois-render.ts, profile-overlay.ts) ---- */

/** One PROFILE row: user|nick|board|status|statusText|firstSeen|lastSeen|idleSeconds|online|sharedRooms. */
typedef struct {
    char nick[32], board[24], status[10], text[64], last[8], rooms[96];
    int idle, online;
} profile_info;

/** Reads a user's profile. AE_CHAT_OK, or the error (not_found: the server never saw the user). */
static int profile_fetch(const char *user, profile_info *p)
{
    char *f[AE_CHAT_ROW_FIELDS];
    char full[64];
    int rc;
    target_user(user, full, sizeof full, 0, 0);
    rc = ae_chat_profile(&chat, full);
    if (rc != AE_CHAT_OK) return rc;
    if (ae_chat_next_row(&chat, f, AE_CHAT_ROW_FIELDS) < 9) return AE_CHAT_ERR_ARGS;
    clean(p->nick, sizeof p->nick, f[1]);
    clean(p->board, sizeof p->board, f[2]);
    clean(p->status, sizeof p->status, f[3]);
    clean(p->text, sizeof p->text, f[4]);
    hhmm_of(p->last, f[6]);
    p->idle = atoi(f[7]);
    p->online = atoi(f[8]);
    clean(p->rooms, sizeof p->rooms, f[9]);
    return AE_CHAT_OK;
}

/** Room ids of a profile's sharedRooms (comma separated) as the names this door knows them by. */
static void room_names_of(const char *ids, char *out, size_t cap)
{
    char one[48];
    size_t n = 0;
    out[0] = '\0';
    while (*ids != '\0') {
        size_t k = 0;
        int i;
        const char *name;
        while (*ids != '\0' && *ids != ',' && k + 1 < sizeof one) one[k++] = *ids++;
        one[k] = '\0';
        if (*ids == ',') ids++;
        name = one;
        for (i = 0; i < rooms_count; i++) if (strcmp(rooms_id[i], one) == 0) name = rooms_name[i];
        if (n + strlen(name) + 3 >= cap) break;
        if (n > 0) { out[n++] = ','; out[n++] = ' '; }
        strcpy(out + n, name);
        n += strlen(name);
    }
}

/** /whois and the Whois menu entry: the profile as pane lines. */
static void whois_typed(const char *arg)
{
    profile_info p;
    char line[160], rooms[96], who[32];
    int rc;
    nick_of(who, sizeof who, arg);
    if (who[0] == '\0') { usage("/whois @user"); return; }
    rc = profile_fetch(arg, &p);
    if (rc != AE_CHAT_OK) {
        if (rc == AE_CHAT_ERR_TRANSPORT) { show_error(rc); return; }
        sprintf(line, "User %.30s is not online.", who);       /* TS formatWhois, nobody found */
        system_line(line);
        return;
    }
    sprintf(line, "%.30s - %.20s%s%.60s", p.nick, p.status, p.text[0] != '\0' ? ": " : "", p.text);
    system_line(line);
    sprintf(line, "Board %.20s, %s, last seen %.5s, idle %ds", p.board, p.online ? "online" : "offline", p.last, p.idle);
    system_line(line);
    room_names_of(p.rooms, rooms, sizeof rooms);
    if (rooms[0] != '\0') { sprintf(line, "Shared rooms: %.100s", rooms); system_line(line); }
}

/** The profile overlay (TS overlays/profile-overlay.ts): a box titled with the name. */
static void profile_overlay(const char *arg)
{
    profile_info p;
    char text[96], who[32];
    int rc;
    nick_of(who, sizeof who, arg);
    rc = profile_fetch(arg, &p);
    if (rc != AE_CHAT_OK) {
        sprintf(text, "User %.40s not found.", who);               /* TS showProfile, nobody found */
        message_modal("Profile", text);
        return;
    }
    sprintf(text, "%.16s%s%.30s, %s, idle %ds", p.status, p.text[0] != '\0' ? ": " : "", p.text,
            p.online ? "online" : "offline", p.idle);
    message_modal(p.nick, text);
}

static void away_typed(const char *rest)
{
    char line[160];
    const char *msg = *rest != '\0' ? rest : "Away";
    int rc = ae_chat_presence(&chat, "away", msg);
    if (rc != AE_CHAT_OK) { show_error(rc); return; }
    sprintf(line, "You are now away: %.100s", msg);
    system_line(line);
    do_poll();
}

static void back_typed(void)
{
    int rc = ae_chat_presence_clear(&chat);
    if (rc != AE_CHAT_OK) { show_error(rc); return; }
    system_line("You are now online");
    do_poll();
}

static void status_typed(const char *rest)
{
    char word[12], line[160];
    const char *text;
    int rc;
    arg_word(rest, word, sizeof word);
    if (word[0] == '\0') strcpy(word, "online");
    if (strcmp(word, "online") != 0 && strcmp(word, "away") != 0 && strcmp(word, "busy") != 0 && strcmp(word, "dnd") != 0) {
        usage("/status [online|away|busy|dnd] [message]");
        return;
    }
    text = after_word(rest);
    rc = ae_chat_presence(&chat, word, text);
    if (rc != AE_CHAT_OK) { show_error(rc); return; }
    sprintf(line, "Status set to %s%s%.100s", word, *text != '\0' ? ": " : "", text);
    system_line(line);
    do_poll();
}

/** Ignore (on) or lift an ignore (off) for `arg`; the lines say what the TS door says. */
static void ignore_set(const char *arg, int on)
{
    char full[64], nick[32], line[128];
    int rc;
    nick_of(nick, sizeof nick, arg);
    if (on && hide_find(ignored, nignored, nick) >= 0) { sprintf(line, "Already ignoring %.30s.", nick); system_line(line); return; }
    if (!on && hide_find(ignored, nignored, nick) < 0) { sprintf(line, "You are not ignoring %.30s.", nick); system_line(line); return; }
    target_user(arg, full, sizeof full, 0, 0);
    rc = ae_chat_ignore(&chat, full, on);
    if (rc != AE_CHAT_OK) { show_error(rc); return; }
    if (on) {
        hide_remove(muted, &nmuted, nick);                      /* the level is set, replacing a mute */
        hide_add(ignored, &nignored, nick);
        sprintf(line, "Ignoring %.30s - their messages and DMs are hidden.", nick);
    } else {
        hide_remove(ignored, &nignored, nick);
        sprintf(line, "%.30s is no longer hidden.", nick);
    }
    system_line(line);
}

static void ignore_typed(const char *rest)
{
    char a[40], line[400];
    int i;
    arg_word(rest, a, sizeof a);
    if (a[0] != '\0') { ignore_set(a, 1); return; }
    load_ignores();
    if (nignored == 0) { system_line("You are not ignoring anyone."); return; }
    strcpy(line, "Ignoring: ");
    for (i = 0; i < nignored && strlen(line) + 34 < sizeof line; i++) {
        if (i > 0) strcat(line, ", ");
        strcat(line, ignored[i]);
    }
    system_line(line);
}

static void unignore_typed(const char *rest)
{
    char a[40];
    arg_word(rest, a, sizeof a);
    if (a[0] == '\0') { usage("/unignore <user>"); return; }
    ignore_set(a, 0);
}

/* TS core/mute-list.ts: one level per user; mute is this door's, ignore and block the server enforces. */
enum { LV_NONE, LV_MUTE, LV_IGNORE, LV_BLOCK };

static int level_of(const char *nick)
{
    return hide_find(blocked, nblocked, nick) >= 0 ? LV_BLOCK
         : hide_find(ignored, nignored, nick) >= 0 ? LV_IGNORE
         : hide_find(muted, nmuted, nick) >= 0 ? LV_MUTE : LV_NONE;
}

/** The Mute/Ignore/Block menu entries (TS toggleMute): the same level again lifts it. Crossing into or out of
    ignore/block tells the server, as TS chat:ignore / chat:unignore; the line is TS muteMessage. */
static void level_toggle(const char *arg, int level)
{
    char nick[32], full[64], line[128];
    int before, now;
    nick_of(nick, sizeof nick, arg);
    before = level_of(nick);
    now = before == level ? LV_NONE : level;
    if ((before >= LV_IGNORE) != (now >= LV_IGNORE)) {
        int rc;
        target_user(arg, full, sizeof full, 0, 0);
        rc = ae_chat_ignore(&chat, full, now >= LV_IGNORE);
        if (rc != AE_CHAT_OK) { show_error(rc); return; }
    }
    hide_remove(muted, &nmuted, nick);
    hide_remove(ignored, &nignored, nick);
    hide_remove(blocked, &nblocked, nick);
    if (now == LV_MUTE) hide_add(muted, &nmuted, nick);
    else if (now == LV_IGNORE) hide_add(ignored, &nignored, nick);
    else if (now == LV_BLOCK) hide_add(blocked, &nblocked, nick);
    if (now == LV_NONE) sprintf(line, "%.30s is no longer hidden.", nick);
    else if (now == LV_MUTE) sprintf(line, "Muted %.30s - their room messages are hidden.", nick);
    else if (now == LV_IGNORE) sprintf(line, "Ignoring %.30s - their messages and DMs are hidden.", nick);
    else {
        sprintf(line, "Blocked %.30s for you - they are hidden everywhere, but the server does not yet stop them sending.", nick);
        alert_line(line);
        return;
    }
    system_line(line);
}

/** The message a typed number names (the pane's numbers, as /react), or a line saying there is none. */
static lc_line *typed_message(const char *a)
{
    lc_line *l = line_by_number(atoi(a));
    if (l == 0 || l->id[0] == '\0') { system_line("No such message number."); return 0; }
    return l;
}

/** After a call that answers with an event: poll it in, or say why the server refused. */
static void then_poll(int rc)
{
    if (rc == AE_CHAT_OK) do_poll(); else show_error(rc);
}

static ui_hist hist;            /* sent lines: Up / Down recall (TS features/input-history.ts) */

/** A line to the room (TS sendRoomLine): history keeps it under its clientId, renamed to the server's
 *  id once the send reply carries it (TS room:message:ack -> inputHistory.rekey). */
static void send_room_line(const char *text, const char *reply_to)
{
    int rc = ae_chat_send(&chat, room_id, text, reply_to, 0);
    if (chat.client_id[0] != '\0') {
        ui_hist_add_id(&hist, chat.client_id, text);
        if (chat.sent_id[0] != '\0') ui_hist_rekey(&hist, chat.client_id, chat.sent_id);
    }
    if (rc == AE_CHAT_OK) do_poll(); else show_error(rc);
}

static void edit_typed(const char *rest)
{
    char a[16];
    const char *text = rest;
    lc_line *l;
    arg_word(rest, a, sizeof a);
    if (*rest == '\0') { usage("/edit [msg-id] new text"); return; }
    if (digits(a) && *after_word(rest) != '\0') {          /* TS: an id only with two or more words */
        if ((l = typed_message(a)) == 0) return;
        text = after_word(rest);
    } else if ((l = own_last()) == 0) {
        system_line("No message to edit: name one with its id, or send a line first.");
        return;
    }
    then_poll(ae_chat_edit(&chat, l->id, text));
}

/** /reply [msg-id] text (alias r), /thread msg-id text (alias t): TS sends both as a reply line. */
static void reply_typed(const char *rest, int thread)
{
    char a[16];
    const char *text = rest;
    lc_line *l;
    arg_word(rest, a, sizeof a);
    if (thread && digits(a) && *after_word(rest) == '\0') {          /* TS: an id alone opens the thread view */
        if ((l = typed_message(a)) != 0) thread_view(l->id);
        return;
    }
    if (thread && (a[0] == '\0' || *after_word(rest) == '\0')) { usage("/thread <msg-id> message"); return; }
    if (*rest == '\0') { usage("/reply [msg-id] message"); return; }
    if (digits(a) && *after_word(rest) != '\0') {          /* TS: an id only with two or more words */
        if ((l = typed_message(a)) == 0) return;
        text = after_word(rest);
    } else if (thread) { usage("/thread <msg-id> message"); return; }
    else if ((l = room_last()) == 0) {
        system_line("No message to reply to: name one with its id.");
        return;
    }
    if (room_id[0] == '\0') { system_line("Join a room first."); return; }
    send_room_line(text, l->id);
}

static void delete_typed(const char *rest)
{
    char a[16];
    lc_line *l;
    arg_word(rest, a, sizeof a);
    if (a[0] != '\0' && !digits(a)) { usage("/deletemsg [msg-id]"); return; }
    if (a[0] != '\0') { if ((l = typed_message(a)) == 0) return; }
    else if ((l = own_last()) == 0) {
        system_line("No message to delete: name one with its id, or send a line first.");
        return;
    }
    then_poll(ae_chat_delete(&chat, l->id));
}

/** /kick /ban /mute /unmute /unban /op /deop: TS mod-kick.ts and mod-ban.ts. */
static void moderate_typed(const char *cmd, const char *rest)
{
    char a[48], user[64], shown[48], line[96];
    const char *action = cmd, *use, *reason = "";
    int level, minutes = 0, rc;
    if (strcmp(cmd, "kick") == 0) { level = 80; use = "/kick @user [reason]"; }
    else if (strcmp(cmd, "mute") == 0) { level = 80; use = "/mute @user [minutes]"; }
    else if (strcmp(cmd, "unmute") == 0) { level = 80; use = "/unmute @user"; }
    else if (strcmp(cmd, "ban") == 0) { level = 100; use = "/ban @user [reason]"; }
    else if (strcmp(cmd, "unban") == 0) { level = 100; use = "/unban @user"; }
    else { level = 255; use = "/op @user  |  /op -@user  |  /deop @user"; }
    if (!need_level(level)) return;
    arg_word(rest, a, sizeof a);
    if (level == 255 && a[0] == '-') { action = "deop"; memmove(a, a + 1, strlen(a)); }
    if (a[0] == '\0' || strcmp(a, "@") == 0) { usage(use); return; }
    target_user(a, user, sizeof user, shown, sizeof shown);
    rest = after_word(rest);
    if (strcmp(cmd, "kick") == 0 || strcmp(cmd, "ban") == 0) reason = *rest != '\0' ? rest : "No reason given";
    if (strcmp(cmd, "mute") == 0) minutes = *rest != '\0' ? atoi(rest) : 5;
    rc = ae_chat_moderate(&chat, room_id, action, user, reason, minutes);
    if (rc == AE_CHAT_ERR_STATUS && strcmp(chat.error, "not_found") == 0) {
        sprintf(line, "User %.40s not found", shown);
        system_line(line);
    } else then_poll(rc);
}

/** /invite /uninvite @user [#room]: TS mode.ts; a #room is a room name, else the current room. */
static void invite_typed(int add, const char *rest)
{
    char a[48], r[48], user[64], shown[48], line[96];
    const char *room = room_id;
    int i, rc;
    arg_word(rest, a, sizeof a);
    if (a[0] == '\0') { usage(add ? "/invite @user [#room]" : "/uninvite @user [#room]"); return; }
    arg_word(after_word(rest), r, sizeof r);
    if (r[0] == '#') memmove(r, r + 1, strlen(r));
    for (i = 0; r[0] != '\0' && i < rooms_count; i++)
        if (strcmp(rooms_name[i], r) == 0 || strcmp(rooms_id[i], r) == 0) { room = rooms_id[i]; break; }
    target_user(a, user, sizeof user, shown, sizeof shown);
    rc = ae_chat_invite(&chat, room, user, add);
    if (rc == AE_CHAT_OK) {
        if (add) sprintf(line, "Invited %.40s to the room", shown); else sprintf(line, "Revoked invite for %.40s", shown);
        system_line(line);
    } else if (rc == AE_CHAT_ERR_STATUS && strcmp(chat.error, "not_found") == 0) {
        sprintf(line, "User %.40s not found", shown);
        system_line(line);
    } else show_error(rc);
}

/** /reactions n: TS 'chat:reactions:list' -> `Reactions on N: [disp n] ...` or `No reactions on N`. */
static void reactions_typed(const char *rest)
{
    char a[16], line[LC_TEXT_MAX];
    char *f[3];
    lc_line *l;
    int rc, any = 0;
    arg_word(rest, a, sizeof a);
    if (a[0] == '\0') { usage("/reactions <msg-id>"); return; }
    if ((l = typed_message(a)) == 0) return;
    rc = ae_chat_reactions(&chat, l->id);
    if (rc != AE_CHAT_OK) { show_error(rc); return; }
    sprintf(line, "Reactions on %.12s:", a);
    while (ae_chat_next_row(&chat, f, 3) > 0) {       /* no chat call inside: the rows live in the reply buffer */
        if (atoi(f[1]) <= 0 || strlen(line) + 32 >= sizeof line) continue;
        sprintf(line + strlen(line), " [%.12s %d]", lc_react_display(f[0]), atoi(f[1]));
        any = 1;
    }
    if (!any) sprintf(line, "No reactions on %.12s", a);
    system_line(line);
}

/** /unreact [emoji] <n>, /thumbsup /like /love <n>. */
static void react_typed(const char *cmd, const char *rest)
{
    char a[24], b[16];
    lc_line *l;
    int count = 0, rc;
    arg_word(rest, a, sizeof a);
    if (strcmp(cmd, "unreact") == 0) {
        arg_word(after_word(rest), b, sizeof b);
        if (b[0] == '\0') { usage("/unreact [emoji] <msg-id>"); return; }
        if ((l = typed_message(b)) == 0) return;
        then_poll(ae_chat_unreact(&chat, l->id, a[0] != '\0' ? a : "+1", &count));
        return;
    }
    if (a[0] == '\0') { usage(strcmp(cmd, "love") == 0 ? "/love <msg-id>" : "/thumbsup <msg-id>"); return; }
    if ((l = typed_message(a)) == 0) return;
    rc = react_on(l->id, strcmp(cmd, "love") == 0 ? "heart" : "+1");
    if (rc != AE_CHAT_OK) show_error(rc);
}

/** /help <command>: "/name - description" then the usage, or "Unknown: x". */
static void help_for(const char *want)
{
    unsigned i;
    char line[160], al[40];
    const char *p;
    for (i = 0; i < (unsigned)lc_cmd_help_count; i++) {
        int hit = strcmp(lc_cmd_help[i][0], want) == 0;
        for (p = lc_cmd_help[i][1]; !hit && *p != '\0'; ) {
            int n = 0;
            while (*p != '\0' && *p != ',' && n < (int)sizeof al - 1) al[n++] = *p++;
            al[n] = '\0';
            if (*p == ',') p++;
            hit = strcmp(al, want) == 0;
        }
        if (!hit) continue;
        sprintf(line, "/%s - %.100s", lc_cmd_help[i][0], lc_cmd_help[i][2]);
        system_line(line);
        system_line(lc_cmd_help[i][3]);
        return;
    }
    sprintf(line, "Unknown: %.60s", want);
    system_line(line);
}

/* ---- Phase A: pins, search, motd, emoji, events, /me, /delete, thread view (TS commands/) ---- */

static int ev_logins = 1, ev_files = 1, ev_doors = 1, ev_messages = 1, ev_announce = 1;   /* TS prefs.show*: all on */
static int ev_mute_all;

/** TS updateMuteAllEvents: muted when every kind is off. */
static void events_sync(void)
{
    ev_mute_all = !(ev_logins || ev_files || ev_doors || ev_messages || ev_announce);
}

/** True when the preferences hide this BBS event type: TS socket-typing.ts shouldShowEvent, same
 *  type names and grouping. Unknown types show. Gates the legacy login lines of take_bbs_event. */
static int event_hidden(const char *type)
{
    static const char *const logins[] = { "user_login", "user_logout", 0 };
    static const char *const files[] = { "upload_start", "upload_complete", "download_start", "download_complete", 0 };
    static const char *const doors[] = { "door_enter", "door_exit", 0 };
    static const char *const messages[] = { "new_message", "page_sysop", "conference_join", "node_activity", 0 };
    static const char *const announce[] = { "system_announcement", 0 };
    static const char *const *const groups[] = { logins, files, doors, messages, announce };
    const int shown[] = { ev_logins, ev_files, ev_doors, ev_messages, ev_announce };
    int g, i;
    for (g = 0; g < 5; g++)
        for (i = 0; groups[g][i]; i++)
            if (strcmp(type, groups[g][i]) == 0) return ev_mute_all || !shown[g];
    return 0;
}

static const char *on_off(int v) { return v ? "ON" : "OFF"; }

static void events_typed(const char *rest)
{
    char sub[16], t[96];
    arg_word(rest, sub, sizeof sub);
    if (sub[0] == '\0') {
        system_line("BBS Event Display Settings:");
        system_line("");
        sprintf(t, "  User Logins/Logouts:     %s", on_off(ev_logins)); system_line(t);
        sprintf(t, "  File Uploads/Downloads:  %s", on_off(ev_files)); system_line(t);
        sprintf(t, "  Door Activity:           %s", on_off(ev_doors)); system_line(t);
        sprintf(t, "  New Messages:            %s", on_off(ev_messages)); system_line(t);
        sprintf(t, "  System Announcements:    %s", on_off(ev_announce)); system_line(t);
        system_line("");
        system_line("Use /events <type> to toggle a specific event type:");
        system_line("  /events logins       - Toggle login/logout events");
        system_line("  /events files        - Toggle file upload/download events");
        system_line("  /events doors        - Toggle door activity events");
        system_line("  /events messages     - Toggle new message events");
        system_line("  /events announcements - Toggle system announcements");
        system_line("  /events on           - Enable all event types");
        system_line("  /events off          - Disable all event types");
        return;
    }
    if (strcmp(sub, "on") == 0 || strcmp(sub, "enable") == 0 || strcmp(sub, "off") == 0 || strcmp(sub, "disable") == 0) {
        int on = sub[1] == 'n';
        ev_logins = ev_files = ev_doors = ev_messages = ev_announce = on;
        events_sync();
        system_line(on ? "All BBS events enabled" : "All BBS events disabled");
        return;
    }
    if (strcmp(sub, "logins") == 0 || strcmp(sub, "login") == 0) {
        ev_logins = !ev_logins; sprintf(t, "Login/logout events: %s", on_off(ev_logins));
    } else if (strcmp(sub, "files") == 0 || strcmp(sub, "file") == 0) {
        ev_files = !ev_files; sprintf(t, "File upload/download events: %s", on_off(ev_files));
    } else if (strcmp(sub, "doors") == 0 || strcmp(sub, "door") == 0) {
        ev_doors = !ev_doors; sprintf(t, "Door activity events: %s", on_off(ev_doors));
    } else if (strcmp(sub, "messages") == 0 || strcmp(sub, "message") == 0) {
        ev_messages = !ev_messages; sprintf(t, "New message events: %s", on_off(ev_messages));
    } else if (strcmp(sub, "announcements") == 0 || strcmp(sub, "announcement") == 0 || strcmp(sub, "system") == 0) {
        ev_announce = !ev_announce; sprintf(t, "System announcements: %s", on_off(ev_announce));
    } else {
        sprintf(t, "Unknown event type: %.30s", sub);
        system_line(t);
        system_line("Valid types: logins, files, doors, messages, announcements, on, off");
        return;
    }
    events_sync();
    system_line(t);
}

/* ---- settings overlay and stored prefs: TS overlays/settings.ts, prefs-sync.ts, AEW_CHATPREFS ---- */

static int is_enter(int key);
static int snd_on = 1;                           /* TS notificationSound/mentionSound (= !muteSounds) */

static void pref_set_bool(const char *key, int v) { ae_chat_prefs_set(key, v ? "1" : "0"); }

/** TS saveSettings(prefsAsCheckboxes(...)): every boolean the TS door stores. */
static void prefs_save(void)
{
    pref_set_bool("showLogins", ev_logins);
    pref_set_bool("showFileActivity", ev_files);
    pref_set_bool("showDoorActivity", ev_doors);
    pref_set_bool("showMessages", ev_messages);
    pref_set_bool("showSystemAnnouncements", ev_announce);
    pref_set_bool("notificationSound", snd_on);
    pref_set_bool("mentionSound", snd_on);
    pref_set_bool("showTimestamps", M.timestamps);
    pref_set_bool("compactMode", M.compact);
    pref_set_bool("muteAllEvents", ev_mute_all);
}

/** TS togglePinnedChannel: the hub stores pinned channels by name, comma separated. */
static void pins_save(void)
{
    char list[200];
    int i, j, len = 0;
    list[0] = '\0';
    for (i = 0; i < pinned_rooms.n; i++)
        for (j = 0; j < rooms_count; j++)
            if (strcmp(rooms_id[j], pinned_rooms.id[i]) == 0) {
                int n = (int)strlen(rooms_name[j]);
                if (len + n + 2 > (int)sizeof list) break;     /* the 198-byte wire cap */
                if (len > 0) list[len++] = ',';
                strcpy(list + len, rooms_name[j]);
                len += n;
            }
    ae_chat_prefs_set("pinnedChannels", list);
}

/** TS chat:prefs at start + applyServerPrefs: the stored booleans, then pinned names -> the rooms' ids. */
static void prefs_load(void)
{
    char line[200], pins[200];
    const char *p;
    if (ae_chat_prefs_get(line, sizeof line) > 0) {
        ev_logins = ae_chat_prefs_bool(line, "showLogins", ev_logins);
        ev_files = ae_chat_prefs_bool(line, "showFileActivity", ev_files);
        ev_doors = ae_chat_prefs_bool(line, "showDoorActivity", ev_doors);
        ev_messages = ae_chat_prefs_bool(line, "showMessages", ev_messages);
        ev_announce = ae_chat_prefs_bool(line, "showSystemAnnouncements", ev_announce);
        snd_on = ae_chat_prefs_bool(line, "notificationSound", snd_on);
        M.timestamps = ae_chat_prefs_bool(line, "showTimestamps", M.timestamps);
        M.compact = ae_chat_prefs_bool(line, "compactMode", M.compact);
        events_sync();
    }
    if (ae_chat_prefs_get_pinned(pins, sizeof pins) <= 0) return;
    for (p = pins; *p != '\0';) {
        char name[40];
        int n = 0, j;
        while (*p != '\0' && *p != ',') { if (n + 1 < (int)sizeof name) name[n++] = *p; p++; }
        name[n] = '\0';
        if (*p == ',') p++;
        for (j = 0; j < rooms_count; j++)
            if (strcmp(rooms_name[j], name) == 0 && idset_find(&pinned_rooms, rooms_id[j]) < 0) idset_put(&pinned_rooms, rooms_id[j]);
    }
}

/** /sounds on|off: TS commands/handlers.ts, saved as notificationSound + mentionSound. */
static void sounds_typed(const char *rest)
{
    char w[8];
    arg_word(rest, w, sizeof w);
    if (strcmp(w, "on") != 0 && strcmp(w, "off") != 0) { usage("/sounds on|off"); return; }
    snd_on = w[1] == 'n';
    prefs_save();
    system_line(snd_on ? "Sounds enabled" : "Sounds disabled");
}

/** The caller's own presence from the users sidebar: 0 online, 1 away, 2 busy, 3 dnd. */
static int own_status(void)
{
    int i;
    for (i = 0; i < M.nside; i++)
        if (M.side[i].kind == LC_SIDE_USER && is_self(M.side[i].label))
            return M.side[i].status == 'a' ? 1 : M.side[i].status == 'b' ? 2 : M.side[i].status == 'd' ? 3 : 0;
    return 0;
}

/* ---- View > Theme: TS overlays/theme-menu.ts. The highlight IS the preview (every move re-themes the whole door), Enter keeps
   the theme and saves it on the account, Esc or q puts back the theme the panel opened on. ---- */
static ae_session *theme_session;       /* main's session, for ae_set_user_theme */
static char theme_id[32];               /* the theme running now, by id */

static void theme_show(int index)
{
    const ui_theme *th = ui_theme_at(index);
    M.theme_sel = index;
    M.rgb = th->rgb;
    lc_theme_set(th->rgb);
    copy(theme_id, sizeof theme_id, th->id);
}

static void theme_open(void)
{
    int i, n = ui_theme_count(), at = 0;
    for (i = 0; i < n; i++) if (strcmp(ui_theme_at(i)->id, ui_theme_by_id(theme_id)->id) == 0) at = i;
    M.theme = 1;
    M.theme_opened = at;
    M.theme_sel = at;
}

static void theme_close(int keep)
{
    if (!keep) theme_show(M.theme_opened);
    M.theme = 0;
    if (keep && theme_session != 0) ae_set_user_theme(theme_session, theme_id);
}

static void theme_key(int key)
{
    int n = ui_theme_count();
    if (key == UI_KEY_ESC || key == 'q') theme_close(0);
    else if (is_enter(key)) theme_close(1);
    else if (key == UI_KEY_UP) { if (M.theme_sel > 0) theme_show(M.theme_sel - 1); }
    else if (key == UI_KEY_DOWN) { if (M.theme_sel < n - 1) theme_show(M.theme_sel + 1); }
    else if (key == UI_KEY_PGUP) theme_show(M.theme_sel > 9 ? M.theme_sel - 10 : 0);
    else if (key == UI_KEY_PGDN) theme_show(M.theme_sel + 10 < n ? M.theme_sel + 10 : n - 1);
}

/** Ctrl+S, /settings, View > Settings. The TS overlay resets its boxes to defaults on open; this shows the
    current values instead. */
static void settings_open(void)
{
    M.settings = 1;
    M.settings_sel = 0;
    M.settings_val[LC_SET_LOGINS] = ev_logins;
    M.settings_val[LC_SET_FILES] = ev_files;
    M.settings_val[LC_SET_DOORS] = ev_doors;
    M.settings_val[LC_SET_MESSAGES] = ev_messages;
    M.settings_val[LC_SET_ANNOUNCE] = ev_announce;
    M.settings_val[LC_SET_MUTE] = !snd_on;
    M.settings_val[LC_SET_TYPING] = show_typing;
    M.settings_val[LC_SET_STAMPS] = M.timestamps;
    M.settings_status = own_status();
}

/** TS overlay destroy: the boxes become the prefs, then saveSettings. */
static void settings_close(void)
{
    M.settings = 0;
    ev_logins = M.settings_val[LC_SET_LOGINS];
    ev_files = M.settings_val[LC_SET_FILES];
    ev_doors = M.settings_val[LC_SET_DOORS];
    ev_messages = M.settings_val[LC_SET_MESSAGES];
    ev_announce = M.settings_val[LC_SET_ANNOUNCE];
    snd_on = !M.settings_val[LC_SET_MUTE];
    show_typing = M.settings_val[LC_SET_TYPING];
    if (!show_typing) M.ntyping = 0;
    M.timestamps = M.settings_val[LC_SET_STAMPS];
    events_sync();
    prefs_save();
}

/** Space/Enter or a click on an item: a box toggles, a status applies at once (TS radio change ->
    presenceService.setStatus), Close saves and closes. */
static void settings_pick(int item)
{
    static const char *const words[] = { "online", "away", "busy", "dnd" };
    if (item < 0 || item >= LC_SET_ITEMS) return;
    M.settings_sel = item;
    if (item <= LC_SET_STAMPS) M.settings_val[item] = !M.settings_val[item];
    else if (item <= LC_SET_DND) {
        int rc = ae_chat_presence(&chat, words[item - LC_SET_ONLINE], "");
        if (rc != AE_CHAT_OK) { show_error(rc); return; }
        M.settings_status = item - LC_SET_ONLINE;
        do_poll();
    } else settings_close();
}

static void settings_key(int key)
{
    if (key == UI_KEY_ESC) settings_close();
    else if (key == UI_KEY_UP || key == UI_KEY_BACKTAB) M.settings_sel = (M.settings_sel + LC_SET_ITEMS - 1) % LC_SET_ITEMS;
    else if (key == UI_KEY_DOWN || key == 9) M.settings_sel = (M.settings_sel + 1) % LC_SET_ITEMS;
    else if (key == ' ' || is_enter(key)) settings_pick(M.settings_sel);
}

/** /me: TS sends `ACTION: * user text` to the room. */
static void me_typed(const char *rest)
{
    char text[LC_TEXT_MAX];
    if (*rest == '\0') { usage("/me does something"); return; }
    if (room_id[0] == '\0') { system_line("Join a room first."); return; }
    sprintf(text, "ACTION: * %.30s %.190s", handle, rest);
    send_room_line(text, 0);
}

/** /delete #channel: TS admin:delete-channel. The room is found by name in the room list. */
static void channel_delete(const char *name)
{
    char id[48];
    int i, rc = load_rooms();
    if (rc != AE_CHAT_OK) { show_error(rc); return; }
    for (i = 0; i < rooms_count && strcmp(rooms_name[i], name) != 0 && strcmp(rooms_id[i], name) != 0; i++) {}
    if (i == rooms_count) { system_line("Not found."); return; }
    copy(id, sizeof id, rooms_id[i]);
    rc = ae_chat_room_delete(&chat, id);
    if (rc != AE_CHAT_OK) { show_error(rc); return; }
    if (strcmp(id, room_id) == 0) leave_room();
    load_sidebar();
}

static void delete_channel_typed(const char *rest)
{
    char a[48];
    arg_word(rest, a, sizeof a);
    if (a[0] == '#') memmove(a, a + 1, strlen(a));
    if (a[0] == '\0') { usage("/delete #channel"); return; }
    channel_delete(a);
}

/** The id a typed message number names, else the word as the id itself (TS never parses ids). */
static const char *message_id_arg(const char *a, char *out, size_t cap)
{
    lc_line *l = digits(a) ? line_by_number(atoi(a)) : 0;
    copy(out, cap, l != 0 && l->id[0] != '\0' ? l->id : a);
    return out;
}

static void pin_typed(int pin, const char *rest)
{
    char a[48], id[48];
    if (!need_level(80)) return;
    arg_word(rest, a, sizeof a);
    if (a[0] == '\0' || *after_word(rest) != '\0') { usage(pin ? "/pin <msg-id>" : "/unpin <msg-id>"); return; }
    if (room_id[0] == '\0') { system_line("Join a room first."); return; }
    message_id_arg(a, id, sizeof id);
    if (ae_chat_pin(&chat, room_id, id, pin) != AE_CHAT_OK) { show_error(AE_CHAT_ERR_STATUS); return; }
    system_line(pin ? "Pin requested." : "Unpin requested.");
}

static void view_open(const char *title)
{
    M.view = 1;
    M.view_top = 0;
    M.view_n = 0;
    copy(M.view_title, sizeof M.view_title, title);
}

static void view_add(const char *text)
{
    if (M.view_n < LC_VIEW_MAX) copy(M.view_text[M.view_n++], LC_VIEW_COL, text);
}

/** An epoch the server sent (seconds or milliseconds) as JS toLocaleString("en-US") prints it: "11/9/2026, 3:04:05 PM". */
static void datetime_of(char *out, const char *epoch)
{
    char digits[12];
    time_t t;
    struct tm *tm;
    size_t n = strlen(epoch);
    if (n > 10) n = 10;
    memcpy(digits, epoch, n);
    digits[n] = '\0';
    t = (time_t)atol(digits);
    tm = localtime(&t);
    if (n == 0 || tm == 0) { strcpy(out, "--"); return; }
    sprintf(out, "%d/%d/%d, %d:%02d:%02d %s", tm->tm_mon + 1, tm->tm_mday, tm->tm_year + 1900,
            tm->tm_hour % 12 == 0 ? 12 : tm->tm_hour % 12, tm->tm_min % 60, tm->tm_sec % 61, tm->tm_hour < 12 ? "AM" : "PM");
}

/** /pinned (alias pins): TS ui/pinned-panel.ts as the text overlay: per pin "N. user: message", the "Sent:" time of
    the message (the hub's messageAt) and "Pinned by X on <date>". */
static void pinned_typed(void)
{
    char *f[AE_CHAT_ROW_FIELDS];
    char t[LC_VIEW_COL + 80], d[40], rows[8][LC_VIEW_COL], by[8][80], sent[8][40];
    int n = 0, i, rc, k;
    if (room_id[0] == '\0') { system_line("Join a room first."); return; }
    rc = ae_chat_pins(&chat, room_id);
    if (rc != AE_CHAT_OK) { show_error(rc); return; }
    while ((k = ae_chat_next_row(&chat, f, AE_CHAT_ROW_FIELDS)) >= 5 && n < 8) {
        sprintf(rows[n], "%.24s: %.60s", f[1], f[2]);
        datetime_of(d, f[4]);
        sprintf(by[n], "%.20s on %.30s", f[3], d);
        datetime_of(sent[n], k >= 6 ? f[5] : "");
        n++;
    }
    view_open("Pinned Messages");
    if (n == 0) { view_add("No pinned messages in this room."); }
    else {
        sprintf(t, "=Pinned Messages (%d):", n);
        view_add(t);
        view_add("");
        for (i = 0; i < n; i++) {
            sprintf(t, "%d. %.90s", i + 1, rows[i]);
            view_add(t);
            sprintf(t, "   Sent: %s", sent[i]);
            view_add(t);
            sprintf(t, "   Pinned by %s", by[i]);
            view_add(t);
            view_add("");
        }
        view_add("");
    }
    view_add("");
    view_add("Press ESC to close");
}

/** TS ui/search-overlay.ts: /search, /find, Ctrl+F and the menu's "Search (Ctrl+F)" all open the modal empty (TS
    ignores a typed query). Query and Username (optional) fields, a Results list, Tab between the two fields. */
static int is_enter(int key);
static int is_text(int key);

static void search_open(void)
{
    M.search = 1;
    M.search_field = 0;
    M.search_q[0] = M.search_user[0] = '\0';
    M.search_n = M.search_sel = M.search_top = 0;
}

/** Enter in the Query field: TS onSearch(query, username). Under 2 characters is the TS system line; otherwise the
    hub search fills the Results list ("N. user @ date: message") and server.ts prints "Found N results for ...". */
static void search_run(void)
{
    char *f[AE_CHAT_ROW_FIELDS];
    char t[LC_VIEW_COL + 120], d[40];
    int rc;
    if (strlen(M.search_q) < 2) { system_line("Search query must be at least 2 characters"); return; }
    rc = ae_chat_search(&chat, M.search_q, room_id[0] != '\0' ? room_id : 0,
                        M.search_user[0] != '\0' ? M.search_user : 0, LC_SEARCH_MAX);
    if (rc != AE_CHAT_OK) { show_error(rc); return; }
    M.search_n = M.search_sel = M.search_top = 0;
    while (ae_chat_next_row(&chat, f, AE_CHAT_ROW_FIELDS) >= 5 && M.search_n < LC_SEARCH_MAX) {
        datetime_of(d, f[3]);
        sprintf(t, "%d. %.20s @ %.24s: %.60s", M.search_n + 1, f[2], d, f[4]);
        copy(M.search_rows[M.search_n++], LC_VIEW_COL, t);
    }
    sprintf(t, "Found %d results for \"%.100s\"", M.search_n, M.search_q);
    system_line(t);
}

/** Keys while the search modal is up: Esc closes, Tab/Shift+Tab switch field, Enter in Query searches, arrows move
    the Results selection, text and Backspace edit the focused field. */
static void search_key(int key)
{
    char *field = M.search_field == 0 ? M.search_q : M.search_user;
    int cap = M.search_field == 0 ? (int)sizeof M.search_q : (int)sizeof M.search_user;
    int len = (int)strlen(field);
    if (key == UI_KEY_ESC || key == 27) M.search = 0;
    else if (key == 9 || key == UI_KEY_BACKTAB) M.search_field = !M.search_field;
    else if (is_enter(key)) { if (M.search_field == 0) search_run(); }
    else if (key == UI_KEY_DOWN && M.search_sel < M.search_n - 1) M.search_sel++;
    else if (key == UI_KEY_UP && M.search_sel > 0) M.search_sel--;
    else if (key == UI_KEY_PGDN) { M.search_sel += 10; if (M.search_sel > M.search_n - 1) M.search_sel = M.search_n > 0 ? M.search_n - 1 : 0; }
    else if (key == UI_KEY_PGUP) { M.search_sel -= 10; if (M.search_sel < 0) M.search_sel = 0; }
    else if ((key == 8 || key == 127) && len > 0) field[len - 1] = '\0';
    else if (is_text(key) && len + 1 < cap) { field[len] = (char)key; field[len + 1] = '\0'; }
    if (M.search_sel < M.search_top) M.search_top = M.search_sel;
}

/** /motd [text|--clear]: the room's message of the day (TS commands/mode.ts + input-submit-handler).
    No text shows the cached one; a set or clear goes to the hub, whose room_motd event prints it. */
static void motd_typed(const char *rest)
{
    char t[LC_TEXT_MAX];
    if (*rest == '\0') {
        if (room_motd[0] == '\0') { system_line("No MOTD set. Use /motd <text> to set, /motd --clear to clear."); return; }
        sprintf(t, "[MOTD] %.200s", room_motd);
        motd_line(t);
        return;
    }
    if (room_id[0] == '\0') { system_line("Join a room first."); return; }
    then_poll(ae_chat_room_motd(&chat, room_id, strcmp(rest, "--clear") == 0 ? "" : rest));
}

/** /mode <+modes> [param ...]: TS commands/mode.ts; the hub applies it as web/backend mode.handler.ts.
 * Each per-flag refusal is a TS room:error modal, shown one after another. */
static void mode_typed(const char *rest)
{
    char modes[72], a[48], user[64], params[200], applied[200], errors[200];
    const char *p;
    int rc;
    arg_word(rest, modes, sizeof modes);
    if (modes[0] == '\0') { usage("/mode <+modes> [param]  e.g. /mode +i  or /mode +o alice"); return; }
    if ((modes[0] != '+' && modes[0] != '-') || !isalpha((unsigned char)modes[1])) {
        system_line("Error: Mode string must start with + or -");
        return;
    }
    if (room_id[0] == '\0') { message_modal("Error", "Not in a room"); return; }
    params[0] = '\0';
    for (p = after_word(rest); *p != '\0'; p = after_word(p)) {
        arg_word(p, a, sizeof a);
        if (a[0] == '\0') break;
        target_user(a, user, sizeof user, 0, 0);
        if (strlen(params) + strlen(user) + 2 > sizeof params) break;
        if (params[0] != '\0') strcat(params, " ");
        strcat(params, user);
    }
    rc = ae_chat_mode(&chat, room_id, modes, params, applied, sizeof applied, errors, sizeof errors);
    if (rc == AE_CHAT_ERR_STATUS && strcmp(chat.error, "forbidden") == 0) { message_modal("Error", "Only moderators can change modes"); return; }
    if (rc == AE_CHAT_ERR_STATUS && strcmp(chat.error, "bad_request") == 0) { message_modal("Error", "Mode string must start with + or -"); return; }
    if (rc == AE_CHAT_OK && errors[0] != '\0') {
        copy(modal_next, sizeof modal_next, errors);
        modal_close();
    }
    then_poll(rc);
}

/** /emoji <query> and /emojis [category] (TS commands/emoji.ts): the lists print as system lines. */
static void emoji_search_typed(const char *query)
{
    char t[LC_TEXT_MAX], one[LC_TEXT_MAX];
    int k, n = 0, shown = 0;
    for (k = lc_emoji_next(0, -1, query); k >= 0; k = lc_emoji_next(k + 1, -1, query)) n++;
    if (n == 0) { sprintf(t, "No emojis found for \"%.100s\"", query); system_line(t); return; }
    sprintf(t, "Emojis matching \"%.100s\":", query);
    accent_line(t);
    for (k = lc_emoji_next(0, -1, query); k >= 0 && shown < 10; k = lc_emoji_next(k + 1, -1, query), shown++) {
        lc_emoji_line(k, one, sizeof one);
        system_line(one);
    }
    if (n > 10) { sprintf(t, "  ... and %d more", n - 10); system_line(t); }
}

static void emojis_typed(const char *rest)
{
    static const struct { const char *name; int cat; } map[] = {
        { "emotions", 0 }, { "emotion", 0 }, { "actions", 1 }, { "action", 1 },
        { "symbols", 2 }, { "symbol", 2 }, { "special", 3 } };
    char word[40], t[LC_TEXT_MAX], one[LC_TEXT_MAX];
    int i, k, cat = -1;
    arg_word(rest, word, sizeof word);
    for (i = 0; word[i] != '\0'; i++) if (word[i] >= 'A' && word[i] <= 'Z') word[i] = (char)(word[i] + 32);
    if (word[0] == '\0') {
        accent_line("Emoji Categories:");
        system_line("  emotions - Smileys and emotions");
        system_line("  actions - Kaomoji actions");
        system_line("  symbols - Common symbols");
        system_line("  special - BBS-themed emojis");
        system_line("");
        system_line("Use /emojis <category> to list emojis in a category");
        system_line("Use /emoji to open emoji picker");
        return;
    }
    for (i = 0; i < 7; i++) if (strcmp(word, map[i].name) == 0) cat = map[i].cat;
    if (cat < 0) { sprintf(t, "Unknown category \"%.30s\". Use /emojis to see all categories.", word); system_line(t); return; }
    sprintf(t, "%s Emojis:", lc_emoji_cat_name(cat));
    accent_line(t);
    for (k = lc_emoji_next(0, cat, 0); k >= 0; k = lc_emoji_next(k + 1, cat, 0)) {
        lc_emoji_line(k, one, sizeof one);
        system_line(one);
    }
}

/** The hub's custom emoji, so a ":name:" in a message shows its ascii face (TS utils/emojis.ts). Best effort. */
static void load_custom_emoji(void)
{
    char *f[AE_CHAT_ROW_FIELDS];
    lc_custom_emoji_clear();
    if (ae_chat_emoji_list(&chat) != AE_CHAT_OK) return;
    while (ae_chat_next_row(&chat, f, AE_CHAT_ROW_FIELDS) >= 3) lc_custom_emoji_add(f[0], f[2]);
}

/** A typed emoji name as TS keeps it: the colons stripped, lower case. */
static void emoji_name(const char *word, char *out, int cap)
{
    int n = 0;
    if (*word == ':') word++;
    while (*word && n < cap - 1) out[n++] = (char)tolower((unsigned char)*word++);
    if (n > 0 && out[n - 1] == ':') n--;
    out[n] = '\0';
}

/** /customemoji add <name> <display> <ascii> | remove <name> | list (TS commands/emoji.ts). A web caller
 *  sees <display>, a telnet caller <ascii>; the hub refuses a non-sysop add or remove. */
static void customemoji_typed(const char *rest)
{
    char sub[12], word[40], name[40], display[LC_CUSTOM_EMOJI_TEXT], ascii[LC_CUSTOM_EMOJI_TEXT];
    char *f[AE_CHAT_ROW_FIELDS], t[LC_TEXT_MAX], code[44];
    int n = 0, i, rc;
    arg_word(rest, sub, sizeof sub);
    rest = after_word(rest);
    for (i = 0; sub[i]; i++) sub[i] = (char)tolower((unsigned char)sub[i]);
    if (sub[0] == '\0' || strcmp(sub, "list") == 0) {
        rc = ae_chat_emoji_list(&chat);
        if (rc != AE_CHAT_OK) { show_error(rc); return; }
        while (ae_chat_next_row(&chat, f, AE_CHAT_ROW_FIELDS) >= 3) {
            if (f[1][0] == '\0' || f[2][0] == '\0') continue;   /* an image emoji: the web chat's own */
            sprintf(t, "  %.32s  web %.32s  telnet %.32s", f[0], f[1], f[2]);
            system_line(t);
            n++;
        }
        if (n == 0) system_line("No custom emoji yet.");
        return;
    }
    if (strcmp(sub, "add") != 0 && strcmp(sub, "remove") != 0) {
        system_line("Usage: /customemoji add <name> <display> <ascii> | remove <name> | list");
        return;
    }
    if (!is_sysop) { system_line("Only the sysop can add or remove custom emoji."); return; }
    arg_word(rest, word, sizeof word);
    rest = after_word(rest);
    emoji_name(word, name, sizeof name);
    if (strcmp(sub, "remove") == 0) {
        if (name[0] == '\0') { system_line("Usage: /customemoji remove <name>"); return; }
        then_poll(ae_chat_emoji_remove(&chat, name));
        load_custom_emoji();
        return;
    }
    arg_word(rest, display, sizeof display);
    rest = after_word(rest);
    arg_word(rest, ascii, sizeof ascii);
    sprintf(code, ":%.40s:", name);
    if (name[0] == '\0' || display[0] == '\0' || ascii[0] == '\0') system_line("Usage: /customemoji add <name> <display> <ascii>");
    else if (lc_emoji_builtin(code)) { sprintf(t, "%s is a built-in emoji.", code); system_line(t); }
    else { then_poll(ae_chat_emoji_add(&chat, name, display, ascii)); load_custom_emoji(); }
}

/** The thread of a message as the text overlay (TS overlays/thread-view.ts). The first row is the parent. */
static void thread_view(const char *msg_id)
{
    char *f[AE_CHAT_ROW_FIELDS];
    char t[LC_VIEW_COL + 40], rows[LC_VIEW_MAX / 2][LC_VIEW_COL];
    char first[LC_VIEW_COL], when[8];
    int n = 0, i, rc = ae_chat_thread_list(&chat, msg_id, 30);
    if (rc != AE_CHAT_OK) { show_error(rc); return; }
    first[0] = '\0';
    while (ae_chat_next_row(&chat, f, AE_CHAT_ROW_FIELDS) >= AE_CHAT_ROW_MIN) {
        if (first[0] == '\0') { hhmm_of(when, f[3]); sprintf(first, "%.24s: %.60s", f[2], f[7]); }
        else if (n < LC_VIEW_MAX / 2) { sprintf(rows[n], "%.24s: %.60s", f[2], f[7]); n++; }
    }
    if (first[0] == '\0') { system_line("Not found."); return; }
    view_open("Thread");
    view_add("=Original Message:");
    view_add(first);
    sprintf(t, "%.5s", when);
    view_add(t);
    view_add("");
    if (n == 0) view_add("No replies yet. Be the first to reply!");
    else {
        sprintf(t, "=Replies (%d):", n);
        view_add(t);
        for (i = 0; i < n; i++) { sprintf(t, "%d. %.90s", i + 1, rows[i]); view_add(t); }
    }
    view_add("");
    view_add("Press ESC to close");
}

/** A DM the hub refused. not_found (404 "unknown user: a, b") is the TS door's system line
    `User <name> not found`, not a modal; anything else is show_error. `who` is the typed 1:1 target; empty (group DM) takes the names from the hub's detail. */
static void dm_error(int rc, const char *who)
{
    char line[160];
    if (rc == AE_CHAT_ERR_STATUS && strcmp(chat.error, "not_found") == 0) {
        const char *names = who;
        if (who[0] == '\0' && strncmp(chat.detail, "unknown user: ", 14) == 0 && chat.detail[14] != '\0') names = chat.detail + 14;
        sprintf(line, "User %.100s not found", names);
        system_line(line);
    } else show_error(rc);
}

/** Runs one "/..." line. Returns 1 when the door should end. */
static void emoji_show(void);
static int run_command(const char *line)
{
    char cmd[12], a[48];
    const char *rest = after_word(line + 1);
    int rc, n;
    arg_word(line + 1, cmd, sizeof cmd);
    if (is_cmd(cmd, "q", "quit") || is_cmd(cmd, "exit", "bye")) return 1;
    if (is_cmd(cmd, "?", "h") || is_cmd(cmd, "help", "commands")) {
        arg_word(rest, a, sizeof a);
        if (a[0] != '\0') help_for(a[0] == '/' ? a + 1 : a); else open_help();
        return 0;
    }
    if (strcmp(cmd, "compact") == 0) { M.compact = !M.compact; prefs_save(); system_line(M.compact ? "Compact mode on" : "Compact mode off"); return 0; }
    if (is_cmd(cmd, "timestamps", "time")) { M.timestamps = !M.timestamps; prefs_save(); system_line(M.timestamps ? "Timestamps on" : "Timestamps off"); return 0; }
    if (strcmp(cmd, "settings") == 0) { settings_open(); return 0; }
    if (strcmp(cmd, "sounds") == 0) { sounds_typed(rest); return 0; }
    if (is_cmd(cmd, "l", "rooms") || strcmp(cmd, "list") == 0) { list_rooms(); return 0; }
    if (is_cmd(cmd, "w", "who")) { list_who(); return 0; }
    if (is_cmd(cmd, "j", "join")) { join_typed(rest); return 0; }
    if (is_cmd(cmd, "leave", "part")) { leave_room(); return 0; }
    if (is_cmd(cmd, "clear", "cls")) { lines_clear(); return 0; }
    if (is_cmd(cmd, "n", "np") || strcmp(cmd, "create") == 0) {
        if (*rest == '\0') { open_prompt(PROMPT_CREATE, "New room name"); return 0; }
        if (cmd[0] == 'c') create_flags(rest);                       /* TS createCmd: name, topic, flags */
        else create_typed(rest, cmd[1] == 'p' ? "private" : "public");    /* the whole rest: a name may have spaces */
        return 0;
    }
    if (is_cmd(cmd, "r", "reply")) { reply_typed(rest, 0); return 0; }
    if (is_cmd(cmd, "t", "thread")) { reply_typed(rest, 1); return 0; }
    if (strcmp(cmd, "react") == 0) {
        lc_line *l;
        arg_word(rest, a, sizeof a);
        n = atoi(a);
        l = line_by_number(n);
        rest = after_word(rest);
        if (l == 0 || l->id[0] == '\0') { system_line("No such message number."); return 0; }
        if (*rest == '\0') { system_line("Usage: /react n emoji"); return 0; }
        rc = react_on(l->id, rest);
        if (rc != AE_CHAT_OK) show_error(rc);
        return 0;
    }
    if (is_cmd(cmd, "msg", "dm") || strcmp(cmd, "pm") == 0 || strcmp(cmd, "d") == 0) {
        /* TS msgCmd: every leading word that starts with '@' is a target; one target is a DM,
           two or more a group DM (chat:dm / chat:group-dm). */
        char targets[10][48], users[10][72], word[48], list[200], tid[32];
        const char *peers[10];
        int n = 0, i;
        while (*rest == '@' && n < 10) {
            arg_word(rest, word, sizeof word);        /* the hub names users handle@BOARD (TS bridge chat:dm) */
            target_user(word, users[n], sizeof users[0], targets[n], sizeof targets[0]);
            peers[n] = users[n];
            rest = after_word(rest);
            n++;
        }
        if (n == 0 || *rest == '\0') { system_line("Usage: /msg @user [@user2 ...] message"); return 0; }
        rc = n == 1 ? ae_chat_dm(&chat, peers[0], rest, tid, sizeof tid)
                    : ae_chat_dm_group(&chat, peers, n, rest, tid, sizeof tid);
        if (rc != AE_CHAT_OK) { dm_error(rc, n == 1 ? targets[0] : ""); return 0; }
        if (n == 1 && dm_thread[0] != '\0' && strcmp(peers[0], dm_peer) == 0) { do_poll(); return 0; }
        list[0] = '\0';
        for (i = 0; i < n; i++) {
            if (i > 0) strcat(list, ", ");
            strncat(list, targets[i], sizeof list - strlen(list) - 3);
        }
        dm_line(1, n > 1, list, rest);
        return 0;
    }
    if (is_cmd(cmd, "whois", "info")) { whois_typed(rest); return 0; }
    if (is_cmd(cmd, "away", "afk")) { away_typed(rest); return 0; }
    if (strcmp(cmd, "back") == 0) { back_typed(); return 0; }
    if (strcmp(cmd, "status") == 0) { status_typed(rest); return 0; }
    if (strcmp(cmd, "nick") == 0) { char t[64]; sprintf(t, "Your username: %.40s", handle); system_line(t); return 0; }
    if (strcmp(cmd, "ignore") == 0) { ignore_typed(rest); return 0; }
    if (strcmp(cmd, "unignore") == 0) { unignore_typed(rest); return 0; }
    if (strcmp(cmd, "edit") == 0) { edit_typed(rest); return 0; }
    if (is_cmd(cmd, "deletemsg", "delmsg")) { delete_typed(rest); return 0; }
    if (strcmp(cmd, "topic") == 0) {
        if (*rest == '\0') { usage("/topic <new topic>"); return 0; }
        then_poll(ae_chat_topic(&chat, room_id, rest));
        return 0;
    }
    if (is_cmd(cmd, "kick", "ban") || is_cmd(cmd, "mute", "unmute") || is_cmd(cmd, "unban", "op")
        || strcmp(cmd, "deop") == 0) { moderate_typed(cmd, rest); return 0; }
    if (is_cmd(cmd, "invite", "uninvite")) { invite_typed(cmd[0] == 'i', rest); return 0; }
    if (strcmp(cmd, "reactions") == 0) { reactions_typed(rest); return 0; }
    if (is_cmd(cmd, "unreact", "love") || is_cmd(cmd, "thumbsup", "like")) { react_typed(cmd, rest); return 0; }
    if (strcmp(cmd, "me") == 0) { me_typed(rest); return 0; }
    if (strcmp(cmd, "delete") == 0) { delete_channel_typed(rest); return 0; }
    if (strcmp(cmd, "events") == 0) { events_typed(rest); return 0; }
    if (strcmp(cmd, "pin") == 0 || strcmp(cmd, "unpin") == 0) { pin_typed(cmd[0] == 'p', rest); return 0; }
    if (is_cmd(cmd, "pinned", "pins")) { pinned_typed(); return 0; }
    if (is_cmd(cmd, "search", "find")) { search_open(); return 0; }
    if (strcmp(cmd, "motd") == 0) { motd_typed(rest); return 0; }
    if (strcmp(cmd, "mode") == 0) { mode_typed(rest); return 0; }
    if (strcmp(cmd, "emoji") == 0 && *rest == '\0') { emoji_show(); return 0; }   /* commands/emoji.ts: no query opens the picker */
    if (strcmp(cmd, "emoji") == 0) { emoji_search_typed(rest); return 0; }
    if (strcmp(cmd, "emojis") == 0) { emojis_typed(rest); return 0; }
    if (strcmp(cmd, "customemoji") == 0) { customemoji_typed(rest); return 0; }
    /* TS commands/voice.ts, mic.ts, ui.ts open web-only views: voice, mic and video are not on
     * Amiga yet (spot 2026-11-09); draw and files wait for their ports. */
    if (is_cmd(cmd, "voice", "vc") || is_cmd(cmd, "deafen", "deaf") || is_cmd(cmd, "undeafen", "undeaf")
        || strcmp(cmd, "mic") == 0) { system_line("Voice is not on Amiga yet."); return 0; }
    if (is_cmd(cmd, "draw", "whiteboard") || strcmp(cmd, "art") == 0) { system_line("Drawing is not on Amiga yet."); return 0; }
    if (is_cmd(cmd, "files", "file") || strcmp(cmd, "share") == 0) { system_line("File sharing is not on Amiga yet."); return 0; }
    system_line("Unknown command. Type /help for help.");
    return 0;
}

/** Enter in the input box. Returns 1 when the door should end. */
static int submit(void)
{
    int rc, ended = 0;
    char edit_id[UI_HIST_ID];
    if (M.input_len == 0) return 0;
    copy(edit_id, sizeof edit_id, ui_hist_editing(&hist) ? ui_hist_editing(&hist) : "");
    ui_hist_reset(&hist);                       /* TS: getEditingId, then inputHistory.reset() */
    M.editing = 0;
    if (M.input[0] == '/') {
        ended = run_command(M.input);
    } else if (edit_id[0] != '\0') {
        then_poll(ae_chat_edit(&chat, edit_id, M.input));
    } else if (dm_thread[0] != '\0') {
        char tid[32];
        rc = ae_chat_dm(&chat, dm_peer, M.input, tid, sizeof tid);
        if (rc == AE_CHAT_OK) do_poll(); else dm_error(rc, dm_peer);
    } else if (room_id[0] != '\0') {
        send_room_line(M.input, 0);
    } else {
        system_line("Join a channel first: /join room");
    }
    M.input[0] = '\0';
    M.input_len = 0;
    M.input_cur = 0;
    M.input_anchor = -1;
    return ended;
}

/* One idle step: sleep a moment, and say whether a network poll is due. Split from door_idle so the
   sleep length and the poll cadence can be tested without a screen. */
static int idle_step(void)
{
    ae_delay_ticks(IDLE_SLEEP_TICKS);
    idle_ticks += IDLE_SLEEP_TICKS;
    if (idle_ticks < poll_steps * poll_step_ticks) return 0;
    idle_ticks = 0;
    return 1;
}

static int is_enter(int key) { return key == UI_KEY_ENTER || key == '\r' || key == '\n'; }
static int is_text(int key) { return (key >= 32 && key < 127) || (key >= 160 && key < 256); }

/* ---- the format picker (TS ui/format-picker.ts, wired as server.ts:1792-1860 and 2872-2900) ---- */

/* TS shows the picker 500 ms after the input's selection last changed. The door's clock is the
   ticks door_idle sleeps, so the wait counts down in those (25 ticks at 1/50 s). */
#define FMT_WAIT_TICKS 25
static int fmt_wait;                     /* ticks left before a held selection opens the picker; 0 = none pending */
static int fmt_seen_a = -1, fmt_seen_b = -1;   /* the selection the wait was started for */

/** The input's selection as a..b (a < b). 0 when nothing is selected. */
static int input_selection(int *a, int *b)
{
    if (M.input_anchor < 0 || M.input_anchor == M.input_cur) return 0;
    *a = M.input_anchor < M.input_cur ? M.input_anchor : M.input_cur;
    *b = M.input_anchor < M.input_cur ? M.input_cur : M.input_anchor;
    return 1;
}

/** Open the picker over the selection. 0 when there is none to format. */
static int fmt_show(void)
{
    int a, b;
    if (!input_selection(&a, &b)) return 0;
    lc_fmt_open(&M, a, b);
    fmt_wait = 0;
    return 1;
}

/** After a key: a selection that changed restarts the wait; no selection cancels it (TS select debounce). */
static void fmt_watch(void)
{
    int a, b;
    if (!input_selection(&a, &b)) { fmt_wait = 0; fmt_seen_a = fmt_seen_b = -1; return; }
    if (a == fmt_seen_a && b == fmt_seen_b) return;
    fmt_seen_a = a; fmt_seen_b = b;
    fmt_wait = FMT_WAIT_TICKS;
}

/** One idle step of the wait. 1 when it ran out and the picker opened. */
static int fmt_tick(int ticks)
{
    if (fmt_wait <= 0) return 0;
    fmt_wait -= ticks;
    if (fmt_wait > 0) return 0;
    fmt_wait = 0;
    if (M.fmt || M.focus != LC_FOCUS_INPUT || M.modal != LC_MODAL_NONE || M.help || M.view || M.ctx.open) return 0;
    return fmt_show();
}

static void set_focus(int focus);

/** Open the emoji picker (TS emojiPicker.show: F4, the Tools menu, /emoji). Already shown: nothing, as TS isVisible. */
static void emoji_show(void)
{
    if (M.fmt) return;
    lc_emoji_open(&M);
    fmt_wait = 0;
}

/** The picked emoji (TS insertEmojiIntoInput): getValue() + code + ' ', the cursor at the end, the input focused. */
static void emoji_apply(void)
{
    int cap = LC_INPUT_MAX < AE_CHAT_TEXT_MAX + 1 ? LC_INPUT_MAX : AE_CHAT_TEXT_MAX + 1;
    const char *code = lc_emoji_code(M.fmt_cat, M.fmt_item);
    int n = (int)strlen(code);
    M.fmt = 0;
    if (n == 0) return;
    if (M.input_len + n + 1 >= cap) { system_line("The emoji does not fit the input"); return; }
    memcpy(M.input + M.input_len, code, (size_t)n);
    M.input_len += n;
    M.input[M.input_len++] = ' ';
    M.input[M.input_len] = '\0';
    M.input_cur = M.input_len;
    M.input_anchor = -1;
    set_focus(LC_FOCUS_INPUT);
    fmt_wait = 0; fmt_seen_a = fmt_seen_b = -1;
}

/** The picked item (TS onSelect): the selection is replaced by its wrapped form, the cursor goes after it. */
static void fmt_apply(void)
{
    int cap = LC_INPUT_MAX < AE_CHAT_TEXT_MAX + 1 ? LC_INPUT_MAX : AE_CHAT_TEXT_MAX + 1;
    int a = M.fmt_a, b = M.fmt_b, w;
    char wrapped[LC_INPUT_MAX + 64];
    if (M.fmt_kind == LC_PICK_EMOJI) { emoji_apply(); return; }
    M.fmt = 0;
    if (a < 0 || b > M.input_len || a >= b) return;
    w = lc_fmt_wrap(wrapped, (int)sizeof wrapped, M.fmt_cat, M.fmt_item, M.input + a, b - a, M.rgb);
    if (w < 0 || M.input_len - (b - a) + w >= cap) { system_line("The formatted text does not fit the input"); return; }
    memmove(M.input + a + w, M.input + b, (size_t)(M.input_len - b + 1));
    memcpy(M.input + a, wrapped, (size_t)w);
    M.input_len += w - (b - a);
    M.input_cur = a + w;
    M.input_anchor = -1;
    fmt_wait = 0; fmt_seen_a = fmt_seen_b = -1;
}

/** The picker owns every key while it is shown (CategoryPicker's focus trap). */
static void fmt_key(int key)
{
    if (key == UI_KEY_UP || key == 'k') lc_fmt_move(&M, -1);
    else if (key == UI_KEY_DOWN || key == 'j') lc_fmt_move(&M, 1);
    else if (key == 9 || key == UI_KEY_BACKTAB) M.fmt_pane = M.fmt_pane == LC_FMT_PANE_CAT ? LC_FMT_PANE_ITEM : LC_FMT_PANE_CAT;
    else if (is_enter(key)) { if (M.fmt_pane == LC_FMT_PANE_CAT) M.fmt_pane = LC_FMT_PANE_ITEM; else fmt_apply(); }
    else if (key == UI_KEY_ESC) M.fmt = 0;
}

/** A press while the picker is shown. Rows act as a blessed list does: the first click selects,
    a click on the selected row enters it. A left press outside cancels; a right press does not. */
static void fmt_click(int hit, int idx, int button)
{
    if (hit == LC_HIT_FMT_CLOSE) { M.fmt = 0; return; }
    if (hit == LC_HIT_FMT_CAT && idx >= 0 && idx < lc_pick_cats(&M)) {
        if (idx == M.fmt_cat) { M.fmt_pane = LC_FMT_PANE_ITEM; return; }
        M.fmt_pane = LC_FMT_PANE_CAT;
        lc_fmt_move(&M, idx - M.fmt_cat);
        return;
    }
    if (hit == LC_HIT_FMT_ITEM && idx >= 0) {
        int item = idx + M.fmt_item_top;
        if (item >= (M.fmt_kind == LC_PICK_EMOJI ? lc_emoji_count(M.fmt_cat) : lc_fmt_count(M.fmt_cat))) return;
        if (M.fmt_pane == LC_FMT_PANE_ITEM && item == M.fmt_item) { fmt_apply(); return; }
        M.fmt_pane = LC_FMT_PANE_ITEM;
        lc_fmt_move(&M, item - M.fmt_item);
        return;
    }
    if (hit == LC_HIT_FMT) return;
    if (button == AE_MOUSE_LEFT) M.fmt = 0;
}

static void door_idle(void *ctx)
{
    int polled, opened;
    IGNORE(ctx);
    polled = idle_step();
    if (polled) do_poll();
    opened = fmt_tick(IDLE_SLEEP_TICKS);
    if (typing_tick(IDLE_SLEEP_TICKS)) opened = 1;
    if (polled || opened) present();
}

/* ---- keys --------------------------------------------------------------- */


static void set_focus(int focus)
{
    if (focus == LC_FOCUS_SIDEBAR && lc_sidebar_width(&M) == 0) focus = LC_FOCUS_MENU;
    M.focus = focus;
    if (focus == LC_FOCUS_MENU) { if (M.menu < 0) { M.menu = 0; M.menu_sel = 0; } }
    else M.menu = -1;
}

static void cycle_focus(int step)
{
    int f = M.focus;
    do { f = (f + step + 3) % 3; } while (f == LC_FOCUS_SIDEBAR && lc_sidebar_width(&M) == 0);
    set_focus(f);
}

/** TS Copy Text (osc52Copy): the line's words without the canvas style bytes, put on the caller's clipboard.
    The OSC goes out with the next frame; a terminal without OSC 52 ignores it. */
static void copy_text(const char *marked)
{
    char plain[sizeof M.lines[0].text];
    int n = 0;
    for (; *marked != '\0' && n < (int)sizeof plain - 1; marked++) {
        if (*marked == UI_CELLS_M_FG || *marked == UI_CELLS_M_ATTR || *marked == UI_CELLS_M_WIDE) {
            if (marked[1] == '\0') break;
            marked++;                                    /* the marker's argument byte */
        } else if (*marked == UI_CELLS_M_BOLD || *marked == UI_CELLS_M_PLAIN || *marked == UI_CELLS_M_BASE) {
            continue;
        } else plain[n++] = *marked;
    }
    plain[n] = '\0';
    ansi_clipboard(&screen.buf, plain);
}

/** An item whose wire this board does not have yet: say so instead of doing nothing. */
static void not_yet(const char *item)
{
    char text[96];
    sprintf(text, "%.40s is not yet available here.", item);
    message_modal("Not available", text);
}

/* ---- Context menus: labels and order of the TS door's features/context-menus.ts. ---- */
static const char *ctx_items[UI_POPUP_MAX];

static void ctx_open(int kind, int target, int row, int col)
{
    int n = 0;
    unsigned long alert = 0;
    if (kind == LC_CTX_USER) {
        static const char *const u[] = { "View Profile", "Send Message", "Whois", "---", "Mention", "Add Note",
                                         "View History", "---", "Mute User", "Ignore", "Block" };
        char who[32];
        for (n = 0; n < 11; n++) ctx_items[n] = u[n];
        nick_of(who, sizeof who, M.side[target].label);          /* TS muteMenuLabels: the entries say the current state */
        if (level_of(who) == LV_MUTE) ctx_items[8] = "Unmute User";
        if (level_of(who) == LV_IGNORE) ctx_items[9] = "Unignore";
        if (level_of(who) == LV_BLOCK) ctx_items[10] = "Unblock";
        if (is_sysop) { ctx_items[n++] = "---"; alert = 3UL << n; ctx_items[n++] = "Kick User"; ctx_items[n++] = "Ban User"; }
    } else if (kind == LC_CTX_CHAT) {
        static const char *const c[] = { "Reply", "Quote", "React", "---", "Copy Text", "Pin Message", "---",
                                         "Mark Unread", "Edit", "Delete" };
        for (n = 0; n < 10; n++) ctx_items[n] = c[n];
    } else if (kind == LC_CTX_REACT) {          /* sysop 2026-11-08: React picks one of the eight, not +1 */
        for (n = 0; n < LC_REACT_KINDS; n++) ctx_items[n] = lc_react_label(n);
    } else if (kind == LC_CTX_KIND) {           /* TS ROOM_KIND_LABELS, in ROOM_KINDS order */
        static const char *const k[] = { "Text", "Voice", "Art" };
        for (n = 0; n < 3; n++) ctx_items[n] = k[n];
    } else {
        static const char *const r[] = { "Join", "Leave", "Info", "Expand/Collapse", "---", "Pin Channel" };
        for (n = 0; n < 6; n++) ctx_items[n] = r[n];
        if (idset_find(&pinned_rooms, M.side[target].id) >= 0) ctx_items[5] = "Unpin Channel";
        if (is_sysop) { ctx_items[n++] = "---"; ctx_items[n++] = "Clear History";
                        alert = 3UL << n; ctx_items[n++] = "Archive"; ctx_items[n++] = "Delete Channel"; }
    }
    M.ctx_kind = kind;
    M.ctx_target = target;
    M.menu = -1;
    ui_popup_open(&M.ctx, ctx_items, n, alert, row, col, M.rows, M.cols);
}

static void input_set(const char *text)
{
    copy(M.input, sizeof M.input, text);
    M.input_len = (int)strlen(M.input);
    M.input_cur = M.input_len;
    M.input_anchor = -1;
    set_focus(LC_FOCUS_INPUT);
}

static void ctx_run(const char *it)
{
    char buf[LC_INPUT_MAX];
    if (M.ctx_kind == LC_CTX_USER) {
        const char *nick = M.side[M.ctx_target].label;
        if (strcmp(it, "Send Message") == 0) { sprintf(buf, "/msg %.30s ", nick); input_set(buf); }
        else if (strcmp(it, "Mention") == 0) { sprintf(buf, "@%.30s %.200s", nick, M.input); input_set(buf); }
        else if (strcmp(it, "View History") == 0) {
            int i, found = 0;
            for (i = M.nlines - 1; i >= 0 && found < 15; i--) {
                if (M.lines[i].number == 0 || strcmp(M.lines[i].nick, nick) != 0) continue;
                sprintf(buf, "%.24s: %.180s", nick, M.lines[i].text);
                system_line(buf);
                found++;
            }
            if (!found) { sprintf(buf, "No recent messages from %.30s.", nick); system_line(buf); }
        } else if (strcmp(it, "Kick User") == 0 || strcmp(it, "Ban User") == 0) {
            char user[64];
            int kick = it[0] == 'K', rc;
            if (!is_sysop) { alert_line("Sysop only."); return; }
            target_user(nick, user, sizeof user, 0, 0);          /* TS: no prompt, no reason */
            rc = ae_chat_moderate(&chat, room_id, kick ? "kick" : "ban", user, "", 0);
            if (rc != AE_CHAT_OK) { show_error(rc); return; }
            sprintf(buf, "%s requested for %.30s", kick ? "Kick" : "Ban", nick);
            alert_line(buf);
        } else if (strcmp(it, "View Profile") == 0) profile_overlay(nick);
        else if (strcmp(it, "Whois") == 0) whois_typed(nick);
        else if (strcmp(it, "Mute User") == 0 || strcmp(it, "Unmute User") == 0) level_toggle(nick, LV_MUTE);
        else if (strcmp(it, "Ignore") == 0 || strcmp(it, "Unignore") == 0) level_toggle(nick, LV_IGNORE);
        else if (strcmp(it, "Block") == 0 || strcmp(it, "Unblock") == 0) level_toggle(nick, LV_BLOCK);
        else if (strcmp(it, "Add Note") == 0) {          /* TS showPromptDialog("Note about X:") then bbs.addUserNote */
            char q[48];
            copy(note_target, sizeof note_target, nick);
            sprintf(q, "Note about %.30s:", nick);
            open_prompt(PROMPT_NOTE, q);
        }
    } else if (M.ctx_kind == LC_CTX_CHAT) {
        const lc_line *l = M.ctx_target >= 0 && M.ctx_target < M.nlines ? &M.lines[M.ctx_target] : 0;
        if (strcmp(it, "Mark Unread") == 0) {                   /* TS: acts on the current room, not the line */
            if (room_id[0] == '\0') { system_line("No channel to mark unread."); return; }
            {
                int i = idset_put(&unread_rooms, room_id);
                if (i >= 0 && unread_rooms.count[i] < 1) unread_rooms.count[i] = 1;   /* TS max(1, unreadCount) */
            }
            sprintf(buf, "Marked %.40s as unread", room_name);
            system_line(buf);
            load_sidebar();
            return;
        }
        if (!l || l->number == 0) { motd_line("Right-click on a message first."); return; }
        if (strcmp(it, "React") == 0) ctx_open(LC_CTX_REACT, M.ctx_target, M.ctx.top, M.ctx.left);   /* the picker, where the menu was */
        else if (strcmp(it, "Reply") == 0) { sprintf(buf, "/reply %d ", l->number); input_set(buf); }
        else if (strcmp(it, "Quote") == 0) {                          /* TS quoteInputText */
            char q[sizeof l->quote];
            quote_of(l, q, (int)sizeof q);
            sprintf(buf, "> %s ", q);
            input_set(buf);
        }
        else if (strcmp(it, "Edit") == 0) { sprintf(buf, "/edit %d ", l->number); input_set(buf); }
        else if (strcmp(it, "Delete") == 0) then_poll(ae_chat_delete(&chat, l->id));    /* TS: no confirm */
        else if (strcmp(it, "Pin Message") == 0) { sprintf(buf, "/pin %d", l->number); run_command(buf); }
        else if (strcmp(it, "Copy Text") == 0) { copy_text(l->text); system_line("Copied to clipboard."); }
    } else if (M.ctx_kind == LC_CTX_KIND) {
        char kind[8];
        int i;
        for (i = 0; it[i] != '\0' && i < (int)sizeof kind - 1; i++) kind[i] = (char)tolower((unsigned char)it[i]);
        kind[i] = '\0';
        if (pending_room[0] != '\0') {
            char line[64];
            sprintf(line, "Creating #%.40s...", pending_room);     /* TS input-submit-handler pickRoomKind */
            system_line(line);
            new_room(pending_room, pending_topic, pending_vis, kind, pending_motd);
        }
        pending_room[0] = '\0';
    } else if (M.ctx_kind == LC_CTX_REACT) {
        const lc_line *l = M.ctx_target >= 0 && M.ctx_target < M.nlines ? &M.lines[M.ctx_target] : 0;
        int i;
        for (i = 0; i < LC_REACT_KINDS && strcmp(it, lc_react_label(i)) != 0; i++) {}
        if (!l || l->number == 0 || i == LC_REACT_KINDS) return;
        if (react_on(l->id, lc_react_names[i]) == AE_CHAT_OK) system_line("Reaction sent.");   /* TS context-menus.ts */
        else system_line("The reaction did not go through.");
    } else {
        lc_side *r = &M.side[M.ctx_target];
        if (strcmp(it, "Join") == 0) join_room(r->id);
        else if (strcmp(it, "Leave") == 0) { if (strcmp(r->id, room_id) == 0) leave_room(); else system_line("You are not in that channel."); }
        else if (strcmp(it, "Info") == 0) { sprintf(buf, "Channel: %.40s", r->label); system_line(buf); }
        else if (strcmp(it, "Expand/Collapse") == 0) {               /* TS onToggleChannelExpand */
            if (idset_find(&collapsed_rooms, r->id) < 0) { idset_drop(&expanded_rooms, r->id); idset_put(&collapsed_rooms, r->id); }
            else { idset_drop(&collapsed_rooms, r->id); idset_put(&expanded_rooms, r->id); }
            load_sidebar();
        }
        else if (strcmp(it, "Pin Channel") == 0 || strcmp(it, "Unpin Channel") == 0) {
            int pin = idset_find(&pinned_rooms, r->id) < 0;
            sprintf(buf, "%s %.40s", pin ? "Pinned" : "Unpinned", r->label);
            if (pin) idset_put(&pinned_rooms, r->id); else idset_drop(&pinned_rooms, r->id);
            pins_save();                                             /* TS togglePinnedChannel -> prefsSender */
            accent_line(buf);
            load_sidebar();
        }
        else if (strcmp(it, "Clear History") == 0 || strcmp(it, "Archive") == 0 || strcmp(it, "Delete Channel") == 0) {
            char label[40];                                          /* TS: sysop only, alert lines */
            if (!is_sysop) { alert_line("Sysop only."); return; }
            copy(label, sizeof label, r->label);                     /* the sidebar is rebuilt below */
            if (it[0] == 'C') {
                then_poll(ae_chat_room_clear(&chat, r->id));
                sprintf(buf, "Clear history requested for %.36s", label);
            } else if (it[0] == 'A') {
                sprintf(buf, "Archive requested for %.36s", label);  /* TS admin:archive-channel has no server handler */
            } else {
                ae_chat_room_delete(&chat, r->id);
                if (strcmp(r->id, room_id) == 0) leave_room();
                load_sidebar();
                sprintf(buf, "Delete requested for %.36s", label);
            }
            alert_line(buf);
        }
    }
}

static void side_move(int step);
static void side_pick(void);
static int menu_pick(int menu, int item);

/** The mouse wake byte: drain the board's queue and act on each event as TS handlers/mouse-handling.ts. */
static int handle_mouse(void)
{
    ae_mouse_event evs[16];
    int n = ae_bs_mouse_drain(evs, 16), i;
    for (i = 0; i < n; i++) {
        ae_mouse_event *e = &evs[i];
        int row = e->row - 1, col = e->col - 1, idx, hit;
        mouse_events++;
        if (e->kind == 'U' || e->kind == 'D') {
            int up = e->kind == 'U';
            if (lc_hit(row, col, &idx) == LC_HIT_SIDEBAR) side_move(up ? -2 : 2);
            else { M.scroll += up ? 3 : -3; if (M.scroll < 0) M.scroll = 0; if (M.scroll > LC_LINES * 4) M.scroll = LC_LINES * 4; }
            continue;
        }
        if (bar_drag) {                         /* the thumb follows the pointer until the button comes up */
            if (e->kind == 'R') bar_drag = 0;
            else if (e->kind == 'M' && e->button != AE_MOUSE_NONE) { int sc = lc_scroll_at(row); if (sc >= 0) M.scroll = sc; }
            else if (e->kind == 'M') bar_drag = 0;   /* a hover with no button: the release was lost */
            if (e->kind != 'P') continue;
            bar_drag = 0;
        }
        if (e->kind != 'P') continue;          /* act on the press; releases and other motion carry nothing here */
        hit = lc_hit(row, col, &idx);
        if (M.fmt) {                            /* the picker is on top: it takes the press, or a left press outside closes it */
            int shown = hit == LC_HIT_FMT || hit == LC_HIT_FMT_CAT || hit == LC_HIT_FMT_ITEM || hit == LC_HIT_FMT_CLOSE;
            fmt_click(hit, idx, e->button);
            if (shown || e->button != AE_MOUSE_LEFT) continue;
        }
        if (M.ctx.open) {
            int pick = ui_popup_click(&M.ctx, row, col);
            if (pick >= 0) ctx_run(M.ctx.items[pick]);
            else if (pick == UI_POPUP_CLOSE) kind_cancelled();
            if (pick != UI_POPUP_CLOSE || hit == LC_HIT_POPUP) continue;
        }
        if (M.modal == LC_MODAL_QUIT) {
            if (hit == LC_HIT_YES) return 1;
            if (hit == LC_HIT_NO) M.modal = LC_MODAL_NONE;
            continue;
        }
        if (M.modal == LC_MODAL_RECONNECT) {
            if (hit == LC_HIT_YES && reconnect_retry()) return 1;
            if (hit == LC_HIT_NO) return 1;
            continue;
        }
        if (M.modal == LC_MODAL_MESSAGE) { if (hit == LC_HIT_OK || hit != LC_HIT_MODAL) modal_close(); continue; }
        if (M.theme) {                          /* a click on a theme previews it; a click outside the panel is ignored */
            if (hit == LC_HIT_THEME_ROW && idx >= 0 && idx < ui_theme_count()) theme_show(idx);
            continue;
        }
        if (M.settings) {                       /* a click on an item picks it, as Space does */
            if (hit == LC_HIT_SETTINGS_ROW) settings_pick(idx);
            continue;
        }
        if (M.search) {                         /* TS: a click on a field focuses it; a click on a result selects it */
            if (hit == LC_HIT_SEARCH_Q) M.search_field = 0;
            else if (hit == LC_HIT_SEARCH_USER) M.search_field = 1;
            else if (hit == LC_HIT_SEARCH_ROW && idx >= 0 && idx < M.search_n) M.search_sel = idx;
            continue;
        }
        if (M.modal != LC_MODAL_NONE || M.help || M.view) continue;
        if (hit == LC_HIT_MENU && idx >= 0) { M.menu = idx; M.menu_sel = 0; M.focus = LC_FOCUS_MENU; continue; }
        if (hit == LC_HIT_DROPDOWN && M.menu >= 0 && idx >= 0 && idx < lc_menu_count[M.menu]) {
            int menu = M.menu;
            M.menu = -1;
            if (menu_pick(menu, idx)) return 1;
            continue;
        }
        if (M.focus == LC_FOCUS_MENU) set_focus(LC_FOCUS_INPUT);
        if (hit == LC_HIT_STATUS) { M.sidebar_hidden = 0; switch_tab(idx >= 7); continue; }   /* TS mouse-handling: x < 7 channels, else users */
        if (hit == LC_HIT_INPUT) {              /* TS: a right-click on the input with a selection shows the format picker */
            set_focus(LC_FOCUS_INPUT);
            if (e->button == AE_MOUSE_RIGHT) fmt_show();
            continue;
        }
        if (hit == LC_HIT_SCROLLBAR) {          /* press jumps the thumb under the pointer; held, it drags */
            int sc = lc_scroll_at(row);
            if (sc >= 0) M.scroll = sc;
            bar_drag = e->button == AE_MOUSE_LEFT;
            continue;
        }
        if (hit == LC_HIT_CHAT) { ctx_open(LC_CTX_CHAT, idx, row, col); continue; }
        if (hit == LC_HIT_SIDEBAR && side_selectable(idx)) {
            lc_side *s = &M.side[idx];
            M.side_sel = idx;
            if (s->kind == LC_SIDE_USER && !is_self(s->label)) ctx_open(LC_CTX_USER, idx, row, col);
            else if (s->kind == LC_SIDE_ROOM) ctx_open(LC_CTX_CHANNEL, idx, row, col);
            else if (e->button == AE_MOUSE_LEFT) side_pick();
        }
    }
    return 0;
}

/** A menu entry. Returns 1 when the door should end. */
static int menu_pick(int menu, int item)
{
    const char *it = lc_menu_items[menu][item];
    set_focus(LC_FOCUS_INPUT);
    if (strncmp(it, "Help (", 6) == 0 || strcmp(it, "Keyboard Shortcuts") == 0) open_help();
    else if (strncmp(it, "Channel List", 12) == 0 || strcmp(it, "Toggle Sidebar") == 0) M.sidebar_hidden = !M.sidebar_hidden;
    else if (strncmp(it, "Next Channel", 12) == 0) next_channel();
    else if (strcmp(it, "Join Channel...") == 0) open_prompt(PROMPT_JOIN, "Channel to join");
    else if (strcmp(it, "Leave Channel") == 0) leave_room();
    else if (strcmp(it, "Clear Chat") == 0) lines_clear();
    else if (strcmp(it, "About") == 0) message_modal("LIVECHAT68", "Federated chat for AmiExpress boards");
    else if (strncmp(it, "Quit", 4) == 0) M.modal = LC_MODAL_QUIT;
    else if (strncmp(it, "Pins", 4) == 0) run_command("/pinned");
    else if (strncmp(it, "Search", 6) == 0) search_open();
    else if (strncmp(it, "Threads", 7) == 0) {
        const lc_line *l = room_last();
        if (l != 0 && l->id[0] != '\0') thread_view(l->id);
        else system_line("Threads: use /threads or reply on any message to open the thread view.");
    }
    else if (strncmp(it, "Emoji", 5) == 0) emoji_show();
    else if (strncmp(it, "Settings", 8) == 0) settings_open();
    else if (strcmp(it, "Theme") == 0) theme_open();
    else not_yet(it);                       /* Files */
    return 0;
}

static void side_move(int step)
{
    int i = M.side_sel + step;
    while (i >= 0 && i < M.nside && !side_selectable(i)) i += step;
    if (side_selectable(i)) M.side_sel = i;
}

static void side_pick(void)
{
    lc_side *s;
    if (!side_selectable(M.side_sel)) return;
    s = &M.side[M.side_sel];
    if (s->kind == LC_SIDE_ROOM) { if (!s->joined || dm_thread[0] != '\0') join_room(s->id); }
    else if (s->kind == LC_SIDE_DM) { open_dm(s->id, s->label); set_focus(LC_FOCUS_INPUT); }
    else if (s->kind == LC_SIDE_USER) {
        sprintf(M.input, "/msg %.30s ", s->label);
        M.input_len = (int)strlen(M.input);
        M.input_cur = M.input_len;
        M.input_anchor = -1;
        set_focus(LC_FOCUS_INPUT);
    }
}

/** The "/" menu (TS command-palette.ts): while the input starts with "/" and something matches, Up/Down
 *  move, Tab/Right fill "/name ", Enter runs an exact "/name" or fills, Esc hides. Returns 1 when the key
 *  was used; any other key falls through to the input and re-arms the menu. */
static int slash_key(int key)
{
    int idx[LC_SLASH_MAX], n = lc_slash_match(M.input, idx, LC_SLASH_MAX), pick, ghost;
    char fill[40];
    if (n == 0) { M.slash_sel = 0; M.slash_top = 0; M.slash_off = 0; return 0; }
    if (M.slash_off) {
        if (!is_text(key) && key != 8 && key != 127) return 0;
        M.slash_off = 0;                                 /* typing re-arms the menu (TS shows it after each key) */
    }
    if (M.slash_sel >= n) M.slash_sel = n - 1;
    if (key == UI_KEY_UP || key == UI_KEY_DOWN) {
        M.slash_sel += key == UI_KEY_UP ? -1 : 1;
        if (M.slash_sel < 0) M.slash_sel = 0;
        if (M.slash_sel > n - 1) M.slash_sel = n - 1;
        M.slash_top = lc_slash_scroll(M.slash_sel, M.slash_top, n);
        return 1;
    }
    if (key == UI_KEY_ESC) { M.slash_off = 1; return 1; }
    if (key != 9 && key != UI_KEY_RIGHT && !is_enter(key)) {
        M.slash_sel = 0; M.slash_top = 0;               /* an edit changes the term: selection restarts at the top */
        return 0;
    }
    ghost = lc_slash_ghost(M.input, idx, n);
    if (!is_enter(key)) {                                /* TS: Tab/Right accept the ghost; without one Tab hides, Right moves the cursor */
        if (ghost < 0) {
            if (key == 9) { M.slash_off = 1; return 1; }
            return 0;
        }
        pick = ghost;
    } else {
        pick = ghost >= 0 ? ghost : idx[M.slash_sel];   /* TS enterOnCommandSuggestions: the ghost outranks the selected row */
        if (lc_slash_exact(M.input, pick)) return 0;    /* "/name" typed in full: the normal submit runs it */
    }
    sprintf(fill, "/%s ", lc_cmd_help[pick][0]);
    copy(M.input, sizeof M.input, fill);
    M.input_len = M.input_cur = (int)strlen(M.input);
    M.input_anchor = -1;
    M.slash_sel = 0; M.slash_top = 0;
    M.slash_off = 1;                                     /* TS hides the list after a fill; the next typed key re-arms it */
    return 1;
}

/** TS pickRoomKind(null): the type picker closed without a pick, so nothing reaches the hub. */
static void kind_cancelled(void)
{
    char line[64];
    if (M.ctx_kind != LC_CTX_KIND || pending_room[0] == '\0') return;
    sprintf(line, "#%.40s was not created.", pending_room);
    system_line(line);
    pending_room[0] = '\0';
}

/** One key. Returns 1 when the door should end. */
static int handle_key(int key)
{
    if (key == UI_KEY_MOUSE) return handle_mouse();
    if (M.ctx.open) {
        int pick = ui_popup_key(&M.ctx, key);
        if (pick >= 0) ctx_run(M.ctx.items[pick]);
        else if (pick == UI_POPUP_CLOSE) kind_cancelled();
        return 0;
    }
    if (M.help) {
        if (key == UI_KEY_DOWN && M.help_top < lc_help_lines - 1) M.help_top++;
        else if (key == UI_KEY_UP && M.help_top > 0) M.help_top--;
        else if (key == UI_KEY_PGDN) { M.help_top += 10; if (M.help_top > lc_help_lines - 1) M.help_top = lc_help_lines - 1; }
        else if (key == UI_KEY_PGUP) { M.help_top -= 10; if (M.help_top < 0) M.help_top = 0; }
        else if (key == UI_KEY_ESC || key == 'q') M.help = 0;
        return 0;
    }
    if (M.view) {
        if (key == UI_KEY_DOWN && M.view_top < M.view_n - 1) M.view_top++;
        else if (key == UI_KEY_UP && M.view_top > 0) M.view_top--;
        else if (key == UI_KEY_PGDN) { M.view_top += 10; if (M.view_top > M.view_n - 1) M.view_top = M.view_n - 1; }
        else if (key == UI_KEY_PGUP) { M.view_top -= 10; if (M.view_top < 0) M.view_top = 0; }
        else if (key == UI_KEY_ESC || key == 'q') M.view = 0;
        return 0;
    }
    if (M.search) { search_key(key); return 0; }
    if (M.settings) { settings_key(key); return 0; }
    if (M.theme) { theme_key(key); return 0; }
    if (M.modal == LC_MODAL_MESSAGE) { modal_close(); return 0; }
    if (M.modal == LC_MODAL_RECONNECT) {
        if (key == 'r' || key == 'R' || is_enter(key)) return reconnect_retry();
        if (key == 'c' || key == 'C' || key == UI_KEY_ESC) return 1;
        return 0;
    }
    if (M.modal == LC_MODAL_QUIT) {
        if (key == 'y' || key == 'Y' || is_enter(key)) return 1;
        if (key == 'n' || key == 'N' || key == UI_KEY_ESC) M.modal = LC_MODAL_NONE;
        return 0;
    }
    if (M.modal == LC_MODAL_PROMPT) {
        if (key == UI_KEY_ESC) { M.modal = LC_MODAL_NONE; return 0; }
        if (is_enter(key)) {
            char text[sizeof M.prompt];
            copy(text, sizeof text, M.prompt);
            M.modal = LC_MODAL_NONE;
            if (text[0] == '\0') return 0;
            if (prompt_kind == PROMPT_JOIN) join_typed(text);
            else if (prompt_kind == PROMPT_NOTE) note_typed(text);
            else create_typed(text, "public");
            return 0;
        }
        if ((key == 8 || key == 127) && M.prompt_len > 0) { M.prompt[--M.prompt_len] = '\0'; return 0; }
        if (is_text(key) && M.prompt_len + 1 < (int)sizeof M.prompt) { M.prompt[M.prompt_len++] = (char)key; M.prompt[M.prompt_len] = '\0'; }
        return 0;
    }
    if (M.fmt) { fmt_key(key); return 0; }
    if (key == 17) { M.modal = LC_MODAL_QUIT; return 0; }
    if (key == 19) { settings_open(); return 0; }  /* TS: Ctrl+S opens the settings */
    if (key == 6) { search_open(); return 0; }     /* TS: Ctrl+F opens the message search */
    if (M.focus == LC_FOCUS_INPUT && M.pick[0] == '\0' && slash_key(key)) return 0;
    if (key == UI_KEY_F1) { open_help(); return 0; }
    if (key == UI_KEY_F2) { M.sidebar_hidden = !M.sidebar_hidden; if (M.focus == LC_FOCUS_SIDEBAR) set_focus(LC_FOCUS_INPUT); return 0; }
    if (key == UI_KEY_F3) { switch_tab(!M.side_tab); return 0; }
    if (key == UI_KEY_F4) { emoji_show(); return 0; }       /* TS server.ts F4 */
    if (key == UI_KEY_F6) { system_line("File sharing is not on Amiga yet."); return 0; }  /* TS F6: file manager */
    if (key == UI_KEY_F7) { pinned_typed(); return 0; }     /* TS server.ts F7: getPinnedMessages */
    if (key == UI_KEY_F5) {                                  /* TS server.ts: F5 formats the selection */
        if (!fmt_show()) system_line("Select text first (Shift+Arrow keys), then press F5 for formatting");
        return 0;
    }
    if (key == 9) { cycle_focus(1); return 0; }
    if (key == UI_KEY_BACKTAB) { cycle_focus(-1); return 0; }
    if (key == UI_KEY_PGUP) { M.scroll += 10; if (M.scroll > LC_LINES * 4) M.scroll = LC_LINES * 4; return 0; }
    if (key == UI_KEY_PGDN) { M.scroll -= 10; if (M.scroll < 0) M.scroll = 0; return 0; }

    if (M.pick[0] != '\0') {                  /* the keyboard picker owns Up/Down/Enter/Esc (TS server.ts) */
        if (key == UI_KEY_UP) { lc_pick_move(&M, -1); return 0; }
        if (key == UI_KEY_DOWN) { lc_pick_move(&M, 1); return 0; }
        if (key == UI_KEY_ESC) { lc_pick_stop(&M); return 0; }
        if (is_enter(key)) {                    /* confirm: the chat menu a click on the message opens */
            int idx = lc_pick_index(&M);
            lc_pick_stop(&M);
            if (idx >= 0) ctx_open(LC_CTX_CHAT, idx, 3, lc_sidebar_width(&M) + 2);
            return 0;
        }
        if (is_text(key)) lc_pick_stop(&M);    /* a typed character cancels the pick and is still typed */
    }
    if (M.focus == LC_FOCUS_MENU) {
        if (key == UI_KEY_LEFT) { M.menu = (M.menu + LC_MENUS - 1) % LC_MENUS; M.menu_sel = 0; }
        else if (key == UI_KEY_RIGHT) { M.menu = (M.menu + 1) % LC_MENUS; M.menu_sel = 0; }
        else if (key == UI_KEY_DOWN) M.menu_sel = (M.menu_sel + 1) % lc_menu_count[M.menu];
        else if (key == UI_KEY_UP) M.menu_sel = (M.menu_sel + lc_menu_count[M.menu] - 1) % lc_menu_count[M.menu];
        else if (is_enter(key)) return menu_pick(M.menu, M.menu_sel);
        else if (key == UI_KEY_ESC) set_focus(LC_FOCUS_INPUT);
        return 0;
    }
    if (M.focus == LC_FOCUS_SIDEBAR) {
        if (key == UI_KEY_UP) side_move(-1);
        else if (key == UI_KEY_DOWN) side_move(1);
        else if (key == UI_KEY_HOME) { M.side_sel = 0; side_move(1); }
        else if (key == UI_KEY_END) { M.side_sel = M.nside - 1; if (!side_selectable(M.side_sel)) side_move(-1); }
        else if (is_enter(key)) side_pick();
        else if (key == UI_KEY_ESC) set_focus(LC_FOCUS_INPUT);
        return 0;
    }
    if (is_enter(key)) return submit();
    if (key == UI_KEY_ESC) {
        if (M.input_anchor >= 0) { M.input_anchor = -1; return 0; }   /* Esc drops a selection first */
        if (dm_thread[0] != '\0' && room_id[0] != '\0') join_room(room_id);   /* out of the DM, back to the room */
        return 0;
    }
    {
        int cap = LC_INPUT_MAX < AE_CHAT_TEXT_MAX + 1 ? LC_INPUT_MAX : AE_CHAT_TEXT_MAX + 1;
        if (!ui_line_sel_key(M.input, cap, &M.input_len, &M.input_cur, &M.input_anchor, key, &hist, 0)
            && key == UI_KEY_UP && M.input_len == 0)
            lc_pick_start(&M);                  /* Up on an empty input picks a message (TS start({kind:'menu'})) */
        M.editing = ui_hist_editing(&hist) != 0;
        fmt_watch();                            /* a selection held still opens the format picker (TS 500 ms debounce) */
    }
    return 0;
}

/** A modal before the chat runs (no config, no server): shown, one key, gone. */
static void early_message(const char *title, const char *text)
{
    message_modal(title, text);
    present();
    ui_key_read(&keys, &pushback);
}

/** Row `row` of the wizard: label, then what the sysop answered. */
static void wizard_say(int row, const char *label, const char *value)
{
    char line[120];
    sprintf(line, "%-12s %.80s", label, value);
    ansi_text(&screen.buf, row, 3, line, screen.cols - 4);
}

/** One wizard field. Returns the ui_input result: 1 entered, 0 empty, -1 caller gone. */
static int wizard_field(ui_input_style *st, int row, const char *label, char *buf, int cap)
{
    int r;
    st->row = row;
    r = ui_input(&screen.buf, &keys, &pushback, st, label, buf, cap);
    if (r >= 0) wizard_say(row, label, buf);
    return r;
}

/**
 * First-run setup for a sysop: ask for the board's name, ask the hub for a key (A2), write
 * LIVECHAT.cfg (ae_chat_config_save owns the format). Returns 1 with `cfg` ready to connect,
 * 0 when the sysop left, -1 when the caller went away. Nothing is written unless the hub answered.
 */
static int setup_wizard(ae_chat_config *cfg)
{
    ui_input_style st;
    char name[48], sysop[48], contact[88], host[64], port[8], err[160], key[128];
    int r, rc;

    name[0] = '\0'; contact[0] = '\0'; port[0] = '\0';
    copy(sysop, sizeof sysop, handle);
    ae_chat_config_parse(cfg, "");
    copy(host, sizeof host, "doors.uprough.net");
    sprintf(port, "%d", 443);

    ui_input_style_init(&st);
    st.left = 3;
    st.cols = screen.cols - 6;
    for (;;) {
        int tls;
        ansi_clear(&screen.buf);
        ansi_text(&screen.buf, 1, 3, "LiveChat68 setup", screen.cols - 4);
        ansi_text(&screen.buf, 2, 3, "Your board has no key yet. Register it with the chat server.", screen.cols - 4);
        ansi_text(&screen.buf, 3, 3, "Leave the board name empty to cancel.", screen.cols - 4);
        r = wizard_field(&st, 5, "Board name", name, sizeof name);
        if (r <= 0) return r;
        if ((r = wizard_field(&st, 7, "Sysop name", sysop, sizeof sysop)) < 0) return r;
        if (sysop[0] == '\0') copy(sysop, sizeof sysop, handle);
        if ((r = wizard_field(&st, 9, "Contact", contact, sizeof contact)) < 0) return r;
        if ((r = wizard_field(&st, 11, "Host", host, sizeof host)) < 0) return r;
        if ((r = wizard_field(&st, 13, "Port", port, sizeof port)) < 0) return r;
        st.row = 15;
        tls = ui_confirm(&screen.buf, &keys, &pushback, &st, "Use TLS (HTTPS)?", 1);
        if (tls < 0) return -1;

        if (host[0] != '\0') copy(cfg->host, sizeof cfg->host, host);
        cfg->port = atoi(port) > 0 && atoi(port) < 65536 ? atoi(port) : (tls ? 443 : 80);
        cfg->tls = tls;
        cfg->allow_plain_http = !tls;                      /* choosing plain HTTP here is the sysop's say-so */
        ansi_text(&screen.buf, 17, 3, "Registering...", screen.cols - 4);
        ui_screen_flush(&screen);
        rc = ae_chat_register(cfg, name, sysop, contact, key, (int)sizeof key, err, (int)sizeof err);
        if (rc == AE_CHAT_OK) {
            copy(cfg->board_key, sizeof cfg->board_key, key);
            if (ae_chat_config_save(cfg, LIVECHAT_CFG_PATH) != 0) {
                early_message("Setup", "Could not write LIVECHAT.cfg. Is the Doors drawer writable?");
                return 0;
            }
            return 1;
        }
        if (rc == AE_CHAT_ERR_TRANSPORT) copy(err, sizeof err, "Cannot reach the chat server.");
        else if (err[0] == '\0') copy(err, sizeof err, "The chat server gave an unexpected answer.");
        ansi_text(&screen.buf, 17, 3, "Registration failed:", screen.cols - 4);
        ansi_text(&screen.buf, 18, 3, err, screen.cols - 4);
        st.row = 20;
        r = ui_confirm(&screen.buf, &keys, &pushback, &st, "Try again?", 1);
        if (r <= 0) return r < 0 ? -1 : 0;
    }
}

static void finish(void)
{
    ui_cells_close(&cells);
    ui_screen_close(&screen);
    ae_shutdown();
}

int main(int argc, char **argv)
{
    ae_session session;
    ae_chat_config cfg;
    char line[96];
    int node = (argc > 1) ? atoi(argv[1]) : 1;
    int ended = 0;

    /* A fresh run starts with nothing joined (a host test calls main more than once). */
    idle_ticks = 0; pushback = -1; msg_number = 0; help_shown = 0; rooms_count = 0; keep_lines = 0;
    room_id[0] = '\0'; room_name[0] = '\0'; room_topic[0] = '\0'; dm_thread[0] = '\0'; dm_peer[0] = '\0';
    nignored = 0; nmuted = 0; nblocked = 0;
    typing_sent[0] = typing_room[0] = typing_seen[0] = '\0'; typing_wait = typing_pending = typing_blink_t = 0;
    snd_on = 1; show_typing = 1; ev_logins = ev_files = ev_doors = ev_messages = ev_announce = 1; ev_mute_all = 0;
    pinned_rooms.n = expanded_rooms.n = collapsed_rooms.n = unread_rooms.n = 0;
    fmt_wait = 0; fmt_seen_a = fmt_seen_b = -1;

    if (ae_start(node) != 0) return 20;
    if (ae_open_bbs(&session, session_storage, (long)sizeof(session_storage), node) != 0) {
        ae_shutdown();
        return 20;
    }
    ui_screen_open_responsive(&screen, &session, frame_buffer, (long)sizeof(frame_buffer));
    ae_user_name(&session, handle, (int)sizeof(handle));
    if (handle[0] == '\0') strcpy(handle, "Anon");
    lc_set_self(handle);                                      /* its @mentions are highlighted in incoming lines */
    if (ui_cells_open(&cells, screen.cols, screen.rows) != 0) {
        ui_screen_close(&screen);
        ae_shutdown();
        return 20;
    }
    lc_model_init(&M, screen.cols, screen.rows);
    copy(M.handle, sizeof M.handle, handle);
    M.node = node;
    {   /* the caller's theme: every colour of the door is a token of it, as T.* are in TS */
        char saved[32];
        saved[0] = '\0';
        ae_user_theme(&session, saved, (int)sizeof saved);
        theme_session = &session;
        copy(theme_id, sizeof theme_id, ui_theme_by_id(saved)->id);
        M.rgb = ui_theme_by_id(theme_id)->rgb;
        lc_theme_set(M.rgb);
    }

    keys.next = door_key;
    keys.pending = door_pending;
    keys.settle = door_settle;
    keys.idle = 0;                                           /* no polling before there is a chat */
    keys.flush = door_flush;
    keys.ctx = 0;

    ansi_clear(&screen.buf);
    ui_cells_invalidate(&cells);

    user_level = ae_user_level(&session);
    if (!ae_chat_config_load(&cfg, LIVECHAT_CFG_PATH)) {
        /* A sysop sets the door up here; anyone else is told to ask one. */
        if (user_level < 255 || setup_wizard(&cfg) != 1) {
            if (user_level < 255) early_message("Setup", "No LIVECHAT.cfg. Setup needs access 255.");
            finish();
            return 0;
        }
        ansi_clear(&screen.buf);
        ui_cells_invalidate(&cells);
    }
    if (ae_chat_config_refusal(&cfg) != 0) {
        early_message("Setup", ae_chat_config_refusal(&cfg));
        finish();
        return 0;
    }
    if (ae_chat_open(&chat, &cfg, handle, (is_sysop = user_level >= 255)) != AE_CHAT_OK) {
        /* A connect failure carries its reason (certificate rejected, AmiSSL missing...) */
        early_message("Error", chat.http_error == AE_HTTP_ERR_CONNECT ? ae_net_last_error()
                                                                      : "The chat server did not answer.");
        finish();
        return 0;
    }
    keys.idle = door_idle;
    ae_bs_mouse(1);                                     /* "MOUSE ON B": clicks with their button, as menudoor */

    load_custom_emoji();
    load_ignores();                                     /* the server keeps the list; hiding the lines is ours */
    /* As the TS door: list the rooms and walk into the first. */
    if (load_rooms() == AE_CHAT_OK) {
        prefs_load();                                /* TS chat:prefs: stored prefs, pinned names -> ids */
        sprintf(line, "Found %d room%s", rooms_count, rooms_count == 1 ? "" : "s");
        system_line(line);
        if (rooms_count > 0) {
            sprintf(line, "Auto-joining #%.40s...", rooms_name[0]);
            system_line(line);
            keep_lines = 1;                              /* the TS door keeps these two lines above the join */
            join_room(rooms_id[0]);
            keep_lines = 0;
        } else {
            system_line("No rooms yet. Type /create name to make one.");
            load_sidebar();
        }
    } else {
        system_line("Could not list the rooms.");
    }
    present();

    while (!ended) {
        int key = ui_key_read(&keys, &pushback);
        if (key < 0) break;
        /* idle_ticks is NOT reset here: it counts the idle hook's sleeps, the door's only clock. The
           board's any-motion mouse mode sends a wake byte on every hover move, and a reset per key
           kept the event poll from ever running while the mouse moved (sysop test 2026-11-08). */
        ended = handle_key(key);
        if (!ended) { typing_sync(); present(); }
    }

    ae_bs_mouse(0);
    typing_stop();
    if (room_id[0] != '\0') ae_chat_leave(&chat, room_id);
    ae_chat_close(&chat);
    finish();
    return 0;
}
