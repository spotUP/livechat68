/*
 * ui_theme - see ui_theme.h. The table is generated; this is the lookup.
 */

#include "ui_theme.h"
#include "theme_tables.h"

#include <string.h>

int ui_theme_count(void)
{
    return UI_THEME_COUNT;
}

const ui_theme *ui_theme_at(int index)
{
    if (index < 0 || index >= UI_THEME_COUNT) return &UI_THEME_TABLE[0];
    return &UI_THEME_TABLE[index];
}

const ui_theme *ui_theme_by_id(const char *id)
{
    int i;

    /* No id, or one this board does not have (a saved "classic" among
       them), is entry 0 - the default, Uprough Neon, which is the same
       answer the TypeScript themeById() gives and the same rule ae_host
       takes for an unknown host. */
    if (!id || !*id) return &UI_THEME_TABLE[0];

    for (i = 0; i < UI_THEME_COUNT; i++) {
        if (strcmp(UI_THEME_TABLE[i].id, id) == 0) return &UI_THEME_TABLE[i];
    }
    return &UI_THEME_TABLE[0];
}
