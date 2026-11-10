/* test_livechat_ui.c - draws the LIVECHAT68 main chat state and compares every cell
 * (character, fg, bg, bold) with grids captured from the TS LIVECHAT door. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_cells.h"
#include "livechat_ui.h"
#include "ui_theme.h"

static void add_line(lc_model *m, int kind, const char *time, const char *nick, const char *text)
{
    lc_line *l = &m->lines[m->nlines++];
    memset(l, 0, sizeof *l);
    l->kind = kind;
    strcpy(l->time, time);
    strcpy(l->nick, nick);
    strcpy(l->text, text);
    lc_lines_changed(m);
}

static void add_side(lc_model *m, int kind, int joined, const char *label)
{
    lc_side *s = &m->side[m->nside++];
    memset(s, 0, sizeof *s);
    s->kind = kind;
    s->joined = joined;
    s->open = (char)(kind == LC_SIDE_ROOM && joined);   /* door: the active room is expanded (livechat.c isExpanded) */
    strcpy(s->label, label);
}

static void main_state(lc_model *m, int cols, int rows)
{
    lc_model_init(m, cols, rows);
    m->focus = -1;                 /* the TS grids were captured with no panel focused: every border idle */
    strcpy(m->handle, "Guest");
    strcpy(m->room, "general");
    strcpy(m->pane_label, "general - General chat");
    add_side(m, LC_SIDE_HEAD, 0, "TEXT CHANNELS");
    add_side(m, LC_SIDE_ROOM, 1, "general");
    add_side(m, LC_SIDE_USER, 0, "carol");
    add_side(m, LC_SIDE_ROOM, 0, "amiga");
    add_side(m, LC_SIDE_ROOM, 0, "secret");
    m->side_sel = 1;
    add_line(m, LC_SYSTEM, "", "", "Found 3 rooms");
    add_line(m, LC_SYSTEM, "", "", "Auto-joining #general...");
    add_line(m, LC_SYSTEM, "", "", "Joined #general (3 users)");
    add_line(m, LC_ACTIVITY_LINE, "17:03", "", "Joined #general");
    add_line(m, LC_SYSTEM, "", "", "--- 2 earlier messages ---");
    add_line(m, LC_HISTORY, "11:01", "alice", "hello world");
    add_line(m, LC_HISTORY, "11:02", "bob", "this is a long line that should wrap around the chat pane width for sure yes indeed");
    add_line(m, LC_LIVE, "17:03", "alice", "hi sysop, how are you?");
    add_line(m, LC_LIVE, "17:03", "bob", "nice **bold** and :D");
    add_line(m, LC_SYSTEM, "", "", "carol joined the room");
    add_line(m, LC_ACTIVITY_LINE, "17:03", "", "carol joined");
    add_line(m, LC_SYSTEM, "", "", "bob left the room");
    add_line(m, LC_ACTIVITY_LINE, "17:03", "", "bob left");
}

/* The grids were captured from the TS door when its colours were fixed numbers. The door now draws the active
 * theme's tokens (View > Theme), so each recorded number is read as the role it stood for and compared against
 * that role's colour on the running theme. -1 (the terminal's own background) stays. */
static int fixture_colour(int old)
{
    switch (old) {
    case 255: case 7: case 3: case 195: return LC_INK;     /* text; label bar text; ink thumb */
    case 204: return LC_ACCENT;
    case 196: return LC_ALERT;
    case 240: case 66: return LC_DIM;                      /* dim text; scrollbar track */
    case 221: return LC_ACTIVITY;
    case 78: return LC_OK;
    case 0: return LC_BAR_INK;
    case 4: case 16: return lc_tone(LC_TK_BAR);            /* label bar ground; black bar */
    default: return old;
    }
}

