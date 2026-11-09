/*
 * ae_request - the global-requests client. See ae_request.h for what this is.
 *
 * Modelled line for line on ae_achieve.c: fixed stack buffers that refuse to
 * overflow, the JSON written by hand because there is exactly one shape per
 * call, the same two extra headers, json_lite on the reply. Nothing here
 * allocates and nothing here mentions AMIGA.
 */
#include <string.h>
#include <stdio.h>

#include "ae_request.h"
#include "ae_http.h"
#include "json_lite.h"

/* One write's JSON. A kind, a title (<=60), notes (<=200) and a handle (<=24)
 * do not reach this even at their longest. */
#define AE_REQ_BODY_MAX 1024
/* The answer: a handful of scalar fields. */
#define AE_REQ_REPLY_MAX 2048

static int last_http_error = 0;

int ae_req_last_http_error(void)
{
    return last_http_error;
}

static int blank(const char *s)
{
    return s == 0 || s[0] == '\0';
}

/* A JSON string body, escaped as the standard requires and no further - the
 * same escaper ae_achieve.c uses. Returns the byte count, or -1 if it would
 * not fit. */
long ae_req_put_json_string(char *buf, unsigned long size, unsigned long at, const char *s)
{
    unsigned long i = at;
    const unsigned char *p = (const unsigned char *)s;

    if (i + 1 >= size) return -1;
    buf[i++] = '"';
    for (; *p != '\0'; p++) {
        const char *escape = 0;
        char unicode[7];

        switch (*p) {
            case '"':  escape = "\\\""; break;
            case '\\': escape = "\\\\"; break;
            case '\b': escape = "\\b";  break;
            case '\f': escape = "\\f";  break;
            case '\n': escape = "\\n";  break;
            case '\r': escape = "\\r";  break;
            case '\t': escape = "\\t";  break;
            default:
                if (*p < 0x20) {
                    sprintf(unicode, "\\u%04x", (unsigned int)*p);
                    escape = unicode;
                }
                break;
        }

        if (escape != 0) {
            unsigned long len = (unsigned long)strlen(escape);
            if (i + len + 1 >= size) return -1;
            memcpy(buf + i, escape, len);
            i += len;
        } else {
            if (i + 2 >= size) return -1;
            buf[i++] = (char)*p;
        }
    }
    if (i + 1 >= size) return -1;
    buf[i++] = '"';
    buf[i] = '\0';
    return (long)i;
}

/* Literal text, bounds-checked the same way. */
long ae_req_put_raw(char *buf, unsigned long size, unsigned long at, const char *text)
{
    unsigned long len = (unsigned long)strlen(text);
    if (at + len + 1 >= size) return -1;
    memcpy(buf + at, text, len);
    buf[at + len] = '\0';
    return (long)(at + len);
}

static const char *kind_word(ae_req_kind kind)
{
    return kind == AE_REQ_KIND_ART ? "art" : "release";
}

long ae_req_build_post_body(ae_req_kind kind, const char *title,
                            const char *notes, const char *handle,
                            char *buf, unsigned long size)
{
    long at;

    if (buf == 0 || size == 0) return -1;
    if (blank(title) || blank(handle)) return -1;

    at = ae_req_put_raw(buf, size, 0, "{\"kind\":");
    if (at < 0) return -1;
    at = ae_req_put_json_string(buf, size, (unsigned long)at, kind_word(kind));
    if (at < 0) return -1;
    at = ae_req_put_raw(buf, size, (unsigned long)at, ",\"title\":");
    if (at < 0) return -1;
    at = ae_req_put_json_string(buf, size, (unsigned long)at, title);
    if (at < 0) return -1;
    /* notes is optional: left out entirely rather than sent as "" so the
     * server's default takes over cleanly. */
    if (!blank(notes)) {
        at = ae_req_put_raw(buf, size, (unsigned long)at, ",\"notes\":");
        if (at < 0) return -1;
        at = ae_req_put_json_string(buf, size, (unsigned long)at, notes);
        if (at < 0) return -1;
    }
    at = ae_req_put_raw(buf, size, (unsigned long)at, ",\"handle\":");
    if (at < 0) return -1;
    at = ae_req_put_json_string(buf, size, (unsigned long)at, handle);
    if (at < 0) return -1;
    at = ae_req_put_raw(buf, size, (unsigned long)at, "}");
    return at;
}

