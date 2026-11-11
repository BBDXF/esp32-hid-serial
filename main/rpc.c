/* Single command dispatcher shared by UART, HTTP /api and MCP. */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "esp_app_desc.h"
#include "rpc.h"
#include "config.h"
#include "wifi_mgr.h"
#include "hid_dev.h"
#include "uart_proto.h"

#define PROTO_VERSION 1

void rpc_set_err(rpc_err_t *e, int code, const char *fmt, ...)
{
    e->code = code;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    va_end(ap);
}

static cJSON *ok(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return o;
}

static const char *pstr(const cJSON *p, const char *k)
{
    const cJSON *v = p ? cJSON_GetObjectItem(p, k) : NULL;
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double pnum(const cJSON *p, const char *k, double def)
{
    const cJSON *v = p ? cJSON_GetObjectItem(p, k) : NULL;
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS((uint32_t)(uintptr_t)arg));
    esp_restart();
}

/* ---------- sys.* ---------- */
static cJSON *m_sys(const char *m, const cJSON *p, rpc_err_t *err)
{
    if (!strcmp(m, "sys.ping")) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "pong", true);
        cJSON_AddNumberToObject(o, "uptime_ms", (double)(esp_timer_get_time() / 1000));
        return o;
    }
    if (!strcmp(m, "sys.info")) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "fw", esp_app_get_description()->version);
        cJSON_AddNumberToObject(o, "proto", PROTO_VERSION);
        esp_chip_info_t ci;
        esp_chip_info(&ci);
        char chip[32];
        snprintf(chip, sizeof chip, "ESP32 rev%d.%d cores=%d", ci.revision / 100, ci.revision % 100, ci.cores);
        cJSON_AddStringToObject(o, "chip", chip);
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_BT);
        char ms[18];
        snprintf(ms, sizeof ms, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON_AddStringToObject(o, "bt_mac", ms);
        cJSON_AddNumberToObject(o, "heap_free", esp_get_free_heap_size());
        cJSON_AddNumberToObject(o, "heap_min", esp_get_minimum_free_heap_size());
        return o;
    }
    if (!strcmp(m, "sys.reboot")) {
        uint32_t d = (uint32_t)pnum(p, "delay_ms", 300);
        hid_dev_release_all();
        xTaskCreate(reboot_task, "reboot", 2048, (void *)(uintptr_t)d, 5, NULL);
        return ok();
    }
    if (!strcmp(m, "sys.factory_reset")) {
        const cJSON *c = p ? cJSON_GetObjectItem(p, "confirm") : NULL;
        if (!cJSON_IsTrue(c)) { rpc_set_err(err, E_INVALID_PARAMS, "confirm:true required"); return NULL; }
        config_reset();
        hid_dev_unpair_all();
        xTaskCreate(reboot_task, "reboot", 2048, (void *)300, 5, NULL);
        return ok();
    }
    if (!strcmp(m, "sys.log")) {
        static const char *lv[] = {"none", "error", "warn", "info", "debug"};
        const char *l = pstr(p, "level");
        for (int i = 0; l && i < 5; i++)
            if (!strcmp(l, lv[i])) { uart_proto_set_log_level(i); return ok(); }
        rpc_set_err(err, E_INVALID_PARAMS, "level: none|error|warn|info|debug");
        return NULL;
    }
    rpc_set_err(err, E_NO_METHOD, "method not found: %s", m);
    return NULL;
}

/* ---------- wifi.* ---------- */
static cJSON *m_wifi(const char *m, const cJSON *p, rpc_err_t *err)
{
    if (!strcmp(m, "wifi.status")) return wifi_mgr_status_json();
    if (!strcmp(m, "wifi.scan")) return wifi_mgr_scan_json();
    if (!strcmp(m, "wifi.connect")) {
        if (wifi_mgr_connect() != ESP_OK) { rpc_set_err(err, E_INVALID_PARAMS, "no ssid configured"); return NULL; }
        return ok();
    }
    if (!strcmp(m, "wifi.disconnect")) { wifi_mgr_disconnect(); return ok(); }
    if (!strcmp(m, "wifi.forget")) {
        wifi_mgr_disconnect();
        g_cfg.wifi_ssid[0] = g_cfg.wifi_pass[0] = 0;
        config_save();
        return ok();
    }
    if (!strcmp(m, "wifi.set")) {
        /* map short param names onto config keys */
        static const char *map[][2] = {
            {"ssid", "wifi.ssid"}, {"pass", "wifi.pass"}, {"ip", "wifi.ip"}, {"mask", "wifi.mask"},
            {"gw", "wifi.gw"}, {"dns", "wifi.dns"}, {"hostname", "wifi.hostname"},
        };
        if (!pstr(p, "ssid")) { rpc_set_err(err, E_INVALID_PARAMS, "ssid required"); return NULL; }
        cJSON *c = cJSON_CreateObject();
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
            const cJSON *v = cJSON_GetObjectItem(p, map[i][0]);
            if (v) cJSON_AddItemToObject(c, map[i][1], cJSON_Duplicate(v, true));
        }
        const char *mode = pstr(p, "ip_mode");
        if (mode) cJSON_AddBoolToObject(c, "wifi.static", !strcmp(mode, "static"));
        if (!cJSON_GetObjectItem(c, "wifi.pass")) cJSON_AddStringToObject(c, "wifi.pass", "");
        const char *bad = NULL;
        bool rb = false;
        esp_err_t e = config_apply_json(c, &rb, &bad);
        cJSON_Delete(c);
        if (e != ESP_OK) {
            rpc_set_err(err, e == ESP_ERR_INVALID_ARG || e == ESP_ERR_NOT_FOUND ? E_INVALID_PARAMS : E_CFG_WRITE,
                        "invalid: %s", bad ? bad : "?");
            return NULL;
        }
        err->reboot_required = rb;
        wifi_mgr_connect();
        return ok();
    }
    rpc_set_err(err, E_NO_METHOD, "method not found: %s", m);
    return NULL;
}

