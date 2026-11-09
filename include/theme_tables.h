/*
 * theme_tables.h - GENERATED. Do not edit.
 *
 * Written by sdk/c/tools/generate-theme-tables.ts from
 * sdk/engines/ui/theme/tokens.ts, which stays the single source of truth for
 * what a theme IS. Each token is reduced to the nearest of the eight colours
 * a C door has, by RGB distance, so a theme keeps its intent - phosphor stays
 * green, neon stays magenta - without pretending the Amiga can render the
 * exact shade.
 *
 * Regenerate:  cd sdk && npx tsx c/tools/generate-theme-tables.ts
 * Check:       ... --check   (fails when this file is stale)
 */

#ifndef UI_THEME_TABLES_H
#define UI_THEME_TABLES_H

#define UI_THEME_COUNT 6

static const ui_theme UI_THEME_TABLE[UI_THEME_COUNT] = {
    {
        "uprough-neon",
        "Uprough Neon",
        "Demoscene magenta and cyan, double-ruled, masthead slashes.",
        /* ground        */ ANSI_BLACK,    /* black -> 0 */
        /* ink           */ ANSI_BRIGHT_WHITE, /* #E4ECFA -> 15 */
        /* chrome        */ ANSI_BRIGHT_CYAN, /* #4DE0F0 -> 14 */
        /* dim           */ ANSI_GRAY,     /* #48566E -> 8 */
        /* bar           */ ANSI_BLACK,    /* black -> 0 */
        /* bar_ink       */ ANSI_BRIGHT_MAGENTA, /* #FF3D9A -> 13 */
        /* accent        */ ANSI_BRIGHT_MAGENTA, /* #FF3D9A -> 13 */
        /* accent_alt    */ ANSI_BRIGHT_YELLOW, /* #F5C451 -> 11 */
        /* selection_bg  */ ANSI_MAGENTA,  /* magenta -> 5 */
        /* selection_ink */ ANSI_BLACK,    /* black -> 0 */
        /* ok            */ ANSI_BRIGHT_GREEN, /* #57E389 -> 10 */
        /* warn          */ ANSI_BRIGHT_YELLOW, /* #F5C451 -> 11 */
        /* alert         */ ANSI_BRIGHT_RED, /* #FF5C7A -> 9 */
        /* The exact colours, in ui_token order. */
        { 0x000000UL, 0xe4ecfaUL, 0x4de0f0UL, 0x48566eUL, 0x000000UL, 0xff3d9aUL, 0xff3d9aUL, 0xf5c451UL, 0xaa00aaUL, 0x000000UL, 0x57e389UL, 0xf5c451UL, 0xff5c7aUL },
        /* And the nearest of sixteen, same order, for a screen with no palette. */
        { 0, 15, 14, 8, 0, 13, 13, 11, 5, 0, 10, 11, 9 },
        UI_BORDER_LINE,
        "/////"
    },
    {
        "neon-muted",
        "Uprough Neon (muted)",
        "The neon frame, but the colour spent only where it counts.",
        /* ground        */ ANSI_BLACK,    /* black -> 0 */
        /* ink           */ ANSI_WHITE,    /* #A8B4C8 -> 7 */
        /* chrome        */ ANSI_CYAN,     /* #2E6E7A -> 6 */
        /* dim           */ ANSI_GRAY,     /* #5A6474 -> 8 */
        /* bar           */ ANSI_BLACK,    /* black -> 0 */
        /* bar_ink       */ ANSI_BRIGHT_MAGENTA, /* #FF3D9A -> 13 */
        /* accent        */ ANSI_BRIGHT_MAGENTA, /* #FF3D9A -> 13 */
        /* accent_alt    */ ANSI_GRAY,     /* #5A6474 -> 8 */
        /* selection_bg  */ ANSI_MAGENTA,  /* magenta -> 5 */
        /* selection_ink */ ANSI_BLACK,    /* black -> 0 */
        /* ok            */ ANSI_WHITE,    /* #A8B4C8 -> 7 */
        /* warn          */ ANSI_WHITE,    /* #A8B4C8 -> 7 */
        /* alert         */ ANSI_BRIGHT_MAGENTA, /* #FF3D9A -> 13 */
        /* The exact colours, in ui_token order. */
        { 0x000000UL, 0xa8b4c8UL, 0x2e6e7aUL, 0x5a6474UL, 0x000000UL, 0xff3d9aUL, 0xff3d9aUL, 0x5a6474UL, 0xaa00aaUL, 0x000000UL, 0xa8b4c8UL, 0xa8b4c8UL, 0xff3d9aUL },
        /* And the nearest of sixteen, same order, for a screen with no palette. */
        { 0, 7, 6, 8, 0, 13, 13, 8, 5, 0, 7, 7, 13 },
        UI_BORDER_LINE,
        "/////"
    },
    {
        "slate-slash",
        "Slate & Slash",
        "Quiet slate chrome, one magenta accent, room to breathe.",
        /* ground        */ ANSI_BLACK,    /* black -> 0 */
        /* ink           */ ANSI_BRIGHT_WHITE, /* #C9D4E8 -> 15 */
        /* chrome        */ ANSI_GRAY,     /* #48566E -> 8 */
        /* dim           */ ANSI_GRAY,     /* #48566E -> 8 */
        /* bar           */ ANSI_BLACK,    /* black -> 0 */
        /* bar_ink       */ ANSI_BRIGHT_WHITE, /* #C9D4E8 -> 15 */
        /* accent        */ ANSI_BRIGHT_MAGENTA, /* #FF3D9A -> 13 */
        /* accent_alt    */ ANSI_BRIGHT_CYAN, /* #4DE0F0 -> 14 */
        /* selection_bg  */ ANSI_MAGENTA,  /* magenta -> 5 */
        /* selection_ink */ ANSI_BLACK,    /* black -> 0 */
        /* ok            */ ANSI_BRIGHT_GREEN, /* #57E389 -> 10 */
        /* warn          */ ANSI_BRIGHT_YELLOW, /* #F5C451 -> 11 */
        /* alert         */ ANSI_BRIGHT_RED, /* #FF5C7A -> 9 */
        /* The exact colours, in ui_token order. */
        { 0x000000UL, 0xc9d4e8UL, 0x48566eUL, 0x48566eUL, 0x000000UL, 0xc9d4e8UL, 0xff3d9aUL, 0x4de0f0UL, 0xaa00aaUL, 0x000000UL, 0x57e389UL, 0xf5c451UL, 0xff5c7aUL },
        /* And the nearest of sixteen, same order, for a screen with no palette. */
        { 0, 15, 8, 8, 0, 15, 13, 14, 5, 0, 10, 11, 9 },
        UI_BORDER_LINE,
        "///"
    },
    {
        "slate-muted",
        "Slate & Slash (muted)",
        "Near-monochrome. One accent, spent on the selection.",
        /* ground        */ ANSI_BLACK,    /* black -> 0 */
        /* ink           */ ANSI_WHITE,    /* #95A0B4 -> 7 */
        /* chrome        */ ANSI_GRAY,     /* #3A4354 -> 8 */
        /* dim           */ ANSI_GRAY,     /* #5A6474 -> 8 */
        /* bar           */ ANSI_BLACK,    /* black -> 0 */
        /* bar_ink       */ ANSI_BRIGHT_MAGENTA, /* #FF3D9A -> 13 */
        /* accent        */ ANSI_BRIGHT_MAGENTA, /* #FF3D9A -> 13 */
        /* accent_alt    */ ANSI_GRAY,     /* #5A6474 -> 8 */
        /* selection_bg  */ ANSI_MAGENTA,  /* magenta -> 5 */
        /* selection_ink */ ANSI_BLACK,    /* black -> 0 */
        /* ok            */ ANSI_WHITE,    /* #95A0B4 -> 7 */
        /* warn          */ ANSI_WHITE,    /* #95A0B4 -> 7 */
        /* alert         */ ANSI_BRIGHT_MAGENTA, /* #FF3D9A -> 13 */
        /* The exact colours, in ui_token order. */
        { 0x000000UL, 0x95a0b4UL, 0x3a4354UL, 0x5a6474UL, 0x000000UL, 0xff3d9aUL, 0xff3d9aUL, 0x5a6474UL, 0xaa00aaUL, 0x000000UL, 0x95a0b4UL, 0x95a0b4UL, 0xff3d9aUL },
        /* And the nearest of sixteen, same order, for a screen with no palette. */
        { 0, 7, 8, 8, 0, 13, 13, 8, 5, 0, 7, 7, 13 },
        UI_BORDER_LINE,
        "///"
    },
    {
        "quiet-phosphor",
        "Quiet Phosphor",
        "One phosphor hue, no borders, hierarchy by brightness alone.",
        /* ground        */ ANSI_BLACK,    /* black -> 0 */
        /* ink           */ ANSI_BRIGHT_GREEN, /* #57E389 -> 10 */
        /* chrome        */ ANSI_GRAY,     /* #2E4A3C -> 8 */
        /* dim           */ ANSI_GRAY,     /* #2E4A3C -> 8 */
        /* bar           */ ANSI_BLACK,    /* black -> 0 */
        /* bar_ink       */ ANSI_BRIGHT_GREEN, /* #57E389 -> 10 */
        /* accent        */ ANSI_BRIGHT_GREEN, /* #8CFFB4 -> 10 */
        /* accent_alt    */ ANSI_BRIGHT_YELLOW, /* #F5C451 -> 11 */
        /* selection_bg  */ ANSI_GREEN,    /* green -> 2 */
        /* selection_ink */ ANSI_BLACK,    /* black -> 0 */
        /* ok            */ ANSI_BRIGHT_GREEN, /* #57E389 -> 10 */
        /* warn          */ ANSI_BRIGHT_YELLOW, /* #F5C451 -> 11 */
        /* alert         */ ANSI_BRIGHT_RED, /* #FF8B6B -> 9 */
        /* The exact colours, in ui_token order. */
        { 0x000000UL, 0x57e389UL, 0x2e4a3cUL, 0x2e4a3cUL, 0x000000UL, 0x57e389UL, 0x8cffb4UL, 0xf5c451UL, 0x00aa00UL, 0x000000UL, 0x57e389UL, 0xf5c451UL, 0xff8b6bUL },
        /* And the nearest of sixteen, same order, for a screen with no palette. */
        { 0, 10, 8, 8, 0, 10, 10, 11, 2, 0, 10, 11, 9 },
        UI_BORDER_NONE,
        "////"
    },
    {
        "phosphor-muted",
        "Quiet Phosphor (muted)",
        "One hue and nothing else. Brightness alone carries the hierarchy.",
        /* ground        */ ANSI_BLACK,    /* black -> 0 */
        /* ink           */ ANSI_BRIGHT_GREEN, /* #57E389 -> 10 */
        /* chrome        */ ANSI_GRAY,     /* #1E3A28 -> 8 */
        /* dim           */ ANSI_GRAY,     /* #2E4A3C -> 8 */
        /* bar           */ ANSI_BLACK,    /* black -> 0 */
        /* bar_ink       */ ANSI_BRIGHT_GREEN, /* #8CFFB4 -> 10 */
        /* accent        */ ANSI_BRIGHT_GREEN, /* #8CFFB4 -> 10 */
        /* accent_alt    */ ANSI_GRAY,     /* #2E4A3C -> 8 */
        /* selection_bg  */ ANSI_BLACK,    /* black -> 0 */
        /* selection_ink */ ANSI_BRIGHT_GREEN, /* #C8FFDC -> 10 */
        /* ok            */ ANSI_BRIGHT_GREEN, /* #57E389 -> 10 */
        /* warn          */ ANSI_BRIGHT_GREEN, /* #8CFFB4 -> 10 */
        /* alert         */ ANSI_BRIGHT_GREEN, /* #C8FFDC -> 10 */
        /* The exact colours, in ui_token order. */
        { 0x000000UL, 0x57e389UL, 0x1e3a28UL, 0x2e4a3cUL, 0x000000UL, 0x8cffb4UL, 0x8cffb4UL, 0x2e4a3cUL, 0x000000UL, 0xc8ffdcUL, 0x57e389UL, 0x8cffb4UL, 0xc8ffdcUL },
        /* And the nearest of sixteen, same order, for a screen with no palette. */
        { 0, 10, 8, 8, 0, 10, 10, 8, 0, 10, 10, 10, 10 },
        UI_BORDER_NONE,
        "////"
    }
};

#endif /* UI_THEME_TABLES_H */
