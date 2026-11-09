/*
 * ae_http - a streaming HTTP/1.1 client for C doors.
 *
 * Lifted from examples/doorrepo-c/http.c on 2026-09-07 (backlog 4.11), where
 * it fetched a 620 KB catalogue and 130 KB archives through bsdsocket.library
 * on a 7 MHz 68000. What changed in the lift: the target is a struct rather
 * than DoorRepo's config, the User-Agent is the target's, and the response
 * headers a door cares about reach it through a callback rather than three
 * DoorRepo-shaped fields. Everything else - the framing, the bounds, the
 * error codes and their values - is the same code.
 *
 * What it does: one request, one response, streamed to a sink in bounded
 * chunks. Connection: close on every request, so a body without a
 * Content-Length ends at EOF. What it does not do: TLS, redirects, chunked
 * transfer decoding (rejected, see AE_HTTP_ERR_CHUNKED), keep-alive.
 *
 * Platform-neutral C89: this file and ae_http.c must never contain
 * "#ifdef AMIGA" - all socket access goes through ae_net.h.
 */
#ifndef AE_HTTP_H
#define AE_HTTP_H

#ifdef __cplusplus
extern "C" {
#endif

#define AE_HTTP_OK                     0
#define AE_HTTP_ERR_ARGS               -1  /* NULL target/path/resp/sink */
#define AE_HTTP_ERR_CONNECT            -2  /* ae_net_open() failed; see ae_net_last_error() */
#define AE_HTTP_ERR_WRITE              -3  /* could not send the request */
#define AE_HTTP_ERR_READ               -4  /* socket read failed or EOF mid-response */
#define AE_HTTP_ERR_STATUS_LINE        -5  /* status line missing/malformed/too long */
#define AE_HTTP_ERR_HEADER_TOO_LONG    -6  /* a header line exceeded the fixed line buffer */
#define AE_HTTP_ERR_CHUNKED            -7  /* Transfer-Encoding: chunked - not decoded, rejected rather than mis-parsed */
#define AE_HTTP_ERR_LENGTH_MISMATCH    -8  /* Content-Length promised more bytes than the server sent */
#define AE_HTTP_ERR_SINK_ABORT         -9  /* sink() returned non-zero */
#define AE_HTTP_ERR_REQUEST_TOO_LONG   -10 /* host/path/headers did not fit the fixed request buffer */
#define AE_HTTP_ERR_HEADER_ABORT       -11 /* on_header() returned non-zero */

/* The User-Agent sent when a target names none. */
#define AE_HTTP_USER_AGENT "AmiExpress-C-Door/1.0"

/* Where a request goes. The strings are borrowed for the call only. */
typedef struct {
    const char *host;        /* "bbs.uprough.net"; never NULL */
    int port;                /* 443 (tls) or 80 (plain) gets no ":port" suffix in the Host header */
    int timeout_secs;        /* bounds the connect and every read */
    const char *user_agent;  /* NULL -> AE_HTTP_USER_AGENT */
    int tls;                 /* nonzero (last, so positional initialisers stay valid; 0 = plain): HTTPS through ae_net_open_tls(); the certificate is always verified */
} ae_http_target;

typedef struct {
    int status;                    /* HTTP status code, e.g. 200, 404 */
    unsigned long content_length;  /* only meaningful when have_content_length != 0 */
    int have_content_length;       /* 1 if a Content-Length header was present and sane */
} ae_http_response;

/* Receives the body in order, in bounded pieces. Return non-zero to stop:
 * the request ends with AE_HTTP_ERR_SINK_ABORT and nothing more is read. */
typedef int (*ae_http_sink_fn)(void *ctx, const unsigned char *buf, unsigned long len);

/* Sees every response header as "name" and "value" (leading and trailing
 * blanks removed, name as the server spelt it - compare case-insensitively).
 * Both strings are valid only during the call. Return non-zero to stop:
 * the request ends with AE_HTTP_ERR_HEADER_ABORT. */
typedef int (*ae_http_header_fn)(void *ctx, const char *name, const char *value);

/* GET path_and_query from the target and stream the body to sink. Returns
 * AE_HTTP_OK with resp filled, or one of the AE_HTTP_ERR_* codes. A non-2xx
 * status is NOT an error here: the body is still streamed and resp->status
 * says what the server said. */
int ae_http_get(const ae_http_target *target, const char *path_and_query,
                ae_http_response *resp, ae_http_sink_fn sink, void *ctx);

/* The general form. `method` is sent verbatim. A non-NULL body is written
 * after the headers with a Content-Length line (body_len may be 0);
 * extra_headers are complete "Name: value\r\n" lines, sent as given.
 * on_header may be NULL. The one `ctx` goes to both callbacks. */
int ae_http_request(const ae_http_target *target, const char *method,
                    const char *path_and_query,
                    const char *body, unsigned long body_len,
                    const char * const *extra_headers, int extra_header_count,
                    ae_http_response *resp,
                    ae_http_sink_fn sink, void *ctx,
                    ae_http_header_fn on_header);

#ifdef __cplusplus
}
#endif

#endif /* AE_HTTP_H */