/* ---------- ble.* ---------- */
static cJSON *apply_prefixed(const cJSON *p, const char *prefix, const char *const *keys, size_t n,
                             const char *const *cfgkeys, rpc_err_t *err)
{
    cJSON *c = cJSON_CreateObject();
    for (size_t i = 0; i < n; i++) {
        const cJSON *v = p ? cJSON_GetObjectItem(p, keys[i]) : NULL;
        if (v) cJSON_AddItemToObject(c, cfgkeys[i], cJSON_Duplicate(v, true));
    }
    const char *bad = NULL;
    bool rb = false;
    esp_err_t e = config_apply_json(c, &rb, &bad);
    cJSON_Delete(c);
    if (e != ESP_OK) { rpc_set_err(err, E_INVALID_PARAMS, "invalid: %s", bad ? bad : "?"); return NULL; }
    err->reboot_required = rb;
    return ok();
}

static cJSON *m_ble(const char *m, const cJSON *p, rpc_err_t *err)
{
    if (!strcmp(m, "ble.status")) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "state", hid_dev_connected() ? "connected" : "advertising");
        cJSON_AddStringToObject(o, "peer", hid_dev_peer());
        cJSON_AddNumberToObject(o, "bonded", hid_dev_bond_count());
        cJSON_AddStringToObject(o, "name", g_cfg.ble_name);
        return o;
    }
    if (!strcmp(m, "ble.set_identity")) {
        static const char *k[] = {"name", "manufacturer", "model", "serial", "vid_src", "vid", "pid", "ver"};
        static const char *ck[] = {"ble.name", "ble.manufacturer", "ble.model", "ble.serial",
                                   "ble.vid_src", "ble.vid", "ble.pid", "ble.ver"};
        return apply_prefixed(p, "ble.", k, 8, ck, err);
    }
    if (!strcmp(m, "ble.set_profile")) {
        static const char *k[] = {"keyboard", "mouse", "abs_pointer", "media", "gamepad"};
        static const char *ck[] = {"hid.keyboard", "hid.mouse", "hid.abs_pointer", "hid.media", "hid.gamepad"};
        return apply_prefixed(p, "hid.", k, 5, ck, err);
    }
    if (!strcmp(m, "ble.advertise")) {
        const cJSON *en = p ? cJSON_GetObjectItem(p, "enable") : NULL;
        hid_dev_advertise(!en || cJSON_IsTrue(en));
        return ok();
    }
    if (!strcmp(m, "ble.unpair")) {
        hid_dev_unpair_all();
        return ok();
    }
    rpc_set_err(err, E_NO_METHOD, "method not found: %s", m);
    return NULL;
}

/* ---------- cfg.* ---------- */
static cJSON *m_cfg(const char *m, const cJSON *p, rpc_err_t *err)
{
    if (!strcmp(m, "cfg.get") || !strcmp(m, "cfg.export")) {
        cJSON *all = config_to_json(false);
        const cJSON *keys = p ? cJSON_GetObjectItem(p, "keys") : NULL;
        if (!cJSON_IsArray(keys)) return all;
        cJSON *o = cJSON_CreateObject();
        const cJSON *k;
        cJSON_ArrayForEach(k, keys) {
            if (!cJSON_IsString(k)) continue;
            cJSON *v = cJSON_GetObjectItem(all, k->valuestring);
            if (v) cJSON_AddItemToObject(o, k->valuestring, cJSON_Duplicate(v, true));
        }
        cJSON_Delete(all);
        return o;
    }
    if (!strcmp(m, "cfg.set") || !strcmp(m, "cfg.import")) {
        const char *bad = NULL;
        bool rb = false;
        esp_err_t e = config_apply_json(p, &rb, &bad);
        if (e != ESP_OK) {
            rpc_set_err(err, (e == ESP_ERR_INVALID_ARG || e == ESP_ERR_NOT_FOUND) ? E_INVALID_PARAMS : E_CFG_WRITE,
                        "invalid key/value: %s", bad ? bad : "?");
            return NULL;
        }
        err->reboot_required = rb;
        return ok();
    }
    rpc_set_err(err, E_NO_METHOD, "method not found: %s", m);
    return NULL;
}

