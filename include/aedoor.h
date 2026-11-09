/* aedoor.h - AmiExpress XIM door I/O layer for the DoorRepo C client.
 *
 * Two implementations share this interface:
 *   aedoor_amiga.c  - the real XIM message-port protocol against a running
 *                      AmiExpress node.
 *   aedoor_native.c - a stdio twin used for native TDD and dev-machine runs.
 * Neither file contains "#ifdef AMIGA" or any other platform conditional --
 * the split at the file level IS the platform mechanism. The Makefile
 * (Task 6) picks which .c file to compile.
 *
 * Style and structure are modelled on the AmiExpress-author-adjacent glue in
 * amiexpress_doors/Sources/_C/AE_DOORS/AEDoor.c (functions PortStart,
 * CheckMessage, TakeOffEh, ShutDown, GetString, PutString) by Gord
 * Dimitrieff, and on the shorter complete example
 * amiexpress_doors/Sources/_AREXX/DC-X107I/DC-SX107install/SX/Developer/
 * SASC/Example2.c (XIMFunction). Protocol facts (struct layout, command
 * codes, the 198-char usable payload, the Data==-1 carrier-loss signal) are
 * drawn from
 * thoughts/shared/research/2026-08-17_xim-door-protocol-for-c-clients.md,
 * which supersedes doordocs.txt on several points where the two disagree --
 * see the citations in aedoor_amiga.c.
 *
 * C89. ASCII only.
  *
 * MOVED HERE from examples/doorrepo-c (2026-09-03). This is the AEDoor
 * protocol - the only way a 68K door talks to this board - and it belongs to
 * every C door, not to the one that happened to implement it first. The two
 * backends moved with it: src/ae_transport_amiga.c is the real exec message
 * round trip, src/ae_transport_native.c the host stand-in the tests use.
 *
 * It is deliberately still the SAME API DoorRepo has always called, because
 * that door is the proof this protocol works and a rewrite of its call sites
 * would put that proof at risk for no gain.
 */

#ifndef AEDOOR_H
#define AEDOOR_H

/* Usable JH_Message String payload is 198 characters plus NUL, not the full
 * 200-byte array -- both aedoor.library and our emulator cap there (research
 * doc: "Usable String payload is 198 chars + NUL, not 200"; DoorTypes.ts:
 * 93-95; aedoor.library.asm:293-296). ae_put() chunks output at this
 * boundary in BOTH implementations so the two backends behave identically
 * and the boundary condition is exercised by the native tests. */
#define AE_MAX_LINE 198

/* PortStart equivalent (AEDoor.c PortStart(), lines 187-229; Example2.c
 * main(), lines 35-55). Locates AEDoorPort<node> and registers this door
 * with the BBS (JH_REGISTER). node is the value AmiExpress passed as
 * argv[1] (research doc: "Node number arrives as argv[1] only";
 * express.e:4308). Returns 0 on success, non-zero on failure (reply port
 * could not be created, message memory could not be allocated, or the BBS
 * door port was not found -- i.e. not actually running under AmiExpress). */
int ae_start(int node);

/* PutString equivalent (AEDoor.c PutString(), lines 275-281). Sends text
 * with no embedded line terminator expected from the caller; newline != 0
 * asks the BBS to append its own line break after the LAST chunk (research
 * doc: "Send text with NO trailing newline and Data = 1; the BBS appends
 * the break. Never both."). Text longer than AE_MAX_LINE is split across
 * multiple sends -- never truncated.
 *
 * Protocol-avoidance, not cosmetics: text whose first non-whitespace chars
 * are "bbs:" (case-insensitive) is silently rerouted by the BBS to file
 * display instead of being printed (research doc divergence #4; io.ts:
 * 661-667 checks trimmed.toLowerCase().startsWith('bbs:')). A sysop can
 * legitimately configure a download directory as "bbs:doors/", making this
 * a live trap for ordinary status lines, not a hypothetical one. Both
 * implementations guard it automatically -- a leading '.' is inserted so
 * the check's prefix test fails and the line still prints -- so callers
 * never need to remember this for any message they add. The guard is
 * re-checked on EVERY chunk, not just the first: the BBS's reroute check
 * runs on each physical send independently, so a "bbs:" appearing at a
 * later chunk's own start (not the caller's logical string start) is just
 * as vulnerable and gets its own guard byte. */
