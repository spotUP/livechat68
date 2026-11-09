/* json_lite.h - narrow, targeted JSON extraction for the DoorRepo admin API
 * (owner-mode curation: login token, submission list). NOT a general JSON
 * parser, matching this door's existing minimalist-parser ethos -
 * listtxt.c reads six positional pipe-delimited fields and ignores the
 * rest; this reads a handful of named keys out of a flat or shallowly
 * nested object and ignores everything else. Every key this door ever
 * looks for (token, id, archiveName, error, ok, size, md5, note, status)
 * is unique within the one response it is read from, so none of the
 * scalar extractors below track {}/[] nesting depth at all - only
 * json_next_array_object() does, because that is the one place nesting
 * depth is unavoidable (a row's own closing brace must not be confused
 * with its nested "derived" object's closing brace).
 *
 * CAVEAT ON "every key is unique" (found in task review, not merely
 * theoretical): this premise holds for id/archiveName/size/md5/note/
 * status ONLY as long as the slice handed to json_extract_string()/
 * json_extract_bool() does not itself contain a "derived" sub-object -
 * see those functions' own doc comments below for the exact risk and the
 * required caller mitigation before extracting from a row that may carry
 * one.
 *
 * This is the single most important security surface in the owner-mode
 * curation plan (see this door's own README.md, "Security" section,
 * vulnerability class #4 - "unbounded response bodies"): these functions
 * are handed byte content that arrived over the network from a server
 * this door does not control. Every one of them is byte-by-byte bounded,
 * refuses rather than guesses on anything malformed or truncated, and
 * never reads past a NUL terminator - see json_lite.c's file header for
 * the specific defenses.
 *
 * Pure, I/O-free. C89. No stdint.h (not available on the m68k-amiga-elf/
 * vbcc toolchain).
  *
 * MOVED HERE from examples/doorrepo-c (2026-09-03). It was always general -
 * no door state, no BBS, just bytes in and values out - and phase 4's
 * settings need exactly this. It is compiled once, from sdk/c, and DoorRepo
 * reads it from here rather than keeping the second copy that a move would
 * otherwise have left behind.
 */

#ifndef DOORREPO_JSON_LITE_H
#define DOORREPO_JSON_LITE_H

/* Finds "key":"value" (a string field) anywhere in a flat or nested JSON
 * object and copies the unescaped value into `out` (\" \\ \/ \n \t \r \b
 * \f and \uXXXX are unescaped per the JSON spec's minimum - this door's
 * own values never need more; a \uXXXX code point above 0xFF is truncated
 * to its low byte, matching the Latin-1-everywhere assumption this door
 * already makes elsewhere - see listtxt.h's file header). Scans
 * byte-by-byte and respects quoted strings (a key name appearing inside
 * an unrelated string VALUE, e.g. `{"note":"the token field matters"}`,
 * must not false-match when looking for "token") but does NOT track {}
 * object-nesting depth - safe for this door's use because every key it
 * looks for is unique within the one response it's read from (see the
 * file header above) - WITH ONE DOCUMENTED EXCEPTION:
 *
 * DERIVED-OBJECT COLLISION (found in task review): because this function
 * scans linearly and does not track nesting, a row object carrying a
 * "derived":{...} sub-object (per json_next_array_object()'s doc comment
 * below, opaque server-side metadata about the derived/processed archive)
 * that ITSELF happens to carry a same-named field - e.g. "derived":
 * {"size":999,"md5":"..."} - will be matched INSTEAD OF the row's own
 * top-level field, if "derived" appears earlier in the row than the field
 * being searched for. This is server-controlled content, not something
 * this door controls, and the consequence is real: a sysop approving or
 * rejecting a submission by an id/size/md5 read from the wrong slice of
 * the response.
 *
 * REQUIRED MITIGATION for any caller extracting from a row slice that may
 * contain "derived": before calling json_extract_string()/
 * json_extract_bool() on the row, locate `"derived"` in the row's own
 * copied buffer (a plain, bounded `strstr(row, "\"derived\"")` is
 * sufficient here - "derived" is this module's own reserved/known key
 * name, not attacker-chosen text this door needs to defend a false-match
 * against) and, if found, NUL-terminate the row buffer at that byte
 * BEFORE extracting id/archiveName/size/md5/note/status from it. This is
 * a FAIL-SAFE mitigation, not a provably complete one, and the two ways
 * it can fall short are both benign rather than dangerous: (1) `strstr`
 * matches the literal bytes `"derived"` wherever they occur, so a row
 * field whose own VALUE happens to be the string "derived" (e.g.
 * `"note":"derived"`) truncates earlier than necessary; (2) JSON object
 * key order is a convention this server has followed so far, not a
 * guaranteed invariant, so a future response that emits "derived" before
 * one of the fields this door reads would truncate that field away too.
 * Both cases only ever cause a field this door wanted to read to come
 * back ABSENT (a clean, already-handled extraction failure) - never a
 * field silently populated with the WRONG value, which is the actual
 * defect this mitigation exists to prevent. This was deliberately NOT
 * built into this module itself: doing so would require
 * either abandoning the "flat or nested" search this function is
 * otherwise documented (and tested) to support, or hardcoding the literal
 * name "derived" into a supposedly key-agnostic extractor - both are a
 * worse trade than one documented `strstr` at the one call site that
 * actually needs it.
 *
 * Returns 0 on success. Returns non-zero if the key is absent, the value
 * is not a JSON string, the input is truncated/malformed (an unterminated
 * string, a cut escape sequence, or a scan-length cap reached before a
 * terminator was found), or the decoded value would not fit `outcap` -
 * `out` is left as "" on any failure, never partially filled. */
