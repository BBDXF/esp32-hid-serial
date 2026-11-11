/* hid.* method implementations. Runs only on the HID worker task. */
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "hid_actions.h"
#include "hid_dev.h"
#include "keymap.h"
#include "config.h"

#define MAX_HOLD_MS 5000
#define MAX_TEXT    1024
#define MAX_STEPS   64

static uint8_t s_mod, s_keys[6];
static uint8_t s_btn;          /* relative mouse buttons */
static uint8_t s_abs_btn;
static uint16_t s_abs_x = 16384, s_abs_y = 16384;
static int64_t s_held_since;   /* 0 = nothing held */

static void sleep_ms(int ms) { if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms)); }

static int clampi(double v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : (int)v; }

static double num(const cJSON *p, const char *k, double def)
{
    const cJSON *v = p ? cJSON_GetObjectItem(p, k) : NULL;
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static const char *str(const cJSON *p, const char *k)
{
    const cJSON *v = p ? cJSON_GetObjectItem(p, k) : NULL;
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void mark_held(void)
{
    bool any = s_mod || s_btn || s_abs_btn;
    for (int i = 0; i < 6; i++) any |= s_keys[i] != 0;
    if (!any) s_held_since = 0;
    else if (!s_held_since) s_held_since = esp_timer_get_time();
}

static esp_err_t kbd_send(void)
{
    uint8_t r[8] = { s_mod, 0 };
    memcpy(r + 2, s_keys, 6);
    mark_held();
    return hid_send(RID_KEYBOARD, r, sizeof r);
}

static esp_err_t mouse_send(int dx, int dy, int wheel, int pan)
{
    uint8_t r[7] = { s_btn, dx & 0xFF, (dx >> 8) & 0xFF, dy & 0xFF, (dy >> 8) & 0xFF,
                     (uint8_t)(int8_t)wheel, (uint8_t)(int8_t)pan };
    mark_held();
    return hid_send(RID_MOUSE, r, sizeof r);
}

static esp_err_t abs_send(void)
{
    uint8_t r[5] = { s_abs_btn, s_abs_x & 0xFF, s_abs_x >> 8, s_abs_y & 0xFF, s_abs_y >> 8 };
    mark_held();
    return hid_send(RID_ABS, r, sizeof r);
}

static void key_add(uint8_t u)
{
    for (int i = 0; i < 6; i++) if (s_keys[i] == u) return;
    for (int i = 0; i < 6; i++) if (!s_keys[i]) { s_keys[i] = u; return; }
}

static void key_del(uint8_t u)
{
    for (int i = 0; i < 6; i++) if (s_keys[i] == u) s_keys[i] = 0;
}

/* parse keys array into mod mask + usages */
static bool parse_keys(const cJSON *p, uint8_t *mod, uint8_t *u, int *n, rpc_err_t *err)
{
    const cJSON *arr = p ? cJSON_GetObjectItem(p, "keys") : NULL;
    *mod = 0; *n = 0;
    if (!cJSON_IsArray(arr)) { rpc_set_err(err, E_INVALID_PARAMS, "keys: array required"); return false; }
    const cJSON *k;
    cJSON_ArrayForEach(k, arr) {
        uint8_t usage, m;
        if (!cJSON_IsString(k) || !keymap_lookup(k->valuestring, &usage, &m)) {
            rpc_set_err(err, E_INVALID_PARAMS, "invalid key: %s", cJSON_IsString(k) ? k->valuestring : "?");
            return false;
        }
        *mod |= m;
        if (usage) {
            if (*n >= 6) { rpc_set_err(err, E_INVALID_PARAMS, "max 6 non-modifier keys"); return false; }
            u[(*n)++] = usage;
        }
    }
    return true;
}

static uint8_t button_mask(const char *b)
{
    if (!b || !strcasecmp(b, "left")) return 1;
    if (!strcasecmp(b, "right")) return 2;
    if (!strcasecmp(b, "middle")) return 4;
    if (!strcasecmp(b, "back")) return 8;
    if (!strcasecmp(b, "forward")) return 16;
    return 0;
}

static cJSON *ok(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return o;
}

#define REQUIRE(cond, msg) do { if (!(cond)) { rpc_set_err(err, E_INVALID_PARAMS, msg); return NULL; } } while (0)
#define SEND(expr) do { if ((expr) != ESP_OK) { rpc_set_err(err, E_BLE_NOT_CONN, "send failed (not connected?)"); return NULL; } } while (0)

static cJSON *do_key(const cJSON *p, rpc_err_t *err, int mode /*0 tap,1 down,2 up*/)
{
    uint8_t mod, u[6];
    int n;
    if (mode == 2) {
        const cJSON *arr = p ? cJSON_GetObjectItem(p, "keys") : NULL;
        if (!arr || cJSON_GetArraySize(arr) == 0) { hid_actions_release_all(); return ok(); }
    }
    if (!parse_keys(p, &mod, u, &n, err)) return NULL;
    if (mode == 2) {
        s_mod &= ~mod;
        for (int i = 0; i < n; i++) key_del(u[i]);
        SEND(kbd_send());
        return ok();
    }
    s_mod |= mod;
    SEND(kbd_send());                         /* modifiers first, some hosts need it */
    for (int i = 0; i < n; i++) key_add(u[i]);
    SEND(kbd_send());
    if (mode == 1) return ok();
    sleep_ms(clampi(num(p, "hold_ms", 30), 0, MAX_HOLD_MS));
    for (int i = 0; i < n; i++) key_del(u[i]);
    SEND(kbd_send());
    s_mod &= ~mod;
    SEND(kbd_send());
    return ok();
}

static cJSON *do_type(const cJSON *p, rpc_err_t *err)
{
    const char *t = str(p, "text");
    REQUIRE(t, "text required");
    REQUIRE(strlen(t) <= MAX_TEXT, "text too long (max 1024)");
    int iv = clampi(num(p, "interval_ms", g_cfg.hid_interval_ms), 2, 1000);
    int typed = 0, skipped = 0;
    uint8_t saved_mod = s_mod;
    for (const char *c = t; *c; c++) {
        uint8_t u; bool sh;
        if ((unsigned char)*c >= 0x80 || !keymap_ascii(*c, &u, &sh)) { skipped++; continue; }
        s_mod = saved_mod | (sh ? MOD_LSHIFT : 0);
        memset(s_keys, 0, 6);
        s_keys[0] = u;
        SEND(kbd_send());
        sleep_ms(iv);
        s_keys[0] = 0;
        s_mod = saved_mod;
        SEND(kbd_send());
        sleep_ms(iv);
        typed++;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "typed", typed);
    cJSON_AddNumberToObject(o, "skipped", skipped);   /* non-ASCII etc. */
    return o;
}

static cJSON *do_move(const cJSON *p, rpc_err_t *err)
{
    int dx = clampi(num(p, "dx", 0), -100000, 100000);
    int dy = clampi(num(p, "dy", 0), -100000, 100000);
    while (dx || dy) {
        int sx = clampi(dx, -32767, 32767), sy = clampi(dy, -32767, 32767);
        SEND(mouse_send(sx, sy, 0, 0));
        dx -= sx; dy -= sy;
        if (dx || dy) sleep_ms(8);
    }
    return ok();
}

static bool set_abs(const cJSON *p, rpc_err_t *err)
{
    const cJSON *x = cJSON_GetObjectItem(p, "x"), *y = cJSON_GetObjectItem(p, "y");
    if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y) || x->valuedouble < 0 || x->valuedouble > 1 ||
        y->valuedouble < 0 || y->valuedouble > 1) {
        rpc_set_err(err, E_INVALID_PARAMS, "x,y must be numbers in 0.0..1.0");
        return false;
    }
    if (!g_cfg.hid_abs) { rpc_set_err(err, E_INVALID_PARAMS, "abs_pointer profile disabled"); return false; }
    s_abs_x = (uint16_t)(x->valuedouble * 32767);
    s_abs_y = (uint16_t)(y->valuedouble * 32767);
    return true;
}