void ae_put(const char *text, int newline);

/* GetString equivalent (AEDoor.c GetString(), lines 267-273). Reads a line
 * of input into buf. The prompt must already have been emitted via ae_put()
 * -- this call does not prompt (it uses JH_LI, a pre-filled-default read,
 * not JH_PM). Truncates safely at maxlen; buf is always NUL-terminated. */
void ae_get(char *buf, int maxlen);

/* Single keypress read (JH_HK). Returns the key value (0-255), or -1 on
 * carrier loss or console timeout. */
int ae_key(void);

/* Is there input already waiting? (GETKEY, command 500; express.e:3811-3813)
 *
 *   CASE GETKEY
 *     IF checkInput() THEN msg.string[0]:="1" ELSE msg.string[0]:="0"
 *
 * Returns 1 when the user has already typed something, 0 when they have not,
 * and 0 on carrier loss (the next blocking read reports that properly).
 *
 * Crucially it does NOT consume the key - it only answers the question. That
 * is what makes it safe for a door to ask before doing something expensive:
 * an earlier attempt at this used JH_FetchKey, which DOES consume, and so had
 * to decode and push back what it took. That went wrong twice - once on an
 * escape sequence it could not finish without blocking, and once by eating
 * every queued key before redrawing, so the screen never moved while the user
 * was pressing keys.
 *
 * The native backend answers from stdin itself: select() with a two-tick
 * grace, then a one-byte peek that is put back. It has to be real, not a
 * flat 0, because the door reads a lone ESC as a key and tells it from an
 * arrow sequence with exactly this call (flow_decode_escape). */
int ae_input_pending(void);

/* Sleep for `ticks` fiftieths of a second (dos.library Delay()).
 *
 * Used to hold off expensive work for a moment to see whether the user is
 * still typing - a detail pane that costs an HTTP fetch and a screenful of
 * output should not be drawn for a row the cursor is passing over. Delay()
 * is the AmigaOS way to wait without burning the CPU, and this project's
 * emulator honours it (the door is descheduled rather than spun).
 *
 * The native backend returns immediately: its ae_input_pending() carries
 * its own grace period, and it exists to be driven by scripts that should
 * not be slowed. */
void ae_delay_ticks(int ticks);

/* CheckMessage equivalent (AEDoor.c CheckMessage(), lines 154-183), but
 * non-blocking and read-only: returns non-zero once a prior ae_get/ae_key/
 * ae_put round trip has observed Data == -1 -- carrier lost or console
 * timeout (research doc: "Data == -1 from any input call means carrier
 * loss or timeout -- the caller must stop and shut down"). Does not itself
 * perform any protocol exchange. */
int ae_check(void);

/* Ask the BBS to stop swallowing the cursor keys.
 *
 * On a real AmiExpress node an arrow key is marked "control" and consumed
 * by readChar()'s loop, so a door never receives one, until the door
 * toggles the BBS's rawArrow flag (express.e:3814-3815, 7514-7528). With it
 * on, arrows arrive as the single-byte codes 2/3/4/5 (LEFT/RIGHT/UP/DOWN,
 * axconsts.e:75-78) - NOT as escape sequences.
 *
 * Call ae_raw_arrows(1) once after ae_start() if the door reads cursor
 * keys. The backend restores the previous state on every shutdown path, so
 * a door does not have to remember to. Calling it twice with the same
 * argument is a no-op, which matters because the underlying BBS command is
 * a toggle with no way to read the current state.
 *
 * The native backend has no BBS to ask and does nothing. */
