#include "Arduino.h"
#include "SD_MMC.h"
#include "WiFi.h"
#include "HTTPClient.h"
#include "WiFiClientSecure.h"
#include <WebServer.h>
#include <vector>
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"

extern "C" {
#include "config.h"
#include "src/i2c_bsp/i2c_bsp.h"
#include "src/audio/audio_bsp.h"
}

#include "src/power/board_power_bsp.h"
#include "src/display/epaper_driver_bsp.h"
#include "logo_bitmap.h"
#include "sounds.h"

#include <Adafruit_GFX.h>
#include <pgmspace.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

#include "types.h"
#include "globals.h"
#include "src/app/draw.h"
#include "src/app/battery.h"
#include "src/app/rtc.h"
#include "src/app/notes.h"
#include "src/app/ui.h"
#include "src/app/buttons.h"
#include "src/app/network.h"
#include "src/app/sleep.h"
#include "src/app/record.h"
#include "src/meet/meet.h"
#include "src/meet/meet_globals.h"
#include "src/config/config.h"
#include "src/web/web_handlers.h"
#include "src/time/ntp_time.h"

// All pin, timing, path and threshold constants live in config.h.

// ─── Content arrays ───────────────────────────────────────────────────────
const char* DEFAULT_TAGS[]    = { "Note", "Work", "Idea", "Buy", "Private" };
const char* MENU_ITEMS[]     = { "Record", "Notes", "Tags", "Meet", "Sync", "Settings" };
const char* SETTINGS_ITEMS[] = { "Sounds", "Device" };

// ─── Global variable definitions ─────────────────────────────────────────
board_power_bsp_t      board(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);
epaper_driver_display* display = nullptr;

std::vector<NoteEntry> noteIndex;

AppState state          = STATE_IDLE;
int      listCursor     = 0;
int      tagCursor      = 2;
int      menuCursor     = 0;
int      settingsCursor = 0;
bool     soundsOn       = true;
int      activeFilter   = -1;
int      lastRecNum     = -1;

uint32_t lastActivityMs      = 0;
bool     wokeFromUltraSleep  = false;
bool     wakeToMenuRequested = false;
bool     wakeToRecRequested  = false;

uint32_t tickerLastMs = 0;
int      tickerOffset = 0;
int      tickerCursor = -1;

// Notes served via main server (port 80) — no separate WebServer instance

// ─── Meeting mode globals ─────────────────────────────────────────────────
// WebServer on port 80 — used by MeetingRecorder web dashboard.
// Declared as "server" to match src/core/globals.h (extern WebServer server).
WebServer server(80);

volatile bool meetingActive    = false;
volatile bool finalStop        = false;
volatile bool processingFinal  = false;
volatile bool needFactoryReset = false;
volatile bool needWifiReconnect = false;
volatile bool needApRestart    = false;
volatile bool needHistoryDelete = false;
String        pendingHistoryDeleteDir;

String fullTranscript      = "";
String rollingSummary      = "";
String finalTranscriptText = "";
String finalSummaryText    = "";
String meetingDir          = "";
String meetingTimestamp    = "";
String meetingDisplayTime  = "";
bool   ntpSynced           = false;
time_t meetingStartEpoch   = 0;
int    wordCount           = 0;
int    chunkIndex          = 0;

String wifiSSID      = "";
String wifiPass      = "";
String elApiKey      = "";
String openaiApiKey  = "";
String apSSID        = "";
String apPass        = "";
int    tzOffsetMin   = 0;   // UTC; set in config.json

char configDefaultTags[CONFIG_MAX_DEFAULT_TAGS][32] = {};
int  configDefaultTagCount = 0;

// ─── AI provider / model config ───────────────────────────────────────────────
// aiProvider: "openai" | "anthropic" | "gemini" | "local"
// aiModel:    model name string for the chosen provider
// localUrl:   base URL for local Ollama / LM Studio (e.g. "http://192.168.1.147:11434")
String aiProvider    = "openai";
String aiModel       = "gpt-4o-mini";  // override via Settings or config.json
String anthropicKey  = "";
String geminiKey     = "";
String localUrl      = "http://192.168.1.147:11434";

// API URL constants (used by api.cpp)
const char* EL_STT_URL     = "https://api.elevenlabs.io/v1/speech-to-text";
const char* OPENAI_URL     = "https://api.openai.com/v1/chat/completions";
const char* ANTHROPIC_URL  = "https://api.anthropic.com/v1/messages";
const char* GEMINI_URL_BASE = "https://generativelanguage.googleapis.com/v1beta/models/";

