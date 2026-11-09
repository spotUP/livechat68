/*
 * ui_chrome - the three bars a door wears: masthead, footer, status.
 *
 * A door's screen is a list in the middle and bars around it. The bars are
 * where doors have historically drifted apart, because each one hand-rolled
 * its own width arithmetic and its own truncation, and the failures are not
 * cosmetic: DoorRepo's footer once dropped Q=Quit on any row that had ads
 * AND a doc, because it concatenated everything and cut at `cols`.
 *
 * So the width budget lives here, and it has one guarantee: the SUFFIX is
 * never dropped. A narrow screen loses optional keys, highest priority
 * first, and never the one that gets you out.
 */

#ifndef UI_CHROME_H
#define UI_CHROME_H

#include "ui_ansi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Build a footer line that fits `cols`.
 *
 * `prefix` and `suffix` are mandatory - the suffix is appended whatever the
 * width, which is the "never silently drop Q=Quit" rule. `optional` is in
 * PRIORITY ORDER: the first part that does not fit stops the loop, and
 * later parts are not tried in its place, because a shorter part appearing
 * where a longer higher-priority one was dropped would invert the ordering
 * the caller expressed.
 *
 * Returns the length written, or -1 when `out` is too small - which is a
 * programming error, not a narrow screen.
 *
 * Lifted from examples/doorrepo-c/flow.c:1887, where it has been the rule
 * for DoorRepo's footer.
 */
int ui_footer_build(char *out, unsigned long cap, int cols,
                    const char *prefix,
                    const char *const *optional, int optional_count,
                    const char *suffix);

/**
 * Draw a full-width bar: `text` on the left, the rest filled with the bar's
 * colour so it reads as one band rather than a coloured word.
 */
void ui_bar_draw(ansi_buf *b, int row, int left, int cols,
                 const char *text, int fg, int bg);

/**
 * The masthead: a title on the left and the theme's rail filling the row.
 *
 * The rail is branding, not information, so it is what gets cut when the
 * row is narrow - never the title. A rail of "" draws a plain bar, which is
 * what `classic` (and any 40-column caller) gets.
 */
void ui_masthead_draw(ansi_buf *b, int row, int left, int cols,
                      const char *title, const char *rail, int fg, int bg);

/**
 * One window onto the rail's stream, `width` cells of it, `offset` along.
 *
 * A 1:1 port of railStream() in sdk/engines/ui/theme/chrome.ts, down to the
 * generator: state = state * 1664525 + 1013904223 (mod 2^32), runs of 2 to 8
 * marks separated by 1 to 3 spaces, a ring at least twice the width and
 * never shorter than 64, doubled so a window near the end wraps cleanly.
 *
 * Same seed, same bytes - which is the point. The masthead a C door draws
 * has to be the masthead a TypeScript door draws, or the board has two
 * brandings (sysop, 2026-09-06: "reference the blessed sdk and do it 1:1").
 * tests/test_ui_chrome.c pins the C output against strings taken from the
 * TypeScript.
 *
 * A rail of "///" is not tiled: the movement people see is the GAPS
 * travelling, which is what a stream has and a repeated pattern does not.
 */
void ui_rail_stream(const char *rail, int width, int offset,
                    unsigned long seed, char *out, unsigned long cap);

/**
 * The same masthead, with the rail shifted `tick` cells along.
 *
 * One frame of the slide the TypeScript chrome runs (railFrame(),
 * engines/ui/theme/chrome.ts): the period is the rail's own length, so no
 * frame is ever empty - a gap reads as the branding having broken rather
 * than as movement. `tick` 0 is what ui_masthead_draw draws.
 *
 * A door calls this from its idle hook (ui_key.h) because a 68K door has no
 * timers; every frame costs an XIM message, so a tick of four or five a
 * second is the sensible rate, not twenty.
 */
void ui_masthead_draw_tick(ansi_buf *b, int row, int left, int cols,
                           const char *title, const char *rail,
                           int fg, int bg, int tick);

