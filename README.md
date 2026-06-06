# ai-note-meet

An ESP32-S3 firmware for the **Waveshare ESP32-S3-ePaper-1.54G-EN** that combines a voice note recorder with a WiFi-connected AI meeting recorder — displayed on a 200×200 4-color e-paper screen.

Built by merging two open-source projects:
- **[pala_note](https://github.com/search?q=pala_note)** — e-paper voice recorder with RTC, SD card, and battery UI (original hardware abstraction and display layer)
- **[MeetingRecorder](https://github.com/techiesms/MeetingRecorder)** by [techiesms](https://github.com/techiesms) — WiFi meeting recorder with ElevenLabs STT, AI summaries, and web dashboard

All credit to the original authors for the foundations. This project adds multi-provider AI support, Markdown output, a unified hardware layer, and merges both feature sets into a single firmware.

---

## Features

### Notes mode (pala_note)
- Record voice memos directly to SD card
- Browse, tag, and manage notes on the e-paper display
- RTC-stamped filenames
- Battery level display
- Deep sleep with wake on button press

### Meeting mode (MeetingRecorder)
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
| Display | 200×200 4-color e-paper (R/Y/B/W), partial refresh |
| Audio | ES8311 codec (I2C + I2S) |
| Storage | MicroSD via SDMMC 1-bit |
| RTC | On-board (NTP-synced over WiFi) |

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
- `ESP32 Audio` (or included audio BSP — see `src/audio/`)
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

Format as FAT32. No files needed to start — `config.json` is created on first save from the web dashboard. The firmware falls back to AP mode (`MeetingRecorder` / `recorder123`) if no WiFi config is found.

### 5. Flash

Open `pala_meet.ino` in Arduino IDE and hit Upload. First boot starts a WiFi AP — connect to it and open `http://192.168.4.1` to configure.

---

## Button map

| Button | Short press | Long press | Double press |
|--------|-------------|------------|--------------|
| REC (GPIO0) | Advance menu / confirm | Start recording | Back |
| PWR (GPIO18) | Back / cancel | Power off | — |

---

## Web dashboard

Browse to `http://meetingrecorder.local` on the same network.

| Tab | What it does |
|-----|-------------|
| Transcript | Live transcript as chunks arrive |
| Summary | AI-generated meeting summary |
| History | Past meetings — download `.md` files |
| Settings | WiFi, API keys, AI provider/model |

---

## Project structure

```
pala_meet.ino          — Main sketch, state machine, FreeRTOS task launch
types.h                — Shared enums and state definitions
config.h               — Hardware pin assignments
secrets.h              — Local credentials (gitignored)
src/
  api/                 — ElevenLabs STT + multi-provider AI dispatcher
  app/                 — Buttons, battery, draw, notes, record, sleep, UI
  audio/               — ES8311 audio BSP (I2S read/write)
  codec_board/         — Board-level codec init
  config/              — SD card config.json load/save
  core/globals.h       — Shared extern declarations
  display/             — E-paper driver BSP
  esp_codec_dev/       — ESP codec device drivers (es8311, es7210)
  i2c_bsp/             — I2C bus setup
  json/                — ShubhJson (lightweight ArduinoJson alternative)
  meet/                — Meeting coordinator (WiFi, mDNS, task launch)
  power/               — Board power management
  process/             — Chunked STT + rolling summary pipeline
  time/                — NTP sync helpers
  web/                 — HTTP handlers and HTML dashboard
```

---

## Roadmap

- [ ] Notes tab in web dashboard (Phase 2)
- [ ] Migrate from `secrets.h` to full `config.json` onboarding (Phase 3)
- [ ] Compile verification pass (pre-hardware)
- [ ] First hardware flash and test on physical board

---

## Acknowledgements

- **pala_note** — original e-paper recorder firmware (hardware abstraction, display, button handling, audio BSP)
- **[MeetingRecorder](https://github.com/techiesms/MeetingRecorder)** by **techiesms** — WiFi meeting recorder, ElevenLabs integration, web dashboard architecture
- **[ElevenLabs Scribe](https://elevenlabs.io/docs/api-reference/speech-to-text)** — speech-to-text with speaker diarization
- **ShubhJson** — lightweight JSON library for embedded targets

---

## License

This project inherits from open-source originals. Please check the licenses of the upstream projects before any commercial use. Original modifications in this repo are released under MIT.
