#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"
#include "rpc.h"

/* Report IDs */
#define RID_KEYBOARD 1
#define RID_MOUSE    2
#define RID_MEDIA    3
#define RID_ABS      4
#define RID_GAMEPAD  5

esp_err_t hid_dev_init(void);
bool hid_dev_connected(void);
const char *hid_dev_peer(void);          /* "" when not connected */
void hid_dev_advertise(bool enable);
esp_err_t hid_dev_unpair_all(void);
int hid_dev_bond_count(void);

/* Queue an hid.* method for serialized execution on the HID worker task.
 * Sync methods block until done (or timeout) and return result.
 * Long methods (hid.type, hid.batch) return {"accepted":true} immediately
 * and emit event hid.done when finished. */
cJSON *hid_dev_submit(const char *method, const cJSON *params, int req_id, rpc_err_t *err);

/* immediate emergency release (bypasses queue) */
void hid_dev_release_all(void);
