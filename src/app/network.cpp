#include "Arduino.h"
#include "../../config.h"
#include "../../globals.h"
#include "../core/globals.h"
#include "../../types.h"
#include "network.h"
#include "notes.h"
#include "rtc.h"
#include "ui.h"
#include "../api/api.h"   // transcribeAudio() — shared ElevenLabs STT path
#include "WiFi.h"
#include "WiFiClientSecure.h"
#include <WebServer.h>
#include "SD_MMC.h"

// Main server on port 80 — declared in pala_meet.ino, externed in src/core/globals.h
extern WebServer server;

// Note transcription uses the same ElevenLabs Scribe path as meeting chunks
// (transcribeAudio in api.cpp — 3 attempts with WiFi reconnect between tries).
bool transcribe(const String& wavPath, int noteNum) {
  String text = transcribeAudio(wavPath);
  // "["-prefixed results are error/no-speech sentinels — keep the note
  // untranscribed so the next Sync retries it.
  if (text.length() == 0 || text.startsWith("[")) return false;

  String tp = wavPath; tp.replace(".wav", ".txt");
  if (SD_MMC.exists(tp.c_str())) SD_MMC.remove(tp.c_str());  // FILE_WRITE appends
  File tf = SD_MMC.open(tp.c_str(), FILE_WRITE);
  if (tf) { tf.print(text); tf.close(); }

  updateIndexHasText(noteNum);

  // WAV served its purpose — delete it to free SD space.
  SD_MMC.remove(wavPath.c_str());

  return true;
}

void transcribeAll() {
  int pending = 0;
  for (int i=0; i<(int)noteIndex.size(); i++) if(!noteIndex[i].hasText) pending++;
  Serial.printf("[Sync] transcribeAll: %d notes total, %d pending, webhookUrl='%s'\n",
                (int)noteIndex.size(), pending, webhookUrl.c_str());
  int done = 0;
  for (int i=0; i<(int)noteIndex.size(); i++) {
    if (noteIndex[i].hasText) continue;
    char wp[64]; snprintf(wp, sizeof(wp), "%s/note_%03d.wav", NOTES_DIR, noteIndex[i].num);
    if (!SD_MMC.exists(wp)) {
      Serial.printf("[Sync] Note %d: WAV missing, skipping\n", noteIndex[i].num);
      continue;
    }
    showTranscribing(done, pending);
    if (transcribe(String(wp), noteIndex[i].num)) {
      done++;
      if (!webhookUrl.isEmpty()) {
        char txtPath[64];
        snprintf(txtPath, sizeof(txtPath), "%s/note_%03d.txt", NOTES_DIR, noteIndex[i].num);
        String text = readSmallFile(txtPath, 2000);
        String ts   = noteCreatedUtc(noteIndex[i].num);
        postWebhookNote(String(noteIndex[i].tag), text, ts);
      }
    }
  }
}

// ─── Portal helpers ────────────────────────────────────────────────────────

String htmlEscape(const String& s) {
  String out = s;
  out.replace("&", "&amp;"); out.replace("<", "&lt;");
  out.replace(">", "&gt;"); out.replace("\"", "&quot;");
  return out;
}

String readSmallFile(const char* path, size_t maxLen) {
  File f = SD_MMC.open(path);
  if (!f) return "";
  String out;
  while (f.available() && out.length() < maxLen) out += (char)f.read();
  f.close();
  return out;
}

