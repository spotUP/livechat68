/*
 * ui_profile - what fits on the screen this caller has.
 *
 * The C twin of the TypeScript layout tiers
 * (sdk/engines/ui/blessed/core/responsive-constants.ts). A C door and a
 * TypeScript door on the same 40-column caller must fold the same way, or
 * the board has two answers to one question.
 *
 * The rule that matters is the XXS tier: a C64 has 40 columns, borders cost
 * two of them, and decoration costs the rest. Everything here follows from
 * that one measurement.
 */

#ifndef UI_PROFILE_H
#define UI_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

/** The width at which the 40-column tier ends (responsive-constants.ts:34). */
#define UI_BREAKPOINT_XXS 41

/** What a layout may spend at this width. */
typedef struct {
    /** Draw borders at all. False at 40 columns, where they cost 5%. */
    int borders;
    /** Stack what would have been columns. */
    int single_column;
    /** Masthead and footer collapse to one line. */
    int collapse_chrome;
    /** Cells between neighbours. */
    int gap;
    /** Cells inside a frame. */
    int padding;
    /**
     * A cell may carry its own background colour.
     *
     * False at the 40-column tier, because that tier is a C64: the VIC-II
     * has ONE screen background and per-cell background is dropped on the
     * way out (sdk/petscii/ansi-to-petscii.ts, "Per-cell background has no
     * C64 equivalent and is dropped"). A door that marks a selected row
     * with a background bar therefore marks nothing at all there - the
     * sysop, 2026-09-06: "i see no selected line". Reverse video is what a
     * C64 has instead, and it survives the transducer as $12/$92.
     *
     * The TypeScript side states the same rule per door (the C64 sprite
     * variants paint bg 0); this is where the C side keeps it.
     */
    int cell_backgrounds;
    /**
     * Mark a selected row with a caret rather than a bar.
     *
     * True at 40 columns, where a full-width bar costs the whole row and
     * the TypeScript doors use ">>" instead.
     */
    int caret_selection;
    /**
     * The terminal erases to the end of a line in the colour in force (CSI K), so a cell's blank tail can be one
     * erase instead of dozens of spaces. True where cell_backgrounds is: an ANSI terminal. False at 40 columns -
     * the C64 transducer fills an erase with its own idea of the colour, and the 40-column grid has no tail worth
     * saving - so a door there keeps writing the spaces.
     */
    int line_erase;
    /**
     * The caller takes colour. The board answers it from session.ansiMode
     * (DT_ISANSI, field 541), and M at the prompt turns it off.
     *
     * With colour off the board strips every SGR on the way out - inverse
     * too, the way express.e's aePuts2 does - so a bar or a reverse cell is
     * gone and only characters are left. ui_screen asks once, at open, and
     * keeps the answer across every later measure; when it is 0 the screen
     * also forces caret_selection, so a selected row is marked with ">>".
     * ui_profile_for answers 1: width alone cannot know.
     */
    int colour;
} ui_profile;

/** The tier for a width. Always pass the LIVE width, never a constant. */
ui_profile ui_profile_for(int width);

/** True at the 40-column tier. */
int ui_is_compact_width(int width);

/**
 * Whether decoration may run at this width.
 *
 * A 40x25 screen has no spare cells for a glitch or a marquee, and running
 * them there leaves stray glyphs mid-row - measured on the PETSCII canvas,
 * 2026-09-02. Doors gate their effect setup on this.
 *
 * Also 0 at any width when the caller turned animations off (W row 23),
 * which ae_open hands over through ui_set_board_effects.
 */
int ui_effects_allowed(int width);

/** The board's animations answer (ae_animations_allowed); 1 until set. */
void ui_set_board_effects(int on);

#ifdef __cplusplus
}
#endif

#endif /* UI_PROFILE_H */
