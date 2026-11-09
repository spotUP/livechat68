/*
 * ae_net - the socket a C door talks HTTP over.
 *
 * Four calls and an error string. ae_http.c is the only intended caller;
 * a door that wants raw TCP may use it too. Every function blocks, with a
 * timeout: ae_net_open() bounds the connect, ae_net_read() bounds the wait
 * for each read. Nothing here allocates.
 *
 * Which sockets are behind it is decided at compile time, in ae_net.c:
 * bsdsocket.library on AMIGA (real hardware and amiexpress-web's emulator
 * both provide it), POSIX everywhere else, and a stub under AE_NET_STUB
 * for compile-only proofs.
 */
#ifndef AE_NET_H
#define AE_NET_H

#ifdef __cplusplus
extern "C" {
#endif

/* Resolves `host`, connects to `port`, and returns a socket descriptor
 * (>= 0), or -1 - then ae_net_last_error() says why. The connect waits at
 * most timeout_secs; the same bound is used in front of every later read. */
int ae_net_open(const char *host, int port, int timeout_secs);

/* As ae_net_open(), then a TLS handshake: the certificate chain is checked
 * against the system trust store (OpenSSL natively, AmiSSL 5 on the Amiga) and
 * the certificate must name `host`. Returns a descriptor that ae_net_read,
 * ae_net_write and ae_net_close treat as an encrypted connection, or -1 -
 * ae_net_last_error() says why (certificate rejected, AmiSSL not installed...).
 * There is no way to skip verification. */
int ae_net_open_tls(const char *host, int port, int timeout_secs);

/* Sends up to `len` bytes. Returns the count sent (which may be short),
 * or <= 0 on error. */
long ae_net_write(int s, const void *buf, unsigned long len);

/* Receives up to `len` bytes, waiting at most the open's timeout. Returns
 * the count (0 at a clean EOF), or -1 on error or timeout. */
long ae_net_read(int s, void *buf, unsigned long len);

/* Closes the socket. On AMIGA the last close also closes the library. */
void ae_net_close(int s);

/* The last failure, as text a caller may print to the user. Never NULL. */
const char *ae_net_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* AE_NET_H */
