/* ae_net.c - the socket layer under ae_http, for every C door.
 * See ae_net.h for the interface contract.
 *
 * Lifted from examples/doorrepo-c/netio.c on 2026-09-07 (backlog 4.11);
 * the three branches and their behaviour are unchanged, only the names.
 *
 * THE ONLY NETWORK FILE IN THE SDK ALLOWED TO CONTAIN "#ifdef AMIGA".
 * Three branches, selected at compile time:
 *
 *   1. AE_NET_STUB defined      -> compile-only stub, no real sockets.
 *      Exists so ae_ae_http.c and everything above it can be built to m68k
 *      object code on a toolchain without the Roadshow/AmiTCP headers.
 *   2. AMIGA defined (and AE_NET_STUB not defined) -> bsdsocket.library.
 *      This is what a door links on a real Amiga AND on amiexpress-web,
 *      whose emulator provides bsdsocket.library (LibraryManager.ts).
 *   3. neither defined          -> POSIX (getaddrinfo + BSD sockets).
 *      The native dev/test build; test_ae_ae_http.c exercises this one.
 *
 * TLS (ae_net_open_tls) is in ae_net_tls.c (OpenSSL 3 natively, AmiSSL 5 on AMIGA),
 * reached only through the ops table it registers (ae_net_internal.h); it is the
 * one stated exemption to the "#ifdef AMIGA" rule above. Branch 1 answers
 * "TLS not built".
 *
 * Branches 2 and 3 are written to mirror each other function-for-function:
 * resolve host, open a non-blocking socket, connect with a wait-for-
 * writable timeout, switch back to blocking, then use a wait-for-readable
 * timeout in front of every read.
 */

#include <string.h>
#include "ae_net.h"
#include "ae_net_internal.h"

static char g_last_error[128] = "";

static void set_error(const char *msg)
{
    unsigned long len;
    len = 0;
    while (msg[len] != '\0' && len < sizeof(g_last_error) - 1) {
        g_last_error[len] = msg[len];
        len++;
    }
    g_last_error[len] = '\0';
}

const char *ae_net_last_error(void)
{
    return g_last_error;
}

#if !defined(AE_NET_STUB)
/* TLS lives in ae_net_tls.c, which this file never references: ae_net_open_tls()
 * registers its ops here, so a door that does not use TLS does not link it.
 * A descriptor the ops claim is an SSL session; every other one is a plain socket. */
static const struct ae_net_tls_ops *g_tls = (const struct ae_net_tls_ops *) 0;

void ae_net_register_tls(const struct ae_net_tls_ops *ops)
{
    g_tls = ops;
}

static int tls_is(int s)
{
    return g_tls != (const struct ae_net_tls_ops *) 0 && g_tls->is(s);
}
static long tls_write(int s, const void *buf, unsigned long len) { return g_tls->write(s, buf, len); }
static long tls_read(int s, void *buf, unsigned long len) { return g_tls->read(s, buf, len); }
static void tls_close(int s) { g_tls->close(s); }
#endif

#if defined(AE_NET_STUB)

/* ---- Branch 1: compile-only stub. Every call fails cleanly with a
 * distinct message; no header beyond ae_net.h itself is required, which is
 * the entire point - it lets a vbcc m68k build link ae_http.c without ever
 * needing sys/socket.h, netinet/in.h, or netdb.h. */

/* Parameters are intentionally unused here - this branch never touches a
 * real socket. Not suppressed with "(void) param;": that idiom (needed
 * under native -Wextra, where AE_NET_STUB is never defined so this branch
 * never actually compiles there) trips vbcc's "statement has no effect"
 * warning instead, and vbcc does not warn on unused parameters by
 * default - so simply leaving them unused is the one form that is silent
 * on both toolchains. */

int ae_net_open(const char *host, int port, int timeout_secs)
{
    set_error("ae_net: AE_NET_STUB build, no real networking");
    return -1;
}

long ae_net_write(int s, const void *buf, unsigned long len)
{
    set_error("ae_net: AE_NET_STUB build, no real networking");
    return -1;
}

long ae_net_read(int s, void *buf, unsigned long len)
{
    set_error("ae_net: AE_NET_STUB build, no real networking");
    return -1;
}

void ae_net_close(int s)
{
}

int ae_net_open_tls(const char *host, int port, int timeout_secs)
{
    set_error("ae_net: TLS not built (AE_NET_STUB)");
    return -1;
}

#elif defined(AMIGA)

