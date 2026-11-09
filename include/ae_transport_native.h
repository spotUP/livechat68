/* ae_transport_native.h - the HOST backend's own seam.
 *
 * ae_transport_native.c is the stdio twin of the AEDoor protocol: it exists so
 * door logic can be written once, TDD'd on a dev machine, and cross-compiled
 * unchanged against ae_transport_amiga.c. Most of aedoor.h maps onto stdio
 * with no help - ae_put is fwrite, ae_key is getchar.
 *
 * ae_field_exchange does not. "Send a query, get an answer back" has no
 * meaning against a terminal, and a door module built on it (ae_msgbase.c)
 * cannot be tested at all unless something can PLAY THE BOARD. That is what
 * this header is: the one hook a host test fills in, kept out of aedoor.h so
 * the door-facing API stays the same on both backends.
 *
 * With no hook set, ae_field_exchange answers 0 - which is exactly what a
 * classic AmiExpress answers to a command it does not have, so the "no host"
 * path is the DEFAULT here rather than a special case somebody has to
 * remember to test.
 *
 * C89. ASCII only. Never compiled for the Amiga target.
 */

#ifndef AE_TRANSPORT_NATIVE_H
#define AE_TRANSPORT_NATIVE_H

/**
 * A stand-in board.
 *
 * Same contract as ae_field_exchange: write the answer into `out` (at most
 * `cap - 1` bytes plus a NUL) and return the number of bytes written, 0 for
 * "nothing more"/"unknown command", or -1 for a dropped carrier.
 */
typedef int (*ae_native_exchange_fn)(void *ctx, int field,
                                     const char *request, char *out, int cap);

/** Install one. Passing 0 removes it, which restores the classic-board case. */
void ae_native_set_exchange(ae_native_exchange_fn fn, void *ctx);

#endif /* AE_TRANSPORT_NATIVE_H */
