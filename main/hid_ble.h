#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t hid_ble_init_stack(void);          /* controller + nimble init */
esp_err_t hid_ble_start(uint16_t appearance); /* after esp_hidd_dev_init */
esp_err_t hid_ble_adv_start(void);
void hid_ble_set_adv(bool enable);
bool hid_ble_connected(void);
const char *hid_ble_peer(void);
int hid_ble_bond_count(void);
esp_err_t hid_ble_unpair_all(void);
