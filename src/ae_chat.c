/*
 * ae_chat - the livechat client. See ae_chat.h for what this is.
 *
 * Modelled on ae_request.c: fixed buffers that refuse to overflow, JSON
 * written by hand (it shares ae_request's two writers so a string is escaped
 * one way everywhere), json_lite only on the session reply.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ae_chat.h"
#include "ae_request.h"
#include "json_lite.h"

#define BODY_MAX 1024
#define PATH_MAX_LEN 256

static int blank(const char *s)
{
    return s == 0 || s[0] == '\0';
}

/* ------------------------------------------------------------------ config */

static void rtrim(char *s)
{
    unsigned long len = (unsigned long)strlen(s);
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n' ||
                       s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
}

static void copy_field(char *dst, unsigned long cap, const char *src)
{
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

int ae_chat_config_parse(ae_chat_config *cfg, const char *text)
{
    char line[256];
    const char *p = text;

    memset(cfg, 0, sizeof *cfg);
    cfg->timeout_secs = AE_CHAT_DEFAULT_TIMEOUT;
    cfg->tls = 1;
    if (text == 0) {
        cfg->port = 443;
        return 0;
    }

    while (*p != '\0') {
        unsigned long n = 0;
        char *eq;
        char *value;

        while (*p != '\0' && *p != '\n') {
            if (n < sizeof line - 1) line[n++] = *p;
            p++;
        }
        if (*p == '\n') p++;
        line[n] = '\0';
        rtrim(line);
        if (line[0] == '#' || line[0] == ';' || line[0] == '\0') continue;
        eq = strchr(line, '=');
        if (eq == 0) continue;
        *eq = '\0';
        value = eq + 1;
        while (*value == ' ' || *value == '\t') value++;

        if (strcmp(line, "Host") == 0) {
            copy_field(cfg->host, sizeof cfg->host, value);
        } else if (strcmp(line, "Port") == 0) {
            int port = atoi(value);
            if (port > 0 && port < 65536) cfg->port = port;
        } else if (strcmp(line, "BoardKey") == 0) {
            copy_field(cfg->board_key, sizeof cfg->board_key, value);
        } else if (strcmp(line, "Timeout") == 0) {
            int secs = atoi(value);
            if (secs > 0 && secs < 120) cfg->timeout_secs = secs;
        } else if (strcmp(line, "Tls") == 0) {
            cfg->tls = atoi(value) != 0;
        } else if (strcmp(line, "AllowPlainHttp") == 0) {
            cfg->allow_plain_http = atoi(value) != 0;
        }
    }
    if (cfg->port == 0) cfg->port = cfg->tls ? 443 : 80;
    return cfg->host[0] != '\0' && cfg->board_key[0] != '\0';
}

const char *ae_chat_config_refusal(const ae_chat_config *cfg)
{
    if (cfg->tls || cfg->allow_plain_http) return 0;
    return "Plain HTTP refused: set Tls=1, or AllowPlainHttp=1 in LIVECHAT.cfg.";
}

int ae_chat_config_load(ae_chat_config *cfg, const char *path)
{
    char text[1024];
    unsigned long n;
    FILE *f = fopen(path, "r");

    if (f == 0) {
        ae_chat_config_parse(cfg, "");
        return 0;
    }
    n = (unsigned long)fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = '\0';
    return ae_chat_config_parse(cfg, text);
}

/* The one owner of the cfg format: every key the parser reads is written, so a
 * file the wizard made loads back to the same ae_chat_config. */
int ae_chat_config_save(const ae_chat_config *cfg, const char *path)
{
    FILE *f = fopen(path, "w");
    int ok;

    if (f == 0) return -1;
    ok = fprintf(f, "# LiveChat68 settings. NEVER share this file: BoardKey identifies your board.\n"
                    "Host=%s\nPort=%d\nBoardKey=%s\nTimeout=%d\nTls=%d\nAllowPlainHttp=%d\n",
                 cfg->host, cfg->port, cfg->board_key,
                 cfg->timeout_secs > 0 ? cfg->timeout_secs : AE_CHAT_DEFAULT_TIMEOUT,
                 cfg->tls ? 1 : 0, cfg->allow_plain_http ? 1 : 0) > 0;
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

typedef struct {
    char text[512];
    unsigned long len;
} register_reply;

static int register_collect(void *ctx, const unsigned char *buf, unsigned long len)
{
    register_reply *r = (register_reply *)ctx;
    unsigned long room = (unsigned long)sizeof r->text - 1 - r->len;
    unsigned long take = len < room ? len : room;

    if (take > 0) {
        memcpy(r->text + r->len, buf, take);
        r->len += take;
        r->text[r->len] = '\0';
    }
    return 0;
}

static void register_error(char *err, int err_cap, const char *text)
{
    if (err != 0 && err_cap > 0) copy_field(err, (unsigned long)err_cap, text);
}

int ae_chat_register(const ae_chat_config *cfg, const char *name, const char *sysop,
                     const char *contact, char *key_out, int key_cap, char *err, int err_cap)
{
    char body[BODY_MAX];
    const char *headers[2];
    register_reply reply;
    ae_http_target target;
    ae_http_response response;
    long at;
    int rc;

    register_error(err, err_cap, "");
    if (cfg == 0 || key_out == 0 || key_cap < 2 || blank(name) || blank(sysop)) return AE_CHAT_ERR_ARGS;
    key_out[0] = '\0';
    if (ae_chat_config_refusal(cfg) != 0) {
        register_error(err, err_cap, ae_chat_config_refusal(cfg));
        return AE_CHAT_ERR_ARGS;
    }

    at = ae_req_put_raw(body, sizeof body, 0, "{\"name\":");
    if (at >= 0) at = ae_req_put_json_string(body, sizeof body, (unsigned long)at, name);
    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, ",\"sysop\":");
    if (at >= 0) at = ae_req_put_json_string(body, sizeof body, (unsigned long)at, sysop);
    if (at >= 0 && !blank(contact)) {
        at = ae_req_put_raw(body, sizeof body, (unsigned long)at, ",\"contact\":");
        if (at >= 0) at = ae_req_put_json_string(body, sizeof body, (unsigned long)at, contact);
    }
    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, "}");
    if (at < 0) return AE_CHAT_ERR_TOO_LONG;

    headers[0] = "Accept: text/plain\r\n";
    headers[1] = "Content-Type: application/json\r\n";
    memset(&target, 0, sizeof target);
    target.host = cfg->host;
    target.tls = cfg->tls;
    target.port = cfg->port > 0 ? cfg->port : (cfg->tls ? 443 : 80);
    target.timeout_secs = cfg->timeout_secs > 0 ? cfg->timeout_secs : AE_CHAT_DEFAULT_TIMEOUT;
    target.user_agent = "AmiExpress-LIVECHAT68/1.0";
    memset(&response, 0, sizeof response);
    memset(&reply, 0, sizeof reply);

    rc = ae_http_request(&target, "POST", AE_CHAT_REGISTER_PATH, body, (unsigned long)at,
                         headers, 2, &response, register_collect, &reply, 0);
    if (rc != AE_HTTP_OK) return AE_CHAT_ERR_TRANSPORT;
    if (response.status < 200 || response.status > 299) {
        char detail[160];
        detail[0] = '\0';
        json_extract_string(reply.text, "detail", detail, sizeof detail);
        if (detail[0] == '\0') json_extract_string(reply.text, "error", detail, sizeof detail);
        register_error(err, err_cap, detail[0] != '\0' ? detail : "The chat server refused the registration.");
        return AE_CHAT_ERR_STATUS;
    }
    if (strncmp(reply.text, "OK ", 3) != 0) return AE_CHAT_ERR_BODY;
    {
        const char *k = reply.text + 3;
        unsigned long n = 0;
        while (k[n] != '\0' && k[n] != '\r' && k[n] != '\n' && k[n] != ' ') n++;
        if (n == 0 || strchr(k, '.') == 0 || n >= (unsigned long)key_cap) return AE_CHAT_ERR_BODY;
        memcpy(key_out, k, n);
        key_out[n] = '\0';
    }
    return AE_CHAT_OK;
}

