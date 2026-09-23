/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "dmx512.h"
#include "fixture_t8_7x.h"

static const char *TAG = "fixture";

#define NVS_NAMESPACE "t8fixture"
#define NVS_KEY_ADDR  "dmx_addr"

#define IDENTIFY_HALF_PERIOD_MS 500
#define SWEEP_HUNT_WINDOW       16
#define SWEEP_HUNT_DWELL_MS     1500
#define SWEEP_WALK_DWELL_MS     1000

typedef struct {
    const char *name;
    uint8_t lo;
    uint8_t hi;
} t8_band_t;

/* CH2 - MODE. Only CCT and HSI are ever selected by the Matter path; the rest are kept so
 * the console's raw/mode commands still describe the whole fixture. */
static const t8_band_t MODES[T8_MODE_COUNT] = {
    [T8_MODE_CCT] = {"cct", 0, 38},
    [T8_MODE_HSI] = {"hsi", 39, 77},
    [T8_MODE_RGBW] = {"rgbw", 78, 115},
    [T8_MODE_EFFECT] = {"effect", 116, 154},
    [T8_MODE_PIXFX] = {"pixfx", 155, 191},
};

typedef enum {
    SWEEP_OFF = 0,
    SWEEP_HUNT,
    SWEEP_WALK,
} sweep_mode_t;

static SemaphoreHandle_t s_lock;

static uint16_t s_start_address = CONFIG_T8_DMX_START_ADDRESS;
static t8_mode_t s_mode = T8_MODE_CCT;

static bool s_power;
static uint8_t s_level = T8_MATTER_MAX;
static uint32_t s_kelvin = (CONFIG_T8_CCT_MIN_KELVIN + CONFIG_T8_CCT_MAX_KELVIN) / 2;
static uint16_t s_hue_degrees;
static uint8_t s_saturation;

static bool s_identify_active;
static bool s_identify_on;
static uint8_t s_identify_level;

static sweep_mode_t s_sweep_mode;
static uint16_t s_sweep_pos;
static uint16_t s_sweep_hi;
static int64_t s_sweep_stamp_ms;

/* ---------------------------------------------------------------- value encodings ---
 * All linear, no gamma: the fixture applies its own curve. Ported verbatim from
 * dmx_fixture_control_esp32c6.ino.
 */

static uint8_t kelvin_to_byte(uint32_t kelvin)
{
    if (kelvin < CONFIG_T8_CCT_MIN_KELVIN) {
        kelvin = CONFIG_T8_CCT_MIN_KELVIN;
    }
    if (kelvin > CONFIG_T8_CCT_MAX_KELVIN) {
        kelvin = CONFIG_T8_CCT_MAX_KELVIN;
    }
    return (uint8_t)(((kelvin - CONFIG_T8_CCT_MIN_KELVIN) * 255UL) /
                     (CONFIG_T8_CCT_MAX_KELVIN - CONFIG_T8_CCT_MIN_KELVIN));
}

static uint8_t hue_degrees_to_byte(uint16_t degrees)
{
    if (degrees > 359) {
        degrees = 359;
    }
    return (uint8_t)((degrees * 255UL) / 359);
}

/* Matter tops out at 254; the fixture's channels run to 255. */
static uint8_t matter_to_byte(uint8_t matter_value)
{
    if (matter_value >= T8_MATTER_MAX) {
        return 255;
    }
    return (uint8_t)(((uint32_t)matter_value * 255U + T8_MATTER_MAX / 2) / T8_MATTER_MAX);
}

static uint8_t band_mid(const t8_band_t *band)
{
    return (uint8_t)((band->lo + band->hi) / 2);
}

/* ------------------------------------------------------------------- DMX plumbing --- */

/* The fixture mutex guards the state model; the DMX batch lock makes the resulting run of
 * slot writes land in a single frame. Always taken in this order. */
static void fixture_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    dmx512_lock();
}

static void fixture_unlock(void)
{
    dmx512_unlock();
    xSemaphoreGive(s_lock);
}

/* CH numbers are 1-based within the personality. Callers hold s_lock. */
static void set_ch(uint16_t channel, uint8_t value)
{
    dmx512_set_slot(s_start_address + channel - 1, value);
}

/* CH3..CH43 mean completely different things in each mode, so a stale byte from the
 * previous mode is read back as a garbage parameter (in the original project a leftover
 * saturation value showed up as AMBIENT: INT SHIFT). Always wipe them on a mode change. */
static void clear_params(void)
{
    dmx512_fill_slots(s_start_address + 2, 0, T8_FOOTPRINT - 2);
}

