/*
 * ae_chat - the livechat client a C door talks to the door server through.
 * A sibling of ae_request: the same wire (`Authorization: Board <key>`), the
 * same ae_http, the same bounded allocation-free style. The contract is the
 * /chat/v1 surface in CHAT-API.md; this file mirrors its field order.
 *
 * Differences from ae_request:
 *  - A session. open() gets a sid, every later call sends it as
 *    X-Chat-Session, close() ends it. A session the server expired
 *    (session_expired) is reopened once, silently, and the call retried.
 *  - The reply buffer is 8192 bytes and owned here (a 20-row history of 500
 *    byte lines does not fit ae_request's 2048).
 *  - Receive is plain text rows: `|` separated, a literal `|` travels as `!`
 *    and is turned back here. JSON is read only for the session reply.
 *
 * Reading rows: after a list call, loop ae_chat_next_row(c, fields, max)
 * until it returns 0. The fields are pointers into the reply buffer, valid
 * until the next call on the same ae_chat. A header row (ROOMS|, EVENTS|) is
 * consumed by the call itself; ae_chat_poll stores the next `since` in
 * c->last_seq.
 *
 * Platform-neutral C89: sockets through ae_net, framing through ae_http.
 */
#ifndef AE_CHAT_H
#define AE_CHAT_H

#include "ae_http.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AE_CHAT_OK            0
#define AE_CHAT_ERR_ARGS      -1  /* NULL argument or an empty required field */
#define AE_CHAT_ERR_TOO_LONG  -2  /* the body or path did not fit its fixed buffer */
#define AE_CHAT_ERR_TRANSPORT -3  /* the request never completed; see c->http_error */
#define AE_CHAT_ERR_STATUS    -4  /* the server answered with an error; see c->status and c->error */
#define AE_CHAT_ERR_BODY      -5  /* a 2xx whose body is not the expected shape */

#define AE_CHAT_BASE "/chat/v1"
#define AE_CHAT_REPLY_MAX 8192
#define AE_CHAT_DEFAULT_TIMEOUT 10
#define AE_CHAT_TEXT_MAX 500      /* the server's limit on one message */
#define AE_CHAT_ROW_FIELDS 10     /* the widest row: profile (events 9, history 8) */
#define AE_CHAT_ROW_MIN 8         /* history and events rows carry at least this many: an older server sends events without replyTo */

/** What PROGDIR:LIVECHAT.cfg holds. */
typedef struct {
    char host[64];
    int port;                  /* 443 with tls, else 80, unless set */
    char board_key[128];       /* <board-id>.<secret> */
    int timeout_secs;          /* 10 unless set */
    int tls;                   /* Tls=: 1 unless set to 0 */
    int allow_plain_http;      /* AllowPlainHttp=: 0 unless set to 1; without it Tls=0 is refused */
} ae_chat_config;

/** NULL when the config may connect; else the one ASCII line saying why not
 *  (plain HTTP without AllowPlainHttp=1). The board key is never sent in
 *  clear text unless the sysop allowed it. */
const char *ae_chat_config_refusal(const ae_chat_config *cfg);

/**
 * Parse `Key=value` text (`#` and `;` comments; keys Host, Port, BoardKey,
 * Timeout) into `cfg`. Returns 1 when Host and BoardKey are both non-empty,
 * else 0. `cfg` is always left filled with the defaults for what is missing.
 */
int ae_chat_config_parse(ae_chat_config *cfg, const char *text);

/** Read a file and parse it. Returns 0 when the file cannot be opened. */
int ae_chat_config_load(ae_chat_config *cfg, const char *path);

/** Write every key (Host, Port, BoardKey, Timeout, Tls, AllowPlainHttp) so ae_chat_config_load reads
 *  it back unchanged. Returns 0, or -1 when the file cannot be written. */
int ae_chat_config_save(const ae_chat_config *cfg, const char *path);

#define AE_CHAT_REGISTER_PATH "/achievements/v1/boards/register"

/** POST AE_CHAT_REGISTER_PATH (no key needed) over the cfg's Host/Port/Tls. `contact` may be NULL.
 *  On success `key_out` holds "<id>.<secret>". On AE_CHAT_ERR_STATUS `err` (may be NULL) holds the
 *  hub's one-line reason; on a refused config the config's refusal line. */
int ae_chat_register(const ae_chat_config *cfg, const char *name, const char *sysop,
                     const char *contact, char *key_out, int key_cap, char *err, int err_cap);

