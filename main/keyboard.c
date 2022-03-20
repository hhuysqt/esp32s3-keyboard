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

/**
 * Driver for Thinkpad keyboard & trackpad
 * USB & BLE interface.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "esp_log.h"

#include "driver/gpio.h"
#include "driver/uart.h"

#include "tusb.h"
#include "tusb_hid.h"
#include "tinyusb.h"
#include "esp_hidd_prf_api.h"
#include "esp_gap_ble_api.h"

#include "keymap.h"

#include "esp_err.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"

#include <sys/fcntl.h>
#include <sys/errno.h>
#include <sys/unistd.h>
#include <sys/select.h>

#include "pin_cfg.h"
#include "keyboard_pm.h"

/****************************************************************
 * 
 *  Private Definition
 * 
 ****************************************************************/

#define USE_FN_TRACKPOINT_PAN
// #define FN_SWITCH_TRACKPOINT_MIDPOINT
#define SCALE_TRACKPOINT_SPEED
#define MOUSE_SCALE_MIN 1

/****************************************************************
 * 
 *  Private Varibles
 * 
 ****************************************************************/

static bool is_init_finish = false;
static bool is_map_midkey_pan = 0;

// keyboard pin array
static uint rowscan_pins[18] = {
  KB_ROW_0, KB_ROW_1, KB_ROW_2, KB_ROW_3, KB_ROW_4, KB_ROW_5, KB_ROW_6, KB_ROW_7,
  KB_ROW_8, KB_ROW_9, KB_ROW_10, KB_ROW_11, KB_ROW_12, KB_ROW_13, KB_ROW_14, KB_ROW_15,
  KB_ROW_16, KB_ROW_17,
};

// UART1 fd for select()
static int uart1_fd = -1;

static uint wakeup_time = 0;
static const uint wakeup_period_us = 15000000;

// backlight duration
static const int MAX_BACKLIGHT_ON_US = 60*1000000;

static const char *TAG = "kb-task";

/****************************************************************
 * 
 *  Public Varibles
 * 
 ****************************************************************/

volatile bool is_usb_connected = false;
volatile bool is_backlight_on = false;
volatile int backlight_start_time;

// Manage LED state since Win10 won't report such info on BLE.
volatile bool is_caplk_on = false;
volatile bool is_numlk_on = false;

// bluetooth stuff
extern bool is_ble_connected;
extern esp_ble_conn_update_params_t ble_conn_param;
extern esp_pm_config_esp32s3_t esp_idf_pm_cfg;

/****************************************************************
 * 
 *  Private function prototypes
 * 
 ****************************************************************/

static void ps2_gpio_init(void);
static uint8_t ps2_read(void);
static void ps2_write_1(uint8_t ch);
static void ps2_write_2(uint8_t ch);

static void init_usb(void);
static void init_trackpad(void);
static void init_matrix_keyboard(void);

static void kb_set_column_scan(int n);
static void do_fnfunc(fn_function_t fncode);
static void led_task(void *arg);
static void poll_trackpoint(uint poll_ms);

/****************************************************************
 * 
 *  Callback override
 * 
 ****************************************************************/

// tinyusb callbacks for connection
void tud_mount_cb(void)
{
  is_usb_connected = true;
  printf("USB connected.\n");
  if (is_init_finish) {
    flush_power_state(PM_CHARGING);
  }
}

// tinyusb callbacks for disconnection
void tud_umount_cb(void)
{
  is_usb_connected = false;
  printf("USB disconnected\n");
}

void tud_suspend_cb(bool remote_wakeup_en)
{
  (void)remote_wakeup_en;
  is_usb_connected = false;
  // printf("USB suspended, %s\n");
  printf("%s(%s)\n", __func__, remote_wakeup_en ? "true" : "false");
}

void tud_resume_cb(void)
{
  is_usb_connected = true;
  printf("%s\n", __func__);
  if (is_init_finish) {
    flush_power_state(PM_CHARGING);
  }
}

void kb_led_cb(uint8_t kbd_leds)
{
  if (kbd_leds & KEYBOARD_LED_NUMLOCK) {
    LED_NUMLK_ON;
    is_numlk_on = true;
  } else {
    LED_NUMLK_OFF;
    is_numlk_on = false;
  }

  if (kbd_leds & KEYBOARD_LED_CAPSLOCK) {
    // Sometimes the GPIO may be reset on wake-up...
    // Manually init it again...
    GPIO_INIT_OUT_PULLUP(LED_CAPLK);
    LED_CAPLK_ON;
    is_caplk_on = true;
  } else {
    LED_CAPLK_OFF;
    is_caplk_on = false;
  }
}


