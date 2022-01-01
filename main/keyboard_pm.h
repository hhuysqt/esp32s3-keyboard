/**
 * This file is part of esp32s3-keyboard.
 *
 * Copyright (C) 2020-2021 Yuquan He <heyuquan20b at ict dot ac dot cn> 
 * (Institute of Computing Technology, Chinese Academy of Sciences)
 *
 * esp32s3-keyboard is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * esp32s3-keyboard is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with esp32s3-keyboard. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _MY_KB_PM_H
#define _MY_KB_PM_H

#include <stdint.h>
#include <stdbool.h>

/****************************************************************
 * 
 *  Typedefs
 * 
 ****************************************************************/

/**
 * Power state type
 */
typedef struct {
  uint32_t kb_int_us;   // 1/8 of keyboard scan interval
  uint32_t ble_int_cnt; // 4/5 of BLE connection interval
  uint32_t duration_us; // Time in microsecond of this state
  bool is_sleep;        // Enable auto light-sleep in esp-idf
} kb_pm_state_t;

/**
 * Power states
 */
typedef enum {
  PM_IDLE_LONG_TIME,
  PM_IDLE_SHORT_TIME,
  PM_KB_ACTIVE,
  PM_KB_TP_ACTIVE,
  PM_CHARGING
} kb_pm_t;

/****************************************************************
 * 
 *  Public interface
 * 
 ****************************************************************/

/**
 * Initializa the keyboard power management
 */
void init_pm(void);

/**
 * Update power management
 * @param new_pm_state New PM state when duration expires
 */
void flush_power_state(kb_pm_t new_pm_state);

/**
 * Get the keyboard scanning interval
 * @return scan interval in milliseconds
 */
unsigned get_kb_scan_interval_us(void);

/**
 * Wait for a while ifr the BLE connection interval decreases rapidly.
 */
bool pm_should_wait(void);

#endif