/* Returns the number of cells that differ; prints the first few. */
static int compare(const char *path)
{
    static char line[4096];
    FILE *f = fopen(path, "r");
    int cols = 0, rows = 0, row = -1, diff = 0, shown = 0, cr, cc;
    ui_cells c;
    lc_model *m = (lc_model *)malloc(sizeof *m);
    if (f == 0 || m == 0 || fgets(line, sizeof line, f) == 0 || sscanf(line, "%d %d", &cols, &rows) != 2) {
        printf("FAIL cannot read %s\n", path);
        return -1;
    }
    ui_cells_open(&c, cols, rows);
    main_state(m, cols, rows);
    lc_draw(&c, m, &cr, &cc);
    while (fgets(line, sizeof line, f) != 0) {
        if (line[0] == 'T') {
            int k;
            row++;
            for (k = 0; k < cols; k++) {
                char want = line[1 + k] == '\n' || line[1 + k] == '\0' ? ' ' : line[1 + k];
                ui_cell *got = &c.back[row * cols + k];
                if (row == rows - 1 && k == cols - 1) break;   /* ui_cells_flush never sends the bottom-right cell */
                if ((char)got->ch != want) {
                    diff++;
                    if (shown++ < 6) printf("  %s r%d c%d char want '%c' got '%c'\n", path, row, k, want, got->ch);
                }
                if (line[1 + k] == '\n' || line[1 + k] == '\0') break;
            }
        } else if (line[0] == 'A') {
            char *p = line + 1;
            int a, b, fg, bg, bold, n, k;
            while (sscanf(p, "%d-%d:%d/%d/%d%n", &a, &b, &fg, &bg, &bold, &n) == 5) {
                for (k = a; k <= b && k < cols; k++) {
                    ui_cell *got = &c.back[row * cols + k];
                    int gbg = (got->flags & UI_CELLS_BG_DEFAULT) ? -1 : got->bg;
                    int gbold = (got->flags & UI_CELLS_BOLD) ? 1 : 0;
                    fg = fixture_colour(fg); bg = fixture_colour(bg);
                    if (got->ch == ' ' && bg == gbg) continue;   /* fg of a blank cell does not show */
                    if (got->fg != fg || gbg != bg || gbold != bold) {
                        diff++;
                        if (shown++ < 6) printf("  %s r%d c%d attr want %d/%d/%d got %d/%d/%d\n", path, row, k, fg, bg, bold, got->fg, gbg, gbold);
                    }
                }
                p += n;
                while (*p == ' ') p++;
            }
        }
    }
    fclose(f);
    ui_cells_close(&c);
    free(m);
    printf("%s: %d differing cells\n", path, diff);
    return diff;
}

/* Reactions: one glyph and a count each, on their own rows under the message, indented to the
 * message text column, wrapping at the pane width; no row at all when nothing has a count.
 * (The TS door still prints the old ' [<3 2] [*F* 1]' suffix; the port spec is in the handoff.) */
static int react_rows_under_the_message(void)
{
    static lc_model m;
    lc_line l;
    char out[256];
    ui_cells c;
    int cr, cc, r, k, idx, fail = 0, found = -1, col = -1;
    memset(&l, 0, sizeof l);
    l.kind = LC_LIVE;
    strcpy(l.time, "17:03"); strcpy(l.nick, "bob"); strcpy(l.text, "hi");
    strcpy(l.react[0].emoji, "heart"); l.react[0].count = 2;
    strcpy(l.react[1].emoji, "fire"); l.react[1].count = 1;
    l.nreact = 2;
    lc_react_rows(&l, 13, 0, out, sizeof out);
    if (strcmp(out, "\n             <3 2  * 1") != 0) { printf("react rows: '%s'\n", out); fail = 1; }
    lc_react_rows(&l, 4, 10, out, sizeof out);                      /* "<3 2" fits, "  * 1" would pass 10 */
    if (strcmp(out, "\n    <3 2\n    * 1") != 0) { printf("react wrap: '%s'\n", out); fail = 1; }
    if (strcmp(lc_react_label(2), "<3  Heart") != 0) { printf("react label: '%s'\n", lc_react_label(2)); fail = 1; }
    l.react[0].count = 0; l.react[1].count = 0;
    lc_react_rows(&l, 4, 80, out, sizeof out);
    if (out[0] != '\0') { printf("react rows, zero counts: '%s'\n", out); fail = 1; }
    lc_line_marked(&l, out, sizeof out, 1, 60);
    if (strchr(out, '\n') != 0) { printf("react: a row with no counts\n"); fail = 1; }
    if (strcmp(lc_react_label(0), ":)  Thumbs Up") != 0 || strcmp(lc_react_label(2), "<3  Heart") != 0
        || strcmp(lc_react_display("angry"), ">(") != 0 || strcmp(lc_react_display("zzz"), "zzz") != 0) {
        printf("react labels: '%s' '%s'\n", lc_react_label(0), lc_react_label(2)); fail = 1;
    }
    /* Drawn: alice's live line (index 7) gets a row; the cell under her text holds the glyph and a
     * click on that row names her message. */
    main_state(&m, 80, 25);
    strcpy(m.lines[7].react[0].emoji, "+1"); m.lines[7].react[0].count = 3; m.lines[7].nreact = 1;
    if (ui_cells_open(&c, 80, 25) != 0) return 1;
    lc_draw(&c, &m, &cr, &cc);
    for (r = 2; r < c.rows - 4 && found < 0; r++)
        for (k = 0; k + 3 < c.cols; k++)
            if (c.back[r * c.cols + k].ch == ':' && c.back[r * c.cols + k + 1].ch == ')'
                && c.back[r * c.cols + k + 2].ch == ' ' && c.back[r * c.cols + k + 3].ch == '3') { found = r; col = k; break; }
    if (found < 0) { printf("react: no ':) 3' row drawn\n"); fail = 1; }
    else {
        int textcol = -1;
        for (k = 0; k + 1 < c.cols; k++)
            if (c.back[(found - 1) * c.cols + k].ch == 'h' && c.back[(found - 1) * c.cols + k + 1].ch == 'i') { textcol = k; break; }
        if (textcol != col) { printf("react: row at col %d, text at col %d\n", col, textcol); fail = 1; }
        if (lc_hit(found, col, &idx) != LC_HIT_CHAT || idx != 7) { printf("react: click on the row -> line %d\n", idx); fail = 1; }
    }
    ui_cells_close(&c);
    return fail;
}

