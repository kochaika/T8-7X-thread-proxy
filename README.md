# T8-7X-thread-proxy

Matter-over-Thread control of a **Nanlite PavoTube T8-7X** from a Seeed XIAO ESP32-C6,
speaking DMX512 to the fixture.

The firmware is a protocol proxy: Matter cluster attributes in, DMX512 frames out. It
appears to Apple Home / Google Home / Home Assistant as an **Extended Color Light**, and
drives only the fixture's **CCT** and **HSI** engines — RGBW, EFFECT and PIXEL FX are not
exposed.

Hardware and wiring are unchanged from [T8-7X-DMX](https://github.com/kochaika/T8-7X-DMX),
which is where the fixture's channel map was reverse-engineered and confirmed against a real
tube. That project's Wi-Fi web console is gone; Thread replaces it.

---

## Hardware

| Part | Notes |
|---|---|
| Seeed **XIAO ESP32C6** | Single RISC-V core, Wi-Fi 6 + 802.15.4. Wi-Fi is compiled out here. USB-C is native USB, so the console does not consume a UART. |
| Seeed **RS485 Breakout Board for XIAO** | Carrier board, TP8485E transceiver. The XIAO plugs straight in. |
| Nanlite **USB-C to DMX Cable** | CB-DMX-USBC-1/3II. The T8-7X has no DMX port; this cable adds one. |
| Nanlite **PavoTube T8-7X** | In the **ULTIMATE DMX 8bit** personality (43 channels). |
| A Thread Border Router | HomePod mini, Apple TV 4K, Nest Hub (2nd gen), or an ESP Thread BR. |

The breakout is a carrier, so there is nothing to wire on the MCU side. These connections are
made by the PCB:

| XIAO pad | GPIO | TP8485E |
|---|---|---|
| `D4` | 22 | pin 4 `D` — driver input |
| `D5` | 23 | pin 1 `R` — receiver output, unused (reserved for RDM) |
| `D2` | 2 | pins 2 `/RE` + 3 `DE`, tied together |

**`DE` is not automatic on this board.** The firmware drives GPIO2 high in `dmx512_init()`.
Leave it low and nothing reaches the bus no matter how correct the UART is.

Signal terminal (J2) → the Nanlite cable's **male** 5-pin XLR (male is DMX IN; the female
tail is THRU and driving it puts you in contention with the fixture's own driver):

| Terminal | XLR pin |
|---|---|
| `A` (D+) | 3 — DATA + |
| `B` (D−) | 2 — DATA − |
| `GND` | 1 — signal ground |

Board switches: **120R off** (this board is the head of the chain; terminate at the fixture)
and **5V to IN**. Both are explained at length in the T8-7X-DMX README.

### Antenna

The XIAO ESP32-C6 has an FM8625H RF switch: **GPIO3** enables it, **GPIO14** selects the
onboard ceramic antenna (0) or the external U.FL connector (1). The firmware defaults to the
onboard antenna; set `CONFIG_T8_USE_EXTERNAL_ANTENNA=y` if a U.FL antenna is fitted. With the
switch set to external and no antenna attached, range collapses.

Neither GPIO3 nor GPIO14 may be repurposed.

---

## What Matter exposes

Endpoint 1 is an **Extended Color Light** (device type `0x010D`).

| Cluster | Feature | Notes |
|---|---|---|
| On/Off `0x0006` | Lighting | `StartUpOnOff` null — restores the last state |
| Level Control `0x0008` | OnOff, Lighting | `CurrentLevel` 1..254 |
| Color Control `0x0300` | **HS \| XY \| CT** (`FeatureMap` / `ColorCapabilities` = `0x0019`) | |
| Identify `0x0003` | | Blinks the tube at ~1 Hz |

XY is mandatory for this device type, but the fixture has no XY engine. An XY write is
converted to hue and saturation (`xy_to_rgb()` from the esp-matter device HAL, then a local
RGB→hue/sat) and sent on the **HSI** engine, so only CCT and HSI ever reach the DMX wire.

### Attribute → DMX mapping

Channels are relative to the DMX start address.

| Matter | DMX |
|---|---|
| `OnOff` false | CH1 = 0. Mode and colour channels are retained, so the tube comes back to the same look. |
| `OnOff` true | CH1 = last level |
| `CurrentLevel` (0..254) | CH1 = `level * 255 / 254` |
| `ColorTemperatureMireds` | `K = 1000000 / mireds`, clamped 2700..12000 → CH2 = 19 (CCT band), CH3 = `(K - 2700) * 255 / 9300`, CH4 = 132 (neutral green/magenta), CH5 = 0 (strobe off) |
| `CurrentHue` (0..254) | CH2 = 58 (HSI band), CH3 = `hue * 359 / 254`, then `deg * 255 / 359` |
| `CurrentSaturation` (0..254) | CH4 = `sat * 255 / 254` |
| `CurrentX` / `CurrentY` | converted to hue/saturation, then as above |

`ColorTempPhysicalMinMireds` = **83** (12000 K) and `ColorTempPhysicalMaxMireds` = **370**
(2700 K); controllers read these as the bounds of their colour-temperature slider.

CH3–CH43 mean different things in each mode, so **every mode change zeroes them** before
writing the new values. This is not hygiene: in the original project a leftover saturation
byte was reinterpreted by the fixture as `AMBIENT: INT SHIFT`.

Green/magenta tint and strobe are not exposed to Matter — the tint is held at neutral and the
strobe at off.

### Button

The XIAO's BOOT button (GPIO9, active low):

- **short press** — toggles the fixture; on turns it to 100 %
- **long press ≥ 5 s** — Matter factory reset

---

## Building

Requires ESP-IDF **v5.5.x** and this esp-matter checkout.

```bash
cd ~/esp-matter && source ./export.sh
export ESP_MATTER_PATH=~/esp-matter
export IDF_CCACHE_ENABLE=1

cd ~/esp-matter/T8-7X-thread-proxy
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/cu.usbmodem2101 flash monitor
```

To start over:

```bash
idf.py fullclean && rm -rf build/
esptool.py --chip esp32c6 --port /dev/cu.usbmodem2101 erase_flash
```

## Commissioning

Generate factory data, then flash it **before** commissioning:

```bash
esp-matter-mfg-tool -n 2 \
  -v 0xFFF1 -p 0x8002 \
  --vendor-name "ChaikaMatter" \
  --product-name "T8_7X_Proxy" \
  --hw-ver 1 --hw-ver-str "1.0"

esptool.py --chip esp32c6 --port /dev/cu.usbmodem2101 \
  write_flash 0x10000 out/fff1_8002/<uuid>/<uuid>-partition.bin
```

`CONFIG_CHIP_FACTORY_NAMESPACE_PARTITION_LABEL` is `"nvs"`, and `0x10000` **is** the `nvs`
partition — which is also OpenThread's storage and where fabrics live. Flashing the blob
wipes both, so it has to happen before you pair, not after.

The pairing QR code and manual code are in the generated `*-qrcode.png` and `*-onb_codes.csv`
under `out/`. Pair from Apple Home / Google Home, or:

```bash
chip-tool pairing ble-thread <node-id> hex:<thread-dataset> <passcode> <discriminator>
```

Attestation uses the CHIP example DAC (`CONFIG_EXAMPLE_DAC_PROVIDER`), which is fine for a
private fabric and not fine for certification.

The VID/PID in `sdkconfig.defaults.esp32c6`, the vendor/product names in
`main/CMakeLists.txt`, and the arguments to `esp-matter-mfg-tool` must all agree.

---

## Serial console

The fixture's personality and start address are stored **inside** the fixture and cannot be
read back over plain DMX — there is no display and no menu, and changing them needs the
WC-USBC-C1 wire controller, the NANLINK app, or RDM. So the firmware keeps a small console
on the USB serial port for discovering and setting the address.

| Command | |
|---|---|
| `addr [<n>]` | show, or set and persist, the DMX start address (default 100) |
| `dump` | print CH1–CH43 of the outgoing frame |
| `status` | address, mode, identify and sweep state |
| `dim <0-100>` | set the dimmer |
| `cct <kelvin>` | CCT mode |
| `hsi <deg 0-359> <sat 0-255>` | HSI mode |
| `raw <ch 1-43> <0-255>` | write one channel directly |
| `blackout` | dimmer to zero |
| `hunt` | sweep a 16-channel window across all 512 slots to locate the fixture |
| `walk <lo> <hi>` | step one channel at a time; the one that turns the light on is the dimmer |
| `stop` | abort a running sweep |

Console commands write straight to the DMX buffer and the next Matter update overrides them;
this is a bench tool, not a second control surface. Set `CONFIG_T8_ENABLE_CONSOLE=n` to drop
it.

Bench-testing the DMX path before involving Matter at all:

```
addr 100
dim 50
cct 3200
cct 9000
hsi 120 255
dim 0
```

Nothing happening? Check that DE (GPIO2) is high, that you fed the **male** XLR tail, and run
`hunt` then `walk <lo> <hi>` to rediscover the address.

---

## Configuration

`idf.py menuconfig` → **T8-7X Thread Proxy**:

| Option | Default |
|---|---|
| `T8_DMX_TX_GPIO` | 22 |
| `T8_DMX_DE_GPIO` | 2 |
| `T8_DMX_BREAK_US` | 100 (spec min 92) |
| `T8_DMX_MAB_US` | 20 (spec min 8) |
| `T8_DMX_TASK_PRIORITY` | 3 |
| `T8_DMX_START_ADDRESS` | 100 (NVS overrides it) |
| `T8_CCT_MIN_KELVIN` / `T8_CCT_MAX_KELVIN` | 2700 / 12000 |
| `T8_ENABLE_CONSOLE` | y |
| `T8_USE_EXTERNAL_ANTENNA` | n |

---

## Notes on the implementation

**The BREAK.** DMX needs a >92 µs low on an idle-high line, which a UART cannot send as data.
This firmware inverts the TX line for the duration — one register write
(`uart_set_line_inverse()`), no baud change, works with the driver running. The two common
alternatives both fail on this exact board:

- `esp_dmx` hangs in `dmx_driver_install()` on the XIAO ESP32-C6
  ([#219](https://github.com/someweisguy/esp_dmx/issues/219), closed "not planned").
- The baud-switch break (drop to ~83 k, send `0x00`, jump back) has a known
  [Wi-Fi interaction](https://github.com/espressif/arduino-esp32/issues/5940) and a
  [clock-source trap](https://github.com/espressif/arduino-esp32/issues/10641) where the
  switch silently fails.

The BREAK and mark-after-break are a busy-wait of about 120 µs per ~23 ms frame — 0.5 % duty
— and are deliberately **not** in a critical section. Preemption there only makes them
longer, which is legal, so the 802.15.4 radio's timing is never perturbed.

**Clock source.** UART1 runs from `UART_SCLK_XTAL`: 40 MHz divides exactly by 250 kbaud, and
XTAL does not move with dynamic frequency scaling the way the default PLL source does.

**Task model.** One FreeRTOS task at priority 3 (`CONFIG_T8_DMX_TASK_PRIORITY`) — above
ordinary application tasks so a busy Matter stack cannot stall the fixture, well below the
OpenThread and lwIP tasks so the radio always wins. `vTaskDelay(1)` between frames gives
~42 fps at `CONFIG_FREERTOS_HZ=1000`.

**Buffering.** Two 513-byte buffers. Writers fill `s_pending` under a recursive mutex; the
task snapshots it into `s_tx` and transmits with no lock held. A mode change takes the lock
across all of its writes, so the wipe of CH3–CH43 and the new colour values always land in
the same frame.

**Transitions.** `MoveToLevel`, `MoveToHueAndSaturation` and `MoveToColorTemperature` with a
transition time are handled inside the CHIP cluster servers, which emit a stream of attribute
updates. There is no fade engine here; the 42 fps refresh renders them smoothly.

**Thread, not Wi-Fi.** `CONFIG_ENABLE_WIFI_STATION=n`, OpenThread FTD (the fixture is mains
powered, so no sleepy-end-device / ICD configuration). On the C6 Wi-Fi and 802.15.4 share one
RF path, which is part of why the web console did not come across.
