/* json_lite.c - narrow, targeted JSON extraction. See json_lite.h for the
 * interface contract and the reasoning behind each decision.
 *
 * This door speaks plain HTTP by design (see README.md's "Security"
 * section) and the owner-mode curation plan's admin API responses parsed
 * here (login token, submissions list) arrive over that same untrusted
 * network path. README.md's vulnerability class #4 - "unbounded response
 * bodies" - was found and fixed once already elsewhere in this door (the
 * catalog fetch and archive download had no ceiling on total bytes
 * received); every function in this file is the same class of defense
 * applied to a new parser:
 *
 *   - Every scan is byte-by-byte over a NUL-terminated buffer, and every
 *     scan loop's termination condition checks for '\0' FIRST, before any
 *     other test - so a truncated/adversarial buffer with no closing
 *     quote/brace/escape sequence can never be read past its terminator.
 *   - Every scan loop ALSO carries a JSON_LITE_MAX_SCAN_BYTES cap,
 *     independent of the NUL check - defense in depth against a future
 *     caller that violates the "always NUL-terminated" contract, per this
 *     plan's own instruction not to rely entirely on the caller.
 *   - json_next_array_object()'s bracket-nesting stack is a small FIXED
 *     array (JSON_LITE_MAX_ARRAY_DEPTH), never dynamically sized -
 *     nesting deeper than that is refused, not overflowed.
 *   - No function here ever writes past the end of a caller-supplied
 *     buffer: every write is preceded by a capacity check against
 *     `outcap`/`outsize`, and every failure path leaves the output empty
 *     ("") rather than partially filled.
 *
 * C89. No stdint.h (not available on the m68k-amiga-elf/vbcc toolchain).
 */

#include <string.h>
#include "json_lite.h"

/* Defensive scan-length ceiling, independent of NUL-termination - see the
 * file header above. Generous relative to any real admin-API response
 * this door will ever parse (a login response or one page of submissions
 * is a few hundred bytes to a few KB), while still being a hard bound
 * rather than "trust strlen() forever". */
#define JSON_LITE_MAX_SCAN_BYTES 65536UL

/* Fixed nesting-depth ceiling for json_next_array_object()'s bracket
 * stack. This door's own JSON shapes never exceed 3-4 levels (response
 * wrapper -> "rows" array -> row object -> nested "derived" object); 16
 * is generous headroom while still being a hard, non-dynamic bound. */
#define JSON_LITE_MAX_ARRAY_DEPTH 16

static int is_json_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Non-zero when `c` could continue a bare identifier/number token -
 * used so "true"/"false" only match the whole token, not a prefix of
 * some longer, unrecognized garbage word (e.g. adversarial input like
 * "truexyz" must be refused, not silently read as boolean true). */
static int is_json_token_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}

/* Finds `"key":` anywhere in `json`, respecting quoted strings so a key
 * name appearing inside an unrelated string VALUE never false-matches -
 * only a bare, unescaped string whose full content equals `key` AND is
 * immediately followed (after optional whitespace) by ':' counts as a
 * match. Does NOT track {}/[] nesting - see json_lite.h's file header for
 * why that is safe for this door's use.
 *
 * Returns a pointer to the first byte of the value (past the colon and
 * any following whitespace), or NULL if `key` is not found as an actual
 * key, a string is left unterminated before a closing quote/NUL/the scan
 * cap, or the scan cap is reached first. */
static const char *json_find_value(const char *json, const char *key)
{
    const char *p = json;
    unsigned long key_len = (unsigned long) strlen(key);
    unsigned long scanned = 0;

    while (*p != '\0' && scanned < JSON_LITE_MAX_SCAN_BYTES) {
        if (*p == '"') {
            const char *str_start = p + 1;
            const char *q = str_start;
            int matched;

            /* Walk to the closing, unescaped quote. Bails on NUL or the
             * scan cap - whichever comes first - so an unterminated
             * string can never spin this past the input's own length. */
            while (*q != '\0' && *q != '"' && scanned < JSON_LITE_MAX_SCAN_BYTES) {
                if (*q == '\\' && q[1] != '\0') {
                    q += 2;
                    scanned += 2;
                } else {
                    q += 1;
                    scanned += 1;
                }
            }
            if (*q != '"') {
                return (const char *) 0; /* unterminated string: malformed */
            }

            matched = (key_len > 0)
                && ((unsigned long) (q - str_start) == key_len)
                && (memcmp(str_start, key, (size_t) key_len) == 0);

            p = q + 1;
            scanned += 1;

            if (matched) {
                while (is_json_ws(*p)) {
                    p++;
                    scanned++;
                    if (scanned >= JSON_LITE_MAX_SCAN_BYTES) {
                        return (const char *) 0;
                    }
                }
                if (*p == ':') {
                    p++;
                    while (is_json_ws(*p)) {
                        p++;
                        scanned++;
                        if (scanned >= JSON_LITE_MAX_SCAN_BYTES) {
                            return (const char *) 0;
                        }
                    }
                    return p;
                }
                /* A string EQUAL to `key`'s text but not followed by ':'
                 * is a VALUE, not a key (e.g. {"note":"token"} while
                 * looking for "token") - keep scanning past it rather
                 * than treating it as a match. */
            }
            continue;
        }
        p++;
        scanned++;
    }
    return (const char *) 0;
}