/****************************************************************
 * 
 *  Private functions
 * 
 ****************************************************************/

/**
 * Initialize trackpad GPIO
 */
static void ps2_gpio_init(void)
{
  GPIO_INIT_OUT_PULLUP(PS2_CLK_PIN);
  GPIO_INIT_OUT_PULLUP(PS2_DATA_PIN);
  PS2_CLK_HIGH;
  PS2_DATA_HIGH;
}

/**
 * Read one byte from PS2.
 * **Only for trackpoint initialization!**
 * @return result
 */
static uint8_t ps2_read(void)
{
  while (PS2_CLK_STATE == 1);
  while (PS2_CLK_STATE == 0);

  uint8_t res = 0;
  for (int i = 0; i < 8; i++) {
    while (PS2_CLK_STATE == 1);
    if (PS2_DATA_STATE != 0) {
      res |= (1 << i);
    }
    while (PS2_CLK_STATE == 0);
  }
  while (PS2_CLK_STATE == 1);
  while (PS2_CLK_STATE == 0);
  while (PS2_CLK_STATE == 1);
  while (PS2_CLK_STATE == 0);

  printf("receive 0x%02x\n", res);
  return res;
}

/**
 * Write one byte to PS2.
 * **Only for trackpoint initialization!**
 * @param ch command
 */
static void ps2_write_1(uint8_t ch)
{
  uint8_t op = ch ^ 0x1;
  op = op ^ (op >> 4);
  op = op ^ (op >> 2);
  op = op ^ (op >> 1);
  op &= 0x1;
  
  printf("0x%02x, parity %c\n", ch, op ? '1' : '0');

  PS2_CLK_OUTPUT;
  PS2_DATA_OUTPUT;

  // start
  PS2_CLK_LOW;
  usleep(50);
  PS2_DATA_LOW;
  usleep(50);

  PS2_CLK_HIGH;
  PS2_CLK_INPUT;

  // data
  while (PS2_CLK_STATE == 1);
  for (int i = 0; i < 8; i++) {
    while (PS2_CLK_STATE == 0);
    if (ch & 0x1) {
      PS2_DATA_HIGH;
    } else {
      PS2_DATA_LOW;
    }
    ch >>= 1;
    while (PS2_CLK_STATE == 1);
  }

  // odd parity
  while (PS2_CLK_STATE == 0);
  if (op) {
    PS2_DATA_HIGH;
  } else {
    PS2_DATA_LOW;
  }
  while (PS2_CLK_STATE == 1);

  // end
  while (PS2_CLK_STATE == 0);
  PS2_DATA_HIGH;
  PS2_DATA_INPUT;
  while (PS2_CLK_STATE == 1);

  // ack
  while (PS2_CLK_STATE == 0);
  while (PS2_CLK_STATE == 1);
}

/**
 * Write one byte to PS2, using yet another timing...
 * **Only for trackpoint initialization!**
 * @param ch command
 */
static void ps2_write_2(uint8_t ch)
{
  uint8_t op = ch ^ 0x1;
  op = op ^ (op >> 4);
  op = op ^ (op >> 2);
  op = op ^ (op >> 1);
  op &= 0x1;
  
  printf("0x%02x, parity %c\n", ch, op ? '1' : '0');

  PS2_CLK_OUTPUT;
  PS2_DATA_OUTPUT;

  // start
  PS2_CLK_LOW;
  usleep(50);
  PS2_DATA_LOW;
  usleep(50);

  PS2_CLK_HIGH;
  PS2_CLK_INPUT;

  // data
  while (PS2_CLK_STATE == 1);
  for (int i = 0; i < 8; i++) {
    usleep(20);
    if (ch & 0x1) {
      PS2_DATA_HIGH;
    } else {
      PS2_DATA_LOW;
    }
    ch >>= 1;
    while (PS2_CLK_STATE == 0);
    while (PS2_CLK_STATE == 1);
  }

  // odd parity
  usleep(20);
  if (op) {
    PS2_DATA_HIGH;
  } else {
    PS2_DATA_LOW;
  }
  while (PS2_CLK_STATE == 0);
  while (PS2_CLK_STATE == 1);

  // end
  usleep(20);
  // PS2_DATA_HIGH;
  PS2_DATA_INPUT;
  while (PS2_DATA_STATE == 1);
  while (PS2_CLK_STATE == 1);

  // ack
  while (PS2_CLK_STATE == 0);
  while (PS2_CLK_STATE == 1);
}


