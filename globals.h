#pragma once
#include <vector>
#include <WebServer.h>
#include "types.h"
#include "src/power/board_power_bsp.h"
#include "src/display/epaper_driver_bsp.h"

extern std::vector<NoteEntry> noteIndex;

extern AppState  state;
extern int       listCursor;
extern int       tagCursor;
extern int       menuCursor;
extern int       settingsCursor;
extern bool      soundsOn;
extern int       activeFilter;
extern int       lastRecNum;

extern uint32_t  lastActivityMs;
extern bool      wokeFromUltraSleep;
extern bool      wakeToMenuRequested;
extern bool      wakeToRecRequested;

extern uint32_t  tickerLastMs;
extern int       tickerOffset;
extern int       tickerCursor;

// Notes served via main server (port 80) — no separate transfer server

extern bool      timeReady;

extern bool      audioPlaying;
extern bool      stopPlayback;

extern int       detailScrollPage;
extern int       detailTotalLines;

extern uint32_t  lastBatCheckMs;
extern bool      batLowWarned;
extern bool      batWarnActive;
extern uint32_t  batWarnShowUntilMs;

extern char      tags[20][32];
extern int       tagCount;

// Default tags loaded from config.json; used by createDefaultTags() when tags.txt is absent.
#ifndef CONFIG_MAX_DEFAULT_TAGS
#define CONFIG_MAX_DEFAULT_TAGS 20
#endif
extern char configDefaultTags[CONFIG_MAX_DEFAULT_TAGS][32];
extern int  configDefaultTagCount;

extern board_power_bsp_t      board;
extern epaper_driver_display* display;
