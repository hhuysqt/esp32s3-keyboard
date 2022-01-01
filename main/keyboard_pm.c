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

#include "keyboard_pm.h"
#include "pin_cfg.h"

#include "esp_hidd_prf_api.h"
#include "esp_gap_ble_api.h"
#include "esp_err.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "driver/uart.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_debug_helpers.h"

/****************************************************************
 * 
 *  Private Varibles
 * 
 ****************************************************************/

// power management state configuration
static kb_pm_state_t pm_cfg[] = {
  // keyboard idle for a long time. 5mA without BLE, 1
  [PM_IDLE_LONG_TIME] = {
    .kb_int_us = 25000,   // *8 = 160ms per scan
    .ble_int_cnt = 800,   // *1.25 = 1000ms BLE connection interval
    .duration_us = -1,
    .is_sleep = true
  },
  // keyboard idle for a short time. 26mA with BLE
  [PM_IDLE_SHORT_TIME] = {
    .kb_int_us = 5000,    // *8 = 40ms per scan
    .ble_int_cnt = 32,    // *1.25 = 40ms BLE connection interval
    .duration_us = 120000000,
    .is_sleep = true
  },
  // keyboard active but trackpoint inactive. 30mA with BLE
  [PM_KB_ACTIVE] = {
    .kb_int_us = 5000,    // *8 = 40ms per scan
    .ble_int_cnt = 20,    // *1.25 = 25ms BLE connection interval
    .duration_us = 420000000,
    .is_sleep = true
  },
  // trackpoint active. 50mA with BLE
  [PM_KB_TP_ACTIVE] = {
    .kb_int_us = 5000,    // *8 = 40ms per scan
    .ble_int_cnt = 10,    // *1.25 = 12.5ms BLE connection interval
    .duration_us = 120000000,
    .is_sleep = false
  },
  // charging
  [PM_CHARGING] = {
    .kb_int_us = 2000,    // *8 = 16ms per scan
    .ble_int_cnt = 10,    // *1.25 = 12.5ms BLE connection interval
    .duration_us = 60000000,
    .is_sleep = false
  },
};

static const int nr_pm_states = sizeof(pm_cfg) / sizeof(kb_pm_state_t);
static kb_pm_t curr_pm_state = PM_IDLE_LONG_TIME;
static volatile uint last_pm_timestamp = 0;

static SemaphoreHandle_t pm_lock;

// charging pin event queue
static xQueueHandle gpio_evt_queue = NULL;

static const char *TAG = "kb-pm";

static bool is_pm_increase_rapid = false;

/****************************************************************
 * 
 *  Public varibles
 * 
 ****************************************************************/

// power management in esp-idf library
esp_pm_config_esp32s3_t esp_idf_pm_cfg = {
  .max_freq_mhz = 80, // e.g. 80, 160, 240
  .min_freq_mhz = 80, // e.g. 40
  .light_sleep_enable = false,  // not enable at boot time
};

extern volatile bool is_usb_connected;
extern volatile bool is_ble_connected;
extern esp_ble_conn_update_params_t ble_conn_param;

/****************************************************************
 * 
 *  Private Functions
 * 
 ****************************************************************/

/**
 * Charging pin detection handler
 */
static void IRAM_ATTR gpio_isr_handler(void* arg)
{
  uint32_t gpio_num = (uint32_t) arg;
  xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void charging_detection_task(void* arg)
{
  uint32_t io_num;
  for(;;) {
    if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
      // Hang FreeRTOS light sleep for a while...
      vTaskDelay(1);
      vTaskDelay(1);
      vTaskDelay(1);
      if (CHARGING_STATE != 0) {
        ESP_LOGI(TAG, "Charging. Turn off power saving.");
        flush_power_state(PM_CHARGING);
      }
    }
  }
}

/**
 * Update the BLE connection interval and power management settings
 * @param new_pm_state The state index
 */
static void update_ble_and_pm(kb_pm_t new_pm_state)
{
  // if (new_pm_state == PM_KB_TP_ACTIVE) {
  //   esp_backtrace_print(5);
  // }
  ESP_LOGI(TAG, "State %d, keyboard %d, BLE %d", new_pm_state,
    pm_cfg[new_pm_state].kb_int_us, pm_cfg[new_pm_state].ble_int_cnt);
  if (is_ble_connected 
    && pm_cfg[new_pm_state].ble_int_cnt != ble_conn_param.min_int)
  {
    int lastint = ble_conn_param.min_int;
    ble_conn_param.min_int = 
    ble_conn_param.max_int = pm_cfg[new_pm_state].ble_int_cnt;
    if (esp_ble_gap_update_conn_params(&ble_conn_param) != ESP_OK) {
      // restore last parameter and wait for next trial
      ble_conn_param.min_int = 
      ble_conn_param.max_int = lastint;
    }
  }

  if (esp_idf_pm_cfg.light_sleep_enable != pm_cfg[new_pm_state].is_sleep) {
    esp_idf_pm_cfg.light_sleep_enable = pm_cfg[new_pm_state].is_sleep;
    esp_pm_configure(&esp_idf_pm_cfg);
  }
}