/**
 * Is there anything for the idle hook to animate?
 *
 * A theme with NO rail draws ` title ` and nothing else, so every tick paints
 * the identical row - five times a second, 172 bytes a frame, and on a real
 * terminal the text under the repaint flickers. The sysop, 2026-09-15, in
 * THEMEC and REQUESTS: "theme and uests are blinking" - the tail of each
 * masthead title, being cleared and redrawn about five times a second with
 * nothing changing.
 *
 * The rule lives here rather than in each door because four of them have the
 * same idle hook: ask before painting, and a themeless caller gets a still
 * masthead instead of a blinking one. The rail themes animate exactly as
 * before.
 */
int ui_masthead_rail_animates(const char *rail);

/**
 * ONE FRAME OF THE RAIL, and nothing else on the row.
 *
 * ui_masthead_draw_tick draws the whole masthead - fill, rail, title - which
 * is right when the screen has just been cleared and wrong five times a
 * second: the title does not move, so repainting it is what the caller sees
 * as the title blinking. The sysop, 2026-09-15: "themec still blinks", with
 * the rail theme on, and it is the tail of "DOOR THEME" that flickers.
 *
 * So the idle hook calls THIS, which writes only the run's own cells. A view
 * change still calls ui_masthead_draw_tick, because then the row really is
 * new. Both take the run's width from one place, so the animation cannot
 * creep onto the title.
 */
void ui_masthead_rail_tick(ansi_buf *b, int row, int left, int cols,
                           const char *title, const char *rail,
                           int fg, int bg, int tick);

/**
 * How long a door waits between rail frames, in Amiga ticks (50 a second).
 *
 * ONE definition: the four doors with an idle hook had the same literal 10,
 * so the board's animation speed was a thing you changed in four places and
 * got wrong in one. The sysop, 2026-09-15: "the rails animate too slow".
 *
 * The rail moves one cell per frame - a terminal cannot move less than a
 * whole cell - so smoothness is entirely frame RATE. Three ticks asks for
 * about sixteen frames a second and MEASURES 8.4 on the board's emulator,
 * which pays roughly 45ms per XIM message: the delay is a floor, not a
 * promise.
 *
 * What pays for it is the frame being the rail's run alone, about a hundred
 * bytes. Measured on THEMEC, `slate-slash`, over nine idle seconds:
 *
 *     the old full-row frame, 10 ticks   3.7 fps   ~860 bytes/s
 *     the run alone, 5 ticks             6.2 fps    467 bytes/s
 *     the run alone, 3 ticks             8.4 fps    633 bytes/s
 *
 * So this is faster than the board has ever drawn it AND cheaper than it
 * used to cost. Below three a real Amiga starts spending its time on
 * decoration: at ~45ms a frame the door would draw more often than it looks
 * for a keypress.
 */
#define UI_RAIL_TICK_DELAY 3

/**
 * How far the rail travels per frame, in cells.
 *
 * ONE. The sysop looked at two and at three and chose one: "looks like shit
 * with 2 steps". A multi-cell step is the only way to make the rail cross the
 * row faster, and it reads as the pattern JUMPING rather than sliding, which
 * is worse than slow.
 *
 * So the rail's speed is the frame rate, and the frame rate has a ceiling
 * that is not ours: the emulator pays about 45ms per XIM message, so a door
 * asking for fifty frames a second measures 10.8 and one asking for sixteen
 * measures 10.2. Nothing here can beat that - measured, not assumed, by
 * building at delay 1 and delay 3 and reading the frames off the wire.
 *
 *     step 1   10.8 fps   ~11 cells/s   817 bytes/s   chosen
 *     step 2   10.3 fps   ~21 cells/s   780 bytes/s
 *     step 3   10.2 fps   ~31 cells/s   774 bytes/s
 *
 * If the rail ever needs to be genuinely faster AND smooth, the thing to
 * attack is the 45ms, not this number.
 */
#define UI_RAIL_STEP 1

/**
 * The status line: left text, right text, and the gap between them filled.
 *
 * The right side is dropped rather than overlapped when both cannot fit -
 * two strings colliding mid-row is the shape a caller reads as corruption.
 */
void ui_status_draw(ansi_buf *b, int row, int left, int cols,
                    const char *left_text, const char *right_text,
                    int fg, int bg);

#ifdef __cplusplus
}
#endif

#endif /* UI_CHROME_H */
