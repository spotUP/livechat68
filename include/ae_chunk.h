/*
 * ae_chunk - how much of a frame fits in one AEDoor message.
 *
 * A JH_SM message carries 198 characters, and a frame is bigger than that,
 * so ae_put() sends it in pieces. Cutting at 198 exactly would sometimes cut
 * THROUGH an ANSI escape sequence, and the half that arrived would be
 * printed as text - which is the "stray [0;40;37m in the middle of a row"
 * a caller sees.
 *
 * MOVED from examples/doorrepo-c/flow.c (2026-09-03): it is a fact about the
 * protocol, not about DoorRepo, and the transport that needs it now lives in
 * this SDK.
 */

#ifndef AE_CHUNK_H
#define AE_CHUNK_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * How many of `len` bytes may be sent now without tearing an escape
 * sequence: `budget` at most, less when the cut would land inside one.
 */
unsigned long ae_safe_chunk(const char *text, unsigned long len,
                            unsigned long budget);

/* The name DoorRepo has always called it by. */
#define flow_safe_chunk(text, len, budget) ae_safe_chunk((text), (len), (budget))

#ifdef __cplusplus
}
#endif

#endif /* AE_CHUNK_H */
