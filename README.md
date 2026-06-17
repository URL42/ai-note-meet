# pala_meet

An ESP32-S3 firmware for the **Waveshare ESP32-S3-ePaper-1.54G-EN** that combines a voice note recorder with a WiFi-connected AI meeting recorder — displayed on a 200×200 4-color e-paper screen (black, white, red, yellow).

Built by merging two open-source projects:
- **pala_note** — e-paper voice recorder with RTC, SD card, and battery UI (original hardware abstraction and display layer)
- **[MeetingRecorder](https://github.com/techiesms/MeetingRecorder)** by [techiesms](https://github.com/techiesms) — WiFi meeting recorder with ElevenLabs STT, AI summaries, and web dashboard

All credit to the original authors for the foundations. This project adds multi-provider AI support, Markdown output, a unified hardware layer for the 4-color G panel, a voice notes portal, and merges both feature sets into a single firmware with a single web interface at `notemeet.local`.

---

## Features

### Notes mode
- Record voice memos directly to SD card as WAV files
- Tag each note on-device after recording (configurable tags via `config.json`)
- Browse and manage notes by tag on the e-paper display
- **Sync**: one-tap transcription via ElevenLabs Scribe — WAVs deleted after successful transcription, TXT transcripts retained
- RTC-stamped filenames
- Battery level indicator
- Deep sleep with wake on button press

### Meeting mode
- Record long meetings in 15-second WAV chunks to SD card (Core 0 audio capture task)
- Automatic transcription via **ElevenLabs Scribe v1** (with speaker diarization) — no sync step needed
- AI summary via a configurable provider — no reflash needed to switch
- Summary generated using single-call or map-reduce strategy depending on meeting length
- Start/stop recording from the device buttons **or** from the web dashboard

### Web dashboard — `notemeet.local`
A single dark-mode SPA served from port 80, accessible on the same WiFi network or via the device's own AP (`notemeet` / `recorder123` → `http://192.168.4.1`):

| Tab | What it does |
|-----|-------------|
| Dashboard | Live recording status, start/stop controls |
| Transcript | Live transcript as meeting chunks arrive |
| Summary | AI-generated meeting summary with **Retry** button to regenerate if the API call fails |
| History | Past meetings — download `.md` files, delete, or regenerate summary |
| Notes | Browse and manage voice notes — filter by tag, play audio, download TXT |
| Settings | WiFi, API keys, AI provider/model, timezone |

### Multi-provider AI
Configurable at runtime via the web dashboard or `config.json` on the SD card — no reflash required:

| Provider   | Models (examples)                                          |
|------------|------------------------------------------------------------|
| OpenAI     | `gpt-5.4-mini` (default), `gpt-4o`, `gpt-4o-mini`         |
| Anthropic  | `claude-opus-4-8`, `claude-sonnet-4-6`, `claude-haiku-4-5` |
| Google     | `gemini-1.5-pro`                                           |
| Ollama     | any locally-served model                                   |

---

## Hardware

| Component | Part |
|-----------|------|
| Board | Waveshare ESP32-S3-ePaper-1.54G-EN |
| MCU | ESP32-S3-PICO-1-N8R8 (8 MB flash, 8 MB OPI PSRAM) |
| Display | 200×200 4-color e-paper (R/Y/B/W), UC8151 controller, full refresh only |
| Audio | ES8311 codec (I2C + I2S) |
| Storage | MicroSD via SDMMC 1-bit |
| RTC | PCF85063 (NTP-synced over WiFi) |

> **Display refresh time:** First boot does one slow full clear (~14s). After that, fast-refresh mode is active (~3–5s per update). The 4-color G panel does not support partial refresh.

> **Panel protection:** The firmware issues a Power-Off command (0x02) after every refresh and Power-On (0x04) before the next one. Leaving the panel drivers active long-term between refreshes can permanently damage the internal layer.

> **Planned upgrade:** The 4-color G panel has ~15s refresh and no partial-refresh support. A Waveshare ESP32-S3-ePaper-1.54 (B&W, SSD1681 controller) is a drop-in hardware replacement that enables true partial refresh at ~300ms — the same driver used in pala_note.

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

### 3. SD Card & credentials

Format the SD card as FAT32. Create `config.json` in the root:

```json
{
  "ssid": "your-wifi-network",
  "pass": "your-wifi-password",
  "openai_key": "sk-...",
  "el_key": "your-elevenlabs-key",
  "ai_provider": "openai",
  "ai_model": "gpt-5.4-mini",
  "tags": ["Note", "Work", "Meeting", "Buy", "Private"]
}
```

All fields are optional at first boot — if `config.json` is missing or incomplete, the device starts in AP-only mode. Connect to the `notemeet` network (password `recorder123`), open `http://192.168.4.1`, and enter credentials via the Settings tab. The firmware writes them to `config.json` automatically.

The `tags` array seeds the on-device tag list on first boot. Once tags are saved to the SD card (`/notes/tags.txt`), that file takes precedence — edit tags via the web dashboard Notes → Tags page.

> **No `secrets.h` required.** All credentials are stored in `config.json` on the SD card and managed at runtime. There is no compile-time credential file.

### 4. Flash

Open `pala_meet.ino` in Arduino IDE and click Upload.

> **Upload tip:** If the upload fails with "port busy" or "no such file", close the Serial Monitor first, then hold BOOT and tap RESET to enter bootloader mode.

---

## Using the device

### Boot sequence

1. Power on with the PWR button (GPIO18).
2. The display clears to white (~14s — only on first boot per flash, then uses fast-refresh ~3–5s).
3. The device connects to WiFi, syncs time via NTP, and starts the AP + web server.
4. The idle screen shows the device name and dashboard URL.

### Button controls

| From | Short REC | Long REC | Double REC | PWR |
|------|-----------|----------|------------|-----|
| Idle | Open menu | Start recording | Open menu | Open menu |
| Menu | Select item | **Start recording** | Back to idle | Next item |
| Recording | Stop recording | Stop recording | — | — |
| Tag select | Confirm tag | — | — | Next tag |
| Settings | Toggle / select | Back to menu | Back to menu | Next item |
| Submenus / lists | Confirm / select | Back | Back | Next item |

**Long-press REC** is a universal shortcut to start a voice note from anywhere in top-level navigation. The Settings screen shows a `back: dbl-click` hint in the footer.

> **GPIO0 note:** REC is on GPIO0 (ESP32-S3 strapping/boot pin). The firmware uses a release-latch pattern — the button must be seen HIGH (released) before any press is accepted in the idle state, preventing boot events from triggering a recording.

### Main menu

| Item | What it does |
|------|-------------|
| Notes | Browse recorded voice notes, filtered by tag |
| Tags | Manage tags (add, delete, view note counts) |
| Meet | Enter meeting recording mode |
| Sync | Transcribe untranscribed voice notes via ElevenLabs |
| Settings | Sounds on/off · Device info |

### Recording a voice note

1. **Long-press REC** from the idle screen or menu.
2. The display shows a recording indicator. Speak your note.
3. **Tap REC** to stop (toggle). The WAV is saved to SD.
4. Choose a tag with PWR (cycle) and confirm with REC.
5. Run **Sync** from the menu to transcribe — ElevenLabs Scribe processes the WAV and saves a TXT transcript. The WAV is deleted after successful transcription.

### Recording a meeting

**From the device:**
1. From the main menu, select **Meet**.
2. **Long-press REC** to start. Audio is recorded in 15-second chunks on Core 0; transcription and rolling summaries run concurrently on Core 1.
3. **Tap REC** to stop. A final AI summary pass runs automatically.

**From the dashboard:**
1. Open `http://notemeet.local` and go to the Dashboard tab.
2. Click **Start** — the device begins recording immediately (no button press needed).
3. Click **Stop** — the device finishes the current chunk and generates the final summary.

Open the **Transcript** and **Summary** tabs to follow along live. If the summary fails (e.g., API key issue), use the **Retry** button on the Summary tab to regenerate from the saved transcript without re-recording.

### Web dashboard

Browse to `http://notemeet.local` (same WiFi as the device) or `http://192.168.4.1` (connect to the `notemeet` AP first).

Settings saved via the dashboard are written to `config.json` on the SD card immediately and take effect without a reboot.

---

## Architecture

### FreeRTOS tasks

| Task | Core | Stack | Role |
|------|------|-------|------|
| `loop()` | 1 | default | State machine, button handling, display updates |
| `recordTask` | 0 | 8 KB | Audio capture: `audio_playback_read` → WAV chunks → `chunkQueue` |
| `processTask` | 1 | 20 KB | ElevenLabs STT + rolling/final GPT summary; also handles SD delete/factory-reset |
| `webTask` | 1 | 6 KB | HTTP server — serves the dashboard SPA and all `/api/*` routes |

`recordTask` idles (20ms delay loop) when no meeting is active. `processTask` blocks on `chunkQueue` with a 100ms timeout.

### Meeting pipeline

```
recordTask (Core 0)
  └─ audio_playback_read() every ~128ms (8 KB stereo → 4 KB mono PCM)
  └─ writes chunk_N.wav every 15 seconds
  └─ xQueueSend(chunkQueue, path)
       │
       ▼
processTask (Core 1)
  └─ xQueueReceive(chunkQueue)
  └─ transcribeAudio()  → ElevenLabs Scribe
  └─ generateSummary()  → rolling summary (GPT)
  └─ on finalStop: generateSummary(full transcript) → final summary
  └─ saves full_transcript.md + summary_final.md to SD card
```

### Display driver

All display calls go through `draw.cpp` → `epaper_driver_bsp.cpp`:
- Converts 1bpp frame buffer → 2bpp on the fly (EXPAND_NIBBLE lookup table)
- Skips refresh when buffer is unchanged since last display call (dirty-flag `memcmp`)
- Issues PON (0x04) before writing data, POF (0x02) after every refresh (panel protection)

---

## Project structure

```
pala_meet.ino          — Main sketch, globals, state machine
types.h                — Shared enums, state definitions, DEFAULT_TAGS
config.h               — Hardware pin assignments, timing constants
globals.h              — Root extern declarations (display, notes, tags)
src/
  api/                 — ElevenLabs STT + multi-provider AI dispatcher
  app/                 — Buttons, battery, draw, notes, record, sleep, UI, network
  app/network.cpp      — ElevenLabs note transcription, sync flow, notes portal
  audio/               — ES8311 audio BSP (I2S read/write, volume)
  codec_board/         — Board-level codec init
  config/              — SD card config.json load/save (credentials + default tags)
  core/globals.h       — Meet-side extern declarations (WiFi, AI keys, queues)
  display/             — E-paper driver BSP (1.54G 4-color UC8151, 2bpp)
  esp_codec_dev/       — ESP codec device drivers
  i2c_bsp/             — I2C bus setup
  json/                — ArduinoJson v7 shim (ShubhJson)
  meet/                — Meeting coordinator + recordTask (record_task.cpp)
  power/               — Board power management (battery latch, EPD power pin)
  process/             — Chunked STT + rolling/final summary pipeline
  time/                — NTP sync helpers
  web/                 — HTTP handlers (web_handlers.cpp) + dashboard SPA (html_pages.h)
```

---

## Acknowledgements

- **pala_note** — original e-paper recorder firmware (hardware abstraction, display, button handling, audio BSP)
- **[MeetingRecorder](https://github.com/techiesms/MeetingRecorder)** by **techiesms** — WiFi meeting recorder, ElevenLabs integration, web dashboard architecture
- **[ElevenLabs Scribe](https://elevenlabs.io/docs/api-reference/speech-to-text)** — speech-to-text with speaker diarization
- **[Waveshare ESP32-S3-ePaper-1.54G](https://github.com/waveshareteam/ESP32-S3-ePaper-1.54G)** — official board examples, display driver reference

---

## License

This project inherits from open-source originals. Please check the licenses of the upstream projects before any commercial use. Original modifications in this repo are released under MIT.
