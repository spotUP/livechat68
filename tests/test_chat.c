/* test_chat.c - ae_chat (the /chat/v1 client) and the LIVECHAT68 door, against a routed stub server.
 *
 * Three layers: the config parser and the client calls are checked directly; the door's real main() is
 * then driven with scripted keys, and what it did is read off the stub's POST counters (the reachability
 * proof: a count that moves only if the door's code reached the wire) and off the rendered screen. */
#define main livechat_main
#define LIVECHAT_CFG_PATH "build/test_chat.cfg"
#include "../examples/livechat/livechat.c"
#undef main
#include "../include/ae_transport_native.h"
#include "stub_server.h"
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static char theme_written[64];                     /* the last AE_FIELD_THEME value the door saved */
int ae_field_write(int field, const char *value)
{
    if (field == AE_FIELD_THEME) { strncpy(theme_written, value, sizeof theme_written - 1); theme_written[sizeof theme_written - 1] = '\0'; }
    return 0;
}
int ae_query_bit(int acs) { (void)acs; return 1; }
static const char *user_level_text = "255";        /* the caller's level the board reports */

int ae_field_read(int field, char *out, int cap)
{
    const char *text = field == AE_FIELD_USER_LEVEL ? user_level_text : "Tester";
    int n = (int)strlen(text);
    if (cap <= 0) return 0;
    if (n > cap - 1) n = cap - 1;
    memcpy(out, text, (size_t)n); out[n] = '\0';
    return n;
}

/* Scripted clicks: each MOUSE? answers the next step, worked out from the frame the door drew
 * (the test includes livechat.c, so it reads M and lc_hit). "" ends one wake's drain. */
enum { MS_END, MS_RMB_MESSAGE, MS_CLICK_REACT, MS_CLICK_PICK, MS_CLICK_TOOLS, MS_CLICK_FILES, MS_CLICK_STATUS_USERS, MS_CLICK_STATUS_CHANNELS,
       MS_CLICK_USER, MS_CLICK_ITEM, MS_CLICK_ROOM, MS_FILL, MS_BAR_PRESS, MS_BAR_DRAG, MS_BAR_RELEASE, MS_BAR_HOVER, MS_CHECK };
static const char *ms_items[8];                           /* names the MS_CLICK_ITEM steps click, in order */
static int ms_item_n;
static const char *ms_rooms[4];                           /* sidebar rooms the MS_CLICK_ROOM steps right-click, in order */
static int ms_room_n;
static const int *mouse_script;
static int mouse_step, mouse_armed;
static int ms_bar_top_scroll, ms_bar_mid_scroll;

static int mouse_reply(char *out)
{
    int step = mouse_script ? mouse_script[mouse_step] : MS_END, r, c, idx;
    if (step == MS_END) { if (mouse_script) mouse_step++; return 0; }
    mouse_step++;
    if (step == MS_FILL) {                              /* enough chat for a scrollbar; the next wake draws it */
        for (r = 0; r < 60; r++) system_line("scrollback filler");
        return 0;
    }
    if (step >= MS_BAR_PRESS && step <= MS_CHECK) {    /* the chat scrollbar column, its top and bottom rows */
        static int bar_c = -1, bar_top, bar_bottom;
        if (step == MS_BAR_PRESS) {
            for (r = 0; r < 25 && bar_c < 0; r++)
                for (c = 0; c < 80; c++) if (lc_hit(r, c, &idx) == LC_HIT_SCROLLBAR) { bar_c = c; bar_top = r; break; }
            assert(bar_c >= 0 && M.scroll == 0);
            for (bar_bottom = bar_top; lc_hit(bar_bottom + 1, bar_c, &idx) == LC_HIT_SCROLLBAR; bar_bottom++) {}
            return sprintf(out, "P\t%d\t%d\t0", bar_c + 1, bar_top + 1);      /* track top: the oldest lines */
        }
        if (step == MS_BAR_DRAG) {
            assert(M.scroll > 0 && M.ctx.open == 0);        /* the press scrolled back, no chat menu opened */
            ms_bar_top_scroll = M.scroll;
            return sprintf(out, "M\t%d\t%d\t0", bar_c + 1, (bar_top + bar_bottom) / 2 + 1);
        }
        if (step == MS_BAR_RELEASE) {
            assert(M.scroll > 0 && M.scroll < ms_bar_top_scroll);   /* held motion moved the thumb to the middle */
            ms_bar_mid_scroll = M.scroll;
            return sprintf(out, "R\t%d\t%d\t0", bar_c + 1, bar_bottom + 1);
        }
        if (step == MS_BAR_HOVER) return sprintf(out, "M\t%d\t%d\t3", bar_c + 1, bar_bottom + 1);
        assert(M.scroll == ms_bar_mid_scroll);              /* MS_CHECK: release and hover left it alone */
        bar_c = -1;
        return 0;
    }
    if (step == MS_RMB_MESSAGE) {                       /* the first chat row that shows a message */
        for (r = 0; r < 25; r++)
            if (lc_hit(r, 30, &idx) == LC_HIT_CHAT && idx >= 0 && M.lines[idx].number > 0)
                return sprintf(out, "P\t31\t%d\t2", r + 1);
        return sprintf(out, "P\t31\t4\t2");
    }
    if (step == MS_CLICK_REACT) {                       /* item 2 of the open popup */
        for (idx = 0; idx < M.ctx.n && strcmp(M.ctx.items[idx], "React") != 0; idx++) {}
        return sprintf(out, "P\t%d\t%d\t0", M.ctx.left + 5, M.ctx.top + 1 + idx + 1);
    }
    if (step == MS_CLICK_USER) {                        /* alice in the sidebar opens the user menu */
        for (r = 0; r < 25; r++)
            if (lc_hit(r, 3, &idx) == LC_HIT_SIDEBAR && M.side[idx].kind == LC_SIDE_USER && strcmp(M.side[idx].label, "alice") == 0)
                return sprintf(out, "P\t4\t%d\t2", r + 1);
        assert(!"alice not in the sidebar");
    }
    if (step == MS_CLICK_ROOM) {                        /* a named room in the sidebar opens the channel menu */
        const char *want = ms_rooms[ms_room_n++];
        for (r = 0; r < 25; r++)
            if (lc_hit(r, 3, &idx) == LC_HIT_SIDEBAR && M.side[idx].kind == LC_SIDE_ROOM && strcmp(M.side[idx].label, want) == 0)
                return sprintf(out, "P\t4\t%d\t2", r + 1);
        assert(!"room not in the sidebar");
    }
    if (step == MS_CLICK_ITEM) {                        /* a named entry of the open menu */
        const char *want = ms_items[ms_item_n++];
        assert(M.ctx.n > 0);
        for (idx = 0; idx < M.ctx.n && strcmp(M.ctx.items[idx], want) != 0; idx++) {}
        assert(idx < M.ctx.n);
        return sprintf(out, "P\t%d\t%d\t0", M.ctx.left + 5, M.ctx.top + 1 + idx + 1);
    }
    if (step == MS_CLICK_PICK) {                        /* the reaction picker's first item, +1 */
        assert(M.ctx_kind == LC_CTX_REACT && M.ctx.n == LC_REACT_KINDS);
        return sprintf(out, "P\t%d\t%d\t0", M.ctx.left + 5, M.ctx.top + 1 + 0 + 1);
    }
    if (step == MS_CLICK_STATUS_USERS) return sprintf(out, "P\t10\t25\t0");        /* status row, x = 9 >= 7 */
    if (step == MS_CLICK_STATUS_CHANNELS) return sprintf(out, "P\t3\t25\t0");      /* x = 2 < 7 */
    if (step == MS_CLICK_TOOLS) return sprintf(out, "P\t17\t1\t0");
    c = 16 + 1; r = 2 + 1 + 1;                          /* Tools dropdown: item 1 "Files (F6)" on row 3 */
    return sprintf(out, "P\t%d\t%d\t0", c + 1, r);
}

static char note_request[256];   /* the last AEW_USERNOTE request */
static const char *note_reply = "OK"; /* "" = the board cannot take notes */

static char prefs_stored[200];   /* the AEW_CHATPREFS GET reply ("" = nothing stored) */
static char prefs_pinned[200];   /* the GET\tpinned reply */
static char prefs_sets[2048];    /* every SET request, one per line */

static int board(void *ctx, int field, const char *request, char *out, int cap)
{
    (void)ctx; (void)cap;
    out[0] = '\0';
    if (field == AE_FIELD_CHATPREFS) {
        if (strcmp(request, "GET") == 0) strcpy(out, prefs_stored);
        else if (strcmp(request, "GET\tpinned") == 0) strcpy(out, prefs_pinned);
        else if (strncmp(request, "SET\t", 4) == 0) {
            if (strlen(prefs_sets) + strlen(request) + 2 < sizeof prefs_sets) { strcat(prefs_sets, request + 4); strcat(prefs_sets, "\n"); }
            strcpy(out, "OK");
        }
        return (int)strlen(out);
    }
    if (field == AE_FIELD_USERNOTE) {
        strncpy(note_request, request, sizeof note_request - 1);
        strcpy(out, note_reply);
        return (int)strlen(out);
    }
    if (field != AE_FIELD_SCREEN) return 0;
    if (strcmp(request, "SIZE") == 0) { strcpy(out, "80\t25"); return 4; }
    if (strcmp(request, "MOUSE ON B") == 0) { mouse_armed++; strcpy(out, "OK"); return 2; }
    if (strcmp(request, "MOUSE OFF") == 0) { strcpy(out, "OK"); return 2; }
    if (strcmp(request, "MOUSE?") == 0) return mouse_reply(out);
    return 0;
}

static char screen_text[262144];
static stub_chat_stats *st;
static int stub_pid, stub_port;

static void start_stub(void)
{
    FILE *f;
    stub_port = stub_chat_start(&st, &stub_pid);
    assert(stub_port > 0);
    f = fopen("build/test_chat.cfg", "w");
    assert(f);
    fprintf(f, "# test\nHost=127.0.0.1\nPort=%d\nTls=0\nAllowPlainHttp=1\nBoardKey=boardA.secret\nTimeout=5\n", stub_port);
    fclose(f);
}

static void stop_stub(void)
{
    stub_chat_stop(stub_pid); remove("build/test_chat.cfg");
    prefs_stored[0] = prefs_pinned[0] = prefs_sets[0] = '\0';   /* the next run starts with nothing stored */
}

/* Runs the door's main with `keys` on stdin (all at once, or via a slow feeder when `slow`: 1 pauses
 * after each CR, more than 1 is a pause in microseconds after every byte). */