/* -------------------------------------------------------------- transport */

static int collect(void *ctx, const unsigned char *buf, unsigned long len)
{
    ae_chat *c = (ae_chat *)ctx;
    unsigned long room = (unsigned long)sizeof c->reply - 1 - c->reply_len;
    unsigned long take = len < room ? len : room;

    if (take > 0) {
        memcpy(c->reply + c->reply_len, buf, take);
        c->reply_len += take;
        c->reply[c->reply_len] = '\0';
    }
    return 0; /* never abort: the status matters even when the body is long */
}

static int open_session(ae_chat *c);

static int call_once_x(ae_chat *c, const char *method, const char *path,
                       const char *body, unsigned long body_len,
                       ae_http_sink_fn sink, void *sink_ctx, ae_http_header_fn on_header)
{
    char auth[256];
    char sidh[96];
    const char *headers[3];
    int nheaders = 0;
    ae_http_target target;
    ae_http_response response;
    int rc;

    c->reply_len = 0;
    c->reply[0] = '\0';
    c->cursor = 0;
    c->status = 0;
    c->error[0] = '\0';
    c->detail[0] = '\0';
    c->retry_after = 0;
    c->http_error = 0;

    if ((unsigned long)strlen(c->cfg.board_key) + 32 >= sizeof auth) return AE_CHAT_ERR_TOO_LONG;
    sprintf(auth, "Authorization: Board %s\r\n", c->cfg.board_key);
    headers[nheaders++] = auth;
    if (c->sid[0] != '\0') {
        sprintf(sidh, "X-Chat-Session: %.64s\r\n", c->sid);
        headers[nheaders++] = sidh;
    }
    if (body != 0) headers[nheaders++] = "Content-Type: application/json\r\n";

    if (ae_chat_config_refusal(&c->cfg) != 0) return AE_CHAT_ERR_ARGS;

    memset(&target, 0, sizeof target);
    target.host = c->cfg.host;
    target.tls = c->cfg.tls;
    target.port = c->cfg.port > 0 ? c->cfg.port : (c->cfg.tls ? 443 : 80);
    target.timeout_secs = c->cfg.timeout_secs > 0 ? c->cfg.timeout_secs : AE_CHAT_DEFAULT_TIMEOUT;
    target.user_agent = "AmiExpress-LIVECHAT68/1.0";
    memset(&response, 0, sizeof response);

    c->calls++;
    rc = ae_http_request(&target, method, path, body, body_len, headers, nheaders,
                         &response, sink, sink_ctx, on_header);
    if (rc != AE_HTTP_OK) {
        c->http_error = rc;
        return AE_CHAT_ERR_TRANSPORT;
    }
    c->status = response.status;
    if (response.status < 200 || response.status > 299) {
        int retry = 0;
        json_extract_string(c->reply, "error", c->error, sizeof c->error);
        json_extract_string(c->reply, "detail", c->detail, sizeof c->detail);
        if (json_extract_bool(c->reply, "retryAfter", &retry) == 0) c->retry_after = retry;
        return AE_CHAT_ERR_STATUS;
    }
    return AE_CHAT_OK;
}

static int call_once(ae_chat *c, const char *method, const char *path,
                     const char *body, unsigned long body_len)
{
    return call_once_x(c, method, path, body, body_len, collect, c, 0);
}

/* One call, and when the server says the session expired, one silent reopen
 * and one retry. The poll every few seconds keeps a live session alive; this
 * covers a caller who sat in a prompt past the 90 s limit. */
static int call_x(ae_chat *c, const char *method, const char *path,
                  const char *body, unsigned long body_len,
                  ae_http_sink_fn sink, void *sink_ctx, ae_http_header_fn on_header)
{
    int rc = call_once_x(c, method, path, body, body_len, sink, sink_ctx, on_header);

    if (rc == AE_CHAT_ERR_STATUS && strcmp(c->error, "session_expired") == 0 &&
        !blank(c->handle)) {
        if (open_session(c) == AE_CHAT_OK) {
            rc = call_once_x(c, method, path, body, body_len, sink, sink_ctx, on_header);
        } else {
            rc = AE_CHAT_ERR_STATUS;
        }
    }
    return rc;
}

static int call(ae_chat *c, const char *method, const char *path,
                const char *body, unsigned long body_len)
{
    return call_x(c, method, path, body, body_len, collect, c, 0);
}

/* ----------------------------------------------------------------- session */

static int open_session(ae_chat *c)
{
    char body[BODY_MAX];
    long at;
    int rc;
    int expires = 0;

    c->sid[0] = '\0';
    at = ae_req_put_raw(body, sizeof body, 0, "{\"handle\":");
    if (at >= 0) at = ae_req_put_json_string(body, sizeof body, (unsigned long)at, c->handle);
    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at,
                                     c->sysop ? ",\"sysop\":true,\"client\":" : ",\"sysop\":false,\"client\":");
    if (at >= 0) at = ae_req_put_json_string(body, sizeof body, (unsigned long)at, "livechat68/1.0");
    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, "}");
    if (at < 0) return AE_CHAT_ERR_TOO_LONG;

    rc = call_once(c, "POST", AE_CHAT_BASE "/sessions", body, (unsigned long)at);
    if (rc != AE_CHAT_OK) return rc;
    if (json_extract_string(c->reply, "sid", c->sid, sizeof c->sid) != 0 || c->sid[0] == '\0') {
        c->sid[0] = '\0';
        return AE_CHAT_ERR_BODY;
    }
    json_extract_string(c->reply, "user", c->user, sizeof c->user);
    if (json_extract_bool(c->reply, "expiresIn", &expires) == 0) c->expires_in = expires;
    return AE_CHAT_OK;
}

int ae_chat_open(ae_chat *c, const ae_chat_config *cfg, const char *handle, int sysop)
{
    if (c == 0 || cfg == 0 || blank(handle)) return AE_CHAT_ERR_ARGS;
    if (blank(cfg->host) || blank(cfg->board_key)) return AE_CHAT_ERR_ARGS;
    if (ae_chat_config_refusal(cfg) != 0) return AE_CHAT_ERR_ARGS;
    memset(c, 0, sizeof *c);
    c->cfg = *cfg;
    copy_field(c->handle, sizeof c->handle, handle);
    c->sysop = sysop ? 1 : 0;
    return open_session(c);
}

