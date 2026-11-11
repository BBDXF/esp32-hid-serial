/* HID device: esp_hid init + serialized action queue. */
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_hidd.h"
#include "hid_dev.h"
#include "hid_ble.h"
#include "hid_report.h"
#include "hid_actions.h"
#include "config.h"
#include "uart_proto.h"

#define QUEUE_DEPTH 32

typedef struct {
    char method[24];
    cJSON *params;
    int id;
    bool async;
    SemaphoreHandle_t done;   /* sync only */
    rpc_err_t *err;           /* sync only */
    cJSON **result;           /* sync only */
} job_t;

static esp_hidd_dev_t *s_dev;
static QueueHandle_t s_q;
static SemaphoreHandle_t s_send_lock;
static esp_hid_raw_report_map_t s_maps[1];
static esp_hid_device_config_t s_hcfg;

esp_err_t hid_send(uint8_t rid, uint8_t *data, size_t len)
{
    if (!s_dev || !hid_ble_connected()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    esp_err_t e = esp_hidd_dev_input_set(s_dev, 0, rid, data, len);
    xSemaphoreGive(s_send_lock);
    return e;
}

static void hidd_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_hidd_event_data_t *p = data;
    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_START_EVENT:
        hid_ble_adv_start();
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        hid_actions_reset_state();
        break;
    case ESP_HIDD_OUTPUT_EVENT:
        if (p->output.report_id == RID_KEYBOARD && p->output.length >= 1) {
            uint8_t l = p->output.data[0];
            cJSON *d = cJSON_CreateObject();
            cJSON_AddBoolToObject(d, "num", l & 1);
            cJSON_AddBoolToObject(d, "caps", l & 2);
            cJSON_AddBoolToObject(d, "scroll", l & 4);
            uart_proto_event("hid.led", d);
        }
        break;
    default:
        break;
    }
}

static void worker(void *arg)
{
    job_t *j;
    for (;;) {
        if (xQueueReceive(s_q, &j, pdMS_TO_TICKS(200)) != pdTRUE) {
            hid_actions_tick();
            continue;
        }
        rpc_err_t local = {0};
        cJSON *res = hid_exec(j->method, j->params, j->async ? &local : j->err);
        cJSON_Delete(j->params);
        if (j->async) {
            cJSON *d = cJSON_CreateObject();
            cJSON_AddNumberToObject(d, "id", j->id);
            if (local.code) {
                cJSON_AddNumberToObject(d, "code", local.code);
                cJSON_AddStringToObject(d, "message", local.msg);
            }
            uart_proto_event("hid.done", d);
            cJSON_Delete(res);
            free(j);
        } else {
            *j->result = res;
            xSemaphoreGive(j->done);
        }
        hid_actions_tick();
    }
}

cJSON *hid_dev_submit(const char *method, const cJSON *params, int req_id, rpc_err_t *err)
{
    if (!strcmp(method, "hid.release_all")) {   /* emergency: bypass queue */
        hid_dev_release_all();
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", true);
        return o;
    }
    if (!hid_actions_known(method)) { rpc_set_err(err, E_NO_METHOD, "method not found: %s", method); return NULL; }
    if (!hid_ble_connected()) { rpc_set_err(err, E_BLE_NOT_CONN, "BLE host not connected"); return NULL; }

    bool async = !strcmp(method, "hid.type") || !strcmp(method, "hid.batch");
    job_t *j = calloc(1, sizeof *j);
    if (!j) { rpc_set_err(err, E_INTERNAL, "oom"); return NULL; }
    strlcpy(j->method, method, sizeof j->method);
    j->params = params ? cJSON_Duplicate(params, true) : NULL;
    j->id = req_id;
    j->async = async;
    cJSON *result = NULL;
    if (!async) {
        j->done = xSemaphoreCreateBinary();
        j->err = err;
        j->result = &result;
    }
    if (xQueueSend(s_q, &j, 0) != pdTRUE) {
        cJSON_Delete(j->params);
        if (j->done) vSemaphoreDelete(j->done);
        free(j);
        rpc_set_err(err, E_HID_BUSY, "HID queue full");
        return NULL;
    }
    if (async) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "accepted", true);
        return o;
    }
    /* every action is time-bounded in hid_exec, so wait unconditionally */
    xSemaphoreTake(j->done, portMAX_DELAY);
    vSemaphoreDelete(j->done);
    free(j);
    return result;
}

void hid_dev_release_all(void)
{
    hid_actions_release_all();
}

esp_err_t hid_dev_init(void)
{
    s_send_lock = xSemaphoreCreateMutex();
    s_q = xQueueCreate(QUEUE_DEPTH, sizeof(job_t *));

    const uint8_t *map;
    size_t len = hid_report_build(g_cfg.hid_keyboard, g_cfg.hid_mouse, g_cfg.hid_media,
                                  g_cfg.hid_abs, g_cfg.hid_gamepad, &map);
    s_maps[0].data = map;
    s_maps[0].len = len;
    s_hcfg = (esp_hid_device_config_t){
        .vendor_id = g_cfg.vid,
        .product_id = g_cfg.pid,
        .version = g_cfg.ver,
        .device_name = g_cfg.ble_name,
        .manufacturer_name = g_cfg.manufacturer,
        .serial_number = g_cfg.serial,
        .report_maps = s_maps,
        .report_maps_len = 1,
    };

    esp_err_t e = hid_ble_init_stack();
    if (e != ESP_OK) return e;
    e = esp_hidd_dev_init(&s_hcfg, ESP_HID_TRANSPORT_BLE, hidd_cb, &s_dev);
    if (e != ESP_OK) return e;
    uint16_t appearance = g_cfg.hid_keyboard ? ESP_HID_APPEARANCE_KEYBOARD
                        : g_cfg.hid_gamepad ? ESP_HID_APPEARANCE_GAMEPAD
                        : ESP_HID_APPEARANCE_MOUSE;
    hid_ble_start(appearance);
    xTaskCreate(worker, "hid_worker", 4096, NULL, 9, NULL);
    return ESP_OK;
}

bool hid_dev_connected(void) { return hid_ble_connected(); }
const char *hid_dev_peer(void) { return hid_ble_peer(); }
void hid_dev_advertise(bool en) { hid_ble_set_adv(en); }
esp_err_t hid_dev_unpair_all(void) { return hid_ble_unpair_all(); }
int hid_dev_bond_count(void) { return hid_ble_bond_count(); }
