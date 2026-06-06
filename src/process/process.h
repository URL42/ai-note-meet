#pragma once
#include <Arduino.h>
#include "FS.h"

void processTask(void* pv);
void createMeetingDir();
void deleteDirRecursive(const String& path);
String regenerateSummaryForMeeting(const String& dir);