typedef struct {
    ae_chat_config cfg;
    char handle[32];           /* kept so an expired session can be reopened */
    int sysop;
    char sid[64];
    char user[48];             /* handle@BOARD as the server names us */
    int expires_in;
    long last_seq;             /* the next `since` for ae_chat_poll */
    long send_counter;         /* makes clientId unique within the session */
    char client_id[48];        /* the clientId of the last line sent ("" before any) */
    char sent_id[48];          /* the server's id for it, from the send reply ("" until it answers) */
    int status;                /* HTTP status of the last call */
    char error[32];            /* the server's error code on a non-2xx */
    char detail[160];          /* the server's reason text on a non-2xx ("" when none) */
    int retry_after;           /* seconds, when the server sent retryAfter */
    int http_error;            /* ae_http error of the last AE_CHAT_ERR_TRANSPORT */
    int calls;                 /* requests sent, reopens included */
    char reply[AE_CHAT_REPLY_MAX];
    unsigned long reply_len;
    unsigned long cursor;      /* where ae_chat_next_row reads next */
} ae_chat;

/** Start a session. `cfg` is copied. Returns AE_CHAT_OK or an AE_CHAT_ERR_*. */
int ae_chat_open(ae_chat *c, const ae_chat_config *cfg, const char *handle, int sysop);

/** End the session (DELETE /sessions/current). Safe on an unopened ae_chat. */
int ae_chat_close(ae_chat *c);

/** GET rooms.txt. Rows: id, name, topic, visibility, members, owner, kind. A hub from before room
 *  types sends no kind: such a room is a text room. */
int ae_chat_rooms(ae_chat *c);

/** POST /rooms. `topic` may be NULL. `visibility` "public" or "private". `kind` "text", "voice" or
 *  "art"; NULL or "" leaves it to the hub, which makes a text room. On success `id` (may be NULL)
 *  gets the new room's id: rooms are joined by id, never by name. */
int ae_chat_room_create(ae_chat *c, const char *name, const char *topic, const char *visibility,
                        const char *kind, char *id, int id_cap);

/** POST /rooms/:id/join. A "since" newer than c->last_seq moves last_seq up to it. On success `name`/`topic`/`motd` (any may be NULL) get the room's, `members` its count. */
int ae_chat_join(ae_chat *c, const char *room, char *name, int name_cap,
                 char *topic, int topic_cap, char *motd, int motd_cap, int *members);

/** POST /rooms/:id/leave. */
int ae_chat_leave(ae_chat *c, const char *room);

/** GET history.txt, newest `limit` rows, older than `before` when > 0.
 *  Rows: seq, id, user, at, replyTo, threadId, roleColor, text. */
int ae_chat_history(ae_chat *c, const char *room, long before, int limit);

/** POST a line to a room. `reply_to` / `thread_id` may be NULL. Sets c->client_id to the line's clientId,
 *  and on success c->sent_id to the id the server stored it under (the TS room:message:ack). */
int ae_chat_send(ae_chat *c, const char *room, const char *text,
                 const char *reply_to, const char *thread_id);

/** GET events.txt?since=c->last_seq&wait=`wait`. Rows: seq, type, room, user, ref, at, roleColor, text, replyTo
 *  (replyTo set on message events that answer one; absent from an older server). */
int ae_chat_poll(ae_chat *c, int wait);

/** POST /messages/:id/react. `count` (may be NULL) gets the new count. */
int ae_chat_react(ae_chat *c, const char *msg_id, const char *emoji, int *count);

/** DELETE /messages/:id/react/:emoji: takes the caller's reaction back. `count` (may be NULL) gets the new count. */
int ae_chat_unreact(ae_chat *c, const char *msg_id, const char *emoji, int *count);

/** GET /messages/:id/reactions.txt. Rows: emoji, count, mine (1 when the caller reacted with it). */
int ae_chat_reactions(ae_chat *c, const char *msg_id);

/** PUT /messages/:id {content}: replaces the text of the caller's own message. */
int ae_chat_edit(ae_chat *c, const char *msg_id, const char *text);

/** DELETE /messages/:id: the caller's own message, or anyone's for a room moderator. */
int ae_chat_delete(ae_chat *c, const char *msg_id);

/** PUT /rooms/:id/topic {topic}. An empty topic clears it. */
int ae_chat_topic(ae_chat *c, const char *room, const char *topic);

/** PUT /rooms/:id/motd {motd}: the room's message of the day (TS /motd). An empty motd clears it. Owner or moderator only. */
int ae_chat_room_motd(ae_chat *c, const char *room, const char *motd);

/** POST /rooms/:id/typing {text} (TS live typing preview): the caller's whole input line, so other
 * members see it as it is typed. An empty text means the caller stopped (sent or cleared the line).
 * The hub emits it as a "typing" event; callers throttle. */
int ae_chat_typing(ae_chat *c, const char *room, const char *text);

/** POST /rooms/:id/mode {modes, params} (TS /mode): staff apply a mode string such as "+mv" with
 * space-separated params. `applied` gets what took effect ("+m +v bob@BOARD"), `errors` the
 * per-flag refusals joined by '|'. Either may be NULL. */
int ae_chat_mode(ae_chat *c, const char *room, const char *modes, const char *params,
                 char *applied, int applied_cap, char *errors, int errors_cap);