QueueHandle_t     chunkQueue        = NULL;
SemaphoreHandle_t stateMutex        = NULL;
TaskHandle_t      recordTaskHandle  = NULL;
TaskHandle_t      processTaskHandle = NULL;

bool timeReady    = false;
bool audioPlaying = false;
bool stopPlayback = false;

int detailScrollPage = 0;
int detailTotalLines = 0;

uint32_t lastBatCheckMs    = 0;
bool     batLowWarned      = false;
bool     batWarnActive     = false;
uint32_t batWarnShowUntilMs = 0;

char tags[20][32];
int  tagCount = 0;

// ─── Power latch ──────────────────────────────────────────────────────────
void keepBatteryPowerOn() {
  pinMode(PWR_HOLD_PIN, OUTPUT);
  digitalWrite(PWR_HOLD_PIN, HIGH);
}

// ─── Flow functions ───────────────────────────────────────────────────────
void startRecordFlow() {
  state = STATE_RECORDING;
  showRecording();

  palaSoundSetEnabled(false);
  bool recOk = record();
  palaSoundSetEnabled(true);

  if (!recOk) {
    showError("REC FAIL");
    delay(1600);
    state = STATE_IDLE;
    showIdle();
    return;
  }

  soundSaved();

  state = STATE_SAVED;
  showSaved(lastRecNum);
  delay(900);

  tagCursor = min(2, max(tagCount - 1, 0));
  state = STATE_TAG_SELECT;
  showTagSelect(tagCursor);
}

void startSyncFlow() {
  const int MAX_TRIES = 20;
  showWifiConnecting(0, MAX_TRIES);

  WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < MAX_TRIES) {
    delay(500); tries++;
    showWifiConnecting(tries, MAX_TRIES);
  }

  if (WiFi.status() == WL_CONNECTED) {
    syncTimeFromNTP(6000);
    transcribeAll();
    loadIndex();
    WiFi.disconnect(true);
    showDone();
    soundSuccess();
    delay(1600);
  } else {
    showError("NO WIFI");
    delay(1800);
  }

  if (wakeToMenuRequested) {
    menuCursor = 0;
    state = STATE_MENU;
    showMenu(menuCursor);
  } else {
    showIdle();
  }
}


// ─── Setup ────────────────────────────────────────────────────────────────
void setup() {
  keepBatteryPowerOn();  // GPIO17 power latch — must be first or board loses power

  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Pala Note " FIRMWARE_VERSION " ===");

  pinMode(BTN_REC, INPUT_PULLUP);
  pinMode(BTN_PWR, INPUT_PULLUP);

  board.VBAT_POWER_ON();

  wokeFromUltraSleep  = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1);
  delay(50);

  wakeToMenuRequested = (wokeFromUltraSleep && digitalRead(BTN_PWR) == LOW);
  wakeToRecRequested  = (wokeFromUltraSleep && digitalRead(BTN_REC) == LOW);

  resetActivity();
  delay(20);

  board.POWEER_EPD_ON();
  board.POWEER_Audio_ON();
  delay(200);

  custom_lcd_spi_t dispCfg = {};
  dispCfg.cs       = EPD_CS_PIN;
  dispCfg.dc       = EPD_DC_PIN;
  dispCfg.rst      = EPD_RST_PIN;
  dispCfg.busy     = EPD_BUSY_PIN;
  dispCfg.mosi     = EPD_MOSI_PIN;
  dispCfg.scl      = EPD_SCK_PIN;
  dispCfg.spi_host = EPD_SPI_NUM;
  dispCfg.buffer_len = (200*200)/8;

  display = new epaper_driver_display(200, 200, dispCfg);
  display->EPD_Init();      // full-quality init + power-on
  display->EPD_Clear();     // white buffer
  display->EPD_Display();   // one slow full refresh to clear the panel
  display->EPD_Init_Fast(); // switch to fast LUT (~3-5s) for all subsequent refreshes

  i2c_master_Init();
  delay(50);

  audio_bsp_init();
  audio_play_init();

  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (!SD_MMC.begin("/sdcard", true)) {
    showError("SD ERR");
    while (true) delay(1000);
  }
  if (!SD_MMC.exists(NOTES_DIR)) SD_MMC.mkdir(NOTES_DIR);
  loadTags();
  loadIndex();
  Serial.printf("[SD] %d notes\n", (int)noteIndex.size());

  meetSetup();

  if (wakeToMenuRequested) {
    menuCursor = 0;
    state = STATE_MENU;
    showMenu(menuCursor);
  } else {
    showIdle();
  }
}