static void select_mode(t8_mode_t mode)
{
    if (mode != s_mode) {
        clear_params();
    }
    s_mode = mode;
    set_ch(2, band_mid(&MODES[mode]));
}

static void render_dimmer(void)
{
    uint8_t value = 0;
    if (s_identify_active) {
        value = s_identify_on ? matter_to_byte(s_identify_level) : 0;
    } else if (s_power) {
        value = matter_to_byte(s_level);
    }
    set_ch(1, value);
}

static void render_cct(void)
{
    select_mode(T8_MODE_CCT);
    set_ch(3, kelvin_to_byte(s_kelvin));
    set_ch(4, T8_GM_NEUTRAL);
    set_ch(5, 0); /* strobe off; CH5 in both CCT and HSI */
}

static void render_hsi(void)
{
    select_mode(T8_MODE_HSI);
    set_ch(3, hue_degrees_to_byte(s_hue_degrees));
    set_ch(4, s_saturation);
    set_ch(5, 0);
}

static void render_colour(void)
{
    if (s_mode == T8_MODE_HSI) {
        render_hsi();
    } else {
        render_cct();
    }
}

/* Repaints the whole personality block from the state model. */
static void render_all(void)
{
    render_colour();
    render_dimmer();
}

/* ----------------------------------------------------------------------- sweeps ----- */

static void sweep_apply(void)
{
    dmx512_fill_slots(1, 0, DMX_MAX_CHANNEL);
    if (s_sweep_mode == SWEEP_HUNT) {
        uint16_t top = s_sweep_pos + SWEEP_HUNT_WINDOW - 1;
        if (top > DMX_MAX_CHANNEL) {
            top = DMX_MAX_CHANNEL;
        }
        dmx512_fill_slots(s_sweep_pos, 255, top - s_sweep_pos + 1);
    } else if (s_sweep_mode == SWEEP_WALK) {
        dmx512_set_slot(s_sweep_pos, 255);
    }
}

static void sweep_stop_locked(void)
{
    s_sweep_mode = SWEEP_OFF;
    dmx512_fill_slots(1, 0, DMX_MAX_CHANNEL);
    render_all();
}

/* Advanced once per frame from the DMX task. */
static void sweep_service_locked(void)
{
    if (s_sweep_mode == SWEEP_OFF) {
        return;
    }
    const int64_t dwell = (s_sweep_mode == SWEEP_HUNT) ? SWEEP_HUNT_DWELL_MS : SWEEP_WALK_DWELL_MS;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_sweep_stamp_ms < dwell) {
        return;
    }
    s_sweep_stamp_ms = now_ms;
    s_sweep_pos += (s_sweep_mode == SWEEP_HUNT) ? SWEEP_HUNT_WINDOW : 1;
    if (s_sweep_pos > s_sweep_hi) {
        ESP_LOGI(TAG, "sweep finished");
        sweep_stop_locked();
        return;
    }
    ESP_LOGI(TAG, "sweep at channel %u", s_sweep_pos);
    sweep_apply();
}

/* ------------------------------------------------------------------- frame hook ----- */

static void fixture_frame_hook(void)
{
    fixture_lock();
    sweep_service_locked();
    if (s_identify_active && s_sweep_mode == SWEEP_OFF) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        const bool on = ((now_ms / IDENTIFY_HALF_PERIOD_MS) % 2) == 0;
        if (on != s_identify_on) {
            s_identify_on = on;
            render_dimmer();
        }
    }
    fixture_unlock();
}

/* ---------------------------------------------------------------------- NVS ---------- */

static uint16_t load_start_address(void)
{
    nvs_handle_t handle;
    uint16_t address = CONFIG_T8_DMX_START_ADDRESS;

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No stored DMX address, using default %u", address);
        return address;
    }
    if (nvs_get_u16(handle, NVS_KEY_ADDR, &address) != ESP_OK) {
        address = CONFIG_T8_DMX_START_ADDRESS;
    }
    nvs_close(handle);
    return address;
}

static esp_err_t store_start_address(uint16_t address)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u16(handle, NVS_KEY_ADDR, address);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/* ---------------------------------------------------------------- public API -------- */

esp_err_t fixture_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    s_start_address = load_start_address();
    if (s_start_address < 1 || s_start_address + T8_FOOTPRINT - 1 > DMX_MAX_CHANNEL) {
        ESP_LOGW(TAG, "Stored DMX address %u does not fit a %d-channel personality, using %d",
                 s_start_address, T8_FOOTPRINT, CONFIG_T8_DMX_START_ADDRESS);
        s_start_address = CONFIG_T8_DMX_START_ADDRESS;
    }

    esp_err_t err = dmx512_init();
    if (err != ESP_OK) {
        return err;
    }

    /* Park the fixture in a defined state until Matter restores the persisted values. */
    fixture_lock();
    render_all();
    fixture_unlock();

    dmx512_register_frame_hook(fixture_frame_hook);

    ESP_LOGI(TAG, "T8-7X at DMX address %u, %d channels", s_start_address, T8_FOOTPRINT);
    return ESP_OK;
}

