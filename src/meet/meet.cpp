#include "meet.h"
#include "meet_globals.h"
#include "../process/process.h"
#include "../config/config.h"
// Use pala_note's syncTimeFromNTP from rtc.h rather than MeetingRecorder's ntp_time.h
// (both achieve NTP sync; rtc.h is already compiled into the project)
#include "../app/rtc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "SD_MMC.h"
#include "WiFi.h"
#include "ESPmDNS.h"
#include <WebServer.h>

// web server and route setup are declared in web_handlers.h
// but we call startWebServer() which registers all routes and starts server
extern void startWebServer();

// webTaskHandle is declared extern in meet_globals.h
TaskHandle_t webTaskHandle = NULL;

// ─── Web task ────────────────────────────────────────────────────────────────
static void webTask(void* pv) {
    Serial.println("[WebTask] Started on core " + String(xPortGetCoreID()));
    for (;;) {
        server.handleClient();
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }
}

void meetSetup() {
    // RTOS primitives
    chunkQueue  = xQueueCreate(8, CHUNK_PATH_LEN);
    stateMutex  = xSemaphoreCreateMutex();

    // Load config from SD
    loadConfig();

    // Connect WiFi
    if (wifiSSID.length() > 0) {
        WiFi.mode(WIFI_AP_STA);
        WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
        Serial.print("[WiFi] Connecting");
        int tries = 0;
        while (WiFi.status() != WL_CONNECTED && tries < 40) {
            delay(500); Serial.print("."); tries++;
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());
            syncTimeFromNTP(6000);
        } else {
            Serial.println("\n[WiFi] STA failed — will retry in AP mode");
            WiFi.mode(WIFI_AP_STA);  // keep STA active so it can reconnect if router comes up
        }
    } else {
        WiFi.mode(WIFI_AP);
        Serial.println("[WiFi] No credentials — AP only");
    }

    // Start AP
    String ssid = apSSID.length() > 0 ? apSSID : "notemeet";
    String pass = apPass.length() > 0 ? apPass : "recorder123";
    WiFi.softAP(ssid.c_str(), pass.c_str());
    Serial.println("[WiFi] AP: " + ssid + " @ " + WiFi.softAPIP().toString());

    // mDNS
    if (MDNS.begin("notemeet")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("[mDNS] notemeet.local");
    }

    // Web server routes + start
    startWebServer();

    // Start background tasks
    // recordTask: Core 0 — audio capture, doesn't compete with WiFi/TLS on Core 1
    // processTask: Core 1 — STT + GPT HTTP calls need the network stack
    // webTask: Core 1 — serves the dashboard
    xTaskCreatePinnedToCore(recordTask,  "record",  8192,  NULL, 2, &recordTaskHandle,  0);
    xTaskCreatePinnedToCore(processTask, "process", 20480, NULL, 1, &processTaskHandle, 1);
    xTaskCreatePinnedToCore(webTask,     "web",      6144, NULL, 3, &webTaskHandle,     1);

    Serial.println("[Meet] Setup complete");
}

void meetStart() {
    if (meetingActive) return;
    setCpuFrequencyMhz(240);
    chunkIndex  = 0;   // reset so chunk filenames start at chunk_0 each meeting
    createMeetingDir();
    meetingActive = true;
    finalStop     = false;
    Serial.println("[Meet] Recording started → " + meetingDir);
}

void meetStop() {
    if (!meetingActive) return;
    meetingActive = false;
    finalStop     = true;
    Serial.println("[Meet] Recording stopped — processing final summary");
}

bool meetIsRecording()  { return meetingActive; }
bool meetIsProcessing() { return processingFinal; }