/* ---- Branch 2: bsdsocket.library.
 *
 * COMPILE-VERIFIED, NOT LINK/RUN-VERIFIED. Earlier drafts of this file
 * called this branch entirely unverified because vbcc 0.9hp3's own SDK
 * ships proto/bsdsocket.h + inline/bsdsocket_protos.h (the call
 * stubs/pragmas) but no AmiTCP SDK - no sys/socket.h, netinet/in.h,
 * netdb.h, devices/timer.h. That is still true of vbcc's OWN tree. But
 * this repo separately vendors NDK3.2R4 at "Documentation/7-Reference
 * Sources/NDK3.2R4/", and once that is added to the include path this
 * branch compiles to m68k object code with ZERO warnings under
 * `-std=c89`-equivalent vbcc strictness. Exact recipe (see the task-4
 * report's fix-round section for the full verbatim transcript):
 *   -I <vbcc's own targets/m68k-amigaos/include>
 *   -I <a directory containing ONLY devices/timer.h, symlinked from
 *       NDK3.2R4/Include_H/devices/timer.h - see the WaitSelect comment
 *       below for why this one specific file needs to come from there
 *       rather than the next include path>
 *   -I NDK3.2R4/SANA+RoadshowTCP-IP/netinclude
 *       (sys/socket.h: struct sockaddr/sa_family_t/socklen_t/AF_INET/
 *       SOCK_STREAM/SOL_SOCKET/SO_ERROR; netinet/in.h: struct in_addr,
 *       struct sockaddr_in [4.4BSD-style, with a sin_len byte - set
 *       explicitly below], htons()/htonl() as identity macros, correct
 *       on m68k which is already big-endian; netdb.h: struct hostent.
 *       Both sys/socket.h and netinet/in.h have a
 *       "#elif defined(__VBCC__) #pragma amiga-align" branch - this
 *       vendored NDK was written with vbcc as a target, not just SAS/C.)
 *   -I <m68k-amiga-elf-gcc's sys-include> (fallback only, for
 *       exec/types.h and similar OS-generic headers neither of the above
 *       provides; its OWN devices/timer.h is intentionally shadowed by
 *       the earlier -I - see below)
 * Hand-declaring sockaddr/hostent/etc. from scratch remains ruled out per
 * the plan (ABI-mismatch risk) - this is not that: the vendored NDK is
 * the authoritative source for this exact library version, not a guess.
 * What is still NOT verified: linking against a real bsdsocket.library
 * and running on real (or emulated) AmigaOS - no such environment is
 * available here. Compiling clean is strong evidence the API usage is
 * structurally correct, but LVO offsets, register conventions at the
 * ABI level, and runtime behavior remain unexercised.
 */

#include <exec/types.h>
#include <proto/exec.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdlib.h>
#include <proto/bsdsocket.h>

/* bsdsocket.library's WaitSelect() timeout parameter - what it actually
 * expects, determined by experiment rather than assumed:
 *
 * The master .sfd (NDK3.2R4/
 * SANA+RoadshowTCP-IP/sfd/bsdsocket_lib.sfd:34) says "struct timeval
 * *_timeout". The vendored NDK's own DOCUMENTATION copy of
 * inline/bsdsocket_protos.h renames that to "struct __timeval *" in its
 * generated prototype (evidently to sidestep AmigaOS's separate, unrelated
 * "struct timeval" from <devices/timer.h>) and never defines
 * "struct __timeval" anywhere - confirmed by grepping the whole vendored
 * NDK tree. BUT the vbcc 0.9hp3 SDK actually INSTALLED on this machine
 * (targets/m68k-amigaos/include/inline/bsdsocket_protos.h:56, checked
 * directly, not assumed from the vendored doc copy above) declares
 * WaitSelect's timeout parameter as plain "struct timeval *" - no
 * "__timeval" involved at all for this toolchain version. So the fix is
 * not an alias shim; it is supplying a real, complete "struct timeval".
 *
 * That struct is reachable from the vendored NDK: <sys/socket.h> (see
 * NDK3.2R4/SANA+RoadshowTCP-IP/
 * netinclude/sys/socket.h, bottom of file) includes <devices/timer.h> and
 * then "#define tv_sec tv_secs" / "#define tv_usec tv_micro", giving
 * POSIX-named field access on top of the AmigaOS-native struct - exactly
 * what this file uses below (tv.tv_sec / tv.tv_usec). The one wrinkle:
 * <devices/timer.h> as vendored in the m68k-amiga-elf-gcc sys-include tree
 * wraps tv_sec/tv_secs (and tv_usec/tv_micro) in anonymous unions, a GNU
 * extension vbcc does not accept (warning 53, "struct/union member needs
 * identifier") - so the build points -I at the ORIGINAL NDK3.2R4 copy
 * (NDK3.2R4/Include_H/devices/timer.h,
 * a plain non-union struct) ahead of it. See the task-4 report's fix-round
 * section for the exact include order and vbcc transcript (0 warnings). */