static void init_usb(void)
{
    ESP_LOGI(TAG, "USB initialization");

    // Setting of descriptor. You can use descriptor_tinyusb and
    // descriptor_str_tinyusb as a reference
    tusb_desc_device_t my_descriptor = {
        .bLength = sizeof(my_descriptor),
        .bDescriptorType = TUSB_DESC_DEVICE,
        .bcdUSB = 0x0200, // USB version. 0x0200 means version 2.0
        .bDeviceClass = TUSB_CLASS_UNSPECIFIED,
        .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

        .idVendor = 0x303A,
        .idProduct = 0x3000,
        .bcdDevice = 0x0101, // Device FW version

        .iManufacturer = 0x01, // see string_descriptor[1] bellow
        .iProduct = 0x02,      // see string_descriptor[2] bellow
        .iSerialNumber = 0x03, // see string_descriptor[3] bellow

        .bNumConfigurations = 0x01
    };

    tusb_desc_strarray_device_t my_string_descriptor = {
        // array of pointer to string descriptors
        (char[]){0x09, 0x04}, // 0: is supported language is English (0x0409)
        "hhuysqt",            // 1: Manufacturer
        "Keyboard Hacker",    // 2: Product
        "012-345",            // 3: Serials, should use chip ID

        "my CDC",             // 4: CDC Interface
        "my MSC",             // 5: MSC Interface
        "my HID",             // 6: HID Interface
    };

    tinyusb_config_t tusb_cfg = {
        .descriptor = &my_descriptor,
        .string_descriptor = my_string_descriptor,
        .external_phy = false // In the most cases you need to use a `false` value
    };

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB initialization DONE");
}

