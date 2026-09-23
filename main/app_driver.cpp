/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_log.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include <esp_matter.h>

#include <app_priv.h>
#include <button_gpio.h>
#include <color_format.h>
#include <device.h>
#include <iot_button.h>

#include "fixture_t8_7x.h"

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "app_driver";
extern uint16_t light_endpoint_id;

/* Hue and saturation arrive as separate attribute writes, and so do x and y, so each half
 * has to be remembered to reconstruct a complete colour. */
static uint16_t s_hue_degrees = 0;
static uint8_t s_saturation_255 = 0;
static uint16_t s_current_x = 0;
static uint16_t s_current_y = 0;

/* Non-NULL sentinel: the endpoint's priv_data is only used to prove a driver is present.
 * There is no per-instance driver state — the fixture module is a singleton. */
static int s_driver_sentinel;

/* --------------------------------------------------------------- unit conversions --- */

static uint16_t matter_hue_to_degrees(uint8_t hue)
{
    return (uint16_t)(((uint32_t)hue * 359U) / MATTER_HUE);
}

static uint8_t matter_saturation_to_byte(uint8_t saturation)
{
    if (saturation >= MATTER_SATURATION) {
        return 255;
    }
    return (uint8_t)(((uint32_t)saturation * 255U + MATTER_SATURATION / 2) / MATTER_SATURATION);
}

static uint32_t mireds_to_kelvin(uint16_t mireds)
{
    if (mireds == 0) {
        return CONFIG_T8_CCT_MAX_KELVIN;
    }
    return 1000000U / mireds;
}

/* The repo has no RGB->HSV anywhere, so this is the one piece of colour maths that is new.
 * Hue comes out in degrees, which is exactly what the fixture's HSI engine takes. */
static void rgb_to_hue_saturation(const RGB_color_t &rgb, uint16_t *hue_degrees, uint8_t *saturation_255)
{
    const uint8_t max = MAX(rgb.red, MAX(rgb.green, rgb.blue));
    const uint8_t min = MIN(rgb.red, MIN(rgb.green, rgb.blue));
    const uint8_t delta = (uint8_t)(max - min);

    *saturation_255 = max ? (uint8_t)(((uint32_t)delta * 255U) / max) : 0;

    if (delta == 0) {
        *hue_degrees = 0;
        return;
    }

    int32_t hue;
    if (max == rgb.red) {
        hue = 60 * ((int32_t)rgb.green - (int32_t)rgb.blue) / delta;
    } else if (max == rgb.green) {
        hue = 120 + 60 * ((int32_t)rgb.blue - (int32_t)rgb.red) / delta;
    } else {
        hue = 240 + 60 * ((int32_t)rgb.red - (int32_t)rgb.green) / delta;
    }
    if (hue < 0) {
        hue += 360;
    }
    *hue_degrees = (uint16_t)(hue % 360);
}

/* The fixture has no XY engine, so an XY write is resolved to a hue and saturation and sent
 * on the HSI engine. Brightness is passed as full scale so the result carries chromaticity
 * only — intensity stays with Level Control on CH1. */
static void apply_xy(uint16_t x, uint16_t y)
{
    XY_color_t xy = {x, y};
    RGB_color_t rgb = {0, 0, 0};
    xy_to_rgb(xy, 255, &rgb);
    rgb_to_hue_saturation(rgb, &s_hue_degrees, &s_saturation_255);
    fixture_set_hsi(s_hue_degrees, s_saturation_255);
}

/* ------------------------------------------------------------------ Matter -> DMX --- */

static esp_err_t app_driver_light_set_power(esp_matter_attr_val_t *val)
{
    fixture_set_power(val->val.b);
    return ESP_OK;
}

static esp_err_t app_driver_light_set_brightness(esp_matter_attr_val_t *val)
{
    fixture_set_level(val->val.u8);
    return ESP_OK;
}

static esp_err_t app_driver_light_set_hue(esp_matter_attr_val_t *val)
{
    s_hue_degrees = matter_hue_to_degrees(val->val.u8);
    fixture_set_hsi(s_hue_degrees, s_saturation_255);
    return ESP_OK;
}

static esp_err_t app_driver_light_set_saturation(esp_matter_attr_val_t *val)
{
    s_saturation_255 = matter_saturation_to_byte(val->val.u8);
    fixture_set_hsi(s_hue_degrees, s_saturation_255);
    return ESP_OK;
}

static esp_err_t app_driver_light_set_temperature(esp_matter_attr_val_t *val)
{
    fixture_set_cct_kelvin(mireds_to_kelvin(val->val.u16));
    return ESP_OK;
}

static void app_driver_button_toggle_cb(void *arg, void *data)
{
    ESP_LOGI(TAG, "Toggle button pressed");
    uint16_t endpoint_id = light_endpoint_id;

    attribute_t *on_off_attr = attribute::get(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id);
    esp_matter_attr_val_t val = esp_matter_invalid(NULL);
    attribute::get_val(on_off_attr, &val);
    bool is_on = val.val.b;

    /* Write through the data model rather than straight to DMX, so the fabric stays in sync. */
    if (is_on) {
        esp_matter_attr_val_t off_val = esp_matter_bool(false);
        attribute::update(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &off_val);
    } else {
        esp_matter_attr_val_t brightness_val = esp_matter_nullable_uint8(BRIGHTNESS_FULL);
        attribute::update(endpoint_id, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id,
                          &brightness_val);
        esp_matter_attr_val_t on_val = esp_matter_bool(true);
        attribute::update(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &on_val);
    }
}

esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle, uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id, esp_matter_attr_val_t *val)
{
    esp_err_t err = ESP_OK;
    if (endpoint_id != light_endpoint_id) {
        return err;
    }

    if (cluster_id == OnOff::Id) {
        if (attribute_id == OnOff::Attributes::OnOff::Id) {
            err = app_driver_light_set_power(val);
        }
    } else if (cluster_id == LevelControl::Id) {
        if (attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
            err = app_driver_light_set_brightness(val);
        }
    } else if (cluster_id == ColorControl::Id) {
        /* Which attribute was written picks the fixture's engine: hue, saturation and xy all
         * end up on HSI, colour temperature on CCT. ColorMode itself is not usable here —
         * this callback runs at PRE_UPDATE, before the cluster server has updated it. */
        if (attribute_id == ColorControl::Attributes::CurrentHue::Id) {
            err = app_driver_light_set_hue(val);
        } else if (attribute_id == ColorControl::Attributes::CurrentSaturation::Id) {
            err = app_driver_light_set_saturation(val);
        } else if (attribute_id == ColorControl::Attributes::ColorTemperatureMireds::Id) {
            err = app_driver_light_set_temperature(val);
        } else if (attribute_id == ColorControl::Attributes::CurrentX::Id) {
            s_current_x = val->val.u16;
            apply_xy(s_current_x, s_current_y);
        } else if (attribute_id == ColorControl::Attributes::CurrentY::Id) {
            s_current_y = val->val.u16;
            apply_xy(s_current_x, s_current_y);
        }
    }
    return err;
}

esp_err_t app_driver_light_set_defaults(uint16_t endpoint_id)
{
    esp_err_t err = ESP_OK;
    esp_matter_attr_val_t val = esp_matter_invalid(NULL);
    attribute_t *attribute = NULL;

    /* Colour first, then brightness, then power: the fixture should never briefly show the
     * wrong colour at the moment it comes up. Here, unlike in the update callback, ColorMode
     * is authoritative — Matter has started and the persisted value is loaded. */
    attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::ColorMode::Id);
    if (attribute) {
        attribute::get_val(attribute, &val);
        const uint8_t color_mode = val.val.u8;

        if (color_mode == (uint8_t)ColorControl::ColorMode::kCurrentHueAndCurrentSaturation) {
            attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentHue::Id);
            if (attribute) {
                attribute::get_val(attribute, &val);
                s_hue_degrees = matter_hue_to_degrees(val.val.u8);
            }
            attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentSaturation::Id);
            if (attribute) {
                attribute::get_val(attribute, &val);
                s_saturation_255 = matter_saturation_to_byte(val.val.u8);
            }
            fixture_set_hsi(s_hue_degrees, s_saturation_255);
        } else if (color_mode == (uint8_t)ColorControl::ColorMode::kCurrentXAndCurrentY) {
            attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentX::Id);
            if (attribute) {
                attribute::get_val(attribute, &val);
                s_current_x = val.val.u16;
            }
            attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentY::Id);
            if (attribute) {
                attribute::get_val(attribute, &val);
                s_current_y = val.val.u16;
            }
            apply_xy(s_current_x, s_current_y);
        } else {
            attribute = attribute::get(endpoint_id, ColorControl::Id,
                                       ColorControl::Attributes::ColorTemperatureMireds::Id);
            if (attribute) {
                attribute::get_val(attribute, &val);
                fixture_set_cct_kelvin(mireds_to_kelvin(val.val.u16));
            }
        }
    }

    attribute = attribute::get(endpoint_id, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id);
    if (attribute) {
        attribute::get_val(attribute, &val);
        err |= app_driver_light_set_brightness(&val);
    }

    attribute = attribute::get(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id);
    if (attribute) {
        attribute::get_val(attribute, &val);
        err |= app_driver_light_set_power(&val);
    }

    return err;
}

app_driver_handle_t app_driver_light_init()
{
    esp_err_t err = fixture_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize the DMX fixture driver: %s", esp_err_to_name(err));
        return NULL;
    }
    return (app_driver_handle_t)&s_driver_sentinel;
}

app_driver_handle_t app_driver_button_init()
{
    button_handle_t handle = NULL;
    const button_config_t btn_cfg = {0};
    button_gpio_config_t btn_gpio_cfg = button_driver_get_config();
    btn_gpio_cfg.gpio_num = BUTTON_GPIO;

    if (iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create the button device");
        return NULL;
    }
    iot_button_register_cb(handle, BUTTON_PRESS_DOWN, NULL, app_driver_button_toggle_cb, NULL);
    return (app_driver_handle_t)handle;
}