static cJSON *do_abs(const cJSON *p, rpc_err_t *err)
{
    if (!set_abs(p, err)) return NULL;
    SEND(abs_send());
    return ok();
}

static cJSON *do_click(const cJSON *p, rpc_err_t *err)
{
    uint8_t m = button_mask(str(p, "button"));
    REQUIRE(m, "button: left|right|middle|back|forward");
    int count = clampi(num(p, "count", 1), 1, 5);
    bool use_abs = p && cJSON_GetObjectItem(p, "x");
    if (use_abs) {
        if (!set_abs(p, err)) return NULL;
        SEND(abs_send());
        sleep_ms(20);
    }
    for (int i = 0; i < count; i++) {
        if (use_abs) { s_abs_btn |= m; SEND(abs_send()); } else { s_btn |= m; SEND(mouse_send(0, 0, 0, 0)); }
        sleep_ms(30);
        if (use_abs) { s_abs_btn &= ~m; SEND(abs_send()); } else { s_btn &= ~m; SEND(mouse_send(0, 0, 0, 0)); }
        if (i + 1 < count) sleep_ms(60);
    }
    return ok();
}

static cJSON *do_button(const cJSON *p, rpc_err_t *err)
{
    uint8_t m = button_mask(str(p, "button"));
    const char *st = str(p, "state");
    REQUIRE(m && st, "button and state(down|up) required");
    if (!strcmp(st, "down")) s_btn |= m;
    else if (!strcmp(st, "up")) s_btn &= ~m;
    else REQUIRE(false, "state: down|up");
    SEND(mouse_send(0, 0, 0, 0));
    return ok();
}

