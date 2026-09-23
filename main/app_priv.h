/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <esp_err.h>
#include <esp_matter.h>

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include "esp_openthread_types.h"
#endif

/** Matter max values (used for remapping attributes) */
#define MATTER_BRIGHTNESS 254
#define MATTER_HUE        254
#define MATTER_SATURATION 254

/** Default attribute values used during initialization */
#define DEFAULT_POWER      true
#define DEFAULT_BRIGHTNESS 254

/** Button toggles the fixture between OFF and full brightness */
#define BRIGHTNESS_FULL 254

/* The T8-7X spans 2700 K to 12000 K. Matter expresses colour temperature in mireds
 * (1000000 / kelvin), so the *minimum* mired value corresponds to the *maximum* kelvin.
 * Controllers read these two attributes as the bounds of their colour-temperature slider. */
#define T8_CT_MIN_MIREDS (1000000 / CONFIG_T8_CCT_MAX_KELVIN) /* 12000 K -> 83  */
#define T8_CT_MAX_MIREDS (1000000 / CONFIG_T8_CCT_MIN_KELVIN) /* 2700 K  -> 370 */
/* ~4000 K, used until the persisted StartUpColorTemperatureMireds takes over. */
#define T8_CT_DEFAULT_MIREDS 250

/* Built-in XIAO ESP32-C6 BOOT button (GPIO9), active low. Short press toggles the fixture,
 * long press (>= CONFIG_BUTTON_LONG_PRESS_TIME_MS) is a Matter factory reset. */
#define BUTTON_GPIO 9

/* FM8625H RF switch on the XIAO ESP32-C6. GPIO3 enables it; GPIO14 selects the onboard
 * ceramic antenna (0) or the external U.FL connector (1). Neither pin may be repurposed. */
#define RF_SWITCH_ENABLE_GPIO 3
#define RF_ANTENNA_SELECT_GPIO 14

typedef void *app_driver_handle_t;

/** Initialize the light driver: DMX transmitter plus the T8-7X fixture model. */
app_driver_handle_t app_driver_light_init();

/** Initialize the button driver. */
app_driver_handle_t app_driver_button_init();

/** Driver Update
 *
 * This API should be called to update the driver for the attribute being updated.
 * This is usually called from the common `app_attribute_update_cb()`.
 */
esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle, uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id, esp_matter_attr_val_t *val);

/** Push the data model's persisted values into the fixture after Matter has started. */
esp_err_t app_driver_light_set_defaults(uint16_t endpoint_id);

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()                                           \
    {                                                                                   \
        .radio_mode = RADIO_MODE_NATIVE,                                                \
    }

#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG()                                            \
    {                                                                                   \
        .host_connection_mode = HOST_CONNECTION_MODE_NONE,                              \
    }

#define ESP_OPENTHREAD_DEFAULT_PORT_CONFIG()                                            \
    {                                                                                   \
        .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10, \
    }
#endif
