// Copyright 2020-2021 Espressif Systems (Shanghai) Co. Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "tusb_hid.h"
#include "descriptors_control.h"
#include "esp_debug_helpers.h"

static const char *TAG = "tusb_hid";

uint8_t curr_resolution_multiplier = 1;

void tinyusb_hid_mouse_report(
    uint8_t buttons, int8_t x, int8_t y, int8_t vertical, int8_t horizontal)
{
    ESP_LOGD(TAG, "buttons=%02x, x=%d, y=%d, vertical=%d, horizontal=%d", 
        buttons, x, y, vertical, horizontal);

    // Remote wakeup
    if (tud_suspended()) {
        // Wake up host if we are in suspend mode
        // and REMOTE_WAKEUP feature is enabled by host
        tud_remote_wakeup();
    } else {
        // Send the 1st of report chain, the rest will be sent by tud_hid_report_complete_cb()
        // skip if hid is not ready yet
        int i = 0;
        for (; i < 5 && !tud_hid_ready(); i++) {
            vTaskDelay(5);
        }
        if (i >= 5) {
            ESP_LOGW(__func__, "tinyusb not ready");
            return;
        }

        tud_hid_mouse_report(REPORT_ID_MOUSE, buttons, x, y, vertical, horizontal);
    }
}

void tinyusb_hid_keyboard_report(uint8_t *keycode)
{
    ESP_LOGD(TAG, "keycode: %02x %02x %02x %02x %02x %02x", 
        keycode[0], keycode[1], keycode[2], keycode[3], keycode[4], keycode[5]);

    // Remote wakeup
    if (tud_suspended()) {
        // Wake up host if we are in suspend mode
        // and REMOTE_WAKEUP feature is enabled by host
        tud_remote_wakeup();
    } else {
        // Send the 1st of report chain, the rest will be sent by tud_hid_report_complete_cb()
        // skip if hid is not ready yet
        int i = 0;
        for (; i < 5 && !tud_hid_ready(); i++) {
            vTaskDelay(5);
        }
        if (i >= 5) {
            ESP_LOGW(__func__, "tinyusb not ready");
            return;
        }

        tud_hid_keyboard_report(REPORT_ID_KEYBOARD, keycode[0], &keycode[2]);
    }
}

void tinyusb_hid_consumer_report(uint16_t keycode)
{
    ESP_LOGD(TAG, "consumer code: %04x", keycode);

    // Remote wakeup
    if (tud_suspended()) {
        // Wake up host if we are in suspend mode
        // and REMOTE_WAKEUP feature is enabled by host
        tud_remote_wakeup();
    } else {
        // Send the 1st of report chain, the rest will be sent by tud_hid_report_complete_cb()
        // skip if hid is not ready yet
        int i = 0;
        for (; i < 5 && !tud_hid_ready(); i++) {
            vTaskDelay(5);
        }
        if (i >= 5) {
            ESP_LOGW(__func__, "tinyusb not ready");
            return;
        }

        tud_hid_report(REPORT_ID_CONSUMER, &keycode, sizeof(keycode));
    }
}

/************************************************** TinyUSB callbacks ***********************************************/
// Invoked when sent REPORT successfully to host
// Application can use this to send the next report
// Note: For composite reports, report[0] is report ID
void tud_hid_report_complete_cb(uint8_t itf, uint8_t const *report, uint8_t len)
{
    (void) itf;
    (void) report;
    (void) len;
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    (void) instance;
    // esp_backtrace_print(8);
    // ESP_LOGI(TAG, "get instance %d, report id %d, report type %d, len %d", 
    //   instance, report_id, report_type, reqlen);

    if (report_type == HID_REPORT_TYPE_FEATURE) {
      if (report_id == REPORT_ID_MOUSE && reqlen >= 1) {
        /**
         * Return the resolution multiplier for high-resolution pointer & wheel.
         * Windows may deliberately aquire for this parameter, whereas Linux may not...
         */
        buffer[0] = curr_resolution_multiplier;
        return 1;
      }
    }

    return 0;
}

// My template LED callback
void __attribute__((weak)) kb_led_cb(uint8_t kbd_leds)
{
    ESP_LOGI(TAG, "LED: 0x%02x", kbd_leds);
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
  (void) instance;
  ESP_LOGI(TAG, "set instance %d, report id %d, report type %d, len %d, buf[0] 0x%02x", 
    instance, report_id, report_type, bufsize, buffer[0]);

  if (report_type == HID_REPORT_TYPE_OUTPUT) {
    // Set keyboard LED e.g Capslock, Numlock etc...
    if (report_id == REPORT_ID_KEYBOARD) {
      // bufsize should be (at least) 1
      if ( bufsize < 1 ) return;

      kb_led_cb(buffer[0]);
    }
  } else if (report_type == HID_REPORT_TYPE_FEATURE) {
    if (report_id == REPORT_ID_MOUSE) {
      /**
       * Set the resolution multiplier.
       * Windows should set it on connection.
       */
      if (bufsize >= 1) {
        curr_resolution_multiplier = buffer[0];
      }
    }
  }
}
