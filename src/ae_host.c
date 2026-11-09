/*
 * ae_host - reading what the board said.
 *
 * Two ways in, one answer. On the Amiga the variables come from
 * dos.library's GetVar(), which reads both the in-memory local variables and
 * ENV:; on a native host build (the tests, and any tool built on Unix) they
 * come from getenv(). Nothing else differs, so the parsing is tested on the
 * host and the same code runs on the 68K.
 *
 * Everything here defaults to the SAFE answer, because the case that must
 * not break is a door on classic AmiExpress: no AE_HOST, so no PETSCII, no
 * wide terminal, no mouse.
 */

#include "ae_host.h"

#include <string.h>

#ifdef AMIGA
#include <proto/dos.h>
#else
#include <stdlib.h>
#endif

/* The names the board publishes. See host-vars.ts, which is the other half. */
#define VAR_HOST        "AE_HOST"
#define VAR_VERSION     "AE_HOST_VERSION"
#define VAR_CONNECTION  "AE_CONNECTION"
#define VAR_CLIENT      "AE_CLIENT"
#define VAR_CAPS        "AE_CAPS"

#define HOST_WEB_ID     "amiexpress-web"

static ae_host_info_t g_info;
static int g_loaded = 0;

/**
 * Read one variable into a caller's buffer.
 *
 * Returns 1 when the variable exists and fitted, 0 otherwise - and on 0 the
 * buffer is an empty string, so every caller can ignore the return and still
 * be reading something valid.
 */
static int read_var(const char *name, char *out, unsigned long size)
{
    if (size == 0) return 0;
    out[0] = '\0';

#ifdef AMIGA
    {
        LONG got = GetVar((STRPTR)name, (STRPTR)out, (LONG)size, 0);
        if (got <= 0) { out[0] = '\0'; return 0; }
        out[size - 1] = '\0';
        return 1;
    }
#else
    {
        const char *value = getenv(name);
        if (!value || !*value) return 0;
        strncpy(out, value, (size_t)size - 1);
        out[size - 1] = '\0';
        return 1;
    }
#endif
}

/** One entry of a comma list, compared whole. */
static int caps_has(const char *caps, const char *want)
{
    size_t want_len = strlen(want);
    const char *at = caps;

    while (*at) {
        const char *end = strchr(at, ',');
        size_t len = end ? (size_t)(end - at) : strlen(at);

        /* Whole entries only: "pet" must not match "petscii". */
        if (len == want_len && strncmp(at, want, want_len) == 0) return 1;
        if (!end) break;
        at = end + 1;
    }
    return 0;
}

ae_caps_t ae_caps_parse(const char *caps)
{
    ae_caps_t bits = 0;

    if (!caps || !*caps) return 0;

    if (caps_has(caps, "ansi"))     bits |= AE_CAP_ANSI;
    if (caps_has(caps, "petscii"))  bits |= AE_CAP_PETSCII;
    if (caps_has(caps, "c64adapt")) bits |= AE_CAP_C64ADAPT;
    if (caps_has(caps, "wide"))     bits |= AE_CAP_WIDE;
    if (caps_has(caps, "mouse"))    bits |= AE_CAP_MOUSE;

    return bits;
}

static void load(void)
{
    char host[32];
    char caps[128];
    char connection[16];
    char client[16];

    memset(&g_info, 0, sizeof(g_info));
    g_info.host = AE_HOST_CLASSIC;
    g_info.connection = AE_CONNECTION_UNKNOWN;
    g_info.client = AE_CLIENT_ANSI;
    /* A terminal that takes ANSI is the one thing true of every board. */
    g_info.caps = AE_CAP_ANSI;
    g_info.version[0] = '\0';

    g_loaded = 1;

    if (!read_var(VAR_HOST, host, sizeof(host))) return;
    if (strcmp(host, HOST_WEB_ID) != 0) return;

    g_info.host = AE_HOST_WEB;
    read_var(VAR_VERSION, g_info.version, sizeof(g_info.version));

    if (read_var(VAR_CONNECTION, connection, sizeof(connection))) {
        if (strcmp(connection, "web") == 0)         g_info.connection = AE_CONNECTION_WEB;
        else if (strcmp(connection, "telnet") == 0) g_info.connection = AE_CONNECTION_TELNET;
        else if (strcmp(connection, "ssh") == 0)    g_info.connection = AE_CONNECTION_SSH;
    }

    if (read_var(VAR_CLIENT, client, sizeof(client))) {
        if (strcmp(client, "petscii") == 0) g_info.client = AE_CLIENT_PETSCII;
    }

    if (read_var(VAR_CAPS, caps, sizeof(caps))) {
        /* The board's list replaces the default rather than adding to it:
           what it says it can carry is the whole truth for this caller. */
        g_info.caps = ae_caps_parse(caps);
    }
}

const ae_host_info_t *ae_host_info(void)
{
    if (!g_loaded) load();
    return &g_info;
}

ae_host_t ae_host(void)
{
    return ae_host_info()->host;
}

int ae_can(ae_caps_t capability)
{
    return (ae_host_info()->caps & capability) ? 1 : 0;
}

void ae_host_reset(void)
{
    g_loaded = 0;
}
