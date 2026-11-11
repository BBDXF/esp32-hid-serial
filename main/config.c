#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include "config.h"
#include "nvs_flash.h"
#include "nvs.h"

#define NS "appcfg"
#define KEY "cfg"
#define CFG_VERSION 1

app_config_t g_cfg;

typedef enum { T_STR, T_BOOL, T_U8, T_U16, T_U32 } ftype_t;
typedef struct {
    const char *key;
    ftype_t type;
    size_t off;
    size_t size;      /* buffer size for strings */
    bool secret;
    bool reboot;      /* change requires reboot */
} field_t;

#define F(k, t, m, s, r) { k, t, offsetof(app_config_t, m), sizeof(((app_config_t *)0)->m), s, r }
static const field_t FIELDS[] = {
    F("wifi.ssid",   T_STR,  wifi_ssid, false, false),
    F("wifi.pass",   T_STR,  wifi_pass, true,  false),
    F("wifi.static", T_BOOL, wifi_static, false, false),
    F("wifi.ip",     T_STR,  wifi_ip,   false, false),
    F("wifi.mask",   T_STR,  wifi_mask, false, false),
    F("wifi.gw",     T_STR,  wifi_gw,   false, false),
    F("wifi.dns",    T_STR,  wifi_dns,  false, false),
    F("wifi.hostname", T_STR, hostname, false, true),
    F("ble.name",    T_STR,  ble_name,  false, true),
    F("ble.manufacturer", T_STR, manufacturer, false, true),
    F("ble.model",   T_STR,  model,     false, true),
    F("ble.serial",  T_STR,  serial,    false, true),
    F("ble.vid_src", T_U8,   vid_src,   false, true),
    F("ble.vid",     T_U16,  vid,       false, true),
    F("ble.pid",     T_U16,  pid,       false, true),
    F("ble.ver",     T_U16,  ver,       false, true),
    F("hid.keyboard", T_BOOL, hid_keyboard, false, true),
    F("hid.mouse",   T_BOOL, hid_mouse, false, true),
    F("hid.abs_pointer", T_BOOL, hid_abs, false, true),
    F("hid.media",   T_BOOL, hid_media, false, true),
    F("hid.gamepad", T_BOOL, hid_gamepad, false, true),
    F("hid.default_interval_ms", T_U16, hid_interval_ms, false, false),
    F("hid.auto_release_ms", T_U16, hid_auto_release_ms, false, false),
    F("http.enable", T_BOOL, http_enable, false, true),
    F("http.port",   T_U16,  http_port, false, true),
    F("http.token",  T_STR,  http_token, true, false),
    F("mcp.enable",  T_BOOL, mcp_enable, false, false),
    F("uart.baud",   T_U32,  uart_baud, false, true),
    F("uart.auth_token", T_STR, uart_token, true, false),
};
#define NFIELDS (sizeof(FIELDS) / sizeof(FIELDS[0]))

typedef struct { uint32_t version; app_config_t cfg; } blob_t;

static void set_defaults(app_config_t *c)
{
    memset(c, 0, sizeof(*c));
    strcpy(c->hostname, "hid-agent");
    strcpy(c->ble_name, "HID-Agent");
    strcpy(c->manufacturer, "HID-Agent");
    strcpy(c->model, "ESP32-HID-Serial");
    strcpy(c->serial, "0001");
    c->vid_src = 2;
    c->vid = 0x2225;
    c->pid = 0x3206;
    c->ver = 0x0100;
    c->hid_keyboard = c->hid_mouse = c->hid_abs = c->hid_media = true;
    c->hid_gamepad = false;
    c->hid_interval_ms = 8;
    c->hid_auto_release_ms = 5000;
    c->http_enable = true;
    c->http_port = 80;
    c->mcp_enable = true;
    c->uart_baud = 921600;
}

esp_err_t config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    set_defaults(&g_cfg);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK;
    blob_t b;
    size_t len = sizeof(b);
    if (nvs_get_blob(h, KEY, &b, &len) == ESP_OK && len == sizeof(b) && b.version == CFG_VERSION) {
        g_cfg = b.cfg;
    }
    nvs_close(h);
    return ESP_OK;
}

esp_err_t config_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    blob_t b = { .version = CFG_VERSION, .cfg = g_cfg };
    err = nvs_set_blob(h, KEY, &b, sizeof(b));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t config_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    set_defaults(&g_cfg);
    return ESP_OK;
}

cJSON *config_to_json(bool include_secrets)
{
    cJSON *o = cJSON_CreateObject();
    for (size_t i = 0; i < NFIELDS; i++) {
        const field_t *f = &FIELDS[i];
        uint8_t *p = (uint8_t *)&g_cfg + f->off;
        switch (f->type) {
        case T_STR:
            cJSON_AddStringToObject(o, f->key,
                (f->secret && !include_secrets) ? (*(char *)p ? "***" : "") : (char *)p);
            break;
        case T_BOOL: cJSON_AddBoolToObject(o, f->key, *(bool *)p); break;
        case T_U8:   cJSON_AddNumberToObject(o, f->key, *(uint8_t *)p); break;
        case T_U16:  cJSON_AddNumberToObject(o, f->key, *(uint16_t *)p); break;
        case T_U32:  cJSON_AddNumberToObject(o, f->key, *(uint32_t *)p); break;
        }
    }
    return o;
}

static bool parse_num(const cJSON *v, double *out)
{
    if (cJSON_IsNumber(v)) { *out = v->valuedouble; return true; }
    if (cJSON_IsString(v)) {   /* accept "0x1234" */
        char *end;
        unsigned long n = strtoul(v->valuestring, &end, 0);
        if (*end) return false;
        *out = (double)n;
        return true;
    }
    return false;
}

esp_err_t config_apply_json(const cJSON *obj, bool *need_reboot, const char **err_key)
{
    if (!cJSON_IsObject(obj)) return ESP_ERR_INVALID_ARG;
    app_config_t tmp = g_cfg;
    bool reboot = false;
    const cJSON *it;
    cJSON_ArrayForEach(it, obj) {
        const field_t *f = NULL;
        for (size_t i = 0; i < NFIELDS; i++) {
            if (strcmp(FIELDS[i].key, it->string) == 0) { f = &FIELDS[i]; break; }
        }
        if (!f) { *err_key = it->string; return ESP_ERR_NOT_FOUND; }
        uint8_t *p = (uint8_t *)&tmp + f->off;
        double n;
        switch (f->type) {
        case T_STR:
            if (!cJSON_IsString(it) || strlen(it->valuestring) >= f->size) goto bad;
            if (f->secret && strcmp(it->valuestring, "***") == 0) continue; /* unchanged */
            strcpy((char *)p, it->valuestring);
            break;
        case T_BOOL:
            if (!cJSON_IsBool(it)) goto bad;
            *(bool *)p = cJSON_IsTrue(it);
            break;
        case T_U8:  if (!parse_num(it, &n) || n < 0 || n > 0xFF) goto bad;       *(uint8_t *)p = n; break;
        case T_U16: if (!parse_num(it, &n) || n < 0 || n > 0xFFFF) goto bad;     *(uint16_t *)p = n; break;
        case T_U32: if (!parse_num(it, &n) || n < 0 || n > 0xFFFFFFFF) goto bad; *(uint32_t *)p = n; break;
        }
        if (f->reboot) reboot = true;
        continue;
bad:
        *err_key = it->string;
        return ESP_ERR_INVALID_ARG;
    }
    g_cfg = tmp;
    if (need_reboot) *need_reboot = reboot;
    return config_save();
}
