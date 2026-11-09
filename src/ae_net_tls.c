/* ae_net_tls.c - TLS for ae_net: OpenSSL 3 natively, AmiSSL 5 on the Amiga.
 * Split out of ae_net.c so a door that never calls ae_net_open_tls() links none
 * of it (vbcc has no gc-sections; the linker takes whole objects). ae_net.c has
 * no reference into this file: ae_net_open_tls() registers a small ops table
 * (ae_net_internal.h) on first use, and ae_net_read/write/close dispatch
 * through it. Design: thoughts/shared/plans/2026-11-08-livechat68-tls.md, point 3.
 *
 * EXEMPTION: ae_net.c is the only network file allowed "#ifdef AMIGA"; this file
 * is the one exception, because AmiSSL's library open/close is Amiga-only and
 * is not part of the socket layer. It contains no socket code of its own.
 */
#if !defined(AE_NET_STUB)
/* ---- TLS, shared by the Amiga and POSIX branches --------------------------
 *
 * ae_net_open_tls() opens the plain socket with ae_net_open(), switches it to
 * non-blocking, and handshakes over it. The descriptor it returns is the
 * socket itself: a small slot table keyed by that descriptor remembers which
 * ones carry an SSL session, and ae_net_read/write/close consult it first.
 * Every wait is bounded by the open's timeout (wait_fd), as in the plain path.
 *
 * The certificate chain is always verified (SSL_VERIFY_PEER, the system trust
 * store) and the name is always checked against the host the caller asked
 * for. There is no switch to turn either off.
 */
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "ae_net.h"
#include "ae_net_internal.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>

#ifdef AMIGA
#include <exec/types.h>
#include <proto/exec.h>
#include <utility/tagitem.h>
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <proto/dos.h>

struct Library *AmiSSLMasterBase = (struct Library *) 0;
struct Library *AmiSSLBase = (struct Library *) 0;
struct Library *AmiSSLExtBase = (struct Library *) 0;
static int g_amissl_open = 0;
extern struct Library *SocketBase; /* ae_net.c, opened by the first ae_net_open() */

/* atexit order is the reverse of registration: ae_net_open() registered the
 * socket library's close first, so this runs before it, which is the order
 * AmiSSL needs (AmiSSL, then the master, then the sockets). */
static void amissl_cleanup(void)
{
    if (g_amissl_open) { CloseAmiSSL(); g_amissl_open = 0; }
    if (AmiSSLMasterBase != (struct Library *) 0) {
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = (struct Library *) 0;
    }
}

static int tls_lib_open(void)
{
    LONG err;
    struct DateStamp ds;
    unsigned long seed[4];

    if (g_amissl_open) return 0;

    AmiSSLMasterBase = OpenLibrary((STRPTR) "amisslmaster.library", AMISSLMASTER_MIN_VERSION);
    if (AmiSSLMasterBase == (struct Library *) 0) {
        ae_net_set_error("AmiSSL 5 is not installed (amisslmaster.library)");
        return -1;
    }
    err = OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
                         AmiSSL_UsesOpenSSLStructs, TRUE,
                         AmiSSL_GetAmiSSLBase, &AmiSSLBase,
                         AmiSSL_GetAmiSSLExtBase, &AmiSSLExtBase,
                         AmiSSL_SocketBase, SocketBase,
                         AmiSSL_ErrNoPtr, &errno,
                         TAG_DONE);
    if (err != 0) {
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = (struct Library *) 0;
        ae_net_set_error(err == 1 ? "AmiSSL 5: amisslmaster.library would not initialise"
                : err == 2 ? "AmiSSL 5: the AmiSSL library would not open"
                :            "AmiSSL 5: initialisation failed");
        return -1;
    }
    g_amissl_open = 1;
    atexit(amissl_cleanup);

    OPENSSL_init_ssl(OPENSSL_INIT_SSL_DEFAULT | OPENSSL_INIT_ADD_ALL_CIPHERS
                     | OPENSSL_INIT_ADD_ALL_DIGESTS, NULL);
    DateStamp(&ds);
    seed[0] = (unsigned long) ds.ds_Days;
    seed[1] = (unsigned long) ds.ds_Minute;
    seed[2] = (unsigned long) ds.ds_Tick;
    seed[3] = (unsigned long) &ds;
    RAND_seed(seed, (int) sizeof(seed));
    return 0;
}
#else
static int tls_lib_open(void)
{
    OPENSSL_init_ssl(0, NULL);
    return 0;
}
#endif

#define TLS_SLOTS 8

struct tls_slot {
    int used;
    int fd;
    SSL *ssl;
    SSL_CTX *ctx;
};

static struct tls_slot g_tls[TLS_SLOTS];

static struct tls_slot *tls_find(int s)
{
    int i;
    for (i = 0; i < TLS_SLOTS; i++) {
        if (g_tls[i].used && g_tls[i].fd == s) return &g_tls[i];
    }
    return (struct tls_slot *) 0;
}

static int tls_is(int s)
{
    return tls_find(s) != (struct tls_slot *) 0;
}

static void tls_free(struct tls_slot *t)
{
    if (t->ssl != NULL) SSL_free(t->ssl);
    if (t->ctx != NULL) SSL_CTX_free(t->ctx);
    t->ssl = NULL;
    t->ctx = NULL;
    t->used = 0;
}