/** POST /rooms/:id/<action> {user, reason?, minutes?}. action: kick, ban, unban, mute, unmute, op, deop.
 *  user is handle@BOARD. reason may be NULL or ""; minutes (mute only) 0 means until unmuted. */
int ae_chat_moderate(ae_chat *c, const char *room, const char *action, const char *user,
                     const char *reason, int minutes);

/** add: POST /rooms/:id/invites {user}; else DELETE /rooms/:id/invites/:user. user is handle@BOARD. */
int ae_chat_invite(ae_chat *c, const char *room, const char *user, int add);

/** POST /messages/:id/thread. */
int ae_chat_thread(ae_chat *c, const char *msg_id, const char *text);

/** GET /threads/:id.txt. Rows as history. */
int ae_chat_thread_list(ae_chat *c, const char *msg_id, int limit);

/** GET /dm/:threadId/history.txt. Rows as history. */
int ae_chat_dm_history(ae_chat *c, const char *thread_id, long before, int limit);

/** POST /dm/:peer ("handle@BOARD"). `thread_id` (may be NULL) gets the DM thread. */
int ae_chat_dm(ae_chat *c, const char *peer, const char *text, char *thread_id, int cap);

/** POST /dm-group: one message to a group of `n` peers ("handle@BOARD").
    The hub adds the caller; it needs 3+ unique members and at most 10.
    `thread_id` (may be NULL) gets the group thread ("dg..."). */
int ae_chat_dm_group(ae_chat *c, const char *const *peers, int n, const char *text,
                     char *thread_id, int cap);

/** GET dm.txt. Rows: threadId, peer, lastAt, unread. */
int ae_chat_dm_list(ae_chat *c);

/** GET who.txt. Rows: user, room, sysop, roleColor, idleSeconds, status, statusText. */
int ae_chat_who(ae_chat *c);

/** GET members.txt. Rows: user, sysop, op, roleColor, roleName, status, statusText. */
int ae_chat_members(ae_chat *c, const char *room);

/** PUT /presence: status is online, away, busy or dnd; `text` (may be NULL) is the away message. */
int ae_chat_presence(ae_chat *c, const char *status, const char *text);

/** DELETE /presence: back to online with no text. */
int ae_chat_presence_clear(ae_chat *c);

/** GET /users/:user/profile.txt ("handle@BOARD"). One row: user, nick, board, status, statusText,
 *  firstSeen, lastSeen, idleSeconds, online, sharedRooms. 404 not_found when the server never saw them. */
int ae_chat_profile(ae_chat *c, const char *user);

/** GET ignores.txt. Rows: user, at. */
int ae_chat_ignores(ae_chat *c);

/** add: POST /ignores {user}; else DELETE /ignores/:user. `user` is "handle@BOARD". */
int ae_chat_ignore(ae_chat *c, const char *user, int add);

/** GET /rooms/:id/pins (JSON on the wire, read here as rows): messageId, user, content, pinnedBy, at (pin time),
 *  messageAt (time the message was posted). */
int ae_chat_pins(ae_chat *c, const char *room);

/** pin: POST /rooms/:id/pins {messageId}; else DELETE /rooms/:id/pins/:messageId. Needs room op. */
int ae_chat_pin(ae_chat *c, const char *room, const char *msg_id, int pin);

/** GET /search?q=&room=&user=&limit= (room and user may be NULL). `user` is a bare
 *  handle or "handle@BOARD", matched case-insensitively. Rows: id, room, user, at, content. */
int ae_chat_search(ae_chat *c, const char *query, const char *room, const char *user, int limit);

/** GET /emoji.txt. Rows: name, display, ascii (an image emoji has the two text fields empty). */
int ae_chat_emoji_list(ae_chat *c);

/** PUT /emoji/:name {display, ascii}. Sysop only; each text 1 to 32 bytes without '|' or line breaks. */
int ae_chat_emoji_add(ae_chat *c, const char *name, const char *display, const char *ascii);

/** DELETE /emoji/:name. Sysop only. */
int ae_chat_emoji_remove(ae_chat *c, const char *name);

/** GET /motd: the board-wide message of the day into text (empty when none). */
int ae_chat_motd(ae_chat *c, char *text, int cap);

/** PUT /motd {text}; empty text clears. Sysop only. */
int ae_chat_motd_set(ae_chat *c, const char *text);

/** DELETE /rooms/:id. */
int ae_chat_room_delete(ae_chat *c, const char *room);

/** POST /rooms/:id/clear. */
int ae_chat_room_clear(ae_chat *c, const char *room);

/**
 * Next row of the last list reply, split on `|` with `!` turned back into
 * `|`. Fills fields[0..max-1] (missing trailing fields point at ""), returns
 * how many fields the row really had, or 0 when there are no more rows.
 */
int ae_chat_next_row(ae_chat *c, char **fields, int max);

#ifdef __cplusplus
}
#endif

#endif /* AE_CHAT_H */
