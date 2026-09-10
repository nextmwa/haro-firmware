# Haro ESP32-S3 Firmware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port Haro's voice-assistant behavior (wake word → capture → stream to the existing AI server → play the response → reflect state/emotion on a face display) from the Raspberry Pi Python client to firmware for the Waveshare ESP32-S3-AUDIO-Board, using official ESP-IDF/Espressif components wherever one exists instead of hand-rolled code.

**Architecture:** An ESP-IDF v6.1 component-per-concern project mirroring `haro/src/haro/*.py`'s module boundaries. Two FreeRTOS tasks (audio/wake-word, orchestrator) communicate over queues; `esp_websocket_client` runs its own task and bridges into the orchestrator's queue via an event handler. Pure-logic components (`protocol`, `orchestrator`) are dependency-injected via function-pointer op-structs so they can be unit-tested on ESP-IDF's Linux host target without hardware, mirroring the Protocol-based fakes the Python test suite already uses.

**Tech Stack:** ESP-IDF v6.1, target `esp32s3`, C. Official components: `espressif/esp_codec_dev`, `espressif/esp-sr`, `espressif/esp_io_expander_tca95xx_16bit`, `esp_websocket_client`, `espressif/network_provisioning`, ESP-IDF core `esp_lcd` (SSD1306), `cJSON`, NVS. Unity for host-side tests.

**Spec:** [docs/superpowers/specs/2026-09-10-esp32-s3-firmware-design.md](../specs/2026-09-10-esp32-s3-firmware-design.md)

## Global Constraints

- Target ESP-IDF **v6.1** only — no v5.x fallback code paths.
- Target chip: `esp32s3`.
- `server/` (the Python AI backend) and `haro/` (the Raspberry Pi client) are **not modified** by this plan.
- The wire protocol to `server/` is **unchanged**: JSON text frames for control messages, raw binary WS frames for PCM/TTS audio — exact schema in the spec's "Protocollo verso il server" section.
- Wake word: pre-trained WakeNet model **`hiesp`** ("Hi ESP") — no custom model training in this plan.
- Internal board pin mapping (verified from Waveshare's own `bsp_board.h`, not to be re-derived or guessed):
  I2C SCL=GPIO10, SDA=GPIO11 · I2S MCLK=GPIO12, BCLK=GPIO13, WS=GPIO14, DIN(mic)=GPIO15, DOUT(speaker)=GPIO16.
- `esp-sr` dependency must resolve to **`>=2.5.0`** (the IDF v6.1 Kconfig fix) — do not pin an exact older version.
- Every component that has an official ESP-IDF/Espressif implementation must use it; only `orchestrator` (Haro's own business logic) is hand-written from scratch.

---

## File Structure

```
esp32-firmware/
├── CMakeLists.txt                          (project root)
├── partitions.csv                          (adds the "model" SPIFFS partition for WakeNet)
├── sdkconfig.defaults
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml                   (registry deps: esp_codec_dev, esp-sr, esp_io_expander_tca95xx_16bit, network_provisioning)
│   └── main.c                              (app_main: wiring, task creation)
└── components/
    ├── protocol/
    │   ├── CMakeLists.txt
    │   ├── include/protocol.h
    │   ├── protocol.c
    │   └── test/test_protocol.c
    ├── haro_config/
    │   ├── CMakeLists.txt
    │   ├── include/haro_config.h
    │   └── haro_config.c
    ├── audio_pipeline/
    │   ├── CMakeLists.txt
    │   ├── include/audio_pipeline.h
    │   └── audio_pipeline.c
    ├── wake_word/
    │   ├── CMakeLists.txt
    │   ├── include/wake_word.h
    │   └── wake_word.c
    ├── server_client/
    │   ├── CMakeLists.txt
    │   ├── include/server_client.h
    │   └── server_client.c
    ├── face_display/
    │   ├── CMakeLists.txt
    │   ├── include/face_display.h
    │   └── face_display.c
    ├── wifi_provisioning/
    │   ├── CMakeLists.txt
    │   ├── include/haro_wifi_provisioning.h
    │   └── haro_wifi_provisioning.c
    └── orchestrator/
        ├── CMakeLists.txt
        ├── include/orchestrator.h
        ├── orchestrator.c
        └── test/test_orchestrator.c
```

Each component owns one Python module's responsibility (see the spec's mapping table). `protocol` and `orchestrator` get a `test/` directory because they run on the Linux host target; the rest are hardware-dependent and validated manually on the board (same reasoning as `haro/`'s own test suite, which only unit-tests the parts that don't touch real hardware).

---

### Task 1: Project scaffolding + Linux host test harness

**Files:**
- Create: `esp32-firmware/CMakeLists.txt`
- Create: `esp32-firmware/main/CMakeLists.txt`
- Create: `esp32-firmware/main/main.c`
- Create: `esp32-firmware/sdkconfig.defaults`
- Create: `esp32-firmware/.gitignore`

**Interfaces:**
- Produces: a buildable, flashable "hello world" ESP-IDF v6.1 project for `esp32s3`, and a working `idf.py --preview set-target linux` host-test flow that later tasks' `test/` directories plug into.

- [ ] **Step 1: Create the project**

```bash
cd /Users/leonardo/Progetti/Haro/esp32-firmware
idf.py create-project --path . haro_firmware
```

This scaffolds `CMakeLists.txt`, `main/CMakeLists.txt`, `main/main.c` (adjust/overwrite `main.c` per below).

- [ ] **Step 2: Set the target**

```bash
idf.py set-target esp32s3
```

Confirm `sdkconfig` now has `CONFIG_IDF_TARGET="esp32s3"`.

- [ ] **Step 3: Replace `main/main.c` with a minimal placeholder**

```c
#include "esp_log.h"

static const char *TAG = "haro";

void app_main(void)
{
    ESP_LOGI(TAG, "Haro firmware starting");
}
```

- [ ] **Step 4: Build for the target and confirm it succeeds**

Run: `idf.py build`
Expected: `Project build complete.` with no errors.

- [ ] **Step 5: Add `.gitignore` for ESP-IDF build artifacts**

```
build/
sdkconfig.old
managed_components/
dependencies.lock
```

- [ ] **Step 6: Verify the Linux host target works for future component tests**

```bash
idf.py --preview set-target linux
idf.py build
```

Expected: builds clean (nothing to test yet — this just proves the toolchain path works before later tasks depend on it).

```bash
idf.py set-target esp32s3
```

Switch back — `set-target` wipes `sdkconfig`/build dir, so re-set to the real target before continuing hardware work.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "chore: scaffold ESP-IDF v6.1 project for esp32s3"
```

---

### Task 2: `protocol` component (TDD, Linux host target)

**Files:**
- Create: `components/protocol/CMakeLists.txt`
- Create: `components/protocol/include/protocol.h`
- Create: `components/protocol/protocol.c`
- Create: `components/protocol/test/test_protocol.c` (test-only component, see step 1)

**Interfaces:**
- Produces (used by `server_client` and `orchestrator` in later tasks):
  ```c
  typedef enum {
      PROTOCOL_EVENT_EMOTION,
      PROTOCOL_EVENT_RESPONSE_END,
      PROTOCOL_EVENT_ERROR,
  } protocol_event_type_t;

  typedef struct {
      protocol_event_type_t type;
      char value[32];    // emotion value, e.g. "happy" — only valid when type == PROTOCOL_EVENT_EMOTION
      char message[128]; // error message — only valid when type == PROTOCOL_EVENT_ERROR
  } protocol_event_t;

  char *protocol_encode_hello(const char *session_id);          // caller must free()
  char *protocol_encode_end_of_speech(void);                    // caller must free()
  esp_err_t protocol_parse_server_message(const char *text, protocol_event_t *out);
  ```

This mirrors `haro/src/haro/protocol.py` exactly: `encode_hello`, `encode_end_of_speech`, `parse_server_message`, and the four server event shapes (binary audio chunks are **not** part of this component — `server_client` distinguishes text vs binary WS frames itself via `op_code`, per the spec).

- [ ] **Step 1: Create the test-only component structure**

`components/protocol/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "protocol.c"
    INCLUDE_DIRS "include"
    REQUIRES json
)