static cJSON *do_scroll(const cJSON *p, rpc_err_t *err)
{
    int v = clampi(num(p, "v", 0), -1000, 1000), h = clampi(num(p, "h", 0), -1000, 1000);
    while (v || h) {
        int sv = clampi(v, -127, 127), sh = clampi(h, -127, 127);
        SEND(mouse_send(0, 0, sv, sh));
        v -= sv; h -= sh;
        if (v || h) sleep_ms(10);
    }
    return ok();
}

static cJSON *do_media(const cJSON *p, rpc_err_t *err)
{
    uint16_t u = keymap_media(str(p, "key"));
    REQUIRE(u, "unknown media key");
    uint8_t r[2] = { u & 0xFF, u >> 8 };
    SEND(hid_send(RID_MEDIA, r, 2));
    sleep_ms(clampi(num(p, "hold_ms", 30), 0, MAX_HOLD_MS));
    r[0] = r[1] = 0;
    SEND(hid_send(RID_MEDIA, r, 2));
    return ok();
}

static int gp_button(const char *n)
{
    static const char *names[] = {"A", "B", "X", "Y", "LB", "RB", "BACK", "START", "LS", "RS", "HOME"};
    for (int i = 0; i < 11; i++) if (!strcasecmp(n, names[i])) return i;
    if ((n[0] == 'B' || n[0] == 'b') && n[1]) { int k = atoi(n + 1); if (k >= 1 && k <= 16) return k - 1; }
    return -1;
}

static int gp_hat(const char *h)
{
    static const char *names[] = {"up", "up_right", "right", "down_right", "down", "down_left", "left", "up_left"};
    if (!h) return 8;
    for (int i = 0; i < 8; i++) if (!strcasecmp(h, names[i])) return i;
    return 8;  /* center = null state */
}

static cJSON *do_gamepad(const cJSON *p, rpc_err_t *err)
{
    REQUIRE(g_cfg.hid_gamepad, "gamepad profile disabled (ble.set_profile)");
    uint16_t b = 0;
    const cJSON *arr = p ? cJSON_GetObjectItem(p, "buttons") : NULL, *it;
    cJSON_ArrayForEach(it, arr) {
        int i = cJSON_IsString(it) ? gp_button(it->valuestring) : -1;
        REQUIRE(i >= 0, "invalid gamepad button");
        b |= 1u << i;
    }
    uint8_t r[9] = {
        b & 0xFF, b >> 8, (uint8_t)gp_hat(str(p, "hat")),
        (uint8_t)(int8_t)clampi(num(p, "lx", 0), -127, 127), (uint8_t)(int8_t)clampi(num(p, "ly", 0), -127, 127),
        (uint8_t)(int8_t)clampi(num(p, "rx", 0), -127, 127), (uint8_t)(int8_t)clampi(num(p, "ry", 0), -127, 127),
        (uint8_t)clampi(num(p, "lt", 0), 0, 255), (uint8_t)clampi(num(p, "rt", 0), 0, 255),
    };
    SEND(hid_send(RID_GAMEPAD, r, sizeof r));
    sleep_ms(clampi(num(p, "hold_ms", 50), 0, MAX_HOLD_MS));
    uint8_t z[9] = {0, 0, 8, 0, 0, 0, 0, 0, 0};
    SEND(hid_send(RID_GAMEPAD, z, sizeof z));
    return ok();
}