/* "prefix: detail" into the error buffer. */
static void set_error2(const char *prefix, const char *detail)
{
    char buf[128];
    size_t a = strlen(prefix);
    size_t b = strlen(detail);

    if (a > sizeof(buf) - 1) a = sizeof(buf) - 1;
    memcpy(buf, prefix, a);
    if (b > sizeof(buf) - 1 - a) b = sizeof(buf) - 1 - a;
    memcpy(buf + a, detail, b);
    buf[a + b] = '\0';
    ae_net_set_error(buf);
}

static int looks_like_ip(const char *host)
{
    return strchr(host, ':') != NULL || strspn(host, "0123456789.") == strlen(host);
}

/* Waits for the condition an SSL call asked for. 1 go on, 0 give up. */
static int tls_wait(struct tls_slot *t, int err, const char *what)
{
    int w;
    if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) return 0;
    w = ae_net_wait_fd(t->fd, err == SSL_ERROR_WANT_WRITE);
    if (w > 0) return 1;
    ae_net_set_error(w == 0 ? what : "ae_net: select failed during TLS");
    return 0;
}

static int tls_is(int s);
static long tls_write(int s, const void *buf, unsigned long len);
static long tls_read(int s, void *buf, unsigned long len);
static void tls_close(int s);
static const struct ae_net_tls_ops g_tls_ops = { tls_is, tls_write, tls_read, tls_close };

int ae_net_open_tls(const char *host, int port, int timeout_secs)
{
    struct tls_slot *t = (struct tls_slot *) 0;
    int i, s, ip;

    if (host == NULL || *host == '\0') { ae_net_set_error("ae_net: empty host"); return -1; }
    for (i = 0; i < TLS_SLOTS; i++) {
        if (!g_tls[i].used) { t = &g_tls[i]; break; }
    }
    if (t == (struct tls_slot *) 0) { ae_net_set_error("ae_net: too many TLS connections"); return -1; }

    ae_net_register_tls(&g_tls_ops);
    s = ae_net_open(host, port, timeout_secs);
    if (s < 0) return -1;
    if (tls_lib_open() != 0) { ae_net_close(s); return -1; }

    t->used = 1;
    t->fd = s;
    t->ssl = NULL;
    t->ctx = SSL_CTX_new(TLS_client_method());
    if (t->ctx == NULL) { ae_net_set_error("ae_net: SSL_CTX_new failed"); goto fail; }
    SSL_CTX_set_verify(t->ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_default_verify_paths(t->ctx);
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    SSL_CTX_set_options(t->ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif
    t->ssl = SSL_new(t->ctx);
    if (t->ssl == NULL) { ae_net_set_error("ae_net: SSL_new failed"); goto fail; }

    ip = looks_like_ip(host);
    if (ip) {
        if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(t->ssl), host) != 1) {
            ae_net_set_error("ae_net: bad IP address for the certificate check");
            goto fail;
        }
    } else {
        SSL_set_tlsext_host_name(t->ssl, host);
        if (SSL_set1_host(t->ssl, host) != 1) {
            ae_net_set_error("ae_net: cannot set the host name to verify");
            goto fail;
        }
    }

    ae_net_set_nonblocking(s, 1);
    if (SSL_set_fd(t->ssl, s) != 1) { ae_net_set_error("ae_net: SSL_set_fd failed"); goto fail; }

    for (;;) {
        int rv = SSL_connect(t->ssl);
        int err;
        if (rv == 1) break;
        err = SSL_get_error(t->ssl, rv);
        if (tls_wait(t, err, "ae_net: TLS handshake timed out")) continue;
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) goto fail;
        if (SSL_get_verify_result(t->ssl) != X509_V_OK) {
            set_error2("ae_net: certificate rejected: ",
                       X509_verify_cert_error_string(SSL_get_verify_result(t->ssl)));
        } else {
            ae_net_set_error("ae_net: TLS handshake failed");
        }
        goto fail;
    }
    return s;

fail:
    tls_free(t);
    ae_net_close(s);
    return -1;
}

static long tls_write(int s, const void *buf, unsigned long len)
{
    struct tls_slot *t = tls_find(s);
    for (;;) {
        int n = SSL_write(t->ssl, buf, (int) len);
        int err;
        if (n > 0) return n;
        err = SSL_get_error(t->ssl, n);
        if (tls_wait(t, err, "ae_net: TLS write timed out")) continue;
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) ae_net_set_error("ae_net: TLS write failed");
        return -1;
    }
}

static long tls_read(int s, void *buf, unsigned long len)
{
    struct tls_slot *t = tls_find(s);
    for (;;) {
        int n = SSL_read(t->ssl, buf, (int) len);
        int err;
        if (n > 0) return n;
        err = SSL_get_error(t->ssl, n);
        if (err == SSL_ERROR_ZERO_RETURN) return 0;
        if (tls_wait(t, err, "ae_net: read timed out")) continue;
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) ae_net_set_error("ae_net: TLS read failed");
        return -1;
    }
}

static void tls_close(int s)
{
    struct tls_slot *t = tls_find(s);
    if (t == (struct tls_slot *) 0) return;
    SSL_shutdown(t->ssl);   /* one best-effort close_notify; the socket closes next */
    tls_free(t);
}
#endif
