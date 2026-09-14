# haro-firmware

ESP32-S3 firmware for **Haro**, a personal desk voice-assistant robot: wake
word detection, an animated Cozmo-style OLED face, a 7-LED mood ring, and a
WebSocket client that streams audio to [haro-server](https://github.com/nextmwa/haro-server)
for speech-to-text, an LLM reply, and text-to-speech.

## Hardware

Built for the **Waveshare ESP32-S3-AUDIO-Board** (ESP32-S3R8, 16MB flash,
8MB Octal PSRAM):

- ES8311 speaker codec + ES7210 mic array encoder (onboard I2S codecs)
- TCA9555 I2C GPIO expander (speaker amp enable, camera power -- unused by
  default)
- SSD1306 128x64 OLED display (I2C, shares the codec bus via the board's
  external header)
- 7x WS2812 addressable LEDs on GPIO38

## Prerequisites

- [ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/get-started/index.html),
  installed and exported (`. $HOME/esp/esp-idf/export.sh` or equivalent)
- A running instance of [haro-server](https://github.com/nextmwa/haro-server)
  reachable from the robot's WiFi network

## Build and flash

```
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

`<PORT>` is the board's USB serial device (e.g. `/dev/cu.usbmodem2101` on
macOS, `/dev/ttyUSB0` on Linux).

## First boot: WiFi + server setup

On first boot (or after erasing NVS), the device is unprovisioned and starts
a SoftAP named **`Haro-Setup`**. Connect to it and provision WiFi credentials
using the [Espressif SoftAP Provisioning app](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/provisioning/wifi_provisioning.html)
(iOS/Android) or ESP-IDF's `esp_prov.py` script.

The server address the firmware connects to defaults to the value baked in
at build time via Kconfig:

```
idf.py menuconfig
# -> Haro Config -> Default Haro server WebSocket URL
```

Set it to `ws://<haro-server-host>:8765` (port `8765` is haro-server's
default), then rebuild and reflash. To change it later without a full
rebuild, re-provision the device (erase the `nvs` partition and go through
SoftAP setup again):

```
esptool.py -p <PORT> erase-region 0x9000 0x6000
```

## Re-provisioning WiFi

Same as first boot: erase the `nvs` partition (see the command above) and
reconnect to the `Haro-Setup` SoftAP.

## Project layout

- `main/` -- app entry point, wiring, idle/gaze/blink behavior
- `components/audio_pipeline/` -- ES8311/ES7210 codec I/O
- `components/wake_word/` -- ESP-SR AFE + WakeNet "Hi,ESP" detection
- `components/face_display/` -- SSD1306 face rendering and animation
- `components/status_led/` -- WS2812 mood ring
- `components/server_client/` -- WebSocket client + protocol framing
- `components/orchestrator/` -- turn/state machine tying it together
- `components/wifi_provisioning/` -- SoftAP provisioning wrapper
- `components/haro_config/` -- Kconfig-backed runtime config
