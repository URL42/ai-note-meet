#pragma once
#include <Arduino.h>

// ─── Meeting mode coordinator ─────────────────────────────────────────────
// Called from pala_meet.ino to manage meeting lifecycle.

void meetSetup();          // init RTOS primitives, start web + process + record tasks
void meetStart();          // begin recording a meeting
void meetStop();           // stop recording, trigger final summary
bool meetIsRecording();    // true while chunks are being captured
bool meetIsProcessing();   // true while final summary is in flight

void recordTask(void* pv); // Core 0 audio capture task (defined in record_task.cpp)

// Re-broadcast the softAP with the current apSSID/apPass and restart the
// captive-portal DNS.  Call from loop() (never from a web handler — the
// client is on the AP being torn down and would never see the response).
void restartSoftAP();