/* Phase B: the TS markdown, colour tags, effects, mentions and emoji codes as cells. Draws the body of a live line
 * and returns the column where `needle` starts (-1 when absent); *cell gets the first needle cell's style. */
static int body_cell(const char *body, const char *needle, ui_cell *cell, char *shown, int shown_cap)
{
    lc_line l;
    char out[LC_TEXT_MAX * 4];
    ui_cells c;
    int k, found = -1, n = 0;
    memset(&l, 0, sizeof l);
    l.kind = LC_LIVE;
    strcpy(l.nick, "bob");
    snprintf(l.text, sizeof l.text, "%s", body);
    lc_line_marked(&l, out, sizeof out, 0, 200);
    if (ui_cells_open(&c, 200, 4) != 0) return -1;
    ui_cells_marked(&c, 0, 0, out, 0, (int)strlen(out), 200);
    for (k = 0; k < 200 && n + 1 < shown_cap; k++) shown[n++] = c.back[k].ch == 0 ? ' ' : (char)c.back[k].ch;
    shown[n] = '\0';
    {
        const char *at = strstr(shown, needle);
        if (at != 0) { found = (int)(at - shown); *cell = c.back[found]; }
    }
    ui_cells_close(&c);
    return found;
}

static int message_formatting(void)
{
    ui_cell cell;
    char shown[256];
    int fail = 0;
#define CHECK_CELL(body, needle, want_ok, what) \
    do { if (body_cell(body, needle, &cell, shown, sizeof shown) < 0 || !(want_ok)) { printf("format %s: '%s' in '%s'\n", what, needle, shown); fail = 1; } } while (0)
    lc_set_self("Bob");
    CHECK_CELL("a *it* b", "it", cell.flags & UI_CELLS_ITALIC, "italic");                      /* markdown.ts *x* */
    CHECK_CELL("a __un__ b", "un", cell.flags & UI_CELLS_UNDERLINE, "underline");              /* __x__ */
    CHECK_CELL("a ~~gone~~ b", "gone", cell.fg == LC_DIM, "strike");                            /* ~~x~~ dim */
    CHECK_CELL("a `code` b", " code ", cell.flags & UI_CELLS_REVERSE, "code");                  /* inverse, padded */
    CHECK_CELL("a **bd** b", "bd", cell.flags & UI_CELLS_BOLD, "bold");
    CHECK_CELL("a *it* b", " b", !(cell.flags & (UI_CELLS_ITALIC | UI_CELLS_BOLD)), "italic ends");
    CHECK_CELL("a {red-fg}hot{/} b", "hot", cell.fg == 1, "blessed red");
    CHECK_CELL("a {#FF5C7A-fg}hot{/} b", "hot", cell.fg == 16 + 36 * 5 + 6 * 1 + 2, "hex colour");
    CHECK_CELL("a {red}hot{/red} b", "hot", cell.fg == 1, "short colour tag");
    CHECK_CELL("a {blue-bg}x{/blue-bg} b", "x", cell.fg != 4, "bg tag is not a foreground");
    CHECK_CELL("a {unknown} b", "{unknown}", 1, "unknown braces stay");
    CHECK_CELL("a ~rainbow~fx~/rainbow~ b", "fx", cell.fg == LC_ACTIVITY && (cell.flags & UI_CELLS_BOLD), "rainbow preview");
    CHECK_CELL("a ~gradient from=red to=blue~gx~/gradient~ b", "gx", cell.fg == LC_ACCENT, "gradient preview");
    CHECK_CELL("hi @Bob there", "@Bob", (cell.flags & UI_CELLS_REVERSE) && cell.fg == 3, "self mention");
    CHECK_CELL("hi @alice there", "@alice", cell.fg == LC_ACCENT, "other mention");
    CHECK_CELL("hi @everyone there", "@everyone", cell.fg == LC_ALERT, "everyone mention");
    CHECK_CELL("see [docs](http://x.y/z) ok", "[docs]", cell.fg == LC_ACCENT, "link text");
    CHECK_CELL("go :fire: now", "(*)", 1, "emoji code");                                      /* emojis.ts */
    CHECK_CELL("go :thumbsup: now", "(Y)", 1, "emoji thumbsup");
    lc_custom_emoji_clear();
    lc_custom_emoji_add("party", "\\o/");
    lc_custom_emoji_add("pic", "");                                                        /* image only: left as typed */
    CHECK_CELL("go :party: now", "\\o/", 1, "custom emoji shows its ascii face");
    CHECK_CELL("go :pic: now", ":pic:", 1, "image emoji stays as typed");
    CHECK_CELL("go :nope: now", ":nope:", cell.fg != LC_ACTIVITY, "unknown emoji code");
    lc_custom_emoji_clear();
    return fail;
}

