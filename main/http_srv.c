/* HTTP server: config page, /api endpoints, and MCP (Streamable HTTP, JSON responses). */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "cJSON.h"
#include "http_srv.h"
#include "config.h"
#include "rpc.h"

#define MAX_BODY 4096
#define MCP_PROTO "2025-03-26"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_srv;

static bool authorized(httpd_req_t *req)
{
    if (!g_cfg.http_token[0]) return true;
    char h[80];
    if (httpd_req_get_hdr_value_str(req, "Authorization", h, sizeof h) != ESP_OK) return false;
    return strncmp(h, "Bearer ", 7) == 0 && strcmp(h + 7, g_cfg.http_token) == 0;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, s ? s : "{}");
    cJSON_free(s);
    return e;
}

static esp_err_t deny(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
    return httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
}

static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > MAX_BODY) return NULL;
    char *buf = malloc(req->content_len + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) { free(buf); return NULL; }
        got += r;
    }
    buf[got] = 0;
    cJSON *o = cJSON_Parse(buf);
    free(buf);
    return o;
}

/* ---------- GET / ---------- */
static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

/* ---------- POST /api/rpc  {"method":..,"params":..} ---------- */
static esp_err_t h_rpc(httpd_req_t *req)
{
    if (!authorized(req)) return deny(req);
    cJSON *in = read_json(req);
    cJSON *out = cJSON_CreateObject();
    const cJSON *m = in ? cJSON_GetObjectItem(in, "method") : NULL;
    if (!cJSON_IsString(m)) {
        cJSON *e = cJSON_AddObjectToObject(out, "error");
        cJSON_AddNumberToObject(e, "code", in ? E_INVALID_REQ : E_PARSE);
        cJSON_AddStringToObject(e, "message", "bad request");
    } else {
        rpc_err_t err;
        cJSON *res = rpc_call(m->valuestring, cJSON_GetObjectItem(in, "params"), RPC_SRC_HTTP, 0, &err);
        if (err.code) {
            cJSON_Delete(res);
            cJSON *e = cJSON_AddObjectToObject(out, "error");
            cJSON_AddNumberToObject(e, "code", err.code);
            cJSON_AddStringToObject(e, "message", err.msg);
        } else {
            cJSON_AddItemToObject(out, "result", res ? res : cJSON_CreateObject());
            if (err.reboot_required) cJSON_AddBoolToObject(out, "reboot_required", true);
        }
    }
    esp_err_t r = send_json(req, "200 OK", out);
    cJSON_Delete(out);
    cJSON_Delete(in);
    return r;
}

/* ---------- GET/POST /api/config ---------- */
static esp_err_t h_cfg_get(httpd_req_t *req)
{
    if (!authorized(req)) return deny(req);
    cJSON *o = config_to_json(false);
    esp_err_t r = send_json(req, "200 OK", o);
    cJSON_Delete(o);
    return r;
}

static esp_err_t h_cfg_post(httpd_req_t *req)
{
    if (!authorized(req)) return deny(req);
    cJSON *in = read_json(req);
    const char *bad = NULL;
    bool rb = false;
    esp_err_t e = in ? config_apply_json(in, &rb, &bad) : ESP_ERR_INVALID_ARG;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", e == ESP_OK);
    if (e != ESP_OK) cJSON_AddStringToObject(o, "error", bad ? bad : "invalid body");
    cJSON_AddBoolToObject(o, "reboot_required", rb);
    esp_err_t r = send_json(req, e == ESP_OK ? "200 OK" : "400 Bad Request", o);
    cJSON_Delete(o);
    cJSON_Delete(in);
    return r;
}

/* ---------- MCP ---------- */
static cJSON *mcp_result(const cJSON *id)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    cJSON_AddItemToObject(r, "id", id ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
    return r;
}

static cJSON *mcp_error(const cJSON *id, int code, const char *msg)
{
    cJSON *r = mcp_result(id);
    cJSON *e = cJSON_AddObjectToObject(r, "error");
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg);
    return r;
}

static void tool_name_to_method(const char *name, char *out, size_t n)
{
    strlcpy(out, name, n);
    char *u = strchr(out, '_');      /* first '_' separates group: hid_mouse_move -> hid.mouse_move */
    if (u) *u = '.';
}

