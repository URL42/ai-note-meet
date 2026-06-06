#pragma once
// ─── Meeting mode shared state ─────────────────────────────────────────────
// Pulls in MeetingRecorder's central globals header, which declares all
// shared variables (extern).  Definitions live in pala_meet.ino.
//
// Any translation unit that needs meeting state should #include this header.

#include "../core/globals.h"

// ─── Additional constants not in core/globals.h ─────────────────────────────
#define SAMPLE_RATE_MEET   16000
#define SAMPLE_BITS_MEET   16
#define I2S_READ_LEN       1024
#define CHUNK_SECONDS_MEET 15   // avoid clash — process.cpp has its own CHUNK_SECONDS

// ─── RTOS task handle for web and process tasks ──────────────────────────────
extern TaskHandle_t webTaskHandle;
