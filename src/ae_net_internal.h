/* ae_net_internal.h - what ae_net.c shares with ae_net_tls.c. Not for doors. */
#ifndef AE_NET_INTERNAL_H
#define AE_NET_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* The TLS backend: answers for descriptors ae_net_open_tls() returned. */
struct ae_net_tls_ops {
    int  (*is)(int s);
    long (*write)(int s, const void *buf, unsigned long len);
    long (*read)(int s, void *buf, unsigned long len);
    void (*close)(int s);
};

/* Called by ae_net_open_tls() before its first socket; ae_net.c dispatches through it. */
void ae_net_register_tls(const struct ae_net_tls_ops *ops);

/* The socket layer's own helpers, for the TLS backend. */
void ae_net_set_error(const char *msg);
int  ae_net_wait_fd(int s, int for_write);   /* 1 ready, 0 timed out, <0 failed */
void ae_net_set_nonblocking(int s, int on);

#ifdef __cplusplus
}
#endif

#endif