if(CONFIG_IDF_TARGET_LINUX)
    idf_component_register(
        SRC_DIRS "." "test"
        INCLUDE_DIRS "include"
        REQUIRES json unity
    )
endif()
```

Note: `idf_component_register` can only be called once per component — replace the two-call sketch above with a single conditional `SRC_DIRS`/`REQUIRES` selection:
```cmake
if(CONFIG_IDF_TARGET_LINUX)
    set(srcs "protocol.c" "test/test_protocol.c")
    set(reqs json unity)
else()
    set(srcs "protocol.c")
    set(reqs json)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES ${reqs}
)
```

- [ ] **Step 2: Write the header**

`components/protocol/include/protocol.h`:
```c
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROTOCOL_EVENT_EMOTION,
    PROTOCOL_EVENT_RESPONSE_END,
    PROTOCOL_EVENT_ERROR,
} protocol_event_type_t;

typedef struct {
    protocol_event_type_t type;
    char value[32];
    char message[128];
} protocol_event_t;

char *protocol_encode_hello(const char *session_id);
char *protocol_encode_end_of_speech(void);
esp_err_t protocol_parse_server_message(const char *text, protocol_event_t *out);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Write the failing tests**

`components/protocol/test/test_protocol.c`:
```c
#include "unity.h"
#include "protocol.h"
#include "cJSON.h"
#include <string.h>

TEST_CASE("encode_hello produces the expected JSON", "[protocol]")
{
    char *json = protocol_encode_hello("haro-session");
    cJSON *root = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_STRING("hello", cJSON_GetObjectItem(root, "type")->valuestring);
    TEST_ASSERT_EQUAL_STRING("haro-session", cJSON_GetObjectItem(root, "session_id")->valuestring);
    cJSON_Delete(root);
    free(json);
}

TEST_CASE("encode_end_of_speech produces the expected JSON", "[protocol]")
{
    char *json = protocol_encode_end_of_speech();
    cJSON *root = cJSON_Parse(json);
    TEST_ASSERT_EQUAL_STRING("end_of_speech", cJSON_GetObjectItem(root, "type")->valuestring);
    cJSON_Delete(root);
    free(json);
}

TEST_CASE("parse_server_message decodes an emotion event", "[protocol]")
{
    protocol_event_t evt;
    esp_err_t err = protocol_parse_server_message(
        "{\"type\": \"emotion\", \"value\": \"happy\"}", &evt);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(PROTOCOL_EVENT_EMOTION, evt.type);
    TEST_ASSERT_EQUAL_STRING("happy", evt.value);
}

TEST_CASE("parse_server_message decodes a response_end event", "[protocol]")
{
    protocol_event_t evt;
    esp_err_t err = protocol_parse_server_message(
        "{\"type\": \"response_end\"}", &evt);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(PROTOCOL_EVENT_RESPONSE_END, evt.type);
}

TEST_CASE("parse_server_message decodes an error event", "[protocol]")
{
    protocol_event_t evt;
    esp_err_t err = protocol_parse_server_message(
        "{\"type\": \"error\", \"message\": \"boom\"}", &evt);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(PROTOCOL_EVENT_ERROR, evt.type);
    TEST_ASSERT_EQUAL_STRING("boom", evt.message);
}

TEST_CASE("parse_server_message rejects invalid JSON", "[protocol]")
{
    protocol_event_t evt;
    esp_err_t err = protocol_parse_server_message("not json", &evt);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

TEST_CASE("parse_server_message rejects an unknown type", "[protocol]")
{
    protocol_event_t evt;
    esp_err_t err = protocol_parse_server_message("{\"type\": \"mystery\"}", &evt);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}
```

- [ ] **Step 4: Create a minimal Linux-target test app that pulls this component in**

`main/CMakeLists.txt` needs `set(COMPONENTS main protocol)` guarded for the linux target per the host-testing docs (see Task 1's research: a Linux-target app must list `COMPONENTS` explicitly to skip auto-including every IDF component). Add near the top of the root `CMakeLists.txt`:

```cmake
if(IDF_TARGET STREQUAL "linux")
    set(COMPONENTS main protocol)
endif()
```//placed BEFORE `include($ENV{IDF_PATH}/tools/cmake/project.cmake)`

- [ ] **Step 5: Run the tests and confirm they fail to build (protocol.c doesn't exist yet)**

```bash
idf.py --preview set-target linux
idf.py build
```
Expected: FAIL — linker/compile error, `protocol_encode_hello` undefined.

- [ ] **Step 6: Implement `protocol.c`**

```c
#include "protocol.h"
#include "cJSON.h"
#include <string.h>

char *protocol_encode_hello(const char *session_id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "session_id", session_id);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char *protocol_encode_end_of_speech(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "end_of_speech");
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

esp_err_t protocol_parse_server_message(const char *text, protocol_event_t *out)
{
    cJSON *root = cJSON_Parse(text);
    if (root == NULL || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    esp_err_t result = ESP_OK;

    if (strcmp(type->valuestring, "emotion") == 0) {
        cJSON *value = cJSON_GetObjectItem(root, "value");
        if (!cJSON_IsString(value)) {
            result = ESP_ERR_INVALID_ARG;
        } else {
            out->type = PROTOCOL_EVENT_EMOTION;
            strncpy(out->value, value->valuestring, sizeof(out->value) - 1);
        }
    } else if (strcmp(type->valuestring, "response_end") == 0) {
        out->type = PROTOCOL_EVENT_RESPONSE_END;
    } else if (strcmp(type->valuestring, "error") == 0) {
        cJSON *message = cJSON_GetObjectItem(root, "message");
        out->type = PROTOCOL_EVENT_ERROR;
        if (cJSON_IsString(message)) {
            strncpy(out->message, message->valuestring, sizeof(out->message) - 1);
        }
    } else {
        result = ESP_ERR_INVALID_ARG;
    }

    cJSON_Delete(root);
    return result;
}
```

- [ ] **Step 7: Run the tests and confirm they pass**

```bash
idf.py build
idf.py monitor   # Unity test runner output on the Linux target executable
```
Expected: all 7 `[protocol]` test cases PASS.

- [ ] **Step 8: Commit**

```bash
git add components/protocol CMakeLists.txt
git commit -m "feat: add protocol component with host-side unit tests"
```

---

### Task 3: `haro_config` component (NVS)

**Files:**
- Create: `components/haro_config/CMakeLists.txt`
- Create: `components/haro_config/include/haro_config.h`
- Create: `components/haro_config/haro_config.c`

**Interfaces:**
- Produces (used by `main.c` and `server_client`):
  ```c
  esp_err_t haro_config_init(void);                              // calls nvs_flash_init()
  esp_err_t haro_config_get_server_url(char *out, size_t out_len); // reads "server_url" key, "haro" namespace
  esp_err_t haro_config_set_server_url(const char *url);
  ```

This is the NVS-backed replacement for the Pi's `config.json` (`server_url` is the only field the Python `Config` class requires without a default — see `haro/src/haro/config.py`).

- [ ] **Step 1: Write the header**

`components/haro_config/include/haro_config.h`:
```c
#pragma once
#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t haro_config_init(void);
esp_err_t haro_config_get_server_url(char *out, size_t out_len);
esp_err_t haro_config_set_server_url(const char *url);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Implement using the NVS C API**

`components/haro_config/haro_config.c`:
```c
#include "haro_config.h"
#include "nvs_flash.h"
#include "nvs.h"

#define NVS_NAMESPACE "haro"
#define KEY_SERVER_URL "server_url"

esp_err_t haro_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t haro_config_get_server_url(char *out, size_t out_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_str(handle, KEY_SERVER_URL, out, &out_len);
    nvs_close(handle);
    return err;
}

esp_err_t haro_config_set_server_url(const char *url)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, KEY_SERVER_URL, url);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
```

`components/haro_config/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "haro_config.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
)
```

- [ ] **Step 3: Build for esp32s3 to confirm it compiles**

```bash
idf.py set-target esp32s3
idf.py build
```
Expected: clean build (this component isn't wired into `main.c` yet — later tasks do that; this step only proves it compiles standalone).

- [ ] **Step 4: Commit**

```bash
git add components/haro_config
git commit -m "feat: add NVS-backed haro_config component"
```

---

### Task 4: `audio_pipeline` component (ES8311 + ES7210 via esp_codec_dev)

**Files:**
- Create: `components/audio_pipeline/CMakeLists.txt`
- Create: `components/audio_pipeline/include/audio_pipeline.h`
- Create: `components/audio_pipeline/audio_pipeline.c`
- Modify: `main/idf_component.yml` (add `espressif/esp_codec_dev`)

**Interfaces:**
- Produces (used by `wake_word` for capture and `orchestrator`/`main.c` for playback):
  ```c
  esp_err_t audio_pipeline_init(void);                                   // I2C bus + I2S channel + both codec devs
  esp_err_t audio_pipeline_read(void *buf, size_t len, size_t *bytes_read);   // mic capture, blocking
  esp_err_t audio_pipeline_write(const void *buf, size_t len);           // speaker playback, blocking
  esp_err_t audio_pipeline_set_out_volume(int volume);                   // 0-100
  ```

Ported from Waveshare's own `bsp_board.c`/`bsp_codec_adc_init`/`bsp_codec_dac_init` (verified source, not re-derived), using the pin constants from the Global Constraints section.

- [ ] **Step 1: Add the `esp_codec_dev` dependency**

`main/idf_component.yml` (create if it doesn't exist yet):
```yaml
dependencies:
  espressif/esp_codec_dev: ">=1.5.1"
