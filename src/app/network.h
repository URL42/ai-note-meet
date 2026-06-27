#pragma once
#include <Arduino.h>

bool transcribe(const String& wavPath, int noteNum);
void transcribeAll();

String htmlEscape(const String& s);
String readSmallFile(const char* path, size_t maxLen = 1600);
String urlDecodeSimple(String s);
String portalCss();

void handlePortalRoot();
void handlePortalJson();
void handleExportTxt();
void sendFileByNum(const char* ext, const char* mime, bool attachment);
void handleTagsPage();
void handleTagAdd();
void handleTagDelete();
void handleNoteDelete();

void registerNoteRoutes();  // called by startWebServer() to register /notes/* routes

void postWebhookNote(const String& tag, const String& text, const String& timestamp);
void postWebhookMeeting(const String& title, const String& summary, const String& transcript, const String& timestamp);