/* H3: a system line with a tone (TS "{accentAlt}[MOTD] x{/}") draws its text in the tone and its frame in grey. */
static int system_line_tone(void)
{
    lc_line l;
    char out[LC_TEXT_MAX * 4], shown[128];
    ui_cells c;
    int k, at, fail = 0;
    memset(&l, 0, sizeof l);
    l.kind = LC_SYSTEM;
    l.tone = LC_ACTIVITY;
    strcpy(l.text, "[MOTD] Be nice");
    lc_line_marked(&l, out, sizeof out, 0, 100);
    if (ui_cells_open(&c, 100, 2) != 0) return 1;
    ui_cells_marked(&c, 0, 0, out, 0, (int)strlen(out), 100);
    for (k = 0; k < 100; k++) shown[k] = c.back[k].ch == 0 ? ' ' : (char)c.back[k].ch;
    shown[100] = '\0';
    at = (int)(strstr(shown, "[MOTD]") - shown);
    if (strstr(shown, "*** [MOTD] Be nice ***") == 0 || c.back[at].fg != LC_ACTIVITY
        || c.back[at - 2].fg != LC_DIM || c.back[at + 15].fg != LC_DIM) { printf("system tone: '%s'\n", shown); fail = 1; }
    ui_cells_close(&c);
    return fail;
}

/* B3: /timestamps and /compact decide whether the time column is drawn. */
static int stamps_follow_the_pref(void)
{
    lc_line l;
    char on[256], off[256];
    memset(&l, 0, sizeof l);
    l.kind = LC_LIVE;
    strcpy(l.time, "17:03"); strcpy(l.nick, "bob"); strcpy(l.text, "hi");
    lc_line_marked(&l, on, sizeof on, 1, 0);
    lc_line_marked(&l, off, sizeof off, 0, 0);
    if (strstr(on, "17:03") == 0 || strstr(off, "17:03") != 0 || strstr(off, "hi") == 0) {
        printf("stamps: on='%s' off='%s'\n", on, off);
        return 1;
    }
    return 0;
}

/* The focused panel's border is the bright one (TS PANEL_BORDER_FOCUS), the others stay accent,
 * and moving focus moves it. Cells: sidebar top-left corner (row 1, col 0), input top-left. */
static int focus_border(int focus, int *side, int *input)
{
    static lc_model m;
    ui_cells c;
    int ir, cr, cc;
    main_state(&m, 80, 25);
    m.focus = focus;
    if (ui_cells_open(&c, 80, 25) != 0) return 1;
    lc_draw(&c, &m, &cr, &cc);
    ir = c.rows - 4;
    *side = c.back[1 * c.cols + 0].fg;
    *input = c.back[ir * c.cols + 0].fg;
    ui_cells_close(&c);
    return 0;
}

static int active_panel_is_highlighted(void)
{
    int s, i;
    if (focus_border(LC_FOCUS_INPUT, &s, &i) || s != LC_ACCENT || i != LC_BORDER_FOCUS) { printf("focus input: side %d input %d\n", s, i); return 1; }
    if (focus_border(LC_FOCUS_SIDEBAR, &s, &i) || s != LC_BORDER_FOCUS || i != LC_ACCENT) { printf("focus sidebar: side %d input %d\n", s, i); return 1; }
    if (focus_border(LC_FOCUS_MENU, &s, &i) || s != LC_ACCENT || i != LC_ACCENT) { printf("focus menu: side %d input %d\n", s, i); return 1; }
    return 0;
}

/* TS message-picker.ts: Up on an empty input picks the newest message, Up/Down step one message
 * and stop at either end, local lines are skipped. While picking, the chat border is the focus
 * colour, the picked rows are the accent bar, and the input label says how to drive the picker. */
