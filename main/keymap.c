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
 * Common part of keymap
 */

#include "keymap.h"

#ifdef USE_KEYMAP_E530
#include "keymap-e530.c"
#elif defined (USE_KEYMAP_E580)
#include "keymap-e580.c"
#else
#include "keymap-e580.c"
#endif

static const int nr_keys = sizeof(kbtbl) / sizeof(keytable_t);
static const int nr_fn_keys = sizeof(fntbl) / sizeof(fn_keytable_t);

int search_hid_key(unsigned scan1, unsigned scan2)
{
  uint16_t cmbcode = scan1 | (scan2 << 8);
  int i;
  for (i = 0; i < nr_keys; i++) {
    uint16_t item = *(uint16_t*)(&kbtbl[i].scan1);
    if (item == cmbcode) {
      break;
    }
  }
  return i < nr_keys ? kbtbl[i].hidcode : -1;
}

fn_keytable_t* search_fn(unsigned scan1, unsigned scan2)
{
  uint16_t cmbcode = scan1 | (scan2 << 8);
  int i;
  for (i = 0; i < nr_fn_keys; i++) {
    uint16_t item = *(uint16_t*)(&fntbl[i].scan1);
    if (item == cmbcode) {
      break;
    }
  }
  return i < nr_fn_keys ? &fntbl[i] : 0;
}
