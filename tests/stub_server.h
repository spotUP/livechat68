/* stub_server.h - minimal one-shot HTTP stub server for testing ae_http.c
 * against controlled, scripted responses. POSIX-only (fork + sockets);
 * used only by the native test suite, never linked into the door itself.
 */

#ifndef AE_TEST_STUB_SERVER_H
#define AE_TEST_STUB_SERVER_H

/* Forks a child process that listens on an ephemeral loopback port,
 * accepts exactly one connection, writes `response` (exactly
 * `response_len` bytes, verbatim - whatever the client sends is not
 * inspected), then closes the connection and exits. Returns the port
 * number to connect to on success (and stores the child pid in
 * *out_pid, for stub_server_reap()), or -1 on failure. */
int stub_server_start(const unsigned char *response, unsigned long response_len, int *out_pid);

/* Waits for the child process started by stub_server_start() to exit, so
 * the test suite does not accumulate zombies. Safe to call with any pid
 * returned by stub_server_start(). */
void stub_server_reap(int pid);

/* Returns a loopback TCP port number that nothing is listening on (a
 * listening socket is opened just long enough to claim an ephemeral port,
 * then closed before returning), for testing the connect-refused error
 * path. Returns -1 on failure. */
int stub_closed_port(void);

/* Like stub_server_start(), but also captures every byte the client sends
 * (request line, headers, and any request body) instead of discarding
 * them. The forked child drains the client's request with a short receive
 * timeout (so it stops once no more bytes arrive, without needing to know
 * the request length up front or waiting for the client to close its
 * write side - the client never does, since it is still waiting to read
 * the response), THEN writes the captured bytes across a pipe to the
 * parent, THEN sends `response`, exactly like stub_server_start().
 *
 * Returns the port to connect to on success (storing the child pid in
 * *out_pid and a pipe read-fd in *out_capture_fd, for
 * stub_server_read_capture()), or -1 on failure. */
int stub_server_start_capturing(const unsigned char *response, unsigned long response_len,
                                 int *out_pid, int *out_capture_fd);

/* Reads everything the paired stub_server_start_capturing() child
 * captured from the pipe at capture_fd into `out` (bounded to outsize
 * bytes - any excess captured bytes are still drained from the pipe so
 * the child never blocks on a full pipe buffer, but are not copied into
 * `out`), then closes capture_fd. Call this after the HTTP exchange
 * (e.g. after ae_http_get()/ae_ae_http_request() returns), before
 * stub_server_reap(). Returns the number of bytes copied into `out`. */
unsigned long stub_server_read_capture(int capture_fd, unsigned char *out, unsigned long outsize);

/* Like stub_server_start(), but serves `count` responses in sequence across
 * `count` separate connections, all on the SAME port - for testing code
 * that makes more than one HTTP round trip to the same host:port within
 * one logical operation (e.g. owner_auth_call()'s re-login-and-retry flow:
 * an admin call that gets 401, a re-login, then the retried call - three
 * separate connections, since ae_http_request() always sends
 * Connection: close). `responses[i]`/`response_lens[i]` is served to the
 * i-th connection accepted, in order; each connection's inbound bytes are
 * drained and discarded first, exactly like stub_server_start(). If the
 * code under test makes FEWER connections than `count` (e.g. a retry that
 * should have fired but didn't), the child gives up waiting for the next
 * one after a few seconds and exits instead of blocking forever - a
 * connection-count shortfall then surfaces as a clean connect failure on
 * whichever attempt never arrives, not a hung test suite. Returns the
 * port to connect to on success (storing the child pid in *out_pid, for
 * stub_server_reap()), or -1 on failure. */
int stub_server_start_sequence(const unsigned char * const *responses,
                                const unsigned long *response_lens,
                                int count, int *out_pid);