static int picker_selects_and_draws(void)
{
    static lc_model m;
    ui_cells c;
    int cr, cc, sw, r, k, ir, tinted = 0, fail = 0;
    char label[48];
    main_state(&m, 80, 25);
    m.focus = LC_FOCUS_INPUT;
    if (lc_pick_index(&m) != -1 || lc_pick_start(&m) != 0) { printf("picker: started with no messages\n"); return 1; }
    for (k = 5; k <= 8; k++) { m.lines[k].number = k - 4; sprintf(m.lines[k].id, "m%d", k - 4); }
    if (!lc_pick_start(&m) || lc_pick_index(&m) != 8) { printf("picker: start %d\n", lc_pick_index(&m)); fail = 1; }
    lc_pick_move(&m, 1);
    if (lc_pick_index(&m) != 8) { printf("picker: moved past the newest\n"); fail = 1; }
    lc_pick_move(&m, -1); lc_pick_move(&m, -1); lc_pick_move(&m, -1); lc_pick_move(&m, -1);
    if (lc_pick_index(&m) != 5) { printf("picker: oldest %d\n", lc_pick_index(&m)); fail = 1; }
    lc_pick_move(&m, 1);
    if (lc_pick_index(&m) != 6 || strcmp(m.pick, "m2") != 0) { printf("picker: move %d\n", lc_pick_index(&m)); fail = 1; }
    if (ui_cells_open(&c, 80, 25) != 0) return 1;
    lc_draw(&c, &m, &cr, &cc);
    sw = lc_sidebar_width(&m);
    if (c.back[1 * c.cols + sw].fg != LC_BORDER_FOCUS) { printf("picker: chat border %d\n", c.back[1 * c.cols + sw].fg); fail = 1; }
    for (r = 2; r < c.rows - 4; r++) {
        ui_cell *p = &c.back[r * c.cols + sw + 1];
        if (p->fg == LC_BAR_INK && p->bg == LC_ACCENT) tinted++;
    }
    if (tinted != 2) { printf("picker: %d tinted rows, want the 2 rows of bob's wrapped line\n", tinted); fail = 1; }
    ir = c.rows - 4;
    for (k = 0; k < 40; k++) label[k] = (char)c.back[ir * c.cols + 3 + k].ch;
    label[40] = '\0';
    if (strstr(label, "[PICK MESSAGE: Up/Down, Enter, Esc]") == 0) { printf("picker: label '%s'\n", label); fail = 1; }
    lc_pick_stop(&m);
    m.editing = 1;
    lc_draw(&c, &m, &cr, &cc);
    if (c.back[1 * c.cols + sw].fg != LC_ACCENT) { printf("picker: border after stop %d\n", c.back[1 * c.cols + sw].fg); fail = 1; }
    for (k = 0; k < 40; k++) label[k] = (char)c.back[ir * c.cols + 3 + k].ch;
    if (strstr(label, "[EDITING]") == 0) { printf("editing: label '%s'\n", label); fail = 1; }
    ui_cells_close(&c);
    return fail;
}

/* draw_chat reuses its wrapped row counts while the lines stay put (pass 1 wraps all of them), and
 * it no longer clears the pane first: every interior cell must still be written by the row fills. */
static int chat_cache_and_pane_fill(void)
{
    static lc_model a, b;
    ui_cells ca, cb;
    int cr, cc, fail = 0, r, k, sw;
    unsigned long n0;
    if (ui_cells_open(&ca, 80, 25) != 0 || ui_cells_open(&cb, 80, 25) != 0) return 1;
    main_state(&a, 80, 25);
    for (k = 0; k < 80 * 25; k++) ca.back[k].ch = 'X';              /* a stale canvas: nothing may survive in the pane */
    lc_draw(&ca, &a, &cr, &cc);
    sw = lc_sidebar_width(&a);
    for (r = 2; r < 19; r++)
        for (k = sw + 1; k < 78; k++)
            if (ca.back[r * 80 + k].ch == 'X') { printf("pane fill: stale cell at row %d col %d\n", r, k); fail = 1; r = 99; break; }
    n0 = lc_chat_rewraps;
    lc_draw(&ca, &a, &cr, &cc);
    lc_draw(&ca, &a, &cr, &cc);
    if (lc_chat_rewraps != n0) { printf("cache: unchanged lines re-wrapped (%lu)\n", lc_chat_rewraps - n0); fail = 1; }
    strcpy(a.lines[7].text, "an edited line that is now long enough to wrap onto a second row of the chat pane for sure");
    lc_lines_changed(&a);
    lc_draw(&ca, &a, &cr, &cc);
    if (lc_chat_rewraps != n0 + 1) { printf("cache: a changed line was not re-wrapped (%lu)\n", lc_chat_rewraps - n0); fail = 1; }
    main_state(&b, 80, 25);                                          /* the same edit on a model that never cached */
    strcpy(b.lines[7].text, a.lines[7].text);
    lc_lines_changed(&b);
    lc_draw(&cb, &b, &cr, &cc);
    if (memcmp(ca.back, cb.back, sizeof(ui_cell) * 80 * 25) != 0) { printf("cache: edited frame differs from a fresh draw\n"); fail = 1; }
    lc_draw(&ca, &a, &cr, &cc);                                      /* the cache is one slot: b's draw replaced it */
    n0 = lc_chat_rewraps;
    a.timestamps = !a.timestamps;                                              /* a wrap input other than the lines */
    lc_draw(&ca, &a, &cr, &cc);
    if (lc_chat_rewraps != n0 + 1) { printf("cache: stamps toggle did not re-wrap\n"); fail = 1; }
    ui_cells_close(&ca); ui_cells_close(&cb);
    return fail;
}

