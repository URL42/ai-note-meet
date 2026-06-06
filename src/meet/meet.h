#pragma once
#include <Arduino.h>

// ─── Meeting mode coordinator ─────────────────────────────────────────────
// Called from pala_meet.ino to manage meeting lifecycle.

void meetSetup();          // init RTOS primitives, start web + process tasks
void meetStart();          // begin recording a meeting
void meetStop();           // stop recording, trigger final summary
bool meetIsRecording();    // true while chunks are being captured
bool meetIsProcessing();   // true while final summary is in flight