```

- [ ] **Step 2: Write the header**

`components/audio_pipeline/include/audio_pipeline.h`:
```c
#pragma once
#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_pipeline_init(void);
esp_err_t audio_pipeline_read(void *buf, size_t len, size_t *bytes_read);
esp_err_t audio_pipeline_write(const void *buf, size_t len);
esp_err_t audio_pipeline_set_out_volume(int volume);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Implement, following the verified Waveshare init sequence**

`components/audio_pipeline/audio_pipeline.c`:
```c
#include "audio_pipeline.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

static const char *TAG = "audio_pipeline";

#define GPIO_I2C_SCL GPIO_NUM_10
#define GPIO_I2C_SDA GPIO_NUM_11
#define GPIO_I2S_MCLK GPIO_NUM_12
#define GPIO_I2S_BCLK GPIO_NUM_13
#define GPIO_I2S_WS   GPIO_NUM_14
#define GPIO_I2S_DIN  GPIO_NUM_15  // mic data in
#define GPIO_I2S_DOUT GPIO_NUM_16  // speaker data out

static i2c_master_bus_handle_t s_i2c_bus;
static i2s_chan_handle_t s_tx_handle;
static i2s_chan_handle_t s_rx_handle;
static esp_codec_dev_handle_t s_record_dev;
static esp_codec_dev_handle_t s_play_dev;

static esp_err_t init_i2c(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_I2C_SDA,
        .scl_io_num = GPIO_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    return i2c_new_master_bus(&bus_config, &s_i2c_bus);
}

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle);
    if (err != ESP_OK) return err;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(32, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_I2S_MCLK,
            .bclk = GPIO_I2S_BCLK,
            .ws   = GPIO_I2S_WS,
            .dout = GPIO_I2S_DOUT,
            .din  = GPIO_I2S_DIN,
        },
    };
    err = i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
    if (err != ESP_OK) return err;
    err = i2s_channel_init_std_mode(s_rx_handle, &std_cfg);
    if (err != ESP_OK) return err;
    err = i2s_channel_enable(s_tx_handle);
    if (err != ESP_OK) return err;
    return i2s_channel_enable(s_rx_handle);
}

static esp_err_t init_mic_codec(void)
{
    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_1, .rx_handle = s_rx_handle, .tx_handle = NULL };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = { .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c_bus };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = ctrl_if,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    const audio_codec_if_t *codec_if = es7210_codec_new(&es7210_cfg);

    esp_codec_dev_cfg_t dev_cfg = { .codec_if = codec_if, .data_if = data_if, .dev_type = ESP_CODEC_DEV_TYPE_IN };
    s_record_dev = esp_codec_dev_new(&dev_cfg);
    if (s_record_dev == NULL) return ESP_FAIL;

    esp_codec_dev_sample_info_t fs = { .sample_rate = 16000, .channel = 2, .bits_per_sample = 32 };
    return esp_codec_dev_open(s_record_dev, &fs);
}

static esp_err_t init_speaker_codec(void)
{
    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_1, .rx_handle = NULL, .tx_handle = s_tx_handle };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = { .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c_bus };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es8311_cfg = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .pa_pin = -1,        // no dedicated PA-enable GPIO on this board's ES8311 path (verified: Waveshare's own demo leaves this unmanaged)
        .use_mclk = false,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);

    esp_codec_dev_cfg_t dev_cfg = { .codec_if = codec_if, .data_if = data_if, .dev_type = ESP_CODEC_DEV_TYPE_OUT };
    s_play_dev = esp_codec_dev_new(&dev_cfg);
    if (s_play_dev == NULL) return ESP_FAIL;

    esp_codec_dev_sample_info_t fs = { .sample_rate = 16000, .channel = 1, .bits_per_sample = 16 };
    esp_codec_dev_set_out_vol(s_play_dev, 60);
    return esp_codec_dev_open(s_play_dev, &fs);
}

esp_err_t audio_pipeline_init(void)
{
    esp_err_t err = init_i2c();
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2c init failed: %s", esp_err_to_name(err)); return err; }
    err = init_i2s();
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2s init failed: %s", esp_err_to_name(err)); return err; }
    err = init_mic_codec();
    if (err != ESP_OK) { ESP_LOGE(TAG, "mic codec init failed: %s", esp_err_to_name(err)); return err; }
    return init_speaker_codec();
}

esp_err_t audio_pipeline_read(void *buf, size_t len, size_t *bytes_read)
{
    esp_err_t err = esp_codec_dev_read(s_record_dev, buf, len);
    if (bytes_read) *bytes_read = (err == ESP_OK) ? len : 0;
    return err;
}

esp_err_t audio_pipeline_write(const void *buf, size_t len)
{
    return esp_codec_dev_write(s_play_dev, (void *)buf, len);
}

esp_err_t audio_pipeline_set_out_volume(int volume)
{
    return esp_codec_dev_set_out_vol(s_play_dev, volume);
}
```

`components/audio_pipeline/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "audio_pipeline.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_codec_dev driver
)
```

- [ ] **Step 4: Wire a manual smoke test into `main.c` temporarily**

```c
#include "audio_pipeline.h"
ESP_ERROR_CHECK(audio_pipeline_init());
ESP_LOGI(TAG, "audio_pipeline initialized");
```

- [ ] **Step 5: Build and flash**

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```
Expected: log line `audio_pipeline initialized`, no `ESP_ERROR_CHECK` abort, no I2C/I2S errors in the log.

- [ ] **Step 6: Manual loopback test (mirrors the Pi's `overlays/loopback_test.sh`)**

Add a temporary block after init: read ~2s of mic audio into a buffer, then `audio_pipeline_write` it back out. Rebuild, flash, speak near the mic, confirm your voice plays back through the speaker. Remove this temporary test block once confirmed (it's not part of the final `main.c` — Task 10 replaces `main.c` wholesale).

- [ ] **Step 7: Commit**

```bash
git add components/audio_pipeline main/idf_component.yml
git commit -m "feat: add audio_pipeline component (ES8311+ES7210 via esp_codec_dev)"
```

---

### Task 5: `wake_word` component (ESP-SR AFE + WakeNet)

**Files:**
- Create: `components/wake_word/CMakeLists.txt`
- Create: `components/wake_word/include/wake_word.h`
- Create: `components/wake_word/wake_word.c`
- Modify: `main/idf_component.yml` (add `espressif/esp-sr`)
- Modify: `partitions.csv` (add the `model` SPIFFS partition)
- Create: `sdkconfig.defaults` additions for the custom partition table

**Interfaces:**
- Produces (used by `orchestrator` via a FreeRTOS queue):
  ```c
  typedef enum { WAKE_WORD_DETECTED, WAKE_WORD_SPEECH_END } wake_word_event_type_t;

  esp_err_t wake_word_start(QueueHandle_t event_queue);  // starts the feed+detect tasks
  ```
  Posts `wake_word_event_type_t` values to `event_queue` as the AFE reports `WAKENET_DETECTED` / VAD end-of-speech, adapted from Waveshare's `mic_speech.c` (dropping the MultiNet/command-recognition half — Haro doesn't do on-device command parsing, it streams to the server).

- [ ] **Step 1: Add the `esp-sr` dependency**

`main/idf_component.yml`, add:
```yaml
  espressif/esp-sr: ">=2.5.0"