int ae_chat_close(ae_chat *c)
{
    int rc;
    if (c == 0 || c->sid[0] == '\0') return AE_CHAT_ERR_ARGS;
    rc = call_once(c, "DELETE", AE_CHAT_BASE "/sessions/current", 0, 0);
    c->sid[0] = '\0';
    return rc;
}

/* -------------------------------------------------------------------- rows */

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* Consume the "NAME|version|revision|count" header every text route opens with (the door server's
 * renderText: ROOMS, EVENTS, HISTORY, DM, WHO, MEMBERS, REACTIONS, ...). Left in, a header reads as a data
 * row wherever a caller asks for fewer than four fields (a DM list showed a thread "1"), and a caller that
 * stops at the first short row reads nothing at all (the FILES list never reached the door, so LiveChat 68K
 * placed no pictures). Every renderText name a door reads must be listed here. */
static int has_header(const char *reply)
{
    static const char *const names[] = { "ROOMS|", "EVENTS|", "HISTORY|", "DM|", "WHO|", "MEMBERS|", "REACTIONS|", "PROFILE|", "IGNORES|", "EMOJI|", "FILES|" };
    unsigned i;
    for (i = 0; i < sizeof names / sizeof names[0]; i++) if (starts_with(reply, names[i])) return 1;
    return 0;
}

static void skip_header(ae_chat *c, long *seq)
{
    const char *nl;

    if (!has_header(c->reply)) return;
    if (seq != 0 && starts_with(c->reply, "EVENTS|")) {
        const char *p = strchr(c->reply, '|');           /* after EVENTS */
        p = p != 0 ? strchr(p + 1, '|') : 0;             /* after version */
        if (p != 0) *seq = atol(p + 1);
    }
    nl = strchr(c->reply, '\n');
    c->cursor = nl != 0 ? (unsigned long)(nl - c->reply) + 1 : c->reply_len;
}

/* An events row of type canvas: "seq|canvas|...". */
static int is_canvas_row(const char *line)
{
    const char *p = line;
    while (*p >= '0' && *p <= '9') p++;
    return p != line && starts_with(p, "|canvas|");
}

int ae_chat_next_row(ae_chat *c, char **fields, int max)
{
    char *line;
    char *p;
    int n = 0;
    int i;

    while (c->cursor < c->reply_len) {
        line = c->reply + c->cursor;
        p = strchr(line, '\n');
        if (p != 0) { *p = '\0'; c->cursor = (unsigned long)(p - c->reply) + 1; }
        else c->cursor = c->reply_len;
        if (is_canvas_row(line)) {
            /* a canvas event's ops end in cells, and a trailing space is one: strip only the CR */
            unsigned long n = (unsigned long)strlen(line);
            while (n > 0 && line[n - 1] == '\r') line[--n] = '\0';
        } else {
            rtrim(line);
        }
        if (line[0] == '\0') continue;

        for (i = 0; i < max; i++) fields[i] = line + strlen(line); /* "" */
        for (;;) {
            /* Split every field, so one the server appended later is dropped
             * here instead of folded into the last field this caller reads. */
            char *bar = strchr(line, '|');
            if (n < max) fields[n] = line;
            n++;
            if (bar == 0) break;
            *bar = '\0';
            line = bar + 1;
        }
        /* No unescape: the server writes '|' as '!' and leaves '!' alone, so the
         * wire cannot say which a '!' was. Mapping it back turned every "hi!"
         * into "hi|" (CHAT-API.md: a pipe is shown as '!'; the TS door agrees). */
        return n;
    }
    return 0;
}

/* ------------------------------------------------------------------ routes */

/* path = prefix + escaped id + suffix. */
static int make_path(char *out, unsigned long cap, const char *prefix,
                     const char *id, const char *suffix)
{
    char esc[160];
    unsigned long total;

    if (blank(id)) return AE_CHAT_ERR_ARGS;
    if (ae_req_url_escape(id, esc, sizeof esc) < 0) return AE_CHAT_ERR_TOO_LONG;
    total = (unsigned long)(strlen(prefix) + strlen(esc) + strlen(suffix));
    if (total + 1 > cap) return AE_CHAT_ERR_TOO_LONG;
    sprintf(out, "%s%s%s", prefix, esc, suffix);
    return AE_CHAT_OK;
}

/* A GET of a text route: the reply's rows start after its header. */
static int get_text(ae_chat *c, const char *path)
{
    int rc = call(c, "GET", path, 0, 0);
    if (rc == AE_CHAT_OK) skip_header(c, 0);
    return rc;
}

int ae_chat_rooms(ae_chat *c)
{
    int rc = call(c, "GET", AE_CHAT_BASE "/rooms.txt", 0, 0);
    if (rc == AE_CHAT_OK) skip_header(c, 0);
    return rc;
}

int ae_chat_room_create(ae_chat *c, const char *name, const char *topic, const char *visibility,
                        const char *kind, char *id, int id_cap)
{
    char body[BODY_MAX];
    long at;
    int rc;

    if (id != 0 && id_cap > 0) id[0] = '\0';

    if (blank(name)) return AE_CHAT_ERR_ARGS;
    at = ae_req_put_raw(body, sizeof body, 0, "{\"name\":");
    if (at >= 0) at = ae_req_put_json_string(body, sizeof body, (unsigned long)at, name);
    if (at >= 0 && !blank(topic)) {
        at = ae_req_put_raw(body, sizeof body, (unsigned long)at, ",\"topic\":");
        if (at >= 0) at = ae_req_put_json_string(body, sizeof body, (unsigned long)at, topic);
    }
    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, ",\"visibility\":");
    if (at >= 0) at = ae_req_put_json_string(body, sizeof body, (unsigned long)at,
                                             blank(visibility) ? "public" : visibility);
    if (at >= 0 && !blank(kind)) {                       /* no kind: the hub makes a text room */
        at = ae_req_put_raw(body, sizeof body, (unsigned long)at, ",\"kind\":");
        if (at >= 0) at = ae_req_put_json_string(body, sizeof body, (unsigned long)at, kind);
    }
    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, "}");
    if (at < 0) return AE_CHAT_ERR_TOO_LONG;
    rc = call(c, "POST", AE_CHAT_BASE "/rooms", body, (unsigned long)at);
    if (rc == AE_CHAT_OK && id != 0 && id_cap > 0 &&
        json_extract_string(c->reply, "id", id, (unsigned long)id_cap) != 0) {
        return AE_CHAT_ERR_STATUS;                       /* a 200 without an id leaves nothing to join */
    }
    return rc;
}

int ae_chat_join(ae_chat *c, const char *room, char *name, int name_cap,
                 char *topic, int topic_cap, char *motd, int motd_cap, int *members)
{
    char path[PATH_MAX_LEN];
    int rc = make_path(path, sizeof path, AE_CHAT_BASE "/rooms/", room, "/join");
    int n = 0;

    if (rc != AE_CHAT_OK) return rc;
    rc = call(c, "POST", path, "{}", 2);
    if (rc != AE_CHAT_OK) return rc;
    if (name != 0) json_extract_string(c->reply, "name", name, (unsigned long)name_cap);
    if (topic != 0) json_extract_string(c->reply, "topic", topic, (unsigned long)topic_cap);
    if (motd != 0) json_extract_string(c->reply, "motd", motd, (unsigned long)motd_cap);
    if (members != 0) *members = (json_extract_bool(c->reply, "members", &n) == 0) ? n : 0;
    if (json_extract_bool(c->reply, "since", &n) == 0 && n > c->last_seq) c->last_seq = n;
    return AE_CHAT_OK;
}