/* {"handle":"...","note":"..."} - note only when given. */
static long build_action_body(const char *handle, const char *note, char *buf, unsigned long size)
{
    long at;

    if (blank(handle)) return -1;
    at = ae_req_put_raw(buf, size, 0, "{\"handle\":");
    if (at < 0) return -1;
    at = ae_req_put_json_string(buf, size, (unsigned long)at, handle);
    if (at < 0) return -1;
    if (!blank(note)) {
        at = ae_req_put_raw(buf, size, (unsigned long)at, ",\"note\":");
        if (at < 0) return -1;
        at = ae_req_put_json_string(buf, size, (unsigned long)at, note);
        if (at < 0) return -1;
    }
    at = ae_req_put_raw(buf, size, (unsigned long)at, "}");
    return at;
}

long ae_req_url_escape(const char *s, char *out, unsigned long cap)
{
    static const char HEX[] = "0123456789ABCDEF";
    unsigned long i = 0;
    const unsigned char *p;

    if (s == 0 || out == 0 || cap == 0) return -1;
    for (p = (const unsigned char *)s; *p != '\0'; p++) {
        unsigned char c = *p;
        int unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                         c == '.' || c == '~';
        if (unreserved) {
            if (i + 1 >= cap) return -1;
            out[i++] = (char)c;
        } else {
            if (i + 3 >= cap) return -1;
            out[i++] = '%';
            out[i++] = HEX[(c >> 4) & 0xf];
            out[i++] = HEX[c & 0xf];
        }
    }
    out[i] = '\0';
    return (long)i;
}

static int hex_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int ae_req_form_value(const char *query, const char *key, char *out, unsigned long cap)
{
    const char *p = query;
    unsigned long klen, n = 0;
    if (out == 0 || cap == 0) return -1;
    out[0] = '\0';
    if (query == 0 || key == 0) return 0;
    klen = (unsigned long)strlen(key);
    while (*p != '\0') {
        const char *end = strchr(p, '&');
        if (end == 0) end = p + strlen(p);
        if ((unsigned long)(end - p) > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            for (p += klen + 1; p < end; p++) {
                int c = (unsigned char)*p, hi, lo;
                if (c == '+') c = ' ';
                else if (c == '%' && end - p > 2 && (hi = hex_digit(p[1])) >= 0 && (lo = hex_digit(p[2])) >= 0) {
                    c = hi * 16 + lo;
                    p += 2;
                }
                if (n + 1 >= cap) { out[n] = '\0'; return -1; }
                out[n++] = (char)c;
            }
            out[n] = '\0';
            return 1;
        }
        p = *end == '&' ? end + 1 : end;
    }
    return 0;
}

/* The reply, kept only as far as it fits: the door needs the scalar fields at
 * the front, not a long body. */
typedef struct {
    char body[AE_REQ_REPLY_MAX];
    unsigned long len;
} reply_buffer;

static int collect(void *ctx, const unsigned char *buf, unsigned long len)
{
    reply_buffer *reply = (reply_buffer *)ctx;
    unsigned long room = (unsigned long)(sizeof reply->body) - 1 - reply->len;
    unsigned long take = len < room ? len : room;

    if (take > 0) {
        memcpy(reply->body + reply->len, buf, take);
        reply->len += take;
        reply->body[reply->len] = '\0';
    }
    return 0; /* never abort: the status matters even when the body is long */
}

/* Auth header shared by every call. Returns 0 on success. */
static int authorization(const ae_req_config *cfg, char *out, unsigned long size)
{
    if ((unsigned long)strlen(cfg->board_key) + 32 >= size) return -1;
    sprintf(out, "Authorization: Board %s\r\n", cfg->board_key);
    return 0;
}

static void fill_target(const ae_req_config *cfg, ae_http_target *target)
{
    memset(target, 0, sizeof *target);
    target->host = cfg->host;
    target->port = cfg->port > 0 ? cfg->port : 80;
    target->timeout_secs = cfg->timeout_secs > 0 ? cfg->timeout_secs : AE_REQ_DEFAULT_TIMEOUT;
    target->user_agent = cfg->user_agent;
}

