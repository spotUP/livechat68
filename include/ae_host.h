/*
 * ae_host - where is this door running, and what may it do?
 *
 * "the 68k door can't display petscii unless they run in amiexpress-web so
 * they need to detect where they are running" (sysop, 2026-09-02).
 *
 * A door binary may be run on this board or on a classic AmiExpress on real
 * hardware, and the two can carry different things. The board answers
 * through the environment; this is the door's side of that contract, which
 * is written down in Documentation/4-Door-Developers/HOST_DETECTION.md and
 * implemented in web/backend/src/amiga-emulation/utils/host-vars.ts.
 *
 * The rule the API enforces by shape: everything defaults to the SAFE
 * answer. A door that never calls anything here, or that runs somewhere
 * with no AE_HOST at all, is treated as being on a classic board with an
 * 80x25 ANSI terminal and nothing else.
 *
 * This is phase 0 of the C SDK and is deliberately first: what a door is
 * ALLOWED to assume has to exist before anything that assumes it.
 */

#ifndef AE_HOST_H
#define AE_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

/** Which board is behind this door. */
typedef enum {
    /** No AE_HOST in the environment: classic AmiExpress, or unknown. */
    AE_HOST_CLASSIC = 0,
    /** amiexpress-web. */
    AE_HOST_WEB = 1
} ae_host_t;

/** How the caller is attached. */
typedef enum {
    AE_CONNECTION_UNKNOWN = 0,
    AE_CONNECTION_WEB,
    AE_CONNECTION_TELNET,
    AE_CONNECTION_SSH
} ae_connection_t;

/** What the caller's terminal reads. */
typedef enum {
    /** ANSI. The safe assumption, and the default. */
    AE_CLIENT_ANSI = 0,
    /** A C64. The BOARD converts what the door writes - see ae_caps_petscii. */
    AE_CLIENT_PETSCII
} ae_client_t;

/**
 * What this host can carry for THIS caller.
 *
 * A bit field rather than a string, so a door tests one thing. The names
 * match AE_CAPS entries one for one.
 */
typedef unsigned long ae_caps_t;

/** ANSI colour and cursor control reach the caller. Set everywhere. */
#define AE_CAP_ANSI      (1UL << 0)
/**
 * The caller is a C64 and the board transduces the door's ANSI into PETSCII.
 *
 * This does NOT mean write PETSCII: PETSCII on this board is a transducer in
 * the backend. It means draw for 40 columns and keep to characters that
 * survive the conversion.
 */
#define AE_CAP_PETSCII   (1UL << 1)
/** The board reduces the door's 80x25 frames to the C64's 40 columns. */
#define AE_CAP_C64ADAPT  (1UL << 2)
/** The caller can be sent more than 80 columns. */
#define AE_CAP_WIDE      (1UL << 3)
/** Mouse reports reach the door. */
#define AE_CAP_MOUSE     (1UL << 4)

/** Everything the board said, read once. */
typedef struct {
    ae_host_t host;
    ae_connection_t connection;
    ae_client_t client;
    ae_caps_t caps;
    /** AE_HOST_VERSION, or "" - compare as a string, it is not a number. */
    char version[32];
} ae_host_info_t;

/**
 * Read the environment and answer.
 *
 * Cheap to call and safe to call more than once; the answer is cached after
 * the first call. Never fails: a host that says nothing yields
 * AE_HOST_CLASSIC with AE_CAP_ANSI and nothing else.
 */
const ae_host_info_t *ae_host_info(void);

/** Shorthand: which board. */
ae_host_t ae_host(void);

/**
 * Shorthand: may this door do X here?
 *
 *     if (ae_can(AE_CAP_WIDE)) { ... }
 *
 * Test the capability you are about to use, one at a time. Do not infer one
 * from another: the list will grow.
 */
int ae_can(ae_caps_t capability);

/**
 * Parse an AE_CAPS value.
 *
 * Exposed because it is the part with edges - whole entries only, so a door
 * asking for "pet" does not match "petscii" - and so it can be tested
 * without an environment.
 */
ae_caps_t ae_caps_parse(const char *caps);

/** Forget the cached answer. For tests; a door has no reason to call it. */
void ae_host_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* AE_HOST_H */