/* Decodes one JSON string body starting right after its opening quote
 * (`p` points at the first content byte, or the closing quote for an
 * empty string) into `out`, stopping at the matching unescaped closing
 * quote. Handles \" \\ \/ \n \t \r \b \f \uXXXX (a \uXXXX code point above 0xFF
 * is truncated to its low byte - see json_lite.h). Returns 0 on success
 * with `*end_out` set to the byte just past the closing quote; non-zero
 * (with `out` left as "") on an unterminated string, a truncated or
 * unrecognized escape, or a decoded value that would not fit `outcap`. */
static int json_decode_string(const char *p, char *out, unsigned long outcap,
                              const char **end_out)
{
    static const char hexdigits_lower[] = "0123456789abcdef";
    unsigned long n = 0;
    unsigned long scanned = 0;

    out[0] = '\0';

    while (scanned < JSON_LITE_MAX_SCAN_BYTES) {
        char c = *p;

        if (c == '\0') {
            out[0] = '\0';
            return 1; /* unterminated string */
        }
        if (c == '"') {
            out[n] = '\0';
            *end_out = p + 1;
            return 0;
        }
        if (c == '\\') {
            char e = p[1];
            char decoded;

            if (e == '\0') {
                out[0] = '\0';
                return 1; /* truncated mid-escape */
            }
            switch (e) {
            case '"':  decoded = '"';  p += 2; scanned += 2; break;
            case '\\': decoded = '\\'; p += 2; scanned += 2; break;
            case '/':  decoded = '/';  p += 2; scanned += 2; break;
            case 'n':  decoded = '\n'; p += 2; scanned += 2; break;
            case 't':  decoded = '\t'; p += 2; scanned += 2; break;
            case 'r':  decoded = '\r'; p += 2; scanned += 2; break;
            case 'b':  decoded = '\b'; p += 2; scanned += 2; break;
            case 'f':  decoded = '\f'; p += 2; scanned += 2; break;
            case 'u': {
                unsigned int val = 0;
                int i;

                for (i = 0; i < 4; i++) {
                    char h = p[2 + i];
                    const char *digit;

                    if (h == '\0') {
                        out[0] = '\0';
                        return 1; /* truncated \u escape */
                    }
                    digit = strchr(hexdigits_lower,
                                   (h >= 'A' && h <= 'F') ? (h - 'A' + 'a') : h);
                    if (digit == (const char *) 0 || *digit == '\0') {
                        out[0] = '\0';
                        return 1; /* not a hex digit: malformed escape */
                    }
                    val = (val << 4) | (unsigned int) (digit - hexdigits_lower);
                }
                decoded = (char) (val & 0xFFU);
                p += 6;
                scanned += 6;
                break;
            }
            default:
                out[0] = '\0';
                return 1; /* unrecognized escape: refuse, never guess */
            }

            if (n + 1 >= outcap) {
                out[0] = '\0';
                return 1;
            }
            out[n++] = decoded;
            out[n] = '\0';
            continue;
        }

        /* Ordinary byte, including high-bit Latin-1 (0x80-0xFF), copied
         * through verbatim. */
        if (n + 1 >= outcap) {
            out[0] = '\0';
            return 1;
        }
        out[n++] = c;
        out[n] = '\0';
        p++;
        scanned++;
    }

    out[0] = '\0';
    return 1; /* scan cap reached before a closing quote: refuse */
}

int json_extract_string(const char *json, const char *key,
                         char *out, unsigned long outcap)
{
    const char *val;
    const char *end;

    if (out != (char *) 0 && outcap > 0) {
        out[0] = '\0';
    }
    if (json == (const char *) 0 || key == (const char *) 0 || key[0] == '\0'
        || out == (char *) 0 || outcap == 0) {
        return 1;
    }

    val = json_find_value(json, key);
    if (val == (const char *) 0) {
        return 1;
    }
    if (*val != '"') {
        return 1; /* the field is present but is not a JSON string */
    }

    return json_decode_string(val + 1, out, outcap, &end);
}