int ae_chat_leave(ae_chat *c, const char *room)
{
    char path[PATH_MAX_LEN];
    int rc = make_path(path, sizeof path, AE_CHAT_BASE "/rooms/", room, "/leave");
    if (rc != AE_CHAT_OK) return rc;
    return call(c, "POST", path, "{}", 2);
}

static int history_at(ae_chat *c, const char *prefix, const char *id, long before, int limit)
{
    char path[PATH_MAX_LEN];
    char suffix[64];
    int rc;

    if (limit < 1) limit = 20;
    if (limit > 50) limit = 50;
    if (before > 0) sprintf(suffix, "/history.txt?before=%ld&limit=%d", before, limit);
    else sprintf(suffix, "/history.txt?limit=%d", limit);
    rc = make_path(path, sizeof path, prefix, id, suffix);
    if (rc != AE_CHAT_OK) return rc;
    return get_text(c, path);
}

int ae_chat_history(ae_chat *c, const char *room, long before, int limit)
{
    return history_at(c, AE_CHAT_BASE "/rooms/", room, before, limit);
}

int ae_chat_dm_history(ae_chat *c, const char *thread_id, long before, int limit)
{
    return history_at(c, AE_CHAT_BASE "/dm/", thread_id, before, limit);
}

/* {"content":..,"clientId":..[,"replyTo":..][,"threadId":..]} */
static int build_message(ae_chat *c, const char *text, const char *reply_to,
                         const char *thread_id, char *body, unsigned long size)
{
    char client_id[48];
    long at;

    if (blank(text)) return AE_CHAT_ERR_ARGS;
    c->send_counter++;
    sprintf(client_id, "%.8s-%ld", c->sid, c->send_counter);
    strcpy(c->client_id, client_id);
    at = ae_req_put_raw(body, size, 0, "{\"content\":");
    if (at >= 0) at = ae_req_put_json_string(body, size, (unsigned long)at, text);
    if (at >= 0) at = ae_req_put_raw(body, size, (unsigned long)at, ",\"clientId\":");
    if (at >= 0) at = ae_req_put_json_string(body, size, (unsigned long)at, client_id);
    if (at >= 0 && !blank(reply_to)) {
        at = ae_req_put_raw(body, size, (unsigned long)at, ",\"replyTo\":");
        if (at >= 0) at = ae_req_put_json_string(body, size, (unsigned long)at, reply_to);
    }
    if (at >= 0 && !blank(thread_id)) {
        at = ae_req_put_raw(body, size, (unsigned long)at, ",\"threadId\":");
        if (at >= 0) at = ae_req_put_json_string(body, size, (unsigned long)at, thread_id);
    }
    if (at >= 0) at = ae_req_put_raw(body, size, (unsigned long)at, "}");
    return at < 0 ? AE_CHAT_ERR_TOO_LONG : (int)at;
}

int ae_chat_send(ae_chat *c, const char *room, const char *text,
                 const char *reply_to, const char *thread_id)
{
    char path[PATH_MAX_LEN];
    char body[BODY_MAX + AE_CHAT_TEXT_MAX * 6];
    int len = make_path(path, sizeof path, AE_CHAT_BASE "/rooms/", room, "/messages");

    if (len != AE_CHAT_OK) return len;
    c->client_id[0] = '\0';
    c->sent_id[0] = '\0';
    len = build_message(c, text, reply_to, thread_id, body, sizeof body);
    if (len < 0) return len;
    len = call(c, "POST", path, body, (unsigned long)len);
    if (len == AE_CHAT_OK && json_extract_string(c->reply, "id", c->sent_id, sizeof c->sent_id) != 0) c->sent_id[0] = '\0';
    return len;
}

int ae_chat_poll(ae_chat *c, int wait)
{
    char path[96];
    int rc;

    if (wait < 0) wait = 0;
    if (wait > 25) wait = 25;
    sprintf(path, AE_CHAT_BASE "/events.txt?since=%ld&wait=%d", c->last_seq, wait);
    rc = call(c, "GET", path, 0, 0);
    if (rc == AE_CHAT_OK) skip_header(c, &c->last_seq);
    return rc;
}

int ae_chat_react(ae_chat *c, const char *msg_id, const char *emoji, int *count)
{
    char path[PATH_MAX_LEN];
    char body[128];
    long at;
    int rc = make_path(path, sizeof path, AE_CHAT_BASE "/messages/", msg_id, "/react");
    int n = 0;

    if (rc != AE_CHAT_OK) return rc;
    if (blank(emoji)) return AE_CHAT_ERR_ARGS;
    at = ae_req_put_raw(body, sizeof body, 0, "{\"emoji\":");
    if (at >= 0) at = ae_req_put_json_string(body, sizeof body, (unsigned long)at, emoji);
    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, "}");
    if (at < 0) return AE_CHAT_ERR_TOO_LONG;
    rc = call(c, "POST", path, body, (unsigned long)at);
    if (rc == AE_CHAT_OK && count != 0) *count = (json_extract_bool(c->reply, "count", &n) == 0) ? n : 0;
    return rc;
}

int ae_chat_unreact(ae_chat *c, const char *msg_id, const char *emoji, int *count)
{
    char path[PATH_MAX_LEN];
    char esc[64];
    int rc = make_path(path, sizeof path, AE_CHAT_BASE "/messages/", msg_id, "/react/");
    int n = 0;

    if (rc != AE_CHAT_OK) return rc;
    if (blank(emoji)) return AE_CHAT_ERR_ARGS;
    if (ae_req_url_escape(emoji, esc, sizeof esc) < 0) return AE_CHAT_ERR_TOO_LONG;
    if (strlen(path) + strlen(esc) + 1 > sizeof path) return AE_CHAT_ERR_TOO_LONG;
    strcat(path, esc);
    rc = call(c, "DELETE", path, 0, 0);
    if (rc == AE_CHAT_OK && count != 0) *count = (json_extract_bool(c->reply, "count", &n) == 0) ? n : 0;
    return rc;
}

int ae_chat_reactions(ae_chat *c, const char *msg_id)
{
    char path[PATH_MAX_LEN];
    int rc = make_path(path, sizeof path, AE_CHAT_BASE "/messages/", msg_id, "/reactions.txt");
    if (rc != AE_CHAT_OK) return rc;
    return get_text(c, path);
}

int ae_chat_files(ae_chat *c, const char *room)
{
    char path[PATH_MAX_LEN];
    int rc = make_path(path, sizeof path, AE_CHAT_BASE "/rooms/", room, "/files.txt");
    if (rc != AE_CHAT_OK) return rc;
    return get_text(c, path);
}