/* ---- /chat/v1 -------------------------------------------------------
 * A routed, long-lived stub of the chat surface: one forked child that
 * accepts connections until stub_chat_stop(), answers by method and path,
 * and counts what the client did in a block of shared memory the test reads.
 * Canned world: rooms "lobby" (public) and "secret" (private); lobby history
 * is two messages by alice and bob; the FIRST events poll delivers a message
 * from carol; a posted message is echoed to the next events poll, the way
 * the real server's event log would; so is a reaction (a react row, then
 * reactions.txt answers +1 x1 mine). */
typedef struct {
    volatile int sessions, session_closes, rooms_listed, rooms_created, joins, leaves;
    volatile int histories, messages, reacts, threads, dms, dm_lists, whos, events;
    volatile int unreacts, reaction_lists;     /* DELETE /messages/:id/react/:emoji, GET reactions.txt */
    volatile int edits, deletes, topics, moderations, invites, uninvites;
    volatile int presences, presence_clears, profiles, ignore_lists, ignore_adds, ignore_removes;  /* B4 routes */
    volatile int registers;                /* POST /achievements/v1/boards/register (A3) */
    char last_register_body[300];          /* its JSON body; a name of "bad" is answered 400 */
    char presence_user[32];                /* test sets: whose presence the next presence write reports (default Tester) */
    char alice_status[16], alice_text[80];  /* the presence alice set (who.txt then reports it) */
    char ignore_list[128];                 /* comma separated nicks the stub holds as ignored (a test may preset it) */
    char pending_row[300];                 /* an event row (no seq) a write queued; the next events poll carries it as seq 9 */
    volatile int events_ok_first, events_fail;  /* test sets: serve this many events polls, then answer 500 to the next events_fail */
    volatile int bad_auth;                 /* requests that lacked "Authorization: Board " */
    volatile int bad_session;              /* post-open requests lacking X-Chat-Session */
    volatile int react_known;              /* test sets: the caller already has this reaction, so a react
                                              emits no event (the server's INSERT OR IGNORE) */
    volatile int expire_next;              /* test sets: next authenticated call answers session_expired once */
    char last_message[512];                /* content of the latest POST /messages */
    char last_reply_to[32];                /* its replyTo; the message event row carries it back */
    char last_react[64];
    char last_react_msg[24];               /* message id of the latest react or unreact */
    volatile int pin_calls, emoji_calls, room_admin_calls, thread_views, motd_set;   /* Phase A */
    char emoji_body[120];                  /* body of the latest PUT /emoji/:name */
    char room_motd[128];                   /* the room's motd: PUT /rooms/:id/motd sets it, the join reply carries it */
    volatile int room_motd_puts;
    volatile int mode_calls;               /* POST /rooms/:id/mode */
    volatile int typing_calls;             /* POST /rooms/:id/typing (D3) */
    char typing_log[600];                  /* every typing text sent, each followed by '|' */
    char last_write_path[128];             /* path of the latest edit, delete, topic, moderation or invite call */
    char last_write_body[300];             /* its JSON body ("" for DELETE) */
    char last_dm_peer[64];
    char dm_unknown[96];      /* non-empty: DM and group DM answer 404 not_found "unknown user: <this>" */
    char last_dm_text[512];
    int group_dms;                         /* POST /chat/v1/dm-group */
    char session_user[48];                 /* test sets: the session's user name ("" = Tester) */
    char group_dm_reject[160];             /* test sets: POST /dm-group answers 400 bad_request with this detail */
    char last_group_body[1024];            /* its raw JSON body */
    char last_created[64];
    char last_created_kind[16];            /* "kind" of the latest POST /rooms, "" when it sent none */
    char last_created_topic[100];          /* its "topic" */
    char last_created_vis[16];             /* its "visibility" */
    int rooms_kinds;                       /* set: rooms.txt also lists an art and a voice room, kind last */
    char last_join[64];                    /* room id from the latest POST /rooms/<id>/join path */
} stub_chat_stats;

/* Starts the chat stub. Returns its port, or -1; *stats points at shared counters. */
int stub_chat_start(stub_chat_stats **stats, int *out_pid);
void stub_chat_stop(int pid);

#endif /* AE_TEST_STUB_SERVER_H */