int json_extract_bool(const char *json, const char *key, int *out)
{
    const char *val;

    if (json == (const char *) 0 || key == (const char *) 0 || key[0] == '\0'
        || out == (int *) 0) {
        return 1;
    }

    val = json_find_value(json, key);
    if (val == (const char *) 0) {
        return 1;
    }

    if (strncmp(val, "true", 4) == 0 && !is_json_token_char(val[4])) {
        *out = 1;
        return 0;
    }
    if (strncmp(val, "false", 5) == 0 && !is_json_token_char(val[5])) {
        *out = 0;
        return 0;
    }

    {
        const char *p = val;
        int neg = 0;
        long v = 0;
        unsigned long digits = 0;

        if (*p == '-') {
            neg = 1;
            p++;
        }
        /* Capped at 9 digits - comfortably covers every real field this
         * door reads this way (an httpish "ok" flag, an archive size in
         * bytes) while never risking an overflow of `long`, let alone the
         * `int *out` the caller actually receives. */
        while (*p >= '0' && *p <= '9' && digits < 9) {
            v = v * 10 + (long) (*p - '0');
            p++;
            digits++;
        }
        if (digits == 0 || is_json_token_char(*p)) {
            /* No digits at all, or a numeric-looking run immediately
             * followed by more identifier characters (e.g. "123abc") -
             * neither is a real JSON number; refuse rather than guess. */
            return 1;
        }
        *out = (int) (neg ? -v : v);
        return 0;
    }
}

