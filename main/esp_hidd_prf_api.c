// Copyright 2017-2018 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "esp_hidd_prf_api.h"
#include "hidd_le_prf_int.h"
#include "hid_dev.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

// HID keyboard input report length
#define HID_KEYBOARD_IN_RPT_LEN     8

// HID LED output report length
#define HID_LED_OUT_RPT_LEN         1

// HID mouse input report length
#define HID_MOUSE_IN_RPT_LEN        5

// HID consumer control input report length
#define HID_CC_IN_RPT_LEN           2

extern uint16_t hid_conn_id;
extern bool is_ble_connected;

esp_err_t esp_hidd_register_callbacks(esp_hidd_event_cb_t callbacks)
{
    esp_err_t hidd_status;

    if(callbacks != NULL) {
   	    hidd_le_env.hidd_cb = callbacks;
    } else {
        return ESP_FAIL;
    }

    if((hidd_status = hidd_register_cb()) != ESP_OK) {
        return hidd_status;
    }

    esp_ble_gatts_app_register(BATTRAY_APP_ID);

    if((hidd_status = esp_ble_gatts_app_register(HIDD_APP_ID)) != ESP_OK) {
        return hidd_status;
    }

    return hidd_status;
}

esp_err_t esp_hidd_profile_init(void)
{
     if (hidd_le_env.enabled) {
        ESP_LOGE(HID_LE_PRF_TAG, "HID device profile already initialized");
        return ESP_FAIL;
    }
    // Reset the hid device target environment
    memset(&hidd_le_env, 0, sizeof(hidd_le_env_t));
    hidd_le_env.enabled = true;
    return ESP_OK;
}

esp_err_t esp_hidd_profile_deinit(void)
{
    uint16_t hidd_svc_hdl = hidd_le_env.hidd_inst.att_tbl[HIDD_LE_IDX_SVC];
    if (!hidd_le_env.enabled) {
        ESP_LOGE(HID_LE_PRF_TAG, "HID device profile already initialized");
        return ESP_OK;
    }

    if(hidd_svc_hdl != 0) {
	esp_ble_gatts_stop_service(hidd_svc_hdl);
	esp_ble_gatts_delete_service(hidd_svc_hdl);
    } else {
	return ESP_FAIL;
   }

    /* register the HID device profile to the BTA_GATTS module*/
    esp_ble_gatts_app_unregister(hidd_le_env.gatt_if);

    return ESP_OK;
}

uint16_t esp_hidd_get_version(void)
{
	return HIDD_VERSION;
}

void esp_hidd_send_consumer_value(uint16_t key)
{
    ESP_LOGD(HID_LE_PRF_TAG, "key = %04x", key);
    hid_dev_send_report(hidd_le_env.gatt_if, hid_conn_id,
        HID_RPT_ID_CC_IN, HID_REPORT_TYPE_INPUT, HID_CC_IN_RPT_LEN, (uint8_t*)&key);
    return;
}

void esp_hidd_send_keyboard_value(uint8_t *buffer)
{
    if (!is_ble_connected) {
        return;
    }

    ESP_LOGD(HID_LE_PRF_TAG, "the key vaule = %d,%d,%d,%d,%d,%d,%d,%d", 
        buffer[0], buffer[1], buffer[2], buffer[3], 
        buffer[4], buffer[5], buffer[6], buffer[7]);
    hid_dev_send_report(hidd_le_env.gatt_if, hid_conn_id,
        HID_RPT_ID_KEY_IN, HID_REPORT_TYPE_INPUT, HID_KEYBOARD_IN_RPT_LEN, buffer);
    return;
}

void esp_hidd_send_mouse_value(uint8_t buttons, 
    int8_t dx, int8_t dy, int8_t vertical, int8_t horizontal)
{
    if (!is_ble_connected) {
        return;
    }

    uint8_t buffer[HID_MOUSE_IN_RPT_LEN];
    buffer[0] = buttons;        // Buttons
    buffer[1] = dx;             // X
    buffer[2] = dy;             // Y
    buffer[3] = vertical;       // Wheel
    buffer[4] = horizontal;     // AC Pan

    hid_dev_send_report(hidd_le_env.gatt_if, hid_conn_id,
        HID_RPT_ID_MOUSE_IN, HID_REPORT_TYPE_INPUT, HID_MOUSE_IN_RPT_LEN, buffer);
    return;
}
