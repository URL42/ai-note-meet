#pragma once
/*
 * globals.h
 * ─────────────────────────────────────────────────────────────────
 * Central header included by every module.
 * Declares (extern) every shared variable so all .cpp files can
 * read/write the same state.  Definitions live in MeetingRecorder.ino
 * ─────────────────────────────────────────────────────────────────
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
// Note: i2s_pdm.h and gpio.h from MeetingRecorder removed here to avoid
// conflicts with pala_note's audio BSP.  The ESP-IDF I2S driver types are
// only needed inside the audio BSP, not in shared globals.
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// ─── AP hotspot (always on) ───────────────────────────────────────────────────
// Default values used when no config.json exists on the SD card.
#define AP_SSID_DEFAULT  "notemeet"
#define AP_PASS_DEFAULT  "recorder123"
#define CONFIG_FILE      "/config.json"

// Runtime AP credentials (saved to /config.json).
// Changed via the Settings UI without reflashing.
extern String apSSID;
extern String apPass;

// Set true by handleApiConfig() when AP settings change so loop() can
// restart softAP after the HTTP response is already sent.
extern volatile bool needApRestart;

// ─── Remote API endpoints ─────────────────────────────────────────────────────
extern const char* EL_STT_URL;      // ElevenLabs speech-to-text
extern const char* OPENAI_URL;      // OpenAI chat completions
extern const char* ANTHROPIC_URL;   // Anthropic Claude messages
extern const char* GEMINI_URL_BASE; // Google Gemini (model + key appended at call time)

// ─── AI provider / model selection ───────────────────────────────────────────
// Loaded from config.json; changed via the Settings UI without reflashing.
// aiProvider: "openai" | "anthropic" | "gemini" | "local"
extern String aiProvider;   // which provider to use for summaries
extern String aiModel;      // model name for the chosen provider
extern String anthropicKey; // Anthropic API key
extern String geminiKey;    // Google Gemini API key
extern String localUrl;     // base URL for local Ollama/LM Studio

// ─── Audio constants ─────────────────────────────────────────────────────────
// Note: SAMPLE_RATE, SAMPLE_BITS, WAV_HEADER_SIZE, CHUNK_SECONDS defined here
// for MeetingRecorder internals.  pala_note's config.h takes precedence for
// hardware pin assignments — the pin defines below are removed to avoid
// conflicts (SD_CS_PIN, BUTTON_PIN, PDM_* differ between boards).
#define MEET_SAMPLE_RATE   16000U
#define MEET_SAMPLE_BITS   16
#ifndef WAV_HEADER_SIZE
#define WAV_HEADER_SIZE    44
#endif
#define I2S_READ_CHUNK     1024
#ifndef CHUNK_SECONDS
#define CHUNK_SECONDS      15
#endif

// ─── Memory limit ─────────────────────────────────────────────────────────────
#ifndef MAX_TRANSCRIPT_RAM
#define MAX_TRANSCRIPT_RAM 12000
#endif

// ─── Runtime WiFi + API credentials (saved to /config.json) ──────────────────
extern String wifiSSID;
extern String wifiPass;
extern String elApiKey;
extern String openaiApiKey;

// ─── Meeting state ────────────────────────────────────────────────────────────
extern volatile bool meetingActive;
extern volatile bool finalStop;
extern volatile bool needWifiReconnect;
// True while processTask is busy generating the final summary (including the
// multi-call map-reduce path).  Used by the LED state machine in loop() to
// blink the recording LED through the whole "stop pressed → summary ready"
// window — not just during the recording itself.
extern volatile bool processingFinal;

// Set by the /api/factory-reset web handler; consumed by processTask, which
// has the 20 KB stack needed to walk the SD card, recursively delete every
// meeting directory, and remove config.json without overflowing.  Doing
// that work in the web task (6 KB stack) crashes with a stack canary
// watchpoint on devices with many stored meetings.
extern volatile bool needFactoryReset;

extern String meetingDir;
extern String fullTranscript;
extern String finalTranscriptText; // full transcript snapshot saved before reset
extern String rollingSummary;
extern String finalSummaryText;
extern int    chunkIndex;
extern int    wordCount;          // approximate word count for UI

// ─── Time / Naming ────────────────────────────────────────────────────────────
// meetingTimestamp  →  used as directory/file names  e.g. "2025-01-15_14-30-00"
// meetingDisplayTime →  shown in UI                  e.g. "Jan 15, 2025 · 2:30 PM"
// ntpSynced         →  true once NTP or browser time was obtained
// meetingStartEpoch →  Unix timestamp when recording began
extern String meetingTimestamp;
extern String meetingDisplayTime;
extern bool   ntpSynced;
extern time_t meetingStartEpoch;

// User-selected timezone offset in MINUTES from UTC (e.g. IST = +330,
// EST = -300).  Loaded from config.json (key "tz_min"), used by ntp_time.h
// when calling configTime() and when re-stamping after browser-time sync.
extern int tzOffsetMin;

// ─── Chunk queue ──────────────────────────────────────────────────────────────
// Replaces the old single-slot (chunkReady + currentChunkPath) handoff.
// Up to CHUNK_QUEUE_SIZE finished WAV paths can queue up so a slow STT
// retry never causes recordTask to silently overwrite an un-processed chunk.
// 8 slots × 15 s = 2 minutes of back-log before anything is dropped.
#define CHUNK_QUEUE_SIZE  8
#define CHUNK_PATH_LEN    128
extern QueueHandle_t chunkQueue;

// ─── RTOS handles ─────────────────────────────────────────────────────────────
extern TaskHandle_t      recordTaskHandle;
extern TaskHandle_t      processTaskHandle;
// Guards the transcript/summary Strings shared between processTask (writer)
// and webTask via /api/status (reader). String += / .remove() reallocate
// the internal buffer, so a reader mid-write can see freed memory.
extern SemaphoreHandle_t stateMutex;
// Note: rx_handle (i2s_chan_handle_t) is managed inside audio_bsp — not shared
// extern i2s_chan_handle_t rx_handle;

// ─── Web server instance ──────────────────────────────────────────────────────
extern WebServer server;