/* livechat_ui.c - draws the LIVECHAT68 frame from an lc_model (see livechat_ui.h). */
#include "ui_key.h"
#include <stdio.h>
#include <string.h>

#include "livechat_ui.h"
#include "ui_theme.h"

#define MENU_TEXT " Chat v3.2.0   Tools   View   Help  "
#define MENU_TEXT_COLS 36
#define MASTHEAD "LIVE CHAT"

/* Column of each menu label in MENU_TEXT; the dropdown opens one column left of it. */
static const int menu_col[LC_MENUS] = { 1, 15, 23, 30 };

const char *const lc_menu_label[LC_MENUS] = { "Chat v3.2.0", "Tools", "View", "Help" };
static const char *const menu_chat[] = { "Help (F1)", "Channel List (F2)", "Next Channel (F3)", "Join Channel...", "Leave Channel" };
static const char *const menu_tools[] = { "Emoji (F4)", "Files (F6)", "Pins (F7)", "Search (Ctrl+F)", "Threads" };
static const char *const menu_view[] = { "Settings (Ctrl+S)", "Theme", "Toggle Sidebar", "Clear Chat" };
static const char *const menu_help[] = { "About", "Keyboard Shortcuts", "Quit (Ctrl+Q)" };
const int lc_menu_count[LC_MENUS] = { 5, 5, 4, 3 };
const char *const *const lc_menu_items[LC_MENUS] = { menu_chat, menu_tools, menu_view, menu_help };

/* The TS door's help (board/Doors/livechat help content), voice section left out: this door has no voice. */
const char *const lc_help_text[] = {
    "=== LIVECHAT v3.0 - DESKTOP-LEVEL BBS CHAT ===",
    "A full-featured multi-user chat system with neo-blessed UI",
    "",
    "--- KEYBOARD SHORTCUTS ---",
    "Navigation:",
    "  Tab              Next channel",
    "  Shift+Tab        Previous channel",
    "  PageUp           Scroll chat up",
    "  PageDown         Scroll chat down",
    "  Escape           Close dialogs / Return to input",
    "Chat:",
    "  Enter            Send message",
    "  Ctrl+N           New message prompt",
    "  Ctrl+R           Room menu (join room)",
    "  Ctrl+S           Open settings",
    "  F1               Show this help",
    "Window Controls:",
    "  F2               Toggle sidebar visibility",
    "  F3               Switch sidebar tab (Channels/Users)",
    "  F4 / Ctrl+E      Open emoji picker",
    "  F5               Format selected text (colors/effects)",
    "  F6               File sharing browser",
    "  Tab              Cycle focus between panels",
    "  Ctrl+Q           Quit (with confirmation)",
    "Text Formatting:",
    "  Shift+Arrow      Select text in input box",
    "  F5               Open format picker (with text selected)",
    "  Right-click      Open format picker (with text selected)",
    "",
    "--- COMMANDS ---",
    "Room Commands:",
    "  /join <room>     Join a channel",
    "  /leave           Leave current channel",
    "  /create <name>   Create new channel",
    "  /topic <text>    Set channel topic",
    "  /rooms           List all rooms",
    "User Commands:",
    "  /who             List online users",
    "  /whois <user>    User information",
    "  /dm <user> <msg> Send direct message",
    "  /ignore <user>   Ignore a user",
    "  /unignore <user> Unignore a user",
    "Status Commands:",
    "  /away [msg]      Set away status",
    "  /back            Return from away",
    "  /status <status> Set status (online/away/busy/dnd)",
    "Other Commands:",
    "  /me <action>     Send action message",
    "  /clear           Clear chat window",
    "  /draw <channel>  Open drawing whiteboard",
    "  /files           Open file sharing browser",
    "  /emoji [search]  Open emoji picker or search",
    "  /events [type]   Manage event notifications",
    "  /react [emoji] <msg-id>, /thread <msg-id> message",
    "  /help            Show this help",
};
const int lc_help_lines = (int)(sizeof lc_help_text / sizeof lc_help_text[0]);

/* One clock for every model, so a fresh model never shares a generation with a cached one. */
static unsigned long lines_clock;
unsigned long lc_chat_rewraps;

void lc_lines_changed(lc_model *m) { m->lines_gen = ++lines_clock; }

int lc_col[UI_TOKEN_COUNT];

int lc_tone(int tone) { return tone >= LC_TK_BASE && tone < LC_TK_BASE + UI_TOKEN_COUNT ? lc_col[tone - LC_TK_BASE] : tone; }

/* rgb_cube is defined below with the format picker; the palette needs it first. */
static int rgb_cube(unsigned long rgb);

void lc_theme_set(const unsigned long *rgb)
{
    int t;
    for (t = 0; t < UI_TOKEN_COUNT; t++) lc_col[t] = rgb_cube(rgb[t]);
}

void lc_model_init(lc_model *m, int cols, int rows)
{
    memset(m, 0, sizeof *m);
    m->rgb = ui_theme_by_id("")->rgb;
    lc_theme_set(m->rgb);
    m->input_anchor = -1;
    lc_lines_changed(m);
    m->cols = cols;
    m->rows = rows;
    m->node = 1;
    m->menu = -1;
    m->timestamps = 1;
}

void lc_model_resize(lc_model *m, int cols, int rows)
{
    m->cols = cols;
    m->rows = rows;
    m->scroll = 0;                                   /* display rows back from the newest: that count is width-bound */
    lc_lines_changed(m);
}

int lc_sidebar_width(const lc_model *m)
{
    int w;
    if (m->sidebar_hidden || m->cols <= 40) return 0;
    w = m->cols - 24;
    if (w > 21) w = 21;
    return w < 8 ? 0 : w;
}

/* Menu bar, three input rows and the status line leave the boxes rows - 5; borders and label bar take 3. */
int lc_text_rows(const lc_model *m)
{
    int n = m->rows - 5 - 3;
    return n < 1 ? 1 : n;
}

int lc_nick_color(const char *nick)
{
    unsigned sum = 0;
    for (; *nick != '\0'; nick++) sum += (unsigned char)*nick;
    return (int)(sum % 6) + 1;
}

typedef struct { char *p; int n, cap; } sb;

static void sb_ch(sb *b, int ch) { if (b->n + 1 < b->cap) b->p[b->n++] = (char)ch; b->p[b->n] = '\0'; }
/* Appends len bytes of s, clipped to the buffer: one bounds check per run instead of one per byte. */
static void sb_run(sb *b, const char *s, int len)
{
    int room = b->cap - 1 - b->n;
    if (room < 0) room = 0;
    if (len > room) len = room;
    memcpy(b->p + b->n, s, (size_t)len);
    b->n += len;
    b->p[b->n] = '\0';
}
static void sb_str(sb *b, const char *s) { sb_run(b, s, (int)strlen(s)); }
static void sb_fg(sb *b, int fg) { sb_ch(b, UI_CELLS_M_FG); sb_ch(b, lc_tone(fg)); }

/* ---- Incoming message formatting: the TS render path, board/Doors/livechat/core/formatter.ts:12-13 (parseContent,
 * then highlightMentions), utils/markdown.ts and utils/mentions.ts. The cell canvas paints no background per run, so a
 * blessed "-bg" tag is read and dropped. ---- */

typedef struct { int fg, bold, attr; } style;       /* fg -1: the pen's own colour */

/* Brings the emitted style to `want` with the fewest markers (BASE returns fg, bold and attr to the pen's). */
static void style_set(sb *b, style *have, const style *want)
{
    if (want->fg != have->fg) {
        if (want->fg >= 0) sb_fg(b, want->fg);
        else { sb_ch(b, UI_CELLS_M_BASE); have->bold = 0; have->attr = 0; }
        have->fg = want->fg;
    }
    if (want->bold != have->bold) { sb_ch(b, want->bold ? UI_CELLS_M_BOLD : UI_CELLS_M_PLAIN); have->bold = want->bold; }
    if (want->attr != have->attr) { sb_ch(b, UI_CELLS_M_ATTR); sb_ch(b, UI_CELLS_ATTR(want->attr)); have->attr = want->attr; }
}

static char self_nick[32];
static struct { char name[LC_CUSTOM_EMOJI_LEN], ascii[LC_CUSTOM_EMOJI_TEXT]; } custom_emoji[LC_CUSTOM_EMOJI_MAX];
static int ncustom_emoji;

void lc_set_self(const char *handle)
{
    int n = 0;
    while (handle[n] != '\0' && handle[n] != '@' && n < (int)sizeof self_nick - 1) { self_nick[n] = handle[n]; n++; }
    self_nick[n] = '\0';
}

void lc_custom_emoji_clear(void) { ncustom_emoji = 0; }
void lc_custom_emoji_add(const char *name, const char *ascii)
{
    /* TS utils/emojis.ts: a byte terminal sees `ascii`. An image emoji (no text form) is left as typed. */
    if (ncustom_emoji >= LC_CUSTOM_EMOJI_MAX || name[0] == '\0' || ascii[0] == '\0') return;
    sprintf(custom_emoji[ncustom_emoji].name, "%.*s", LC_CUSTOM_EMOJI_LEN - 1, name);
    sprintf(custom_emoji[ncustom_emoji].ascii, "%.*s", LC_CUSTOM_EMOJI_TEXT - 1, ascii);
    ncustom_emoji++;
}

/* TS utils/emojis.ts EMOJIS (code, display text, category: 0 Emotions 1 Actions 2 Symbols 3 Special). */
static const struct { const char *code, *text, *keywords; int cat; } emoji_table[] = {
    { ":smile:", ":-)", "happy, grin", 0 },
    { ":grin:", ":D", "big, smile, laugh", 0 },
    { ":joy:", "XD", "laugh, lol", 0 },
    { ":wink:", ";-)", "flirt", 0 },
    { ":heart:", "<3", "love", 0 },
    { ":sad:", ":-(", "unhappy, frown", 0 },
    { ":cry:", ":'-(", "tears, sob", 0 },
    { ":shock:", ":O", "surprised, wow", 0 },
    { ":angry:", ">:-(", "mad, furious", 0 },
    { ":cool:", "B-)", "sunglasses, awesome", 0 },
    { ":tongue:", ":P", "playful, silly", 0 },
    { ":kiss:", ":-*", "love, romance", 0 },
    { ":blush:", ":-}", "shy, embarrassed", 0 },
    { ":confused:", ":-/", "unsure, puzzled", 0 },
    { ":neutral:", ":-|", "meh, okay", 0 },
    { ":surprised:", ":o", "wow, shocked", 0 },
    { ":evil:", ">:-)", "devil, mischief", 0 },
    { ":angel:", "O:-)", "innocent, halo", 0 },
    { ":tableflip:", "(>_<)~~ |[__]|", "rage, flip, angry", 1 },
    { ":unflip:", "|---| (-_-)", "calm, fix", 1 },
    { ":shrug:", "\\(o.O)/", "dunno, whatever", 1 },
    { ":fight:", "(>_<)*", "battle, fite", 1 },
    { ":dance:", "\\(^o^)/", "party", 1 },
    { ":hug:", "(>^_^)<", "cuddle, embrace", 1 },
    { ":run:", "(>_>)===>", "flee, escape", 1 },
    { ":wave:", "(^.^)/", "hello, bye", 1 },
    { ":facepalm:", "(>_<;)", "disappointed, sigh", 1 },
    { ":thumbsup:", "(Y)", "yes, ok, good", 2 },
    { ":thumbsdown:", "(N)", "no, bad", 2 },
    { ":fire:", "(*)", "hot, lit", 2 },
    { ":star:", "*", "favorite", 2 },
    { ":check:", "[v]", "yes, done", 2 },
    { ":x:", "[x]", "no, fail", 2 },
    { ":skull:", "8X", "death, pirate", 2 },
    { ":music:", "~o~", "song, note", 2 },
    { ":heart2:", "<3", "love, like", 2 },
    { ":diamond:", "<>", "gem, shine", 2 },
    { ":arrow:", "->", "next, go", 2 },
    { ":warning:", "(!)", "caution, alert", 2 },
    { ":lightning:", "/!\\", "power, fast", 2 },
    { ":amiga:", "[A]", "boing, retro, computer", 3 },
    { ":bbs:", "[BBS]", "board, system", 3 },
    { ":door:", "[=>]", "game, app", 3 },
};
#define NEMOJI ((int)(sizeof emoji_table / sizeof emoji_table[0]))

int lc_emoji_builtin(const char *code)
{
    int i;
    for (i = 0; i < NEMOJI; i++) if (strcmp(code, emoji_table[i].code) == 0) return 1;
    return 0;
}