int json_extract_string(const char *json, const char *key,
                         char *out, unsigned long outcap);

/* Same contract for a bare boolean or integer/numeric field (e.g. an
 * httpish "ok":true read as 1, or "size":12345 read as 12345). Accepts
 * the literal tokens `true`/`false` (read as 1/0) or an optionally
 * negative run of ASCII digits; anything else at the value position
 * (a string, null, an object/array, or no digits at all) is refused.
 * Same key-finding rule as json_extract_string() above (string-respecting,
 * no {} nesting tracking) - INCLUDING the same "derived" sub-object
 * collision risk and required mitigation documented on that function;
 * it applies here identically (e.g. a row's "derived":{"size":999} could
 * shadow the row's own top-level "size" field). Returns 0/non-zero the
 * same way; `*out` is left untouched on failure. */
int json_extract_bool(const char *json, const char *key, int *out);

/* Cursor-based scanner for a top-level JSON array of objects (GET
 * /submissions' `{"rows":[{...},{...}]}` shape). Call repeatedly with
 * the same `*cursor` (start it at 0); each call returns a pointer to the
 * START of the next {...} object that is a DIRECT ELEMENT of the array
 * that follows `array_key`'s own colon (e.g. "rows"), and its length,
 * and advances *cursor past it.
 *
 * MATCHING RULE, STATED PRECISELY (tightened in task review - a prior
 * draft matched the first array-of-objects found ANYWHERE in `json`,
 * with no anchor to a specific key at all): the scan is anchored to the
 * array that follows the FIRST occurrence of `array_key` found via the
 * same string-respecting key search json_find_value() uses internally
 * for json_extract_string()/json_extract_bool() - so an unrelated array
 * appearing earlier in the document under a DIFFERENT key (e.g. a
 * hypothetical `{"filters":[{"a":1}],"rows":[{...}]}` response gaining a
 * "filters" array ahead of "rows") can never be mistaken for the target
 * and returned as a "row". RESIDUAL CAVEAT this rule does NOT close (out
 * of scope for the finding that prompted this rewrite, noted here rather
 * than silently left implicit): the search is not anchored to
 * TOP-LEVEL, so a response carrying the SAME key name at more than one
 * nesting depth (e.g. `{"debug":{"rows":[...]},"rows":[real]}`) still
 * anchors to whichever occurrence comes first in scan order, which may
 * not be the top-level one. This module's own key-uniqueness premise
 * (file header above) covers this the same way it covers every other
 * key this door looks for - real only if the server-controlled response
 * shape ever legitimately nests a same-named key, which the documented
 * `/submissions` shape does not. Everything before the matched array's
 * own '[' - the response's own outer wrapping object, any other array,
 * any other key - is never even scanned, let alone matched. `array_key`
 * must be present in `json` with an array value, or this returns
 * non-zero immediately (key absent, or
 * present but not an array).
 *
 * This is the one function in this module that DOES track bracket
 * nesting - a small, fixed-depth stack of container types ('{' / '[')
 * rather than a single integer, so a row's own nested "derived":{...}
 * object is walked through (its braces balanced, its content otherwise
 * ignored) without ending the row early, and so an object nested inside
 * something OTHER than the target array is never mistaken for a row
 * either. "derived" is never parsed by THIS function - it is opaque,
 * skipped whole; v1 has no use for it. The caller then runs
 * json_extract_string()/json_extract_bool() against just that object's
 * slice (copied into a small NUL-terminated buffer first - this function
 * returns a pointer/length INTO `json`, not a NUL-terminated string on
 * its own) for id/archiveName/size/md5/note/status - see those two
 * functions' own doc comments for the required "derived" truncation
 * mitigation before doing so on a row that may carry one.
 *
 * Once the target array's own closing `]` is reached, the scan stops
 * cleanly (non-zero return, "end of array") WITHOUT interpreting
 * anything beyond it - bytes after the array (sibling keys, the
 * document's own outer closing brace) are outside this function's
 * anchored scope and are never treated as bracket-balance errors just
 * because they weren't modeled.
 *
 * Returns 0 and sets *obj_start and *obj_len when an object was found,
 * non-zero at the end of the array (including a genuinely empty `[]`),
 * on malformed input (mismatched/unbalanced brackets, an unterminated
 * string, nesting deeper than this scanner's fixed stack, or a
 * scan-length cap reached before a terminator was found - the latter is
 * an explicit REFUSAL, not a best-effort partial scan: a *cursor value
 * whose reconstruction would require re-walking more than this
 * function's scan cap is treated as unusable rather than proceeding with
 * an incompletely-reconstructed bracket depth, since this door's own
 * real catalog is documented at ~442KB, well over that cap), or on bad
 * arguments (`array_key` absent/not-an-array, or `*cursor` past the end
 * of `json`). A non-zero return means "stop calling" - the caller should
 * not keep looping on the same cursor expecting a different answer. */
