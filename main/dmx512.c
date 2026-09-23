/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "dmx512.h"

static const char *TAG = "dmx512";

#define DMX_UART_NUM        UART_NUM_1
#define DMX_BAUD_RATE       250000
#define DMX_FRAME_LEN       (DMX_MAX_CHANNEL + 1) /* start code + 512 slots */
/* uart_driver_install() requires an RX buffer larger than the 128-byte hardware FIFO even
 * though RX is unused here (it is only broken out for a possible future RDM listener). */
#define DMX_RX_BUF_SIZE     256
#define DMX_TX_BUF_SIZE     1024

/* Written by the Matter and console sides under s_lock, snapshotted into s_tx once per
 * frame. The original sketch shared one buffer lock-free, which is fine for single-byte
 * writes but not for the 41-byte clear-parameters transaction on a mode change. */
static uint8_t s_pending[DMX_FRAME_LEN];
static uint8_t s_tx[DMX_FRAME_LEN];
static SemaphoreHandle_t s_lock;
static dmx512_frame_hook_t s_frame_hook;
static bool s_initialized;

static void dmx512_frame(void)
{
    /* Wait until the previous frame has left the shifter, otherwise inverting the line
     * would corrupt its last bytes. */
    uart_wait_tx_done(DMX_UART_NUM, portMAX_DELAY);

    /* Idle-high inverts to low, which is the BREAK. Deliberately not inside a critical
     * section: preemption here only lengthens BREAK/MAB, which is legal, and it keeps the
     * ~120 us busy-wait from ever perturbing 802.15.4 timing. */
    uart_set_line_inverse(DMX_UART_NUM, UART_SIGNAL_TXD_INV);
    esp_rom_delay_us(CONFIG_T8_DMX_BREAK_US);
    uart_set_line_inverse(DMX_UART_NUM, 0);
    esp_rom_delay_us(CONFIG_T8_DMX_MAB_US);

    uart_write_bytes(DMX_UART_NUM, s_tx, DMX_FRAME_LEN);
}

static void dmx512_task(void *arg)
{
    while (true) {
        if (s_frame_hook) {
            s_frame_hook();
        }

        /* Snapshot under the lock, transmit without it. The write itself is buffered, but
         * the frame then takes ~22.6 ms to clock out and the next iteration waits for it;
         * no writer should ever be blocked behind that. */
        xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
        memcpy(s_tx, s_pending, DMX_FRAME_LEN);
        xSemaphoreGiveRecursive(s_lock);

        dmx512_frame();
        vTaskDelay(1);
    }
}

esp_err_t dmx512_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "Failed to create DMX mutex");
        return ESP_ERR_NO_MEM;
    }

    memset(s_pending, 0, sizeof(s_pending));
    memset(s_tx, 0, sizeof(s_tx));
    /* Slot 0 is the DMX start code and stays zero for a standard dimmer frame. */

    const uart_config_t uart_config = {
        .baud_rate = DMX_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_2, /* DMX512 is 8N2 */
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        /* 40 MHz / 250 k divides exactly, and XTAL does not move with dynamic frequency
         * scaling the way the default PLL source does. */
        .source_clk = UART_SCLK_XTAL,
    };

    esp_err_t err = uart_driver_install(DMX_UART_NUM, DMX_RX_BUF_SIZE, DMX_TX_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_param_config(DMX_UART_NUM, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_set_pin(DMX_UART_NUM, CONFIG_T8_DMX_TX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    /* DE and /RE are tied together on the Seeed breakout and are not driven automatically.
     * Held high, the transceiver is a permanent transmitter. */
    const gpio_config_t de_config = {
        .pin_bit_mask = 1ULL << CONFIG_T8_DMX_DE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&de_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DE gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
    gpio_set_level((gpio_num_t)CONFIG_T8_DMX_DE_GPIO, 1);

    if (xTaskCreate(dmx512_task, "dmx", 3072, NULL, CONFIG_T8_DMX_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create DMX task");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "DMX512 up: TX GPIO%d, DE GPIO%d, %d baud 8N2", CONFIG_T8_DMX_TX_GPIO,
             CONFIG_T8_DMX_DE_GPIO, DMX_BAUD_RATE);
    return ESP_OK;
}

void dmx512_lock(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

void dmx512_unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

esp_err_t dmx512_register_frame_hook(dmx512_frame_hook_t hook)
{
    s_frame_hook = hook;
    return ESP_OK;
}

esp_err_t dmx512_set_slot(uint16_t channel, uint8_t value)
{
    return dmx512_set_slots(channel, &value, 1);
}

esp_err_t dmx512_set_slots(uint16_t first_channel, const uint8_t *values, size_t count)
{
    if (!values || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (first_channel < 1 || first_channel + count - 1 > DMX_MAX_CHANNEL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    memcpy(&s_pending[first_channel], values, count);
    xSemaphoreGiveRecursive(s_lock);
    return ESP_OK;
}

esp_err_t dmx512_fill_slots(uint16_t first_channel, uint8_t value, size_t count)
{
    if (count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (first_channel < 1 || first_channel + count - 1 > DMX_MAX_CHANNEL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    memset(&s_pending[first_channel], value, count);
    xSemaphoreGiveRecursive(s_lock);
    return ESP_OK;
}

uint8_t dmx512_get_slot(uint16_t channel)
{
    uint8_t value = 0;
    dmx512_get_slots(channel, &value, 1);
    return value;
}

esp_err_t dmx512_get_slots(uint16_t first_channel, uint8_t *out, size_t count)
{
    if (!out || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (first_channel < 1 || first_channel + count - 1 > DMX_MAX_CHANNEL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    memcpy(out, &s_pending[first_channel], count);
    xSemaphoreGiveRecursive(s_lock);
    return ESP_OK;
}
