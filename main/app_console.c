/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_console.h>
#include <esp_log.h>

#include "app_console.h"
#include "dmx512.h"
#include "fixture_t8_7x.h"

#if CONFIG_T8_ENABLE_CONSOLE

static const char *TAG = "app_console";

/* Commands here write straight to the DMX buffer and bypass the Matter data model, so the
 * next Matter update will overwrite them. That is deliberate: this is a bench tool. */
static void warn_if_matter_owns_the_output(const char *what)
{
    ESP_LOGW(TAG, "%s set from the console; the next Matter update will override it", what);
}

static int parse_int(const char *text, long *out)
{
    char *end = NULL;
    const long value = strtol(text, &end, 10);
    if (end == text || *end != '\0') {
        return -1;
    }
    *out = value;
    return 0;
}

static int cmd_addr(int argc, char **argv)
{
    if (argc < 2) {
        printf("DMX start address is %u\n", fixture_get_start_address());
        return 0;
    }
    long address = 0;
    if (parse_int(argv[1], &address) != 0) {
        printf("usage: addr [<1-%d>]\n", DMX_MAX_CHANNEL - T8_FOOTPRINT + 1);
        return 1;
    }
    if (fixture_set_start_address((uint16_t)address) != ESP_OK) {
        printf("address must leave room for %d channels (1-%d)\n", T8_FOOTPRINT,
               DMX_MAX_CHANNEL - T8_FOOTPRINT + 1);
        return 1;
    }
    printf("DMX start address is now %ld\n", address);
    return 0;
}

static int cmd_dump(int argc, char **argv)
{
    uint8_t channels[T8_FOOTPRINT];
    if (fixture_get_channels(channels, sizeof(channels)) != ESP_OK) {
        printf("failed to read the frame\n");
        return 1;
    }
    printf("start address %u, mode %s\n", fixture_get_start_address(),
           fixture_mode_name(fixture_get_mode()));
    for (int i = 0; i < T8_FOOTPRINT; i++) {
        printf("CH%-3d %3u%s", i + 1, channels[i], ((i + 1) % 8 == 0) ? "\n" : "  ");
    }
    printf("\n");
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    printf("address  %u\n", fixture_get_start_address());
    printf("mode     %s\n", fixture_mode_name(fixture_get_mode()));
    printf("identify %s\n", fixture_identify_active() ? "active" : "idle");
    if (fixture_sweep_active()) {
        printf("sweep    running at channel %u\n", fixture_sweep_position());
    } else {
        printf("sweep    off\n");
    }
    return 0;
}

static int cmd_dim(int argc, char **argv)
{
    long percent = 0;
    if (argc < 2 || parse_int(argv[1], &percent) != 0) {
        printf("usage: dim <0-100>\n");
        return 1;
    }
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    /* Route through the state model so power and the Identify blink stay consistent. */
    fixture_set_power(percent > 0);
    fixture_set_level((uint8_t)((percent * T8_MATTER_MAX) / 100));
    warn_if_matter_owns_the_output("Dimmer");
    return 0;
}

static int cmd_cct(int argc, char **argv)
{
    long kelvin = 0;
    if (argc < 2 || parse_int(argv[1], &kelvin) != 0) {
        printf("usage: cct <%d-%d>\n", CONFIG_T8_CCT_MIN_KELVIN, CONFIG_T8_CCT_MAX_KELVIN);
        return 1;
    }
    fixture_set_cct_kelvin((uint32_t)kelvin);
    warn_if_matter_owns_the_output("Colour temperature");
    return 0;
}

static int cmd_hsi(int argc, char **argv)
{
    long degrees = 0;
    long saturation = 0;
    if (argc < 3 || parse_int(argv[1], &degrees) != 0 || parse_int(argv[2], &saturation) != 0) {
        printf("usage: hsi <deg 0-359> <sat 0-255>\n");
        return 1;
    }
    if (degrees < 0) {
        degrees = 0;
    }
    if (saturation < 0) {
        saturation = 0;
    }
    if (saturation > 255) {
        saturation = 255;
    }
    fixture_set_hsi((uint16_t)degrees, (uint8_t)saturation);
    warn_if_matter_owns_the_output("Hue/saturation");
    return 0;
}