/* `"key":"value"` appended at `at`, a comma first unless it is the first field. -1 when it does not fit. */
int ae_chat_dm_group(ae_chat *c, const char *const *peers, int n, const char *text,
                     char *thread_id, int cap)
{
    char message[BODY_MAX + AE_CHAT_TEXT_MAX * 6];
    char body[BODY_MAX + AE_CHAT_TEXT_MAX * 6 + 512];
    long at;
    int i, len;

    if (peers == 0 || n < 1) return AE_CHAT_ERR_ARGS;
    len = build_message(c, text, 0, 0, message, sizeof message);
    if (len < 0) return len;
    at = ae_req_put_raw(body, sizeof body, 0, "{\"participants\":[");
    for (i = 0; i < n && at >= 0; i++) {
        if (blank(peers[i])) return AE_CHAT_ERR_ARGS;
        if (i > 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, ",");
        if (at >= 0) at = ae_req_put_json_string(body, sizeof body, (unsigned long)at, peers[i]);
    }
    /* the message object without its opening brace: ,"content":...,"clientId":...} */
    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, "],");
    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, message + 1);
    if (at < 0) return AE_CHAT_ERR_TOO_LONG;
    len = call(c, "POST", AE_CHAT_BASE "/dm-group", body, (unsigned long)at);
    if (len == AE_CHAT_OK && thread_id != 0)
        json_extract_string(c->reply, "threadId", thread_id, (unsigned long)cap);
    return len;
}

static long put_field(char *body, unsigned long size, long at, const char *key, const char *value)
{
    char lead[40];

    if (at < 0 || strlen(key) > 30) return -1;
    sprintf(lead, "%s\"%s\":", at <= 1 ? "" : ",", key);
    at = ae_req_put_raw(body, size, (unsigned long)at, lead);
    if (at >= 0) at = ae_req_put_json_string(body, size, (unsigned long)at, value);
    return at;
}

int ae_chat_edit(ae_chat *c, const char *msg_id, const char *text)
{
    char path[PATH_MAX_LEN];
    char body[BODY_MAX + AE_CHAT_TEXT_MAX * 6];
    long at;
    int rc = make_path(path, sizeof path, AE_CHAT_BASE "/messages/", msg_id, "");

    if (rc != AE_CHAT_OK) return rc;
    if (blank(text)) return AE_CHAT_ERR_ARGS;
    at = put_field(body, sizeof body, ae_req_put_raw(body, sizeof body, 0, "{"), "content", text);
    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, "}");
    if (at < 0) return AE_CHAT_ERR_TOO_LONG;
    return call(c, "PUT", path, body, (unsigned long)at);
}

int ae_chat_delete(ae_chat *c, const char *msg_id)
{
    char path[PATH_MAX_LEN];
    int rc = make_path(path, sizeof path, AE_CHAT_BASE "/messages/", msg_id, "");

    if (rc != AE_CHAT_OK) return rc;
    return call(c, "DELETE", path, 0, 0);
}

int ae_chat_topic(ae_chat *c, const char *room, const char *topic)
{
    char path[PATH_MAX_LEN];
    char body[BODY_MAX];
    long at;
    int rc = make_path(path, sizeof path, AE_CHAT_BASE "/rooms/", room, "/topic");

    if (rc != AE_CHAT_OK) return rc;
    at = put_field(body, sizeof body, ae_req_put_raw(body, sizeof body, 0, "{"), "topic", topic ? topic : "");
    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, "}");
    if (at < 0) return AE_CHAT_ERR_TOO_LONG;
    return call(c, "PUT", path, body, (unsigned long)at);
}

int ae_chat_room_motd(ae_chat *c, const char *room, const char *motd)
{
    char path[PATH_MAX_LEN];
    char body[BODY_MAX];
    long at;
    int rc = make_path(path, sizeof path, AE_CHAT_BASE "/rooms/", room, "/motd");

    if (rc != AE_CHAT_OK) return rc;
    at = put_field(body, sizeof body, ae_req_put_raw(body, sizeof body, 0, "{"), "motd", motd ? motd : "");
    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, "}");
    if (at < 0) return AE_CHAT_ERR_TOO_LONG;
    return call(c, "PUT", path, body, (unsigned long)at);
}

int ae_chat_typing(ae_chat *c, const char *room, const char *text)
{
    char path[PATH_MAX_LEN];
    char body[BODY_MAX];
    long at;
    int rc = make_path(path, sizeof path, AE_CHAT_BASE "/rooms/", room, "/typing");

    if (rc != AE_CHAT_OK) return rc;
    at = put_field(body, sizeof body, ae_req_put_raw(body, sizeof body, 0, "{"), "text", text ? text : "");
    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, "}");
    if (at < 0) return AE_CHAT_ERR_TOO_LONG;
    return call(c, "POST", path, body, (unsigned long)at);
}

int ae_chat_canvas_get(ae_chat *c, const char *room, int from, int count)
{
    char path[PATH_MAX_LEN];
    char suffix[64];

    sprintf(suffix, "/canvas.txt?from=%d&count=%d&maxBytes=7680", from, count);
    if (make_path(path, sizeof path, AE_CHAT_BASE "/rooms/", room, suffix) != AE_CHAT_OK) return AE_CHAT_ERR_ARGS;
    return call(c, "GET", path, 0, 0);   /* the reply stays whole: the CANVAS header is the caller's to parse */
}

int ae_chat_canvas_post(ae_chat *c, const char *room, long base, const char *ops, int len, long *rev)
{
    char path[PATH_MAX_LEN];
    char body[BODY_MAX + 2 * 512];
    char text[520];
    char lead[48];
    long at;
    int rc = make_path(path, sizeof path, AE_CHAT_BASE "/rooms/", room, "/canvas");

    if (rc != AE_CHAT_OK) return rc;
    if (len < 1 || len >= (int)sizeof text) return AE_CHAT_ERR_ARGS;
    memcpy(text, ops, (size_t)len);   /* the ops are a (pointer, length): the escaper wants a C string */
    text[len] = '\0';
    sprintf(lead, "{\"base\":%ld,\"ops\":", base);
    at = ae_req_put_raw(body, sizeof body, 0, lead);
    if (at >= 0) at = ae_req_put_json_string(body, sizeof body, (unsigned long)at, text);
    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, "}");
    if (at < 0) return AE_CHAT_ERR_TOO_LONG;
    rc = call(c, "POST", path, body, (unsigned long)at);
    if (rc == AE_CHAT_OK && rev != 0) {
        const char *p = strstr(c->reply, "\"rev\":");
        *rev = p != 0 ? atol(p + 6) : 0;
    }
    return rc;
}

int ae_chat_mode(ae_chat *c, const char *room, const char *modes, const char *params,
                 char *applied, int applied_cap, char *errors, int errors_cap)
{
    char path[PATH_MAX_LEN];
    char body[BODY_MAX];
    long at;
    int rc = make_path(path, sizeof path, AE_CHAT_BASE "/rooms/", room, "/mode");

    if (applied != 0 && applied_cap > 0) applied[0] = '\0';
    if (errors != 0 && errors_cap > 0) errors[0] = '\0';
    if (rc != AE_CHAT_OK) return rc;
    at = put_field(body, sizeof body, ae_req_put_raw(body, sizeof body, 0, "{"), "modes", modes ? modes : "");
    if (at >= 0) at = put_field(body, sizeof body, at, "params", params ? params : "");
    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, "}");
    if (at < 0) return AE_CHAT_ERR_TOO_LONG;
    rc = call(c, "POST", path, body, (unsigned long)at);
    if (rc == AE_CHAT_OK) {
        if (applied != 0 && applied_cap > 0 && json_extract_string(c->reply, "applied", applied, (unsigned long)applied_cap) != 0) applied[0] = '\0';
        if (errors != 0 && errors_cap > 0 && json_extract_string(c->reply, "errors", errors, (unsigned long)errors_cap) != 0) errors[0] = '\0';
    }
    return rc;
}