String urlDecodeSimple(String s) {
  s.replace("+", " ");
  String out = "";
  for (int i = 0; i < (int)s.length(); i++) {
    if (s[i] == '%' && i + 2 < (int)s.length()) {
      String hex = s.substring(i + 1, i + 3);
      out += (char)strtol(hex.c_str(), nullptr, 16);
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

String portalCss() {
  return String(
    "<style>"
    ":root{font-family:-apple-system,BlinkMacSystemFont,'Inter','Segoe UI',sans-serif;color:#111;background:#f3f0e9;}"
    "body{margin:0;padding:24px;background:#f3f0e9;}"
    ".wrap{max-width:780px;margin:0 auto;}"
    ".top{display:flex;align-items:flex-end;justify-content:space-between;gap:16px;margin-bottom:24px;}"
    "h1{font-size:44px;letter-spacing:-.06em;line-height:.9;margin:0;font-weight:800;}"
    ".sub{font-size:13px;text-transform:uppercase;letter-spacing:.12em;color:#6a665f;margin-top:10px;}"
    ".pill{display:inline-flex;border:1px solid #111;border-radius:999px;padding:8px 12px;font-size:13px;background:#fffaf1;}"
    ".grid{display:grid;grid-template-columns:1fr;gap:14px;}"
    ".card{background:#fffaf1;border:1.5px solid #111;border-radius:24px;padding:18px;box-shadow:4px 4px 0 #111;}"
    ".row{display:flex;justify-content:space-between;gap:16px;align-items:flex-start;}"
    ".num{font-size:13px;letter-spacing:.08em;text-transform:uppercase;color:#6a665f;margin-bottom:8px;}"
    ".date{font-size:13px;color:#6a665f;margin:-4px 0 12px;}"
    ".title{font-size:24px;line-height:1.05;letter-spacing:-.04em;font-weight:750;margin:0 0 12px;}"
    ".tag{border:1px solid #111;border-radius:999px;padding:5px 9px;font-size:12px;white-space:nowrap;background:#111;color:#fff;}"
    ".text{font-size:15px;line-height:1.45;color:#222;margin:0 0 14px;white-space:pre-wrap;}"
    ".actions{display:flex;flex-wrap:wrap;gap:8px;margin-top:14px;}"
    "a.btn{color:#111;text-decoration:none;border:1px solid #111;border-radius:999px;padding:8px 12px;background:#f3f0e9;font-size:13px;}"
    "a.btn.primary{background:#111;color:#fff;}"
    ".empty{border:1.5px dashed #111;border-radius:24px;padding:34px;text-align:center;color:#6a665f;}"
    "audio{width:100%;margin-top:8px;}"
    "@media(max-width:520px){body{padding:16px}h1{font-size:36px}.card{border-radius:20px}.title{font-size:21px}}"
    "</style>"
  );
}

// ─── Portal handlers ───────────────────────────────────────────────────────

void handlePortalRoot() {
  loadIndex();

  String filter = "All";
  if (server.hasArg("tag")) filter = server.arg("tag");

  String html = "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>Voice Notes · notemeet</title>" + portalCss() + "</head><body><div class='wrap'>";

  html += "<div class='top'><div><h1>voice<br>notes</h1>"
          "<div class='sub'>notemeet · <a href=\"/notes/tags\" style=\"color:inherit\">tags</a>"
          " · <a href=\"/\" style=\"color:inherit\">dashboard</a></div></div>"
          "<div class='pill'>" + String((int)noteIndex.size()) + " notes</div></div>";

  html += "<div class='actions' style='margin-bottom:18px'>";
  html += "<a class='btn " + String(filter == "All" ? "primary" : "") + "' href='/notes'>All</a>";
  for (int t = 0; t < tagCount; t++) {
    String tag = String(tags[t]);
    html += "<a class='btn " + String(filter == tag ? "primary" : "") + "' href='/notes?tag=" + tag + "'>" + htmlEscape(tag) + "</a>";
  }
  html += "</div>";

  html += "<div class='actions' style='margin-bottom:24px'>";
  html += "<a class='btn primary' href='/notes/export.txt'>Download all TXT</a>";
  if (filter != "All")
    html += "<a class='btn' href='/notes/export.txt?tag=" + filter + "'>Download " + htmlEscape(filter) + " TXT</a>";
  html += "</div>";

  int visibleCount = 0;
  for (int i = 0; i < (int)noteIndex.size(); i++)
    if (filter == "All" || filter == String(noteIndex[i].tag)) visibleCount++;

  if (visibleCount <= 0) {
    html += "<div class='empty'>No notes for this filter.</div>";
  } else {
    html += "<div class='grid'>";
    for (int v = 0; v < (int)noteIndex.size(); v++) {
      int i = (int)noteIndex.size() - 1 - v;
      if (!(filter == "All" || filter == String(noteIndex[i].tag))) continue;
      int num = noteIndex[i].num;

      char txtPath[64], wavPath[64];
      snprintf(txtPath, sizeof(txtPath), "%s/note_%03d.txt", NOTES_DIR, num);
      snprintf(wavPath, sizeof(wavPath), "%s/note_%03d.wav", NOTES_DIR, num);

      String transcript = readSmallFile(txtPath, 1200);
      if (transcript.length() == 0)
        transcript = noteIndex[i].hasText ? "(empty transcript)" : "Not transcribed yet.";

      String title = transcript; title.replace("\n", " "); title.trim();
      if (title.length() > 58) title = title.substring(0, 58) + "...";
      if (title.length() == 0 || title == "Not transcribed yet.")
        title = String("Voice note ") + String(num);

      html += "<div class='card'>";
      html += "<div class='row'><div><div class='num'>#" + String(num) + "</div>";
      html += "<h2 class='title'>" + htmlEscape(title) + "</h2>";
      String createdUtc = noteCreatedUtc(num);
      if (createdUtc.length() > 0)
        html += "<div class='date' data-utc='" + createdUtc + "'>" + createdUtc + "</div>";
      else
        html += "<div class='date'>time not set</div>";
      html += "</div>";
      html += "<div class='tag'>" + htmlEscape(String(noteIndex[i].tag)) + "</div></div>";
      html += "<p class='text'>" + htmlEscape(transcript) + "</p>";
      if (SD_MMC.exists(wavPath))
        html += "<audio controls preload='none' src='/notes/audio?num=" + String(num) + "'></audio>";
      html += "<div class='actions'>";
      html += "<a class='btn primary' href='/notes/txt?num=" + String(num) + "'>Download TXT</a>";
      if (SD_MMC.exists(wavPath))
        html += "<a class='btn' href='/notes/wav?num=" + String(num) + "'>Download WAV</a>";
      html += "<a class='btn' style='margin-left:auto;color:#c0392b;border-color:#c0392b' "
              "href='/notes/note/delete?num=" + String(num) + "' "
              "onclick=\"return confirm('Delete note #" + String(num) + "? This cannot be undone.')\">Delete</a>";
      html += "</div></div>";
    }
    html += "</div>";
  }

  html += "<script>"
          "document.querySelectorAll('[data-utc]').forEach(function(el){"
          "var d=new Date(el.dataset.utc);"
          "if(!isNaN(d)){el.textContent=d.toLocaleString([],{year:'numeric',month:'short',day:'2-digit',hour:'2-digit',minute:'2-digit'});}"
          "});"
          "</script>";
  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

// JSON API — used by the dashboard SPA Notes tab
void handlePortalJson() {
  loadIndex();
  String json = "{\"notes\":[";
  for (int v = 0; v < (int)noteIndex.size(); v++) {
    int i = (int)noteIndex.size() - 1 - v;
    if (v > 0) json += ",";
    int num = noteIndex[i].num;
    char txtPath[64], wavPath[64];
    snprintf(txtPath, sizeof(txtPath), "%s/note_%03d.txt", NOTES_DIR, num);
    snprintf(wavPath, sizeof(wavPath), "%s/note_%03d.wav", NOTES_DIR, num);
    bool hasWav = SD_MMC.exists(wavPath);
    String transcript = "";
    if (noteIndex[i].hasText) {
      transcript = readSmallFile(txtPath, 500);
      // JSON-escape the snippet
      transcript.replace("\\", "\\\\");
      transcript.replace("\"", "\\\"");
      transcript.replace("\n", " ");
      transcript.replace("\r", "");
    }
    String created = noteCreatedUtc(num);
    json += "{";
    json += "\"num\":" + String(num) + ",";
    json += "\"tag\":\"" + htmlEscape(String(noteIndex[i].tag)) + "\",";
    json += "\"hasText\":" + String(noteIndex[i].hasText ? "true" : "false") + ",";
    json += "\"hasWav\":" + String(hasWav ? "true" : "false") + ",";
    json += "\"transcript\":\"" + transcript + "\",";
    json += "\"created\":\"" + created + "\"";
    json += "}";
  }
  json += "],\"tags\":[";
  for (int t = 0; t < tagCount; t++) {
    if (t > 0) json += ",";
    json += "\"" + htmlEscape(String(tags[t])) + "\"";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleExportTxt() {
  loadIndex();
  String filter = "All";
  if (server.hasArg("tag")) filter = server.arg("tag");

  String exportText = "Voice Notes Export · notemeet\nFilter: " + filter + "\n------------------------------\n\n";

  for (int v = 0; v < (int)noteIndex.size(); v++) {
    int i = (int)noteIndex.size() - 1 - v;
    if (!(filter == "All" || filter == String(noteIndex[i].tag))) continue;
    int num = noteIndex[i].num;
    char txtPath[64]; snprintf(txtPath, sizeof(txtPath), "%s/note_%03d.txt", NOTES_DIR, num);
    String transcript = readSmallFile(txtPath, 4000);
    if (transcript.length() == 0)
      transcript = noteIndex[i].hasText ? "(empty transcript)" : "Not transcribed yet.";
    exportText += "#";
    if (num < 100) exportText += "0";
    if (num < 10)  exportText += "0";
    exportText += String(num) + " · " + String(noteIndex[i].tag) + "\n";
    String createdUtc = noteCreatedUtc(num);
    if (createdUtc.length() > 0) exportText += createdUtc + "\n";
    exportText += "\n" + transcript + "\n\n------------------------------\n\n";
    if (exportText.length() > 55000) {
      exportText += "\nExport truncated on device because it became too large.\n";
      break;
    }
  }

  String filename = "notes_export";
  if (filter != "All") filename += "_" + filter;
  filename += ".txt";
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server.send(200, "text/plain", exportText);
}

void sendFileByNum(const char* ext, const char* mime, bool attachment) {
  if (!server.hasArg("num")) { server.send(400, "text/plain", "Missing num"); return; }
  int num = server.arg("num").toInt();
  if (num <= 0) { server.send(400, "text/plain", "Invalid num"); return; }
  char path[64]; snprintf(path, sizeof(path), "%s/note_%03d.%s", NOTES_DIR, num, ext);
  File f = SD_MMC.open(path);
  if (!f) { server.send(404, "text/plain", "File not found"); return; }
  if (attachment) {
    String filename = String("note_") + String(num) + "." + String(ext);
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  }
  server.streamFile(f, mime);
  f.close();
}

void handleTagAdd() {
  if (!server.hasArg("name")) {
    server.sendHeader("Location", "/notes/tags?msg=missing");
    server.send(303); return;
  }
  String name = urlDecodeSimple(server.arg("name"));
  bool ok = addCustomTag(name.c_str());
  server.sendHeader("Location", ok ? "/notes/tags?msg=added" : "/notes/tags?msg=exists");
  server.send(303);
}

void handleTagDelete() {
  if (!server.hasArg("name")) {
    server.sendHeader("Location", "/notes/tags?msg=missing");
    server.send(303); return;
  }
  String name = urlDecodeSimple(server.arg("name"));
  bool hadNotes = tagHasNotes(name.c_str());
  bool ok = deleteTag(name.c_str());
  if (ok && hadNotes) server.sendHeader("Location", "/notes/tags?msg=moved");
  else                server.sendHeader("Location", ok ? "/notes/tags?msg=deleted" : "/notes/tags?msg=protected");
  server.send(303);
}

void handleTagsPage() {
  loadTags();
  loadIndex();
  activeFilter = -1;

  String html = "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>Tags · notemeet</title>"
                "<style>"
                "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;margin:0;padding:24px;background:#f3f0e9;color:#111}"
                ".wrap{max-width:720px;margin:0 auto}"
                "h1{font-size:42px;line-height:.9;letter-spacing:-.05em;margin:0 0 22px;font-weight:800}"
                ".card{background:#fffaf1;border:1.5px solid #111;border-radius:24px;padding:18px;margin:14px 0;box-shadow:4px 4px 0 #111}"
                ".row{display:flex;justify-content:space-between;align-items:center;gap:12px;border-top:1px solid #ddd;padding:12px 0}"
                ".row:first-child{border-top:0}"
                ".tag{font-size:20px;font-weight:700}"
                ".meta{font-size:13px;color:#666;margin-top:4px}"
                "input{font:inherit;padding:12px;border:1.5px solid #111;border-radius:999px;background:#fff;width:100%;box-sizing:border-box}"
                "button,.btn{font:inherit;border:1.5px solid #111;border-radius:999px;padding:10px 14px;background:#111;color:#fff;text-decoration:none;white-space:nowrap}"
                ".danger{background:#fffaf1;color:#111}"
                ".msg{border:1.5px solid #111;border-radius:18px;padding:12px 14px;background:#fff;margin:12px 0}"
                ".hint{font-size:13px;color:#666;line-height:1.4}"
                "form.add{display:flex;gap:10px}"
                "</style></head><body><div class='wrap'>";

  html += "<h1>pala<br>tags</h1>";
  html += "<a class='btn' href='/notes'>Back to notes</a>";

  if (server.hasArg("msg")) {
    String msg = server.arg("msg");
    html += "<div class='msg'>";
    if (msg == "added") html += "Tag added.";
    else if (msg == "exists")    html += "Tag already exists or cannot be added.";
    else if (msg == "deleted")   html += "Tag deleted.";
    else if (msg == "moved")     html += "Tag deleted. Existing notes were moved to Untagged.";
    else if (msg == "protected") html += "This tag cannot be deleted.";
    else html += "Please enter a tag name.";
    html += "</div>";
  }

  html += "<div class='card'><form class='add' action='/notes/tag/add' method='get'>"
          "<input name='name' maxlength='31' placeholder='New tag name'>"
          "<button type='submit'>Add</button></form>"
          "<p class='hint'>Tags appear on the device after recording. Keep them short for the e-paper UI.</p></div>";

  html += "<div class='card'>";
  for (int i = 0; i < tagCount; i++) {
    int cnt = 0;
    for (int n = 0; n < (int)noteIndex.size(); n++)
      if (strcmp(noteIndex[n].tag, tags[i]) == 0) cnt++;
    html += "<div class='row'><div><div class='tag'>" + htmlEscape(String(tags[i])) + "</div>";
    html += "<div class='meta'>" + String(cnt) + (cnt == 1 ? " note" : " notes");
    if (cnt > 0) html += " · deleting moves them to Untagged";
    html += "</div></div>";
    if (strcasecmp(tags[i], "Untagged") != 0) {
      html += "<a class='btn danger' href='/notes/tag/delete?name=" + htmlEscape(String(tags[i])) + "' "
              "onclick=\"return confirm('Delete this tag? Notes will not be deleted. Existing notes will move to Untagged.');\">Delete</a>";
    }
    html += "</div>";
  }
  html += "</div></div></body></html>";
  server.send(200, "text/html", html);
}

void handleNoteDelete() {
  if (!server.hasArg("num")) { server.send(400, "text/plain", "Missing num"); return; }
  int num = server.arg("num").toInt();
  if (num <= 0) { server.send(400, "text/plain", "Invalid num"); return; }
  deleteNote(num);
  server.sendHeader("Location", "/notes");
  server.send(303);
}

// JSON delete endpoint for the SPA Notes tab
static void handleApiNoteDelete() {
  if (!server.hasArg("num")) {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"missing num\"}"); return;
  }
  int num = server.arg("num").toInt();
  if (num <= 0) {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"invalid num\"}"); return;
  }
  deleteNote(num);
  server.send(200, "application/json", "{\"ok\":true}");
}

void registerNoteRoutes() {
  server.on("/notes",             HTTP_GET, handlePortalRoot);
  server.on("/notes/tags",        HTTP_GET, handleTagsPage);
  server.on("/notes/tag/add",     HTTP_GET, handleTagAdd);
  server.on("/notes/tag/delete",  HTTP_GET, handleTagDelete);
  server.on("/notes/note/delete", HTTP_GET, handleNoteDelete);
  server.on("/api/notes",         HTTP_GET, handlePortalJson);
  server.on("/api/notes/delete",  HTTP_GET, handleApiNoteDelete);
  server.on("/notes/export.txt",  HTTP_GET, handleExportTxt);
  server.on("/notes/txt",   HTTP_GET, [](){ sendFileByNum("txt", "text/plain", true); });
  server.on("/notes/wav",   HTTP_GET, [](){ sendFileByNum("wav", "audio/wav",  true); });
  server.on("/notes/audio", HTTP_GET, [](){ sendFileByNum("wav", "audio/wav",  false); });
}

// ─── Webhook helpers ──────────────────────────────────────────────────────────

static String webhookJsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 32);
  for (size_t i = 0; i < s.length(); i++) {
    unsigned char c = (unsigned char)s[i];
    if      (c == '"')  out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (c == '\b') out += "\\b";
    else if (c == '\f') out += "\\f";
    else if (c < 0x20) {
      char esc[7];
      snprintf(esc, sizeof(esc), "\\u%04x", c);
      out += esc;
    }
    else out += (char)c;
  }
  return out;
}

// Records the outcome of the most recent webhook POST so the dashboard can
// show it instead of the user having to go read n8n's execution log.
static void setWebhookResult(const String& msg) {
  Serial.println("[Webhook] " + msg);
  if (stateMutex) xSemaphoreTake(stateMutex, portMAX_DELAY);
  webhookLastResult = msg;
  if (stateMutex) xSemaphoreGive(stateMutex);
}

// One POST attempt.  Returns the HTTP status code, or a negative value when
// the request never got far enough to receive one:
//   -1 connect failed   -2 no response before the timeout
static int webhookPostOnce(const String& payload) {
  String url = webhookUrl;
  bool isHttps = url.startsWith("https://");
  url.replace("https://", "");
  url.replace("http://", "");

  int slashIdx = url.indexOf('/');
  String host = slashIdx >= 0 ? url.substring(0, slashIdx) : url;
  String path = slashIdx >= 0 ? url.substring(slashIdx) : "/";
  int port = isHttps ? 443 : 80;
  int colonIdx = host.indexOf(':');
  if (colonIdx >= 0) {
    port = host.substring(colonIdx + 1).toInt();
    host = host.substring(0, colonIdx);
  }

  // Pick the client by scheme — WiFiClientSecure always negotiates TLS, so
  // using it against a plain-http endpoint (e.g. a local n8n) fails silently.
  WiFiClient       plainClient;
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  WiFiClient& client = isHttps ? (WiFiClient&)secureClient : plainClient;
  client.setTimeout(WEBHOOK_TIMEOUT_MS);

  Serial.printf("[Webhook] POST %s:%d%s (%d bytes)\n",
                host.c_str(), port, path.c_str(), payload.length());
  if (!client.connect(host.c_str(), port)) return -1;

  client.printf("POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
                "Content-Length: %u\r\nConnection: close\r\n\r\n",
                path.c_str(), host.c_str(), payload.length());
  client.print(payload);

  // Wait for the status line.  With the n8n webhook set to respond when the
  // workflow finishes, this code reflects whether the note was actually
  // written — which is the whole point of checking it.
  int code = -2;
  uint32_t deadline = millis() + WEBHOOK_TIMEOUT_MS;
  while (client.connected() && millis() < deadline) {
    if (client.available()) {
      String line = client.readStringUntil('\n');
      if (line.startsWith("HTTP/")) {
        int sp = line.indexOf(' ');
        if (sp > 0) code = line.substring(sp + 1, sp + 4).toInt();
        break;
      }
    }
    delay(10);
  }
  client.stop();
  return code;
}

// POST with retries.  Returns true only on a 2xx response.
static bool webhookPost(const String& payload) {
  if (webhookUrl.isEmpty()) return false;

  for (int attempt = 1; attempt <= WEBHOOK_MAX_ATTEMPTS; attempt++) {
    int code = webhookPostOnce(payload);

    if (code >= 200 && code < 300) {
      setWebhookResult("ok (HTTP " + String(code) + ")");
      return true;
    }

    String why = code == -1 ? "connect failed"
               : code == -2 ? "no response (timeout)"
                            : "HTTP " + String(code);
    if (attempt < WEBHOOK_MAX_ATTEMPTS) {
      Serial.printf("[Webhook] attempt %d/%d failed: %s — retrying\n",
                    attempt, WEBHOOK_MAX_ATTEMPTS, why.c_str());
      delay(attempt * 1000);
    } else {
      setWebhookResult("FAILED after " + String(WEBHOOK_MAX_ATTEMPTS)
                       + " attempts: " + why);
    }
  }
  return false;
}

// ─── runWebhookTest ───────────────────────────────────────────────────────────
// Sends a synthetic payload so the whole chain (device → n8n → wherever the
// note lands) can be verified from the dashboard without recording anything.
// Called from processTask — TLS needs more stack than webTask has.
void runWebhookTest() {
  if (webhookUrl.isEmpty()) {
    setWebhookResult("no webhook URL configured");
    return;
  }
  String j = "{\"type\":\"test\",\"tag\":\"Test\","
             "\"text\":\"Test payload from the NoteMeet dashboard.\","
             "\"timestamp\":\"" + webhookJsonEscape(currentUtcIso()) + "\"}";
  webhookPost(j);
}

void postWebhookNote(const String& tag, const String& text, const String& timestamp) {
  String j = "{\"type\":\"note\",\"tag\":\"" + webhookJsonEscape(tag) +
             "\",\"text\":\"" + webhookJsonEscape(text) +
             "\",\"timestamp\":\"" + webhookJsonEscape(timestamp) + "\"}";
  webhookPost(j);
}

void postWebhookMeeting(const String& title, const String& summary, const String& transcript, const String& timestamp) {
  String j = "{\"type\":\"meeting\",\"title\":\"" + webhookJsonEscape(title) +
             "\",\"summary\":\""    + webhookJsonEscape(summary) +
             "\",\"transcript\":\"" + webhookJsonEscape(transcript) +
             "\",\"timestamp\":\""  + webhookJsonEscape(timestamp) + "\"}";
  webhookPost(j);
}
