# ai-note-meet

An ESP32-S3 firmware for the **Waveshare ESP32-S3-ePaper-1.54G-EN** that combines a voice note recorder with a WiFi-connected AI meeting recorder — displayed on a 200×200 4-color e-paper screen (black, white, red, yellow).

Built by merging two open-source projects:
- **pala_note** — e-paper voice recorder with RTC, SD card, and battery UI (original hardware abstraction and display layer)
- **[MeetingRecorder](https://github.com/techiesms/MeetingRecorder)** by [techiesms](https://github.com/techiesms) — WiFi meeting recorder with ElevenLabs STT, AI summaries, and web dashboard

All credit to the original authors for the foundations. This project adds multi-provider AI support, Markdown output, a unified hardware layer for the 4-color G panel, and merges both feature sets into a single firmware.

---

## Features

### Notes mode
- Record voice memos directly to SD card
- Browse, tag, and manage notes on the e-paper display
- RTC-stamped filenames
- Battery level indicator
- Deep sleep with wake on button press

### Meeting mode
- Record long meetings in 15-second WAV chunks to SD card
- Transcription via **ElevenLabs Scribe v1** (with speaker diarization)
- AI summary via a configurable provider — no reflash needed to switch
- Web dashboard at `http://meetingrecorder.local`
  - Live transcript view
  - Download transcript or summary as `.md`
  - Chat with the AI about the recording
  - Settings UI for WiFi, API keys, and AI provider

### Multi-provider AI
Configurable at runtime via the web dashboard or `config.json` on the SD card:

| Provider   | Models (examples)              |
|------------|-------------------------------|
| OpenAI     | `gpt-4o`, `gpt-4o-mini`       |
| Anthropic  | `claude-3-5-sonnet-20241022`  |
| Google     | `gemini-1.5-pro`              |
| Ollama     | any locally-served model      |

---

## Hardware

| Component | Part |
|-----------|------|
| Board | Waveshare ESP32-S3-ePaper-1.54G-EN |
| MCU | ESP32-S3-PICO-1-N8R8 (8 MB flash, 8 MB OPI PSRAM) |
| Display | 200×200 4-color e-paper (R/Y/B/W), full refresh only |
| Audio | ES8311 codec (I2C + I2S) |
| Storage | MicroSD via SDMMC 1-bit |
| RTC | On-board (NTP-synced over WiFi) |

> **Display refresh time:** First boot does one slow full clear (~14s). After that, fast-refresh mode is active (~3–5s per update). The 4-color G panel does not support partial refresh.

---

## Setup

### 1. Arduino IDE

- Install the **pioarduino ESP32 core** (v3.3.9 or later)
  - Board manager URL: `https://raw.githubusercontent.com/pioarduino/platform-espressif32/refs/heads/main/package_esp32_index.json`
- Select board: `ESP32S3 Dev Module`
- Partition scheme: `16M Flash (3MB APP/9.9MB FATFS)` or similar with enough app space
- PSRAM: `OPI PSRAM`

### 2. Libraries (install via Arduino Library Manager)

- `ArduinoJson` ≥ 7.x
- `WebServer` (bundled with ESP32 core)
- `ESPmDNS` (bundled with ESP32 core)

### 3. Credentials

Copy `secrets.h.example` to `secrets.h` and fill in your details:

```cpp
#define WIFI_SSID   "your-network"
#define WIFI_PASS   "your-password"
#define OPENAI_KEY  "sk-..."
```

API keys for Anthropic, Gemini, and ElevenLabs are set at runtime via the web dashboard and stored in `config.json` on the SD card — no reflash required.

### 4. SD Card

Format as FAT32. Optionally copy `config.json.example` to `config.json`, fill in your credentials, and put it in the root of the SD card before first boot. If no `config.json` is found, the firmware falls back to AP mode (`MeetingRecorder` / `recorder123`) — connect to that network and open `http://192.168.4.1` to configure via the web UI, which writes `config.json` to the SD card automatically.

### 5. Flash

Open `pala_meet.ino` in Arduino IDE and click Upload.

> **Upload tip:** The board uses USB CDC. If the upload fails with "port busy" or "no such file or directory", close the Serial Monitor first, then hold BOOT and tap RESET to enter bootloader mode before uploading.

---

## Using the device

### Boot sequence

1. Power on with the PWR button (GPIO18).
2. The display clears to white (~14s — this only happens once per boot).
3. The device connects to WiFi and syncs time via NTP.
4. The idle screen shows the current time, date, and battery level.

All display updates after boot use fast-refresh mode (~3–5s).

### Button controls

| Button | Action | Result |
|--------|--------|--------|
| REC (GPIO0) | Short press | Advance / confirm in menu |
| REC (GPIO0) | Long press (>350ms) | **Start recording** |
| REC (GPIO0) | Double press | Back / cancel |
| PWR (GPIO18) | Short press | Back / cancel |
| PWR (GPIO18) | Long press (>600ms) | Power off (deep sleep) |

### Recording a voice note

1. From the idle screen, long-press **REC** to start recording.
2. The display shows a recording indicator.
3. Short-press **REC** to stop and save. The note is transcribed/processed by AI.
4. The display returns to the note list.

### Recording a meeting

1. Navigate to Meeting mode from the main menu.
2. Long-press **REC** to start. Audio is saved in 15-second chunks to SD card.
3. Short-press **REC** to stop. The full session is sent for transcription and summary.
4. Open `http://meetingrecorder.local` on the same network to read the transcript and summary.

### Web dashboard

Browse to `http://meetingrecorder.local` (or `http://192.168.4.1` in AP mode).

| Tab | What it does |
|-----|-------------|
| Transcript | Live transcript as chunks arrive |
| Summary | AI-generated meeting summary |
| History | Past meetings — download `.md` files |
| Settings | WiFi, API keys, AI provider/model |

### Changing AI provider

Open the Settings tab in the web dashboard. Changes are written to `config.json` on the SD card immediately — no reboot or reflash needed.

### Deep sleep / wake

Long-press PWR to sleep. The device draws near-zero current. Press either button to wake. The display retains its last image in deep sleep (e-paper holds without power).

---

## Project structure

```
pala_meet.ino          — Main sketch, state machine, FreeRTOS task launch
types.h                — Shared enums and state definitions
config.h               — Hardware pin assignments
secrets.h              — Local credentials (gitignored — copy from secrets.h.example)
src/
  api/                 — ElevenLabs STT + multi-provider AI dispatcher
  app/                 — Buttons, battery, draw, notes, record, sleep, UI
  audio/               — ES8311 audio BSP (I2S read/write)
  codec_board/         — Board-level codec init
  config/              — SD card config.json load/save
  display/             — E-paper driver BSP (1.54G 4-color, 2bpp)
  esp_codec_dev/       — ESP codec device drivers
  i2c_bsp/             — I2C bus setup
  json/                — ArduinoJson v7 shim
  meet/                — Meeting coordinator (WiFi, mDNS, task launch)
  power/               — Board power management
  process/             — Chunked STT + rolling summary pipeline
  time/                — NTP sync helpers
  web/                 — HTTP handlers and HTML dashboard
```

---

## Roadmap

- [x] 4-color e-paper display working (Waveshare 1.54G)
- [x] Fast-refresh mode (~3–5s updates)
- [ ] Notes tab in web dashboard
- [ ] Migrate from `secrets.h` to full `config.json` onboarding

---

## Acknowledgements

- **pala_note** — original e-paper recorder firmware (hardware abstraction, display, button handling, audio BSP)
- **[MeetingRecorder](https://github.com/techiesms/MeetingRecorder)** by **techiesms** — WiFi meeting recorder, ElevenLabs integration, web dashboard architecture
- **[ElevenLabs Scribe](https://elevenlabs.io/docs/api-reference/speech-to-text)** — speech-to-text with speaker diarization
- **[Waveshare ESP32-S3-ePaper-1.54G](https://github.com/waveshareteam/ESP32-S3-ePaper-1.54G)** — official board examples, display driver reference

---

## License

This project inherits from open-source originals. Please check the licenses of the upstream projects before any commercial use. Original modifications in this repo are released under MIT.