int json_next_array_object(const char *json, const char *array_key,
                            unsigned long *cursor,
                            const char **obj_start, unsigned long *obj_len);

/* Builds `{"username":"...","password":"..."}` into `out`, JSON-escaping
 * both fields (`"` and `\` at minimum, per the interface this was
 * specified against - a password containing either must not break the
 * request or, worse, let its content escape the JSON string and be
 * interpreted as JSON structure; this implementation also escapes every
 * control byte 0x00-0x1F as \u00XX, since an unescaped one is invalid
 * JSON and could otherwise inject a raw newline into the request body).
 * High-bit Latin-1 bytes (0x80-0xFF) pass through unescaped, matching how
 * this door treats them everywhere else (see listtxt.h). The escaping is
 * the exact inverse of json_extract_string()'s unescaping - a value built
 * here and read back through that function round-trips byte-for-byte.
 *
 * Same signature style as flow_build_info_content(): returns the byte
 * count written, or -1 if `out`/`username`/`password` is NULL, or the
 * escaped result would not fit `outcap` - in either failure case `out` is
 * left as "". */
int json_build_login_body(char *out, unsigned long outcap,
                           const char *username, const char *password);

/* Builds `{"command":"...","archiveName":"..."}` into `out` - the install
 * report DOORREPO POSTs to the BBS's own /api/door-admin/installed after a
 * successful install (see doorrepo.c's install_door()). Same escaping,
 * same helpers, and the exact same "returns byte count or -1, leaves out
 * as \"\" on any failure" contract as json_build_login_body() above -
 * built the same way for the same reason: the catalog this archiveName
 * comes from holds thousands of Latin-1 names (e.g. archives containing a
 * literal `"` or `\`), and a bare sprintf("%s") would let one break the
 * JSON body and silently drop the install report. High-bit Latin-1 bytes
 * (0x80-0xFF) pass through unescaped, matching json_build_login_body() and
 * this door's Latin-1-everywhere assumption elsewhere. `command` is
 * escaped with the exact same helper as `archive_name` even though this
 * door's own BBS-command validation (flow_is_valid_bbs_command()) already
 * restricts it to A-Z0-9 - one escaping code path for both fields, not a
 * second "trusted" one that stops being trusted the day something upstream
 * of this call changes.
 *
 * Returns the byte count written, or -1 if `out`/`command`/`archive_name`
 * is NULL, or the escaped result would not fit `outcap` - in either
 * failure case `out` is left as "". */
int json_build_install_body(char *out, unsigned long outcap,
                             const char *command, const char *archive_name);

#endif /* DOORREPO_JSON_LITE_H */
