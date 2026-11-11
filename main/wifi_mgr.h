#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

typedef enum { WIFI_ST_IDLE, WIFI_ST_CONNECTING, WIFI_ST_CONNECTED, WIFI_ST_FAILED, WIFI_ST_DISABLED } wifi_state_t;

esp_err_t wifi_mgr_init(void);       /* starts STA if SSID configured */
esp_err_t wifi_mgr_connect(void);    /* (re)connect with g_cfg */
esp_err_t wifi_mgr_disconnect(void);
wifi_state_t wifi_mgr_state(void);
cJSON *wifi_mgr_status_json(void);
cJSON *wifi_mgr_scan_json(void);     /* blocking scan */