#include <devices/timer.h>

struct Library *SocketBase = (struct Library *) 0;
static int g_timeout_secs = 30;

/* The fd_set handed to WaitSelect() below is a single `long`, built with the
 * universal AmigaOS idiom `1L << s`. That shift is only defined while `s` is
 * smaller than the number of bits in a long, so a descriptor at or above
 * that bound cannot be represented in the mask at all: the bit lands nowhere
 * WaitSelect() looks and the door waits forever on a socket that is ready.
 *
 * Rather than assume the stack cooperates, ask it: getdtablesize() reports
 * how many descriptors it is willing to hand out. A stack that can exceed
 * our mask width is clamped to the mask width, and a descriptor past the
 * limit is refused with a clear message instead of hanging. This is the
 * check every door building a single-long fd_set should be making. */
#define NETIO_FD_MASK_BITS ((int) (sizeof(long) * 8))

static int fd_mask_limit(void)
{
    int limit;

    limit = (int) getdtablesize();
    if (limit <= 0 || limit > NETIO_FD_MASK_BITS) {
        limit = NETIO_FD_MASK_BITS;
    }
    return limit;
}

/* Maps a bsdsocket Errno() value to a specific message. These are the
 * classic BSD/AmigaOS numbers the stack's own <errno.h> defines
 * (ECONNREFUSED 61, ETIMEDOUT 60, ENETUNREACH 51, EHOSTUNREACH 65) - NOT
 * the Linux numbering, which differs for everything above 34. Mirrors the
 * POSIX branch, which already reported a refused port distinctly from an
 * unreachable one; folding them all into "connect() failed" left a sysop
 * unable to tell a closed port from a black hole. */
static const char *connect_error_text(int err)
{
    if (err == ECONNREFUSED) return "ae_net: connect() refused";
    if (err == ETIMEDOUT)    return "ae_net: connect() timed out";
    if (err == ENETUNREACH)  return "ae_net: network unreachable";
    if (err == EHOSTUNREACH) return "ae_net: no route to host";
    return "ae_net: connect() failed";
}

static void close_socket_lib(void)
{
    if (SocketBase != (struct Library *) 0) { CloseLibrary(SocketBase); SocketBase = (struct Library *) 0; }
}

static int ensure_lib(void)
{
    if (SocketBase != (struct Library *) 0) return 0;
    SocketBase = OpenLibrary((STRPTR) "bsdsocket.library", 4);
    if (SocketBase == (struct Library *) 0) { set_error("ae_net: cannot open bsdsocket.library v4"); return -1; }
    atexit(close_socket_lib);
    return 0;
}

int ae_net_open(const char *host, int port, int timeout_secs)
{
    int s;
    long on;
    struct hostent *he;
    struct sockaddr_in addr;
    struct timeval tv;
    long wmask;

    if (ensure_lib() != 0) return -1;
    g_timeout_secs = timeout_secs;

    he = gethostbyname((STRPTR) host);
    if (he == (struct hostent *) 0) { set_error("ae_net: gethostbyname() failed"); return -1; }

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { set_error("ae_net: socket() failed"); return -1; }
    if (s >= fd_mask_limit()) {
        /* Cannot be expressed in the single-long fd_set used below. */
        set_error("ae_net: socket descriptor above this stack's fd_set width");
        CloseSocket(s);
        return -1;
    }
    on = 1;
    IoctlSocket(s, FIONBIO, (char *) &on); /* non-blocking, so connect() below can be timed */

    memset(&addr, 0, sizeof(addr));
    addr.sin_len = sizeof(addr); /* the vendored netinclude/netinet/in.h struct sockaddr_in carries a 4.4BSD-style length byte */
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short) port);
    memcpy(&addr.sin_addr, he->h_addr, (size_t) he->h_length);

    if (connect(s, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        int err = Errno();
        if (err != EINPROGRESS) {
            set_error(connect_error_text(err)); CloseSocket(s); return -1;
        }
    }

    wmask = 1L << s;
    tv.tv_sec = timeout_secs; tv.tv_usec = 0;
    if (WaitSelect(s + 1, (APTR) 0, (APTR) &wmask, (APTR) 0, &tv, (unsigned long *) 0) <= 0) {
        set_error("ae_net: connect() timed out"); CloseSocket(s); return -1;
    }

    /* Becoming writable does NOT mean the connection succeeded: a
     * non-blocking connect that is later refused or times out at the stack
     * also wakes WaitSelect(), and the real outcome is in SO_ERROR. Without
     * this check a failed connect looks like a good socket, and the failure
     * only surfaces later as an inexplicable send()/recv() error. The POSIX
     * branch below has always done this; the Amiga branch did not.
     * Prototype per the NDK's own bsdsocket_lib.sfd:29 -
     * LONG getsockopt(LONG, LONG, LONG, APTR, socklen_t *). */
    {
        long soerr;
        socklen_t soerrlen;

        soerr = 0;
        soerrlen = (socklen_t) sizeof(soerr);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (APTR) &soerr, &soerrlen) == 0
            && soerr != 0) {
            set_error(connect_error_text((int) soerr));
            CloseSocket(s);
            return -1;
        }
    }

    on = 0;
    IoctlSocket(s, FIONBIO, (char *) &on); /* back to blocking for the send/recv calls below */
    return s;
}