```
(Per Global Constraints — do not pin to the older `^2.1.5` from Waveshare's own manifest; the Kconfig/IDF-v6.1 fix landed in 2.5.0.)

- [ ] **Step 2: Add the model partition**

`partitions.csv`:
```
# Name,     Type, SubType, Offset,   Size, Flags
nvs,        data,   nvs,      0x9000,       0x6000,
factory,    0,      0,        0x10000,      3M,
model,      data,   spiffs,   ,             5900K,
```

`sdkconfig.defaults`, add:
```
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
```

- [ ] **Step 3: Write the header**

`components/wake_word/include/wake_word.h`:
```c
#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WAKE_WORD_DETECTED,
} wake_word_event_type_t;

esp_err_t wake_word_start(QueueHandle_t event_queue);

#ifdef __cplusplus
}
#endif
```

(VAD/end-of-speech detection is folded into `orchestrator`'s own silence timer over the streamed audio in Task 8 — AFE's `vad_state` is available on the same `afe_fetch_result_t` if a tighter coupling turns out to be wanted later; starting with just wake-word detection here keeps this component's scope matched to its name.)

- [ ] **Step 4: Implement, adapted from Waveshare's `mic_speech.c` (verified source), dropping MultiNet**

```c
#include "wake_word.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "model_path.h"
#include "audio_pipeline.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "wake_word";
static esp_afe_sr_iface_t *s_afe_handle;
static QueueHandle_t s_event_queue;

static void feed_task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    int chunk_size = s_afe_handle->get_feed_chunksize(afe_data);
    int channels = s_afe_handle->get_feed_channel_num(afe_data);
    int16_t *buf = heap_caps_malloc(chunk_size * sizeof(int16_t) * channels, MALLOC_CAP_SPIRAM);

    while (true) {
        size_t bytes_read;
        audio_pipeline_read(buf, chunk_size * sizeof(int16_t) * channels, &bytes_read);
        s_afe_handle->feed(afe_data, buf);
    }
}

static void detect_task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    while (true) {
        afe_fetch_result_t *res = s_afe_handle->fetch(afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            ESP_LOGE(TAG, "AFE fetch error");
            continue;
        }
        if (res->wakeup_state == WAKENET_DETECTED) {
            wake_word_event_type_t evt = WAKE_WORD_DETECTED;
            xQueueSend(s_event_queue, &evt, 0);
        }
    }
}

esp_err_t wake_word_start(QueueHandle_t event_queue)
{
    s_event_queue = event_queue;

    srmodel_list_t *models = esp_srmodel_init("model");
    afe_config_t *afe_config = afe_config_init("MR", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    afe_config->wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "hiesp");
    afe_config->vad_init = true;

    s_afe_handle = esp_afe_handle_from_config(afe_config);
    esp_afe_sr_data_t *afe_data = s_afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);

    if (afe_data == NULL) {
        ESP_LOGE(TAG, "AFE init failed");
        return ESP_FAIL;
    }

    xTaskCreatePinnedToCore(detect_task, "wake_word_detect", 8192, afe_data, 5, NULL, 1);
    xTaskCreatePinnedToCore(feed_task, "wake_word_feed", 8192, afe_data, 5, NULL, 0);
    return ESP_OK;
}
```

**Flag for the implementer:** the input-format string passed to `afe_config_init` (`"MR"` above) and the exact `wakenet_model_name` filter call need to be cross-checked against whichever `esp-sr` version actually resolves (`>=2.5.0`) at build time — the `afe_config_t` shape was confirmed to have changed across the v2.0 refactor and wasn't independently re-verified for 2.5.x during planning (only the top-level `esp_afe_sr_iface_t`/`fetch`/`feed` shape was). Before writing this file for real, run `idf.py build` once with just `#include "esp_afe_sr_iface.h"` and inspect the resolved `managed_components/espressif__esp-sr/include/esp32s3/esp_afe_sr_iface.h` (and the sibling config header) for the current `afe_config_t` field names, rather than trusting the snippet above verbatim.

`components/wake_word/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "wake_word.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-sr audio_pipeline
)
```

- [ ] **Step 5: Build**

```bash
idf.py build
```
Fix any `afe_config_t` field-name mismatches found per the flag above before proceeding.

- [ ] **Step 6: Wire a manual test into `main.c` and flash**

```c
#include "wake_word.h"
QueueHandle_t wake_queue = xQueueCreate(4, sizeof(wake_word_event_type_t));
ESP_ERROR_CHECK(wake_word_start(wake_queue));

wake_word_event_type_t evt;
while (true) {
    if (xQueueReceive(wake_queue, &evt, portMAX_DELAY)) {
        ESP_LOGI(TAG, "wake word detected!");
    }
}
```

- [ ] **Step 7: Flash and say "Hi ESP" near the board**

```bash
idf.py flash monitor
```
Expected: `wake word detected!` logged each time you say it.

- [ ] **Step 8: Commit**

```bash
git add components/wake_word main/idf_component.yml partitions.csv sdkconfig.defaults
git commit -m "feat: add wake_word component (ESP-SR AFE + WakeNet hiesp)"
```

---

### Task 6: `server_client` component (`esp_websocket_client`)

**Files:**
- Create: `components/server_client/CMakeLists.txt`
- Create: `components/server_client/include/server_client.h`
- Create: `components/server_client/server_client.c`

**Interfaces:**
- Consumes: `protocol.h` (Task 2) — `protocol_encode_hello`, `protocol_encode_end_of_speech`, `protocol_parse_server_message`, `protocol_event_t`.
- Produces (used by `orchestrator` in Task 8):
  ```c
  typedef enum {
      SERVER_CLIENT_EVENT_PROTOCOL,   // .protocol_event is valid
      SERVER_CLIENT_EVENT_AUDIO,      // .audio_data/.audio_len are valid (caller must free .audio_data)
      SERVER_CLIENT_EVENT_DISCONNECTED,
  } server_client_event_type_t;

  typedef struct {
      server_client_event_type_t type;
      protocol_event_t protocol_event;
      uint8_t *audio_data;
      size_t audio_len;
  } server_client_event_t;

  esp_err_t server_client_init(const char *url, QueueHandle_t event_queue);
  esp_err_t server_client_send_hello(const char *session_id);
  esp_err_t server_client_send_audio_frame(const uint8_t *data, size_t len);
  esp_err_t server_client_send_end_of_speech(void);
  ```

Mirrors `haro/src/haro/server_client.py`: `esp_websocket_client_start()` handles the connect step; reconnection-with-backoff is driven by the `WEBSOCKET_EVENT_DISCONNECTED` handler re-arming a retry timer (the component's own auto-reconnect, configured via `reconnect_timeout_ms`, replaces the Python `connect_with_retry`'s manual backoff loop — this is the native mechanism, not a re-implementation of it).

- [ ] **Step 1: Add the dependency**

`main/idf_component.yml`, `esp_websocket_client` ships as part of `espressif/esp-protocols` — add:
```yaml
  espressif/esp_websocket_client: "*"
```

- [ ] **Step 2: Write the header**