static int drive(const char *keys, size_t n, int slow)
{
    char *argv[] = { (char *)"livechat", (char *)"1", 0 };
    int saved_out = dup(1), saved_in = dup(0), rc, fds[2];
    pid_t feeder = 0;
    size_t got;
    FILE *f;
    fflush(stdout);
    {
        int out = open("build/test_chat_screen.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        assert(out >= 0);
        dup2(out, 1); close(out);
    }
    if (slow) {
        assert(pipe(fds) == 0);
        feeder = fork();
        assert(feeder >= 0);
        if (feeder == 0) {
            size_t i;
            close(fds[0]);
            for (i = 0; i < n; i++) {
                (void)!write(fds[1], keys + i, 1);
                if (slow > 1) usleep((useconds_t)slow);      /* slow > 1: microseconds after every byte */
                else if (keys[i] == '\r') usleep(400000);
            }
            close(fds[1]);
            _exit(0);
        }
        close(fds[1]);
        dup2(fds[0], 0); close(fds[0]);
        clearerr(stdin);
    } else {
        f = fopen("build/test_chat_keys.bin", "wb"); assert(f);
        fwrite(keys, 1, n, f); fclose(f);
        {
            int in = open("build/test_chat_keys.bin", O_RDONLY);
            assert(in >= 0);
            dup2(in, 0); close(in);
            clearerr(stdin);
        }
    }
    rc = livechat_main(2, argv);
    fflush(stdout);
    dup2(saved_out, 1); close(saved_out);
    dup2(saved_in, 0); close(saved_in);
    clearerr(stdout); clearerr(stdin);
    if (feeder > 0) waitpid(feeder, 0, 0);
    f = fopen("build/test_chat_screen.bin", "rb"); assert(f);
    got = fread(screen_text, 1, sizeof screen_text - 1, f); screen_text[got] = '\0'; fclose(f);
    return rc;
}

#define KEYS(s) s, sizeof(s) - 1

static void test_config(void)
{
    ae_chat_config cfg;
    assert(ae_chat_config_parse(&cfg, "# c\nHost=doors.uprough.net\nPort=8080\nBoardKey=a.b\n; x\nTimeout=7\n") == 1);
    assert(strcmp(cfg.host, "doors.uprough.net") == 0 && cfg.port == 8080 && strcmp(cfg.board_key, "a.b") == 0 && cfg.timeout_secs == 7);
    assert(ae_chat_config_parse(&cfg, "Host=x\n") == 0);
    assert(cfg.port == 443 && cfg.tls == 1 && cfg.allow_plain_http == 0 && cfg.timeout_secs == AE_CHAT_DEFAULT_TIMEOUT);
    assert(ae_chat_config_refusal(&cfg) == 0);
    /* plain HTTP: refused unless the sysop allowed it; the port default follows the scheme */
    assert(ae_chat_config_parse(&cfg, "Host=x\nBoardKey=a.b\nTls=0\n") == 1);
    assert(cfg.port == 80 && ae_chat_config_refusal(&cfg) != 0);
    assert(ae_chat_config_parse(&cfg, "Host=x\nBoardKey=a.b\nTls=0\nAllowPlainHttp=1\n") == 1);
    assert(ae_chat_config_refusal(&cfg) == 0);
    {
        ae_chat refused;
        assert(ae_chat_config_parse(&cfg, "Host=127.0.0.1\nBoardKey=a.b\nTls=0\n") == 1);
        assert(ae_chat_open(&refused, &cfg, "h", 0) == AE_CHAT_ERR_ARGS);
    }
    assert(ae_chat_config_load(&cfg, "build/no-such-file.cfg") == 0);
    printf("  [OK] config parser: keys, comments, defaults, missing file\n");
}

static void test_client(void)
{
    ae_chat c;
    ae_chat_config cfg;
    char name[48], topic[80], motd[64], tid[32], *f[AE_CHAT_ROW_FIELDS];
    int members = 0, count = 0, n;
    FILE *cf;
    start_stub();
    assert(ae_chat_config_load(&cfg, "build/test_chat.cfg") == 1);
    assert(ae_chat_open(&c, &cfg, "Tester", 1) == AE_CHAT_OK && st->sessions == 1 && strcmp(c.sid, "sid0123456789") == 0);

    assert(ae_chat_rooms(&c) == AE_CHAT_OK);
    n = ae_chat_next_row(&c, f, AE_CHAT_ROW_FIELDS);
    assert(n >= 6 && strcmp(f[0], "lobby") == 0 && strcmp(f[3], "public") == 0 && atoi(f[4]) == 3);
    assert(ae_chat_next_row(&c, f, AE_CHAT_ROW_FIELDS) >= 6 && strcmp(f[0], "secret") == 0);
    assert(ae_chat_next_row(&c, f, AE_CHAT_ROW_FIELDS) == 0);

    assert(ae_chat_join(&c, "lobby", name, sizeof name, topic, sizeof topic, 0, 0, &members) == AE_CHAT_OK);
    assert(strcmp(name, "Lobby") == 0 && strcmp(topic, "Hello all") == 0 && members == 3 && c.last_seq == 5);
    assert(ae_chat_room_motd(&c, "lobby", "Be kind") == AE_CHAT_OK && st->room_motd_puts == 1);
    assert(strcmp(st->last_write_path, "/chat/v1/rooms/lobby/motd") == 0 && strcmp(st->last_write_body, "{\"motd\":\"Be kind\"}") == 0);
    assert(ae_chat_join(&c, "lobby", name, sizeof name, topic, sizeof topic, motd, sizeof motd, &members) == AE_CHAT_OK);
    assert(strcmp(motd, "Be kind") == 0);
    st->room_motd[0] = '\0';
    st->pending_row[0] = '\0';
    assert(ae_chat_join(&c, "nosuch", name, sizeof name, topic, sizeof topic, 0, 0, &members) == AE_CHAT_ERR_STATUS);
    assert(strcmp(c.error, "not_found") == 0);

    assert(ae_chat_history(&c, "lobby", 0, 20) == AE_CHAT_OK);
    assert(ae_chat_next_row(&c, f, AE_CHAT_ROW_FIELDS) == 8 && strcmp(f[7], "hello there!bar") == 0);   /* a typed '!' stays '!' */
    /* A field the server appended later is dropped, not folded into the last field a narrower caller reads. */
    assert(ae_chat_history(&c, "lobby", 0, 20) == AE_CHAT_OK);
    assert(ae_chat_next_row(&c, f, 6) == 8 && strcmp(f[5], "m1") == 0);

    assert(ae_chat_send(&c, "lobby", "hi", 0, 0) == AE_CHAT_OK && strcmp(st->last_message, "hi") == 0);
    assert(ae_chat_poll(&c, 0) == AE_CHAT_OK);
    assert(ae_chat_next_row(&c, f, AE_CHAT_ROW_FIELDS) == 8 && strcmp(f[3], "carol") == 0 && c.last_seq == 8);
    assert(ae_chat_react(&c, "m1", "+1", &count) == AE_CHAT_OK && count == 2 && strcmp(st->last_react, "+1") == 0);
    assert(ae_chat_unreact(&c, "m1", "+1", &count) == AE_CHAT_OK && count == 0 && st->unreacts == 1);
    assert(strcmp(st->last_react_msg, "m1") == 0 && st->session_closes == 0);       /* not taken for a session close */
    assert(ae_chat_reactions(&c, "m1") == AE_CHAT_OK && st->reaction_lists == 1);
    assert(ae_chat_next_row(&c, f, AE_CHAT_ROW_FIELDS) == 3 && strcmp(f[0], "+1") == 0 && strcmp(f[2], "1") == 0);
    assert(ae_chat_edit(&c, "m1", "fixed \"it\"") == AE_CHAT_OK && st->edits == 1);
    assert(strcmp(st->last_write_path, "/chat/v1/messages/m1") == 0 && strcmp(st->last_write_body, "{\"content\":\"fixed \\\"it\\\"\"}") == 0);
    assert(ae_chat_delete(&c, "m1") == AE_CHAT_OK && st->deletes == 1 && st->unreacts == 1 && st->session_closes == 0);
    assert(ae_chat_topic(&c, "lobby", "New topic") == AE_CHAT_OK && st->topics == 1);
    assert(strcmp(st->last_write_path, "/chat/v1/rooms/lobby/topic") == 0 && strcmp(st->last_write_body, "{\"topic\":\"New topic\"}") == 0);
    assert(ae_chat_moderate(&c, "lobby", "kick", "bob@AE", "spam", 0) == AE_CHAT_OK && st->moderations == 1);
    assert(strcmp(st->last_write_path, "/chat/v1/rooms/lobby/kick") == 0);
    assert(strcmp(st->last_write_body, "{\"user\":\"bob@AE\",\"reason\":\"spam\"}") == 0);
    assert(ae_chat_moderate(&c, "lobby", "mute", "bob@AE", 0, 15) == AE_CHAT_OK);
    assert(strcmp(st->last_write_body, "{\"user\":\"bob@AE\",\"minutes\":15}") == 0);
    assert(ae_chat_moderate(&c, "lobby", "deop", "bob@AE", "", 0) == AE_CHAT_OK && strcmp(st->last_write_path, "/chat/v1/rooms/lobby/deop") == 0);
    assert(ae_chat_moderate(&c, "lobby", "nuke", "bob@AE", 0, 0) == AE_CHAT_ERR_ARGS && st->moderations == 3);
    assert(ae_chat_invite(&c, "lobby", "bob@AE", 1) == AE_CHAT_OK && st->invites == 1);
    assert(strcmp(st->last_write_body, "{\"user\":\"bob@AE\"}") == 0);
    assert(ae_chat_invite(&c, "lobby", "bob@AE", 0) == AE_CHAT_OK && st->uninvites == 1);
    assert(strcmp(st->last_write_path, "/chat/v1/rooms/lobby/invites/bob%40AE") == 0 || strcmp(st->last_write_path, "/chat/v1/rooms/lobby/invites/bob@AE") == 0);
    st->pending_row[0] = '\0';
    assert(ae_chat_thread(&c, "m1", "reply") == AE_CHAT_OK && st->threads == 1);
    assert(ae_chat_dm(&c, "bob", "psst", tid, sizeof tid) == AE_CHAT_OK && strcmp(tid, "th1") == 0 && strcmp(st->last_dm_peer, "bob") == 0);
    assert(ae_chat_dm_list(&c) == AE_CHAT_OK && ae_chat_next_row(&c, f, AE_CHAT_ROW_FIELDS) >= 4 && strcmp(f[1], "alice") == 0);
    assert(ae_chat_who(&c) == AE_CHAT_OK && ae_chat_next_row(&c, f, AE_CHAT_ROW_FIELDS) >= 5 && strcmp(f[0], "alice") == 0);

    st->expire_next = 1;                                     /* the session died: one silent re-open, the call still lands */
    assert(ae_chat_send(&c, "lobby", "again", 0, 0) == AE_CHAT_OK && st->sessions == 2 && st->messages == 2);
    assert(st->bad_auth == 0 && st->bad_session == 0);

    assert(ae_chat_close(&c) == AE_CHAT_OK && st->session_closes == 1);
    stop_stub();
    cf = fopen("build/test_chat.cfg", "r");
    assert(cf == 0);
    printf("  [OK] client: every call reaches its route, rows unescape, an expired session re-opens, auth on every request\n");
}

/* The door's pane and sidebar are its lc_model M (a static of livechat.c, still set after main returns):
 * the screen is a cell diff, so the checks look at the model, not at the bytes sent. */
static int has_line(const char *nick, const char *text)
{
    int i;
    for (i = 0; i < M.nlines; i++) {
        if (nick != 0 && strcmp(M.lines[i].nick, nick) != 0) continue;
        if (strstr(M.lines[i].text, text) != 0) return 1;
    }
    return 0;
}

static int has_side(int kind, const char *label)
{
    int i;
    for (i = 0; i < M.nside; i++) if (M.side[i].kind == kind && strcmp(M.side[i].label, label) == 0) return 1;
    return 0;
}

static void test_door_keys(void)
{
    int rc;
    start_stub();
    rc = drive(KEYS("\r" "hello world\r" "/react 2 +1\r" "/msg @bob hi bob\r" "/?\r" "x" "/q\r"), 0);
    assert(rc == 0);
    assert(st->sessions == 1 && st->rooms_listed >= 1 && st->joins == 1 && st->histories == 1);
    assert(st->messages == 1 && strcmp(st->last_message, "hello world") == 0);
    assert(st->reacts == 1 && strcmp(st->last_react, "+1") == 0);
    assert(st->dms == 1 && strcmp(st->last_dm_peer, "bob") == 0 && strcmp(st->last_dm_text, "hi bob") == 0);
    assert(st->leaves == 1 && st->session_closes == 1);
    assert(st->bad_auth == 0 && st->bad_session == 0);
    assert(strncmp(M.pane_label, "Lobby", 5) == 0 && strcmp(M.room, "Lobby") == 0);    /* auto-joined the first room */
    assert(has_line("alice", "hello there!bar"));                                      /* history: "!" is never shown as "|" */
    assert(has_line("Tester", "hello world"));                                         /* own line arrived via the post-send poll */
    assert(has_line("carol", "from idle poll"));
    assert(help_shown >= 1);
    assert(strcmp(M.lines[0].text, "Found 2 rooms") == 0 && strcmp(M.lines[1].text, "Auto-joining #Lobby...") == 0);
    assert(strcmp(M.lines[2].text, "Joined #Lobby (3 users)") == 0 && M.lines[3].kind == LC_ACTIVITY_LINE);  /* the TS door's order */                                                           /* /? opened the help overlay */
    assert(!strstr(screen_text, "\x1b[40m"));                                                  /* never paints background 40 */
    stop_stub();
    printf("  [OK] door: join, send, react, direct message, help and quit reach the server and the screen\n");
}

static void test_door_lists(void)
{
    int rc;
    start_stub();
    /* Tab to the sidebar, End to its last entry (the DM with alice), Enter opens it, then /who. */
    rc = drive(KEYS("\t" "\x1b[F" "\r" "/who\r" "/q\r"), 0);
    assert(rc == 0 && st->whos == 2 && st->dm_lists == 1);       /* who: once for the sidebar's people at join, once for /who */
    assert(has_side(LC_SIDE_USER, "alice") && has_side(LC_SIDE_USER, "bob") && has_side(LC_SIDE_DM, "alice"));
    assert(has_line("alice", "private hello"));                                        /* Enter opened the DM thread */
    assert(has_line(0, "bob in #lobby (sysop)"));
    stop_stub();

    start_stub();                                       /* a live DM for the open thread lands in it (label = bare thread id) */
    strcpy(st->pending_row, "dm|th1|alice|d9|1700000500|0|live one");
    poll_steps = 2; poll_step_ticks = IDLE_SLEEP_TICKS;
    drive(KEYS("\t" "\x1b[F" "\r" "/q\r"), 1);
    poll_steps = POLL_STEPS_DEFAULT; poll_step_ticks = POLL_STEP_TICKS;
    assert(has_line("alice", "live one") && !has_line(0, "[DM from alice]"));
    stop_stub();
    printf("  [OK] door: who and direct-message lists load; Enter opens a DM thread; its live DMs land in it\n");
}

static void test_door_idle_poll(void)
{
    int rc;
    start_stub();
    poll_steps = 2; poll_step_ticks = IDLE_SLEEP_TICKS;      /* one idle call per step, as these tests were written */
    rc = drive(KEYS("\r" "/q\r"), 1);                       /* a quiet second after joining: only idle polling can fetch carol */
    assert(rc == 0 && st->events >= 1 && st->messages == 0);
    assert(has_line("carol", "from idle poll"));
    poll_steps = POLL_STEPS_DEFAULT; poll_step_ticks = POLL_STEP_TICKS;
    stop_stub();
    printf("  [OK] door: idle polling shows a message nobody sent from here\n");
}

/* Regression (sysop live test 2026-11-08, "keys lag"): door_idle slept 25 ticks (half a second) per
 * call and a key typed meanwhile waited for the sleep to end. Every idle call must sleep at most two
 * ticks, and the poll cadence must stay what it was in wall time: one poll per poll_steps * 25 ticks
 * slept (6 s at the default 12 steps). A poll is an idle_step() that answers 1. */
extern long ae_native_delay_calls, ae_native_delay_total;
extern int ae_native_delay_max;
static void test_door_idle_is_short_and_cadence_kept(void)
{
    int i, polls = 0;
    poll_steps = POLL_STEPS_DEFAULT; poll_step_ticks = POLL_STEP_TICKS;
    idle_ticks = 0;
    ae_native_delay_calls = 0; ae_native_delay_total = 0; ae_native_delay_max = 0;
    for (i = 0; i < 1000; i++) {
        if (idle_step()) polls++;
    }
    assert(ae_native_delay_calls == 1000 && ae_native_delay_max >= 1 && ae_native_delay_max <= 2);
    assert(polls == (int)(ae_native_delay_total / (POLL_STEPS_DEFAULT * 25)));      /* 6 s of sleeping per poll */
    assert(ae_native_delay_total == 2000 && polls == 6);         /* 2000 ticks = 40 s of sleeping = six 6 s polls */
    idle_ticks = 0;
    printf("  [OK] door: idle sleeps at most two ticks and still polls every six seconds\n");
}

/* Regression (sysop live test 2026-11-08): with the board's any-motion mouse mode every hover sends
 * a wake byte, and the main loop reset the idle count on each one, so the event poll never ran while
 * the mouse moved: a reaction (or anyone's message) never showed. Wakes 60 ms apart leave room for at
 * most two 40 ms idle steps between keys; the poll every 4 steps must still run. */
static void test_door_poll_under_mouse_motion(void)
{
    static char keys[64];
    int rc, n = 0, i;
    start_stub();
    poll_steps = 4; poll_step_ticks = IDLE_SLEEP_TICKS;
    keys[n++] = '\r';
    for (i = 0; i < 30; i++) keys[n++] = '\x1e';
    keys[n++] = '/'; keys[n++] = 'q'; keys[n++] = '\r';
    rc = drive(keys, (size_t)n, 60000);
    assert(rc == 0 && st->events >= 1);
    assert(has_line("carol", "from idle poll"));
    poll_steps = POLL_STEPS_DEFAULT; poll_step_ticks = POLL_STEP_TICKS;
    stop_stub();
    printf("  [OK] door: hover wakes do not starve the event poll\n");
}

/* Regression (2026-11-07 mirror run): the rooms view had no way to make a room, so an empty list was a
 * dead end; and /n joined by the typed NAME while the server joins by id, so a new room never opened. */
static void test_door_new_room(void)
{
    int rc;
    start_stub();
    rc = drive(KEYS("/create\r" "My Room\r" "\r" "/q\r"), 0);          /* no name: the prompt asks; Enter picks Text */
    assert(rc == 0 && st->rooms_created == 1 && strcmp(st->last_created, "My Room") == 0);
    assert(strcmp(st->last_created_kind, "text") == 0);
    assert(st->joins == 2 && strcmp(st->last_join, "newroom") == 0);    /* auto-join, then the id the create reply gave */
    assert(has_line(0, "Created #"));
    stop_stub();
    start_stub();
    rc = drive(KEYS("\r" "/n Second Room\r" "\r" "/q\r"), 0);
    assert(rc == 0 && st->rooms_created == 1 && strcmp(st->last_created, "Second Room") == 0);
    assert(st->joins == 2 && strcmp(st->last_join, "newroom") == 0);
    stop_stub();
    printf("  [OK] door: /create with the name prompt and /n with a name make a room and join it by its new id\n");
}

static void test_door_room_kind(void)
{
    int rc;
    start_stub();
    st->rooms_kinds = 1;
    /* /create asks Text, Voice or Art; Down Down Enter picks Art, which the door cannot open yet. */
    rc = drive(KEYS("\r" "/create Sketch\r" "\x1b[B" "\x1b[B" "\r" "/q\r"), 0);
    assert(rc == 0 && st->rooms_created == 1 && strcmp(st->last_created, "Sketch") == 0);
    assert(strcmp(st->last_created_kind, "art") == 0);
    assert(has_line(0, "Creating #Sketch..."));                    /* TS pickRoomKind: Art is refused, so the log stays */
    assert(has_line(0, "Channel type: Text, Voice or Art?"));
    assert(has_line(0, "Art channels are not on Amiga yet."));
    assert(st->joins == 1);                                        /* only the auto-join: Art is not opened */
    assert(strstr(screen_text, "[A] Paint") != 0);                 /* the Art room is marked */
    assert(strstr(screen_text, "Talk") == 0);                      /* the Voice room is not listed for telnet */
    stop_stub();
    start_stub();
    st->rooms_kinds = 1;
    rc = drive(KEYS("\r" "/create Chatter --type voice\r" "/join talk\r" "/q\r"), 0);
    assert(rc == 0 && st->rooms_created == 1 && strcmp(st->last_created, "Chatter") == 0);
    assert(strcmp(st->last_created_kind, "voice") == 0);
    assert(!has_line(0, "Channel type:"));                         /* --type skips the question */
    assert(has_line(0, "Voice channels are not on Amiga yet."));
    assert(st->joins == 1);                                        /* only the auto-join: talk was refused */
    stop_stub();
    start_stub();
    rc = drive(KEYS("\r" "/create Bad --type sound\r" "/q\r"), 0);
    assert(rc == 0 && st->rooms_created == 0);
    assert(has_line(0, "Error: Channel type must be text, voice or art."));
    stop_stub();
    start_stub();                                                  /* TS createCmd: name, topic, flags, motd */
    rc = drive(KEYS("\r" "/create #Den Cozy corner --type text --invite-only --motd \"Be nice\"\r" "/q\r"), 0);
    assert(rc == 0 && st->rooms_created == 1 && strcmp(st->last_created, "Den") == 0);
    assert(strcmp(st->last_created_topic, "Cozy corner") == 0 && strcmp(st->last_created_vis, "private") == 0);
    assert(strcmp(st->last_created_kind, "text") == 0);
    assert(st->room_motd_puts == 1 && strcmp(st->room_motd, "Be nice") == 0);
    stop_stub();
    start_stub();                                                  /* the picker path keeps the topic; Esc cancels */
    drive(KEYS("\r" "/create Nook Quiet --private\r" "\x1b"), 0);
    assert(st->rooms_created == 0);
    assert(has_line(0, "#Nook was not created."));
    stop_stub();
    printf("  [OK] door: /create asks the channel type, --type skips it, Voice and Art rooms are marked and refused\n");
}

/* Reachability: the real door entry, clicks through the board's MOUSE? field. A right click on a
 * message opens the TS chat context menu, a click on React reacts on the server; a click on the
 * Tools label opens its dropdown and a click on Files says it is not available yet. */
static void test_door_mouse(void)
{
    static const int script[] = { MS_RMB_MESSAGE, MS_END, MS_CLICK_REACT, MS_END, MS_CLICK_PICK, MS_END,
                                  MS_CLICK_TOOLS, MS_END, MS_CLICK_FILES, MS_END };
    int rc, before = mouse_events;
    start_stub();
    mouse_script = script; mouse_step = 0; mouse_armed = 0;
    rc = drive(KEYS("\x1e" "\x1e" "\x1e" "\x1e" "\x1e" " " "/q\r"), 0);
    mouse_script = 0;
    assert(rc == 0 && mouse_armed == 1);
    assert(mouse_events - before == 5 && mouse_step == 10);      /* every scripted event reached handle_mouse */
    assert(st->reacts == 1 && strcmp(st->last_react, "+1") == 0);
    /* the react event redraws the row from the server's counts, as the TS chat:reaction handler does */
    assert(st->reaction_lists == 1);
    assert(has_line(0, "[Tester reacted +1]") && has_line(0, "Tester: +1"));
    {
        int i, found = 0;
        for (i = 0; i < M.nlines; i++)
            if (strcmp(M.lines[i].id, st->last_react_msg) == 0) {
                assert(M.lines[i].nreact == 1 && strcmp(M.lines[i].react[0].emoji, "+1") == 0);
                assert(M.lines[i].react[0].count == 1 && M.lines[i].react[0].mine == 1);
                found = 1;
            }
        assert(found);
    }
    assert(strstr(screen_text, ":) 1"));                                           /* the reaction row reached the wire */
    /* the frame diff skips unchanged blanks, so words reach the wire one by one */
    assert(strstr(screen_text, "|> Reply") && strstr(screen_text, "Unread"));      /* the popup reached the wire */
    assert(strstr(screen_text, "(F6)") && strstr(screen_text, "available"));       /* the not-yet notice too */
    stop_stub();
    printf("  [OK] door: right click on a message reacts and its row shows the count, the menu bar opens and says not yet\n");
}

static int line_count(const char *text)
{
    int i, n = 0;
    for (i = 0; i < M.nlines; i++) if (strstr(M.lines[i].text, text) != 0) n++;
    return n;
}

/* B2: /reply (alias r) and /thread (alias t) send a reply line; a loaded parent shows as a quote row. */
static void test_door_reply(void)
{
    int i, bob = -1, live = -1;
    start_stub();
    assert(drive(KEYS("\r" "/reply\r" "/thread\r" "/r 1 yes alice\r"), 0) == 0);
    assert(st->messages == 1 && strcmp(st->last_message, "yes alice") == 0);
    assert(strstr(st->last_write_body, "\"replyTo\":\"m1\"") != 0);            /* the line number named m1 */
    assert(has_line(0, "Usage: /reply [msg-id] message") && has_line(0, "Usage: /thread <msg-id> message"));
    for (i = 0; i < M.nlines; i++) if (strcmp(M.lines[i].id, "m2") == 0) bob = i;
    assert(bob >= 0 && strcmp(M.lines[bob].quote, "alice: hello there!bar") == 0);   /* history replyTo m1 */
    assert(strstr(screen_text, "> alice: hello there!bar") != 0);
    for (i = 0; i < M.nlines; i++) if (strcmp(M.lines[i].id, "m3") == 0) live = i;
    assert(live >= 0 && strcmp(M.lines[live].quote, "alice: hello there!bar") == 0);  /* the live event's replyTo */
    M.ctx_kind = LC_CTX_CHAT; M.ctx_target = bob;
    ctx_run("Reply");
    assert(strcmp(M.input, "/reply 2 ") == 0);
    ctx_run("Quote");
    assert(strcmp(M.input, "> bob: second line ") == 0);
    stop_stub();

    start_stub();                                       /* no id: the newest message in the pane */
    assert(drive(KEYS("\r" "/t 2 on two\r" "/reply to the last\r"), 0) == 0);
    assert(st->messages == 2 && strcmp(st->last_message, "to the last") == 0);
    assert(strstr(st->last_write_body, "\"replyTo\":\"m") != 0);
    assert(st->threads == 0);
    stop_stub();
    printf("  [OK] door: /reply and /thread send replyTo, history and live replies show a quote row, menu Reply and Quote fill the input\n");
}

static int newest_message(void)
{
    int i;
    for (i = M.nlines - 1; i >= 0; i--) if (M.lines[i].number != 0 && M.lines[i].id[0] != '\0') return i;
    return -1;
}

/* Live bug 2026-11-08: React from the keyboard picker's chat menu showed nothing. The server keeps
 * one reaction per user, emoji and message (INSERT OR IGNORE) and sends a react event only when a row
 * was added, so a repeat +1 came back OK with no event: no note, no counts, no word. The door now
 * says "Reaction sent." (TS context-menus.ts) and reads the counts back itself. */
static void test_door_react_without_event(void)
{
    int idx;
    start_stub();
    st->react_known = 1;
    /* Up picks the newest message, Enter opens its menu, Down Down Enter is React, Down Down Enter
     * in the reaction picker is item 2, heart */
    drive(KEYS("\r" "\x1b[A" "\r" "\x1b[B" "\x1b[B" "\r" "\x1b[B" "\x1b[B" "\r"), 0);
    assert(st->reacts == 1 && strcmp(st->last_react, "heart") == 0);
    for (idx = 0; idx < M.nlines && strcmp(M.lines[idx].id, st->last_react_msg) != 0; idx++) {}
    assert(idx < M.nlines && M.lines[idx].number != 0);                     /* a message row on screen */
    assert(has_line(0, "Reaction sent."));
    assert(st->reaction_lists == 1 && M.lines[idx].nreact == 1 && M.lines[idx].react[0].count == 1);
    assert(strcmp(M.lines[idx].react[0].emoji, "heart") == 0 && strstr(screen_text, "<3 1"));
    stop_stub();
    printf("  [OK] door: React from the picker menu with no react event still confirms and shows the count\n");
}

/* TS Up/Down: Up on an empty input starts the message picker (Enter opens the chat menu a click
 * opens, Esc cancels, a typed key cancels and is typed); Up over typed text recalls a sent line,
 * [EDITING], and Enter edits it under the server id the send reply rekeyed it to. */
static void test_door_picker_and_edit(void)
{
    int newest;
    start_stub();
    drive(KEYS("\r" "\x1b[A" "\r"), 0);
    newest = newest_message();
    assert(newest >= 0 && M.ctx.open && M.ctx_kind == LC_CTX_CHAT && M.ctx_target == newest && M.pick[0] == '\0');
    drive(KEYS("\r" "\x1b[A" "\x1b[A"), 0);
    assert(M.pick[0] != '\0' && strcmp(M.pick, M.lines[newest].id) != 0 && !M.ctx.open);
    drive(KEYS("\r" "\x1b[A" "\x1b[A" "a"), 0);
    assert(M.pick[0] == '\0' && strcmp(M.input, "a") == 0);                  /* typing cancels and is typed */
    stop_stub();

    start_stub();
    drive(KEYS("\r" "hello world\r" "x" "\x1b[A" "!\r"), 0);
    assert(st->messages == 1 && st->edits == 1 && M.editing == 0);
    assert(strcmp(st->last_write_path, "/chat/v1/messages/m3") == 0);          /* the clientId rekeyed to m3 */
    assert(strstr(st->last_write_body, "hello world!") != 0);
    drive(KEYS("\r" "hello world\r" "x" "\x1b[A"), 0);
    assert(M.editing == 1 && strcmp(M.input, "hello world") == 0);
    drive(KEYS("\r" "hello world\r" "x" "\x1b[A" "\x1b[B"), 0);
    assert(M.editing == 0 && strcmp(M.input, "x") == 0);                       /* back to the draft: no edit */
    stop_stub();
    printf("  [OK] door: Up on empty picks a message and Enter opens its menu; recalled lines edit under the server id\n");
}

/* B3: the input line is the SDK widget (cursor, Home, Delete, history ring), F3 and the status click switch the
 * sidebar tab, Ctrl+C asks to quit, and the TS aliases and local prefs run. */
static void test_door_b3(void)
{
    static const int script[] = { MS_CLICK_STATUS_USERS, MS_END, MS_CLICK_STATUS_CHANNELS, MS_END };
    int rc;
    start_stub();
    drive(KEYS("\r" "abc" "\x1b[D" "X" "\x1b[H" "\x1b[3~"), 0);              /* abc, Left, X -> abXc; Home, Delete -> XXc? */
    assert(strcmp(M.input, "bXc") == 0 && M.input_cur == 0);
    stop_stub();

    start_stub();                                                             /* K19: Shift+Left selects, typing replaces */
    drive(KEYS("\r" "hello" "\x81" "\x1b[1;2D" "X"), 0);                     /* board byte and xterm spelling */
    assert(strcmp(M.input, "helX") == 0 && M.input_cur == 4 && M.input_anchor == -1);
    drive(KEYS("\r" "hello" "\x83" "\x80"), 0);                               /* Shift+Home, Delete: all gone */
    assert(M.input[0] == '\0' && M.input_anchor == -1);
    drive(KEYS("\r" "hello" "\x81\x81"), 0);
    assert(M.input_anchor == 5 && M.input_cur == 3);                           /* still selected when the keys stop */
    stop_stub();

    start_stub();
    drive(KEYS("\r" "one\r" "two\r" "dra" "\x1b[A" "\x1b[A"), 0);            /* Up twice reaches the first sent line */
    assert(strcmp(M.input, "one") == 0);
    drive(KEYS("\r" "one\r" "two\r" "dra" "\x1b[A" "\x1b[A" "\x1b[B" "\x1b[B"), 0);
    assert(strcmp(M.input, "dra") == 0);                                      /* Down past the newest restores the draft */
    stop_stub();

    start_stub();
    drive(KEYS("\r" "\x1bOR"), 0);                                            /* F3: users tab */
    assert(M.side_tab == 1 && M.nside > 0 && strcmp(M.side[0].label, "ONLINE USERS") == 0);
    drive(KEYS("\r" "\x1bOR" "\x1bOR"), 0);
    assert(M.side_tab == 0 && strcmp(M.side[0].label, "TEXT CHANNELS") == 0);
    stop_stub();

    start_stub();
    mouse_script = script; mouse_step = 0;
    rc = drive(KEYS("\x1e" "\x1e" " "), 0);
    mouse_script = 0;
    (void)rc;
    assert(mouse_step == 4 && M.side_tab == 0);                               /* users click, then channels click */
    stop_stub();

    start_stub();
    drive(KEYS("\r" "\x11" "hi"), 0);
    assert(M.input[0] == '\0');                                              /* the open modal swallows typing */
    drive(KEYS("\r" "\x11" "n" "hi"), 0);
    assert(strcmp(M.input, "hi") == 0);                                       /* Ctrl+Q then No: back to the chat */
    assert(drive(KEYS("\r" "/exit\r"), 0) == 0);
    assert(drive(KEYS("\r" "/bye\r"), 0) == 0);
    drive(KEYS("\r" "/help clear\r" "/commands nosuch\r" "/compact\r" "/timestamps\r"), 0);
    assert(has_line(0, "/clear - Clear chat display") && has_line(0, "Unknown: nosuch"));
    assert(M.compact == 1 && M.timestamps == 0);
    assert(has_line(0, "Compact mode on") && has_line(0, "Timestamps off"));
    drive(KEYS("\r" "hi\r" "/cls\r"), 0);
    assert(line_count("hi") == 0);
    stop_stub();
    printf("  [OK] door: input editing, Shift selection and history, F3 and status click tab, Ctrl+C quit, /exit /bye /cls /help <cmd> /compact /timestamps\n");
}

/* B2: the TS door's /edit /deletemsg /topic /kick /mute /op /invite /reactions and their events. */
static void test_door_moderation(void)
{
    int rc, i, edited = 0;
    start_stub();
    rc = drive(KEYS("\r" "hello world\r" "/edit fixed text\r" "/edit\r" "/deletemsg x\r"
                    "/topic New topic\r" "/kick @alice rude\r" "/mute alice\r" "/op -@bob\r"
                    "/invite @dave #Lobby\r" "/reactions 1\r" "/unreact 1\r" "/edit 99 nope\r"), 0);
    (void)rc;
    stop_stub();
    assert(st->edits == 1);
    for (i = 0; i < M.nlines; i++)
        if (strcmp(M.lines[i].id, "m3") == 0) { assert(strcmp(M.lines[i].text, "fixed text") == 0); edited = M.lines[i].edited; }
    assert(edited);                                                         /* chat:edited replaced the row, marked */
    assert(has_line(0, "Error: Usage: /edit [msg-id] new text"));
    assert(has_line(0, "Error: Usage: /unreact [emoji] <msg-id>"));
    assert(has_line(0, "Error: Usage: /deletemsg [msg-id]"));
    assert(st->topics == 1 && has_line(0, "Tester set the topic: New topic"));
    assert(strstr(M.pane_label, "New topic") != 0);                         /* the header follows the topic */
    assert(st->moderations == 3);
    assert(has_line(0, "bob is no longer a room operator"));                /* /op -@bob went out as deop */
    assert(st->invites == 1 && has_line(0, "Invited dave to the room"));
    assert(strstr(st->last_write_path, "/rooms/lobby/invites") != 0);       /* #Lobby resolved to its id */
    assert(st->reaction_lists >= 1 && has_line(0, "Reactions on 1: ["));
    assert(has_line(0, "No such message number."));
    printf("  [OK] door: edit, topic, kick, mute, deop, invite and reactions reach the server and their events the pane\n");
}

/* Phase A: /me /delete /events /pin /unpin /pinned /search /motd /customemoji and the thread view. */
static void test_door_phase_a(void)
{
    int i, found = 0;
    start_stub();
    drive(KEYS("\r" "/me waves\r" "/me\r" "/pin\r" "/pin m1\r" "/unpin m1\r" "/search ignored\r" "a\r"), 0);
    assert(M.search && has_line(0, "Search query must be at least 2 characters"));   /* TS: the typed query is ignored */
    assert(strcmp(M.search_q, "a") == 0 && !has_line(0, "results for \"a\""));
    assert(strcmp(st->last_message, "ACTION: * Tester waves") == 0);
    assert(has_line(0, "Error: Usage: /me does something"));
    assert(has_line(0, "Error: Usage: /pin <msg-id>"));
    assert(st->pin_calls == 2 && has_line(0, "Pin requested.") && has_line(0, "Unpin requested."));
    drive(KEYS("\r" "\x06" "ab" "\x1b"), 0);              /* Ctrl+F opens, Esc closes */
    assert(!M.search && !has_line(0, "Search query"));
    drive(KEYS("\r" "/motd\r" "/motd Be nice\r" "/motd\r" "/motd --clear\r"
               "/customemoji\r" "/customemoji remove :party:\r" "/customemoji add :Zork: hi o/\r" "/customemoji add smile a b\r"
               "/customemoji add x y\r" "/customemoji bogus\r"
               "/events\r" "/events logins\r" "/events off\r" "/events bogus\r" "/events on\r"
               "/delete\r" "/delete #nosuch\r" "/delete #Secret\r"), 0);
    assert(has_line(0, "No MOTD set. Use /motd <text> to set, /motd --clear to clear."));
    assert(has_line(0, "[MOTD] Be nice") && has_line(0, "[MOTD] cleared"));
    assert(st->room_motd_puts == 2);
    for (i = 0; i < M.nlines; i++)                          /* TS accentAlt: the motd text has its own colour */
        if (strstr(M.lines[i].text, "[MOTD] ") != 0) { assert(M.lines[i].kind == LC_SYSTEM && M.lines[i].tone == LC_TK_ACCENT_ALT); found++; }
    assert(found == 3);                                      /* the event, the cached show, the clear */
    found = 0;
    assert(has_line(0, "  party  web PARTY  telnet \\o/") && !has_line(0, "  pic  web") && !has_line(0, "  EMOJI  web"));
    assert(st->emoji_calls == 2 && strcmp(st->emoji_body, "{\"display\":\"hi\",\"ascii\":\"o/\"}") == 0);
    assert(has_line(0, ":smile: is a built-in emoji.") && has_line(0, "Usage: /customemoji add <name> <display> <ascii>"));
    assert(has_line(0, "Usage: /customemoji add <name> <display> <ascii> | remove <name> | list"));
    assert(has_line(0, "BBS Event Display Settings:") && has_line(0, "  User Logins/Logouts:     ON"));
    assert(has_line(0, "Login/logout events: OFF") && has_line(0, "All BBS events disabled"));
    assert(has_line(0, "Unknown event type: bogus") && has_line(0, "All BBS events enabled"));
    assert(has_line(0, "Error: Usage: /delete #channel") && has_line(0, "Not found."));
    assert(st->room_admin_calls == 1 && strstr(st->last_write_path, "/rooms/secret") != 0);
    /* /mode (TS commands/mode.ts + room:mode + room:error): usage, the sign check, the hub call with
       qualified params, the accentAlt event line, and one error modal per refusal, in turn. */
    drive(KEYS("\r" "/mode\r" "/mode i\r" "/mode +mo @alice\r"), 0);
    assert(has_line(0, "Error: Usage: /mode <+modes> [param]  e.g. /mode +i  or /mode +o alice"));
    assert(has_line(0, "Error: Mode string must start with + or -"));
    assert(st->mode_calls == 1 && strstr(st->last_write_path, "/rooms/lobby/mode") != 0);
    assert(strcmp(st->last_write_body, "{\"modes\":\"+mo\",\"params\":\"alice\"}") == 0);
    for (i = 0; i < M.nlines; i++)
        if (strstr(M.lines[i].text, "[Tester] set mode +mo alice") != 0) { assert(M.lines[i].tone == LC_TK_ACCENT_ALT); found++; }
    assert(found == 1);
    found = 0;
    drive(KEYS("\r" "/mode +mx nobody\r"), 0);
    assert(M.modal == LC_MODAL_MESSAGE && strcmp(M.modal_text, "Unknown mode flag: x") == 0);
    drive(KEYS("\r" "/mode +mx nobody\r" "\r"), 0);           /* closing the first shows the next */
    assert(M.modal == LC_MODAL_MESSAGE && strcmp(M.modal_text, "User nobody@UPROUGH not found") == 0);
    drive(KEYS("\r" "/mode +mx nobody\r" "\r" "\r"), 0);
    assert(M.modal != LC_MODAL_MESSAGE);
    drive(KEYS("\r" "/mode +z\r"), 0);
    assert(M.modal == LC_MODAL_MESSAGE && strcmp(M.modal_text, "Only moderators can change modes") == 0);
    printf("  [OK] door: /mode sends the hub mode call, shows the set-mode line and each refusal in turn\n");
    stop_stub();

    start_stub();
    drive(KEYS("\r" "/pinned\r"), 0);
    assert(M.view && strcmp(M.view_title, "Pinned Messages") == 0 && st->pin_calls == 0);
    for (i = 0; i < M.view_n; i++) if (strstr(M.view_text[i], "alice: hello there") != 0) found = 1;
    assert(found);
    assert(strcmp(M.view_text[0], "=Pinned Messages (1):") == 0 && strncmp(M.view_text[3], "   Sent: ", 9) == 0
           && strncmp(M.view_text[4], "   Pinned by ", 13) == 0);
    drive(KEYS("\x1b"), 0);
    assert(!M.view);
    drive(KEYS("\x87"), 0);                               /* TS server.ts F7: getPinnedMessages */
    assert(M.view && strcmp(M.view_title, "Pinned Messages") == 0);
    drive(KEYS("\x1b"), 0);
    assert(!M.view);
    drive(KEYS("\x86"), 0);                               /* TS F6: file manager, web only */
    assert(has_line(0, "File sharing is not on Amiga yet."));
    drive(KEYS("\r" "\x06" "hello\talice\t\r"), 0);       /* TS search overlay: Ctrl+F, Tab to Username (optional), back, Enter */
    assert(M.search && M.search_field == 0);
    assert(strcmp(M.search_q, "hello") == 0 && strcmp(M.search_user, "alice") == 0);
    assert(strstr(st->last_write_path, "q=hello") != 0 && strstr(st->last_write_path, "user=alice") != 0);
    assert(M.search_n == 1 && strncmp(M.search_rows[0], "1. alice @ ", 11) == 0 && strstr(M.search_rows[0], ": hello there") != 0);
    assert(has_line(0, "Found 1 results for \"hello\""));
    drive(KEYS("/thread 1\r"), 0);
    assert(st->thread_views == 1 && M.view && strcmp(M.view_title, "Thread") == 0);
    assert(strcmp(M.view_text[0], "=Original Message:") == 0 && strstr(M.view_text[1], "alice: hello there") != 0);
    stop_stub();
    printf("  [OK] door: /pinned and F7 show Sent and Pinned by, F6 says web only, Ctrl+F searches with the user filter, /thread <n> opens the overlay\n");
}

/* C1: /emoji <query> searches and /emojis [category] lists, with the TS texts and the ten-row limit. */
static void test_door_emoji_commands(void)
{
    start_stub();
    drive(KEYS("\r" "/emoji happy\r" "/emoji zzzq\r" "/emoji LOVE\r" "/emoji e\r" "/emojis\r" "/emojis Action\r" "/emojis nope\r"), 0);
    assert(has_line(0, "Emojis matching \"happy\":") && has_line(0, "  :-)  :smile: (happy, grin)"));
    assert(has_line(0, "No emojis found for \"zzzq\""));
    assert(has_line(0, "  <3  :heart: (love)") && has_line(0, "  :-*  :kiss: (love, romance)"));
    assert(has_line(0, "Emojis matching \"e\":") && has_line(0, "  ... and ") && has_line(0, " more"));
    assert(has_line(0, "Emoji Categories:") && has_line(0, "Use /emoji to open emoji picker") && has_line(0, "  actions - Kaomoji actions"));
    assert(has_line(0, "Actions Emojis:") && has_line(0, "  (>^_^)<  :hug: (cuddle, embrace)"));
    assert(has_line(0, "Unknown category \"nope\". Use /emojis to see all categories."));
    stop_stub();
    printf("  [OK] door: /emoji <query> and /emojis [category] print the TS lists\n");
}

/* Web-only TS views (voice, mic, draw, files) answer with "not on Amiga yet", never "Unknown command". */
static void test_door_web_only_commands(void)
{
    start_stub();
    drive(KEYS("\r" "/voice\r" "/vc join x\r" "/mic\r" "/deafen\r" "/undeaf\r" "/draw\r" "/art\r" "/files\r" "/share\r"), 0);
    assert(has_line(0, "Voice is not on Amiga yet."));
    assert(has_line(0, "Drawing is not on Amiga yet."));
    assert(has_line(0, "File sharing is not on Amiga yet."));
    assert(!has_line(0, "Unknown command. Type /help for help."));
    stop_stub();
    printf("  [OK] door: voice, mic, draw and files say they are not on Amiga yet\n");
}

/* B2: delete by default takes the caller's last line; a kick of the caller leaves the room; low levels are refused. */
static void test_door_delete_and_kicked(void)
{
    int rc, i;
    start_stub();
    rc = drive(KEYS("\r" "hello world\r" "/deletemsg\r" "/kick Tester\r"), 0);
    (void)rc;
    assert(st->deletes == 1 && st->moderations == 1);                       /* the kick wrote last */
    for (i = 0; i < M.nlines; i++) assert(strcmp(M.lines[i].id, "m3") != 0);      /* chat:deleted took the row out */
    assert(line_count("hello world") == 0);
    assert(has_line(0, "You have been kicked: No reason given") && has_line(0, "Disconnecting..."));
    assert(M.room[0] == '\0');
    stop_stub();

    user_level_text = "10";
    start_stub();
    rc = drive(KEYS("\r" "/kick alice\r" "/ban alice\r" "/op bob\r" "/q\r"), 0);
    user_level_text = "255";
    assert(rc == 0 && st->moderations == 0);
    assert(has_line(0, "Error: Permission denied. Need level 80"));
    assert(has_line(0, "Error: Permission denied. Need level 100"));
    assert(has_line(0, "Error: Permission denied. Need level 255"));
    stop_stub();
    printf("  [OK] door: delete takes the last own line, a kick of the caller leaves the room, low levels are refused\n");
}

static void test_door_no_config(void)
{
    int rc;
    user_level_text = "10";                                  /* a non-sysop keeps the plain message */
    rc = drive(KEYS("x"), 0);
    user_level_text = "255";
    assert(rc == 0 && M.modal == LC_MODAL_MESSAGE && strstr(M.modal_text, "No LIVECHAT.cfg") && strstr(M.modal_text, "access 255"));
    assert(strstr(screen_text, "No LIVECHAT.cfg"));                                    /* and it reached the wire */
    printf("  [OK] door: no config says so and leaves\n");
}

/* A3: cfg save/load keeps every key, including Tls and AllowPlainHttp. */
static void test_config_save(void)
{
    ae_chat_config a, b;
    ae_chat_config_parse(&a, "");
    strcpy(a.host, "hub.example"); strcpy(a.board_key, "id.sec"); a.port = 8080; a.tls = 0;
    a.allow_plain_http = 1; a.timeout_secs = 9;
    assert(ae_chat_config_save(&a, "build/test_chat_save.cfg") == 0);
    assert(ae_chat_config_load(&b, "build/test_chat_save.cfg") == 1);
    assert(strcmp(b.host, "hub.example") == 0 && strcmp(b.board_key, "id.sec") == 0 && b.port == 8080);
    assert(b.tls == 0 && b.allow_plain_http == 1 && b.timeout_secs == 9);
    a.tls = 1; a.allow_plain_http = 0; a.port = 443;
    assert(ae_chat_config_save(&a, "build/test_chat_save.cfg") == 0);
    ae_chat_config_load(&b, "build/test_chat_save.cfg");
    assert(b.tls == 1 && b.allow_plain_http == 0 && b.port == 443);
    assert(ae_chat_config_save(&a, "build/no_such_dir/x.cfg") != 0);
    remove("build/test_chat_save.cfg");
    printf("  [OK] config: save then load round-trips every key\n");
}

/* A3: ae_chat_register parses "OK <id>.<secret>" and surfaces the hub's error line. */
static void test_register(void)
{
    ae_chat_config cfg;
    char key[64], err[160];
    start_stub();
    ae_chat_config_load(&cfg, "build/test_chat.cfg");
    assert(ae_chat_register(&cfg, "My Board", "Sysop", "me@x.y", key, sizeof key, err, sizeof err) == AE_CHAT_OK);
    assert(strcmp(key, "regd.sekrit") == 0 && st->registers == 1 && st->bad_auth == 0);
    assert(strstr(st->last_register_body, "\"name\":\"My Board\"") && strstr(st->last_register_body, "\"contact\":\"me@x.y\""));
    assert(ae_chat_register(&cfg, "bad", "Sysop", 0, key, sizeof key, err, sizeof err) == AE_CHAT_ERR_STATUS);
    assert(strcmp(err, "name must be printable ASCII") == 0 && key[0] == '\0' && st->registers == 2);
    assert(ae_chat_register(&cfg, "", "Sysop", 0, key, sizeof key, err, sizeof err) == AE_CHAT_ERR_ARGS);
    stop_stub();
    printf("  [OK] register: key parsed from OK reply, hub error line returned\n");
}

/* A3: the door's real main(), no cfg. A non-sysop gets the message; a sysop gets the wizard, which registers
 * once, writes the cfg and goes straight into the chat (a session opens with the new key). */
static void test_door_wizard(void)
{
    ae_chat_config cfg;
    char keys[256], port[16];
    int i, n;
    start_stub();
    remove("build/test_chat.cfg");
    user_level_text = "10";
    drive(KEYS("x"), 0);
    assert(st->registers == 0 && strstr(screen_text, "No LIVECHAT.cfg") && !strstr(screen_text, "LiveChat68 setup"));
    user_level_text = "255";
    sprintf(port, "%d", stub_port);
    n = sprintf(keys, "My Board\r\r\r");                              /* name, sysop (default), contact (none) */
    for (i = 0; i < 17; i++) keys[n++] = 127;                          /* erase the default host */
    n += sprintf(keys + n, "127.0.0.1\r");
    for (i = 0; i < 3; i++) keys[n++] = 127;                           /* erase the default port */
    n += sprintf(keys + n, "%s\rn", port);                             /* port, TLS no */
    drive(keys, (size_t)n, 1);
    assert(strstr(screen_text, "LiveChat68 setup"));
    assert(st->registers == 1 && strstr(st->last_register_body, "\"name\":\"My Board\"") && strstr(st->last_register_body, "\"sysop\":\"Tester\""));
    assert(ae_chat_config_load(&cfg, "build/test_chat.cfg") == 1);
    assert(strcmp(cfg.board_key, "regd.sekrit") == 0 && cfg.tls == 0 && cfg.allow_plain_http == 1 && cfg.port == stub_port);
    assert(st->sessions == 1);                                         /* straight into the chat, no re-run */
    stop_stub();
    printf("  [OK] door: sysop with no config runs the wizard, registers once, writes the cfg and enters the chat\n");
}

/* B4 (C13-C17, D12, D15): presence commands and whois reach the server; the pane says what the TS door says. */
static void test_door_b4_presence(void)
{
    start_stub();
    drive(KEYS("\r" "/whois @alice\r" "/whois\r" "/whois @nobody\r" "/away lunch time\r" "/back\r"
               "/status busy deep work\r" "/status bogus\r" "/nick\r"), 0);
    assert(st->profiles == 2);                                              /* alice and nobody; bare /whois stays local */
    assert(has_line(0, "alice - away: lunch") && has_line(0, "Shared rooms: Lobby, Secret"));      /* C13, D15 */
    assert(has_line(0, "Error: Usage: /whois @user") && has_line(0, "User nobody is not online."));
    assert(st->presences == 2 && st->presence_clears == 1);                 /* C14 away, C16 status; C15 back */
    assert(has_line(0, "You are now away: lunch time") && has_line(0, "You are now online"));
    assert(has_line(0, "Status set to busy: deep work"));                   /* busy goes out as busy, not dnd */
    assert(strstr(st->last_write_body, "\"status\":\"busy\"") != 0 && strstr(st->last_write_path, "/presence") != 0);
    assert(has_line(0, "Error: Usage: /status [online|away|busy|dnd] [message]"));
    assert(has_line(0, "Your username: Tester"));                           /* C17 */
    stop_stub();
    printf("  [OK] door: /whois /away /back /status /nick reach the presence and profile routes\n");
}

/* A8: a hub board_joined row (no room) prints one system line, the same text web chat shows. */
/* D3 typing indicators. Send: the input buffer goes to POST /rooms/:id/typing (leading edge at
 * once, the rest throttled) and submit always sends the final "". Receive: a typing row sets the
 * typer's preview, "" drops it, the caller's own hub echo is ignored (the local echo has it). */
static int typing_row(const char *nick, const char *text)
{
    int i;
    for (i = 0; i < M.ntyping; i++)
        if (strcmp(M.typing[i].nick, nick) == 0) return text == 0 || strcmp(M.typing[i].text, text) == 0;
    return 0;
}

static void test_door_typing(void)
{
    size_t n;
    start_stub();
    drive(KEYS("\r" "hi\r" "/q\r"), 1);
    n = strlen(st->typing_log);
    assert(strncmp(st->typing_log, "h|", 2) == 0);                    /* leading edge: the first key goes at once */
    assert(n >= 3 && strcmp(st->typing_log + n - 2, "||") == 0);      /* the last send is the "" of a submit */
    stop_stub();

    start_stub();                                                      /* another user's buffer becomes a preview row */
    strcpy(st->pending_row, "typing|lobby|Alice||1700000500|0|hel");
    poll_steps = 2; poll_step_ticks = IDLE_SLEEP_TICKS;
    drive(KEYS("\r" "/q\r"), 1);
    assert(typing_row("Alice", "hel") && typing_row("Tester", "/quit "));   /* own row: local echo of the input */
    assert(strstr(screen_text, "Alice") != 0 && strstr(screen_text, "> hel") != 0);   /* painted under the log */
    stop_stub();

    start_stub();                                                      /* "" drops the row */
    strcpy(st->pending_row, "typing|lobby|Alice||1700000500|0|");
    drive(KEYS("\r" "/q\r"), 1);
    assert(!typing_row("Alice", 0));
    stop_stub();

    start_stub();                                                      /* the caller's own hub echo is ignored */
    strcpy(st->pending_row, "typing|lobby|Tester||1700000500|0|ghost");
    drive(KEYS("\r" "/q\r"), 1);
    poll_steps = POLL_STEPS_DEFAULT; poll_step_ticks = POLL_STEP_TICKS;
    assert(!typing_row("Tester", "ghost"));
    stop_stub();
    printf("  [OK] door: typing sends throttled buffers and a final \"\", typing events set and clear preview rows\n");
}

static int dm_note(const char *text, int tone)
{
    int i;
    for (i = 0; i < M.nlines; i++)
        if (M.lines[i].kind == LC_NOTE && strcmp(M.lines[i].text, text) == 0 && M.lines[i].tone == tone) return 1;
    return 0;
}

static int activity_has(const char *text)
{
    int i;
    for (i = 0; i < M.nlines; i++) if (M.lines[i].kind == LC_ACTIVITY_LINE && strcmp(M.lines[i].text, text) == 0) return 1;
    return 0;
}

/* Row 6: TS msgCmd + formatDmLine. Leading "@" words are targets; one is chat:dm, 2+ is
 * chat:group-dm (hub POST /dm-group). Sent lines are magenta, received cyan plus an activity line. */
static void test_door_dm_and_group_dm(void)
{
    start_stub();
    drive(KEYS("\r" "/msg bob hi\r" "/pm @bob hi bob\r" "/dm @bob @carol hey all\r" "/q\r"), 1);
    assert(has_line(0, "Usage: /msg @user [@user2 ...] message"));             /* no "@": TS usage error */
    assert(st->dms == 1 && strcmp(st->last_dm_peer, "bob") == 0 && strcmp(st->last_dm_text, "hi bob") == 0);
    assert(dm_note("[DM to bob]: hi bob", LC_DM_SENT));
    assert(st->group_dms == 1);
    assert(strstr(st->last_group_body, "\"participants\":[\"bob\",\"carol\"]") != 0);
    assert(strstr(st->last_group_body, "\"content\":\"hey all\"") != 0 && strstr(st->last_group_body, "\"clientId\":") != 0);
    assert(dm_note("[Group DM to bob, carol]: hey all", LC_DM_SENT));
    stop_stub();

    start_stub();                                                      /* received 1:1 DM */
    strcpy(st->pending_row, "dm|dm0123456789ab|alice|d5|1700000500|0|psst");
    poll_steps = 2; poll_step_ticks = IDLE_SLEEP_TICKS;
    drive(KEYS("\r" "/q\r"), 1);
    assert(dm_note("[DM from alice]: psst", LC_DM_RECEIVED) && activity_has("DM from alice"));
    stop_stub();

    start_stub();                                                      /* received group DM: thread id starts "dg" */
    strcpy(st->pending_row, "dm|dgabc123def456|alice|g5|1700000500|0|hey all");
    drive(KEYS("\r" "/q\r"), 1);
    poll_steps = POLL_STEPS_DEFAULT; poll_step_ticks = POLL_STEP_TICKS;
    assert(dm_note("[Group DM from alice]: hey all", LC_DM_RECEIVED) && activity_has("Group DM from alice"));
    stop_stub();

    start_stub();                                                      /* the hub names users handle@BOARD */
    strcpy(st->session_user, "Tester@AMIX");
    drive(KEYS("\r" "/msg @bob hi\r" "/msg @bob @carol@UPROUGH hey\r" "/q\r"), 1);
    assert(strcmp(st->last_dm_peer, "bob%40AMIX") == 0);           /* path-encoded; the hub decodes */
    assert(strstr(st->last_group_body, "\"participants\":[\"bob@AMIX\",\"carol@UPROUGH\"]") != 0);
    assert(dm_note("[DM to bob]: hi", LC_DM_SENT) && dm_note("[Group DM to bob, carol@UPROUGH]: hey", LC_DM_SENT));
    stop_stub();

    start_stub();                                                      /* the hub echoes the sender's own DM as handle@BOARD */
    strcpy(st->session_user, "Tester@AMIX");
    strcpy(st->pending_row, "dm|dm0123456789ab|Tester@AMIX|d6|1700000500|0|hi");
    poll_steps = 2; poll_step_ticks = IDLE_SLEEP_TICKS;
    drive(KEYS("\r" "/q\r"), 1);
    poll_steps = POLL_STEPS_DEFAULT; poll_step_ticks = POLL_STEP_TICKS;
    assert(st->events >= 1);
    assert(!dm_note("[DM from Tester@AMIX]: hi", LC_DM_RECEIVED) && !activity_has("DM from Tester@AMIX"));
    stop_stub();

    start_stub();                                                      /* a refusal shows the hub's reason */
    strcpy(st->group_dm_reject, "Group DM needs 3+ unique participants (use /msg for 1:1)");
    drive(KEYS("\r" "/msg @bob @bob hey\r"), 0);
    assert(M.modal == LC_MODAL_MESSAGE && strcmp(M.modal_text, "Group DM needs 3+ unique participants (use /msg for 1:1)") == 0);
    stop_stub();
    printf("  [OK] door: /msg @a and /msg @a @b send DM and group DM, DM lines render as the TS door's\n");
}

static void test_door_board_joined(void)
{
    start_stub();
    strcpy(st->pending_row, "board_joined||||1700000500|0|Night Owl BBS");
    poll_steps = 2; poll_step_ticks = IDLE_SLEEP_TICKS;                     /* idle polling fetches the row before /q */
    drive(KEYS("\r" "/q\r"), 1);
    poll_steps = POLL_STEPS_DEFAULT; poll_step_ticks = POLL_STEP_TICKS;
    assert(st->events >= 1 && has_line(0, "Night Owl BBS joined the federation"));
    stop_stub();
    printf("  [OK] door: a board_joined event says the board joined the federation\n");
}

/* H2: a hub "bbs" row (node|user|type|at|data, data form-encoded) renders the TS door's two
 * bbs:event listeners: the legacy "[EVENT]" + system line for logins (under the logins filter),
 * then formatEvent's line. Guest is skipped. */
static int has_event(const char *head, const char *text, int tone)
{
    int i;
    for (i = 0; i < M.nlines; i++)
        if (M.lines[i].kind == LC_EVENT && strcmp(M.lines[i].head, head) == 0
            && strstr(M.lines[i].text, text) != 0 && (tone < 0 || M.lines[i].tone == tone)) return 1;
    return 0;
}

static void bbs_row(const char *row)
{
    start_stub();
    strcpy(st->pending_row, row);
    poll_steps = 2; poll_step_ticks = IDLE_SLEEP_TICKS;
    drive(KEYS("\r" "/q\r"), 1);
    poll_steps = POLL_STEPS_DEFAULT; poll_step_ticks = POLL_STEP_TICKS;
}

static void test_door_bbs_events(void)
{
    bbs_row("bbs|2|alice|user_login|1700000500|0|location=Stockholm%2C+SE");
    assert(has_event("[EVENT] ", "alice has logged in (Node 2)", LC_TK_DIM));
    assert(has_line(0, "alice has logged in (Node 2)"));
    assert(has_event("* alice", " logged in from Stockholm, SE (Node 2)", LC_EV_OK));
    stop_stub();
    bbs_row("bbs|1|bob|user_logout|1700000500|0|duration=125");
    assert(has_event("[EVENT] ", "bob has logged off", LC_TK_DIM));
    assert(has_event("* bob", " logged out after 2m 5s (Node 1)", LC_TK_ACCENT_ALT));
    stop_stub();
    strcpy(prefs_stored, "showLogins=0");                  /* the stored pref turns the logins group off */
    bbs_row("bbs|1|bob|user_login|1700000500|0|");
    assert(!has_event("[EVENT] ", "bob has logged in", -1) && !has_line(0, "bob has logged in"));
    assert(has_event("* bob", " logged in from Unknown (Node 1)", LC_EV_OK));   /* formatEvent is unfiltered */
    stop_stub();
    bbs_row("bbs|1|Guest|user_login|1700000500|0|location=x");
    assert(!has_event("* Guest", "logged in", -1) && !has_line(0, "Guest has logged in"));
    stop_stub();
    bbs_row("bbs|3|carol|upload|1700000500|0|fileName=demo.lha&fileSize=10240&conferenceName=Amiga");
    assert(has_event("* carol", " uploaded demo.lha (10.0KB) to Amiga", LC_EV_BAR));
    stop_stub();
    bbs_row("bbs|3|carol|custom_door_event|1700000500|0|doorName=Tetris&eventType=score&message=scored+9000&isPersonalBest=true");
    assert(has_event("[Tetris]", " carol: scored 9000", LC_EV_YELLOW));
    {
        int i, pb = 0;
        for (i = 0; i < M.nlines; i++) if (strcmp(M.lines[i].head, "[Tetris]") == 0) pb = M.lines[i].pb;
        assert(pb);
    }
    stop_stub();
    bbs_row("bbs|3|carol|door_activity|1700000500|0|action=exited&doorName=Frogger");
    assert(has_event("* carol", " exited door Frogger", LC_EV_YELLOW));
    stop_stub();
    printf("  [OK] door: hub bbs rows render the TS bbs:event lines, Guest skipped, logins filtered\n");
}

/* D12: who.txt statuses mark the user list, and a presence event updates the mark and the text. */
static void test_door_b4_marks(void)
{
    int i, a = -1, b = -1;
    start_stub();
    strcpy(st->presence_user, "alice");
    drive(KEYS("\r" "/status busy gone fishing\r"), 0);
    for (i = 0; i < M.nside; i++) {
        if (M.side[i].kind != LC_SIDE_USER) continue;
        if (strcmp(M.side[i].label, "alice") == 0) a = i;
        if (strcmp(M.side[i].label, "bob") == 0) b = i;
    }
    assert(a >= 0 && b >= 0 && M.side[b].status == 0 && M.side[b].note[0] == '\0');
    assert(M.side[a].status == 'b' && strcmp(M.side[a].note, "gone fishing") == 0);   /* the presence event, not the who.txt away/lunch */
    stop_stub();
    start_stub();
    drive(KEYS("\r"), 0);
    for (i = 0; i < M.nside; i++) if (M.side[i].kind == LC_SIDE_USER && strcmp(M.side[i].label, "alice") == 0) a = i;
    assert(M.side[a].status == 'a' && strcmp(M.side[a].note, "lunch") == 0);           /* who.txt away|lunch */
    stop_stub();
    printf("  [OK] door: sidebar marks follow who.txt and presence events\n");
}

/* C48, C49: ignore lists, adds and lifts reach the server; an ignored user's lines are hidden. */
static void test_door_b4_ignore(void)
{
    start_stub();
    drive(KEYS("\r" "/ignore\r" "/ignore @carol\r" "/ignore carol\r" "/ignore\r" "/unignore\r" "/unignore dave\r" "/unignore carol\r"), 0);
    assert(st->ignore_adds == 1 && st->ignore_removes == 1);
    assert(has_line(0, "You are not ignoring anyone.") && has_line(0, "Ignoring carol - their messages and DMs are hidden."));
    assert(has_line(0, "Already ignoring carol.") && has_line(0, "Ignoring: carol"));
    assert(has_line(0, "Error: Usage: /unignore <user>") && has_line(0, "You are not ignoring dave."));
    assert(has_line(0, "carol is no longer hidden."));
    stop_stub();

    start_stub();
    strcpy(st->ignore_list, "carol");                                       /* the server already holds it: startup loads it */
    poll_steps = 2; poll_step_ticks = IDLE_SLEEP_TICKS;
    drive(KEYS("\r" "/q\r"), 1);
    assert(st->ignore_lists >= 1 && st->events >= 1);                       /* the idle poll ran ... */
    assert(!has_line("carol", "from idle poll"));                           /* carol's polled line is hidden */
    stop_stub();

    start_stub();
    drive(KEYS("\r" "/q\r"), 1);
    assert(st->events >= 1 && has_line("carol", "from idle poll"));         /* ... and shows her when not ignored */
    poll_steps = POLL_STEPS_DEFAULT; poll_step_ticks = POLL_STEP_TICKS;
    stop_stub();
    printf("  [OK] door: /ignore and /unignore reach the server and hide the ignored user's lines\n");
}

/* M11, M13, M17: the user menu runs View Profile, Whois, Mute and Ignore, and the labels flip. */
static void test_door_b4_user_menu(void)
{
    static const int script[] = { MS_CLICK_USER, MS_END, MS_CLICK_ITEM, MS_END,
                                  MS_CLICK_USER, MS_END, MS_CLICK_ITEM, MS_END,
                                  MS_CLICK_USER, MS_END, MS_CLICK_ITEM, MS_END,
                                  MS_CLICK_USER, MS_END, MS_CLICK_ITEM, MS_END,
                                  MS_CLICK_USER, MS_END, MS_CLICK_ITEM, MS_END };
    start_stub();
    ms_items[0] = "View Profile"; ms_items[1] = "Whois"; ms_items[2] = "Mute User"; ms_items[3] = "Unmute User";
    ms_item_n = 0;
    mouse_script = script; mouse_step = 0;
    drive(KEYS("\r" "\x1e\x1e" "\r" "\x1e\x1e\x1e\x1e\x1e\x1e" " "), 0);
    mouse_script = 0;
    assert(st->profiles == 2);                                              /* M11 and M13 */
    assert(has_line(0, "alice - away: lunch"));                             /* M13 */
    assert(has_line(0, "Muted alice - their room messages are hidden.") && has_line(0, "alice is no longer hidden."));   /* M17 mute, unmute */
    stop_stub();

    start_stub();
    ms_items[0] = "Ignore"; ms_items[1] = "Unignore"; ms_item_n = 0;
    mouse_script = script; mouse_step = 0;
    drive(KEYS("\r" "\x1e\x1e\x1e\x1e" " "), 0);
    mouse_script = 0;
    assert(st->ignore_adds == 1 && st->ignore_removes == 1);                /* M17 ignore, unignore */
    assert(has_line(0, "Ignoring alice - their messages and DMs are hidden.") && has_line(0, "alice is no longer hidden."));
    stop_stub();
    printf("  [OK] door: user menu View Profile, Whois, Mute and Ignore act, labels flip\n");
}

/* C5 (TS context-menus.ts): Copy Text puts the line on the clipboard (OSC 52), Mark Unread shows (1)
 * on the room, Pin Channel lists the room first, Expand/Collapse shuts the active room's user rows,
 * Archive prints the sysop request line, Block tells the server and Unblock lifts it. */
static int side_index(const char *label)
{
    int i;
    for (i = 0; i < M.nside; i++) if (M.side[i].kind == LC_SIDE_ROOM && strcmp(M.side[i].label, label) == 0) return i;
    return -1;
}

static int side_has_user(const char *label)
{
    int i;
    for (i = 0; i < M.nside; i++) if (M.side[i].kind == LC_SIDE_USER && strcmp(M.side[i].label, label) == 0) return 1;
    return 0;
}

static void test_door_c5_context_items(void)
{
    static const int script[] = { MS_RMB_MESSAGE, MS_END, MS_CLICK_ITEM, MS_END,
                                  MS_RMB_MESSAGE, MS_END, MS_CLICK_ITEM, MS_END,
                                  MS_CLICK_ROOM, MS_END, MS_CLICK_ITEM, MS_END,
                                  MS_CLICK_ROOM, MS_END, MS_CLICK_ITEM, MS_END,
                                  MS_CLICK_USER, MS_END, MS_CLICK_ITEM, MS_END,
                                  MS_CLICK_USER, MS_END, MS_CLICK_ITEM, MS_END };
    int lobby;
    start_stub();
    st->rooms_kinds = 1;
    ms_items[0] = "Copy Text"; ms_items[1] = "Mark Unread"; ms_items[2] = "Pin Channel"; ms_items[3] = "Archive";
    ms_items[4] = "Block"; ms_items[5] = "Unblock"; ms_item_n = 0;
    ms_rooms[0] = "Paint"; ms_rooms[1] = "Paint"; ms_room_n = 0;
    mouse_script = script; mouse_step = 0;
    drive(KEYS("\r" "\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1e" " "), 0);
    mouse_script = 0;
    assert(ms_item_n == 6 && ms_room_n == 2);
    assert(has_line(0, "Copied to clipboard.") && strstr(screen_text, "\033]52;c;") != 0);
    assert(has_line(0, "Marked Lobby as unread"));
    lobby = side_index("Lobby");
    assert(lobby >= 0 && M.side[lobby].unread == 1);
    assert(has_line(0, "Pinned Paint") && side_index("Paint") < lobby);      /* the pinned room comes first */
    assert(strstr(prefs_sets, "pinnedChannels\tPaint\n"));                  /* TS prefsSender saves the pin */
    assert(has_line(0, "Archive requested for Paint"));
    assert(M.lines[M.nlines - 1].tone != LC_TK_ALERT);                        /* the last line is Unblock's */
    assert(st->ignore_adds == 1 && st->ignore_removes == 1);
    assert(has_line(0, "Blocked alice for you") && has_line(0, "alice is no longer hidden."));
    stop_stub();

    /* Expand/Collapse on the active room shuts it: arrow > and no user rows; again opens it. */
    start_stub();
    {
        static const int s2[] = { MS_CLICK_ROOM, MS_END, MS_CLICK_ITEM, MS_END };
        ms_items[0] = "Expand/Collapse"; ms_item_n = 0;
        ms_rooms[0] = "Lobby"; ms_room_n = 0;
        mouse_script = s2; mouse_step = 0;
        drive(KEYS("\r" "\x1e\x1e" " "), 0);
        mouse_script = 0;
        lobby = side_index("Lobby");
        assert(lobby >= 0 && M.side[lobby].open == 0 && !side_has_user("alice"));
    }
    stop_stub();
    printf("  [OK] door: Copy Text, Mark Unread, Pin, Expand/Collapse, Archive and Block from the context menus\n");
}

/* R1a (sysop 2026-11-08): dragging the chat scrollbar thumb scrolls the chat. A press on the track
 * jumps there, held motion follows, release ends it; a bare hover afterwards does not scroll. */
/* C6: the user menu's Add Note prompts "Note about X:", trims the text, and sends it to the board's
 * AEW_USERNOTE; OK, NO and no reply give the TS context-menus.ts lines. */
static void test_door_c6_add_note(void)
{
    static const int script[] = { MS_CLICK_USER, MS_END, MS_CLICK_ITEM, MS_END };
    static const char *const replies[] = { "OK", "NO", "" };
    static const char *const lines[] = { "Note saved for alice.", "Could not save note for alice.", "Notes are unavailable." };
    int i;
    for (i = 0; i < 3; i++) {
        start_stub();
        note_request[0] = '\0'; note_reply = replies[i];
        ms_items[0] = "Add Note"; ms_item_n = 0;
        mouse_script = script; mouse_step = 0;
        drive(KEYS("\r" "\x1e\x1e" "  nice person  \r"), 0);
        mouse_script = 0;
        assert(ms_item_n == 1);
        assert(strcmp(note_request, "NOTE\talice\tnice person") == 0);
        assert(has_line(0, lines[i]));
        stop_stub();
    }
    note_reply = "OK";
    printf("  [OK] door: Add Note sends NOTE alice and says saved, refused or unavailable\n");
}

/* D1 (TS settings overlay + chat:prefs): Ctrl+S and /settings open the overlay, a toggled box is saved
 * on close, a status radio sets presence, stored prefs and pinned names load at start, /sounds and
 * /compact persist. */
static void test_door_d1_settings_prefs(void)
{
    int paint;
    start_stub();
    drive(KEYS("\r" "/settings\r"), 0);
    assert(M.settings == 1 && strstr(screen_text, "Settings") && strstr(screen_text, "Show User Logins/Logouts"));
    stop_stub();

    start_stub();
    drive(KEYS("\r" "\x13" " " "\x1b"), 0);                 /* Ctrl+S, Space on Show User Logins, Esc */
    assert(M.settings == 0);
    assert(strstr(prefs_sets, "showLogins\t0\n") && strstr(prefs_sets, "showTimestamps\t1\n"));
    stop_stub();

    start_stub();                                           /* Down to Away (row 9), Space sets presence */
    drive(KEYS("\r" "\x13" "\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B" " "), 0);
    assert(M.settings == 1 && st->presences == 1 && M.settings_status == 1);
    stop_stub();

    start_stub();
    st->rooms_kinds = 1;
    strcpy(prefs_stored, "showTimestamps=0;showLogins=0");
    strcpy(prefs_pinned, "Paint");
    drive(KEYS("\r" "/settings\r"), 0);
    assert(M.timestamps == 0 && M.settings_val[LC_SET_LOGINS] == 0 && M.settings_val[LC_SET_STAMPS] == 0);
    paint = side_index("Paint");
    assert(paint >= 0 && paint < side_index("Lobby"));      /* the stored pin lists Paint first */
    stop_stub();

    start_stub();
    drive(KEYS("\r" "/sounds off\r" "/sounds loud\r" "/compact\r"), 0);
    assert(has_line(0, "Sounds disabled") && has_line(0, "Error: Usage: /sounds on|off"));
    assert(strstr(prefs_sets, "notificationSound\t0\n") && strstr(prefs_sets, "mentionSound\t0\n"));
    assert(strstr(prefs_sets, "compactMode\t1\n"));
    stop_stub();
    printf("  [OK] door: settings overlay saves boxes and sets status, prefs and pins load, /sounds and /compact persist\n");
}

static void test_door_scrollbar_drag(void)
{
    static const int script[] = { MS_FILL, MS_BAR_PRESS, MS_END, MS_BAR_DRAG, MS_END,     /* FILL and CHECK end their own wake */
                                  MS_BAR_RELEASE, MS_END, MS_BAR_HOVER, MS_END, MS_CHECK };
    start_stub();
    mouse_script = script; mouse_step = 0;
    drive(KEYS("\x1e\x1e\x1e\x1e\x1e\x1e" " "), 0);
    mouse_script = 0;
    assert(mouse_step == 10);
    stop_stub();
    printf("  [OK] door: scrollbar press and drag scroll the chat, release ends the drag\n");
}

static void test_door_slash_menu(void)
{
    int rc;
    start_stub();
    rc = drive(KEYS("\r" "/jo"), 0);                       /* typed, then stdin ends: the last frame holds the menu */
    assert(rc == 0);
    assert(strstr(screen_text, "Commands"));
    assert(strstr(screen_text, "/join"));
    assert(strstr(screen_text, "#channel"));               /* the usage column */
    stop_stub();
    start_stub();
    rc = drive(KEYS("\r" "/pin"), 0);                      /* a TS command the door does not run yet is in the menu */
    assert(rc == 0 && strstr(screen_text, "Pin a message") && strstr(screen_text, "Show pinned"));
    stop_stub();
    start_stub();
    rc = drive(KEYS("\r" "/jo" "\t" "x" "\x1b"), 0);
    assert(rc == 0);
    assert(strcmp(M.input, "/join x") == 0);                /* Tab filled "/join ", typing went on in the input */
    stop_stub();
    start_stub();
    rc = drive(KEYS("\r" "/jo" "\t"), 0);                  /* TS: a fill hides the list */
    assert(rc == 0 && M.slash_off == 1 && strcmp(M.input, "/join ") == 0);
    stop_stub();
    start_stub();
    rc = drive(KEYS("\r" "/jo" "\x1b[B" "\r" "\x1b"), 0);   /* TS enterOnCommandSuggestions: the top prefix match outranks the moved selection */
    assert(rc == 0 && strcmp(M.input, "/join ") == 0);
    stop_stub();
    start_stub();
    rc = drive(KEYS("\r" "/annel" "\t"), 0);              /* no prefix match: Tab hides the list, text stays */
    assert(rc == 0 && M.slash_off == 1 && strcmp(M.input, "/annel") == 0);
    stop_stub();
    start_stub();
    rc = drive(KEYS("\r" "/jo" "\x1b"), 0);                 /* Esc hides the menu and keeps the text */
    assert(rc == 0 && M.slash_off == 1 && strcmp(M.input, "/jo") == 0);
    stop_stub();
    start_stub();
    rc = drive(KEYS("\r" "/he" "\r" "\x1b"), 0);          /* Enter on a half-typed name fills */
    assert(rc == 0 && strcmp(M.input, "/help ") == 0 && help_shown == 0);
    stop_stub();
    start_stub();
    help_shown = 0;
    rc = drive(KEYS("\r" "/help" "\r" "x" "/q\r"), 0);   /* Enter on the full name runs it: the help overlay opens */
    assert(rc == 0 && help_shown >= 1);
    stop_stub();
    printf("  [OK] door: the / menu lists matches, Tab and Enter fill, a full name runs, Esc hides\n");
}

/* Phase B: a message typed into the door comes back as an event and is drawn with its markdown, colour tag and
 * mention. Reads the ANSI the door wrote: italic is SGR 3, the red tag foreground 1, the own-name mention reversed
 * (SGR 7) yellow. The unit cases live in test_livechat_ui.c. */
static void test_door_message_formatting(void)
{
    int rc;
    start_stub();
    rc = drive(KEYS("\r" "say *slant* {red-fg}hot{/} @Tester\r"), 0);
    assert(rc == 0 && has_line("Tester", "*slant*"));              /* the event row carries the raw text */
    assert(strstr(screen_text, "\033[0;3;49;38;5;195mslant") != 0);            /* *x*: italic in the text colour */
    assert(strstr(screen_text, "\033[0;49;31mhot") != 0);                          /* {red-fg}: ANSI red, the tag eaten */
    assert(strstr(screen_text, "\033[0;7;49;33m@Tester") != 0);   /* own name: reversed yellow */
    stop_stub();
}

/* View > Theme (TS theme-menu.ts): the highlight re-themes the whole door live, q or Esc puts the opened theme back,
 * Enter keeps it and saves it through ae_set_user_theme. The board answers "Tester" for the saved theme, which is no
 * id, so the door opens on the default uprough-neon (ink 195); neon-muted, the next row, has ink 146. */
static const char *last_of(const char *hay, const char *needle)
{
    const char *p = hay, *last = 0;
    while ((p = strstr(p, needle)) != 0) { last = p; p++; }
    return last;
}

static void test_view_theme(void)
{
    const char *last146, *last195;
    int rc;
    start_stub();
    rc = drive(KEYS("\t\t" "\x1b[C\x1b[C" "\x1b[B" "\r" "\x1b[B" "q" "/q\r"), 0);   /* menu bar, View, Theme; down previews; q reverts */
    assert(rc == 0);
    last146 = last_of(screen_text, "38;5;146m");
    last195 = last_of(screen_text, "38;5;195m");
    assert(last146 != 0);                                             /* the preview drew neon-muted ink */
    assert(last195 != 0 && last195 > last146);                        /* after q the door is neon again */
    stop_stub();
    assert(theme_written[0] == '\0');                                 /* a revert saves nothing */
    start_stub();
    rc = drive(KEYS("\t\t" "\x1b[C\x1b[C" "\x1b[B" "\r" "\x1b[B" "\r" "/q\r"), 0);   /* Enter keeps the preview */
    assert(rc == 0);
    assert(strcmp(theme_written, "neon-muted") == 0);                 /* ae_set_user_theme reached the board with the id */
    stop_stub();
}

/* Disconnection modal (TS ui/disconnection-modal.ts, server.ts:2649): a failed poll opens it; Retry polls once. */
static void test_door_disconnection(void)
{
    int rc;
    poll_steps = 2; poll_step_ticks = IDLE_SLEEP_TICKS;
    start_stub();
    st->events_ok_first = 1; st->events_fail = 1;
    rc = drive(KEYS("\r"), 1);                                /* join, then the idle poll fails */
    assert(rc == 0 && M.modal == LC_MODAL_RECONNECT && M.modal_attempt == 1 && M.modal_max == 3);
    assert(strcmp(M.modal_text, "Cannot reach the chat server.") == 0);
    assert(strstr(screen_text, "Lost connection to server") != 0);
    stop_stub();
    start_stub();
    st->events_ok_first = 1; st->events_fail = 1;
    rc = drive(KEYS("\r" "\r" "x" "/q\r"), 1);                 /* Enter = Retry: the stub is back */
    assert(rc == 0 && M.modal == LC_MODAL_NONE && st->events_fail == 0);
    assert(has_line(0, "Attempting to reconnect...") && has_line(0, "Reconnected to server!"));
    stop_stub();
    start_stub();
    st->events_ok_first = 1; st->events_fail = 100;
    rc = drive(KEYS("\r" "\r" "\r" "\r"), 1);                  /* three Retry presses all fail: past the maximum */
    assert(rc == 0 && st->events_fail == 96);                /* the idle poll and three Retry polls, no more */
    assert(has_line(0, "Maximum reconnection attempts reached. Please restart LiveChat."));
    stop_stub();
    poll_steps = POLL_STEPS_DEFAULT; poll_step_ticks = POLL_STEP_TICKS;
    printf("  [OK] door: a failed poll opens the disconnection modal; Retry reconnects or ends past the maximum\n");
}

/* TS pinned-panel.ts:52 and thread-view.ts:54: the view closes on Esc or q only. */
static void test_view_closes_on_esc_or_q(void)
{
    start_stub();
    drive(KEYS("\r" "/pinned\r" "x"), 0);
    assert(M.view);                                       /* a plain letter is ignored */
    drive(KEYS("\r" "/pinned\r" "q"), 0);
    assert(!M.view);
    stop_stub();
    printf("  [OK] door: the view overlay ignores a plain letter and closes on q\n");
}

/* C2/A12: the TS format picker (ui/format-picker.ts). Shift+Left selects in the input, F5 (or a selection held
 * still for the TS 500 ms = FMT_WAIT_TICKS) opens the picker; Down Down Enter Enter picks Markdown > Bold and the
 * selection is wrapped in place. F5 with no selection says how; a left press outside closes it. */
static void test_door_format_picker(void)
{
    int rc;
    start_stub();
    rc = drive(KEYS("\r" "say hi" "\x81\x81" "\x85" "\x1b[B" "\x1b[B" "\r" "\r"), 0);
    assert(rc == 0 && M.fmt == 0);
    assert(strcmp(M.input, "say **hi**") == 0);                    /* Markdown > Bold wrapped the selection */
    stop_stub();
    start_stub();
    rc = drive(KEYS("\r" "hi" "\x85"), 0);
    assert(rc == 0 && M.fmt == 0 && has_line(0, "Select text first"));
    stop_stub();
    start_stub();
    rc = drive(KEYS("\r" "hi" "\x81"), 800000);              /* no F5: the selection held still opens it from door_idle */
    assert(rc == 0 && M.fmt == 1 && M.fmt_a == 1 && M.fmt_b == 2);
    assert(strstr(screen_text, "Markdown") && strstr(screen_text, "Colors"));   /* the picker reached the wire */
    fmt_click(LC_HIT_CHAT, 0, AE_MOUSE_LEFT);                      /* a left press outside closes it, as TS blur */
    assert(M.fmt == 0 && strcmp(M.input, "hi") == 0);
    stop_stub();
    printf("  [OK] door: a selection opens the format picker (F5 or held still) and Markdown > Bold wraps it\n");
}

/* C1: the TS emoji picker (ui/emoji-picker.ts). F4 opens it; Tab moves to the items, Enter picks the first
 * Emotions emoji and the input gets ":smile: " appended, the cursor at the end, the picker closed. */
static void test_door_emoji_picker(void)
{
    int rc;
    start_stub();
    rc = drive(KEYS("\r" "hi " "\x1bOS" "\t" "\r"), 0);
    assert(rc == 0 && M.fmt == 0);
    assert(strcmp(M.input, "hi :smile: ") == 0 && M.input_cur == M.input_len);
    stop_stub();
    start_stub();
    rc = drive(KEYS("\r" "hi " "\x1bOS"), 0);                   /* left open: it reached the wire */
    assert(rc == 0 && M.fmt == 1 && M.fmt_kind == LC_PICK_EMOJI);
    assert(strstr(screen_text, "Emoji Picker") && strstr(screen_text, "Emotions"));
    fmt_click(LC_HIT_CHAT, 0, AE_MOUSE_RIGHT);                    /* a right press outside does not cancel */
    assert(M.fmt == 1);
    fmt_click(LC_HIT_CHAT, 0, AE_MOUSE_LEFT);                     /* a left press outside does */
    assert(M.fmt == 0 && strcmp(M.input, "hi ") == 0);
    stop_stub();
    printf("  [OK] door: F4 opens the emoji picker and Tab, Enter appends :smile: to the input\n");
}

int main(void)
{
    ae_native_set_exchange(board, 0);
    test_config();
    test_client();
    test_door_keys();
    test_door_lists();
    test_door_slash_menu();
    test_door_idle_poll();
    test_door_idle_is_short_and_cadence_kept();
    test_door_poll_under_mouse_motion();
    test_door_new_room();
    test_door_room_kind();
    test_door_mouse();
    test_door_scrollbar_drag();
    test_door_reply();
    test_door_moderation();
    test_door_phase_a();
    test_door_b3();
    test_door_picker_and_edit();
    test_door_react_without_event();
    test_door_delete_and_kicked();
    test_door_b4_presence();
    test_door_board_joined();
    test_door_dm_and_group_dm();
    test_door_typing();
    test_door_bbs_events();
    test_door_b4_marks();
    test_door_b4_ignore();
    test_door_b4_user_menu();
    test_door_c5_context_items();
    test_door_c6_add_note();
    test_door_d1_settings_prefs();
    test_door_message_formatting();
    test_view_theme();
    test_door_format_picker();
    test_door_emoji_picker();
    test_door_disconnection();
    test_view_closes_on_esc_or_q();
    test_door_emoji_commands();
    test_door_web_only_commands();
    test_door_no_config();
    test_config_save();
    test_register();
    test_door_wizard();
    remove("build/test_chat_keys.bin"); remove("build/test_chat_screen.bin");
    puts("test_chat: OK");
    return 0;
}