/* Waits for the descriptor to become readable (for_write 0) or writable
 * (1), bounded by the open's timeout. 1 ready, 0 timed out, -1 error. */
static int wait_fd(int s, int for_write)
{
    struct timeval tv;
    long mask = 1L << s;

    tv.tv_sec = g_timeout_secs; tv.tv_usec = 0;
    if (for_write) return (int) WaitSelect(s + 1, (APTR) 0, (APTR) &mask, (APTR) 0, &tv, (unsigned long *) 0);
    return (int) WaitSelect(s + 1, (APTR) &mask, (APTR) 0, (APTR) 0, &tv, (unsigned long *) 0);
}

static void set_nonblocking(int s, int on)
{
    long v = on ? 1 : 0;
    IoctlSocket(s, FIONBIO, (char *) &v);
}

long ae_net_write(int s, const void *buf, unsigned long len)
{
    long n;
    if (tls_is(s)) return tls_write(s, buf, len);
    n = send(s, (APTR) buf, (long) len, 0);
    if (n < 0) set_error("ae_net: send() failed");
    return n;
}

/* Every read is bounded by g_timeout_secs (set by ae_net_open()): WaitSelect()
 * first, so a peer that stops sending mid-response cannot hang the door. */
long ae_net_read(int s, void *buf, unsigned long len)
{
    long n;

    if (tls_is(s)) return tls_read(s, buf, len);
    n = wait_fd(s, 0);
    if (n == 0) { set_error("ae_net: read timed out"); return -1; }
    if (n < 0) { set_error("ae_net: WaitSelect() failed"); return -1; }

    n = recv(s, (APTR) buf, (long) len, 0);
    if (n < 0) set_error("ae_net: recv() failed");
    return n;
}

void ae_net_close(int s)
{
    if (s < 0) return;
    if (tls_is(s)) tls_close(s);
    CloseSocket(s);
}

#else

/* ---- Branch 3: POSIX (getaddrinfo + BSD sockets). Native dev/test build.
 * Mirrors the Amiga branch above: non-blocking connect bounded by
 * select(), then blocking I/O with a select()-bounded wait before every
 * read (the socket-level read timeout ae_net_read() promises). */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int g_timeout_secs = 30;

/* Same mapping as the Amiga branch's function of this name, over this
 * platform's errno values (the identifiers are portable; the numbers are
 * not, which is exactly why both branches compare names rather than
 * literals). Keeps the two branches reporting the same distinctions. */
static const char *connect_error_text(int err)
{
    if (err == ECONNREFUSED) return "ae_net: connect() refused";
    if (err == ETIMEDOUT)    return "ae_net: connect() timed out";
    if (err == ENETUNREACH)  return "ae_net: network unreachable";
    if (err == EHOSTUNREACH) return "ae_net: no route to host";
    return "ae_net: connect() failed";
}

/* Formats a non-negative int as decimal into `out` (at least 12 bytes).
 * Avoids sprintf()/snprintf(): C89 has no snprintf, and getaddrinfo()'s
 * port argument is the only place this file needs numeric-to-string
 * conversion, so a tiny hand-rolled formatter is simpler than pulling in
 * stdio's formatting machinery for one field. */