int json_next_array_object(const char *json, const char *array_key,
                            unsigned long *cursor,
                            const char **obj_start, unsigned long *obj_len)
{
    char stack[JSON_LITE_MAX_ARRAY_DEPTH];
    int depth;
    int in_string;
    const char *p;
    const char *start;
    int target_depth;
    const char *arr;
    unsigned long arr_off;
    unsigned long start_off;
    unsigned long json_len;
    unsigned long scanned;

    if (json == (const char *) 0 || array_key == (const char *) 0 || array_key[0] == '\0'
        || cursor == (unsigned long *) 0
        || obj_start == (const char **) 0 || obj_len == (unsigned long *) 0) {
        return 1;
    }

    /* Anchor the whole scan to the array that follows `array_key`'s own
     * colon - reusing json_find_value()'s existing string-respecting key
     * search rather than the old "match the first array-of-objects found
     * ANYWHERE in the document" rule, which would have matched an
     * unrelated array (e.g. a hypothetical "filters" array preceding
     * "rows") and returned ITS elements as if they were rows. See
     * json_lite.h for the full rationale. */
    arr = json_find_value(json, array_key);
    if (arr == (const char *) 0 || *arr != '[') {
        return 1; /* array_key absent, or its value is not a JSON array */
    }
    arr_off = (unsigned long) (arr - json);

    /* strlen() itself is bounded by json's own NUL terminator - this is
     * the one-time cost of refusing to dereference json+*cursor at all
     * when the caller hands back a cursor that is somehow past the end of
     * the buffer (defensive; every real caller only ever passes back a
     * value this function itself returned). */
    json_len = (unsigned long) strlen(json);
    if (*cursor > json_len) {
        return 1;
    }

    /* On the first call (*cursor == 0, or any value at/before the array's
     * own '[') scanning starts right past that '['; on a later call it
     * resumes at *cursor, exactly as before. */
    start_off = (*cursor > arr_off + 1) ? *cursor : arr_off + 1;
    if (start_off > json_len) {
        return 1;
    }

    /* --- Replay pass -------------------------------------------------
     * Reconstructs the bracket/string context as of `start_off` by
     * re-walking from just past the TARGET array's own '[' (never from
     * the start of the whole document, and never from any other array -
     * this is what anchors matching to array_key specifically), bounded
     * strictly to start_off bytes (already proven <= json_len above, so
     * this can never run unbounded even on adversarial input). This is
     * what lets a plain byte-offset cursor correctly recognise "the next
     * '{' is a direct child of the target array" on every call, not just
     * the first - a call starting mid-array otherwise has no memory of
     * already being inside the array's '[', and would fail to recognise
     * the SECOND (and every later) row's own opening brace as an array
     * element at all. Kept as a full-fidelity replay of the same
     * bracket/string state machine the live pass below uses, rather than
     * threading extra state through *cursor itself, so this function's
     * cursor stays exactly the plain `unsigned long *cursor` this was
     * built against. */
    depth = 1;
    stack[0] = '[';
    in_string = 0;
    p = arr + 1;
    scanned = 0;
    while ((unsigned long) (p - json) < start_off && scanned < JSON_LITE_MAX_SCAN_BYTES) {
        char c = *p;

        if (in_string) {
            if (c == '\\' && (unsigned long) (p + 1 - json) < start_off) {
                p += 2;
            } else {
                if (c == '"') {
                    in_string = 0;
                }
                p += 1;
            }
            scanned++;
            continue;
        }
        if (c == '"') {
            in_string = 1;
            p++;
            scanned++;
            continue;
        }
        if (c == '{' || c == '[') {
            if (depth >= JSON_LITE_MAX_ARRAY_DEPTH) {
                return 1;
            }
            stack[depth] = c;
            depth++;
            p++;
            scanned++;
            continue;
        }
        if (c == '}' || c == ']') {
            if (depth == 0) {
                return 1;
            }
            depth--;
            p++;
            scanned++;
            if (depth == 0) {
                /* Walked past the target array's OWN close while
                 * replaying - *cursor names a position outside the array
                 * entirely, which is not a position this function itself
                 * ever produces for this array_key. */
                return 1;
            }
            continue;
        }
        p++;
        scanned++;
    }
    /* Fix for a prior review finding (Important #1): a large *cursor
     * (this door's real catalog is documented at ~442KB, well over
     * JSON_LITE_MAX_SCAN_BYTES) must never let the replay pass silently
     * STOP short of `start_off` and hand the live pass below a
     * depth/stack that describes the WRONG offset - that is
     * guess-instead-of-refuse, exactly the failure mode
     * README.md's vulnerability class #4 exists to rule out. Refuse
     * outright whenever the replay did not fully reach `start_off`,
     * regardless of why it stopped short. */
    if ((unsigned long) (p - json) < start_off) {
        return 1;
    }
    if (in_string) {
        /* *cursor landed inside a string literal - not a position this
         * function itself ever produces, so this can only mean a caller
         * handed back a cursor value that was never ours to begin with. */
        return 1;
    }

    /* --- Live pass -----------------------------------------------------
     * Finds the next object that is a direct child of whatever is on top
     * of the stack right now (only real match when it's '[') - carrying
     * the depth/stack/in_string state straight over from the replay
     * above, so array membership is recognised correctly on every call,
     * not just the first. A row's own nested "derived":{...} object is
     * just more bracket bookkeeping here (its '{' is pushed while the
     * top of stack is already '{', not '[', so it is never mistaken for
     * a new target) - its matching '}' is what lets the ROW's own '}' be
     * told apart from it. */
    start = (const char *) 0;
    target_depth = -1;
    p = json + start_off;
    /* Independent budget from the replay pass above: a large *cursor
     * (many rows already consumed) must not eat into the live pass's own
     * scan-cap allowance. */
    scanned = 0;

    while (*p != '\0' && scanned < JSON_LITE_MAX_SCAN_BYTES) {
        char c = *p;

        if (in_string) {
            if (c == '\\' && p[1] != '\0') {
                p += 2;
                scanned += 2;
            } else {
                if (c == '"') {
                    in_string = 0;
                }
                p += 1;
                scanned += 1;
            }
            continue;
        }

        if (c == '"') {
            in_string = 1;
            p++;
            scanned++;
            continue;
        }

        if (c == '{' || c == '[') {
            if (depth >= JSON_LITE_MAX_ARRAY_DEPTH) {
                return 1; /* nested deeper than this scanner tolerates */
            }
            if (c == '{' && target_depth < 0 && depth > 0 && stack[depth - 1] == '[') {
                start = p;
                target_depth = depth + 1;
            }
            stack[depth] = c;
            depth++;
            p++;
            scanned++;
            continue;
        }

        if (c == '}' || c == ']') {
            char want = (c == '}') ? '{' : '[';

            if (depth == 0 || stack[depth - 1] != want) {
                return 1; /* unbalanced/mismatched brackets: malformed */
            }
            depth--;
            p++;
            scanned++;

            if (depth == 0) {
                /* Closed the TARGET array itself - everything beyond
                 * this point (sibling keys, the document's own outer
                 * structure) is outside this scan's anchored scope and
                 * must never be interpreted as more bracket bookkeeping
                 * for this array_key. Clean end, not an error. */
                *cursor = (unsigned long) (p - json);
                return 1;
            }

            if (target_depth >= 0 && depth == target_depth - 1) {
                *obj_start = start;
                *obj_len = (unsigned long) (p - start);
                *cursor = (unsigned long) (p - json);
                return 0;
            }
            continue;
        }

        p++;
        scanned++;
    }

    /* Ran out of input (NUL) or hit the scan cap before finding another
     * array element: end of the array, or truncated/malformed input -
     * both are "stop calling", which is exactly what a non-zero return
     * means here. */
    *cursor = (unsigned long) (p - json);
    return 1;
}

