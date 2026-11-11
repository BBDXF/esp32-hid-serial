/* UART JSON-Lines transport: '\n' framed, '\r' tolerated, '{' frame start,
 * '#' log lines, oversize discard, 500 ms half-frame timeout. */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cJSON.h"
#include "uart_proto.h"
#include "rpc.h"
#include "config.h"

#define UPORT       UART_NUM_0
#define LINE_MAX    2048
#define HALF_FRAME_TIMEOUT_US 500000

static SemaphoreHandle_t s_tx_lock;
static int s_log_level = 0;
static bool s_authed = false;

static char s_buf[LINE_MAX];
static size_t s_len;
static bool s_discarding;
static int64_t s_last_rx;

static void tx_line(const char *s)
{
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    uart_write_bytes(UPORT, s, strlen(s));
    uart_write_bytes(UPORT, "\n", 1);
    xSemaphoreGive(s_tx_lock);
}

static void tx_json(cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    if (s) { tx_line(s); cJSON_free(s); }
}

static void send_error(const cJSON *id, int code, const char *msg)
{
    cJSON *r = cJSON_CreateObject();
    if (id) cJSON_AddItemToObject(r, "id", cJSON_Duplicate(id, true));
    else cJSON_AddNullToObject(r, "id");
    cJSON *e = cJSON_AddObjectToObject(r, "error");
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg);
    tx_json(r);
    cJSON_Delete(r);
}

void uart_proto_event(const char *name, cJSON *data)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", name);
    cJSON_AddItemToObject(o, "data", data ? data : cJSON_CreateObject());
    tx_json(o);
    cJSON_Delete(o);
}

void uart_proto_set_log_level(int level) { s_log_level = level; }

void uart_proto_log(const char *level, const char *fmt, ...)
{
    static const char *names[] = {"", "error", "warn", "info", "debug"};
    int lv = 0;
    for (int i = 1; i <= 4; i++) if (strcmp(names[i], level) == 0) lv = i;
    if (lv == 0 || lv > s_log_level) return;
    char msg[192];
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(msg, sizeof msg, "#%s: ", level);
    vsnprintf(msg + n, sizeof msg - n, fmt, ap);
    va_end(ap);
    tx_line(msg);
}

/* Route ESP_LOG output as '#' lines so it never collides with JSON frames. */
static int log_vprintf(const char *fmt, va_list ap)
{
    if (s_log_level == 0) return 0;
    char msg[192];
    msg[0] = '#';
    int n = vsnprintf(msg + 1, sizeof msg - 1, fmt, ap);
    if (n <= 0) return 0;
    size_t l = strlen(msg);
    while (l > 1 && (msg[l - 1] == '\n' || msg[l - 1] == '\r')) msg[--l] = 0;
    tx_line(msg);
    return n;
}

static void handle_frame(char *line)
{
    cJSON *req = cJSON_Parse(line);
    if (!req) { send_error(NULL, E_PARSE, "parse error"); return; }
    const cJSON *id = cJSON_GetObjectItem(req, "id");
    const cJSON *method = cJSON_GetObjectItem(req, "method");
    const cJSON *params = cJSON_GetObjectItem(req, "params");
    if (!cJSON_IsString(method) || (id && !cJSON_IsNumber(id) && !cJSON_IsString(id))) {
        send_error(id, E_INVALID_REQ, "invalid request");
        goto out;
    }

    if (g_cfg.uart_token[0] && !s_authed) {
        if (strcmp(method->valuestring, "sys.auth") == 0) {
            const cJSON *t = params ? cJSON_GetObjectItem(params, "token") : NULL;
            if (cJSON_IsString(t) && strcmp(t->valuestring, g_cfg.uart_token) == 0) {
                s_authed = true;
                cJSON *r = cJSON_CreateObject();
                if (id) cJSON_AddItemToObject(r, "id", cJSON_Duplicate(id, true));
                cJSON_AddItemToObject(r, "result", cJSON_CreateTrue());
                tx_json(r); cJSON_Delete(r);
            } else send_error(id, E_UNAUTHORIZED, "bad token");
            goto out;
        }
        send_error(id, E_UNAUTHORIZED, "unauthorized");
        goto out;
    }

    rpc_err_t err = {0};
    int rid = cJSON_IsNumber(id) ? id->valueint : 0;
    cJSON *res = rpc_call(method->valuestring, params, RPC_SRC_UART, rid, &err);
    if (!id) { cJSON_Delete(res); goto out; }   /* notification: no reply */
    if (err.code) {
        cJSON_Delete(res);
        send_error(id, err.code, err.msg);
        goto out;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "id", cJSON_Duplicate(id, true));
    cJSON_AddItemToObject(r, "result", res ? res : cJSON_CreateObject());
    if (err.reboot_required) {
        cJSON *w = cJSON_AddObjectToObject(r, "warn");
        cJSON_AddNumberToObject(w, "code", W_REBOOT_REQ);
        cJSON_AddStringToObject(w, "message", "reboot required");
    }
    tx_json(r);
    cJSON_Delete(r);
out:
    cJSON_Delete(req);
}

static void feed_byte(uint8_t c)
{
    int64_t now = esp_timer_get_time();
    if (s_len && now - s_last_rx > HALF_FRAME_TIMEOUT_US) s_len = 0;
    s_last_rx = now;

    if (c == '\n') {
        if (s_discarding) {
            s_discarding = false; s_len = 0;
            send_error(NULL, E_INVALID_REQ, "frame too long");
            return;
        }
        if (s_len && s_buf[s_len - 1] == '\r') s_len--;
        s_buf[s_len] = 0;
        if (s_len && s_buf[0] == '{') handle_frame(s_buf);
        s_len = 0;   /* '#', empty, noise: dropped */
        return;
    }
    if (s_discarding) return;
    if (s_len == 0 && c != '{' && c != '#') return;
    if (s_len >= LINE_MAX - 1) { s_discarding = true; s_len = 0; return; }
    s_buf[s_len++] = (char)c;
}

static void rx_task(void *arg)
{
    uint8_t tmp[128];
    for (;;) {
        int n = uart_read_bytes(UPORT, tmp, sizeof tmp, pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++) feed_byte(tmp[i]);
    }
}

void uart_proto_init(void)
{
    s_tx_lock = xSemaphoreCreateMutex();
    uart_config_t cfg = {
        .baud_rate = (int)g_cfg.uart_baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UPORT, 4096, 4096, 0, NULL, 0);
    uart_param_config(UPORT, &cfg);
    esp_log_set_vprintf(log_vprintf);
    xTaskCreate(rx_task, "uart_rx", 6144, NULL, 10, NULL);
}