static void format_port(char *out, int port)
{
    char tmp[12];
    int i = 0;
    unsigned int v = (unsigned int) port;

    if (v == 0) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    while (v > 0) {
        tmp[i++] = (char) ('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        *out++ = tmp[--i];
    }
    *out = '\0';
}

int ae_net_open(const char *host, int port, int timeout_secs)
{
    int s;
    int flags;
    struct addrinfo hints;
    struct addrinfo *res;
    struct addrinfo *rp;
    char portbuf[16];
    fd_set wfds;
    struct timeval tv;
    int err;
    socklen_t errlen;
    const char *fail_reason;

    g_timeout_secs = timeout_secs;

    format_port(portbuf, port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portbuf, &hints, &res) != 0) {
        set_error("ae_net: getaddrinfo() failed");
        return -1;
    }

    /* Mirrors the Amiga branch's ae_net_open(): "connect() failed" (refused
     * or otherwise rejected synchronously/asynchronously) and
     * "connect() timed out" (no response within timeout_secs) are
     * reported as distinct ae_net_last_error() strings, not folded into one
     * generic message - a caller (or a human reading a log) can tell a
     * closed port from an unreachable/black-holed one. fail_reason
     * carries the most recent attempt's specific reason forward; if every
     * address in res fails, the last one's reason is what gets reported,
     * same as which failing `s` "wins" is already decided by the loop. */
    fail_reason = "ae_net: connect() failed";
    s = -1;
    for (rp = res; rp != (struct addrinfo *) 0; rp = rp->ai_next) {
        s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s < 0) {
            fail_reason = "ae_net: socket() failed";
            continue;
        }

        flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, flags | O_NONBLOCK);

        if (connect(s, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        if (errno != EINPROGRESS) {
            fail_reason = connect_error_text(errno);
            close(s);
            s = -1;
            continue;
        }

        FD_ZERO(&wfds);
        FD_SET(s, &wfds);
        tv.tv_sec = timeout_secs;
        tv.tv_usec = 0;
        if (select(s + 1, (fd_set *) 0, &wfds, (fd_set *) 0, &tv) <= 0) {
            fail_reason = "ae_net: connect() timed out";
            close(s);
            s = -1;
            continue;
        }

        err = 0;
        errlen = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err != 0) {
            /* Asynchronous failure: the reason is in SO_ERROR, not errno. */
            fail_reason = connect_error_text(err);
            close(s);
            s = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(res);

    if (s < 0) {
        set_error(fail_reason);
        return -1;
    }

    flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags & ~O_NONBLOCK);
    return s;
}

/* Same helpers as the Amiga branch's, over select()/fcntl(). */
static int wait_fd(int s, int for_write)
{
    fd_set fds;
    struct timeval tv;
    int rv;

    for (;;) {
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        tv.tv_sec = g_timeout_secs;
        tv.tv_usec = 0;
        if (for_write) rv = select(s + 1, (fd_set *) 0, &fds, (fd_set *) 0, &tv);
        else           rv = select(s + 1, &fds, (fd_set *) 0, (fd_set *) 0, &tv);
        if (rv < 0 && errno == EINTR) continue;
        return rv;
    }
}

static void set_nonblocking(int s, int on)
{
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}

long ae_net_write(int s, const void *buf, unsigned long len)
{
    ssize_t n;
    if (tls_is(s)) return tls_write(s, buf, len);
    n = write(s, buf, (size_t) len);
    if (n < 0) {
        set_error("ae_net: write() failed");
    }
    return (long) n;
}

long ae_net_read(int s, void *buf, unsigned long len)
{
    int rv;
    ssize_t n;

    if (tls_is(s)) return tls_read(s, buf, len);
    rv = wait_fd(s, 0);
    if (rv == 0) {
        set_error("ae_net: read timed out");
        return -1;
    }
    if (rv < 0) {
        set_error("ae_net: select() failed");
        return -1;
    }

    n = read(s, buf, (size_t) len);
    if (n < 0) {
        set_error("ae_net: read() failed");
    }
    return (long) n;
}

void ae_net_close(int s)
{
    if (s >= 0) {
        if (tls_is(s)) tls_close(s);
        close(s);
    }
}

#endif

#if !defined(AE_NET_STUB)
/* What ae_net_tls.c borrows from the branch above (ae_net_internal.h). */
void ae_net_set_error(const char *msg) { set_error(msg); }
int ae_net_wait_fd(int s, int for_write) { return wait_fd(s, for_write); }
void ae_net_set_nonblocking(int s, int on) { set_nonblocking(s, on); }
#endif
