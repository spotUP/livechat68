/* ae_chatprefs.c - the client for AEW_CHATPREFS (11300). C89. See ae_chatprefs.h. */
#include <string.h>
#include "aedoor.h"
#include "ae_session.h"
#include "ae_chatprefs.h"

static int ask(const char *request, char *out, int outlen)
{
    char reply[AE_MAX_LINE + 1];
    int got, n;
    if (!out || outlen <= 0) return -1;
    out[0] = '\0';
    got = ae_field_exchange(AE_FIELD_CHATPREFS, request, reply, (int)sizeof(reply));
    if (got <= 0 || !reply[0]) return -1;
    n = (int)strlen(reply);
    if (n > outlen - 1) n = outlen - 1;
    memcpy(out, reply, (size_t)n);
    out[n] = '\0';
    return n;
}

int ae_chat_prefs_get(char *out, int outlen) { return ask("GET", out, outlen); }

int ae_chat_prefs_get_pinned(char *out, int outlen) { return ask("GET\tpinned", out, outlen); }

int ae_chat_prefs_set(const char *key, const char *value)
{
    char request[AE_MAX_LINE + 1];
    char reply[AE_MAX_LINE + 1];
    int n = 4, i, got;
    if (!key || !value) return 0;
    memcpy(request, "SET\t", 4);
    /* A TAB or control inside a field would break the split, so it becomes a space. */
    for (i = 0; key[i] && n < AE_MAX_LINE - 2; i++)
        request[n++] = ((unsigned char)key[i] < 0x20 || key[i] == 0x7f) ? ' ' : key[i];
    request[n++] = '\t';
    for (i = 0; value[i] && n < AE_MAX_LINE; i++)
        request[n++] = ((unsigned char)value[i] < 0x20 || value[i] == 0x7f) ? ' ' : value[i];
    request[n] = '\0';
    got = ae_field_exchange(AE_FIELD_CHATPREFS, request, reply, (int)sizeof(reply));
    if (got <= 0 || !reply[0]) return -1;
    return strcmp(reply, "OK") == 0 ? 1 : 0;
}

/* Points at the value of `key` in `line`, or NULL. A key matches only at the start or after ';'. */
static const char *find_value(const char *line, const char *key)
{
    int klen;
    const char *p = line;
    if (!line || !key || !key[0]) return 0;
    klen = (int)strlen(key);
    while (*p) {
        if (strncmp(p, key, (size_t)klen) == 0 && p[klen] == '=') return p + klen + 1;
        while (*p && *p != ';') p++;
        if (*p == ';') p++;
    }
    return 0;
}

int ae_chat_prefs_bool(const char *line, const char *key, int dflt)
{
    const char *v = find_value(line, key);
    if (!v || (v[0] != '0' && v[0] != '1')) return dflt;
    return v[0] == '1';
}

const char *ae_chat_prefs_str(const char *line, const char *key, char *out, int outlen)
{
    const char *v = find_value(line, key);
    int n = 0;
    if (!v || !out || outlen <= 0) return 0;
    while (v[n] && v[n] != ';' && n < outlen - 1) { out[n] = v[n]; n++; }
    out[n] = '\0';
    return out;
}