uint16_t fixture_get_start_address(void)
{
    return s_start_address;
}

esp_err_t fixture_set_start_address(uint16_t address)
{
    if (address < 1 || address + T8_FOOTPRINT - 1 > DMX_MAX_CHANNEL) {
        return ESP_ERR_INVALID_ARG;
    }

    fixture_lock();
    /* Leave no stale block behind at the old address. */
    dmx512_fill_slots(s_start_address, 0, T8_FOOTPRINT);
    s_start_address = address;
    dmx512_fill_slots(s_start_address, 0, T8_FOOTPRINT);
    render_all();
    fixture_unlock();

    esp_err_t err = store_start_address(address);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist DMX address: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "DMX start address is now %u", address);
    return err;
}

void fixture_set_power(bool on)
{
    fixture_lock();
    s_power = on;
    render_dimmer();
    fixture_unlock();
}

void fixture_set_level(uint8_t matter_level)
{
    fixture_lock();
    s_level = matter_level;
    render_dimmer();
    fixture_unlock();
}

void fixture_set_cct_kelvin(uint32_t kelvin)
{
    fixture_lock();
    s_kelvin = kelvin;
    /* A mode change wipes CH3..CH43; CH1 is outside that range and survives. */
    render_cct();
    fixture_unlock();
}

void fixture_set_hsi(uint16_t hue_degrees, uint8_t saturation_255)
{
    fixture_lock();
    s_hue_degrees = hue_degrees > 359 ? 359 : hue_degrees;
    s_saturation = saturation_255;
    render_hsi();
    fixture_unlock();
}

void fixture_identify_start(void)
{
    fixture_lock();
    if (!s_identify_active) {
        /* Blink at the current level, or full if the fixture is off, so it is visible. */
        s_identify_level = (s_power && s_level > 0) ? s_level : T8_MATTER_MAX;
        s_identify_active = true;
        s_identify_on = true;
        render_dimmer();
    }
    fixture_unlock();
}

void fixture_identify_stop(void)
{
    fixture_lock();
    if (s_identify_active) {
        s_identify_active = false;
        render_dimmer();
    }
    fixture_unlock();
}

bool fixture_identify_active(void)
{
    return s_identify_active;
}

esp_err_t fixture_set_raw_channel(uint16_t channel, uint8_t value)
{
    if (channel < 1 || channel > T8_FOOTPRINT) {
        return ESP_ERR_INVALID_ARG;
    }
    fixture_lock();
    set_ch(channel, value);
    fixture_unlock();
    return ESP_OK;
}

esp_err_t fixture_get_channels(uint8_t *out, size_t count)
{
    if (!out || count == 0 || count > T8_FOOTPRINT) {
        return ESP_ERR_INVALID_ARG;
    }
    return dmx512_get_slots(s_start_address, out, count);
}

const char *fixture_mode_name(t8_mode_t mode)
{
    if (mode >= T8_MODE_COUNT) {
        return "?";
    }
    return MODES[mode].name;
}

t8_mode_t fixture_get_mode(void)
{
    return s_mode;
}

void fixture_sweep_hunt(void)
{
    fixture_lock();
    s_sweep_mode = SWEEP_HUNT;
    s_sweep_hi = DMX_MAX_CHANNEL;
    s_sweep_pos = 1;
    s_sweep_stamp_ms = esp_timer_get_time() / 1000;
    sweep_apply();
    fixture_unlock();
}

void fixture_sweep_walk(uint16_t lo, uint16_t hi)
{
    if (lo < 1) {
        lo = 1;
    }
    if (hi > DMX_MAX_CHANNEL) {
        hi = DMX_MAX_CHANNEL;
    }
    fixture_lock();
    s_sweep_mode = SWEEP_WALK;
    s_sweep_hi = hi;
    s_sweep_pos = lo;
    s_sweep_stamp_ms = esp_timer_get_time() / 1000;
    sweep_apply();
    fixture_unlock();
}

void fixture_sweep_stop(void)
{
    fixture_lock();
    sweep_stop_locked();
    fixture_unlock();
}

bool fixture_sweep_active(void)
{
    return s_sweep_mode != SWEEP_OFF;
}

uint16_t fixture_sweep_position(void)
{
    return s_sweep_pos;
}
