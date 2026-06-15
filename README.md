# ai-note-meet

An ESP32-S3 firmware for the **Waveshare ESP32-S3-ePaper-1.54G-EN** that combines a voice note recorder with a WiFi-connected AI meeting recorder — displayed on a 200×200 4-color e-paper screen (black, white, red, yellow).

Built by merging two open-source projects:
- **pala_note** — e-paper voice recorder with RTC, SD card, and battery UI (original hardware abstraction and display layer)
- **[MeetingRecorder](https://github.com/techiesms/MeetingRecorder)** by [techiesms](https://github.com/techiesms) — WiFi meeting recorder with ElevenLabs STT, AI summaries, and web dashboard

All credit to the original authors for the foundations. This project adds multi-provider AI support, Markdown output, a unified hardware layer for the 4-color G panel, a voice notes portal, and merges both feature sets into a single firmware with a single web interface at `notemeet.local`.

---

## Features

### Notes mode
- Record voice memos directly to SD card as WAV files
- Tag each note on-device after recording (Work, Idea, Buy, Private, or custom tags)
- Browse and manage notes by tag on the e-paper display
- **Sync**: one-tap transcription via OpenAI Whisper — WAVs are deleted after successful transcription, TXT transcripts retained
- RTC-stamped filenames
- Battery level indicator
- Deep sleep with wake on button press

### Meeting mode
- Record long meetings in 15-second WAV chunks to SD card
- Automatic transcription via **ElevenLabs Scribe v1** (with speaker diarization) — no sync step needed
- AI summary via a configurable provider — no reflash needed to switch
- Summary generated using single-call or map-reduce strategy depending on meeting length

### Web dashboard — `notemeet.local`
A single dark-mode SPA served from port 80, accessible on the same WiFi network or via the device's own AP:

| Tab | What it does |
|-----|-------------|
| Transcript | Live transcript as meeting chunks arrive |
| Summary | AI-generated meeting summary |
| History | Past meetings — download `.md` files, delete, or regenerate summary |
| Notes | Browse and manage voice notes — filter by tag, play audio, download TXT |
| Settings | WiFi, API keys, AI provider/model, timezone |

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

Format as FAT32. Optionally copy `config.json.example` to `config.json`, fill in your credentials, and put it in the root of the SD card before first boot. If no `config.json` is found, the firmware falls back to AP mode (`notemeet` / `recorder123`) — connect to that network and open `http://192.168.4.1` to configure via the web UI, which writes `config.json` to the SD card automatically.

### 5. Flash

Open `pala_meet.ino` in Arduino IDE and click Upload.

> **Upload tip:** The board uses USB CDC. If the upload fails with "port busy" or "no such file or directory", close the Serial Monitor first, then hold BOOT and tap RESET to enter bootloader mode before uploading.

---

## Using the device

### Boot sequence

1. Power on with the PWR button (GPIO18).
2. The display clears to white (~14s — this only happens once per boot).
3. The device connects to WiFi and syncs time via NTP, then starts the AP.
4. The idle screen shows the current time, date, and battery level.

All display updates after boot use fast-refresh mode (~3–5s).

### Button controls

| Button | Action | Result |
|--------|--------|--------|
| REC (GPIO0) | Long press (>350ms) from idle | Start recording a voice note |
| REC (GPIO0) | Short press | Confirm / advance in menus |
| REC (GPIO0) | Double press | Back / cancel |
| PWR (GPIO18) | Any press from idle | Open main menu |
| PWR (GPIO18) | Short press in menu | Next menu item |
| PWR (GPIO18) | Short press in submenu | Back / cancel |

The device enters deep sleep automatically after a configurable inactivity timeout. Press any button to wake.

### Main menu

| Item | What it does |
|------|-------------|
| Notes | Browse recorded voice notes, filtered by tag |
| Tags | Manage tags (add, delete, view note counts) |
| Meet | Enter meeting recording mode |
| Sync | Transcribe any untranscribed voice notes via Whisper |
| Settings | Sounds on/off · Device info |

### Recording a voice note

1. From the idle screen, **long-press REC** to start recording.
2. The display shows a waveform recording indicator.
3. **Short-press REC** to stop. The WAV is saved to SD card.
4. Choose a tag with PWR (cycle) and confirm with REC. The device sleeps.
5. Next time you press **Sync**, the WAV is sent to Whisper for transcription. The WAV is deleted after a successful transcription; the TXT transcript is kept.

### Recording a meeting

1. From the main menu, select **Meet**.
2. **Long-press REC** to start. Audio is saved in 15-second chunks; transcription and rolling summary happen automatically in the background.
3. **Short or long-press REC** to stop. The full transcript goes through a final AI summary pass.
4. Open `http://notemeet.local` on the same network to read the transcript and summary.

### Syncing voice notes

1. From the main menu, select **Sync**.
2. The device connects to WiFi (using credentials from `secrets.h` or `config.json`) and calls the Whisper API for each un-transcribed note.
3. WAV files are deleted from the SD card after a successful transcription. TXT transcripts are retained.
4. Browse transcribed notes at `http://notemeet.local` → Notes tab, or on-device via the Notes menu.

### Web dashboard

Browse to `http://notemeet.local` (or `http://192.168.4.1` in AP mode).

WiFi credentials and API keys entered via the Settings tab are saved to `config.json` on the SD card immediately and take effect without a reboot.

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
  app/network.cpp      — Whisper transcription, Sync flow, Notes portal handlers
  audio/               — ES8311 audio BSP (I2S read/write)
  codec_board/         — Board-level codec init
  config/              — SD card config.json load/save
  display/             — E-paper driver BSP (1.54G 4-color, 2bpp)
  esp_codec_dev/       — ESP codec device drivers
  i2c_bsp/             — I2C bus setup
  json/                — ArduinoJson v7 shim
  meet/                — Meeting coordinator (WiFi, mDNS, task launch)
  power/               — Board power management
  process/             — Chunked STT + rolling/final summary pipeline
  time/                — NTP sync helpers
  web/                 — HTTP handlers and HTML dashboard SPA
```

---

## Roadmap

- [x] 4-color e-paper display working (Waveshare 1.54G)
- [x] Fast-refresh mode (~3–5s updates)
- [x] Notes tab in web dashboard
- [x] Voice notes portal at `notemeet.local/notes`
- [ ] Migrate from `secrets.h` to full `config.json` onboarding (no compile-time credentials)

---

## Acknowledgements

- **pala_note** — original e-paper recorder firmware (hardware abstraction, display, button handling, audio BSP)
- **[MeetingRecorder](https://github.com/techiesms/MeetingRecorder)** by **techiesms** — WiFi meeting recorder, ElevenLabs integration, web dashboard architecture
- **[ElevenLabs Scribe](https://elevenlabs.io/docs/api-reference/speech-to-text)** — speech-to-text with speaker diarization
- **[Waveshare ESP32-S3-ePaper-1.54G](https://github.com/waveshareteam/ESP32-S3-ePaper-1.54G)** — official board examples, display driver reference

---

## License

This project inherits from open-source originals. Please check the licenses of the upstream projects before any commercial use. Original modifications in this repo are released under MIT.