int ae_chat_moderate(ae_chat *c, const char *room, const char *action, const char *user,
                     const char *reason, int minutes)
{
    static const char *const actions[] = { "kick", "ban", "unban", "mute", "unmute", "op", "deop" };
    char suffix[16];
    char path[PATH_MAX_LEN];
    char body[BODY_MAX];
    char num[16];
    long at;
    int rc, i, known = 0;

    for (i = 0; i < (int)(sizeof actions / sizeof actions[0]); i++)
        if (action != 0 && strcmp(action, actions[i]) == 0) known = 1;
    if (!known || blank(user)) return AE_CHAT_ERR_ARGS;
    sprintf(suffix, "/%s", action);
    rc = make_path(path, sizeof path, AE_CHAT_BASE "/rooms/", room, suffix);
    if (rc != AE_CHAT_OK) return rc;
    at = put_field(body, sizeof body, ae_req_put_raw(body, sizeof body, 0, "{"), "user", user);
    if (!blank(reason)) at = put_field(body, sizeof body, at, "reason", reason);
    if (minutes > 0 && at >= 0) {
        sprintf(num, ",\"minutes\":%d", minutes);
        at = ae_req_put_raw(body, sizeof body, (unsigned long)at, num);
    }
    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, "}");
    if (at < 0) return AE_CHAT_ERR_TOO_LONG;
    return call(c, "POST", path, body, (unsigned long)at);
}

int ae_chat_invite(ae_chat *c, const char *room, const char *user, int add)
{
    char path[PATH_MAX_LEN];
    char body[BODY_MAX];
    char esc[96];
    long at;
    int rc = make_path(path, sizeof path, AE_CHAT_BASE "/rooms/", room, "/invites");

    if (rc != AE_CHAT_OK) return rc;
    if (blank(user)) return AE_CHAT_ERR_ARGS;
    if (!add) {
        if (ae_req_url_escape(user, esc, sizeof esc) < 0) return AE_CHAT_ERR_TOO_LONG;
        if (strlen(path) + strlen(esc) + 2 > sizeof path) return AE_CHAT_ERR_TOO_LONG;
        strcat(path, "/");
        strcat(path, esc);
        return call(c, "DELETE", path, 0, 0);
    }
    at = put_field(body, sizeof body, ae_req_put_raw(body, sizeof body, 0, "{"), "user", user);
    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, "}");
    if (at < 0) return AE_CHAT_ERR_TOO_LONG;
    return call(c, "POST", path, body, (unsigned long)at);
}

int ae_chat_thread(ae_chat *c, const char *msg_id, const char *text)
{
    char path[PATH_MAX_LEN];
    char body[BODY_MAX + AE_CHAT_TEXT_MAX * 6];
    int len = make_path(path, sizeof path, AE_CHAT_BASE "/messages/", msg_id, "/thread");

    if (len != AE_CHAT_OK) return len;
    len = build_message(c, text, 0, 0, body, sizeof body);
    if (len < 0) return len;
    return call(c, "POST", path, body, (unsigned long)len);
}

int ae_chat_thread_list(ae_chat *c, const char *msg_id, int limit)
{
    char path[PATH_MAX_LEN];
    char suffix[32];
    int rc;

    if (limit < 1) limit = 20;
    sprintf(suffix, ".txt?limit=%d", limit);
    rc = make_path(path, sizeof path, AE_CHAT_BASE "/threads/", msg_id, suffix);
    if (rc != AE_CHAT_OK) return rc;
    return get_text(c, path);
}

/* ------------------------------------------------- Phase A: pins, search, emoji, motd, rooms */

/* The hub answers pins, search and emoji as JSON arrays only (router.ts: no .txt variants), so the reply
 * is rewritten in place as pipe rows and read with ae_chat_next_row like every list. `keys` name the
 * object fields, in column order; a number is written as digits, a missing field as "". A '|' in a value
 * becomes '!' and a line break a space, as the text routes do. An object cut off by the reply buffer
 * (long emoji data URLs) ends the list. */
#define ROW_OBJ_MAX 1536

static char rows_out[AE_CHAT_REPLY_MAX];

static void json_to_rows(ae_chat *c, const char *const *keys, int nkeys)
{
    unsigned long at = 0, out = 0;
    char obj[ROW_OBJ_MAX];
    const char *p = strchr(c->reply, '[');

    if (p != 0) at = (unsigned long)(p - c->reply) + 1;
    else at = c->reply_len;
    while (at < c->reply_len) {
        unsigned long start, n;
        int depth = 0, in_str = 0, i;
        const char *q = c->reply + at;
        char *o;

        while (*q != '\0' && *q != '{') q++;
        if (*q == '\0') break;
        start = (unsigned long)(q - c->reply);
        for (n = start; n < c->reply_len; n++) {
            char ch = c->reply[n];
            if (in_str) { if (ch == '\\') n++; else if (ch == '"') in_str = 0; continue; }
            if (ch == '"') in_str = 1;
            else if (ch == '{') depth++;
            else if (ch == '}' && --depth == 0) break;
        }
        if (n >= c->reply_len) break;               /* cut off */
        {
            unsigned long len = n - start + 1;
            if (len >= sizeof obj) len = sizeof obj - 1;
            memcpy(obj, c->reply + start, len);
            obj[len] = '\0';
        }
        at = n + 1;
        for (i = 0; i < nkeys; i++) {
            char val[AE_CHAT_TEXT_MAX + 1];
            int num;
            val[0] = '\0';
            if (json_extract_string(obj, keys[i], val, sizeof val) != 0 || val[0] == '\0') {
                if (json_extract_bool(obj, keys[i], &num) == 0) sprintf(val, "%d", num);
                else val[0] = '\0';
            }
            for (o = val; *o != '\0'; o++) {
                if (*o == '|') *o = '!';
                else if (*o == '\n' || *o == '\r') *o = ' ';
            }
            if (out + strlen(val) + 2 >= sizeof rows_out) { i = nkeys; at = c->reply_len; break; }
            memcpy(rows_out + out, val, strlen(val));
            out += strlen(val);
            rows_out[out++] = i + 1 < nkeys ? '|' : '\n';
        }
    }
    memcpy(c->reply, rows_out, out);
    c->reply[out] = '\0';
    c->reply_len = out;
    c->cursor = 0;
}

int ae_chat_pins(ae_chat *c, const char *room)
{
    static const char *const keys[] = { "messageId", "user", "content", "pinnedBy", "at", "messageAt" };
    char path[PATH_MAX_LEN];
    int rc = make_path(path, sizeof path, AE_CHAT_BASE "/rooms/", room, "/pins");

    if (rc != AE_CHAT_OK) return rc;
    rc = call(c, "GET", path, 0, 0);
    if (rc == AE_CHAT_OK) json_to_rows(c, keys, 6);
    return rc;
}

