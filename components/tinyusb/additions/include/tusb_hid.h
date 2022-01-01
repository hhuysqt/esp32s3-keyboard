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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "tusb.h"
#include "tinyusb.h"


/**
 * @brief Report mouse movement and buttons.
 * @param buttons hid mouse button bit mask
 * @param x Current delta x movement of the mouse
 * @param y Current delta y movement on the mouse
 * @param vertical Current delta wheel movement on the mouse
 * @param horizontal using AC Pan
 */
void tinyusb_hid_mouse_report(
  uint8_t buttons, int8_t x, int8_t y, int8_t vertical, int8_t horizontal);

/**
 * @brief Report key press in the keyboard, using array here, contains six keys at most.
 * @param keycode hid keyboard code array
 */
void tinyusb_hid_keyboard_report(uint8_t *keycode);

/**
 * @brief Report multimedia keys.
 * @param keycode 2-byte multimedia keycode
 */
void tinyusb_hid_consumer_report(uint16_t keycode);

#ifdef __cplusplus
}
#endif
