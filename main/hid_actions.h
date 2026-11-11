#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"
#include "rpc.h"

/* implemented in hid_dev.c */
esp_err_t hid_send(uint8_t rid, uint8_t *data, size_t len);

/* implemented in hid_actions.c (runs on HID worker task) */
bool hid_actions_known(const char *method);
cJSON *hid_exec(const char *method, const cJSON *params, rpc_err_t *err);
void hid_actions_release_all(void);
void hid_actions_reset_state(void);
void hid_actions_tick(void);   /* auto-release of stuck keys */