`components/server_client/include/server_client.h`:
```c
#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "protocol.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SERVER_CLIENT_EVENT_PROTOCOL,
    SERVER_CLIENT_EVENT_AUDIO,
    SERVER_CLIENT_EVENT_DISCONNECTED,
} server_client_event_type_t;

typedef struct {
    server_client_event_type_t type;
    protocol_event_t protocol_event;
    uint8_t *audio_data;
    size_t audio_len;
} server_client_event_t;

esp_err_t server_client_init(const char *url, QueueHandle_t event_queue);
esp_err_t server_client_send_hello(const char *session_id);
esp_err_t server_client_send_audio_frame(const uint8_t *data, size_t len);
esp_err_t server_client_send_end_of_speech(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Implement**

```c
#include "server_client.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "server_client";
static esp_websocket_client_handle_t s_client;
static QueueHandle_t s_event_queue;

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_DATA: {
        if (data->op_code == 0x02) { // binary: TTS audio chunk
            server_client_event_t evt = { .type = SERVER_CLIENT_EVENT_AUDIO };
            evt.audio_data = malloc(data->data_len);
            memcpy(evt.audio_data, data->data_ptr, data->data_len);
            evt.audio_len = data->data_len;
            xQueueSend(s_event_queue, &evt, 0);
        } else if (data->op_code == 0x01) { // text: JSON control message
            server_client_event_t evt = { .type = SERVER_CLIENT_EVENT_PROTOCOL };
            char *text = malloc(data->data_len + 1);
            memcpy(text, data->data_ptr, data->data_len);
            text[data->data_len] = '\0';
            if (protocol_parse_server_message(text, &evt.protocol_event) == ESP_OK) {
                xQueueSend(s_event_queue, &evt, 0);
            } else {
                ESP_LOGW(TAG, "dropping unparseable server message: %s", text);
            }
            free(text);
        }
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED: {
        server_client_event_t evt = { .type = SERVER_CLIENT_EVENT_DISCONNECTED };
        xQueueSend(s_event_queue, &evt, 0);
        break;
    }
    default:
        break;
    }
}

esp_err_t server_client_init(const char *url, QueueHandle_t event_queue)
{
    s_event_queue = event_queue;

    esp_websocket_client_config_t config = {
        .uri = url,
        .reconnect_timeout_ms = 1000,
    };
    s_client = esp_websocket_client_init(&config);
    if (s_client == NULL) {
        return ESP_FAIL;
    }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    return esp_websocket_client_start(s_client);
}

esp_err_t server_client_send_hello(const char *session_id)
{
    char *json = protocol_encode_hello(session_id);
    int sent = esp_websocket_client_send_text(s_client, json, strlen(json), portMAX_DELAY);
    free(json);
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t server_client_send_audio_frame(const uint8_t *data, size_t len)
{
    int sent = esp_websocket_client_send_bin(s_client, (const char *)data, len, portMAX_DELAY);
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t server_client_send_end_of_speech(void)
{
    char *json = protocol_encode_end_of_speech();
    int sent = esp_websocket_client_send_text(s_client, json, strlen(json), portMAX_DELAY);
    free(json);
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}
```

**Flag for the implementer:** `WEBSOCKET_EVENT_ANY` convenience registration is assumed available (common ESP-IDF event-loop pattern); if `esp_websocket_register_events` in the resolved component version requires registering each event ID individually instead, split the single registration call into one per event (`WEBSOCKET_EVENT_DATA`, `WEBSOCKET_EVENT_DISCONNECTED`, etc.) with the same handler — check `esp_websocket_client.h` in `managed_components/` once it's fetched.

`components/server_client/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "server_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_websocket_client protocol
)
```

- [ ] **Step 4: Build**

```bash
idf.py build
```

- [ ] **Step 5: Manual test against the real `server/` backend**

Wire a temporary block in `main.c`: connect WiFi (hardcode credentials for this manual test only — Task 9 replaces this with real provisioning), call `server_client_init("ws://<server-ip>:8765", queue)`, `server_client_send_hello("test-session")`, then log whatever arrives on the queue. Flash, confirm the connection succeeds against a running `server/` instance and a `hello` round-trip doesn't error.

- [ ] **Step 6: Commit**

```bash
git add components/server_client main/idf_component.yml
git commit -m "feat: add server_client component (esp_websocket_client + protocol)"
```

---

### Task 7: `orchestrator` component (TDD, Linux host target)

**Files:**
- Create: `components/orchestrator/CMakeLists.txt`
- Create: `components/orchestrator/include/orchestrator.h`
- Create: `components/orchestrator/orchestrator.c`
- Create: `components/orchestrator/test/test_orchestrator.c`

**Interfaces:**
- Consumes: `protocol.h` (Task 2) for `protocol_event_t`.
- Produces (used by `main.c` in Task 10):
  ```c
  typedef enum { HARO_STATE_IDLE, HARO_STATE_LISTENING, HARO_STATE_THINKING, HARO_STATE_SPEAKING } haro_state_t;

  typedef struct {
      esp_err_t (*send_audio_frame)(void *ctx, const uint8_t *data, size_t len);
      esp_err_t (*send_end_of_speech)(void *ctx);
      void *ctx;
  } orchestrator_server_ops_t;

  typedef struct {
      void (*play_chunk)(void *ctx, const uint8_t *data, size_t len);
      void (*stop)(void *ctx);
      void *ctx;
  } orchestrator_audio_out_ops_t;

  typedef struct {
      void (*show)(void *ctx, int expression);  // expression values match face_display.h's enum, Task 8
      void *ctx;
  } orchestrator_face_ops_t;

  typedef struct {
      orchestrator_server_ops_t server;
      orchestrator_audio_out_ops_t audio_out;
      orchestrator_face_ops_t face;
  } orchestrator_ops_t;

  void orchestrator_init(orchestrator_ops_t ops);
  haro_state_t orchestrator_get_state(void);
  void orchestrator_on_wake_word(void);
  void orchestrator_on_audio_frame(const uint8_t *frame, size_t len, bool is_end_of_speech);
  void orchestrator_on_server_event(server_client_event_t event); // Task 6's type
  ```

This is a direct, faithful port of `orchestrator.py`'s state machine (`IDLE → LISTENING → THINKING → SPEAKING`, same triggers, same error/reconnect handling), dependency-injected the same way the Python version takes `Protocol`-typed fakes in `tests/test_orchestrator.py` — here via `orchestrator_ops_t`'s function pointers instead of duck typing.

- [ ] **Step 1: Set up the Linux-target test component (same pattern as Task 2)**

`components/orchestrator/CMakeLists.txt`:
```cmake
if(CONFIG_IDF_TARGET_LINUX)
    set(srcs "orchestrator.c" "test/test_orchestrator.c")
    set(reqs unity protocol server_client)
else()
    set(srcs "orchestrator.c")
    set(reqs protocol server_client)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES ${reqs}
)
```

Add `orchestrator` to the linux-target `COMPONENTS` list in the root `CMakeLists.txt` alongside `protocol` (from Task 2 Step 4).

- [ ] **Step 2: Write the header** (as specified in Interfaces above — create `components/orchestrator/include/orchestrator.h` verbatim from that block, plus `#include "server_client.h"` for `server_client_event_t`)

- [ ] **Step 3: Write the failing tests, porting the key cases from `haro/tests/test_orchestrator.py`**

