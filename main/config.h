#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

typedef struct {
    /* wifi */
    char wifi_ssid[33];
    char wifi_pass[65];
    bool wifi_static;
    char wifi_ip[16], wifi_mask[16], wifi_gw[16], wifi_dns[16];
    char hostname[32];
    /* ble identity */
    char ble_name[30];
    char manufacturer[32];
    char model[32];
    char serial[32];
    uint8_t vid_src;     /* 1 = Bluetooth SIG, 2 = USB-IF */
    uint16_t vid, pid, ver;
    /* hid profile */
    bool hid_keyboard, hid_mouse, hid_abs, hid_media, hid_gamepad;
    uint16_t hid_interval_ms;
    uint16_t hid_auto_release_ms;
    /* http / mcp */
    bool http_enable;
    uint16_t http_port;
    char http_token[48];
    bool mcp_enable;
    /* uart */
    uint32_t uart_baud;
    char uart_token[48];
} app_config_t;

extern app_config_t g_cfg;

esp_err_t config_init(void);          /* nvs init + load (defaults on miss) */
esp_err_t config_save(void);
esp_err_t config_reset(void);         /* erase namespace */
/* JSON helpers. include_secrets=false -> passwords as "***" */
cJSON *config_to_json(bool include_secrets);
/* apply flat "a.b" keys; returns true in *need_reboot if identity/profile changed.
 * err_msg receives first invalid key */
esp_err_t config_apply_json(const cJSON *obj, bool *need_reboot, const char **err_key);