static cJSON *do_batch(const cJSON *p, rpc_err_t *err)
{
    const cJSON *steps = p ? cJSON_GetObjectItem(p, "steps") : NULL, *s;
    REQUIRE(cJSON_IsArray(steps) && cJSON_GetArraySize(steps) <= MAX_STEPS, "steps: array (max 64)");
    int i = 0;
    cJSON_ArrayForEach(s, steps) {
        const char *m = str(s, "method");
        if (!m) { sleep_ms(clampi(num(s, "delay_ms", 0), 0, 10000)); i++; continue; }
        REQUIRE(strcmp(m, "hid.batch"), "nested batch not allowed");
        cJSON *r = hid_exec(m, cJSON_GetObjectItem(s, "params"), err);
        cJSON_Delete(r);
        if (err->code) {
            char tmp[96];
            strlcpy(tmp, err->msg, sizeof tmp);
            rpc_set_err(err, err->code, "step %d: %s", i, tmp);
            hid_actions_release_all();
            return NULL;
        }
        i++;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "steps", i);
    return o;
}

static const struct { const char *m; cJSON *(*fn)(const cJSON *, rpc_err_t *); } TABLE[] = {
    {"hid.type", do_type}, {"hid.mouse_move", do_move}, {"hid.mouse_abs", do_abs},
    {"hid.click", do_click}, {"hid.button", do_button}, {"hid.scroll", do_scroll},
    {"hid.media", do_media}, {"hid.gamepad", do_gamepad}, {"hid.batch", do_batch},
};

bool hid_actions_known(const char *m)
{
    if (!strcmp(m, "hid.key") || !strcmp(m, "hid.key_down") || !strcmp(m, "hid.key_up")) return true;
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) if (!strcmp(TABLE[i].m, m)) return true;
    return false;
}

cJSON *hid_exec(const char *m, const cJSON *p, rpc_err_t *err)
{
    if (p && !cJSON_IsObject(p)) { rpc_set_err(err, E_INVALID_PARAMS, "params must be object"); return NULL; }
    if (!strcmp(m, "hid.key")) return do_key(p, err, 0);
    if (!strcmp(m, "hid.key_down")) return do_key(p, err, 1);
    if (!strcmp(m, "hid.key_up")) return do_key(p, err, 2);
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++)
        if (!strcmp(TABLE[i].m, m)) return TABLE[i].fn(p, err);
    rpc_set_err(err, E_NO_METHOD, "method not found: %s", m);
    return NULL;
}

void hid_actions_reset_state(void)
{
    s_mod = 0; memset(s_keys, 0, 6); s_btn = 0; s_abs_btn = 0; s_held_since = 0;
}

void hid_actions_release_all(void)
{
    hid_actions_reset_state();
    if (g_cfg.hid_keyboard) kbd_send();
    if (g_cfg.hid_mouse) mouse_send(0, 0, 0, 0);
    if (g_cfg.hid_abs) abs_send();
    if (g_cfg.hid_media) { uint8_t z[2] = {0}; hid_send(RID_MEDIA, z, 2); }
    s_held_since = 0;
}

void hid_actions_tick(void)
{
    if (s_held_since && g_cfg.hid_auto_release_ms &&
        esp_timer_get_time() - s_held_since > (int64_t)g_cfg.hid_auto_release_ms * 1000) {
        hid_actions_release_all();
    }
}