`components/orchestrator/test/test_orchestrator.c`:
```c
#include "unity.h"
#include "orchestrator.h"
#include <string.h>

static char s_sent_audio[256];
static size_t s_sent_audio_len;
static bool s_end_of_speech_sent;
static char s_played_chunks[4][256];
static int s_played_count;
static int s_shown_expressions[16];
static int s_shown_count;

static esp_err_t fake_send_audio_frame(void *ctx, const uint8_t *data, size_t len)
{
    memcpy(s_sent_audio, data, len);
    s_sent_audio_len = len;
    return ESP_OK;
}

static esp_err_t fake_send_end_of_speech(void *ctx)
{
    s_end_of_speech_sent = true;
    return ESP_OK;
}

static void fake_play_chunk(void *ctx, const uint8_t *data, size_t len)
{
    memcpy(s_played_chunks[s_played_count], data, len);
    s_played_count++;
}

static void fake_stop(void *ctx) {}

static void fake_show(void *ctx, int expression)
{
    s_shown_expressions[s_shown_count++] = expression;
}

static orchestrator_ops_t make_fake_ops(void)
{
    s_sent_audio_len = 0;
    s_end_of_speech_sent = false;
    s_played_count = 0;
    s_shown_count = 0;

    orchestrator_ops_t ops = {
        .server = { .send_audio_frame = fake_send_audio_frame, .send_end_of_speech = fake_send_end_of_speech },
        .audio_out = { .play_chunk = fake_play_chunk, .stop = fake_stop },
        .face = { .show = fake_show },
    };
    return ops;
}

TEST_CASE("starts in IDLE", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());
    TEST_ASSERT_EQUAL(HARO_STATE_IDLE, orchestrator_get_state());
}

TEST_CASE("wake word moves IDLE to LISTENING", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());
    orchestrator_on_wake_word();
    TEST_ASSERT_EQUAL(HARO_STATE_LISTENING, orchestrator_get_state());
    TEST_ASSERT_EQUAL(1, s_shown_count);
}

TEST_CASE("audio frames while LISTENING are forwarded to the server", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());
    orchestrator_on_wake_word();
    uint8_t frame[] = { 1, 2, 3, 4 };
    orchestrator_on_audio_frame(frame, sizeof(frame), false);
    TEST_ASSERT_EQUAL(sizeof(frame), s_sent_audio_len);
    TEST_ASSERT_EQUAL_MEMORY(frame, s_sent_audio, sizeof(frame));
}

TEST_CASE("end of speech moves LISTENING to THINKING and signals the server", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());
    orchestrator_on_wake_word();
    uint8_t frame[] = { 1 };
    orchestrator_on_audio_frame(frame, sizeof(frame), true);
    TEST_ASSERT_EQUAL(HARO_STATE_THINKING, orchestrator_get_state());
    TEST_ASSERT_TRUE(s_end_of_speech_sent);
}

TEST_CASE("an emotion event moves THINKING to SPEAKING and shows the matching face", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());
    orchestrator_on_wake_word();
    uint8_t frame[] = { 1 };
    orchestrator_on_audio_frame(frame, sizeof(frame), true);

    server_client_event_t evt = { .type = SERVER_CLIENT_EVENT_PROTOCOL };
    evt.protocol_event.type = PROTOCOL_EVENT_EMOTION;
    strcpy(evt.protocol_event.value, "happy");
    orchestrator_on_server_event(evt);

    TEST_ASSERT_EQUAL(HARO_STATE_SPEAKING, orchestrator_get_state());
}

TEST_CASE("an audio event while SPEAKING plays the chunk", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());
    orchestrator_on_wake_word();
    uint8_t frame[] = { 1 };
    orchestrator_on_audio_frame(frame, sizeof(frame), true);

    server_client_event_t audio_evt = { .type = SERVER_CLIENT_EVENT_AUDIO };
    uint8_t chunk[] = { 9, 9, 9 };
    audio_evt.audio_data = chunk;
    audio_evt.audio_len = sizeof(chunk);
    orchestrator_on_server_event(audio_evt);

    TEST_ASSERT_EQUAL(1, s_played_count);
}

TEST_CASE("response_end returns to IDLE", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());
    orchestrator_on_wake_word();
    uint8_t frame[] = { 1 };
    orchestrator_on_audio_frame(frame, sizeof(frame), true);

    server_client_event_t evt = { .type = SERVER_CLIENT_EVENT_PROTOCOL };
    evt.protocol_event.type = PROTOCOL_EVENT_RESPONSE_END;
    orchestrator_on_server_event(evt);

    TEST_ASSERT_EQUAL(HARO_STATE_IDLE, orchestrator_get_state());
}

TEST_CASE("a disconnect event returns to IDLE", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());
    orchestrator_on_wake_word();
    server_client_event_t evt = { .type = SERVER_CLIENT_EVENT_DISCONNECTED };
    orchestrator_on_server_event(evt);
    TEST_ASSERT_EQUAL(HARO_STATE_IDLE, orchestrator_get_state());
}
```

These 8 cases are a direct port of the shapes already proven correct in `haro/tests/test_orchestrator.py` (IDLE→LISTENING on wake, audio forwarding while LISTENING, end-of-speech → THINKING, emotion → SPEAKING, audio playback while SPEAKING, response_end → IDLE, error/disconnect → IDLE) — cross-check against that file's actual assertions while implementing, since it also covers timeout and mid-turn-disconnect cases not reproduced above; add those as additional `TEST_CASE`s using the same fake-ops pattern rather than skipping them.

