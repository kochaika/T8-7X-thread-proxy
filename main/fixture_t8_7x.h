/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/* Nanlite PavoTube T8-7X in the ULTIMATE DMX 8bit personality (43 channels).
 *
 *   CH1   DIMMER   0-255 = 0-100%
 *   CH2   MODE     0-38 CCT | 39-77 HSI | 78-115 RGBW | 116-154 EFFECT | 155-191 PIXEL FX
 *   CH3+  reinterpreted according to CH2:
 *           CCT   CH3 = 2700..12000 K, CH4 = green/magenta, CH5 = strobe
 *           HSI   CH3 = hue 0-359 deg, CH4 = saturation,    CH5 = strobe
 *
 * This proxy only ever drives the CCT and HSI engines. Green/magenta is held neutral and
 * strobe is held off.
 *
 * The channel map and the byte encodings below were reverse-engineered and confirmed
 * against a real fixture in the T8-7X-DMX project; they are deliberately reproduced as-is.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channels CH1..CH43 within the personality. */
#define T8_FOOTPRINT 43

/* GREEN/MAGENTA is bipolar with dead bands; 132 sits in the 120-145 neutral window. */
#define T8_GM_NEUTRAL 132

/* Matter's level and hue/saturation attributes top out at 254, not 255. */
#define T8_MATTER_MAX 254

typedef enum {
    T8_MODE_CCT = 0,
    T8_MODE_HSI = 1,
    T8_MODE_RGBW = 2,
    T8_MODE_EFFECT = 3,
    T8_MODE_PIXFX = 4,
    T8_MODE_COUNT,
} t8_mode_t;

/* Reads the stored DMX start address, brings up the DMX transmitter and parks the fixture
 * in a defined state (CCT mode, mid colour temperature, dimmer at zero). */
esp_err_t fixture_init(void);

uint16_t fixture_get_start_address(void);
/* Blacks out the old channel block, moves, re-renders, and persists to NVS. */
esp_err_t fixture_set_start_address(uint16_t address);

/* --- state driven by Matter -------------------------------------------------------- */

void fixture_set_power(bool on);
/* Matter LevelControl CurrentLevel, 0..254. */
void fixture_set_level(uint8_t matter_level);
void fixture_set_cct_kelvin(uint32_t kelvin);
void fixture_set_hsi(uint16_t hue_degrees, uint8_t saturation_255);

/* Blinks the dimmer channel at ~1 Hz, then restores the previous output. */
void fixture_identify_start(void);
void fixture_identify_stop(void);
bool fixture_identify_active(void);

/* --- console support --------------------------------------------------------------- */

/* Writes one channel of the personality (1-based), bypassing the state model. */
esp_err_t fixture_set_raw_channel(uint16_t channel, uint8_t value);
/* Copies CH1..CH43 of the current frame into `out`. */
esp_err_t fixture_get_channels(uint8_t *out, size_t count);
const char *fixture_mode_name(t8_mode_t mode);
t8_mode_t fixture_get_mode(void);

/* `hunt` sweeps a 16-channel window across the universe to find the fixture's block;
 * `walk` then steps one channel at a time. The channel that turns the light on is the
 * dimmer, i.e. the start address. Both run as background sweeps advanced once per frame. */
void fixture_sweep_hunt(void);
void fixture_sweep_walk(uint16_t lo, uint16_t hi);
void fixture_sweep_stop(void);
bool fixture_sweep_active(void);
uint16_t fixture_sweep_position(void);

#ifdef __cplusplus
}
#endif
