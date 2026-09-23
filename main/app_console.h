/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/* Serial debug console for the DMX side.
 *
 * The fixture's personality and start address are stored inside the fixture and cannot be
 * read back over plain DMX (there is no display and no menu; changing them needs the
 * WC-USBC-C1 wire controller, the NANLINK app, or RDM). `hunt` and `walk` are therefore the
 * only way to discover where the fixture listens, and `addr` stores the answer.
 */

#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_console_init(void);

#ifdef __cplusplus
}
#endif
