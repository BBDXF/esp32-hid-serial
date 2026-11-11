#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "cJSON.h"
#include "config.h"
#include "uart_proto.h"
#include "wifi_mgr.h"
#include "hid_dev.h"
#include "http_srv.h"
#include "wifi_mgr.h"

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "poweron";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP:return "deepsleep";
    default:               return "other";
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(config_init());
    uart_proto_init();
    ESP_ERROR_CHECK(hid_dev_init());     /* BLE first: releases classic BT memory */
    ESP_ERROR_CHECK(wifi_mgr_init());
    http_srv_start();                    /* binds to any interface; usable once IP obtained */

    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "fw", esp_app_get_description()->version);
    cJSON_AddNumberToObject(d, "proto", 1);
    cJSON_AddStringToObject(d, "reset_reason", reset_reason());
    uart_proto_event("sys.ready", d);
}