static int cmd_raw(int argc, char **argv)
{
    long channel = 0;
    long value = 0;
    if (argc < 3 || parse_int(argv[1], &channel) != 0 || parse_int(argv[2], &value) != 0) {
        printf("usage: raw <ch 1-%d> <0-255>\n", T8_FOOTPRINT);
        return 1;
    }
    if (value < 0) {
        value = 0;
    }
    if (value > 255) {
        value = 255;
    }
    if (fixture_set_raw_channel((uint16_t)channel, (uint8_t)value) != ESP_OK) {
        printf("channel must be 1-%d (relative to the start address)\n", T8_FOOTPRINT);
        return 1;
    }
    return 0;
}

static int cmd_blackout(int argc, char **argv)
{
    fixture_set_power(false);
    warn_if_matter_owns_the_output("Power");
    return 0;
}

static int cmd_hunt(int argc, char **argv)
{
    printf("sweeping a window across all %d slots; watch the fixture, the window that\n", DMX_MAX_CHANNEL);
    printf("lights it contains the start address. Then narrow it down with `walk`.\n");
    fixture_sweep_hunt();
    return 0;
}

static int cmd_walk(int argc, char **argv)
{
    long lo = 0;
    long hi = 0;
    if (argc < 3 || parse_int(argv[1], &lo) != 0 || parse_int(argv[2], &hi) != 0) {
        printf("usage: walk <lo> <hi>\n");
        return 1;
    }
    printf("stepping one channel at a time; the one that turns the light on is the dimmer\n");
    fixture_sweep_walk((uint16_t)lo, (uint16_t)hi);
    return 0;
}

static int cmd_stop(int argc, char **argv)
{
    fixture_sweep_stop();
    printf("sweep stopped\n");
    return 0;
}

static const esp_console_cmd_t s_commands[] = {
    {.command = "addr", .help = "Show or set the DMX start address (persisted)", .func = cmd_addr},
    {.command = "dump", .help = "Print CH1-CH43 of the outgoing frame", .func = cmd_dump},
    {.command = "status", .help = "Print address, mode, identify and sweep state", .func = cmd_status},
    {.command = "dim", .help = "Set the dimmer: dim <0-100>", .func = cmd_dim},
    {.command = "cct", .help = "CCT mode: cct <kelvin>", .func = cmd_cct},
    {.command = "hsi", .help = "HSI mode: hsi <deg 0-359> <sat 0-255>", .func = cmd_hsi},
    {.command = "raw", .help = "Write one channel: raw <ch 1-43> <0-255>", .func = cmd_raw},
    {.command = "blackout", .help = "Dimmer to zero", .func = cmd_blackout},
    {.command = "hunt", .help = "Sweep a 16-channel window to locate the fixture", .func = cmd_hunt},
    {.command = "walk", .help = "Step one channel at a time: walk <lo> <hi>", .func = cmd_walk},
    {.command = "stop", .help = "Abort a running hunt or walk", .func = cmd_stop},
};

esp_err_t app_console_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "t8>";
    repl_config.max_cmdline_length = 64;

    /* The XIAO's USB-C is native USB, so the console normally lives on USB-Serial-JTAG.
     * The UART path is kept for anyone who moves the console back to pins. */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl);
#else
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_uart(&hw_config, &repl_config, &repl);
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create the console REPL: %s", esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); i++) {
        err = esp_console_cmd_register(&s_commands[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register '%s': %s", s_commands[i].command, esp_err_to_name(err));
            return err;
        }
    }

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start the console REPL: %s", esp_err_to_name(err));
    }
    return err;
}

#else  /* CONFIG_T8_ENABLE_CONSOLE */

esp_err_t app_console_init(void)
{
    return ESP_OK;
}

#endif /* CONFIG_T8_ENABLE_CONSOLE */