// ─── Main loop ────────────────────────────────────────────────────────────
void loop() {

  if (state != STATE_RECORDING
      && state != STATE_MEETING_RECORDING && state != STATE_MEETING_DONE) {
    if (millis() - lastActivityMs > ULTRA_SLEEP_MS
        && !meetIsRecording() && !meetIsProcessing()) {
      enterUltraSleep();
      return;
    }
  }

  // WiFi credential change — reconnect using the newly saved credentials.
  // Blocks up to 10 s; only fires after the user deliberately saves settings.
  if (needWifiReconnect && !meetingActive) {
    needWifiReconnect = false;
    WiFi.disconnect();
    delay(200);
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
    Serial.println("[WiFi] Reconnecting with updated credentials...");
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 20) {
      delay(500); tries++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("[WiFi] Connected: " + WiFi.localIP().toString());
      syncTimeFromNTP(6000);
    } else {
      Serial.println("[WiFi] STA connect failed — AP still active");
      WiFi.mode(WIFI_AP);
    }
  }

  if (state == STATE_NOTE_LIST && activeTickerNeedsScroll(listCursor)) {
    if (millis() - tickerLastMs > TICKER_INTERVAL_MS) {
      tickerLastMs = millis();
      tickerOffset++;
      showNoteList(listCursor);
      return;
    }
  }

  // Battery warning: dismiss after 2.5 s without blocking
  if (batWarnActive && millis() >= batWarnShowUntilMs) {
    batWarnActive = false;
    switch (state) {
      case STATE_IDLE:           showIdle();                     break;
      case STATE_MENU:           showMenu(menuCursor);           break;
      case STATE_NOTE_LIST:      showNoteList(listCursor);       break;
      case STATE_NOTE_DETAIL:    showNoteDetail(listCursor);     break;
      case STATE_TAG_SELECT:     showTagSelect(tagCursor);       break;
      case STATE_TAG_BROWSER:    showTagBrowser(tagCursor);      break;
      case STATE_SETTINGS:       showSettings(settingsCursor);   break;
      case STATE_DEVICE_INFO:    showDeviceInfo();               break;
      case STATE_DELETE_CONFIRM: {
        int idx = noteAtFilteredIndex(listCursor);
        if (idx >= 0) showDeleteConfirm(noteIndex[idx].num);
        break;
      }
      default: break;
    }
  }

  // Periodic battery check
  if (state != STATE_RECORDING && !audioPlaying && !batWarnActive) {
    if (millis() - lastBatCheckMs > BAT_CHECK_INTERVAL_MS) {
      lastBatCheckMs = millis();
      int pct = readBatteryPercent();
      if (pct >= 0 && pct <= BAT_LOW_THRESHOLD && !batLowWarned) {
        batLowWarned        = true;
        batWarnActive       = true;
        batWarnShowUntilMs  = millis() + 2500;
        showBatteryLow(pct);
      } else if (pct > BAT_RECOVER_THRESHOLD) {
        batLowWarned = false;
      }
    }
  }

  // IDLE ─────────────────────────────────────────────────────────────────
  if (state == STATE_IDLE) {
    // Release latch: don't accept REC until it's been seen HIGH (released).
    // Prevents the GPIO0 boot/wake-button from immediately firing recording.
    static bool recArmed = false;
    static bool wasIdle  = false;
    if (!wasIdle) { recArmed = false; wasIdle = true; }

    if (digitalRead(BTN_REC) == HIGH) recArmed = true;

    if (recArmed) {
      ButtonEvent rec = readButtonEvent(BTN_REC);
      ButtonEvent pwr = readButtonEvent(BTN_PWR);
      if (rec != EV_NONE) {
        wasIdle = false;
        soundSelect();
        startRecordFlow();
      } else if (pwr != EV_NONE) {
        wasIdle = false;
        soundSelect();
        menuCursor = 0;
        state = STATE_MENU;
        showMenu(menuCursor);
      }
    } else {
      // Not yet armed: still accept PWR (GPIO18 has no strapping-pin issue)
      ButtonEvent pwr = readButtonEvent(BTN_PWR);
      if (pwr != EV_NONE) {
        wasIdle = false;
        soundSelect();
        menuCursor = 0;
        state = STATE_MENU;
        showMenu(menuCursor);
      }
    }

    if (state != STATE_IDLE) wasIdle = false;
  }

  // TAG SELECT after recording ──────────────────────────────────────────
  else if (state == STATE_TAG_SELECT) {
    ButtonEvent rec = readButtonEvent(BTN_REC);
    ButtonEvent pwr = readButtonEvent(BTN_PWR);

    if (rec == EV_SINGLE || rec == EV_LONG) {
      soundSelect();
      saveTag(lastRecNum, tags[constrain(tagCursor, 0, max(tagCount - 1, 0))]);
      state = STATE_IDLE;
      showIdle();
    } else if (pwr == EV_SINGLE) {
      soundNext();
      if (tagCount > 0) tagCursor = (tagCursor + 1) % tagCount;
      showTagSelect(tagCursor);
    }
  }

  // MENU ────────────────────────────────────────────────────────────────
  else if (state == STATE_MENU) {
    ButtonEvent rec = readButtonEvent(BTN_REC);
    ButtonEvent pwr = readButtonEvent(BTN_PWR);

    if (pwr == EV_SINGLE) {
      soundNext();
      menuCursor = (menuCursor + 1) % MENU_COUNT;
      showMenu(menuCursor);
    } else if (rec == EV_SINGLE) {
      soundSelect();
      if (menuCursor == 0) {
        startRecordFlow();
      } else if (menuCursor == 1) {
        activeFilter = -1; listCursor = 0;
        state = STATE_NOTE_LIST;
        showNoteList(listCursor);
      } else if (menuCursor == 2) {
        tagCursor = 0;
        state = STATE_TAG_BROWSER;
        showTagBrowser(tagCursor);
      } else if (menuCursor == 3) {
        state = STATE_MEETING_IDLE;
        showMeetingIdle();
      } else if (menuCursor == 4) {
        startSyncFlow();
      } else {
        settingsCursor = 0;
        state = STATE_SETTINGS;
        showSettings(settingsCursor);
      }
    } else if (rec == EV_LONG) {
      startRecordFlow();
    } else if (rec == EV_DOUBLE) {
      soundBack();
      state = STATE_IDLE;
      showIdle();
    }
  }

  // SETTINGS ────────────────────────────────────────────────────────────
  else if (state == STATE_SETTINGS) {
    ButtonEvent rec = readButtonEvent(BTN_REC);
    ButtonEvent pwr = readButtonEvent(BTN_PWR);

    if (pwr == EV_SINGLE) {
      soundNext();
      settingsCursor = (settingsCursor + 1) % SETTINGS_COUNT;
      showSettings(settingsCursor);
    } else if (rec == EV_SINGLE) {
      soundSelect();
      if (settingsCursor == 0) {
        palaSoundSetEnabled(!palaSoundIsEnabled());
        showSettings(settingsCursor);
      } else {
        state = STATE_DEVICE_INFO;
        showDeviceInfo();
      }
    } else if (rec == EV_DOUBLE || rec == EV_LONG) {
      soundBack();
      state = STATE_MENU;
      showMenu(menuCursor);
    }
  }

  // MEET IDLE ───────────────────────────────────────────────────────────────
  else if (state == STATE_MEETING_IDLE) {
    ButtonEvent rec = readButtonEvent(BTN_REC);
    ButtonEvent pwr = readButtonEvent(BTN_PWR);

    if (rec == EV_LONG) {
      soundSelect();
      meetStart();
      state = STATE_MEETING_RECORDING;
      showMeetingRecording(0);
    } else if (rec == EV_DOUBLE || pwr == EV_SINGLE) {
      soundBack();
      state = STATE_MENU;
      showMenu(menuCursor);
    }
  }

  // MEET RECORDING ──────────────────────────────────────────────────────────
  else if (state == STATE_MEETING_RECORDING) {
    static uint32_t recStartMs = 0;
    if (recStartMs == 0) recStartMs = millis();

    ButtonEvent rec = readButtonEvent(BTN_REC);

    if (rec == EV_SINGLE || rec == EV_LONG) {
      soundSelect();
      meetStop();
      recStartMs = 0;
      state = STATE_MEETING_DONE;
      showMeetingDone();
    } else {
      uint32_t elapsed = (millis() - recStartMs) / 1000;
      static uint32_t lastDispMs = 0;
      if (millis() - lastDispMs > 30000) {
        lastDispMs = millis();
        showMeetingRecording(elapsed);
      }
    }
  }

  // MEET DONE ───────────────────────────────────────────────────────────────
  else if (state == STATE_MEETING_DONE) {
    ButtonEvent rec = readButtonEvent(BTN_REC);
    ButtonEvent pwr = readButtonEvent(BTN_PWR);

    if (!meetIsProcessing()) {
      // Processing finished — allow navigation back
      if (rec == EV_LONG || rec == EV_DOUBLE || rec == EV_SINGLE || pwr == EV_SINGLE) {
        soundBack();
        state = STATE_MENU;
        showMenu(menuCursor);
      }
    }
  }

  // DEVICE INFO ─────────────────────────────────────────────────────────
  else if (state == STATE_DEVICE_INFO) {
    ButtonEvent rec = readButtonEvent(BTN_REC);
    ButtonEvent pwr = readButtonEvent(BTN_PWR);

    if (rec == EV_DOUBLE || rec == EV_LONG || rec == EV_SINGLE || pwr == EV_SINGLE) {
      soundBack();
      state = STATE_SETTINGS;
      showSettings(settingsCursor);
    }
  }

  // TAG BROWSER ─────────────────────────────────────────────────────────
  else if (state == STATE_TAG_BROWSER) {
    ButtonEvent rec = readButtonEvent(BTN_REC);
    ButtonEvent pwr = readButtonEvent(BTN_PWR);

    if (pwr == EV_SINGLE) {
      soundNext();
      if (tagCount > 0) tagCursor = (tagCursor + 1) % tagCount;
      showTagBrowser(tagCursor);
    } else if (rec == EV_SINGLE) {
      soundSelect();
      activeFilter = tagCursor; listCursor = 0;
      state = STATE_NOTE_LIST;
      showNoteList(listCursor);
    } else if (rec == EV_LONG || rec == EV_DOUBLE) {
      soundBack();
      state = STATE_MENU;
      showMenu(menuCursor);
    }
  }

  // NOTE LIST ───────────────────────────────────────────────────────────
  else if (state == STATE_NOTE_LIST) {
    ButtonEvent rec = readButtonEvent(BTN_REC);
    ButtonEvent pwr = readButtonEvent(BTN_PWR);
    int count = filteredCount();

    if (pwr == EV_SINGLE && count > 0) {
      soundNext();
      listCursor = (listCursor + 1) % count;
      showNoteList(listCursor);
    } else if (pwr == EV_DOUBLE && count > 0) {
      soundNext();
      listCursor = (listCursor - 1 + count) % count;
      showNoteList(listCursor);
    } else if (rec == EV_SINGLE && count > 0) {
      soundSelect();
      detailScrollPage = 0;
      state = STATE_NOTE_DETAIL;
      showNoteDetail(listCursor);
    } else if (rec == EV_LONG || rec == EV_DOUBLE) {
      soundBack();
      state = STATE_MENU;
      showMenu(menuCursor);
    }
  }

  // NOTE DETAIL ─────────────────────────────────────────────────────────
  else if (state == STATE_NOTE_DETAIL) {
    ButtonEvent rec = readButtonEvent(BTN_REC);
    ButtonEvent pwr = readButtonEvent(BTN_PWR);

    if (pwr == EV_SINGLE) {
      soundNext();
      const int linesPerPage = 7;
      int totalPages = (detailTotalLines + linesPerPage - 1) / linesPerPage;
      if (detailScrollPage + 1 < totalPages) {
        detailScrollPage++;
      } else {
        detailScrollPage = 0;
        int count = filteredCount();
        if (count > 0) listCursor = (listCursor + 1) % count;
      }
      showNoteDetail(listCursor);
    } else if (rec == EV_SINGLE) {
      int idx = noteAtFilteredIndex(listCursor);
      if (idx >= 0) {
        char wavPath[64];
        snprintf(wavPath, sizeof(wavPath), "%s/note_%03d.wav", NOTES_DIR, noteIndex[idx].num);
        showPlaybackOverlay();
        playWavFile(wavPath);
        showNoteDetail(listCursor);
      }
    } else if (rec == EV_LONG) {
      int idx = noteAtFilteredIndex(listCursor);
      if (idx >= 0) {
        state = STATE_DELETE_CONFIRM;
        showDeleteConfirm(noteIndex[idx].num);
      }
    } else if (rec == EV_DOUBLE) {
      soundBack();
      detailScrollPage = 0;
      state = STATE_NOTE_LIST;
      showNoteList(listCursor);
    }
  }

  // DELETE CONFIRM ──────────────────────────────────────────────────────
  else if (state == STATE_DELETE_CONFIRM) {
    ButtonEvent rec = readButtonEvent(BTN_REC);
    ButtonEvent pwr = readButtonEvent(BTN_PWR);

    if (rec == EV_SINGLE) {
      int idx = noteAtFilteredIndex(listCursor);
      if (idx >= 0) {
        deleteNote(noteIndex[idx].num);
        soundDelete();
      }
      detailScrollPage = 0;
      listCursor = constrain(listCursor, 0, max(filteredCount() - 1, 0));
      state = STATE_NOTE_LIST;
      showNoteList(listCursor);
    } else if (pwr == EV_SINGLE || rec == EV_DOUBLE || rec == EV_LONG) {
      soundBack();
      state = STATE_NOTE_DETAIL;
      showNoteDetail(listCursor);
    }
  }

  delay(15);
}