int ae_req_get(const ae_req_config *cfg, const char *path_and_query,
               ae_http_sink_fn sink, void *ctx)
{
    char auth[256];
    const char *headers[1];
    ae_http_target target;
    ae_http_response response;
    int rc;

    last_http_error = 0;
    if (cfg == 0 || path_and_query == 0 || sink == 0) return AE_REQ_ERR_ARGS;
    if (blank(cfg->host) || blank(cfg->board_key)) return AE_REQ_ERR_ARGS;
    if (authorization(cfg, auth, sizeof auth) != 0) return AE_REQ_ERR_TOO_LONG;
    headers[0] = auth;

    fill_target(cfg, &target);
    memset(&response, 0, sizeof response);
    rc = ae_http_request(&target, "GET", path_and_query, 0, 0,
                         headers, 1, &response, sink, ctx, 0);
    if (rc != AE_HTTP_OK) {
        last_http_error = rc;
        return AE_REQ_ERR_TRANSPORT;
    }
    return response.status;
}

/* Send `body` to `path`, parse the scalar fields the door reads. */
static int write_request(const ae_req_config *cfg, const char *path,
                         const char *body, long body_len, ae_req_result *out)
{
    char auth[256];
    const char *headers[2];
    ae_http_target target;
    ae_http_response response;
    reply_buffer reply;
    int rc;
    int value = 0;

    if (out != 0) memset(out, 0, sizeof *out);
    last_http_error = 0;

    if (cfg == 0) return AE_REQ_ERR_ARGS;
    if (blank(cfg->host) || blank(cfg->board_key)) return AE_REQ_ERR_ARGS;
    if (body_len < 0) return AE_REQ_ERR_TOO_LONG;
    if (authorization(cfg, auth, sizeof auth) != 0) return AE_REQ_ERR_TOO_LONG;
    headers[0] = auth;
    headers[1] = "Content-Type: application/json\r\n";

    fill_target(cfg, &target);
    memset(&response, 0, sizeof response);
    reply.len = 0;
    reply.body[0] = '\0';

    rc = ae_http_request(&target, "POST", path, body, (unsigned long)body_len,
                         headers, 2, &response, collect, &reply, 0);
    if (rc != AE_HTTP_OK) {
        last_http_error = rc;
        return AE_REQ_ERR_TRANSPORT;
    }

    if (out != 0) {
        out->status = response.status;
        if (json_extract_bool(reply.body, "id", &value) == 0) out->id = (long)value;
        if (json_extract_bool(reply.body, "votes", &value) == 0) out->votes = value;
        json_extract_string(reply.body, "state", out->state, sizeof out->state);
        if (response.status != 200) {
            json_extract_string(reply.body, "error", out->error, sizeof out->error);
        }
    }

    if (response.status != 200) return AE_REQ_ERR_STATUS;
    return AE_REQ_OK;
}

int ae_req_post(const ae_req_config *cfg, ae_req_kind kind,
                const char *title, const char *notes, const char *handle,
                ae_req_result *out)
{
    char body[AE_REQ_BODY_MAX];
    long body_len;

    if (out != 0) memset(out, 0, sizeof *out);
    if (cfg == 0) return AE_REQ_ERR_ARGS;
    if (blank(title) || blank(handle)) return AE_REQ_ERR_ARGS;

    body_len = ae_req_build_post_body(kind, title, notes, handle, body, sizeof body);
    if (body_len < 0) return AE_REQ_ERR_TOO_LONG;

    return write_request(cfg, AE_REQ_BASE "/requests", body, body_len, out);
}

int ae_req_action(const ae_req_config *cfg, long id, const char *action,
                  const char *handle, const char *note, ae_req_result *out)
{
    char body[AE_REQ_BODY_MAX];
    char path[128];
    long body_len;

    if (out != 0) memset(out, 0, sizeof *out);
    if (cfg == 0) return AE_REQ_ERR_ARGS;
    if (blank(action) || blank(handle)) return AE_REQ_ERR_ARGS;
    if (id <= 0) return AE_REQ_ERR_ARGS;

    if ((unsigned long)strlen(action) + 32 >= sizeof path) return AE_REQ_ERR_TOO_LONG;
    sprintf(path, AE_REQ_BASE "/requests/%ld/%s", id, action);

    body_len = build_action_body(handle, note, body, sizeof body);
    if (body_len < 0) return AE_REQ_ERR_TOO_LONG;

    return write_request(cfg, path, body, body_len, out);
}