static int slash_menu_rules(void)
{
    int idx[LC_SLASH_MAX], n, fail = 0, uc, ul, i;
    char row[200];
    n = lc_slash_match("/jo", idx, LC_SLASH_MAX);
    if (n < 1 || strcmp(lc_cmd_help[idx[0]][0], "join") != 0) { printf("slash: /jo first is not join (n=%d)\n", n); fail = 1; }
    n = lc_slash_match("/", idx, LC_SLASH_MAX);
    if (n != lc_cmd_help_count) { printf("slash: bare / lists %d of %d\n", n, lc_cmd_help_count); fail = 1; }
    for (i = 1; i < n; i++) if (strcmp(lc_cmd_help[idx[i - 1]][0], lc_cmd_help[idx[i]][0]) > 0) { printf("slash: bare / not alphabetical at %d\n", i); fail = 1; break; }
    n = lc_slash_match("/mute", idx, LC_SLASH_MAX);          /* prefix first (mute), then description matches (unmute: "Unmute user") */
    if (n < 2 || strcmp(lc_cmd_help[idx[0]][0], "mute") != 0 || strcmp(lc_cmd_help[idx[1]][0], "deafen") != 0) { printf("slash: /mute order\n"); fail = 1; }
    n = lc_slash_match("/pin", idx, LC_SLASH_MAX);            /* a TS command the door does not run yet is still listed */
    if (n < 3 || strcmp(lc_cmd_help[idx[0]][0], "pin") != 0 || strcmp(lc_cmd_help[idx[1]][0], "pinned") != 0 || lc_cmd_help_count != 56) { printf("slash: /pin n=%d count=%d\n", n, lc_cmd_help_count); fail = 1; }
    n = lc_slash_match("/reaction", idx, LC_SLASH_MAX);
    if (n < 1 || strcmp(lc_cmd_help[idx[0]][0], "reactions") != 0) { printf("slash: /reaction first\n"); fail = 1; }
    n = lc_slash_match("/Away", idx, LC_SLASH_MAX);
    if (n < 1 || strcmp(lc_cmd_help[idx[0]][0], "away") != 0) { printf("slash: case-insensitive\n"); fail = 1; }
    if (lc_slash_match("/zzzz", idx, LC_SLASH_MAX) != 0 || lc_slash_match("hello", idx, LC_SLASH_MAX) != 0
        || lc_slash_match("/join x", idx, LC_SLASH_MAX) != 0) { printf("slash: no-match cases\n"); fail = 1; }
    n = lc_slash_match("/ban", idx, LC_SLASH_MAX);
    lc_slash_row(row, sizeof row, idx[0], 80, &uc, &ul);
    if (strncmp(row, "/ban          /ban @user [reason]  Ban user from channel", 56) != 0 || uc != 14 || ul != 20) { printf("slash: wide row '%s' %d %d\n", row, uc, ul); fail = 1; }   /* TS prints the usage as written, "/ban" included */
    n = lc_slash_match("/jo", idx, LC_SLASH_MAX);
    if (lc_slash_ghost("/jo", idx, n) != idx[0] || lc_slash_ghost("/", idx, lc_slash_match("/", idx, LC_SLASH_MAX)) != -1) { printf("slash: ghost prefix\n"); fail = 1; }
    n = lc_slash_match("/annel", idx, LC_SLASH_MAX);          /* description-only matches: no ghost */
    if (n < 1 || lc_slash_ghost("/annel", idx, n) != -1) { printf("slash: ghost on description match (n=%d)\n", n); fail = 1; }
    lc_slash_row(row, sizeof row, idx[0], 40, &uc, &ul);
    if (strcmp(row, "/ban Ban user from channel") != 0 || ul != 0) { printf("slash: compact row '%s'\n", row); fail = 1; }
    lc_slash_row(row, sizeof row, 2, 20, &uc, &ul);           /* create: cut to max(8, cols-4) = 16 */
    if (strlen(row) != 16) { printf("slash: compact cut %d\n", (int)strlen(row)); fail = 1; }
    if (!lc_slash_exact("/join", 10) || lc_slash_exact("/joi", 10) || lc_slash_exact("/join x", 10) || !lc_slash_exact("/JOIN ", 10)) { printf("slash: exact\n"); fail = 1; }
    if (lc_slash_scroll(0, 0, 35) != 0 || lc_slash_scroll(8, 0, 35) != 1 || lc_slash_scroll(2, 5, 35) != 2 || lc_slash_scroll(34, 0, 35) != 27 || lc_slash_scroll(3, 0, 5) != 0) { printf("slash: scroll\n"); fail = 1; }
    return fail;
}

/* H2: an LC_EVENT line is TS formatEvent: "[HH:MM] " in accent, the head in its tone, the body in
 * the pen (or body_tone), and " [NEW PB!]" in light green for a personal best. */
