/*
 * ui_profile - the tiers, matching responsive-constants.ts value for value.
 *
 * Kept deliberately dull: five fields, one threshold, no policy of its own.
 * When the TypeScript side moves a number, this file moves with it and
 * tests/test_ui_profile.c is what says they still agree.
 */

#include "ui_profile.h"

/* responsive-constants.ts:100-109 */
#define UI_DEFAULT_GAP     1
#define UI_DEFAULT_PADDING 1
#define UI_MOBILE_GAP      0
#define UI_MOBILE_PADDING  0

int ui_is_compact_width(int width)
{
    return width < UI_BREAKPOINT_XXS;
}

ui_profile ui_profile_for(int width)
{
    ui_profile p;
    int xxs = ui_is_compact_width(width);

    p.borders = !xxs;
    p.single_column = xxs;
    p.collapse_chrome = xxs;
    p.gap = xxs ? UI_MOBILE_GAP : UI_DEFAULT_GAP;
    p.padding = xxs ? UI_MOBILE_PADDING : UI_DEFAULT_PADDING;
    p.cell_backgrounds = !xxs;
    p.caret_selection = xxs;
    p.line_erase = !xxs;
    p.colour = 1;
    return p;
}

/* The board's answer (ae_open asks AE_FIELD_ANIMATIONS once per run). */
static int board_effects = 1;

void ui_set_board_effects(int on)
{
    board_effects = on ? 1 : 0;
}

int ui_effects_allowed(int width)
{
    return board_effects && !ui_is_compact_width(width);
}