static void init_trackpad(void)
{
  ps2_gpio_init();

  // reset mouse
  gpio_pad_select_gpio(PS2_RESET_PIN);
  gpio_set_direction(PS2_RESET_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(PS2_RESET_PIN, 1);
  vTaskDelay(10 / portTICK_PERIOD_MS);
  gpio_set_level(PS2_RESET_PIN, 0);
  vTaskDelay(70 / portTICK_PERIOD_MS);

  uint8_t ret = 0;
  void (*ps2_write)(uint8_t) = ps2_write_1;

  ps2_write(0xff);  // mouse reset
  ret = ps2_read();
  if (ret != 0xfa) {
    ESP_LOGI(TAG, "Use another timing...");
    ps2_write = ps2_write_2;
  }
  int nrtry;
  for (nrtry = 0; nrtry < 5; nrtry++) {
    ESP_LOGI(TAG, "Init round %d", nrtry);
    vTaskDelay(70 / portTICK_PERIOD_MS);
    ps2_write(0xff);  // mouse reset
    if (ps2_read() != 0xfa) continue;
    vTaskDelay(70 / portTICK_PERIOD_MS);
    ps2_write(0xff);  // mouse reset
    if (ps2_read() != 0xfa) continue;
    vTaskDelay(70 / portTICK_PERIOD_MS);
    ps2_write(0xf3);  // set sample rate
    if (ps2_read() != 0xfa) continue;
    vTaskDelay(3 / portTICK_PERIOD_MS);
    ps2_write(0x50);  // set sample rate 80
    if (ps2_read() != 0xfa) continue;
    vTaskDelay(3 / portTICK_PERIOD_MS);
    // ps2_write(0xf2);  // mouse id
    // if (ps2_read() != 0xfa) continue;
    // vTaskDelay(50 / portTICK_PERIOD_MS);
    ps2_write(0xf4);  // enable data reporting
    if (ps2_read() != 0xfa) continue;
    else break;
  }

  if (nrtry < 5) {
    ESP_LOGI(TAG, "PS2 initialized.");

    /**
     * From now on, PS2 will only be used as a receiver, and the DATA line
     * has the identical timing to a UART...
     */
    const uart_config_t uart_config = {
      .baud_rate = 14465,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_ODD,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_APB,
    };
    uart_driver_install(UART_NUM_1, 1024 * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_1, &uart_config);
    uart_set_pin(UART_NUM_1, -1, PS2_DATA_PIN, -1, -1);

    uart1_fd = open("/dev/uart/1", O_RDWR);
    if (uart1_fd < 0) {
      printf("Failed to open uart1. Mouse task exit...\n");
    } else {
      printf("VFS open uart1\n");
    }
  } else {
    ESP_LOGI(TAG, "Failed to init trackpoint...");
  }
}

static void init_matrix_keyboard(void)
{
  GPIO_INIT_OUT_PULLUP(KB_COLSEL_0);
  GPIO_INIT_OUT_PULLUP(KB_COLSEL_1);
  GPIO_INIT_OUT_PULLUP(KB_COLSEL_2);

  for (int i = 0; i < 18; i++) {
    GPIO_INIT_IN_PULLUP(rowscan_pins[i]);
  }

  GPIO_INIT_IN_PULLUP(BUTTON_FN);
  GPIO_INIT_IN_PULLUP(BUTTON_MIDDLE);

  GPIO_INIT_OUT_PULLDOWN(BACKLIGHT_PWM);
  GPIO_INIT_OUT_PULLUP(LED_CAPLK);
  GPIO_INIT_OUT_PULLDOWN(LED_F1);
  GPIO_INIT_OUT_PULLUP(LED_FNLK);     // MUX from TX0
  GPIO_INIT_OUT_PULLDOWN(LED_NUMLK);  // MUX from RX0

  BACKLIGHT_OFF;
  LED_CAPLK_OFF;
  LED_F1_OFF;
  LED_FNLK_OFF;
  LED_NUMLK_OFF;
  is_caplk_on = false;
  is_numlk_on = false;
}

/**
 * Set keyboard column scan.
 * @param n column number 0~7
 */
static void kb_set_column_scan(int n)
{
  gpio_set_level(KB_COLSEL_0, n & 0b001);
  gpio_set_level(KB_COLSEL_1, n & 0b010);
  gpio_set_level(KB_COLSEL_2, n & 0b100);
}

/**
 * Handle the FN function on keyboard
 * @param fncode see enum fn_function_t
 */
static void do_fnfunc(fn_function_t fncode)
{
  switch (fncode) {
  case FN_FNLOCK: {
#ifdef FN_SWITCH_TRACKPOINT_MIDPOINT
    is_map_midkey_pan ^= 0x1;
    if (is_map_midkey_pan) {
      LED_FNLK_ON;
    } else {
      LED_FNLK_OFF;
    }
#endif
    // Seems nothing else to do...
    break;
  }
  case FN_BACKLIGHT: {
    if (is_backlight_on) {
      BACKLIGHT_OFF;
      is_backlight_on = false;
    } else {
      BACKLIGHT_ON;
      backlight_start_time = esp_timer_get_time();
      is_backlight_on = true;
    }
    break;
  }
  default:
    break;
  }
}

/**
 * Blink task
 */
static void led_task(void *arg)
{
  (void)arg;

  while (1) {
    vTaskDelay(2000);
    while (!is_ble_connected && !is_usb_connected) {
      BACKLIGHT_OFF;
      LED_CAPLK_OFF;
      LED_NUMLK_OFF;
      is_backlight_on = false;
      is_caplk_on = false;
      is_numlk_on = false;

      // heart beat
      for (int i = 0; 
            i < 6 && !is_ble_connected && !is_usb_connected; 
            i++
      ) {
        LED_F1_ON;
        vTaskDelay(100 / portTICK_PERIOD_MS);
        LED_F1_OFF;
        vTaskDelay(100 / portTICK_PERIOD_MS);
        LED_F1_ON;
        vTaskDelay(100 / portTICK_PERIOD_MS);
        LED_F1_OFF;
        vTaskDelay(3700 / portTICK_PERIOD_MS);
      }
    }
    LED_F1_OFF;

    if (esp_idf_pm_cfg.light_sleep_enable) {
      int difftimeus = esp_timer_get_time() - backlight_start_time;
      if (difftimeus > MAX_BACKLIGHT_ON_US - 1000000) {
        BACKLIGHT_OFF;
      } else {
        vTaskDelay((MAX_BACKLIGHT_ON_US - difftimeus) / 1000 / portTICK_PERIOD_MS);
        if (esp_idf_pm_cfg.light_sleep_enable && 
          (esp_timer_get_time() - backlight_start_time) > MAX_BACKLIGHT_ON_US - 1000000
        ) {
          BACKLIGHT_OFF;
        }
      }
    }
  }
}

/**
 * Check the trackpoint PS2 input within a short time
 * @param poll_us poll time in microsecond
 */
static void poll_trackpoint(uint poll_us)
{
  if (uart1_fd < 0) {
    vTaskDelay(poll_us / 1000 / portTICK_PERIOD_MS);
    return;
  }

  // expire time for select()
  struct timeval mouse_tv = {
    .tv_sec = 0,
    .tv_usec = poll_us,
  };

  static uint lasttime = 0;
  static bool is_midkey = false, is_pan = true;

  int8_t buttons = 0, dx = 0, dy = 0;
  int8_t pan_x = 0, pan_y = 0;
  bool is_recv = false;

  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(uart1_fd, &rfds);

  // wait for PS2 input...
  int s = select(uart1_fd + 1, &rfds, NULL, NULL, &mouse_tv);

  if (s < 0) {
    ESP_LOGE(TAG, "Select failed: errno %d. Exit...", errno);
    close(uart1_fd);
    uart1_fd = -1;
  } else if (s != 0) {
    flush_power_state(PM_KB_TP_ACTIVE);
    if (is_ble_connected && !is_usb_connected && pm_should_wait()) {
      wakeup_time = esp_timer_get_time();
    }

    // parse all the PS2 packets
    while (1) {
      char mousebuf[3];
      int nrrd = uart_read_bytes(UART_NUM_1, mousebuf, 3, 5);
      if (nrrd > 0) {
        if (nrrd < 3) {
          // read the remaining bytes
          int nrrd2 = uart_read_bytes(UART_NUM_1, &mousebuf[nrrd], 3-nrrd, 3);
          nrrd += nrrd2;
        }
        if (nrrd == 3) {
          // printf("recv: %02x %02x %02x\n", mousebuf[0], mousebuf[1], mousebuf[2]);
          buttons |= mousebuf[0];
          dx += mousebuf[1], dy -= mousebuf[2];
          is_recv = true;
        } else {
          // printf("Only receive %d chars: ", nrrd);
          // for (int nr = 0; nr < nrrd; nr++) {
          //   printf("%02x ", mousebuf[nr]);
          // }
          // printf("\n");

          // discard the dirty data
          buttons = dx = dy = 0;
          is_recv = false;
          uart_flush_input(UART_NUM_1);
          break;
        }
      } else {
        break;
      }
    }
  }

  // suppress the first small motion
  uint currtime = esp_timer_get_time();
  // if (abs(dx) < 2
  //  && abs(dy) < 2 
  //  && (currtime-lasttime) > 10000000
  // ) {
  //   dx = dy = 0;
  //   lasttime = currtime;
  // }

  buttons &= 0b00000111;
  if (is_recv) {
#ifndef USE_FN_TRACKPOINT_PAN
  if (!is_map_midkey_pan) {
    LED_FNLK_OFF;

    // mid key detection
    if (buttons & 0b00000100) {
      is_midkey = true;
      // printf("midkey press\n");
      if (dx != 0 || dy != 0) {
        // middle key for pan
        pan_x = dx > 0 ? 1 : dx < 0 ? -1 : 0;
        pan_y = dy < 0 ? 1 : dy > 0 ? -1 : 0;
        dx = dy = 0;
        is_pan = true;
        // printf("midkey pan\n");
      }
    } else {
      if (is_midkey && !is_pan) {
        // printf("send mid key\n");
        if (is_usb_connected) {
          tinyusb_hid_mouse_report(0b00000100, 0,0,0,0);
          vTaskDelay(20);
          tinyusb_hid_mouse_report(0, 0,0,0,0);
          vTaskDelay(20);
        } else if (is_ble_connected) {
          esp_hidd_send_mouse_value(0b00000100, 0,0,0,0);
          vTaskDelay(20);
          esp_hidd_send_mouse_value(0, 0,0,0,0);
          vTaskDelay(20);
        }
      }
      is_midkey = is_pan = false;

      #ifdef SCALE_TRACKPOINT_SPEED
      // Scale the trackpoint mouse since it may be too slow...
      if (dx > MOUSE_SCALE_MIN) dx += (dx-MOUSE_SCALE_MIN) * 2;
      else if (dx < -MOUSE_SCALE_MIN) dx += (dx+MOUSE_SCALE_MIN) * 2;
      if (dy > MOUSE_SCALE_MIN) dy += (dy-MOUSE_SCALE_MIN) * 2;
      else if (dy < -MOUSE_SCALE_MIN) dy += (dy+MOUSE_SCALE_MIN) * 2;
      #endif
    }

    if (is_usb_connected) {
      tinyusb_hid_mouse_report(buttons & 0b00000011, dx, dy, pan_y, pan_x);
    } else if (is_ble_connected) {
      esp_hidd_send_mouse_value(buttons & 0b00000011, dx, dy, pan_y, pan_x);
    }

  } else {
    LED_FNLK_ON;

    #ifdef SCALE_TRACKPOINT_SPEED
    // Scale the trackpoint mouse since it may be too slow...
    if (dx > MOUSE_SCALE_MIN) dx += (dx-MOUSE_SCALE_MIN) * 2;
    else if (dx < -MOUSE_SCALE_MIN) dx += (dx+MOUSE_SCALE_MIN) * 2;
    if (dy > MOUSE_SCALE_MIN) dy += (dy-MOUSE_SCALE_MIN) * 2;
    else if (dy < -MOUSE_SCALE_MIN) dy += (dy+MOUSE_SCALE_MIN) * 2;
    #endif

    if (is_usb_connected) {
      tinyusb_hid_mouse_report(buttons, dx, dy, pan_y, pan_x);
    } else if (is_ble_connected) {
      esp_hidd_send_mouse_value(buttons, dx, dy, pan_y, pan_x);
    }
  }

#else

    if (BUTTON_FN_STATE == 0) {
      // panning
      pan_x = dx > 0 ? 1 : dx < 0 ? -1 : 0;
      pan_y = dy < 0 ? 1 : dy > 0 ? -1 : 0;
      dx = dy = 0;
    } else {
      #ifdef SCALE_TRACKPOINT_SPEED
      // Scale the trackpoint mouse since it may be too slow...
      if (dx > MOUSE_SCALE_MIN) dx += (dx-MOUSE_SCALE_MIN) * 2;
      else if (dx < -MOUSE_SCALE_MIN) dx += (dx+MOUSE_SCALE_MIN) * 2;
      if (dy > MOUSE_SCALE_MIN) dy += (dy-MOUSE_SCALE_MIN) * 2;
      else if (dy < -MOUSE_SCALE_MIN) dy += (dy+MOUSE_SCALE_MIN) * 2;
      #endif
    }

    if (is_usb_connected) {
      tinyusb_hid_mouse_report(buttons, dx, dy, pan_y, pan_x);
    } else if (is_ble_connected) {
      esp_hidd_send_mouse_value(buttons, dx, dy, pan_y, pan_x);
    }
#endif

    if (is_backlight_on) {
      BACKLIGHT_ON;
      backlight_start_time = currtime;
    }

    // printf("Mouse %3d, %3d; Pan %3d, %3d; Buttons 0x%02x\n", dx, dy, pan_x, pan_y, buttons);
    lasttime = currtime;
  }
}


/****************************************************************
 * 
 *  Public functions
 * 
 ****************************************************************/

/**
 * Keyboard task
 */
void keyboard_task(void *arg)
{
  (void)arg;

  init_usb();
  init_trackpad();
  init_matrix_keyboard();
  init_pm();
  xTaskCreate(&led_task,  "led_task", 4096, NULL, configMAX_PRIORITIES, NULL);
  ESP_LOGI(TAG, "Init finish");

  uint64_t lasthid = 0;
  uint16_t lasthotkey = 0;
  fn_function_t lastfnfunc = FN_NOP;
  // int lasti = -1, lastj = -1;

  while (1) {
    // Poll here and do not bother using semaphores...
    if (!is_usb_connected && !is_ble_connected) {
      vTaskDelay(2000);
      flush_power_state(PM_IDLE_LONG_TIME);
      continue;
    }

    bool is_key_pressed = false;
    uint64_t hid = 0;
    uint8_t *hidbuf = (uint8_t*)&hid;
    uint16_t hotkey = 0;
    fn_function_t fnfunc = FN_NOP;

    // int thisi = -1, thisj = -1;

    for (int i = 0; i < 8; i++) {
      for (int j = 0; j < 18; j++) {
        if (gpio_get_level(rowscan_pins[j]) == 0) {
          // thisi = i; thisj = j;
          if (BUTTON_FN_STATE != 0) {
            // normal keyboard usage
            int hidkey = search_hid_key(i, j);
            if (hidkey > 0) {
              if (hidkey >= KEY_LEFTCTRL && hidkey <= KEY_RIGHTMETA) {
                hidbuf[0] |= 1u << (hidkey & 0x07);
              } else if (!is_key_pressed) {
                hidbuf[2] = hidkey;
                is_key_pressed = true;
              }
              hotkey = 0; // clear hotkey
            }
          } else {
            // hotkey
            fn_keytable_t *fnitem = search_fn(i, j);
            if (fnitem != NULL) {
              is_key_pressed = true;
              hotkey = fnitem->hidcode;
              fnfunc = fnitem->fncode;
              hid = 0;  // clear keyboard key
            }
          }
        }
      }
      kb_set_column_scan(i);

      // static int nrtry = 0;
      // int lasttime = esp_timer_get_time();
      poll_trackpoint(get_kb_scan_interval_us());
      // int curtime = esp_timer_get_time();
      // if (nrtry < 5) {
      //   nrtry++;
      //   printf("pend time %d\n", curtime-lasttime);
      // }
    }

    uint currtime = esp_timer_get_time();

    if (is_key_pressed) {
      flush_power_state(PM_KB_ACTIVE);
      if (is_ble_connected && !is_usb_connected && pm_should_wait()) {
        wakeup_time = currtime;
      }

      if (is_backlight_on) {
        BACKLIGHT_ON;
        backlight_start_time = currtime;
      }
    } else {
      flush_power_state(PM_IDLE_LONG_TIME);
    }

    if (hid != lasthid) {
      // printf("%02x %02x %02x %02x %02x %02x %02x %02x\n",
      //   hidbuf[0], hidbuf[1], hidbuf[2], hidbuf[3], 
      //   hidbuf[4], hidbuf[5], hidbuf[6], hidbuf[7]
      // );
      if (is_usb_connected) {
        tinyusb_hid_keyboard_report(hidbuf);
      } else if (is_ble_connected) {
        esp_hidd_send_keyboard_value(hidbuf);
      }

      // Manage LED since Win10 won't report it.
      if (hidbuf[2] == KEY_CAPSLOCK) {
        if (is_caplk_on) {
          LED_CAPLK_OFF;
          is_caplk_on = false;
        } else  {
          LED_CAPLK_ON;
          is_caplk_on = true;
        }
      } else if (hidbuf[2] == KEY_NUMLOCK) {
        if (is_numlk_on) {
          LED_NUMLK_OFF;
          is_numlk_on = false;
        } else {
          LED_NUMLK_ON;
          is_numlk_on = true;
        }
      }
    }
    lasthid = hid;

    if (hotkey != lasthotkey) {
      // printf("%04x\n", hotkey);
      if (is_usb_connected) {
        tinyusb_hid_consumer_report(hotkey);
      } else if (is_ble_connected) {
        esp_hidd_send_consumer_value(hotkey);
      }
    }
    lasthotkey = hotkey;

    if (fnfunc != lastfnfunc) {
      do_fnfunc(fnfunc);
    }
    lastfnfunc = fnfunc;

    if (is_ble_connected
     && !is_usb_connected
     && currtime - wakeup_time < wakeup_period_us
    ) {
      LED_F1_ON;
      // send dummy data to wake it up
      hid = 0;
      esp_hidd_send_keyboard_value(hidbuf);
      esp_hidd_send_mouse_value(0, 0,0,0,0);
    } else {
      LED_F1_OFF;
    }

    // if (lasti != thisi && lastj != thisj && thisi != -1 && thisj != -1) {
    //   printf("%2d, %2d\n", thisi, thisj);
    // }
    // lasti = thisi;
    // lastj = thisj;
  }
}