- [ ] **Step 4: Run and confirm the tests fail to build** (`orchestrator.c` doesn't exist yet)

```bash
idf.py --preview set-target linux
idf.py build
```

- [ ] **Step 5: Implement `orchestrator.c`**

```c
#include "orchestrator.h"
#include <string.h>

static orchestrator_ops_t s_ops;
static haro_state_t s_state = HARO_STATE_IDLE;

// Must match face_display.h's Expression enum values (Task 8) once wired in main.c;
// duplicated here only as plain ints so this component doesn't need to depend on face_display.
enum { EXPR_IDLE, EXPR_LISTENING, EXPR_THINKING, EXPR_SPEAKING_HAPPY, EXPR_SPEAKING_SAD,
       EXPR_SPEAKING_CONFUSED, EXPR_SPEAKING_NEUTRAL, EXPR_ERROR, EXPR_SETUP };

void orchestrator_init(orchestrator_ops_t ops)
{
    s_ops = ops;
    s_state = HARO_STATE_IDLE;
}

haro_state_t orchestrator_get_state(void)
{
    return s_state;
}

void orchestrator_on_wake_word(void)
{
    if (s_state != HARO_STATE_IDLE) return;
    s_state = HARO_STATE_LISTENING;
    s_ops.face.show(s_ops.face.ctx, EXPR_LISTENING);
}

void orchestrator_on_audio_frame(const uint8_t *frame, size_t len, bool is_end_of_speech)
{
    if (s_state != HARO_STATE_LISTENING) return;

    s_ops.server.send_audio_frame(s_ops.server.ctx, frame, len);

    if (is_end_of_speech) {
        s_state = HARO_STATE_THINKING;
        s_ops.face.show(s_ops.face.ctx, EXPR_THINKING);
        s_ops.server.send_end_of_speech(s_ops.server.ctx);
    }
}

static int expression_for_emotion(const char *value)
{
    if (strcmp(value, "happy") == 0) return EXPR_SPEAKING_HAPPY;
    if (strcmp(value, "sad") == 0) return EXPR_SPEAKING_SAD;
    if (strcmp(value, "confused") == 0) return EXPR_SPEAKING_CONFUSED;
    return EXPR_SPEAKING_NEUTRAL;
}

static void return_to_idle(void)
{
    s_ops.audio_out.stop(s_ops.audio_out.ctx);
    s_state = HARO_STATE_IDLE;
    s_ops.face.show(s_ops.face.ctx, EXPR_IDLE);
}

void orchestrator_on_server_event(server_client_event_t event)
{
    if (s_state != HARO_STATE_THINKING && s_state != HARO_STATE_SPEAKING) {
        if (event.type != SERVER_CLIENT_EVENT_DISCONNECTED) return;
    }

    switch (event.type) {
    case SERVER_CLIENT_EVENT_PROTOCOL:
        if (event.protocol_event.type == PROTOCOL_EVENT_EMOTION) {
            s_state = HARO_STATE_SPEAKING;
            s_ops.face.show(s_ops.face.ctx, expression_for_emotion(event.protocol_event.value));
        } else if (event.protocol_event.type == PROTOCOL_EVENT_RESPONSE_END) {
            return_to_idle();
        } else if (event.protocol_event.type == PROTOCOL_EVENT_ERROR) {
            s_ops.face.show(s_ops.face.ctx, EXPR_ERROR);
            return_to_idle();
        }
        break;
    case SERVER_CLIENT_EVENT_AUDIO:
        s_state = HARO_STATE_SPEAKING;
        s_ops.audio_out.play_chunk(s_ops.audio_out.ctx, event.audio_data, event.audio_len);
        break;
    case SERVER_CLIENT_EVENT_DISCONNECTED:
        s_ops.face.show(s_ops.face.ctx, EXPR_ERROR);
        return_to_idle();
        break;
    }
}
```

- [ ] **Step 6: Run the tests and confirm they pass**

```bash
idf.py build
idf.py monitor
```
Expected: all `[orchestrator]` test cases PASS (including any additional timeout/mid-turn-disconnect cases added per Step 3's note).

- [ ] **Step 7: Commit**

```bash
git add components/orchestrator
git commit -m "feat: add orchestrator component with host-side unit tests"
```

---

### Task 8: `face_display` component (SSD1306 via `esp_lcd`, Cozmo-style faces)

**Files:**
- Create: `components/face_display/CMakeLists.txt`
- Create: `components/face_display/include/face_display.h`
- Create: `components/face_display/face_display.c`

**Interfaces:**
- Produces (used by `main.c`, matched against `orchestrator`'s `EXPR_*` values from Task 7):
  ```c
  typedef enum {
      EXPR_IDLE, EXPR_LISTENING, EXPR_THINKING, EXPR_SPEAKING_HAPPY, EXPR_SPEAKING_SAD,
      EXPR_SPEAKING_CONFUSED, EXPR_SPEAKING_NEUTRAL, EXPR_ERROR, EXPR_SETUP,
  } face_expression_t;

  esp_err_t face_display_init(void);            // I2C bus (separate from the internal codec bus — external header pins) + esp_lcd_panel_ssd1306
  esp_err_t face_display_show(face_expression_t expression);
  ```

Porting note: `face_display.py`'s `_eye()` helper rotates a rounded-rectangle by rendering it to a small offscreen canvas and calling PIL's `Image.rotate()`. C has no equivalent one-line rotate — this task ports that specific technique as manual per-pixel rotation (compute each source pixel's position under the inverse rotation matrix, nearest-neighbor sample into the destination buffer), which is the direct translation of "rasterize small, rotate, composite" into raw framebuffer code.

- [ ] **Step 1: Choose the external I2C GPIOs for the display and write the header**

Per the spec, the external SSD1306 uses two of the 11 free header GPIOs, independent from the internal codec/TCA9555 I2C bus (GPIO10/11) — use GPIO8=SDA, GPIO9=SCL (both confirmed free/unused by the internal board in Waveshare's `bsp_board.h`).

`components/face_display/include/face_display.h`:
```c
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EXPR_IDLE, EXPR_LISTENING, EXPR_THINKING, EXPR_SPEAKING_HAPPY, EXPR_SPEAKING_SAD,
    EXPR_SPEAKING_CONFUSED, EXPR_SPEAKING_NEUTRAL, EXPR_ERROR, EXPR_SETUP,
} face_expression_t;

esp_err_t face_display_init(void);
esp_err_t face_display_show(face_expression_t expression);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Implement I2C + panel init**

```c
#include "face_display.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include <string.h>
#include <math.h>

#define GPIO_DISPLAY_SDA GPIO_NUM_8
#define GPIO_DISPLAY_SCL GPIO_NUM_9
#define DISPLAY_I2C_ADDR 0x3C
#define WIDTH 128
#define HEIGHT 64

static const char *TAG = "face_display";
static esp_lcd_panel_handle_t s_panel;
static uint8_t s_framebuffer[WIDTH * HEIGHT / 8]; // 1bpp, row-major, matching esp_lcd_panel_ssd1306's expected packing

esp_err_t face_display_init(void)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = GPIO_DISPLAY_SDA,
        .scl_io_num = GPIO_DISPLAY_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus);
    if (err != ESP_OK) return err;

    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = DISPLAY_I2C_ADDR,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_bit_offset = 6,
    };
    err = esp_lcd_new_panel_io_i2c(bus, &io_config, &io_handle);
    if (err != ESP_OK) return err;

    esp_lcd_panel_ssd1306_config_t ssd1306_config = { .height = 64 };
    esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config = &ssd1306_config,
    };
    err = esp_lcd_new_panel_ssd1306(io_handle, &panel_config, &s_panel);
    if (err != ESP_OK) return err;

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    return esp_lcd_panel_disp_on_off(s_panel, true);
}
```

**Flag for the implementer:** the exact byte-packing `esp_lcd_panel_draw_bitmap` expects for a 1bpp SSD1306 panel (row-major vs page/column addressing, MSB/LSB-first) was **not** independently verified during planning — read `esp_lcd_panel_ssd1306.c`'s `panel_ssd1306_draw_bitmap` implementation (in the resolved ESP-IDF v6.1 source tree, `components/esp_lcd/`) before writing the framebuffer-composition code in the next step, and adjust `s_framebuffer`'s layout/the final `draw_bitmap` call to match exactly what that function expects — do not assume the naive row-major layout sketched above is correct without that check.

- [ ] **Step 3: Port the drawing primitives from `face_display.py`**

Translate `_eye()` (rounded-rect, optionally rotated), `_capped_arc()`, `_eye_smile()`, `_eye_x()` from `haro/src/haro/face_display.py` into functions that set bits directly in `s_framebuffer` instead of drawing into a PIL `Image`. The non-rotated rounded-rectangle and arc/line cases translate directly (loop over the bounding box, test each pixel against the shape's implicit equation, set the corresponding framebuffer bit). The rotated-eye case (used for SAD/CONFUSED/THINKING) needs the inverse-rotation sampling technique described in this task's header note — for each destination pixel in the eye's bounding box, rotate its coordinate by `-angle` around the eye's center, and test that rotated coordinate against the *unrotated* rounded-rectangle equation:

```c
static bool point_in_rounded_rect(int x, int y, int w, int h, int r)
{
    int hw = w / 2, hh = h / 2;
    int dx = x < 0 ? -x - hw : x - hw; // distance from nearest vertical edge, offset by radius region
    // ... full point-in-rounded-rect test: within the central cross of the
    // stadium shape, or within radius r of one of the four corner circles.
    // Implement following the same geometry as PIL's rounded_rectangle: a
    // W×H rect with corner radius r is the union of a (W-2r)×H rect, a
    // W×(H-2r) rect, and four quarter-circles of radius r at the corners.
    return false; // placeholder for the implementer to complete per the geometry above
}

static void set_eye_pixel(int cx, int cy, int local_x, int local_y, double angle_rad)
{
    double cos_a = cos(-angle_rad), sin_a = sin(-angle_rad);
    double rx = local_x * cos_a - local_y * sin_a;
    double ry = local_x * sin_a + local_y * cos_a;
    // test (rx, ry) against point_in_rounded_rect(...) in the eye's own local space,
    // and if inside, set the framebuffer bit at (cx + local_x, cy + local_y).
}
```

This is a genuine, non-trivial geometry port — budget real implementation time here, verify visually (Step 5) the same way the Pi version was verified with the rendered preview grid, rather than trusting the math on the first try.

- [ ] **Step 4: Implement `face_display_show` for all 9 expressions**, mirroring `render_expression()`'s per-expression geometry from `face_display.py` (eye size/position/tilt and mouth shape per `Expression` value) using the framebuffer primitives from Step 3, ending with:

```c
esp_err_t face_display_show(face_expression_t expression)
{
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    // ... call the Step 3 primitives per `expression`, same geometry as
    // haro/src/haro/face_display.py's render_expression() ...
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_framebuffer);
}
```

- [ ] **Step 5: Build, flash, and visually verify each expression**

```c
// temporary in main.c:
face_display_init();
for (int i = 0; i <= EXPR_SETUP; i++) {
    face_display_show((face_expression_t)i);
    vTaskDelay(pdMS_TO_TICKS(2000));
}
```
```bash
idf.py build flash monitor
```
Expected: cycles through all 9 faces on the physical OLED, visually matching the Pi's rendered preview grid (rounded-pill eyes, tilts for sad/confused/thinking, arc smile for happy, X for error, dots for setup).

- [ ] **Step 6: Commit**

```bash
git add components/face_display
git commit -m "feat: add face_display component (SSD1306 via esp_lcd, ported Cozmo faces)"
```

---

### Task 9: `wifi_provisioning` component (`network_provisioning`)

**Files:**
- Create: `components/wifi_provisioning/CMakeLists.txt`
- Create: `components/wifi_provisioning/include/haro_wifi_provisioning.h`
- Create: `components/wifi_provisioning/haro_wifi_provisioning.c`
- Modify: `main/idf_component.yml` (add `espressif/network_provisioning`)

**Interfaces:**
- Produces (used by `main.c`):
  ```c
  esp_err_t wifi_provisioning_ensure_connected(void); // blocks until WiFi is up: either already-known credentials connect, or SoftAP setup flow completes
  ```

- [ ] **Step 1: Resolve the open UX question flagged in the spec BEFORE writing this component's real logic**

Read the `network_prov_scheme_softap` source (resolves into `managed_components/espressif__network_provisioning/` after the dependency in Step 2 is added and `idf.py build` run once) to determine whether it serves a plain HTML form or expects the protocomm/protobuf-speaking companion app. Write the finding as a comment at the top of `haro_wifi_provisioning.c`, and:
- If a plain web form is available: proceed with SoftAP scheme, matching today's Pi UX (open `Haro-Setup`, browse to a page, enter WiFi credentials).
- If not: this is a real UX change from the Pi's flow — **stop and confirm with the user** whether the app-based flow is acceptable before implementing, rather than silently shipping a worse or different setup experience than what was scoped.

- [ ] **Step 2: Add the dependency**

`main/idf_component.yml`, add:
```yaml
  espressif/network_provisioning: "^1.1.0"
