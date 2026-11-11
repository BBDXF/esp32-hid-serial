/* NimBLE GAP: advertising, security (Just Works + bonding), connection tracking. */
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_hidd.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/dis/ble_svc_dis.h"
#include "hid_ble.h"
#include "config.h"
#include "uart_proto.h"

static const char *TAG = "hid_ble";
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static char s_peer[18];
static bool s_adv_enabled = true;
static uint16_t s_appearance;
static uint8_t s_pnp[7];
static char s_model[32];

void ble_store_config_init(void);

static void addr_str(const ble_addr_t *a, char *out)
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    struct ble_gap_conn_desc desc;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            if (ble_gap_conn_find(s_conn, &desc) == 0) addr_str(&desc.peer_id_addr, s_peer);
            ble_gap_security_initiate(s_conn);
            cJSON *d = cJSON_CreateObject();
            cJSON_AddStringToObject(d, "peer", s_peer);
            uart_proto_event("ble.connected", d);
        } else {
            hid_ble_adv_start();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT: {
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "peer", s_peer);
        cJSON_AddNumberToObject(d, "reason", ev->disconnect.reason);
        uart_proto_event("ble.disconnected", d);
        s_peer[0] = 0;
        hid_ble_adv_start();
        return 0;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) hid_ble_adv_start();
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (ev->enc_change.status == 0) {
            cJSON *d = cJSON_CreateObject();
            cJSON_AddStringToObject(d, "peer", s_peer);
            uart_proto_event("ble.paired", d);
        }
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* host lost its bond: drop ours and re-pair */
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0)
            ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io io = { .action = ev->passkey.params.action };
        if (io.action == BLE_SM_IOACT_NUMCMP) io.numcmp_accept = 1;
        ble_sm_inject_io(ev->passkey.conn_handle, &io);
        return 0;
    }
    }
    return 0;
}

esp_err_t hid_ble_adv_start(void)
{
    if (!s_adv_enabled || s_conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) return ESP_OK;

    struct ble_hs_adv_fields f = {0};
    static ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.appearance = s_appearance;
    f.appearance_is_present = 1;
    f.uuids16 = &hid_uuid;
    f.num_uuids16 = 1;
    f.uuids16_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc) { ESP_LOGE(TAG, "adv fields rc=%d", rc); return ESP_FAIL; }

    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (uint8_t *)g_cfg.ble_name;
    rsp.name_len = strlen(g_cfg.ble_name);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(30),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(50),
    };
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    return rc ? ESP_FAIL : ESP_OK;
}

void hid_ble_set_adv(bool enable)
{
    s_adv_enabled = enable;
    if (enable) hid_ble_adv_start();
    else if (ble_gap_adv_active()) ble_gap_adv_stop();
}

bool hid_ble_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }
const char *hid_ble_peer(void) { return s_peer; }

int hid_ble_bond_count(void)
{
    int n = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &n);
    return n;
}

esp_err_t hid_ble_unpair_all(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    return ble_store_clear() ? ESP_FAIL : ESP_OK;
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t hid_ble_init_stack(void)
{
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    bt_cfg.mode = ESP_BT_MODE_BLE;
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    return esp_nimble_init();
}

esp_err_t hid_ble_start(uint16_t appearance)
{
    s_appearance = appearance;
    /* Just Works + bonding (no display/keyboard on the device) */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_device_name_set(g_cfg.ble_name);
    ble_svc_gap_device_appearance_set(appearance);

    /* esp_hid hardcodes PnP vendor source = USB; override with configured value */
    s_pnp[0] = g_cfg.vid_src;
    s_pnp[1] = g_cfg.vid & 0xFF; s_pnp[2] = g_cfg.vid >> 8;
    s_pnp[3] = g_cfg.pid & 0xFF; s_pnp[4] = g_cfg.pid >> 8;
    s_pnp[5] = g_cfg.ver & 0xFF; s_pnp[6] = g_cfg.ver >> 8;
    ble_svc_dis_pnp_id_set((char *)s_pnp);
    strlcpy(s_model, g_cfg.model, sizeof s_model);
    ble_svc_dis_model_number_set(s_model);

    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