void ae_raw_arrows(int on);

/* ShutDown equivalent (AEDoor.c ShutDown(), lines 255-265): the normal,
 * successful end of the door. Notifies the BBS (JH_SHUTDOWN) then
 * terminates the process with exit(0). On the Amiga backend this never
 * returns to the caller; skipping it leaves the BBS waiting forever
 * (research doc: "JH_SHUTDOWN = 2 last and MANDATORY"; AEDoor.c:147-148).
 * The native backend is a no-op that DOES return, since there is no BBS to
 * notify and no reason to end the caller's process early. */
/* Asks the BBS to run `command` once this door exits.
 *
 * AmiExpress's RETURNCOMMAND (express.e:3492-3493, 4064-4065; XIM command
 * 136): the door hands a command name back and the BBS runs it from its own
 * prompt after the door is gone. That is the only order that works - two
 * doors cannot share a node, which is why DOORMAN queues rather than
 * launching inline (Doors/door-manager/run-door.ts says the same thing about
 * the TypeScript side).
 *
 * The BBS refuses a command that would re-enter THIS door, so a door
 * registered under the name it returns cannot loop (door.handler.ts's
 * recursion guard).
 *
 * Call it, then exit. Nothing runs until the door does.
 */
void ae_return_command(const char *command);

void ae_shutdown(void);

/* ---- user and board fields --------------------------------------------
 *
 * One field, one round trip, with the field number as the command - the
 * DT_ and BB_ numbers express.e reads (axcommon.e) and this board's own
 * additions above MAX_CMD. sdk/c/include/ae_session.h names the ones a door
 * is expected to use and is where the numbers are pinned against the
 * board's table.
 *
 * ae_field_read returns the length written (0 for a field the board did not
 * answer), or -1 on carrier loss; `out` is always a valid string.
 * ae_field_write returns 0 when the board took it, -1 on carrier loss. */
int ae_field_read(int field, char *out, int cap);
int ae_field_write(int field, const char *value);

/* DT_QUERYBIT (1002): does the caller pass this ACS check? express.e answers
 * `msg.command:=checkSecurity(msg.data)` (express.e:3857-3858) - the ACS
 * index goes OUT in Data and the answer comes back in Command, the one field
 * read that answers there. Returns 1 or 0, or -1 on carrier loss. A real
 * AmiExpress answers it, so this is how a door learns the board's own access
 * rules rather than inventing a level threshold. */
int ae_query_bit(int acs);

/* One round trip that SENDS a string and READS one back.
 *
 * Neither of the two above can do it, and that is not an oversight in them:
 * ae_field_read sends an EMPTY String (ae_transport_amiga.c:487) because
 * every DT_/BB_ field is "tell me this one thing", and ae_field_write reads
 * nothing back because a field write has no answer. A board command that
 * takes a QUERY - "LIST 40 8" - and answers with TEXT needs both halves of
 * the same message, which is what this is.
 *
 * `request` goes into the message's String (truncated at AE_MAX_LINE, which
 * is the wire's own limit, not a choice); the reply is copied into `out`,
 * always NUL-terminated. Returns the board's msg.Data - the number of bytes
 * it wrote, 0 for "nothing more" and for a classic board that does not know
 * the command at all - or -1 when the carrier is gone.
 *
 * Data is set to READIT (1) on the way out, the same direction flag every
 * field read uses: this is a READ that happens to carry a question.
 */
int ae_field_exchange(int field, const char *request, char *out, int cap);

/* TakeOffEh equivalent (AEDoor.c TakeOffEh(), lines 231-239): fatal-error
 * shutdown. Still notifies the BBS (JH_SHUTDOWN) before terminating -- the
 * mandate applies on every exit path, not just the happy one -- then calls
 * exit(code). Does not return, on either backend. */
void ae_fatal(int code);

#endif /* AEDOOR_H */