```

- [ ] **Step 3: Write the header**

`components/wifi_provisioning/include/haro_wifi_provisioning.h`:
```c
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_provisioning_ensure_connected(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 4: Implement**, following the verified `network_prov_mgr_init` / `network_prov_mgr_start_provisioning` API (SoftAP scheme, security matching what Step 1 determined — `NETWORK_PROV_SECURITY_1` with a proof-of-possession string is the common default if the plain-web-form path is confirmed), SSID `"Haro-Setup"` and password matching the Pi's `hotspot_password` config default (`"haro1234"`) for behavioral parity, blocking on `NETWORK_PROV_END` via an event-group bit before returning.

(Left as an implementation-time task rather than a full code listing here, since its correctness depends entirely on Step 1's finding, which wasn't resolved during planning — writing concrete code against an unconfirmed API shape would violate this plan's "verify against real docs, don't invent" constraint.)

- [ ] **Step 5: Build, flash, and manually verify**

Power on with no known WiFi network in range/configured; confirm the `Haro-Setup` AP appears, and that connecting to it and (per Step 1's finding) either loading a web form or using the companion app successfully provisions the board onto a real WiFi network.

- [ ] **Step 6: Commit**

```bash
git add components/wifi_provisioning main/idf_component.yml
git commit -m "feat: add wifi_provisioning component (network_provisioning SoftAP)"
```

---

### Task 10: Final integration — `main.c`

**Files:**
- Modify: `main/main.c` (replace all temporary manual-test code from Tasks 4-9 with the real wiring)
- Modify: `main/CMakeLists.txt` (list all components as `REQUIRES`)

**Interfaces:**
- Consumes every component's public header from Tasks 2-9.

- [ ] **Step 1: Write the real `app_main`**

```c
#include "haro_config.h"
#include "haro_wifi_provisioning.h"
#include "audio_pipeline.h"
#include "wake_word.h"
#include "server_client.h"
#include "face_display.h"
#include "orchestrator.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "haro";
static QueueHandle_t s_wake_queue;
static QueueHandle_t s_server_queue;

static esp_err_t server_send_audio_frame(void *ctx, const uint8_t *data, size_t len)
{
    return server_client_send_audio_frame(data, len);
}
static esp_err_t server_send_end_of_speech(void *ctx)
{
    return server_client_send_end_of_speech();
}
static void audio_play_chunk(void *ctx, const uint8_t *data, size_t len)
{
    audio_pipeline_write(data, len);
}
static void audio_stop(void *ctx) {}
static void face_show(void *ctx, int expression)
{
    face_display_show((face_expression_t)expression);
}

static void orchestrator_task(void *arg)
{
    while (true) {
        wake_word_event_type_t wake_evt;
        if (xQueueReceive(s_wake_queue, &wake_evt, 0) == pdTRUE) {
            orchestrator_on_wake_word();
        }

        server_client_event_t server_evt;
        if (xQueueReceive(s_server_queue, &server_evt, pdMS_TO_TICKS(20)) == pdTRUE) {
            orchestrator_on_server_event(server_evt);
            if (server_evt.type == SERVER_CLIENT_EVENT_AUDIO) {
                free(server_evt.audio_data);
            }
        }

        if (orchestrator_get_state() == HARO_STATE_LISTENING) {
            uint8_t frame[512];
            size_t bytes_read;
            if (audio_pipeline_read(frame, sizeof(frame), &bytes_read) == ESP_OK) {
                orchestrator_on_audio_frame(frame, bytes_read, false /* TODO: VAD end-of-speech signal */);
            }
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(haro_config_init());
    ESP_ERROR_CHECK(wifi_provisioning_ensure_connected());
    ESP_ERROR_CHECK(audio_pipeline_init());
    ESP_ERROR_CHECK(face_display_init());

    char server_url[128];
    ESP_ERROR_CHECK(haro_config_get_server_url(server_url, sizeof(server_url)));

    s_wake_queue = xQueueCreate(4, sizeof(wake_word_event_type_t));
    s_server_queue = xQueueCreate(8, sizeof(server_client_event_t));

    ESP_ERROR_CHECK(server_client_init(server_url, s_server_queue));
    ESP_ERROR_CHECK(server_client_send_hello("haro-session"));
    ESP_ERROR_CHECK(wake_word_start(s_wake_queue));

    orchestrator_ops_t ops = {
        .server = { .send_audio_frame = server_send_audio_frame, .send_end_of_speech = server_send_end_of_speech },
        .audio_out = { .play_chunk = audio_play_chunk, .stop = audio_stop },
        .face = { .show = face_show },
    };
    orchestrator_init(ops);
    face_display_show(EXPR_IDLE);

    ESP_LOGI(TAG, "Haro ready, waiting for wake word");
    xTaskCreate(orchestrator_task, "orchestrator", 4096, NULL, 5, NULL);
}
```

**Flag for the implementer:** the `false /* TODO: VAD end-of-speech signal */` above is a known gap — Task 5 deliberately scoped `wake_word` to wake-detection only and deferred VAD/end-of-speech wiring (see that task's note). Before this integration task is really done, either (a) extend `wake_word_start`'s queue to also post an end-of-speech event using AFE's `vad_state` field on the same `afe_fetch_result_t`, or (b) port `haro/src/haro/vad.py`'s standalone silence-duration approach as a small new component. Don't ship Task 10 with end-of-speech permanently hardcoded to `false` — pick one of these and wire it before calling the milestone done.

- [ ] **Step 2: Update `main/CMakeLists.txt`**

```cmake
set(SOURCES "main.c")
set(INCLUDE_DIRS ".")

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    REQUIRES haro_config wifi_provisioning audio_pipeline wake_word server_client face_display orchestrator protocol
)
```

- [ ] **Step 3: Build, flash, and run the full end-to-end test**

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```
With `server/` running and reachable, and WiFi provisioned: say "Hi ESP", speak a question, confirm the face reflects LISTENING → THINKING → SPEAKING, the response plays through the speaker, and it returns to IDLE.

- [ ] **Step 4: Commit**

```bash
git add main/main.c main/CMakeLists.txt
git commit -m "feat: wire all components together in app_main (milestone 1 complete)"
```

---

## Self-Review Notes

- **Spec coverage:** every spec section has a task — toolchain/scaffolding (Task 1), each component-mapping row (Tasks 2-9), state machine (Task 7), concurrency (Task 10), display (Task 8), milestone-1 "everything together" (Task 10). The spec's two open items (esp-sr `afe_config_t` exact fields, network_provisioning web-form-vs-app UX) are carried into Tasks 5 and 9 as explicit implementer checks rather than silently assumed.
- **No invented APIs:** every struct/function signature in Tasks 2, 4, 6's core init path, and Task 1 came from a fetched, cited official source (ESP-IDF v6.1 docs, esp_codec_dev README, esp_websocket_client docs, Waveshare's own demo source) during planning. Tasks 5, 8, and 9 contain explicit "flag for the implementer" notes wherever the verified research left a gap, instead of papering over it with a plausible-sounding guess — resolve those by reading the actual resolved component source at implementation time, per the user's standing instruction to verify against current official documentation rather than assume.
- **Type consistency:** `face_expression_t`'s enum values are duplicated as a plain `enum` in `orchestrator.c` (to avoid a dependency from `orchestrator` on `face_display`) — Task 10's integration step is where a mismatch would surface (the `int` cast from `orchestrator`'s internal enum to `face_display_show`'s `face_expression_t` only works if both enums list values in the same order). Flagged here explicitly: **when implementing, keep `orchestrator.c`'s internal `EXPR_*` enum and `face_display.h`'s `face_expression_t` in the exact same declaration order**, or replace the `int`-based `orchestrator_face_ops_t.show` signature with a shared header both components include.
