/*
 * ae_request - the global-requests client a C door talks to the door server
 * through. A sibling of ae_achieve: the same wire (POST/GET with
 * `Authorization: Board <key>`), the same bounded, allocation-free style, the
 * same json_lite on the reply. Where ae_achieve reports a fact and is told
 * what unlocked, this posts a request, votes, claims and marks done, and reads
 * the listing back as the server's plain-text `|`-separated rows.
 *
 * WHERE THE SERVER IS. The BBS keeps its board key encrypted and a 68K door
 * cannot ask for it, so the door reads its own REQUESTS.cfg beside the
 * executable, exactly as ACHIEVE reads ACHIEVE.cfg.
 *
 * Platform-neutral C89: every socket call goes through ae_net, the framing
 * through ae_http, so this file never mentions AMIGA.
 */
#ifndef AE_REQUEST_H
#define AE_REQUEST_H

#include "ae_http.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AE_REQ_OK            0
#define AE_REQ_ERR_ARGS      -1  /* NULL config, or an empty required field */
#define AE_REQ_ERR_TOO_LONG  -2  /* the body/path did not fit the fixed buffer */
#define AE_REQ_ERR_TRANSPORT -3  /* the request never completed; see ae_req_last_http_error() */
#define AE_REQ_ERR_STATUS    -4  /* the server answered, and not with 200; see out->status/out->error */
#define AE_REQ_ERR_BODY      -5  /* a 200 whose body is not the expected shape */

/** The base path every route hangs off, so a caller can log it. */
#define AE_REQ_BASE "/requests/v1"

/** Where the board's door server is, and who this board is. */
typedef struct {
    const char *host;        /* "doors.uprough.net"; never NULL */
    int port;                /* 80 unless the board says otherwise */
    const char *board_key;   /* the key the sysop minted; never NULL */
    int timeout_secs;        /* 0 -> AE_REQ_DEFAULT_TIMEOUT */
    const char *user_agent;  /* NULL -> ae_http's default */
} ae_req_config;

#define AE_REQ_DEFAULT_TIMEOUT 10

/** What is being asked for. The wire words the server tests with strcmp. */
typedef enum { AE_REQ_KIND_RELEASE = 0, AE_REQ_KIND_ART = 1 } ae_req_kind;

/**
 * What the server said to a write. `id`, `votes` and `state` are filled as far
 * as the reply carried them; on a refusal `error` holds the server's own error
 * code (e.g. "already_claimed") so the door can map it to a sentence.
 */
typedef struct {
    int status;        /* the HTTP status; 200 on success */
    long id;
    int votes;
    char state[16];    /* "open" | "claimed" | "done" | "withdrawn" */
    char error[32];    /* the server's error code on a non-200 */
} ae_req_result;

/**
 * GET a listing or a detail page. `path_and_query` is the full path after the
 * host (the caller builds it, e.g. AE_REQ_BASE "/list.txt?state=open"); the
 * body is streamed to `sink`. Returns the HTTP status, or a negative
 * AE_REQ_ERR_* / ae_http error. A non-200 status is NOT negative here - the
 * body still streamed and the status says what the server said.
 */
int ae_req_get(const ae_req_config *cfg, const char *path_and_query,
               ae_http_sink_fn sink, void *ctx);

/**
 * POST /requests. `notes` may be NULL or empty and is then left out of the
 * JSON. `handle` is the caller's board handle, read by the door - never typed.
 * Returns AE_REQ_OK with `out` filled, or an AE_REQ_ERR_* code.
 */
int ae_req_post(const ae_req_config *cfg, ae_req_kind kind,
                const char *title, const char *notes, const char *handle,
                ae_req_result *out);

/**
 * POST /requests/<id>/<action>, action one of "vote","claim","release",
 * "done","withdraw". `note` is sent only by "done" and only when non-empty;
 * pass NULL otherwise. Returns AE_REQ_OK, or an AE_REQ_ERR_* code with
 * `out->error` naming the refusal on a non-200.
 */
int ae_req_action(const ae_req_config *cfg, long id, const char *action,
                  const char *handle, const char *note, ae_req_result *out);

/**
 * Percent-encode `s` into `out` (RFC 3986 unreserved kept, everything else
 * %XX), for a query-string value such as a handle. Returns the length written
 * (excluding the NUL), or -1 if it would not fit `cap`.
 */
long ae_req_url_escape(const char *s, char *out, unsigned long cap);

/**
 * The value of `key` in a form-encoded query string ("a=1&b=x%20y", the
 * shape URLSearchParams.toString() writes), decoded: "+" is a space and %XX
 * is that byte. Writes "" when the key is absent. Returns 1 when the key was
 * found, 0 when absent, -1 when the decoded value would not fit `cap` (out
 * then holds as much as fits).
 */
int ae_req_form_value(const char *query, const char *key, char *out, unsigned long cap);

/**
 * The JSON a POST /requests becomes, exposed for a door that wants to log it
 * and for the tests. Writes at most `size` bytes including the NUL and returns
 * the length written, or -1 when it would not fit or a required field is
 * missing.
 */
long ae_req_build_post_body(ae_req_kind kind, const char *title,
                            const char *notes, const char *handle,
                            char *buf, unsigned long size);

/**
 * Bounded JSON writers, shared with ae_chat so a body is escaped one way
 * everywhere. ae_req_put_json_string appends `s` as a quoted, escaped JSON
 * string to `buf` at offset `at`; ae_req_put_raw appends literal text. Both
 * return the new length, or -1 when it would not fit `size`.
 */
long ae_req_put_json_string(char *buf, unsigned long size, unsigned long at, const char *s);
long ae_req_put_raw(char *buf, unsigned long size, unsigned long at, const char *text);

/**
 * The ae_http error from the last call that returned AE_REQ_ERR_TRANSPORT, for
 * a door that wants to say why.
 */
int ae_req_last_http_error(void);

#ifdef __cplusplus
}
#endif

#endif /* AE_REQUEST_H */