static unsigned long json_escaped_len(const char *s)
{
    unsigned long len = 0;
    const unsigned char *p;

    for (p = (const unsigned char *) s; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            len += 2;
        } else if (*p < 0x20) {
            len += 6; /* \u00XX */
        } else {
            len += 1;
        }
    }
    return len;
}

/* Writes the escaped form of `s` into `out` starting at `*pos`, advancing
 * `*pos` as it goes. The caller MUST have already sized `out` (via
 * json_escaped_len()) to guarantee this never overruns - this function
 * performs no bounds check of its own, matching flow_build_info_content()'s
 * own compute-need-then-write pattern elsewhere in this door. */
static void json_escape_into(char *out, unsigned long *pos, const char *s)
{
    static const char hexdigits[] = "0123456789ABCDEF";
    const unsigned char *p;

    for (p = (const unsigned char *) s; *p != '\0'; p++) {
        if (*p == '"') {
            out[(*pos)++] = '\\';
            out[(*pos)++] = '"';
        } else if (*p == '\\') {
            out[(*pos)++] = '\\';
            out[(*pos)++] = '\\';
        } else if (*p < 0x20) {
            out[(*pos)++] = '\\';
            out[(*pos)++] = 'u';
            out[(*pos)++] = '0';
            out[(*pos)++] = '0';
            out[(*pos)++] = hexdigits[(*p >> 4) & 0x0F];
            out[(*pos)++] = hexdigits[*p & 0x0F];
        } else {
            out[(*pos)++] = (char) *p;
        }
    }
}

int json_build_login_body(char *out, unsigned long outcap,
                           const char *username, const char *password)
{
    static const char prefix[] = "{\"username\":\"";
    static const char mid[] = "\",\"password\":\"";
    static const char suffix[] = "\"}";
    unsigned long need;
    unsigned long pos;

    if (out == (char *) 0 || outcap == 0
        || username == (const char *) 0 || password == (const char *) 0) {
        if (out != (char *) 0 && outcap > 0) {
            out[0] = '\0';
        }
        return -1;
    }
    out[0] = '\0';

    need = (unsigned long) (sizeof(prefix) - 1) + json_escaped_len(username)
         + (unsigned long) (sizeof(mid) - 1) + json_escaped_len(password)
         + (unsigned long) (sizeof(suffix) - 1);

    if (need + 1 > outcap) {
        return -1;
    }

    pos = 0;
    memcpy(out + pos, prefix, sizeof(prefix) - 1);
    pos += (unsigned long) (sizeof(prefix) - 1);
    json_escape_into(out, &pos, username);
    memcpy(out + pos, mid, sizeof(mid) - 1);
    pos += (unsigned long) (sizeof(mid) - 1);
    json_escape_into(out, &pos, password);
    memcpy(out + pos, suffix, sizeof(suffix) - 1);
    pos += (unsigned long) (sizeof(suffix) - 1);
    out[pos] = '\0';

    return (int) pos;
}

int json_build_install_body(char *out, unsigned long outcap,
                             const char *command, const char *archive_name)
{
    static const char prefix[] = "{\"command\":\"";
    static const char mid[] = "\",\"archiveName\":\"";
    static const char suffix[] = "\"}";
    unsigned long need;
    unsigned long pos;

    if (out == (char *) 0 || outcap == 0
        || command == (const char *) 0 || archive_name == (const char *) 0) {
        if (out != (char *) 0 && outcap > 0) {
            out[0] = '\0';
        }
        return -1;
    }
    out[0] = '\0';

    need = (unsigned long) (sizeof(prefix) - 1) + json_escaped_len(command)
         + (unsigned long) (sizeof(mid) - 1) + json_escaped_len(archive_name)
         + (unsigned long) (sizeof(suffix) - 1);

    if (need + 1 > outcap) {
        return -1;
    }

    pos = 0;
    memcpy(out + pos, prefix, sizeof(prefix) - 1);
    pos += (unsigned long) (sizeof(prefix) - 1);
    json_escape_into(out, &pos, command);
    memcpy(out + pos, mid, sizeof(mid) - 1);
    pos += (unsigned long) (sizeof(mid) - 1);
    json_escape_into(out, &pos, archive_name);
    memcpy(out + pos, suffix, sizeof(suffix) - 1);
    pos += (unsigned long) (sizeof(suffix) - 1);
    out[pos] = '\0';

    return (int) pos;
}