static int is_word(int ch) { return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_'; }
static int lower(int ch);
static int same_word(const char *a, const char *b, int len)
{
    int i;
    for (i = 0; i < len; i++) if (lower((unsigned char)a[i]) != lower((unsigned char)b[i])) return 0;
    return 1;
}

/* TS replaceEmojis: each known ":code:" becomes its text form, the built-ins first, then the board's custom
 * emoji as their `ascii` (utils/emojis.ts getAllEmojis for a byte terminal). */
static void expand_emoji(const char *in, char *out, int cap)
{
    int n = 0;
    while (*in != '\0' && n + 1 < cap) {
        if (*in == ':') {
            int i;
            for (i = 0; i < NEMOJI; i++) {
                int cl = (int)strlen(emoji_table[i].code), tl = (int)strlen(emoji_table[i].text);
                if (strncmp(in, emoji_table[i].code, (size_t)cl) == 0 && n + tl < cap) {
                    memcpy(out + n, emoji_table[i].text, (size_t)tl);
                    n += tl;
                    in += cl;
                    break;
                }
            }
            if (i < NEMOJI) continue;
            for (i = 0; i < ncustom_emoji; i++) {
                int nl = (int)strlen(custom_emoji[i].name), tl = (int)strlen(custom_emoji[i].ascii);
                if (strncmp(in + 1, custom_emoji[i].name, (size_t)nl) == 0 && in[1 + nl] == ':' && n + tl < cap) {
                    memcpy(out + n, custom_emoji[i].ascii, (size_t)tl);
                    n += tl;
                    in += nl + 2;
                    break;
                }
            }
            if (i < ncustom_emoji) continue;
        }
        out[n++] = *in++;
    }
    out[n] = '\0';
}

/* blessed colour names (utils/markdown.ts VALID_COLORS) to xterm indices; "light" names are the bright half. */
static int named_color(const char *s, int len)
{
    static const char *const names[] = { "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white", "gray" };
    int i, light = 0;
    if (len > 5 && strncmp(s, "light", 5) == 0) { light = 8; s += 5; len -= 5; }
    for (i = 0; i < 9; i++)
        if ((int)strlen(names[i]) == len && strncmp(names[i], s, (size_t)len) == 0)
            return i == 8 ? (light ? 15 : 8) : i + light;
    return -1;
}

static int hex_digit(int ch)
{
    return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
}

/* 0xRRGGBB to the nearest of the 256 xterm colours, exactly as the TS door's blessed layer does it
 * (engines/ui/blessed/core/colors.ts match(): the sixteen system colours, the 6x6x6 cube, the 24-step grey
 * ramp; weighted squared distance 30/59/11; the first nearest wins; an exact hit stops the search). */
static const unsigned long sys16[16] = {
    0x000000UL, 0xcd0000UL, 0x00cd00UL, 0xcdcd00UL, 0x0000eeUL, 0xcd00cdUL, 0x00cdcdUL, 0xe5e5e5UL,
    0x7f7f7fUL, 0xff0000UL, 0x00ff00UL, 0xffff00UL, 0x5c5cffUL, 0xff00ffUL, 0x00ffffUL, 0xffffffUL
};

static unsigned long palette256(int i)
{
    int r, g, b, l;
    if (i < 16) return sys16[i];
    if (i < 232) {
        i -= 16;
        r = i / 36; g = (i / 6) % 6; b = i % 6;
        return ((unsigned long)(r ? r * 40 + 55 : 0) << 16) | ((unsigned long)(g ? g * 40 + 55 : 0) << 8) | (unsigned long)(b ? b * 40 + 55 : 0);
    }
    l = (i - 232) * 10 + 8;
    return ((unsigned long)l << 16) | ((unsigned long)l << 8) | (unsigned long)l;
}

static int rgb_cube(unsigned long rgb)
{
    long best = 0x7fffffffL;
    int i, li = 0;
    for (i = 0; i < 256; i++) {
        unsigned long p = palette256(i);
        long dr = 30L * ((long)((rgb >> 16) & 255) - (long)((p >> 16) & 255));
        long dg = 59L * ((long)((rgb >> 8) & 255) - (long)((p >> 8) & 255));
        long db = 11L * ((long)(rgb & 255) - (long)(p & 255));
        long d = dr * dr + dg * dg + db * db;
        if (d == 0) return i;
        if (d < best) { best = d; li = i; }
    }
    return li;
}

/* "#rrggbb" (the theme tokens the format picker writes, ui/format-picker.ts:21-28) to the nearest xterm cube colour. */
static int hex_color(const char *s, int len)
{
    unsigned long rgb = 0;
    int i;
    if (len != 7 || s[0] != '#') return -1;
    for (i = 1; i < 7; i++) {
        int d = hex_digit(s[i]);
        if (d < 0) return -1;
        rgb = rgb * 16 + (unsigned long)d;
    }
    return rgb_cube(rgb);
}

/* "~rainbow~" ... "~/rainbow~": the animation tags (format-picker.ts:31-36). The door animates nothing, so a run takes
 * the static look the TS picker previews for it (format-picker.ts:116-121). Returns the tag length, 0 when s is no tag. */
static const struct { const char *name, *label; int fg, bold; } fx[] = {
    { "rainbow", "Rainbow", LC_TK_ACCENT_ALT, 1 }, { "pulse", "Pulse", LC_TK_ACCENT, 1 }, { "sparkle", "Sparkle", LC_TK_ACCENT_ALT, 1 },
    { "shake", "Shake", LC_TK_ALERT, 1 }, { "wave", "Wave", LC_TK_INK, 1 }, { "gradient", "Gradient", LC_TK_ACCENT, 0 } };
#define FX_COUNT 6

static int effect_tag(const char *s, int *closing, int *fg, int *bold)
{
    const char *p = s + 1, *end;
    size_t i;
    *closing = 0;
    if (*p == '/') { *closing = 1; p++; }
    end = strchr(p, '~');
    if (end == 0) return 0;
    for (i = 0; i < FX_COUNT; i++) {
        size_t nl = strlen(fx[i].name);
        if (strncmp(p, fx[i].name, nl) == 0 && (p + nl == end || (!*closing && p[nl] == ' '))) {
            *fg = lc_tone(fx[i].fg);
            *bold = fx[i].bold;
            return (int)(end - s) + 1;
        }
    }
    return 0;
}

/* The opener at s of `len` characters has a partner when the closer follows with something between. */
static int has_closer(const char *s, int len, const char *closer)
{
    const char *p = strstr(s + len + 1, closer);
    return p != 0;
}

typedef struct { int bold, italic, under, strike, inv, tag_fg, eff_fg, eff_bold; } fmt;

static void fmt_want(const fmt *f, style *want)
{
    want->bold = f->bold || f->eff_bold;
    want->attr = (f->italic ? UI_CELLS_A_ITALIC : 0) | (f->under ? UI_CELLS_A_UNDERLINE : 0) | (f->inv ? UI_CELLS_A_REVERSE : 0);
    want->fg = f->strike ? LC_DIM : f->tag_fg >= 0 ? f->tag_fg : f->eff_fg;
}

/* A word run in a one-off colour (mention, link), then back to the running style. */
static void sb_spot(sb *b, style *have, const style *want, int fg, int rev, const char *s, int len)
{
    style spot = *want;
    spot.fg = fg;
    if (rev) spot.attr |= UI_CELLS_A_REVERSE;
    style_set(b, have, &spot);
    sb_run(b, s, len);
}

/* A {tag} at s (blessed syntax, utils/markdown.ts BLESSED_TAG_NAMES and parseColors). Returns the tag length when it is
 * one that changes the style or is consumed, 0 when the brace stays as typed. *lit is set for {open} and {close}. */
static int brace_tag(const char *s, fmt *f, int *lit)
{
    const char *e = strchr(s, '}');
    int closing, nl, v;
    const char *nm;
    *lit = 0;
    if (e == 0) return 0;
    closing = s[1] == '/';
    nm = s + 1 + closing;
    nl = (int)(e - nm);
    if (nl == 0) {
        if (closing) { f->bold = f->italic = f->under = f->strike = f->inv = 0; f->tag_fg = -1; return (int)(e - s) + 1; }   /* {/} */
        return 0;
    }
    if (nl == 4 && strncmp(nm, "bold", 4) == 0) { f->bold = !closing; return (int)(e - s) + 1; }
    if (nl == 6 && strncmp(nm, "italic", 6) == 0) { f->italic = !closing; return (int)(e - s) + 1; }
    if (nl == 9 && strncmp(nm, "underline", 9) == 0) { f->under = !closing; return (int)(e - s) + 1; }
    if (nl == 7 && strncmp(nm, "inverse", 7) == 0) { f->inv = !closing; return (int)(e - s) + 1; }
    if ((nl == 5 && strncmp(nm, "blink", 5) == 0) || (nl == 9 && strncmp(nm, "invisible", 9) == 0)) return (int)(e - s) + 1;
    if (!closing && nl == 4 && strncmp(nm, "open", 4) == 0) { *lit = '{'; return (int)(e - s) + 1; }
    if (!closing && nl == 5 && strncmp(nm, "close", 5) == 0) { *lit = '}'; return (int)(e - s) + 1; }
    if (nl > 3 && nm[nl - 3] == '-' && (nm[nl - 2] == 'f' || nm[nl - 2] == 'b') && nm[nl - 1] == 'g') {
        v = named_color(nm, nl - 3);
        if (v < 0) v = hex_color(nm, nl - 3);
        if (v < 0) return 0;
        if (nm[nl - 2] == 'f') f->tag_fg = closing ? -1 : v;      /* -bg: read and dropped */
        return (int)(e - s) + 1;
    }
    if (!closing && (v = named_color(nm, nl)) >= 0) {             /* {red}text{/red} */
        char closer[16];
        sprintf(closer, "{/%.*s}", nl, nm);
        if (strstr(e + 1, closer) == 0) return 0;
        f->tag_fg = v;
        return (int)(e - s) + 1;
    }
    if (closing && named_color(nm, nl) >= 0) { f->tag_fg = -1; return (int)(e - s) + 1; }
    if (!closing && nl > 3 && strncmp(nm, "bg:", 3) == 0 && named_color(nm + 3, nl - 3) >= 0 && strstr(e + 1, "{/bg}") != 0) return (int)(e - s) + 1;
    if (closing && nl == 2 && strncmp(nm, "bg", 2) == 0) return (int)(e - s) + 1;
    return 0;
}

/* One message body in the TS order: emoji codes, markdown (bold, italic, underline, code, strike), colour tags and
 * effects, links, mentions. A marker with no partner stays as typed. */
static void sb_markdown(sb *b, const char *text)
{
    char buf[LC_TEXT_MAX * 2];
    const char *s = buf;
    style have = { -1, 0, 0 }, want = { -1, 0, 0 };
    fmt f = { 0, 0, 0, 0, 0, -1, -1, 0 };
    expand_emoji(text, buf, (int)sizeof buf);
    while (*s != '\0') {
        const char *run = s;
        while (*s != '\0' && strchr("*_`~{@[:", *s) == 0) s++;     /* plain text goes in as one run */
        if (s > run) { style_set(b, &have, &want); sb_run(b, run, (int)(s - run)); }
        if (*s == '\0') break;
        if (s[0] == '*' && s[1] == '*' && (f.bold || has_closer(s, 2, "**"))) { f.bold = !f.bold; s += 2; }
        else if (s[0] == '*' && s[1] != '*' && (f.italic || has_closer(s, 1, "*"))) { f.italic = !f.italic; s++; }
        else if (s[0] == '_' && s[1] == '_' && (f.under || has_closer(s, 2, "__"))) { f.under = !f.under; s += 2; }
        else if (s[0] == '~' && s[1] == '~' && (f.strike || has_closer(s, 2, "~~"))) { f.strike = !f.strike; s += 2; }
        else if (s[0] == '`' && s[1] != '`' && s[1] != '\0' && strchr(s + 1, '`') != 0) {
            const char *e = strchr(s + 1, '`');
            style code = want;
            code.attr |= UI_CELLS_A_REVERSE;
            style_set(b, &have, &code);
            sb_ch(b, ' ');
            sb_run(b, s + 1, (int)(e - s - 1));
            sb_ch(b, ' ');
            s = e + 1;
            continue;
        }
        else if (s[0] == '~' && effect_tag(s, &(int){0}, &(int){0}, &(int){0}) > 0) {
            int closing, fg, bd, n = effect_tag(s, &closing, &fg, &bd);
            f.eff_fg = closing ? -1 : fg;
            f.eff_bold = closing ? 0 : bd;
            s += n;
        }
        else if (s[0] == '{' && (brace_tag(s, &f, &(int){0}) > 0)) {
            int lit, n = brace_tag(s, &f, &lit);
            if (lit) { style_set(b, &have, &want); sb_ch(b, lit); }
            s += n;
        }
        else if (s[0] == '@' && is_word(s[1])) {                    /* utils/mentions.ts highlightMentions */
            const char *e = s + 1;
            int len;
            while (is_word(*e)) e++;
            len = (int)(e - s - 1);
            if ((int)strlen(self_nick) == len && same_word(s + 1, self_nick, len)) sb_spot(b, &have, &want, 3, 1, s, (int)(e - s));
            else if ((len == 8 && same_word(s + 1, "everyone", 8)) || (len == 4 && same_word(s + 1, "here", 4))) sb_spot(b, &have, &want, LC_ALERT, 0, s, (int)(e - s));
            else sb_spot(b, &have, &want, LC_ACCENT, 0, s, (int)(e - s));
            s = e;
            continue;
        }
        else if (s[0] == '[' && strchr(s, ']') != 0 && strchr(s, ']')[1] == '(' && strchr(strchr(s, ']'), ')') != 0 && strchr(s, ']') > s + 1) {
            const char *rb = strchr(s, ']');                         /* [text](url): the text kept, the url dropped */
            sb_spot(b, &have, &want, LC_ACCENT, 0, s, (int)(rb - s + 1));
            s = strchr(rb, ')') + 1;
            continue;
        }
        else {                                                       /* not a marker after all: as typed */
            style_set(b, &have, &want);
            sb_ch(b, *s);
            s++;
            continue;
        }
        fmt_want(&f, &want);
    }
    style_set(b, &have, &(style){ -1, 0, 0 });                      /* the line's own tail starts from the pen */
}

const char *const lc_react_names[LC_REACT_KINDS] = { "+1", "-1", "heart", "fire", "laugh", "wow", "sad", "angry" };
static const char *const react_glyphs[LC_REACT_KINDS] = { ":)", ":/", "<3", "*", ":D", ":O", ":(", ">(" };
static const char *const react_words[LC_REACT_KINDS] = { "Thumbs Up", "Thumbs Down", "Heart", "Fire",
                                                          "Laugh", "Wow", "Sad", "Angry" };

const char *lc_react_display(const char *emoji)
{
    int i;
    for (i = 0; i < LC_REACT_KINDS; i++) if (strcmp(lc_react_names[i], emoji) == 0) return react_glyphs[i];
    return emoji;
}

const char *lc_react_label(int i)
{
    static char labels[LC_REACT_KINDS][24];
    if (i < 0 || i >= LC_REACT_KINDS) return "";
    sprintf(labels[i], "%-3s %s", react_glyphs[i], react_words[i]);
    return labels[i];
}

void lc_react_rows(const lc_line *l, int indent, int width, char *out, int cap)
{
    int i, n = 0, col = 0, any = 0;
    char one[24];
    out[0] = '\0';
    if (indent < 0) indent = 0;
    if (width > 0 && indent > width / 2) indent = width / 2;     /* a long nick: keep room for the counts */
    for (i = 0; i < l->nreact; i++) {
        int len, gap, need, cols;
        if (l->react[i].count <= 0) continue;
        sprintf(one, "%.12s %d", lc_react_display(l->react[i].emoji), l->react[i].count);
        len = (int)strlen(one);
        cols = len;
        gap = any && !(width > 0 && col + 2 + cols > width) ? 2 : 0;
        need = gap == 0 ? 1 + indent + len : gap + len;          /* a new row: "\n" and the indent */
        if (n + need + 1 > cap) break;
        if (gap == 0) {
            out[n++] = '\n';
            memset(out + n, ' ', (size_t)indent);
            n += indent;
            col = indent;
        } else {
            out[n++] = ' '; out[n++] = ' ';
            col += 2;
        }
        memcpy(out + n, one, (size_t)len);
        n += len;
        col += cols;
        out[n] = '\0';
        any = 1;
    }
}

void lc_line_marked(const lc_line *l, char *out, int cap, int stamps, int width)
{
    sb b;
    int indent = stamps ? 8 : 0;                     /* "[hh:mm] ": the message text column */
    b.p = out; b.n = 0; b.cap = cap;
    out[0] = '\0';
    if ((l->kind == LC_HISTORY || l->kind == LC_LIVE) && l->quote[0] != '\0') {   /* TS reply-quote: a dim row above */
        sb_fg(&b, LC_DIM); sb_str(&b, "  > "); sb_str(&b, l->quote); sb_ch(&b, '\n'); sb_ch(&b, UI_CELLS_M_BASE);
    }
    switch (l->kind) {
    case LC_SYSTEM:
        sb_fg(&b, LC_DIM); sb_str(&b, "*** ");
        if (l->tone > 0) sb_fg(&b, l->tone);                                  /* TS "{accentAlt}[MOTD] x{/}" inside the grey */
        sb_str(&b, l->text);
        if (l->tone > 0) sb_fg(&b, LC_DIM);
        if (strncmp(l->text, "--- ", 4) == 0) sb_ch(&b, UI_CELLS_M_BASE);   /* the TS history marker ends its grey early */
        sb_str(&b, " ***");
        break;
    case LC_PICTURE:
        sb_ch(&b, ' ');
        break;
    case LC_NOTE:
        sb_fg(&b, l->tone > 0 ? l->tone : LC_ACCENT); sb_str(&b, l->text);
        break;
    case LC_ACTIVITY_LINE:
        sb_fg(&b, LC_ACTIVITY); sb_ch(&b, '['); sb_str(&b, l->time); sb_str(&b, "] "); sb_str(&b, l->text);
        break;
    case LC_EVENT:
        if (l->time[0] != '\0') { sb_fg(&b, LC_ACCENT); sb_ch(&b, '['); sb_str(&b, l->time); sb_ch(&b, ']'); sb_ch(&b, UI_CELLS_M_BASE); sb_ch(&b, ' '); }
        sb_fg(&b, l->tone); sb_str(&b, l->head); sb_ch(&b, UI_CELLS_M_BASE);
        if (l->body_tone > 0) sb_fg(&b, l->body_tone);
        sb_str(&b, l->text);
        if (l->pb) { sb_ch(&b, UI_CELLS_M_BASE); sb_ch(&b, ' '); sb_fg(&b, LC_EV_LIGHTGREEN); sb_str(&b, "[NEW PB!]"); }
        break;
    case LC_HISTORY:                                  /* [hh:mm] <nick> text */
        if (stamps) { sb_fg(&b, LC_DIM); sb_ch(&b, '['); sb_str(&b, l->time); sb_str(&b, "] "); sb_ch(&b, UI_CELLS_M_BASE); }
        sb_str(&b, "<");
        sb_fg(&b, lc_nick_color(l->nick)); sb_str(&b, l->label[0] != '\0' ? l->label : l->nick);
        sb_ch(&b, UI_CELLS_M_BASE); sb_str(&b, "> "); sb_markdown(&b, l->text);
        break;
    default:                                          /* [hh:mm] nick: text, nick bold */
        if (stamps) { sb_fg(&b, LC_DIM); sb_ch(&b, '['); sb_str(&b, l->time); sb_str(&b, "] "); sb_ch(&b, UI_CELLS_M_BASE); }
        sb_fg(&b, lc_nick_color(l->nick)); sb_ch(&b, UI_CELLS_M_BOLD); sb_str(&b, l->label[0] != '\0' ? l->label : l->nick);
        sb_ch(&b, UI_CELLS_M_BASE); sb_str(&b, ": "); sb_markdown(&b, l->text);
        break;
    }
    if ((l->kind == LC_HISTORY || l->kind == LC_LIVE) && l->edited) {   /* TS messageLine's mark */
        sb_ch(&b, UI_CELLS_M_BASE); sb_fg(&b, LC_DIM); sb_str(&b, " (edited)");
    }
    if (l->kind == LC_HISTORY || l->kind == LC_LIVE) {   /* counts on their own rows under it (sysop 2026-11-08) */
        char rows[LC_REACT_MAX * 40];
        indent += (int)strlen(l->label[0] != '\0' ? l->label : l->nick) + (l->kind == LC_HISTORY ? 3 : 2);   /* "<nick> " or "nick: " */
        lc_react_rows(l, indent, width, rows, (int)sizeof rows);
        if (rows[0] != '\0') { sb_ch(&b, UI_CELLS_M_BASE); sb_fg(&b, LC_ACCENT); sb_str(&b, rows); }
    }
}

static void centre(ui_cells *c, int row, int left, int width, const char *text)
{
    int len = (int)strlen(text), at = (width - len) / 2;
    if (at < 0) at = 0;
    ui_cells_text(c, row, left + at, text, width);
}

static void draw_menubar(ui_cells *c)
{
    int w = c->cols;
    ui_cells_bar(c, 0, 0, w, MENU_TEXT, LC_BAR_INK, LC_ACCENT, 0);
    if (w < 60) return;
    /* The filler and the masthead at the right; the gap 12 cells in is where the TS bar's scanner sat in the capture. */
    ui_cells_pen(c, LC_ACCENT, LC_ACCENT, 0);
    ui_cells_fill(c, 0, MENU_TEXT_COLS, 1, w - 12 - MENU_TEXT_COLS + 1, '/');
    if (MENU_TEXT_COLS + 12 <= w - 12) ui_cells_put(c, 0, MENU_TEXT_COLS + 12, ' ');   /* the scanner gap in the filler */
    ui_cells_pen(c, LC_GREY, LC_ACCENT, 0);
    ui_cells_put(c, 0, w - 11, ' ');
    ui_cells_put(c, 0, w - 1, ' ');
    ui_cells_pen(c, LC_INK, LC_ACCENT, 0);
    ui_cells_text(c, 0, w - 10, MASTHEAD, 9);
}

static void draw_side_item(ui_cells *c, const lc_side *s, int row, int width, int selected)
{
    char buf[128];
    const char *mark = selected ? "> " : "  ";
    switch (s->kind) {
    case LC_SIDE_HEAD: sprintf(buf, "  \001\314\002%.38s", s->label); break;
    case LC_SIDE_ROOM:
    {   /* TS sidebar-items-builder: active ink, unread ok, else dim; arrow v when expanded; (n) unread */
        char tail[16] = "";
        int pen = s->joined ? LC_INK : s->unread > 0 ? LC_OK : LC_DIM;
        if (s->unread > 0) sprintf(tail, " (%d)", s->unread);
        sprintf(buf, "%s\001%c%c %s %.36s%s", mark, (char)pen, s->open ? 'v' : '>',
                s->art ? "[A]" : "#", s->label, tail);
        break;
    }
    case LC_SIDE_USER: {                   /* TS user-status marks: [*] online, [~] away, [-] do not disturb; [!] busy */
        const char *dot = s->status == 'a' ? "[~]" : s->status == 'b' ? "[!]" : s->status == 'd' ? "[-]" : "[*]";
        const char *name = s->shown[0] != '\0' ? s->shown : s->label;
        if (s->note[0] != '\0') sprintf(buf, "%s  \001%c%s\004 %.20s: %.14s", mark, (char)LC_DIM, dot, name, s->note);
        else sprintf(buf, "%s  \001%c%s\004 %.38s", mark, (char)LC_DIM, dot, name);
        break;
    }
    case LC_SIDE_DM: sprintf(buf, "%s@ %.38s", mark, s->label); break;
    default: return;
    }
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
    ui_cells_marked(c, row, 1, buf, 0, (int)sizeof buf, width);
}

static void draw_sidebar(ui_cells *c, const lc_model *m, int top, int h, int w)
{
    int i;
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
    ui_cells_fill(c, top + 1, 1, h - 2, w - 2, ' ');
    ui_cells_box(c, top, 0, h, w, m->focus == LC_FOCUS_SIDEBAR ? LC_BORDER_FOCUS : LC_ACCENT, "Sidebar", LC_ACCENT);
    for (i = 0; i < m->nside && i < h - 2; i++)
        draw_side_item(c, &m->side[i], top + 1 + i, w - 2, i == m->side_sel);
}


#define WRAP_MAX 24
static int pickable(const lc_line *l) { return l->number != 0 && l->id[0] != '\0'; }

int lc_pick_index(const lc_model *m)
{
    int i;
    if (m->pick[0] == '\0') return -1;
    for (i = 0; i < m->nlines; i++)
        if (pickable(&m->lines[i]) && strcmp(m->lines[i].id, m->pick) == 0) return i;
    return -1;
}

int lc_pick_start(lc_model *m)
{
    int i;
    for (i = m->nlines - 1; i >= 0; i--)
        if (pickable(&m->lines[i])) {
            strncpy(m->pick, m->lines[i].id, sizeof m->pick - 1);
            m->pick[sizeof m->pick - 1] = '\0';
            return 1;
        }
    m->pick[0] = '\0';
    return 0;
}

void lc_pick_move(lc_model *m, int dir)
{
    int i = lc_pick_index(m);
    if (i < 0) return;
    for (i += dir; i >= 0 && i < m->nlines; i += dir)
        if (pickable(&m->lines[i])) {
            strncpy(m->pick, m->lines[i].id, sizeof m->pick - 1);
            m->pick[sizeof m->pick - 1] = '\0';
            return;
        }
}

void lc_pick_stop(lc_model *m)
{
    m->pick[0] = '\0';
    m->scroll = 0;
}

/* The picked message's rows as the TS selection bar: ground ink on the accent. */
static void tint_row(ui_cells *c, int row, int left, int width)
{
    ui_cell *p = &c->back[row * c->cols + left];
    int k;
    for (k = 0; k < width; k++, p++) {
        p->fg = LC_BAR_INK;
        p->bg = LC_ACCENT;
        p->flags &= (unsigned char)~(UI_CELLS_BG_DEFAULT | UI_CELLS_BOLD);
    }
}

/* TS rebuildChatContent preview: {dim}[time]{/dim} <{color}nick{/color}> buffer, then the cursor:
   an inverse space in the typer's colour while the blink is on, else a space. */
static void typing_marked(const lc_model *m, int i, char *out, int cap)
{
    sb b;
    const lc_typing *t = &m->typing[i];
    int color = lc_nick_color(t->nick);
    b.p = out; b.n = 0; b.cap = cap;
    out[0] = '\0';
    sb_fg(&b, LC_DIM); sb_ch(&b, '['); sb_str(&b, m->typing_time); sb_ch(&b, ']'); sb_ch(&b, UI_CELLS_M_BASE);
    sb_str(&b, " <"); sb_fg(&b, color); sb_str(&b, t->nick); sb_ch(&b, UI_CELLS_M_BASE);
    sb_str(&b, "> "); sb_str(&b, t->text);
    if (m->typing_blink) { sb_fg(&b, color); sb_ch(&b, UI_CELLS_M_ATTR); sb_ch(&b, UI_CELLS_ATTR(UI_CELLS_A_REVERSE)); sb_ch(&b, ' '); sb_ch(&b, UI_CELLS_M_BASE); }
    else sb_ch(&b, ' ');
}

#define CHAT_ROWS_MAX 128
static ui_hit lc_hits;                              /* the last frame's click regions, topmost last */
int lc_chat_body, lc_chat_left, lc_chat_rows, lc_chat_width;
lc_rect lc_covers[LC_COVER_MAX];
int lc_cover_n;

/** Record an overlay drawn over the log this frame; past LC_COVER_MAX the whole screen counts as covered. */
static void cover(int row, int col, int rows, int cols)
{
    if (lc_cover_n == LC_COVER_MAX) { lc_covers[LC_COVER_MAX - 1].row = 0; lc_covers[LC_COVER_MAX - 1].col = 0;
        lc_covers[LC_COVER_MAX - 1].rows = 1000; lc_covers[LC_COVER_MAX - 1].cols = 1000; return; }
    lc_covers[lc_cover_n].row = row; lc_covers[lc_cover_n].col = col;
    lc_covers[lc_cover_n].rows = rows; lc_covers[lc_cover_n].cols = cols;
    lc_cover_n++;
}

/** ui_cells_modal that records its box as a cover (same clamp as ui_cells_modal). */
static void lc_modal(ui_cells *c, int h, int w, int border_fg, const char *label, int label_fg, int *top, int *left)
{
    ui_cells_modal(c, h, w, border_fg, label, label_fg, top, left);
    cover(*top, *left, h > c->rows ? c->rows : h, w > c->cols ? c->cols : w);
}
static int chat_row_line[CHAT_ROWS_MAX];            /* chat body row -> line index, -1 empty */

/* The scrollbar of the last drawn chat pane, for lc_scroll_at (R1a drag). bar_rows 0 = no scrollbar. */
static int bar_body, bar_rows, bar_thumb, bar_span;

int lc_chat_row_line(int r)
{
    return r >= 0 && r < CHAT_ROWS_MAX ? chat_row_line[r] : -1;
}

static void draw_chat(ui_cells *c, const lc_model *m, int top, int h, int left, int w)
{
    static char marked[LC_TEXT_MAX * 2 + 96];
    static int line_rows[LC_LINES];                 /* pass 1's wrapped row count per line */
    static unsigned long cache_gen;                 /* what line_rows was computed for: the model's lines_gen ... */
    static int cache_n = -1, cache_width, cache_stamps;   /* ... and the inputs that change a wrap */
    int stamps = m->timestamps && !m->compact, used = 0;
    int starts[WRAP_MAX], ends[WRAP_MAX];
    int rows = h - 3, width = w - 4, total = 0, first, at = 0, i, k, n, body;
    int picked = lc_pick_index(m), pick_at = 0;

    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
    ui_cells_box(c, top, left, h, w, picked >= 0 ? LC_BORDER_FOCUS : LC_ACCENT, m->pane_label[0] != '\0' ? m->pane_label : "No channel", LC_INK);
    if (c->cols > 40) {                             /* the TS pane drops its label bar at 40 columns */
        ui_cells_bar(c, top + 1, left + 1, w - 5, " Chat", LC_GREY, LC_LABEL_BG, 0);
        ui_cells_bar(c, top + 1, left + w - 4, 3, "", LC_LABEL_TAIL, LC_LABEL_BG, 0);
        body = top + 2;
    } else {
        body = top + 1;
        rows++;
    }
    lc_chat_rows = 0;
    if (rows < 1 || width < 1) {
        ui_cells_fill(c, body, left + 1, top + h - 1 - body, w - 2, ' ');
        return;
    }
    lc_chat_body = body; lc_chat_left = left + 1; lc_chat_rows = rows < CHAT_ROWS_MAX ? rows : CHAT_ROWS_MAX; lc_chat_width = width;
    ui_hit_add(&lc_hits, LC_HIT_CHAT, body, left + 1, rows, w - 2);
    for (k = 0; k < CHAT_ROWS_MAX; k++) chat_row_line[k] = -1;

    if (cache_gen != m->lines_gen || cache_n != m->nlines || cache_width != width || cache_stamps != stamps) {
        for (i = 0; i < m->nlines; i++) {           /* pass 1: only when a line, the width or the stamps changed */
            lc_line_marked(&m->lines[i], marked, (int)sizeof marked, stamps, width);
            line_rows[i] = ui_cells_wrap(marked, width, starts, ends, WRAP_MAX);
        }
        cache_gen = m->lines_gen; cache_n = m->nlines; cache_width = width; cache_stamps = stamps;
        lc_chat_rewraps++;
    }
    for (i = 0; i < m->nlines; i++) {
        if (i == picked) pick_at = total;
        total += line_rows[i];
    }
    for (i = 0; i < m->ntyping; i++) {             /* the typing rows follow the log (few, so always rewrapped) */
        typing_marked(m, i, marked, (int)sizeof marked);
        total += ui_cells_wrap(marked, width, starts, ends, WRAP_MAX);
    }
    if (picked >= 0) first = pick_at + line_rows[picked] / 2 - rows / 2;   /* the picker centres its selection */
    else first = total - m->scroll - rows;          /* the newest rows, filled from the top */
    if (first > total - rows) first = total - rows;
    if (first < 0) first = 0;
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
    for (i = 0; i < m->nlines && at < first + rows; i++) {
        if (at + line_rows[i] <= first) { at += line_rows[i]; continue; }   /* scrolled off the top: not rebuilt */
        lc_line_marked(&m->lines[i], marked, (int)sizeof marked, stamps, width);
        n = ui_cells_wrap(marked, width, starts, ends, WRAP_MAX);
        for (k = 0; k < n; k++, at++) {
            int r = at - first, cols;
            if (at < first || at >= first + rows) continue;
            if (r < CHAT_ROWS_MAX) chat_row_line[r] = i;
            cols = ui_cells_marked(c, body + r, left + 1, marked, starts[k], ends[k], width);
            ui_cells_fill(c, body + r, left + 1 + cols, 1, w - 2 - cols, ' ');   /* the rest of the row: text rows are not pre-cleared */
            used = r + 1;
            if (i == picked) tint_row(c, body + r, left + 1, width);
        }
    }
    for (i = 0; i < m->ntyping && at < first + rows; i++) {   /* preview rows: chat_row_line stays -1 */
        typing_marked(m, i, marked, (int)sizeof marked);
        n = ui_cells_wrap(marked, width, starts, ends, WRAP_MAX);
        for (k = 0; k < n; k++, at++) {
            int r = at - first, cols;
            if (at < first || at >= first + rows) continue;
            cols = ui_cells_marked(c, body + r, left + 1, marked, starts[k], ends[k], width);
            ui_cells_fill(c, body + r, left + 1 + cols, 1, w - 2 - cols, ' ');
            used = r + 1;
        }
    }
    ui_cells_fill(c, body + used, left + 1, rows - used, w - 2, ' ');   /* rows with no line */
    if (total > rows) {                             /* scrollbar: track 66, thumb 195, one column in from the border */
        int thumb = rows * rows / total, pos;
        if (thumb < 1) thumb = 1;
        pos = (first * (rows - thumb)) / (total - rows);
        ui_cells_pen(c, LC_SCROLL_TRACK, LC_SCROLL_TRACK, 0);
        ui_cells_fill(c, body, left + w - 3, pos, 1, ' ');
        ui_cells_fill(c, body + pos + thumb, left + w - 3, rows - pos - thumb, 1, ' ');
        ui_cells_pen(c, LC_SCROLL_THUMB, LC_SCROLL_THUMB, 0);
        ui_cells_fill(c, body + pos, left + w - 3, thumb, 1, ' ');
        ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
        bar_body = body; bar_rows = rows; bar_thumb = thumb; bar_span = total - rows;
        ui_hit_add(&lc_hits, LC_HIT_SCROLLBAR, body, left + w - 3, rows, 1);   /* over the chat rect: last added wins */
    }
}

/* The first input character on screen: the input scrolls to keep the cursor in its box. */
static int input_from(int W, const lc_model *m)
{
    int iw = (W - 6 >= 24) ? W - 6 : W, room = iw - 3;
    return m->input_cur > room ? m->input_cur - room : 0;
}

static void draw_input(ui_cells *c, const lc_model *m, int *cr, int *cc)
{
    int W = c->cols, row = c->rows - 4, iw = (W - 6 >= 24) ? W - 6 : W, room = iw - 3, from = input_from(W, m);
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
    ui_cells_fill(c, row + 1, 1, 1, iw - 2, ' ');
    ui_cells_box(c, row, 0, 3, iw, m->focus == LC_FOCUS_INPUT ? LC_BORDER_FOCUS : LC_ACCENT,
                 m->pick[0] != '\0' ? "[PICK MESSAGE: Up/Down, Enter, Esc]" : m->editing ? "[EDITING]" : "Message", LC_INK);
    ui_cells_text(c, row + 1, 1, m->input + from, room);
    if (m->input_anchor >= 0 && m->input_anchor != m->input_cur) {   /* the Shift selection, inverse */
        int a = m->input_anchor < m->input_cur ? m->input_anchor : m->input_cur;
        int b = m->input_anchor < m->input_cur ? m->input_cur : m->input_anchor;
        if (a < from) a = from;
        if (b > from + room) b = from + room;
        if (b > a) {
            ui_cells_pen(c, LC_BAR_INK, LC_INK, 0);
            ui_cells_text(c, row + 1, 1 + a - from, m->input + a, b - a);
            ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
        }
    }
    *cr = row + 1;
    *cc = 1 + m->input_cur - from;
    if (iw < W) ui_cells_button(c, row, iw, 6, ":D", LC_GREY, LC_ACTIVITY);
}

/* The one command list: /help <command> and the "/" menu both read it. TS /help <command>: name, aliases, description, usage - the registered text of the commands this door runs. */
const char *const lc_cmd_help[][4] = {
    {"ban", "", "Ban user from channel", "/ban @user [reason]"},
    {"clear", "cls", "Clear chat display", "/clear"},
    {"create", "", "Create a new channel. --private and --invite-only make it members-only.", "/create #name [topic] [--private] [--invite-only] [--motd \"text\"]"},
    {"deletemsg", "delmsg", "Delete a message (your last by default)", "/deletemsg [msg-id]"},
    {"edit", "", "Edit one of your messages (your last by default)", "/edit [msg-id] new text"},
    {"away", "afk", "Set away status", "/away [message]"},
    {"back", "", "Return from away", "/back"},
    {"help", "?,commands", "Show available commands", "/help [command]"},
    {"ignore", "", "Ignore a user (no name: list who you ignore)", "/ignore [user]"},
    {"invite", "", "Invite a user to this invite-only channel (moderator only)", "/invite @user [#room]"},
    {"join", "", "Join a channel", "/join #channel"},
    {"kick", "", "Kick user from channel", "/kick @user [reason]"},
    {"leave", "part", "Leave current channel", "/leave"},
    {"love", "", "React with heart", "/love [msg-id]"},
    {"nick", "", "Show your username", "/nick"},
    {"msg", "dm,pm", "Send a direct message (1:1 or group). /msg @a [@b ...] text", "/msg @user [@user2 ...] message"},
    {"mute", "", "Mute user in channel", "/mute @user [minutes]"},
    {"op", "", "Give or take room operator status (sysop)", "/op @user  |  /op -@user  |  /deop @user"},
    {"quit", "exit,bye,q", "Exit chat", "/quit"},
    {"react", "+1,-1,heart,fire", "Add reaction to message", "/react [emoji] [msg-id]"},
    {"reactions", "", "Show reactions", "/reactions [msg-id]"},
    {"reply", "r", "Reply to a message (the newest by default)", "/reply [msg-id] message"},
    {"rooms", "channels", "List all rooms", "/rooms"},
    {"status", "", "Set custom status", "/status [online|away|busy|dnd] [message]"},
    {"thread", "t", "Reply in thread", "/thread <msg-id> message"},
    {"thumbsup", "like", "React with thumbs up", "/thumbsup [msg-id]"},
    {"topic", "", "Set channel topic", "/topic <new topic>"},
    {"unignore", "", "Stop ignoring a user", "/unignore <user>"},
    {"unban", "", "Unban user from channel", "/unban @user"},
    {"uninvite", "", "Revoke a pending invite", "/uninvite @user [#room]"},
    {"unmute", "", "Unmute user in channel", "/unmute @user"},
    {"unreact", "", "Remove reaction", "/unreact [emoji] [msg-id]"},
    {"whois", "info", "Show user info", "/whois @user"},
    {"who", "users,list", "List users in channel", "/who"},
    {"compact", "", "Toggle compact mode", "/compact"},
    {"timestamps", "time", "Toggle timestamps", "/timestamps"},
    /* The rest of the TS registry (board/Doors/livechat/commands, one file per group). Phase A runs me, delete, events, pin, unpin, pinned, search, motd and customemoji; the others fall to the unknown-command path. */
    {"delete", "", "Delete a channel", "/delete #channel"},
    {"emoji", "", "Open emoji picker or search emojis", "/emoji [search]"},
    {"emojis", "", "List all emojis or emojis in a category", "/emojis [category]"},
    {"customemoji", "", "Add, remove or list the board's custom emoji (sysop adds)", "/customemoji add <name> <display> <ascii> | remove <name> | list"},
    {"events", "", "Manage BBS event display preferences", "/events [on|off|logins|files|doors|messages|announcements]"},
    {"mic", "", "List microphones, or switch to one", "/mic [number]"},
    {"discord", "", "Show or switch the Discord mirror of this board's chat (sysop switches)", "/discord [on|off]"},
    {"motd", "", "Set channel MOTD (moderator only; --clear to clear; no args = show current)", "/motd [text]  (use /motd --clear to clear)"},
    {"mode", "", "Set channel or user modes (+i/+m/+o/+v/-X). Moderator only.", "/mode <modes> [param]"},
    {"me", "", "Send an action message", "/me does something"},
    {"pin", "", "Pin a message", "/pin <msg-id>"},
    {"unpin", "", "Unpin a message", "/unpin <msg-id>"},
    {"pinned", "pins", "Show pinned messages", "/pinned"},
    {"sounds", "", "Toggle sound notifications", "/sounds [on|off]"},
    {"search", "find", "Search messages", "/search [query]"},
    {"draw", "whiteboard,art", "Open the drawing whiteboard (or join art:<name>)", "/draw [channel]"},
    {"files", "file,share", "Open the file sharing browser", "/files"},
    {"view", "picture", "Preview a shared picture", "/view <number>"},
    {"settings", "", "Open the settings", "/settings"},
    {"voice", "vc", "Voice channel commands", "/voice [join <channel> | leave]"},
    {"deafen", "deaf", "Deafen (mute audio output)", "/deafen"},
    {"undeafen", "undeaf", "Undeafen (unmute audio output)", "/undeafen"},
};


const int lc_cmd_help_count = (int)(sizeof lc_cmd_help / sizeof lc_cmd_help[0]);

static int lower(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }

static int has_sub(const char *s, const char *term)
{
    int n = (int)strlen(term), i;
    if (n == 0) return 1;
    for (i = 0; s[i] != '\0'; i++) {
        int k = 0;
        while (k < n && s[i + k] != '\0' && lower(s[i + k]) == lower(term[k])) k++;
        if (k == n) return 1;
    }
    return 0;
}

int lc_slash_match(const char *input, int *idx, int max)
{
    const char *term;
    int n = 0, pre[LC_SLASH_MAX], i, j, key, np;
    if (input[0] != '/') return 0;
    term = input + 1;
    np = (int)strlen(term);
    for (i = 0; i < lc_cmd_help_count && n < max && n < LC_SLASH_MAX; i++) {
        const char *name = lc_cmd_help[i][0];
        int p = 1;
        for (j = 0; j < np; j++) if (lower(name[j]) != lower(term[j]) || name[j] == '\0') { p = 0; break; }
        if (!p && !has_sub(lc_cmd_help[i][2], term)) continue;
        /* insertion sort: prefix matches first, then by name */
        key = p;
        for (j = n; j > 0; j--) {
            int o = idx[j - 1], c = strcmp(lc_cmd_help[o][0], name);
            if (pre[j - 1] > key || (pre[j - 1] == key && c < 0)) break;
            idx[j] = idx[j - 1]; pre[j] = pre[j - 1];
        }
        idx[j] = i; pre[j] = key;
        n++;
    }
    return n;
}

void lc_slash_row(char *out, int cap, int cmd, int cols, int *ucol, int *ulen)
{
    const char *name = lc_cmd_help[cmd][0], *desc = lc_cmd_help[cmd][2], *usage = lc_cmd_help[cmd][3];
    *ucol = 0; *ulen = 0;
    if (cap < 1) return;
    if (cols < 80) {
        int room = cols - 4 < 8 ? 8 : cols - 4;
        if (room > cap - 1) room = cap - 1;
        if (desc[0] != '\0') sprintf(out, "/%s %s", name, desc); else sprintf(out, "/%s", name);
        if ((int)strlen(out) > room) out[room] = '\0';
        return;
    }
    {
        int dw = cols - 12 - 20 - 6, ul, len;
        char u[24];
        if (dw < 10) dw = 10;
        ul = (int)strlen(usage);
        if (ul > 20) ul = 20;
        memcpy(u, usage, (size_t)ul);
        u[ul] = '\0';
        len = sprintf(out, "/%-12.12s ", name);
        *ucol = len;
        *ulen = 20;
        len += sprintf(out + len, "%-20s ", u);
        sprintf(out + len, "%.*s", dw < cap - len - 1 ? dw : cap - len - 1, desc);
    }
}

/** TS server.ts ghost completion: the top match, only when its name starts with the whole typed
 *  term (input after "/", case ignored). Returns that command's index, or -1. Tab/Right fill it. */
int lc_slash_ghost(const char *input, const int *idx, int n)
{
    const char *term = input + 1, *name;
    int i = 0;
    if (n <= 0 || input[0] != '/' || term[0] == '\0') return -1;
    name = lc_cmd_help[idx[0]][0];
    while (term[i] != '\0' && name[i] != '\0' && lower(term[i]) == lower(name[i])) i++;
    return term[i] == '\0' ? idx[0] : -1;
}

/* 1 when input (after leading spaces, before trailing ones) is exactly "/" + the n bytes of word. */
static int slash_is(const char *input, const char *word, int n)
{
    int i = 0;
    while (input[0] == ' ') input++;
    if (input[0] != '/') return 0;
    input++;
    while (i < n && input[i] != '\0' && lower(input[i]) == lower(word[i])) i++;
    if (i < n) return 0;
    input += i;
    while (*input == ' ') input++;
    return *input == '\0';
}

int lc_slash_exact(const char *input, int cmd)
{
    const char *name = lc_cmd_help[cmd][0];
    return slash_is(input, name, (int)strlen(name));
}

int lc_slash_complete(const char *input)
{
    int i;
    for (i = 0; i < lc_cmd_help_count; i++) {
        const char *a = lc_cmd_help[i][1];
        if (lc_slash_exact(input, i)) return 1;
        while (*a != '\0') {                              /* comma-separated alias list */
            int n = 0;
            while (a[n] != '\0' && a[n] != ',') n++;
            if (n > 0 && slash_is(input, a, n)) return 1;
            a += n;
            if (*a == ',') a++;
        }
    }
    return 0;
}

int lc_slash_scroll(int sel, int top, int n)
{
    if (sel < top) top = sel;
    if (sel >= top + LC_SLASH_ROWS) top = sel - LC_SLASH_ROWS + 1;
    if (top > n - LC_SLASH_ROWS) top = n - LC_SLASH_ROWS;
    return top < 0 ? 0 : top;
}

static void draw_slash(ui_cells *c, const lc_model *m)
{
    int idx[LC_SLASH_MAX], n, rows, top, i, W = c->cols, brow = c->rows - 4;
    char row[200];
    if (m->slash_off || m->fmt || m->focus != LC_FOCUS_INPUT || m->modal != LC_MODAL_NONE) return;
    n = lc_slash_match(m->input, idx, LC_SLASH_MAX);
    if (n == 0) return;
    rows = n < LC_SLASH_ROWS ? n : LC_SLASH_ROWS;
    top = lc_slash_scroll(m->slash_sel < n ? m->slash_sel : n - 1, m->slash_top, n);
    if (brow - rows - 2 < 1) return;
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
    ui_cells_fill(c, brow - rows - 1, 1, rows, W - 2, ' ');
    ui_cells_box(c, brow - rows - 2, 0, rows + 2, W, LC_ACCENT, " Commands ", LC_INK);
    cover(brow - rows - 2, 0, rows + 2, W);
    for (i = 0; i < rows; i++) {
        int r = brow - rows - 1 + i, ucol, ulen, sel = top + i == m->slash_sel;
        lc_slash_row(row, sizeof row, idx[top + i], W, &ucol, &ulen);
        ui_cells_pen(c, sel ? LC_ACCENT : LC_INK, UI_CELLS_DEFAULT, 0);
        ui_cells_text(c, r, 1, sel ? "> " : "  ", 2);
        ui_cells_text(c, r, 3, row, W - 4);
        if (ulen > 0) {                                  /* the usage column in the dim colour */
            ui_cells_pen(c, LC_DIM, UI_CELLS_DEFAULT, 0);
            if (ucol < W - 4) ui_cells_text(c, r, 3 + ucol, row + ucol, ulen < W - 4 - ucol ? ulen : W - 4 - ucol);
        }
    }
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
}

/* ---- the format picker: TS ui/format-picker.ts FORMATS on the SDK CategoryPicker
 *      (engines/ui/blessed/widgets/category-picker.ts) ---- */
static const char *const fmt_cats[LC_FMT_CATS] = { "Colors", "Effects", "Markdown" };
static const struct { const char *name; int tok; } fmt_colors[] = {
    { "Red", UI_T_ALERT }, { "Green", UI_T_OK }, { "Blue", UI_T_BAR }, { "Yellow", UI_T_ACCENT_ALT },
    { "Cyan", UI_T_ACCENT }, { "Magenta", UI_T_ACCENT_ALT }, { "White", UI_T_INK }, { "Gray", UI_T_DIM } };
static const struct { const char *name, *open, *close; } fmt_md[] = {
    { "Bold", "**", "**" }, { "Italic", "*", "*" }, { "Underline", "__", "__" }, { "Strike", "~~", "~~" }, { "Code", "`", "`" } };
static const int fmt_counts[LC_FMT_CATS] = { 8, FX_COUNT, 5 };
#define FMT_ITEM_ROWS 6            /* the item list: height 100% of the 8 content rows, less its border */

const char *lc_fmt_cat_name(int cat) { return cat >= 0 && cat < LC_FMT_CATS ? fmt_cats[cat] : ""; }
int lc_fmt_count(int cat) { return cat >= 0 && cat < LC_FMT_CATS ? fmt_counts[cat] : 0; }

const char *lc_fmt_name(int cat, int item)
{
    if (item < 0 || item >= lc_fmt_count(cat)) return "";
    return cat == 0 ? fmt_colors[item].name : cat == 1 ? fx[item].label : fmt_md[item].name;
}

static const unsigned long *fmt_rgb(const unsigned long *rgb)
{
    return rgb != 0 ? rgb : ui_theme_by_id("")->rgb;
}

int lc_fmt_wrap(char *out, int cap, int cat, int item, const char *text, int len, const unsigned long *rgb)
{
    int n;
    if (len < 0) len = (int)strlen(text);
    if (item < 0 || item >= lc_fmt_count(cat) || cap <= 0) return -1;
    if (cat == 0)        /* {T.x-fg}t{/} */
        n = snprintf(out, (size_t)cap, "{#%06lX-fg}%.*s{/}", fmt_rgb(rgb)[fmt_colors[item].tok] & 0xFFFFFFUL, len, text);
    else if (cat == 1)   /* ~name~t~/name~; gradient carries from=red to=blue */
        n = snprintf(out, (size_t)cap, "~%s%s~%.*s~/%s~", fx[item].name,
                     strcmp(fx[item].name, "gradient") == 0 ? " from=red to=blue" : "", len, text, fx[item].name);
    else
        n = snprintf(out, (size_t)cap, "%s%.*s%s", fmt_md[item].open, len, text, fmt_md[item].close);
    return n < 0 || n >= cap ? -1 : n;
}

void lc_fmt_open(lc_model *m, int a, int b)
{
    m->fmt = 1;
    m->fmt_kind = LC_PICK_FORMAT;
    m->fmt_cat = m->fmt_item = m->fmt_item_top = 0;
    m->fmt_pane = LC_FMT_PANE_CAT;
    m->fmt_a = a < b ? a : b;
    m->fmt_b = a < b ? b : a;
}

/* ---- the emoji picker: TS ui/emoji-picker.ts, categories Emotions/Actions/Symbols/Special ---- */
static const char *const emoji_cats[LC_EMOJI_CATS] = { "Emotions", "Actions", "Symbols", "Special" };
static char emoji_code_buf[LC_CUSTOM_EMOJI_LEN + 4];

const char *lc_emoji_cat_name(int cat) { return cat >= 0 && cat < LC_EMOJI_CATS ? emoji_cats[cat] : ""; }

int lc_emoji_count(int cat)
{
    int i, n = 0;
    if (cat < 0 || cat >= LC_EMOJI_CATS) return 0;
    for (i = 0; i < NEMOJI; i++) if (emoji_table[i].cat == cat) n++;
    return cat == 3 ? n + ncustom_emoji : n;
}

/* The item-th emoji of the category: a built-in index, or -1 - custom index. */
static int emoji_nth(int cat, int item)
{
    int i, n = 0;
    if (item < 0 || item >= lc_emoji_count(cat)) return NEMOJI + ncustom_emoji;
    for (i = 0; i < NEMOJI; i++) if (emoji_table[i].cat == cat && n++ == item) return i;
    return NEMOJI + (item - (n));
}

const char *lc_emoji_code(int cat, int item)
{
    int k = emoji_nth(cat, item);
    if (k < NEMOJI) return emoji_table[k].code;
    if (k - NEMOJI >= ncustom_emoji) return "";
    sprintf(emoji_code_buf, ":%s:", custom_emoji[k - NEMOJI].name);
    return emoji_code_buf;
}

const char *lc_emoji_text(int cat, int item)
{
    int k = emoji_nth(cat, item);
    if (k < NEMOJI) return emoji_table[k].text;
    return k - NEMOJI < ncustom_emoji ? custom_emoji[k - NEMOJI].ascii : "";
}

/* ---- TS utils/emojis.ts allEmojis(): the built-ins, then the board's custom emoji (keyword "custom", category Special) ---- */
static int emoji_all_cat(int k) { return k < NEMOJI ? emoji_table[k].cat : 3; }

static const char *emoji_all_keywords(int k) { return k < NEMOJI ? emoji_table[k].keywords : "custom"; }

/* 1 when `needle` (already lower case) occurs in `hay`, compared without case: JS includes() on a lower-cased string. */
static int contains_nocase(const char *hay, const char *needle)
{
    size_t i, j, nl = strlen(needle);
    for (i = 0; hay[i] != '\0'; i++) {
        for (j = 0; j < nl; j++) {
            char a = hay[i + j];
            if (a == '\0') return 0;
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (a != needle[j]) break;
        }
        if (j == nl) return 1;
    }
    return nl == 0;
}

/* TS searchEmojis(): the query is a part of the code, or a part of any one keyword. */
static int emoji_all_matches(int k, const char *lower)
{
    char code[LC_CUSTOM_EMOJI_LEN + 4], kw[64];
    const char *p = emoji_all_keywords(k);
    if (k < NEMOJI) sprintf(code, "%s", emoji_table[k].code);
    else sprintf(code, ":%s:", custom_emoji[k - NEMOJI].name);
    if (contains_nocase(code, lower)) return 1;
    while (*p != '\0') {
        size_t n = 0;
        while (p[n] != '\0' && !(p[n] == ',' && p[n + 1] == ' ')) n++;
        sprintf(kw, "%.*s", n < sizeof kw - 1 ? (int)n : (int)sizeof kw - 1, p);
        if (contains_nocase(kw, lower)) return 1;
        p += n;
        if (*p == ',') p += 2;
    }
    return 0;
}

int lc_emoji_next(int from, int cat, const char *query)
{
    char lower[64];
    int k, i;
    if (query != 0) {
        for (i = 0; query[i] != '\0' && i < 63; i++) lower[i] = (query[i] >= 'A' && query[i] <= 'Z') ? (char)(query[i] + 32) : query[i];
        lower[i] = '\0';
    }
    for (k = from; k < NEMOJI + ncustom_emoji; k++) {
        if (cat >= 0 && emoji_all_cat(k) != cat) continue;
        if (query != 0 && !emoji_all_matches(k, lower)) continue;
        return k;
    }
    return -1;
}

void lc_emoji_line(int k, char *out, int cap)
{
    const char *face = k < NEMOJI ? emoji_table[k].text : custom_emoji[k - NEMOJI].ascii;
    if (k < NEMOJI) sprintf(out, "  %s  %s (%s)", face, emoji_table[k].code, emoji_table[k].keywords);
    else sprintf(out, "  %s  :%s: (custom)", face, custom_emoji[k - NEMOJI].name);
    if ((int)strlen(out) >= cap) out[cap - 1] = '\0';
}

#define EMOJI_GLYPH_COLUMN 16      /* emoji-label.ts GLYPH_COLUMN */
int lc_emoji_label(char *out, int cap, int cat, int item, int width)
{
    const char *art = lc_emoji_text(cat, item), *code = lc_emoji_code(cat, item);
    char full[2 * LC_CUSTOM_EMOJI_TEXT + LC_CUSTOM_EMOJI_LEN + 24];
    int n;
    if (cap <= 0 || width <= 0) { if (cap > 0) out[0] = '\0'; return 0; }
    if (width <= EMOJI_GLYPH_COLUMN) sprintf(full, "%s", art);
    else sprintf(full, "%-*s%s", EMOJI_GLYPH_COLUMN, art, code);
    n = (int)strlen(full);
    if (n > width) n = width;
    if (n > cap - 1) n = cap - 1;
    memcpy(out, full, (size_t)n);
    out[n] = '\0';
    return n;
}

void lc_emoji_open(lc_model *m)
{
    lc_fmt_open(m, 0, 0);
    m->fmt_kind = LC_PICK_EMOJI;
    m->fmt_a = m->fmt_b = 0;
}

int lc_pick_cats(const lc_model *m) { return m->fmt_kind == LC_PICK_EMOJI ? LC_EMOJI_CATS : LC_FMT_CATS; }
int lc_pick_rows(const lc_model *m) { return (m->fmt_kind == LC_PICK_EMOJI ? LC_EMOJI_H : LC_FMT_H) - 4; }
static int pick_count(const lc_model *m, int cat) { return m->fmt_kind == LC_PICK_EMOJI ? lc_emoji_count(cat) : lc_fmt_count(cat); }

void lc_fmt_move(lc_model *m, int delta)
{
    int rows = lc_pick_rows(m);
    if (m->fmt_pane == LC_FMT_PANE_CAT) {
        int k = m->fmt_cat + delta;
        if (k < 0) k = 0;
        if (k >= lc_pick_cats(m)) k = lc_pick_cats(m) - 1;
        if (k != m->fmt_cat) { m->fmt_cat = k; m->fmt_item = m->fmt_item_top = 0; }   /* category-picker.ts: reload at 0 */
    } else {
        int n = pick_count(m, m->fmt_cat), k = m->fmt_item + delta;
        if (k >= n) k = n - 1;
        if (k < 0) k = 0;
        m->fmt_item = k;
        if (k < m->fmt_item_top) m->fmt_item_top = k;
        if (k >= m->fmt_item_top + rows) m->fmt_item_top = k - rows + 1;
    }
}

/* CategoryPicker.display at the TS getSelectionPosition (x = input left + 1 + selection start, y = input top + 1):
 * above the input when it fits, else below; kept on screen. */
static void fmt_box(const ui_cells *c, const lc_model *m, int *top, int *left, int *w)
{
    int W = c->cols, x = 1 + m->fmt_a - input_from(W, m), y = c->rows - 3, h = LC_FMT_H;
    *w = W - 2 > 45 ? 45 : W - 2 < 20 ? 20 : W - 2;
    if (m->fmt_kind == LC_PICK_EMOJI) {          /* emoji-picker.ts: width min(52, max(20, cols-2)), x = cols - width, y = rows - 7 */
        *w = W - 2 > 52 ? 52 : W - 2 < 20 ? 20 : W - 2;
        x = W - *w + 2;                          /* display() subtracts 2 again */
        y = c->rows - 7;
        h = LC_EMOJI_H;
    }
    *top = y - h - 1;
    if (*top < 0) *top = y + 1;
    *left = x - 2;
    if (*left + *w > W) *left = W - *w;
    if (*left < 0) *left = 0;
    if (*top < 0) *top = 0;
}

static void draw_fmt(ui_cells *c, const lc_model *m)
{
    const unsigned long *rgb = fmt_rgb(m->rgb);
    int emoji = m->fmt_kind == LC_PICK_EMOJI, H = emoji ? LC_EMOJI_H : LC_FMT_H, ncat = lc_pick_cats(m), rows = lc_pick_rows(m);
    int ground = rgb_cube(rgb[UI_T_GROUND]), ok = rgb_cube(rgb[UI_T_OK]), alt = rgb_cube(rgb[emoji ? UI_T_ACCENT : UI_T_ACCENT_ALT]);
    int top, left, w, catw, ix, iw, n = pick_count(m, m->fmt_cat), i;
    char label[80];
    fmt_box(c, m, &top, &left, &w);
    catw = w - 24 > 14 ? 14 : w - 24 < 8 ? 8 : w - 24;
    ix = left + 1 + catw;
    iw = w - 2 - catw;
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
    ui_cells_fill(c, top + 1, left + 1, H - 2, w - 2, ' ');
    ui_cells_box(c, top, left, H, w, LC_ACCENT, emoji ? " Emoji Picker [Tab | Enter] " : " Format ", LC_INK);
    cover(top, left, H, w);
    ui_cells_box(c, top + 1, left + 1, H - 4, catw, ok, " Category ", LC_INK);   /* height 100%-2 */
    for (i = 0; i < ncat; i++) {                                 /* categoryStyle: selected ground on ok */
        int sel = i == m->fmt_cat;
        ui_cells_pen(c, sel ? ground : LC_INK, sel ? ok : UI_CELLS_DEFAULT, 0);
        ui_cells_fill(c, top + 2 + i, left + 2, 1, catw - 2, ' ');
        ui_cells_text(c, top + 2 + i, left + 3, emoji ? emoji_cats[i] : fmt_cats[i], catw - 3);
    }
    sprintf(label, " %s ", emoji ? emoji_cats[m->fmt_cat] : fmt_cats[m->fmt_cat]);
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
    ui_cells_box(c, top + 1, ix, H - 2, iw, alt, label, LC_INK);
    for (i = 0; i < rows && m->fmt_item_top + i < n; i++) {      /* itemStyle: selected ground on accentAlt (format) / accent (emoji) */
        int k = m->fmt_item_top + i, sel = k == m->fmt_item, r = top + 2 + i, bg = sel ? alt : UI_CELLS_DEFAULT;
        int fg = sel ? ground : LC_INK, bold = 0, under = 0;
        ui_cells_pen(c, sel ? ground : LC_INK, sel ? alt : UI_CELLS_DEFAULT, 0);
        ui_cells_fill(c, r, ix + 1, 1, iw - 2, ' ');
        if (emoji) {                                             /* emojiLabel(e, itemWidth), a space before it */
            lc_emoji_label(label, sizeof label, m->fmt_cat, k, w - catw - 2 - 1 - 2);
            ui_cells_text(c, r, ix + 2, label, iw - 3);
            continue;
        }
        if (m->fmt_cat == 0) fg = rgb_cube(rgb[fmt_colors[k].tok]);                 /* the label in its own colour */
        else if (m->fmt_cat == 1) { fg = fx[k].fg; bold = fx[k].bold; }            /* the effect's static look */
        else if (k == 0) bold = 1;                                                  /* Bold */
        else if (k == 1 || k == 2) under = 1;                                       /* Italic, Underline: blessed underline */
        else if (k == 3) fg = rgb_cube(rgb[UI_T_DIM]);                              /* Strike: dim */
        else { fg = rgb_cube(rgb[UI_T_INK]); bg = 0; }                              /* Code: ink on black */
        ui_cells_pen(c, fg, bg, bold);
        sprintf(label, "%c%c%s", UI_CELLS_M_ATTR, UI_CELLS_ATTR(under ? UI_CELLS_A_UNDERLINE : 0), lc_fmt_name(m->fmt_cat, k));
        ui_cells_marked(c, r, ix + 2, label, 0, (int)strlen(label), iw - 3);
    }
    ui_cells_pen(c, LC_ALERT, 0, 0);                             /* the close button: red on black, right 1 */
    ui_cells_text(c, top + 1, left + w - 5, "[X]", 3);
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
    ui_hit_add(&lc_hits, LC_HIT_FMT, top, left, H, w);
    ui_hit_add(&lc_hits, LC_HIT_FMT_CAT, top + 2, left + 2, ncat, catw - 2);
    ui_hit_add(&lc_hits, LC_HIT_FMT_ITEM, top + 2, ix + 1, n - m->fmt_item_top < rows ? n - m->fmt_item_top : rows, iw - 2);
    ui_hit_add(&lc_hits, LC_HIT_FMT_CLOSE, top + 1, left + w - 5, 1, 3);
}

static void draw_help(ui_cells *c, const lc_model *m)
{
    int top, left, h = c->rows - 2, w = c->cols - 4, i;
    if (w > 76) w = 76;
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
    lc_modal(c, h, w, LC_ACCENT, "Help", LC_INK, &top, &left);
    for (i = 0; i < h - 2 && m->help_top + i < lc_help_lines; i++) {
        const char *t = lc_help_text[m->help_top + i];
        int head = t[0] == '=' || t[0] == '-';
        ui_cells_pen(c, head ? LC_ACCENT : LC_INK, UI_CELLS_DEFAULT, head);
        ui_cells_text(c, top + 1 + i, left + 2, t, w - 4);
    }
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
}

/** The text overlay (TS overlays/thread-view.ts): a box titled view_title, view_text scrolled from view_top. */
static void draw_view(ui_cells *c, const lc_model *m)
{
    int top, left, h = c->rows - 2, w = c->cols - 4, i;
    if (w > 76) w = 76;
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
    lc_modal(c, h, w, LC_ACCENT, m->view_title, LC_INK, &top, &left);
    for (i = 0; i < h - 2 && m->view_top + i < m->view_n; i++) {
        const char *t = m->view_text[m->view_top + i];
        int head = t[0] == '=';
        ui_cells_pen(c, head ? LC_ACCENT : LC_INK, UI_CELLS_DEFAULT, head);
        ui_cells_text(c, top + 1 + i, left + 2, head ? t + 1 : t, w - 4);
    }
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
}

static void draw_modal(ui_cells *c, const lc_model *m, int *cr, int *cc)
{
    int top, left, w = 50;
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
    if (m->modal == LC_MODAL_PROMPT) {
        lc_modal(c, 7, w, LC_ACCENT, "Message", LC_INK, &top, &left);
        if (w > c->cols) w = c->cols;
        centre(c, top + 2, left + 1, w - 2, m->modal_title);
        ui_cells_text(c, top + 4, left + 3, "> ", 2);
        {                                                    /* a long line scrolls: the tail and the cursor stay in the field */
            int field = w - 9, from = m->prompt_len > field ? m->prompt_len - field : 0;
            ui_cells_text(c, top + 4, left + 5, m->prompt + from, field + 1);
            *cr = top + 4;
            *cc = left + 5 + m->prompt_len - from;
        }
        return;
    }
    if (m->modal == LC_MODAL_RECONNECT) {               /* TS ui/disconnection-modal.ts: Retry in T.ok, Cancel in T.alert */
        char attempt[32];
        lc_modal(c, 12, w, LC_ACCENT, "Connection Error", LC_INK, &top, &left);
        if (w > c->cols) w = c->cols;
        ui_cells_pen(c, LC_ALERT, UI_CELLS_DEFAULT, 0);
        centre(c, top + 2, left + 1, w - 2, "Lost connection to server");
        ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
        centre(c, top + 4, left + 1, w - 2, m->modal_text);
        sprintf(attempt, "Attempt %d/%d", m->modal_attempt, m->modal_max);
        centre(c, top + 6, left + 1, w - 2, attempt);
        ui_cells_button(c, top + 8, left + 12, 9, "[ Retry ]", LC_OK, LC_OK);
        ui_cells_button(c, top + 8, left + 29, 10, "[ Cancel ]", LC_ALERT, LC_ALERT);
        ui_hit_add(&lc_hits, LC_HIT_MODAL, top, left, 12, w);
        ui_hit_add(&lc_hits, LC_HIT_YES, top + 8, left + 12, 3, 9);
        ui_hit_add(&lc_hits, LC_HIT_NO, top + 8, left + 29, 3, 10);
        return;
    }
    lc_modal(c, 12, w, LC_ACCENT, "Message", LC_INK, &top, &left);
    if (w > c->cols) w = c->cols;
    centre(c, top + 3, left + 1, w - 2, m->modal_title);
    centre(c, top + 5, left + 1, w - 2, m->modal_text);
    if (m->modal == LC_MODAL_QUIT) {
        ui_cells_button(c, top + 7, left + 12, 9, "[ Yes ]", LC_ACCENT, LC_INK);
        ui_cells_button(c, top + 7, left + 29, 8, "[ No ]", LC_ACCENT, LC_INK);
        ui_hit_add(&lc_hits, LC_HIT_MODAL, top, left, 12, w);
        ui_hit_add(&lc_hits, LC_HIT_YES, top + 7, left + 12, 3, 9);
        ui_hit_add(&lc_hits, LC_HIT_NO, top + 7, left + 29, 3, 8);
    } else {
        ui_cells_button(c, top + 7, left + (w - 10) / 2, 10, "[ OK ]", LC_ACCENT, LC_INK);
        ui_hit_add(&lc_hits, LC_HIT_MODAL, top, left, 12, w);
        ui_hit_add(&lc_hits, LC_HIT_OK, top + 7, left + (w - 10) / 2, 3, 10);
    }
}

/** The search overlay: TS ui/search-overlay.ts on the SDK SearchModal (search-modal.ts). A 90% box titled
    "Message Search"; at row 1 the 3-row Query field and, from the middle, the "Username (optional)" field (the
    focused one on the blue ground); from row 5 the Results list, its selected row on blue; the help line last. */
static void draw_search(ui_cells *c, const lc_model *m, int *cr, int *cc)
{
    int h = c->rows * 9 / 10, w = c->cols * 9 / 10, top, left, fw, ux, rt, rh, rw, i;
    static const char *const help = "Enter: Search | Tab: Next field | Esc: Close";
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
    lc_modal(c, h, w, LC_ACCENT, "Message Search", LC_INK, &top, &left);
    ui_hit_add(&lc_hits, LC_HIT_SEARCH, top, left, h, w);
    fw = (w - 4) / 2 - 1;
    ux = left + w / 2;
    for (i = 0; i < 2; i++) {
        int x = i == 0 ? left + 2 : ux;
        const char *text = i == 0 ? m->search_q : m->search_user;
        int focus = m->search_field == i;
        ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
        ui_cells_box(c, top + 1, x, 3, fw, LC_INK, i == 0 ? "Query" : "Username (optional)", LC_INK);
        ui_cells_pen(c, LC_INK, focus ? LC_LABEL_BG : UI_CELLS_DEFAULT, 0);
        ui_cells_fill(c, top + 2, x + 1, 1, fw - 2, ' ');
        ui_cells_text(c, top + 2, x + 1, text, fw - 3);
        ui_hit_add(&lc_hits, i == 0 ? LC_HIT_SEARCH_Q : LC_HIT_SEARCH_USER, top + 1, x, 3, fw);
        if (focus) { *cr = top + 2; *cc = x + 1 + (int)strlen(text); }
    }
    rt = top + 5; rh = h - 8; rw = w - 4;
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
    ui_cells_box(c, rt, left + 2, rh, rw, LC_INK, "Results", LC_INK);
    for (i = 0; i < rh - 2 && m->search_top + i < m->search_n; i++) {
        int k = m->search_top + i, sel = k == m->search_sel;
        ui_cells_pen(c, LC_INK, sel ? LC_LABEL_BG : UI_CELLS_DEFAULT, 0);
        if (sel) ui_cells_fill(c, rt + 1 + i, left + 3, 1, rw - 2, ' ');
        ui_cells_text(c, rt + 1 + i, left + 3, m->search_rows[k], rw - 2);
    }
    ui_hit_add(&lc_hits, LC_HIT_SEARCH_ROW, rt + 1, left + 3, rh - 2, rw - 2);
    ui_cells_pen(c, LC_ACCENT, UI_CELLS_DEFAULT, 0);
    ui_cells_text(c, top + h - 2, left + 2, help, w - 4);
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
}

static const char *const set_label[LC_SET_ITEMS] = {
    "Show User Logins/Logouts", "Show File Uploads/Downloads", "Show Door Activity",
    "Show New Messages", "Show System Announcements",
    "Mute Sounds", "Show Typing Indicators", "Show Timestamps",
    "Online", "Away", "Busy", "Do Not Disturb", "Close" };
static int set_item_at[64];                 /* the last frame's interior row -> LC_SET_* (-1 a gap) */

const char *lc_settings_label(int item) { return item >= 0 && item < LC_SET_ITEMS ? set_label[item] : ""; }

/* TS places the rows at fixed offsets (events at 2, prefs two apart, the radio under "Status:", Close
   at bottom 1) that need 24 interior rows, more than its own 22-row ceiling holds, so its radio and
   Close overlap. Here the same rows in the same order keep the TS gaps while they fit and give them
   up, last-listed first, when they do not: 16 interior rows hold every item. */
void lc_settings_layout(int cols, int rows, int *h, int *w, int row[LC_SET_ITEMS], int *head, int *rule, int *status)
{
    int ih, extra, r, i;
    int pad_bottom, pad_top, gap_ev, gap_pref, gap_rule, gap_radio;
    *w = cols - 4 < 20 ? 20 : cols - 4; if (*w > 72) *w = 72; if (*w > cols) *w = cols;
    *h = rows - 4 < 18 ? 18 : rows - 4; if (*h > 22) *h = 22; if (*h > rows) *h = rows;
    ih = *h - 2;
    extra = ih - 16; if (extra < 0) extra = 0;
#define TAKE(v, n) do { v = extra < (n) ? extra : (n); extra -= v; } while (0)
    TAKE(pad_bottom, 1); TAKE(gap_radio, 1); TAKE(gap_ev, 1); TAKE(gap_rule, 1); TAKE(pad_top, 2); TAKE(gap_pref, 2);
#undef TAKE
    r = pad_top;
    *head = r++;
    for (i = LC_SET_LOGINS; i <= LC_SET_ANNOUNCE; i++) row[i] = r++;
    r += gap_ev;
    row[LC_SET_MUTE] = r++; if (gap_pref > 0) r++;
    row[LC_SET_TYPING] = r++; if (gap_pref > 1) r++;
    row[LC_SET_STAMPS] = r++;
    r += gap_rule;
    *rule = r++;
    *status = r++;
    r += gap_radio;
    for (i = LC_SET_ONLINE; i <= LC_SET_DND; i++) row[i] = r++;
    row[LC_SET_CLOSE] = ih - 1 - pad_bottom;
}

static void draw_settings(ui_cells *c, const lc_model *m)
{
    int h, w, top, left, row[LC_SET_ITEMS], head, rule, status, i, l;
    char t[80];
    lc_settings_layout(c->cols, c->rows, &h, &w, row, &head, &rule, &status);
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
    lc_modal(c, h, w, LC_ACCENT, "Settings", LC_INK, &top, &left);
    ui_hit_add(&lc_hits, LC_HIT_SETTINGS, top, left, h, w);
    for (i = 0; i < 64; i++) set_item_at[i] = -1;
    top++; left++;                                  /* the interior: TS top/left are inside the border */
    l = left + 2;
    ui_cells_pen(c, LC_ACCENT, UI_CELLS_DEFAULT, 0);
    ui_cells_text(c, top + head, l, "BBS Events:", w - 6);
    for (i = 0; i < LC_SET_ITEMS; i++) {
        int x = i <= LC_SET_ANNOUNCE ? l + 2 : l, sel = m->settings_sel == i;
        if (i == LC_SET_CLOSE) {                    /* blessed.button bottom 1, centre, width 12, bg T.bar, focus bg accent */
            int bx = left + (w - 2 - 12) / 2;
            ui_cells_pen(c, LC_INK, sel ? LC_ACCENT : lc_tone(LC_EV_BAR), 0);
            ui_cells_fill(c, top + row[i], bx, 1, 12, ' ');
            ui_cells_text(c, top + row[i], bx, set_label[i], 12);
        } else {
            if (i <= LC_SET_STAMPS) sprintf(t, "[%c] %s", m->settings_val[i] ? 'X' : ' ', set_label[i]);
            else sprintf(t, "(%c) %s", m->settings_status == i - LC_SET_ONLINE ? 'O' : ' ', set_label[i]);
            ui_cells_pen(c, LC_INK, sel ? LC_LABEL_BG : UI_CELLS_DEFAULT, 0);
            ui_cells_text(c, top + row[i], x, t, w - 2 - (x - left));
        }
        if (row[i] >= 0 && row[i] < 64) set_item_at[row[i]] = i;
    }
    ui_cells_pen(c, LC_DIM, UI_CELLS_DEFAULT, 0);
    ui_cells_fill(c, top + rule, l, 1, w - 6, '-');
    ui_cells_pen(c, LC_ACCENT, UI_CELLS_DEFAULT, 0);
    ui_cells_text(c, top + status, l, "Status:", 20);
    ui_hit_add(&lc_hits, LC_HIT_SETTINGS_ROW, top, left, h - 2, w - 2);
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
}

static int theme_first;                         /* the first theme row shown (the list scrolls to keep the highlight in view) */

/* The name column: the longest theme name plus one space (TS theme-menu.ts computes the same from the table). */
static int theme_name_col(void)
{
    int n = ui_theme_count(), i, best = 0;
    for (i = 0; i < n; i++) {
        int l = (int)strlen(ui_theme_at(i)->name);
        if (l > best) best = l;
    }
    return best + 1;
}

/* TS overlays/theme-menu.ts: a 54-wide panel labelled " Theme ", one row per theme ` name.padEnd(col) blurb`, the
 * highlight in selection colours, and the help line on the interior's last row in the dim colour. */
static void draw_theme(ui_cells *c, const lc_model *m)
{
    int n = ui_theme_count(), h = n + 6 < 20 ? n + 6 : 20, w = 54, top, left, vis, i;
    char t[160];
    int namew = theme_name_col();
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
    lc_modal(c, h, w, LC_CHROME, " Theme ", LC_INK, &top, &left);
    ui_hit_add(&lc_hits, LC_HIT_THEME, top, left, h, w);
    top++; left++;
    vis = n < h - 3 ? n : h - 3;
    theme_first = m->theme_sel >= vis ? m->theme_sel - vis + 1 : 0;
    for (i = 0; i < vis; i++) {
        const ui_theme *th = ui_theme_at(theme_first + i);
        int sel = theme_first + i == m->theme_sel;
        sprintf(t, " %-*s%s", namew, th->name, th->blurb);   /* ui_cells_text cuts the blurb at the row end */
        ui_cells_pen(c, sel ? LC_SEL_INK : LC_INK, sel ? LC_SEL_BG : UI_CELLS_DEFAULT, 0);
        ui_cells_fill(c, top + i, left, 1, w - 2, ' ');
        ui_cells_text(c, top + i, left, t, w - 2);
    }
    ui_hit_add(&lc_hits, LC_HIT_THEME_ROW, top, left, vis, w - 2);
    ui_cells_pen(c, LC_DIM, UI_CELLS_DEFAULT, 0);
    ui_cells_text(c, top + h - 3, left + 1, "UP/DOWN previews.  ENTER keeps it.  ESC puts it back.", w - 4);
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
}

/* ---- the /draw pen picker ---- */

static int lcpk_cols(int page) { return page == LC_PICK_CHAR ? 16 : 8; }
static int lcpk_count(int page) { return page == LC_PICK_CHAR ? LC_PICK_CHARS : 16; }
static int lcpk_cell_w(int page) { return page == LC_PICK_CHAR ? 1 : 2; }
static int lcpk_grid_rows(int page) { return page == LC_PICK_CHAR ? 12 : 2; }

int lc_pick_char(int i)
{
    return i < 95 ? 0x20 + i : 0xA0 + (i - 95);
}

void lc_pick_open(lc_model *m)
{
    int i;
    lc_pick *p = &m->dr_pick;
    p->open = 1;
    p->page = LC_PICK_CHAR;
    p->cur[LC_PICK_FORE] = m->dr_fg & 15;
    p->cur[LC_PICK_BACK] = m->dr_bg & 15;
    p->cur[LC_PICK_CHAR] = 0;
    for (i = 0; i < LC_PICK_CHARS; i++) if (lc_pick_char(i) == m->dr_ch) p->cur[LC_PICK_CHAR] = i;
    p->top = m->rows > 19 ? 1 : 0;
    p->left = m->cols > LC_PICK_W ? (m->cols - LC_PICK_W) / 2 : 0;
}

static void pick_apply(lc_model *m)
{
    lc_pick *p = &m->dr_pick;
    if (p->page == LC_PICK_FORE) m->dr_fg = p->cur[LC_PICK_FORE];
    else if (p->page == LC_PICK_BACK) m->dr_bg = p->cur[LC_PICK_BACK];
    else m->dr_ch = lc_pick_char(p->cur[LC_PICK_CHAR]);
}

int lc_pick_key(lc_model *m, int key)
{
    lc_pick *p = &m->dr_pick;
    int *cur = &p->cur[p->page], cols = lcpk_cols(p->page), n = lcpk_count(p->page), t = *cur;
    if (key == UI_KEY_ESC || key == 'h') { p->open = 0; return 0; }
    if (key == UI_KEY_ENTER || key == '\r' || key == '\n' || key == ' ') { pick_apply(m); return 1; }
    if (key == '\t') { p->page = (p->page + 1) % 3; return 0; }
    if (key == UI_KEY_BACKTAB) { p->page = (p->page + 2) % 3; return 0; }
    if (key == UI_KEY_LEFT) t--;
    else if (key == UI_KEY_RIGHT) t++;
    else if (key == UI_KEY_UP) t -= cols;
    else if (key == UI_KEY_DOWN) t += cols;
    if (t >= 0 && t < n) *cur = t;
    return 0;
}

int lc_pick_click(lc_model *m, int row, int col)
{
    lc_pick *p = &m->dr_pick;
    int r = row - p->top - 2, c = col - p->left - 1, cw, i;
    if (row < p->top || row >= p->top + 3 + lcpk_grid_rows(p->page) || col < p->left || col >= p->left + LC_PICK_W) { p->open = 0; return 0; }
    if (row == p->top + 1) {
        if (c >= 0 && c < 4) p->page = LC_PICK_FORE;
        else if (c >= 5 && c < 9) p->page = LC_PICK_BACK;
        else if (c >= 10 && c < 14) p->page = LC_PICK_CHAR;
        return 0;
    }
    cw = lcpk_cell_w(p->page);
    if (r < 0 || r >= lcpk_grid_rows(p->page) || c < 0 || c >= lcpk_cols(p->page) * cw) return 0;
    i = r * lcpk_cols(p->page) + c / cw;
    if (i >= lcpk_count(p->page)) return 0;
    p->cur[p->page] = i;
    pick_apply(m);
    return 1;
}

static void pick_draw(ui_cells *c, const lc_model *m)
{
    const lc_pick *p = &m->dr_pick;
    static const char *const tab[3] = { "Fore", "Back", "Char" };
    int h = 3 + lcpk_grid_rows(p->page), cw = lcpk_cell_w(p->page), cols = lcpk_cols(p->page), i, k;
    ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
    ui_cells_fill(c, p->top + 1, p->left + 1, h - 2, LC_PICK_W - 2, ' ');
    ui_cells_box(c, p->top, p->left, h, LC_PICK_W, LC_ACCENT, 0, 0);
    cover(p->top, p->left, h, LC_PICK_W);
    for (k = 0; k < 3; k++) {
        ui_cells_pen(c, k == p->page ? LC_INK : LC_ACCENT, k == p->page ? LC_ACCENT : UI_CELLS_DEFAULT, 0);
        ui_cells_text(c, p->top + 1, p->left + 1 + k * 5, tab[k], 4);
    }
    for (i = 0; i < lcpk_count(p->page); i++) {
        int row = p->top + 2 + i / cols, col = p->left + 1 + (i % cols) * cw, sel = i == p->cur[p->page];
        if (p->page == LC_PICK_CHAR) {
            ui_cells_pen(c, sel ? 0 : LC_INK, sel ? LC_ACCENT : UI_CELLS_DEFAULT, 0);
            ui_cells_put(c, row, col, lc_pick_char(i));
        } else {
            ui_cells_pen(c, i < 9 ? 15 : 0, i == 0 ? UI_CELLS_DEFAULT : i, 0);
            ui_cells_put(c, row, col, sel ? '>' : ' ');
            ui_cells_put(c, row, col + 1, "0123456789ABCDEF"[i]);
        }
    }
    ui_cells_pen(c, UI_CELLS_DEFAULT, UI_CELLS_DEFAULT, 0);
}

/** The /draw view (plan D1): canvas rows 0..23 from the model's scroll offset, the status bar below.
 *  A caller narrower than 80 columns sees a window the cursor drags along. The cursor goes to the
 *  terminal caret. */
static void lc_draw_canvas(ui_cells *c, const lc_model *m, int *cursor_row, int *cursor_col)
{
    int W = c->cols, H = c->rows, vw = W < UI_CANVAS_COLS ? W : UI_CANVAS_COLS, vh = H - 1 < UI_CANVAS_ROWS ? H - 1 : UI_CANVAS_ROWS;
    int r, k;
    char status[160];

    ui_hit_reset(&lc_hits);
    lc_cover_n = 0;
    ui_cells_pen(c, UI_CELLS_DEFAULT, UI_CELLS_DEFAULT, 0);
    ui_cells_fill(c, 0, 0, H, W, ' ');
    for (r = 0; r < vh; r++) {
        for (k = 0; k < vw; k++) {
            ui_cell cell;
            ui_canvas_get(m->canvas, m->dr_left + k, m->dr_top + r, &cell);
            ui_cells_pen(c, cell.fg, cell.bg == 0 ? UI_CELLS_DEFAULT : cell.bg, 0);
            ui_cells_put(c, r, k, (unsigned char)cell.ch < 32 ? ' ' : (unsigned char)cell.ch);
        }
    }
    ui_hit_add(&lc_hits, LC_HIT_CANVAS, 0, 0, vh, vw);
    sprintf(status, "%.12s | pen %c fg %d bg %d | %d,%d | %.30s", m->dr_tool, m->dr_ch < 32 ? ' ' : m->dr_ch, m->dr_fg, m->dr_bg,
            m->dr_x, m->dr_y, m->dr_note[0] != '\0' ? m->dr_note : "Space paint, T tools, H pen, Esc back");
    ui_cells_bar(c, H - 1, 0, W, status, LC_INK, UI_CELLS_DEFAULT, 0);
    if (m->dr_pop.open) {
        ui_popup_draw(c, &m->dr_pop, LC_ACCENT, LC_INK, LC_ACCENT, LC_ALERT);
        ui_hit_add(&lc_hits, LC_HIT_POPUP, m->dr_pop.top, m->dr_pop.left, m->dr_pop.n + 2, m->dr_pop.w);
    }
    if (m->dr_pick.open) {
        pick_draw(c, m);
        ui_hit_add(&lc_hits, LC_HIT_POPUP, m->dr_pick.top, m->dr_pick.left, 3 + lcpk_grid_rows(m->dr_pick.page), LC_PICK_W);
    }
    *cursor_row = m->dr_y - m->dr_top;
    *cursor_col = m->dr_x - m->dr_left;
}

void lc_draw(ui_cells *c, const lc_model *m, int *cursor_row, int *cursor_col)
{
    if (m->draw && m->canvas != 0 && m->modal == LC_MODAL_NONE) { lc_draw_canvas(c, m, cursor_row, cursor_col); return; }
    int W = c->cols, H = c->rows, sw = lc_sidebar_width(m), h = H - 5;
    char status[160];
    int k;

    ui_hit_reset(&lc_hits);
    lc_cover_n = 0;
    bar_rows = 0;                                   /* no scrollbar until draw_chat draws one */
    draw_menubar(c);
    for (k = 0; k < LC_MENUS; k++)
        ui_hit_add(&lc_hits, LC_HIT_MENU, 0, menu_col[k], 1, (int)strlen(lc_menu_label[k]));
    ui_hit_add(&lc_hits, LC_HIT_STATUS, H - 1, 0, 1, W);
    ui_hit_add(&lc_hits, LC_HIT_INPUT, H - 4, 0, 3, W);
    if (sw > 0) {
        draw_sidebar(c, m, 1, h, sw);
        ui_hit_add(&lc_hits, LC_HIT_SIDEBAR, 2, 1, h - 2, sw - 2);
    }
    draw_chat(c, m, 1, h, sw, W - sw);
    draw_input(c, m, cursor_row, cursor_col);
    sprintf(status, "@%.30s | Node %d | #%.40s | [L] [*] | F1:Help F4:Emoji", m->handle, m->node, m->room);
    ui_cells_bar(c, H - 1, 0, W, status, LC_INK, UI_CELLS_DEFAULT, 0);

    if (m->menu >= 0 && m->menu < LC_MENUS) {
        ui_cells_pen(c, LC_INK, UI_CELLS_DEFAULT, 0);
        int mw = ui_cells_menu(c, 1, menu_col[m->menu] - 1, lc_menu_items[m->menu], lc_menu_count[m->menu],
                               m->menu_sel, LC_ACCENT, LC_INK, LC_ACCENT);
        ui_hit_add(&lc_hits, LC_HIT_DROPDOWN, 2, menu_col[m->menu], lc_menu_count[m->menu], mw - 2);
        cover(1, menu_col[m->menu] - 1, lc_menu_count[m->menu] + 2, mw);
    }
    draw_slash(c, m);
    if (m->ctx.open) {
        ui_popup_draw(c, &m->ctx, LC_ACCENT, LC_INK, LC_ACCENT, LC_ALERT);
        ui_hit_add(&lc_hits, LC_HIT_POPUP, m->ctx.top, m->ctx.left, m->ctx.n + 2, m->ctx.w);
        cover(m->ctx.top, m->ctx.left, m->ctx.n + 2, m->ctx.w);
    }
    if (m->fmt) draw_fmt(c, m);
    if (m->help) draw_help(c, m);
    if (m->view) draw_view(c, m);
    if (m->search) draw_search(c, m, cursor_row, cursor_col);
    if (m->settings) draw_settings(c, m);
    if (m->theme) draw_theme(c, m);
    if (m->modal != LC_MODAL_NONE) draw_modal(c, m, cursor_row, cursor_col);
}

int lc_scroll_at(int row)
{
    int pos, track;
    if (bar_rows <= 0) return -1;
    track = bar_rows - bar_thumb;
    pos = row - bar_body - bar_thumb / 2;
    if (pos > track) pos = track;
    if (pos < 0) pos = 0;
    /* draw_chat's pos = first*track/span (floor), inverted: first is rounded up so the next draw puts the
     * thumb on the row it was dragged to, not one row above it. */
    return track > 0 ? bar_span - (pos * bar_span + track - 1) / track : 0;
}

/** How far the last drawn chat pane can scroll back, in rows; 0 when it had no scrollbar. */
int lc_scroll_max(void) { return bar_rows > 0 ? bar_span : 0; }

int lc_hit(int row, int col, int *index)
{
    int dr = 0, dc = 0, id = ui_hit_at(&lc_hits, row, col, &dr, &dc), k;
    *index = -1;
    switch (id) {
    case LC_HIT_MENU:
        for (k = 0; k < LC_MENUS; k++)
            if (col >= menu_col[k] && col < menu_col[k] + (int)strlen(lc_menu_label[k])) *index = k;
        break;
    case LC_HIT_DROPDOWN: case LC_HIT_SIDEBAR: case LC_HIT_FMT_CAT: case LC_HIT_FMT_ITEM: case LC_HIT_SEARCH_ROW: *index = dr; break;
    case LC_HIT_THEME_ROW: *index = theme_first + dr; break;
    case LC_HIT_SETTINGS_ROW: *index = dr >= 0 && dr < 64 ? set_item_at[dr] : -1; break;
    case LC_HIT_CHAT: *index = dr < CHAT_ROWS_MAX ? chat_row_line[dr] : -1; break;
    case LC_HIT_STATUS: *index = col; break;
    case -1: return LC_HIT_NONE;
    default: break;
    }
    return id;
}