static cJSON *mcp_handle(const cJSON *msg)
{
    const cJSON *id = cJSON_GetObjectItem(msg, "id");
    const cJSON *m = cJSON_GetObjectItem(msg, "method");
    const cJSON *params = cJSON_GetObjectItem(msg, "params");
    if (!cJSON_IsString(m)) return id ? mcp_error(id, E_INVALID_REQ, "invalid request") : NULL;
    const char *method = m->valuestring;

    if (!id) return NULL;  /* notifications (e.g. notifications/initialized) */

    if (!strcmp(method, "initialize")) {
        cJSON *r = mcp_result(id);
        cJSON *res = cJSON_AddObjectToObject(r, "result");
        const cJSON *pv = params ? cJSON_GetObjectItem(params, "protocolVersion") : NULL;
        cJSON_AddStringToObject(res, "protocolVersion", cJSON_IsString(pv) ? pv->valuestring : MCP_PROTO);
        cJSON *caps = cJSON_AddObjectToObject(res, "capabilities");
        cJSON_AddObjectToObject(caps, "tools");
        cJSON *si = cJSON_AddObjectToObject(res, "serverInfo");
        cJSON_AddStringToObject(si, "name", "esp32-hid-serial");
        cJSON_AddStringToObject(si, "version", "1.0.0");
        cJSON_AddStringToObject(res, "instructions",
            "Controls a computer through a Bluetooth HID keyboard/mouse. "
            "Prefer hid_click with x,y (0..1 fractions) for pointing. Call ble_status first to verify connection.");
        return r;
    }
    if (!strcmp(method, "ping")) {
        cJSON *r = mcp_result(id);
        cJSON_AddObjectToObject(r, "result");
        return r;
    }
    if (!strcmp(method, "tools/list")) {
        cJSON *r = mcp_result(id);
        cJSON *res = cJSON_AddObjectToObject(r, "result");
        cJSON *arr = cJSON_AddArrayToObject(res, "tools");
        int n;
        const rpc_tool_t *t = rpc_tools(&n);
        for (int i = 0; i < n; i++) {
            if (!strncmp(t[i].method, "hid.gamepad", 11) && !g_cfg.hid_gamepad) continue;
            cJSON *o = cJSON_CreateObject();
            char name[40];
            strlcpy(name, t[i].method, sizeof name);
            for (char *c = name; *c; c++) if (*c == '.') *c = '_';
            cJSON_AddStringToObject(o, "name", name);
            cJSON_AddStringToObject(o, "description", t[i].description);
            cJSON_AddItemToObject(o, "inputSchema", cJSON_Parse(t[i].input_schema));
            cJSON_AddItemToArray(arr, o);
        }
        return r;
    }
    if (!strcmp(method, "tools/call")) {
        const cJSON *name = params ? cJSON_GetObjectItem(params, "name") : NULL;
        const cJSON *args = params ? cJSON_GetObjectItem(params, "arguments") : NULL;
        if (!cJSON_IsString(name)) return mcp_error(id, E_INVALID_PARAMS, "name required");
        char rm[40];
        tool_name_to_method(name->valuestring, rm, sizeof rm);
        rpc_err_t err;
        cJSON *res = rpc_call(rm, args, RPC_SRC_MCP, 0, &err);
        if (err.code == E_NO_METHOD) { cJSON_Delete(res); return mcp_error(id, E_INVALID_PARAMS, err.msg); }
        cJSON *r = mcp_result(id);
        cJSON *out = cJSON_AddObjectToObject(r, "result");
        cJSON *content = cJSON_AddArrayToObject(out, "content");
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "type", "text");
        if (err.code) {
            char t[128];
            snprintf(t, sizeof t, "error %d: %s", err.code, err.msg);
            cJSON_AddStringToObject(item, "text", t);
            cJSON_AddBoolToObject(out, "isError", true);
        } else {
            if (!res) res = cJSON_CreateObject();
            char *s = cJSON_PrintUnformatted(res);
            cJSON_AddStringToObject(item, "text", s ? s : "{}");
            cJSON_free(s);
            cJSON_AddBoolToObject(out, "isError", false);
        }
        cJSON_AddItemToArray(content, item);
        cJSON_Delete(res);
        return r;
    }
    return mcp_error(id, E_NO_METHOD, "method not found");
}

static esp_err_t h_mcp_post(httpd_req_t *req)
{
    if (!g_cfg.mcp_enable) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "mcp disabled"); return ESP_OK; }
    if (!authorized(req)) return deny(req);
    cJSON *in = read_json(req);
    if (!in) {
        cJSON *e = mcp_error(NULL, E_PARSE, "parse error");
        send_json(req, "400 Bad Request", e);
        cJSON_Delete(e);
        return ESP_OK;
    }
    cJSON *out = NULL;
    if (cJSON_IsArray(in)) {                 /* batch */
        out = cJSON_CreateArray();
        const cJSON *it;
        cJSON_ArrayForEach(it, in) {
            cJSON *r = mcp_handle(it);
            if (r) cJSON_AddItemToArray(out, r);
        }
        if (cJSON_GetArraySize(out) == 0) { cJSON_Delete(out); out = NULL; }
    } else {
        out = mcp_handle(in);
    }
    cJSON_Delete(in);
    if (!out) {                              /* only notifications/responses */
        httpd_resp_set_status(req, "202 Accepted");
        return httpd_resp_send(req, NULL, 0);
    }
    esp_err_t r = send_json(req, "200 OK", out);
    cJSON_Delete(out);
    return r;
}

static esp_err_t h_mcp_get(httpd_req_t *req)
{
    /* no server-initiated SSE stream */
    httpd_resp_set_status(req, "405 Method Not Allowed");
    httpd_resp_set_hdr(req, "Allow", "POST");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t http_srv_start(void)
{
    if (!g_cfg.http_enable || s_srv) return ESP_OK;
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.server_port = g_cfg.http_port;
    c.stack_size = 8192;
    c.max_uri_handlers = 8;
    c.lru_purge_enable = true;
    esp_err_t e = httpd_start(&s_srv, &c);
    if (e != ESP_OK) return e;
    const httpd_uri_t uris[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = h_index },
        { .uri = "/api/rpc",    .method = HTTP_POST, .handler = h_rpc },
        { .uri = "/api/config", .method = HTTP_GET,  .handler = h_cfg_get },
        { .uri = "/api/config", .method = HTTP_POST, .handler = h_cfg_post },
        { .uri = "/mcp",        .method = HTTP_POST, .handler = h_mcp_post },
        { .uri = "/mcp",        .method = HTTP_GET,  .handler = h_mcp_get },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(s_srv, &uris[i]);
    return ESP_OK;
}

void http_srv_stop(void)
{
    if (s_srv) { httpd_stop(s_srv); s_srv = NULL; }
}