static int event_line_marks(void)
{
    lc_line l;
    char out[400], want[400];
    int fail = 0;
    memset(&l, 0, sizeof l);
    l.kind = LC_EVENT; strcpy(l.time, "17:03"); strcpy(l.head, "[Tetris]"); strcpy(l.text, " bob: 9000");
    l.tone = LC_EV_YELLOW; l.pb = 1;
    lc_line_marked(&l, out, sizeof out, 1, 0);
    snprintf(want, sizeof want, "\001%c[17:03]\004 \001\003[Tetris]\004 bob: 9000\004 \001\012[NEW PB!]", LC_ACCENT);
    if (strcmp(out, want) != 0) { printf("event: '%s'\n", out); fail = 1; }
    l.time[0] = '\0'; strcpy(l.head, "[EVENT] "); strcpy(l.text, "bob has logged off");
    l.tone = LC_TK_DIM; l.body_tone = LC_EV_RED; l.pb = 0;
    lc_line_marked(&l, out, sizeof out, 1, 0);
    snprintf(want, sizeof want, "\001%c[EVENT] \004\001\001bob has logged off", LC_DIM);
    if (strcmp(out, want) != 0) { printf("event legacy: '%s'\n", out); fail = 1; }
    return fail;
}

/* C2: the TS format picker model. FORMATS wraps (format-picker.ts): Colors use the theme token as hex,
 * Effects the ~name~ tags, Markdown its marks. The category list clamps and reloads the items at 0. */
static int format_picker_model(void)
{
    char out[64];
    lc_model m;
    int fail = 0;
    const unsigned long *rgb = ui_theme_by_id("")->rgb;
    char red[32];
    snprintf(red, sizeof red, "{#%06lX-fg}hi{/}", rgb[UI_T_ALERT] & 0xFFFFFFUL);
    if (lc_fmt_wrap(out, sizeof out, 2, 0, "hi", 2, rgb) != 6 || strcmp(out, "**hi**") != 0) { printf("fmt: bold '%s'\n", out); fail = 1; }
    if (lc_fmt_wrap(out, sizeof out, 0, 0, "hi", 2, rgb) < 0 || strcmp(out, red) != 0) { printf("fmt: red '%s'\n", out); fail = 1; }
    if (lc_fmt_wrap(out, 5, 2, 0, "hi", 2, rgb) != -1) { printf("fmt: overflow not refused\n"); fail = 1; }
    if (lc_fmt_wrap(out, sizeof out, 2, 99, "hi", 2, rgb) != -1) { printf("fmt: bad item not refused\n"); fail = 1; }
    memset(&m, 0, sizeof m);
    lc_fmt_open(&m, 5, 2);
    if (!m.fmt || m.fmt_a != 2 || m.fmt_b != 5 || m.fmt_pane != LC_FMT_PANE_CAT) { printf("fmt: open\n"); fail = 1; }
    lc_fmt_move(&m, 9);
    if (m.fmt_cat != LC_FMT_CATS - 1) { printf("fmt: cat clamp %d\n", m.fmt_cat); fail = 1; }
    m.fmt_pane = LC_FMT_PANE_ITEM;
    lc_fmt_move(&m, 99);
    if (m.fmt_item != lc_fmt_count(m.fmt_cat) - 1) { printf("fmt: item clamp %d\n", m.fmt_item); fail = 1; }
    /* the emoji kind (emoji-picker.ts): four categories, TS EMOJIS counts 18/9/13/3, the same move/clamp */
    if (LC_EMOJI_CATS != 4 || strcmp(lc_emoji_cat_name(0), "Emotions") != 0 || strcmp(lc_emoji_cat_name(3), "Special") != 0
        || strcmp(lc_emoji_cat_name(1), "Actions") != 0 || strcmp(lc_emoji_cat_name(2), "Symbols") != 0) { printf("emoji: categories\n"); fail = 1; }
    if (lc_emoji_count(0) != 18 || lc_emoji_count(1) != 9 || lc_emoji_count(2) != 13 || lc_emoji_count(3) != 3 || lc_emoji_count(4) != 0) {
        printf("emoji: counts %d %d %d %d\n", lc_emoji_count(0), lc_emoji_count(1), lc_emoji_count(2), lc_emoji_count(3)); fail = 1; }
    if (strcmp(lc_emoji_code(0, 0), ":smile:") != 0 || strcmp(lc_emoji_text(0, 0), ":-)") != 0 || strcmp(lc_emoji_code(3, 2), ":door:") != 0) { printf("emoji: items\n"); fail = 1; }
    lc_emoji_label(out, sizeof out, 0, 0, 30);                    /* art padded to 16 columns, then the code */
    if (strcmp(out, ":-)             :smile:") != 0) { printf("emoji: label '%s'\n", out); fail = 1; }
    lc_emoji_label(out, sizeof out, 0, 0, 16);                    /* no room for both: the art alone */
    if (strcmp(out, ":-)") != 0) { printf("emoji: narrow label '%s'\n", out); fail = 1; }
    lc_emoji_label(out, sizeof out, 0, 0, 20);                    /* clipped to the width */
    if (strlen(out) != 20) { printf("emoji: clip '%s'\n", out); fail = 1; }
    memset(&m, 0, sizeof m);
    lc_emoji_open(&m);
    if (!m.fmt || m.fmt_kind != LC_PICK_EMOJI || m.fmt_pane != LC_FMT_PANE_CAT) { printf("emoji: open\n"); fail = 1; }
    lc_fmt_move(&m, 9);
    if (m.fmt_cat != LC_EMOJI_CATS - 1) { printf("emoji: cat clamp %d\n", m.fmt_cat); fail = 1; }
    lc_fmt_move(&m, -1);
    m.fmt_pane = LC_FMT_PANE_ITEM;
    lc_fmt_move(&m, 99);
    if (m.fmt_item != 12 || m.fmt_item_top != 12 - lc_pick_rows(&m) + 1) { printf("emoji: item clamp %d top %d\n", m.fmt_item, m.fmt_item_top); fail = 1; }
    return fail;
}