/****************************************************************
 * 
 *  Public interface
 * 
 ****************************************************************/

void init_pm(void)
{
  // Charging detection
  GPIO_INIT_IN_FLOATING(CHARGING_PIN);

  esp_sleep_config_gpio_isolate();

  gpio_wakeup_enable(CHARGING_PIN, GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  gpio_sleep_set_direction(CHARGING_PIN, GPIO_MODE_INPUT);
  gpio_sleep_set_pull_mode(CHARGING_PIN, GPIO_FLOATING);

  // LEDs
  gpio_sleep_set_direction(BACKLIGHT_PWM, GPIO_MODE_OUTPUT);
  gpio_sleep_set_pull_mode(BACKLIGHT_PWM, GPIO_PULLDOWN_ONLY);
  gpio_sleep_set_direction(LED_F1, GPIO_MODE_OUTPUT);
  gpio_sleep_set_pull_mode(LED_F1, GPIO_PULLUP_ONLY);
  gpio_sleep_set_direction(LED_CAPLK, GPIO_MODE_OUTPUT);
  gpio_sleep_set_pull_mode(LED_CAPLK, GPIO_PULLUP_ONLY);

  // PS2 pins
  gpio_sleep_set_direction(PS2_DATA_PIN, GPIO_MODE_INPUT);
  gpio_sleep_set_pull_mode(PS2_DATA_PIN, GPIO_PULLUP_ONLY);
  gpio_sleep_set_direction(PS2_CLK_PIN, GPIO_MODE_INPUT);
  gpio_sleep_set_pull_mode(PS2_CLK_PIN, GPIO_PULLUP_ONLY);
  gpio_sleep_set_direction(PS2_RESET_PIN, GPIO_MODE_OUTPUT);
  gpio_sleep_set_pull_mode(PS2_RESET_PIN, GPIO_PULLDOWN_ONLY);

  // USB pins
  gpio_sleep_set_pull_mode(19, GPIO_PULLDOWN_ONLY);
  gpio_sleep_set_pull_mode(20, GPIO_PULLDOWN_ONLY);

  // charging task
  gpio_evt_queue = xQueueCreate(4, sizeof(uint32_t));
  xTaskCreate(charging_detection_task, "charging_detection_task", 2048, NULL, 10, NULL);

  gpio_set_intr_type(CHARGING_PIN, GPIO_INTR_NEGEDGE);
  gpio_install_isr_service(0);
  gpio_isr_handler_add(CHARGING_PIN, gpio_isr_handler, (void*)CHARGING_PIN);

  ESP_ERROR_CHECK( uart_set_wakeup_threshold(UART_NUM_1, 3) );
  ESP_ERROR_CHECK( esp_sleep_enable_uart_wakeup(UART_NUM_1) );

  pm_lock = xSemaphoreCreateMutex();
  if (CHARGING_STATE == 0) {
    curr_pm_state = PM_IDLE_LONG_TIME;
    esp_idf_pm_cfg.light_sleep_enable = true;
    esp_pm_configure(&esp_idf_pm_cfg);
  } else {
    curr_pm_state = PM_CHARGING;
    esp_idf_pm_cfg.light_sleep_enable = false;
    esp_pm_configure(&esp_idf_pm_cfg);
  }
  last_pm_timestamp = esp_timer_get_time();
}

void flush_power_state(kb_pm_t new_pm_state)
{
  if (xSemaphoreTake(pm_lock, 100) != pdTRUE) {
    ESP_LOGE(TAG, "Mutex wait too long...");
    return;
  }

  uint currtime = esp_timer_get_time();

  if (CHARGING_STATE != 0) {
    // force high power when charging
    new_pm_state = PM_CHARGING;
    last_pm_timestamp = currtime;
  } else if (new_pm_state < curr_pm_state) {
    // gradual decrement
    new_pm_state = curr_pm_state-1;
  } else {
    last_pm_timestamp = currtime;
  }

  // sleep for a while in case the BLE cannot response in time.
  is_pm_increase_rapid = 
       (CHARGING_STATE == 0) 
    && ((curr_pm_state == PM_IDLE_SHORT_TIME && new_pm_state == PM_KB_TP_ACTIVE)
     || (curr_pm_state == PM_IDLE_LONG_TIME && new_pm_state >= PM_KB_ACTIVE)
    );

  uint diff_time = currtime - last_pm_timestamp;
  if (new_pm_state > curr_pm_state // increase power immediately
    || diff_time > pm_cfg[curr_pm_state].duration_us)
  {
    update_ble_and_pm(new_pm_state);
    curr_pm_state = new_pm_state;
    last_pm_timestamp = currtime;
  // } else {
  //   update_ble_and_pm(curr_pm_state);
  }

  xSemaphoreGive(pm_lock);
}

unsigned get_kb_scan_interval_us(void)
{
  return pm_cfg[curr_pm_state].kb_int_us * 5 / 6;
}

bool pm_should_wait(void)
{
  return is_pm_increase_rapid;
}
