#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "keymap.h"

typedef struct { const char *name; uint8_t usage; uint8_t mod; } key_t_;

static const key_t_ NAMED[] = {
    {"CTRL", 0, MOD_LCTRL}, {"LCTRL", 0, MOD_LCTRL}, {"RCTRL", 0, MOD_RCTRL},
    {"SHIFT", 0, MOD_LSHIFT}, {"LSHIFT", 0, MOD_LSHIFT}, {"RSHIFT", 0, MOD_RSHIFT},
    {"ALT", 0, MOD_LALT}, {"LALT", 0, MOD_LALT}, {"RALT", 0, MOD_RALT},
    {"GUI", 0, MOD_LGUI}, {"WIN", 0, MOD_LGUI}, {"CMD", 0, MOD_LGUI},
    {"LGUI", 0, MOD_LGUI}, {"RGUI", 0, MOD_RGUI},
    {"ENTER", 0x28, 0}, {"RETURN", 0x28, 0}, {"ESC", 0x29, 0}, {"ESCAPE", 0x29, 0},
    {"BACKSPACE", 0x2A, 0}, {"TAB", 0x2B, 0}, {"SPACE", 0x2C, 0},
    {"MINUS", 0x2D, 0}, {"EQUAL", 0x2E, 0}, {"LBRACKET", 0x2F, 0}, {"RBRACKET", 0x30, 0},
    {"BACKSLASH", 0x31, 0}, {"SEMICOLON", 0x33, 0}, {"QUOTE", 0x34, 0},
    {"GRAVE", 0x35, 0}, {"COMMA", 0x36, 0}, {"DOT", 0x37, 0}, {"SLASH", 0x38, 0},
    {"CAPSLOCK", 0x39, 0},
    {"PRTSC", 0x46, 0}, {"PRINTSCREEN", 0x46, 0}, {"SCROLLLOCK", 0x47, 0}, {"PAUSE", 0x48, 0},
    {"INSERT", 0x49, 0}, {"HOME", 0x4A, 0}, {"PGUP", 0x4B, 0}, {"PAGEUP", 0x4B, 0},
    {"DELETE", 0x4C, 0}, {"DEL", 0x4C, 0}, {"END", 0x4D, 0}, {"PGDN", 0x4E, 0}, {"PAGEDOWN", 0x4E, 0},
    {"RIGHT", 0x4F, 0}, {"LEFT", 0x50, 0}, {"DOWN", 0x51, 0}, {"UP", 0x52, 0},
    {"NUMLOCK", 0x53, 0}, {"MENU", 0x65, 0}, {"APP", 0x65, 0},
};

bool keymap_lookup(const char *name, uint8_t *usage, uint8_t *mod)
{
    *usage = 0; *mod = 0;
    if (!name || !*name) return false;
    size_t n = strlen(name);
    if (n == 1) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c >= 'A' && c <= 'Z') { *usage = 0x04 + (c - 'A'); return true; }
        if (c >= '1' && c <= '9') { *usage = 0x1E + (c - '1'); return true; }
        if (c == '0') { *usage = 0x27; return true; }
    }
    if ((name[0] == 'F' || name[0] == 'f') && n <= 3 && name[1] >= '1' && name[1] <= '9') {
        int k = atoi(name + 1);
        if (k >= 1 && k <= 12) { *usage = 0x3A + (k - 1); return true; }
        if (k >= 13 && k <= 24) { *usage = 0x68 + (k - 13); return true; }
    }
    if (n > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        char *end;
        unsigned long v = strtoul(name, &end, 16);
        if (*end || v > 0xFF) return false;
        if (v >= 0xE0 && v <= 0xE7) *mod = 1u << (v - 0xE0);
        else *usage = (uint8_t)v;
        return true;
    }
    for (size_t i = 0; i < sizeof(NAMED) / sizeof(NAMED[0]); i++) {
        if (strcasecmp(NAMED[i].name, name) == 0) {
            *usage = NAMED[i].usage; *mod = NAMED[i].mod;
            return true;
        }
    }
    return false;
}

/* US layout for punctuation: unshifted, shifted */
static const struct { char plain, shifted; uint8_t usage; } PUNCT[] = {
    {'1', '!', 0x1E}, {'2', '@', 0x1F}, {'3', '#', 0x20}, {'4', '$', 0x21},
    {'5', '%', 0x22}, {'6', '^', 0x23}, {'7', '&', 0x24}, {'8', '*', 0x25},
    {'9', '(', 0x26}, {'0', ')', 0x27},
    {'-', '_', 0x2D}, {'=', '+', 0x2E}, {'[', '{', 0x2F}, {']', '}', 0x30},
    {'\\', '|', 0x31}, {';', ':', 0x33}, {'\'', '"', 0x34}, {'`', '~', 0x35},
    {',', '<', 0x36}, {'.', '>', 0x37}, {'/', '?', 0x38},
};

bool keymap_ascii(char c, uint8_t *usage, bool *shift)
{
    *shift = false;
    if (c >= 'a' && c <= 'z') { *usage = 0x04 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { *usage = 0x04 + (c - 'A'); *shift = true; return true; }
    switch (c) {
    case ' ':  *usage = 0x2C; return true;
    case '\n': *usage = 0x28; return true;
    case '\t': *usage = 0x2B; return true;
    case '\b': *usage = 0x2A; return true;
    }
    for (size_t i = 0; i < sizeof(PUNCT) / sizeof(PUNCT[0]); i++) {
        if (PUNCT[i].plain == c)   { *usage = PUNCT[i].usage; return true; }
        if (PUNCT[i].shifted == c) { *usage = PUNCT[i].usage; *shift = true; return true; }
    }
    return false;
}

uint16_t keymap_media(const char *name)
{
    static const struct { const char *n; uint16_t u; } M[] = {
        {"VOLUME_UP", 0xE9}, {"VOLUME_DOWN", 0xEA}, {"MUTE", 0xE2},
        {"PLAY_PAUSE", 0xCD}, {"NEXT", 0xB5}, {"PREV", 0xB6}, {"STOP", 0xB7},
        {"BRIGHTNESS_UP", 0x6F}, {"BRIGHTNESS_DOWN", 0x70},
        {"HOME", 0x223}, {"BACK", 0x224}, {"SEARCH", 0x221},
        {"CALCULATOR", 0x192}, {"MAIL", 0x18A}, {"BROWSER", 0x196},
    };
    if (!name) return 0;
    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) return (uint16_t)strtoul(name, NULL, 16);
    for (size_t i = 0; i < sizeof(M) / sizeof(M[0]); i++)
        if (strcasecmp(M[i].n, name) == 0) return M[i].u;
    return 0;
}