/* The theme menu's name column is the longest name plus one, so no name pushes its blurb out of line. */
static int theme_menu_columns(void)
{
    ui_cells c;
    lc_model *m = (lc_model *)malloc(sizeof *m);
    int cr, cc, n = ui_theme_count(), i, longest = 0, shortest = 0, col_long = -1, col_short = -1, r, k, bad = 0;
    static char row[100];
    for (i = 0; i < n; i++) {
        if (strlen(ui_theme_at(i)->name) > strlen(ui_theme_at(longest)->name)) longest = i;
        if (strlen(ui_theme_at(i)->name) < strlen(ui_theme_at(shortest)->name)) shortest = i;
    }
    ui_cells_open(&c, 80, 25);
    main_state(m, 80, 25);
    m->theme = 1;
    lc_draw(&c, m, &cr, &cc);
    for (r = 0; r < 50; r++) {      /* two passes: the longest name's row may come after the shortest's */
        for (k = 0; k < 80; k++) row[k] = c.back[(r % 25) * 80 + k].ch ? (char)c.back[(r % 25) * 80 + k].ch : ' ';
        row[80] = '\0';
        {
            const char *a = strstr(row, ui_theme_at(longest)->name), *b = strstr(row, ui_theme_at(shortest)->name);
            if (a != 0 && col_long < 0) col_long = (int)(a - row) + (int)strlen(ui_theme_at(longest)->name) + 1;
            if (b != 0 && col_short < 0 && col_long >= 0 && b == row + (col_long - 1 - (int)strlen(ui_theme_at(longest)->name)) && row[col_long] == ui_theme_at(shortest)->blurb[0]) col_short = col_long;
            if (a != 0 && row[col_long] != ui_theme_at(longest)->blurb[0]) bad = 1;
        }
    }
    free(m);
    if (col_long < 0 || bad || col_long != col_short) { printf("FAIL theme menu name column long=%d short=%d bad=%d\n", col_long, col_short, bad); return 1; }
    return 0;
}

/* lc_theme_set maps every theme token to the same xterm index the TS door's blessed match() answers
 * (tests/fixtures/lc68-ts-colour-map.txt, generated from engines/ui/blessed/core/colors.ts). The default ink
 * #E4ECFA is grey 255, not the cyan-tinted cube 195. */
static int colours_match_ts(void)
{
    FILE *f = fopen("tests/fixtures/lc68-ts-colour-map.txt", "r");
    char line[256];
    int bad = 0, rows = 0;
    if (f == 0) { printf("FAIL cannot read colour map\n"); return 1; }
    while (fgets(line, sizeof line, f) != 0) {
        int idx, v[UI_TOKEN_COUNT], t, off = 0, used;
        if (line[0] == '#') continue;
        if (sscanf(line, "%d%n", &idx, &used) != 1) continue;
        off = used;
        for (t = 0; t < UI_TOKEN_COUNT; t++) { sscanf(line + off, "%d%n", &v[t], &used); off += used; }
        lc_theme_set(ui_theme_at(idx)->rgb);
        for (t = 0; t < UI_TOKEN_COUNT; t++)
            if (lc_col[t] != v[t]) { printf("FAIL theme %d token %d: C %d, TS %d\n", idx, t, lc_col[t], v[t]); bad = 1; }
        rows++;
    }
    fclose(f);
    lc_theme_set(ui_theme_at(0)->rgb);
    if (rows != ui_theme_count()) { printf("FAIL colour map has %d rows, %d themes\n", rows, ui_theme_count()); bad = 1; }
    if (lc_col[UI_T_INK] != 255) { printf("FAIL default ink %d, want 255\n", lc_col[UI_T_INK]); bad = 1; }
    return bad;
}

int main(void)
{
    int r = chat_cache_and_pane_fill() | react_rows_under_the_message() | stamps_follow_the_pref() | active_panel_is_highlighted() | picker_selects_and_draws() | slash_menu_rules() | message_formatting() | event_line_marks() | system_line_tone() | format_picker_model() | theme_menu_columns() | colours_match_ts();
    int a = compare("tests/fixtures/lc68-ts-main-80x25.grid");
    int b = compare("tests/fixtures/lc68-ts-main-40x25.grid");
    int d = compare("tests/fixtures/lc68-ts-main-80x15.grid");
    if (r != 0 || a != 0 || b != 0 || d != 0) { printf("FAIL livechat_ui fixtures\n"); return 1; }
    printf("PASS livechat_ui fixtures\n");
    return 0;
}
