/*
 * ui_theme - the board's themes, at the eight colours a C door has.
 *
 * The sysop's answer to "must a C door look like its TypeScript twin?" was
 * yes - "the identity of our doors is important" (2026-09-02) - so all seven
 * themes are here, not just classic.
 *
 * THE TABLE IS GENERATED, never hand-edited: sdk/engines/ui/theme/tokens.ts
 * stays the one place a theme is defined, and tools/generate-theme-tables.ts
 * reduces each token to one of eight colours. `make check-themes` fails when
 * the header is stale, so the two cannot drift in silence.
 *
 * What does not survive the reduction, and why that is honest: `double`
 * borders (ansi_box draws one weight of line, so double degrades to line) and the exact
 * shades (a hex ramp has no 8-colour equivalent). What DOES survive is the
 * identity - phosphor stays green, neon stays magenta - which is the part a
 * caller recognises.
 */

#ifndef UI_THEME_H
#define UI_THEME_H

#include "ui_ansi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_BORDER_LINE 0
#define UI_BORDER_NONE 1

/**
 * One theme. The thirteen tokens are exactly ThemeTokens
 * (sdk/engines/ui/theme/tokens.ts), so the two implementations describe the
 * same roles rather than two vocabularies.
 */
typedef struct {
    const char *id;
    /**
     * What a caller is shown: "Slate & Slash", not "slate-slash".
     *
     * The TypeScript picker has listed themes by name since it was written
     * (Doors/theme-picker/app.ts, buildThemeItems), and the C one listed
     * ids because the table had nothing else in it - the sysop put the two
     * screens side by side on 2026-09-06 and that was the first difference.
     */
    const char *name;
    /** The one-line description beside the name at 80 columns. */
    const char *blurb;
    /**
     * The nearest of the terminal's sixteen. Still here, and still what a
     * caller gets on a terminal that cannot do better - a C64 reduces to the
     * VIC palette anyway, so this is the honest answer there.
     */
    unsigned char ground, ink, chrome, dim, bar, bar_ink;
    unsigned char accent, accent_alt, selection_bg, selection_ink;
    unsigned char ok, warn, alert;
    /**
     * And the colour the theme actually IS, 0xRRGGBB per token, in the order
     * of ui_token below.
     *
     * The TypeScript writes the hex straight into its SGR; the C wrote the
     * nearest of sixteen, so the two doors were the right HUE and the wrong
     * shade on a web terminal (sysop, 2026-09-07). Handing this array to
     * ansi_set_palette() makes a C door write `38;2;r;g;b` for exactly the
     * same colour - and the PETSCII transducer maps truecolour to the
     * nearest VIC on the way to a C64, so nothing is lost there either.
     */
    unsigned long rgb[13];
    /**
     * The same thirteen as the nearest of sixteen, in ui_token order.
     *
     * A token drawn without a palette has to land somewhere sensible rather
     * than on colour 106; this is where. Same numbers as the named fields
     * above, in an array a lookup can index.
     */
    unsigned char idx[13];
    int border;
    /** The ASCII branding mark, or "" for a theme with none. */
    const char *rail;
} ui_theme;

/**
 * The thirteen tokens, in the order `rgb` holds them - ThemeTokens' own
 * order (sdk/engines/ui/theme/tokens.ts).
 */
typedef enum {
    UI_T_GROUND = 0, UI_T_INK, UI_T_CHROME, UI_T_DIM, UI_T_BAR, UI_T_BAR_INK,
    UI_T_ACCENT, UI_T_ACCENT_ALT, UI_T_SELECTION_BG, UI_T_SELECTION_INK,
    UI_T_OK, UI_T_WARN, UI_T_ALERT,
    UI_TOKEN_COUNT
} ui_token;

/**
 * A theme by id, or the default (entry 0, Uprough Neon) for an id this
 * board does not have.
 *
 * Never NULL: a door asks once at startup and then reads tokens off the
 * result, and a null check at every one of those would be noise around a
 * case that means "draw the board's default".
 */
const ui_theme *ui_theme_by_id(const char *id);

/** How many themes there are, and one by index - for a picker. */
int ui_theme_count(void);
const ui_theme *ui_theme_at(int index);

#ifdef __cplusplus
}
#endif

#endif /* UI_THEME_H */