int ae_chat_pin(ae_chat *c, const char *room, const char *msg_id, int pin)
{
    char path[PATH_MAX_LEN];
    char body[BODY_MAX];
    long at;
    int rc;

    if (blank(msg_id)) return AE_CHAT_ERR_ARGS;
    if (!pin) {
        char tail[200];
        char esc[160];
        if (ae_req_url_escape(msg_id, esc, sizeof esc) < 0) return AE_CHAT_ERR_TOO_LONG;
        sprintf(tail, "/pins/%s", esc);
        rc = make_path(path, sizeof path, AE_CHAT_BASE "/rooms/", room, tail);
        if (rc != AE_CHAT_OK) return rc;
        return call(c, "DELETE", path, 0, 0);
    }
    rc = make_path(path, sizeof path, AE_CHAT_BASE "/rooms/", room, "/pins");
    if (rc != AE_CHAT_OK) return rc;
    at = put_field(body, sizeof body, ae_req_put_raw(body, sizeof body, 0, "{"), "messageId", msg_id);
    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, "}");
    if (at < 0) return AE_CHAT_ERR_TOO_LONG;
    return call(c, "POST", path, body, (unsigned long)at);
}

int ae_chat_search(ae_chat *c, const char *query, const char *room, const char *user, int limit)
{
    static const char *const keys[] = { "id", "room", "user", "at", "content" };
    char path[PATH_MAX_LEN];
    char esc[160];
    char tail[40];
    int rc;

    if (blank(query)) return AE_CHAT_ERR_ARGS;
    if (limit < 1) limit = 20;
    if (ae_req_url_escape(query, esc, sizeof esc) < 0) return AE_CHAT_ERR_TOO_LONG;
    sprintf(tail, "&limit=%d", limit);
    if (strlen(AE_CHAT_BASE "/search?q=") + strlen(esc) + strlen(tail) + 1 >= sizeof path) return AE_CHAT_ERR_TOO_LONG;
    sprintf(path, "%s%s%s", AE_CHAT_BASE "/search?q=", esc, tail);
    if (!blank(room)) {
        char resc[160];
        if (ae_req_url_escape(room, resc, sizeof resc) < 0 || strlen(path) + strlen(resc) + 8 >= sizeof path)
            return AE_CHAT_ERR_TOO_LONG;
        sprintf(path + strlen(path), "&room=%s", resc);
    }
    if (!blank(user)) {
        char uesc[160];
        if (ae_req_url_escape(user, uesc, sizeof uesc) < 0 || strlen(path) + strlen(uesc) + 8 >= sizeof path)
            return AE_CHAT_ERR_TOO_LONG;
        sprintf(path + strlen(path), "&user=%s", uesc);
    }
    rc = call(c, "GET", path, 0, 0);
    if (rc == AE_CHAT_OK) json_to_rows(c, keys, 5);
    return rc;
}

int ae_chat_emoji_list(ae_chat *c)
{
    return get_text(c, AE_CHAT_BASE "/emoji.txt");
}

int ae_chat_emoji_add(ae_chat *c, const char *name, const char *display, const char *ascii)
{
    char path[PATH_MAX_LEN], body[BODY_MAX];
    int rc = make_path(path, sizeof path, AE_CHAT_BASE "/emoji/", name, "");
    long at;

    if (rc != AE_CHAT_OK) return rc;
    if (blank(display) || blank(ascii)) return AE_CHAT_ERR_ARGS;
    at = put_field(body, sizeof body, ae_req_put_raw(body, sizeof body, 0, "{"), "display", display);
    at = put_field(body, sizeof body, at, "ascii", ascii);
    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, "}");
    if (at < 0) return AE_CHAT_ERR_TOO_LONG;
    return call(c, "PUT", path, body, (unsigned long)at);
}

int ae_chat_emoji_remove(ae_chat *c, const char *name)
{
    char path[PATH_MAX_LEN];
    int rc = make_path(path, sizeof path, AE_CHAT_BASE "/emoji/", name, "");

    if (rc != AE_CHAT_OK) return rc;
    return call(c, "DELETE", path, 0, 0);
}

int ae_chat_motd(ae_chat *c, char *text, int cap)
{
    int rc = call(c, "GET", AE_CHAT_BASE "/motd", 0, 0);

    if (rc == AE_CHAT_OK && text != 0 && cap > 0 && json_extract_string(c->reply, "text", text, (unsigned long)cap) != 0)
        text[0] = '\0';
    return rc;
}

int ae_chat_motd_set(ae_chat *c, const char *text)
{
    char body[BODY_MAX + AE_CHAT_TEXT_MAX * 6];
    long at = put_field(body, sizeof body, ae_req_put_raw(body, sizeof body, 0, "{"), "text", text ? text : "");

    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, "}");
    if (at < 0) return AE_CHAT_ERR_TOO_LONG;
    return call(c, "PUT", AE_CHAT_BASE "/motd", body, (unsigned long)at);
}

static int discord_reply(ae_chat *c, int rc, int *on)
{
    int v;
    if (rc != AE_CHAT_OK) return rc;
    if (json_extract_bool(c->reply, "on", &v) != 0) return AE_CHAT_ERR_BODY;
    if (on != 0) *on = v != 0;
    return AE_CHAT_OK;
}

int ae_chat_discord(ae_chat *c, int *on)
{
    return discord_reply(c, call(c, "GET", AE_CHAT_BASE "/discord", 0, 0), on);
}

int ae_chat_discord_set(ae_chat *c, int on, int *now)
{
    char body[BODY_MAX];
    long at = ae_req_put_raw(body, sizeof body, 0, on ? "{\"on\":true}" : "{\"on\":false}");

    if (at < 0) return AE_CHAT_ERR_TOO_LONG;
    return discord_reply(c, call(c, "PUT", AE_CHAT_BASE "/discord", body, (unsigned long)at), now);
}

int ae_chat_room_delete(ae_chat *c, const char *room)
{
    char path[PATH_MAX_LEN];
    int rc = make_path(path, sizeof path, AE_CHAT_BASE "/rooms/", room, "");

    if (rc != AE_CHAT_OK) return rc;
    return call(c, "DELETE", path, 0, 0);
}

int ae_chat_room_clear(ae_chat *c, const char *room)
{
    char path[PATH_MAX_LEN];
    int rc = make_path(path, sizeof path, AE_CHAT_BASE "/rooms/", room, "/clear");

    if (rc != AE_CHAT_OK) return rc;
    return call(c, "POST", path, "{}", 2);
}

int ae_chat_dm(ae_chat *c, const char *peer, const char *text, char *thread_id, int cap)
{
    char path[PATH_MAX_LEN];
    char body[BODY_MAX + AE_CHAT_TEXT_MAX * 6];
    int len = make_path(path, sizeof path, AE_CHAT_BASE "/dm/", peer, "");

    if (len != AE_CHAT_OK) return len;
    len = build_message(c, text, 0, 0, body, sizeof body);
    if (len < 0) return len;
    len = call(c, "POST", path, body, (unsigned long)len);
    if (len == AE_CHAT_OK && thread_id != 0) {
        json_extract_string(c->reply, "threadId", thread_id, (unsigned long)cap);
    }
    return len;
}