cJSON *rpc_call(const char *method, const cJSON *params, rpc_src_t src, int req_id, rpc_err_t *err)
{
    memset(err, 0, sizeof *err);
    if (params && !cJSON_IsObject(params)) {
        rpc_set_err(err, E_INVALID_PARAMS, "params must be object");
        return NULL;
    }
    /* MCP may only drive HID and read status */
    if (src == RPC_SRC_MCP && strncmp(method, "hid.", 4) && strcmp(method, "ble.status") &&
        strcmp(method, "sys.info") && strcmp(method, "sys.ping")) {
        rpc_set_err(err, E_NO_METHOD, "not exposed via MCP: %s", method);
        return NULL;
    }
    if (!strncmp(method, "hid.", 4)) return hid_dev_submit(method, params, req_id, err);
    if (!strncmp(method, "sys.", 4)) return m_sys(method, params, err);
    if (!strncmp(method, "wifi.", 5)) return m_wifi(method, params, err);
    if (!strncmp(method, "ble.", 4)) return m_ble(method, params, err);
    if (!strncmp(method, "cfg.", 4)) return m_cfg(method, params, err);
    rpc_set_err(err, E_NO_METHOD, "method not found: %s", method);
    return NULL;
}

/* ---------- MCP tool catalogue ---------- */
static const rpc_tool_t TOOLS[] = {
    {"hid.key", "Press and release a key combination, e.g. [\"CTRL\",\"C\"]. Keys: A-Z 0-9 F1-F24 ENTER ESC TAB SPACE BACKSPACE DELETE HOME END PGUP PGDN UP DOWN LEFT RIGHT, modifiers CTRL SHIFT ALT GUI.",
     "{\"type\":\"object\",\"properties\":{\"keys\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"hold_ms\":{\"type\":\"integer\"}},\"required\":[\"keys\"]}"},
    {"hid.type", "Type ASCII text using US keyboard layout. Use \\n for Enter.",
     "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"},\"interval_ms\":{\"type\":\"integer\"}},\"required\":[\"text\"]}"},
    {"hid.mouse_move", "Move mouse relatively by dx,dy pixels (subject to OS acceleration).",
     "{\"type\":\"object\",\"properties\":{\"dx\":{\"type\":\"integer\"},\"dy\":{\"type\":\"integer\"}},\"required\":[\"dx\",\"dy\"]}"},
    {"hid.mouse_abs", "Move pointer to absolute screen position; x,y are fractions 0.0-1.0 of the screen.",
     "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"}},\"required\":[\"x\",\"y\"]}"},
    {"hid.click", "Click a mouse button, optionally at absolute position x,y (0.0-1.0).",
     "{\"type\":\"object\",\"properties\":{\"button\":{\"type\":\"string\",\"enum\":[\"left\",\"right\",\"middle\"]},\"count\":{\"type\":\"integer\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"}}}"},
    {"hid.button", "Press or release a mouse button (for drag).",
     "{\"type\":\"object\",\"properties\":{\"button\":{\"type\":\"string\"},\"state\":{\"type\":\"string\",\"enum\":[\"down\",\"up\"]}},\"required\":[\"state\"]}"},
    {"hid.scroll", "Scroll wheel: v vertical (positive = up), h horizontal.",
     "{\"type\":\"object\",\"properties\":{\"v\":{\"type\":\"integer\"},\"h\":{\"type\":\"integer\"}}}"},
    {"hid.media", "Press a media key: VOLUME_UP VOLUME_DOWN MUTE PLAY_PAUSE NEXT PREV STOP.",
     "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"}},\"required\":[\"key\"]}"},
    {"hid.gamepad", "Set gamepad state for hold_ms then release (gamepad profile must be enabled).",
     "{\"type\":\"object\",\"properties\":{\"buttons\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"lx\":{\"type\":\"integer\"},\"ly\":{\"type\":\"integer\"},\"rx\":{\"type\":\"integer\"},\"ry\":{\"type\":\"integer\"},\"lt\":{\"type\":\"integer\"},\"rt\":{\"type\":\"integer\"},\"hat\":{\"type\":\"string\"},\"hold_ms\":{\"type\":\"integer\"}}}"},
    {"hid.release_all", "Release all keys and buttons.", "{\"type\":\"object\",\"properties\":{}}"},
    {"ble.status", "Get BLE connection status of the HID device.", "{\"type\":\"object\",\"properties\":{}}"},
};

const rpc_tool_t *rpc_tools(int *count)
{
    *count = sizeof TOOLS / sizeof TOOLS[0];
    return TOOLS;
}
