#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "lwip/ip4_addr.h"
#include "mdns.h"
#include "wifi_mgr.h"
#include "config.h"
#include "uart_proto.h"

static esp_netif_t *s_netif;
static volatile wifi_state_t s_state = WIFI_ST_IDLE;
static esp_timer_handle_t s_retry_timer;
static int s_retry;
static bool s_want_connected;
static char s_ip[16];

static const char *state_str(wifi_state_t s)
{
    static const char *n[] = {"idle", "connecting", "connected", "failed", "disabled"};
    return n[s];
}

static void emit_state(void)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "state", state_str(s_state));
    if (s_state == WIFI_ST_CONNECTED) cJSON_AddStringToObject(d, "ip", s_ip);
    uart_proto_event("wifi.state", d);
}

static void retry_cb(void *arg)
{
    if (s_want_connected) esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        s_ip[0] = 0;
        if (!s_want_connected) { s_state = WIFI_ST_IDLE; emit_state(); return; }
        bool auth_fail = d->reason == WIFI_REASON_AUTH_FAIL ||
                         d->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                         d->reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
        wifi_state_t ns = auth_fail && s_retry >= 3 ? WIFI_ST_FAILED : WIFI_ST_CONNECTING;
        if (ns != s_state) { s_state = ns; emit_state(); }
        /* exponential backoff: 1s,2s,4s ... max 60s */
        int64_t delay = 1000000LL << (s_retry < 6 ? s_retry : 6);
        if (delay > 60000000LL) delay = 60000000LL;
        s_retry++;
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, delay);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        s_retry = 0;
        s_state = WIFI_ST_CONNECTED;
        emit_state();
    }
}

static void apply_ip_config(void)
{
    if (!g_cfg.wifi_static) {
        esp_netif_dhcpc_start(s_netif);
        return;
    }
    esp_netif_dhcpc_stop(s_netif);
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = ipaddr_addr(g_cfg.wifi_ip);
    ip.netmask.addr = ipaddr_addr(g_cfg.wifi_mask);
    ip.gw.addr = ipaddr_addr(g_cfg.wifi_gw);
    esp_netif_set_ip_info(s_netif, &ip);
    if (g_cfg.wifi_dns[0]) {
        esp_netif_dns_info_t dns = {0};
        dns.ip.u_addr.ip4.addr = ipaddr_addr(g_cfg.wifi_dns);
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
    }
}

esp_err_t wifi_mgr_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, g_cfg.hostname);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));  /* creds live in app config */
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* NOTE: keep default modem-sleep; required for Wi-Fi/BT coexistence */

    esp_timer_create_args_t ta = { .callback = retry_cb, .name = "wifi_retry" };
    esp_timer_create(&ta, &s_retry_timer);

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(g_cfg.hostname);
        mdns_instance_name_set("ESP32 HID Agent");
        mdns_service_add(NULL, "_http", "_tcp", g_cfg.http_port, NULL, 0);
    }

    if (g_cfg.wifi_ssid[0]) return wifi_mgr_connect();
    return ESP_OK;
}

esp_err_t wifi_mgr_connect(void)
{
    if (!g_cfg.wifi_ssid[0]) return ESP_ERR_INVALID_STATE;
    s_want_connected = false;
    esp_wifi_disconnect();
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, g_cfg.wifi_ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, g_cfg.wifi_pass, sizeof wc.sta.password);
    wc.sta.threshold.authmode = g_cfg.wifi_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) return err;
    apply_ip_config();
    s_retry = 0;
    s_want_connected = true;
    s_state = WIFI_ST_CONNECTING;
    emit_state();
    return esp_wifi_connect();
}

esp_err_t wifi_mgr_disconnect(void)
{
    s_want_connected = false;
    esp_timer_stop(s_retry_timer);
    return esp_wifi_disconnect();
}

wifi_state_t wifi_mgr_state(void) { return s_state; }

cJSON *wifi_mgr_status_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", state_str(s_state));
    cJSON_AddStringToObject(o, "ssid", g_cfg.wifi_ssid);
    cJSON_AddStringToObject(o, "ip", s_ip);
    wifi_ap_record_t ap;
    if (s_state == WIFI_ST_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        cJSON_AddNumberToObject(o, "rssi", ap.rssi);
    char host[48];
    snprintf(host, sizeof host, "%s.local", g_cfg.hostname);
    cJSON_AddStringToObject(o, "mdns", host);
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char m[18];
    snprintf(m, sizeof m, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(o, "mac", m);
    return o;
}

cJSON *wifi_mgr_scan_json(void)
{
    static const char *auth[] = {"open", "wep", "wpa", "wpa2", "wpa/wpa2", "wpa2-ent", "wpa3", "wpa2/wpa3", "wapi", "owe"};
    wifi_scan_config_t sc = { .show_hidden = false };
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "aps");
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return o;
    uint16_t n = 20;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (!recs) return o;
    esp_wifi_scan_get_ap_records(&n, recs);
    for (int i = 0; i < n; i++) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "ssid", (char *)recs[i].ssid);
        cJSON_AddNumberToObject(a, "rssi", recs[i].rssi);
        cJSON_AddNumberToObject(a, "ch", recs[i].primary);
        cJSON_AddStringToObject(a, "auth", recs[i].authmode < 10 ? auth[recs[i].authmode] : "other");
        cJSON_AddItemToArray(arr, a);
    }
    free(recs);
    return o;
}