int ae_chat_dm_list(ae_chat *c)
{
    return get_text(c, AE_CHAT_BASE "/dm.txt");
}

int ae_chat_who(ae_chat *c)
{
    return get_text(c, AE_CHAT_BASE "/who.txt");
}

int ae_chat_members(ae_chat *c, const char *room)
{
    char path[PATH_MAX_LEN];
    int rc = make_path(path, sizeof path, AE_CHAT_BASE "/rooms/", room, "/members.txt");
    if (rc != AE_CHAT_OK) return rc;
    return get_text(c, path);
}

int ae_chat_presence(ae_chat *c, const char *status, const char *text)
{
    static const char *const known[] = { "online", "away", "busy", "dnd" };
    char body[BODY_MAX];
    long at;
    int i, ok = 0;

    for (i = 0; i < 4; i++) if (status != 0 && strcmp(status, known[i]) == 0) ok = 1;
    if (!ok) return AE_CHAT_ERR_ARGS;
    at = put_field(body, sizeof body, ae_req_put_raw(body, sizeof body, 0, "{"), "status", status);
    if (!blank(text) && at >= 0) at = put_field(body, sizeof body, at, "text", text);
    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, "}");
    if (at < 0) return AE_CHAT_ERR_TOO_LONG;
    return call(c, "PUT", AE_CHAT_BASE "/presence", body, (unsigned long)at);
}

int ae_chat_presence_clear(ae_chat *c)
{
    return call(c, "DELETE", AE_CHAT_BASE "/presence", 0, 0);
}

int ae_chat_profile(ae_chat *c, const char *user)
{
    char path[PATH_MAX_LEN];
    int rc = make_path(path, sizeof path, AE_CHAT_BASE "/users/", user, "/profile.txt");

    if (rc != AE_CHAT_OK) return rc;
    return get_text(c, path);
}

int ae_chat_ignores(ae_chat *c)
{
    return get_text(c, AE_CHAT_BASE "/ignores.txt");
}

int ae_chat_ignore(ae_chat *c, const char *user, int add)
{
    char path[PATH_MAX_LEN];
    char body[BODY_MAX];
    long at;
    int rc;

    if (blank(user)) return AE_CHAT_ERR_ARGS;
    if (!add) {
        rc = make_path(path, sizeof path, AE_CHAT_BASE "/ignores/", user, "");
        if (rc != AE_CHAT_OK) return rc;
        return call(c, "DELETE", path, 0, 0);
    }
    at = put_field(body, sizeof body, ae_req_put_raw(body, sizeof body, 0, "{"), "user", user);
    if (at >= 0) at = ae_req_put_raw(body, sizeof body, (unsigned long)at, "}");
    if (at < 0) return AE_CHAT_ERR_TOO_LONG;
    return call(c, "POST", AE_CHAT_BASE "/ignores", body, (unsigned long)at);
}

/* ------------------------------------------------------- picture previews */

/* The binary body goes to the caller's buffer; the same bytes also pass
 * through collect() so an error reply (a small JSON body) reaches c->error
 * exactly as for every other call. A reopened session starts a new attempt:
 * c->calls changes, so the buffer is rewound. */
typedef struct {
    ae_chat *c;
    unsigned char *buf;
    unsigned long cap;
    unsigned long len;
    int overflow;
    int attempt;
    int cols, rows;
} preview_ctx;

static void preview_rewind(preview_ctx *p)
{
    if (p->attempt != p->c->calls) {
        p->attempt = p->c->calls;
        p->len = 0;
        p->overflow = 0;
        p->cols = p->rows = 0;
    }
}

static int preview_sink(void *ctx, const unsigned char *buf, unsigned long len)
{
    preview_ctx *p = (preview_ctx *)ctx;

    preview_rewind(p);
    if (len > p->cap - p->len) {
        p->overflow = 1;
    } else {
        memcpy(p->buf + p->len, buf, len);
        p->len += len;
    }
    return collect(p->c, buf, len);
}

static int preview_header(void *ctx, const char *name, const char *value)
{
    preview_ctx *p = (preview_ctx *)ctx;
    int w = 0, h = 0;
    const char *s = value;
    unsigned long i;
    static const char want[] = "x-cells";

    preview_rewind(p);
    if (strlen(name) != sizeof want - 1) return 0;
    for (i = 0; i < sizeof want - 1; i++) {
        char ch = name[i];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        if (ch != want[i]) return 0;
    }
    while (*s >= '0' && *s <= '9') w = w * 10 + (*s++ - '0');
    if (*s != 'x') return 0;
    s++;
    while (*s >= '0' && *s <= '9') h = h * 10 + (*s++ - '0');
    p->cols = w;
    p->rows = h;
    return 0;
}

int ae_chat_file_preview(ae_chat *c, const char *file_id, int cols, int rows, int rgb,
                         unsigned char *buf, unsigned long cap, int *out_cols, int *out_rows)
{
    char path[PATH_MAX_LEN];
    char query[32];
    char suffix[48];
    preview_ctx p;
    int rc;

    if (c == 0 || buf == 0 || cap == 0) return AE_CHAT_ERR_ARGS;
    if (cap > AE_CHAT_PREVIEW_MAX) cap = AE_CHAT_PREVIEW_MAX;
    query[0] = '\0';
    /* A PNG is asked for as q=pal: the hub's 255-colour indexed picture at exactly cols*8 x rows*16, which
     * DCTelnet draws 1:1 in its own colours (no area-averaging, no 6x6x6 cube dither). */
    if (cols > 0 && rows > 0) sprintf(query, rgb ? "?cols=%d&rows=%d" : "?cols=%d&rows=%d&q=pal", cols, rows);
    else if (!rgb) strcpy(query, "?q=pal");
    sprintf(suffix, "/%s%s", rgb ? "preview.rgb" : "preview.png", query);
    rc = make_path(path, sizeof path, AE_CHAT_BASE "/files/", file_id, suffix);
    if (rc != AE_CHAT_OK) return rc;

    memset(&p, 0, sizeof p);
    p.c = c;
    p.buf = buf;
    p.cap = cap;
    rc = call_x(c, "GET", path, 0, 0, preview_sink, &p, preview_header);
    if (rc == AE_CHAT_ERR_STATUS) {
        if (c->status == 403 && strcmp(c->error, "not_member") == 0) return AE_CHAT_ERR_NOT_MEMBER;
        if (c->status == 415 && strcmp(c->error, "not_image") == 0) return AE_CHAT_ERR_NOT_IMAGE;
        if (c->status == 503 && strcmp(c->error, "files_disabled") == 0) return AE_CHAT_ERR_FILES_DISABLED;
        return rc;
    }
    if (rc != AE_CHAT_OK) return rc;
    if (p.overflow) return AE_CHAT_ERR_PREVIEW_TOO_BIG;
    if (p.len == 0) return AE_CHAT_ERR_BODY;
    if (out_cols != 0) *out_cols = p.cols;
    if (out_rows != 0) *out_rows = p.rows;
    return (int)p.len;
}
