/*
 * web_handlers.cpp
 * ─────────────────────────────────────────────────────────────────
 * All HTTP route handlers for the dashboard: meeting start/stop,
 * settings, history, regeneration scheduling, resets, WiFi scan.
 * SD-heavy / GPT-heavy work is flagged over to processTask — never
 * run it here (6 KB stack, and it blocks every other request).
 * ─────────────────────────────────────────────────────────────────
 */

#include "web_handlers.h"
#include "../core/globals.h"
#include "../app/network.h"
#include "html_pages.h"              // defines PAGE_MAIN and PAGE_SETUP
#include "../config/config.h"
#include "../api/api.h"              // jsonEscape()
#include "../process/process.h"      // deleteDirRecursive()
#include "web_extras.h"              // handleApiStatus(), handleApiChat(), handleApiSetTime()
#include "../time/ntp_time.h"        // stampNow()
#include "FS.h"
#include "SD_MMC.h"
#include <WiFi.h>

// ─── GET / ────────────────────────────────────────────────────────────────────
static void handleRoot() {
    server.send_P(200, "text/html", DASHBOARD_HTML);
}

static void handleSetup() {
    String page = String(FPSTR(SETUP_HTML));
    page.replace("%%SSID%%", wifiSSID);
    server.send(200, "text/html", page);
}

// ─── POST /api/start ─────────────────────────────────────────────────────────
static void handleApiStart() {
    if (!meetingActive) {
        // Full-speed CPU for I2S DMA + STT/GPT TLS work.
        // Dropped back to 80 MHz at the end of the final-summary block in
        // processTask (see process.cpp).
        setCpuFrequencyMhz(240);
        Serial.println("[Power] Active: CPU @ 240 MHz");
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        fullTranscript      = "";
        finalTranscriptText = "";   // clear previous meeting's transcript
        finalSummaryText    = "";   // clear previous meeting's summary
        chunkIndex          = 0;
        wordCount           = 0;
        rollingSummary      = "Meeting started — summary will appear after the first chunk.";
        xSemaphoreGive(stateMutex);
        xQueueReset(chunkQueue);   // discard any leftover chunk paths
        finalStop           = false;
        createMeetingDir();
        meetingActive       = true;
        // LED is managed by the blink state machine in loop().
        Serial.println("[Web] ● Meeting STARTED via web UI");
    }
    server.send(200, "application/json", "{\"ok\":true}");
}

// ─── POST /api/stop ──────────────────────────────────────────────────────────
static void handleApiStop() {
    if (meetingActive) {
        meetingActive = false;
        finalStop     = true;
        // LED keeps blinking while processTask finishes the final summary.
        Serial.println("[Web] ■ Meeting STOPPED via web UI");
    }
    server.send(200, "application/json", "{\"ok\":true}");
}

// ─── POST /api/config ────────────────────────────────────────────────────────
static void handleApiConfig() {
    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "POST only");
        return;
    }
    String ssid     = server.arg("ssid");
    String pass     = server.arg("pass");
    String el       = server.arg("el_key");
    String oai      = server.arg("openai_key");
    String ant      = server.arg("anthropic_key");
    String gem      = server.arg("gemini_key");
    String tz       = server.arg("tz_min");
    String prov     = server.arg("ai_provider");
    String model    = server.arg("ai_model");
    String locUrl   = server.arg("local_url");
    String webhook  = server.arg("webhook_url");
    String newApSsid = server.arg("ap_ssid");
    String newApPass = server.arg("ap_pass");

    // AP credentials.  Reject out-of-range values with a 400 rather than
    // applying them partially: an over-long SSID is written to config.json
    // before the AP restart fails, so the device would come up with a broken
    // hotspot on every subsequent boot — recoverable only via the station
    // link or by editing the SD card by hand.
    bool apChanged = false;
    if (newApSsid.length() > 0 || newApPass.length() > 0) {
        if (newApSsid.length() > 32) {
            server.send(400, "application/json",
                        "{\"ok\":false,\"error\":\"hotspot name must be 32 characters or fewer\"}");
            return;
        }
        if (newApPass.length() > 0 && (newApPass.length() < 8 || newApPass.length() > 63)) {
            server.send(400, "application/json",
                        "{\"ok\":false,\"error\":\"hotspot password must be 8-63 characters\"}");
            return;
        }
        if (newApSsid.length() > 0 && newApSsid != apSSID) { apSSID = newApSsid; apChanged = true; }
        if (newApPass.length() > 0 && newApPass != apPass) { apPass = newApPass; apChanged = true; }
    }

    bool wifiChanged = false;
    if (ssid.length() > 0 && ssid != wifiSSID) { wifiSSID = ssid; wifiChanged = true; }
    if (pass.length() > 0)                     { wifiPass = pass; wifiChanged = true; }
    if (el.length()  > 4)  elApiKey     = el;
    if (oai.length() > 4)  openaiApiKey = oai;
    if (ant.length() > 4)  anthropicKey = ant;
    if (gem.length() > 4)  geminiKey    = gem;
    if (prov.length() > 0) aiProvider   = prov;
    if (model.length() > 0)aiModel      = model;
    if (locUrl.length() > 0) localUrl   = locUrl;
    webhookUrl = webhook;  // empty string is valid (disables webhook)

    // Timezone — minutes from UTC; clamp to a sane range (−12 h … +14 h)
    bool tzChanged = false;
    if (tz.length() > 0) {
        int v = tz.toInt();
        if (v >= -720 && v <= 840 && v != tzOffsetMin) {
            tzOffsetMin = v;
            tzChanged   = true;
        }
    }

    saveConfig();

    server.send(200, "application/json", "{\"ok\":true}");

    // If timezone changed and we're connected, re-init NTP so the clock
    // jumps to the new offset immediately instead of waiting for the
    // next reboot.
    if (tzChanged && WiFi.status() == WL_CONNECTED) {
        ntpInit();
    }

    // WiFi reconnect and AP restart are handled in loop() — safely after the
    // HTTP response has been sent.
    if (wifiChanged && wifiSSID.length() > 0) needWifiReconnect = true;
    if (apChanged) needApRestart = true;

    Serial.printf("[Config] Saved — wifi changed: %s, ap changed: %s, tz changed: %s (now %+d min)\n",
                  wifiChanged ? "YES" : "no", apChanged ? "YES" : "no",
                  tzChanged ? "YES" : "no", tzOffsetMin);
}

// ─── GET /api/history ────────────────────────────────────────────────────────
// Scans SD root for meeting_* directories and returns their final summaries.
//
// TWO-PASS on purpose: opening summary files while the root directory
// iterator is still active corrupts the iterator on ESP32's SD/FatFS layer
// (entries get skipped).  Pass 1 collects dir names and closes root;
// pass 2 opens each summary file independently.
static void handleApiHistory() {

    // ── Pass 1: collect meeting directory names ────────────────────
    const int MAX_MEETINGS = 30;
    String    dirs[MAX_MEETINGS];
    int       dirCount = 0;

    File root = SD_MMC.open("/");
    if (!root) {
        server.send(200, "application/json", "[]");
        return;
    }

    File entry = root.openNextFile();
    while (entry && dirCount < MAX_MEETINGS) {
        String name = String(entry.name());
        if (name.startsWith("/")) name = name.substring(1);   // strip leading /
        bool isDir  = entry.isDirectory();
        entry.close();   // close BEFORE opening any other file

        if (isDir && name.startsWith("meeting_")) {
            dirs[dirCount++] = name;
        }
        entry = root.openNextFile();
    }
    // Also close any remaining entry (loop exited early via count limit)
    if (entry) entry.close();
    root.close();   // ← root is now fully closed before any sub-file open

    // Sort descending (newest timestamp first — lexicographic sort works
    // because directory names contain ISO-style timestamps).
    for (int i = 0; i < dirCount - 1; i++) {
        for (int j = i + 1; j < dirCount; j++) {
            if (dirs[j] > dirs[i]) { String tmp = dirs[i]; dirs[i] = dirs[j]; dirs[j] = tmp; }
        }
    }

    // ── Pass 2: read each summary (root is closed — no iterator conflict) ─
    // Streamed as a chunked response, one meeting at a time — 30 meetings
    // × 8 KB summaries would otherwise build a ~240 KB String in RAM.
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    server.sendContent("[");
    bool first = true;

    for (int i = 0; i < dirCount; i++) {
        const String& name = dirs[i];
        String summary = "";

        // Primary: summary_final.md (what process.cpp always writes)
        File sf = SD_MMC.open(("/" + name + "/summary_final.md").c_str(), FILE_READ);

        // Fallback: timestamped name from older firmware versions
        if (!sf) {
            String pastTs = name.substring(8);   // strip "meeting_"
            if (pastTs.length() > 0) {
                sf = SD_MMC.open(("/" + name + "/summary_" + pastTs + ".md").c_str(), FILE_READ);
            }
        }

        if (sf) {
            // Read the FULL summary file.  Comprehensive final summaries
            // (especially from the map-reduce path on long meetings) are
            // routinely 2-4 KB; the old 1200-char cap was cutting them off
            // mid-word in the History tab even though the Summary tab
            // showed them complete.  We keep a generous safety cap at 8 KB
            // per meeting so the /api/history response can't blow up the
            // HTTP buffer if some file is unexpectedly huge.
            char buf[256];
            while (sf.available() && (int)summary.length() < 8000) {
                int n = sf.readBytes(buf, sizeof(buf) - 1);
                if (n <= 0) break;
                buf[n] = '\0';
                summary += buf;
            }
            sf.close();
            summary.trim();
        }

        // Skip meetings with no usable summary so the History tab doesn't
        // get cluttered with empty entries.  Two ways a meeting can end up
        // useless: (a) user pressed start/stop without speaking, so no
        // summary_final.md was ever written (summary stays "") — or
        // (b) GPT failed and we wrote the fallback "Meeting recorded but
        // summary failed..." marker.  Hide both — the underlying
        // directories stay on the SD card and can still be removed
        // manually if desired.
        bool isUseful = summary.length() >= 50
                     && !summary.startsWith("Meeting recorded but summary failed")
                     && !summary.startsWith("Not enough speech");
        if (!isUseful) {
            Serial.printf("[/api/history] Skipping empty meeting: %s\n", name.c_str());
            continue;
        }

        bool hasTranscript = SD_MMC.exists(("/" + name + "/full_transcript.md").c_str());

        String entry = String(first ? "" : ",")
              +  "{\"dir\":\""         + jsonEscape(name)    + "\","
              +  "\"summary\":\""      + jsonEscape(summary) + "\","
              +  "\"hasTranscript\":"  + (hasTranscript ? "true" : "false") + "}";
        server.sendContent(entry);
        first = false;
    }

    server.sendContent("]");
    server.sendContent("");   // terminating zero-length chunk
}

// ─── POST /api/history/delete ────────────────────────────────────────────────
static void handleApiHistoryDelete() {
    server.sendHeader("Access-Control-Allow-Origin",  "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server.sendHeader("Cache-Control", "no-cache");

    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "POST only");
        return;
    }

    // The JS sends the dir name as BOTH a URL query param and a POST body arg.
    // ESP32 WebServer::arg() searches _getArgs (URL) then _postArgs (body), so
    // one call is sufficient — but on some builds only one source is populated.
    // Read both and prefer whichever is non-empty.
    String dir = server.arg("dir");
    if (dir.isEmpty()) {
        // Manual parse from raw body as last resort
        if (server.hasArg("plain")) {
            String body = server.arg("plain");
            int di = body.indexOf("dir=");
            if (di >= 0) {
                int de = body.indexOf('&', di);
                dir = de < 0 ? body.substring(di + 4) : body.substring(di + 4, de);
                // URL-decode the '+' and percent-encoded chars (basic)
                dir.replace("+", " ");
            }
        }
    }

    // Safety guard — must start with "meeting_", no slashes, no parent traversal
    if (dir.isEmpty()
     || !dir.startsWith("meeting_")
     || dir.indexOf('/') >= 0
     || dir.indexOf('\\') >= 0
     || dir.indexOf("..") >= 0) {
        Serial.printf("[History] Refused delete (bad dir): '%s'\n", dir.c_str());
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid dir\"}");
        return;
    }

    // Offload to processTask (20 KB stack) — same pattern as factory reset.
    // deleteDirRecursive() on the 6 KB webTask stack can canary-crash when
    // a meeting directory contains many chunk files.
    pendingHistoryDeleteDir = "/" + dir;
    needHistoryDelete = true;
    Serial.printf("[History] Delete scheduled for processTask: %s\n", pendingHistoryDeleteDir.c_str());
    server.send(200, "application/json", "{\"ok\":true}");
}

// ─── POST /api/history/delete-transcript ─────────────────────────────────────
static void handleApiHistoryDeleteTranscript() {
    server.sendHeader("Access-Control-Allow-Origin",  "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server.sendHeader("Cache-Control", "no-cache");

    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "POST only");
        return;
    }

    String dir = server.arg("dir");
    if (dir.isEmpty() && server.hasArg("plain")) {
        String body = server.arg("plain");
        int di = body.indexOf("dir=");
        if (di >= 0) {
            int de = body.indexOf('&', di);
            dir = de < 0 ? body.substring(di + 4) : body.substring(di + 4, de);
            dir.replace("+", " ");
        }
    }

    if (dir.isEmpty()
     || !dir.startsWith("meeting_")
     || dir.indexOf('/') >= 0
     || dir.indexOf('\\') >= 0
     || dir.indexOf("..") >= 0) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid dir\"}");
        return;
    }

    String path = "/" + dir + "/full_transcript.md";
    if (SD_MMC.exists(path.c_str())) {
        SD_MMC.remove(path.c_str());
        Serial.printf("[History] Transcript deleted: %s\n", path.c_str());
    }
    server.send(200, "application/json", "{\"ok\":true}");
}

// ─── POST /api/history/regenerate ────────────────────────────────────────────
// Re-run the final-summary GPT pipeline on a past meeting's saved
// full_transcript.md and overwrite summary_final.md with the result.
// Useful when the original final-summary call failed and we fell back to
// the (much shorter) rolling summary.
static void handleApiHistoryRegenerate() {
    server.sendHeader("Access-Control-Allow-Origin",  "*");
    server.sendHeader("Access-Control-Allow-Methods", "POST,OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server.sendHeader("Cache-Control", "no-cache");

    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "POST only");
        return;
    }

    // Same dir-parsing strategy as the delete endpoint
    String dir = server.arg("dir");
    if (dir.isEmpty() && server.hasArg("plain")) {
        String body = server.arg("plain");
        int di = body.indexOf("dir=");
        if (di >= 0) {
            int de = body.indexOf('&', di);
            dir = de < 0 ? body.substring(di + 4) : body.substring(di + 4, de);
            dir.replace("+", " ");
        }
    }

    if (dir.isEmpty()
     || !dir.startsWith("meeting_")
     || dir.indexOf('/') >= 0
     || dir.indexOf('\\') >= 0
     || dir.indexOf("..") >= 0) {
        Serial.printf("[History] Refused regenerate (bad dir): '%s'\n", dir.c_str());
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid dir\"}");
        return;
    }

    if (meetingActive || processingFinal || regenState == REGEN_RUNNING || needSummaryRegen) {
        server.send(409, "application/json",
                    "{\"ok\":false,\"error\":\"device is busy — try again shortly\"}");
        return;
    }

    // Offload to processTask (20 KB stack) and return immediately — GPT can
    // take 30 s to several minutes, and running it here would block every
    // other request AND run TLS on webTask's 6 KB stack.  The dashboard
    // polls the "regen" field of /api/status until done/failed.
    pendingRegenDir  = "/" + dir;
    regenState       = REGEN_RUNNING;
    needSummaryRegen = true;
    Serial.printf("[History] Regenerate scheduled for processTask: %s\n", pendingRegenDir.c_str());
    server.send(200, "application/json", "{\"ok\":true,\"scheduled\":true}");
}

// ─── POST /api/summary/regenerate ────────────────────────────────────────────
// Re-run GPT on the current meeting's full_transcript.md.
// Only valid after the meeting has ended (meetingActive must be false).
static void handleApiSummaryRegenerate() {
    server.sendHeader("Access-Control-Allow-Origin",  "*");
    server.sendHeader("Access-Control-Allow-Methods", "POST,OPTIONS");
    server.sendHeader("Cache-Control", "no-cache");

    if (meetingActive || processingFinal) {
        server.send(409, "application/json",
                    "{\"ok\":false,\"error\":\"meeting still in progress\"}");
        return;
    }

    if (meetingDir.isEmpty() || !meetingDir.startsWith("/meeting_")) {
        server.send(400, "application/json",
                    "{\"ok\":false,\"error\":\"no meeting on record\"}");
        return;
    }

    if (regenState == REGEN_RUNNING || needSummaryRegen) {
        server.send(409, "application/json",
                    "{\"ok\":false,\"error\":\"a regeneration is already running\"}");
        return;
    }

    // Offloaded to processTask — same reasons as the history variant.
    // processTask also refreshes finalSummaryText when dir == meetingDir,
    // so the next /api/status poll delivers the new text.
    pendingRegenDir  = meetingDir;
    regenState       = REGEN_RUNNING;
    needSummaryRegen = true;
    Serial.printf("[Web] Regenerate scheduled for current meeting: %s\n", meetingDir.c_str());
    server.send(200, "application/json", "{\"ok\":true,\"scheduled\":true}");
}

// ─── POST /api/factory-reset ─────────────────────────────────────────────────
// Sets a flag for processTask (which has the 20 KB stack we need for
// recursively walking and deleting every meeting directory) and returns
// immediately.  Doing the actual work in this handler crashed the webTask
// with a stack-canary watchpoint when the device had many stored meetings.
static void handleApiFactoryReset() {
    server.sendHeader("Access-Control-Allow-Origin",  "*");
    server.sendHeader("Access-Control-Allow-Methods", "POST,OPTIONS");
    server.sendHeader("Cache-Control", "no-cache");

    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "POST only");
        return;
    }

    if (meetingActive) {
        server.send(409, "application/json",
                    "{\"ok\":false,\"error\":\"stop the active meeting first\"}");
        return;
    }

    Serial.println("[FactoryReset] Request received — scheduled in processTask");
    needFactoryReset = true;

    // Send the response now; processTask will do the SD work and reboot.
    // The browser's fetch will succeed; the JS then renders its
    // "device reset complete" screen.  If the reboot happens before the
    // browser has fully received the response, the fetch error branch
    // still shows the same screen.
    server.send(200, "application/json",
                "{\"ok\":true,\"msg\":\"factory reset scheduled — device will reboot in a few seconds\"}");
}

// ─── POST /api/reset-credentials ─────────────────────────────────────────────
// Light-weight version of factory reset: clears WiFi credentials, API keys
// and AP settings (i.e. /config.json) but LEAVES every stored meeting on
// the SD card untouched.  Useful when the user wants to change networks
// or move the device to a new owner without losing their recordings.
//
// Runs inline (small amount of work — single file deletion) and reboots
// straight from this handler.  No need to offload to processTask.
static void handleApiResetCredentials() {
    server.sendHeader("Access-Control-Allow-Origin",  "*");
    server.sendHeader("Access-Control-Allow-Methods", "POST,OPTIONS");
    server.sendHeader("Cache-Control", "no-cache");

    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "POST only");
        return;
    }

    if (meetingActive) {
        server.send(409, "application/json",
                    "{\"ok\":false,\"error\":\"stop the active meeting first\"}");
        return;
    }

    Serial.println("\n[ResetCreds] ── BEGIN ─────────────────────────────");

    if (SD_MMC.exists(CONFIG_FILE)) {
        SD_MMC.remove(CONFIG_FILE);
        Serial.println("[ResetCreds] config.json deleted");
    } else {
        Serial.println("[ResetCreds] no config.json found — nothing to wipe");
    }

    Serial.println("[ResetCreds] ── COMPLETE — rebooting in 1 s ─────────");
    server.send(200, "application/json",
                "{\"ok\":true,\"msg\":\"credentials cleared — device will reboot\"}");

    // SD_MMC.end() so the next boot finds the card cleanly (see same pattern
    // in the full factory reset path).
    delay(800);
    SD_MMC.end();
    delay(200);
    ESP.restart();
}

// ─── GET /api/wifi/scan ─────────────────────────────────────────────────────
// Returns a JSON array of nearby WiFi networks so the setup page can show
// a tappable list instead of making the user type the SSID by hand.
//   [{"ssid":"Home","rssi":-45,"sec":3},{"ssid":"Office",...}, ...]
// "sec" is the encryption type as reported by WiFi.encryptionType().
//
// Scan blocks for ~2-4 s — that's why the frontend shows a spinner.
static void handleApiWifiScan() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Cache-Control", "no-cache");

    Serial.println("[WifiScan] starting scan...");
    int n = WiFi.scanNetworks(false /*async*/, true /*show_hidden*/);
    Serial.printf("[WifiScan] found %d networks\n", n);

    String out = "[";
    bool first = true;
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;     // hide truly hidden APs
        if (!first) out += ",";
        out += "{\"ssid\":\"" + jsonEscape(ssid) + "\","
               "\"rssi\":" + String(WiFi.RSSI(i)) + ","
               "\"sec\":" + String((int)WiFi.encryptionType(i)) + "}";
        first = false;
    }
    out += "]";
    WiFi.scanDelete();   // free the result list
    server.send(200, "application/json", out);
}

// ─── GET /api/ai-config ──────────────────────────────────────────────────────
// Returns current AI provider, model, and local URL so the Settings form can
// pre-populate without the user having to re-enter them on every visit.
// API keys are write-only: only a last-4-chars hint is returned, never the
// key itself — anyone on the AP could otherwise read them off this endpoint.
static String keyHint(const String& key) {
    if (key.length() == 0) return "";
    if (key.length() < 8)  return "saved";
    return "…" + key.substring(key.length() - 4);
}

static void handleApiAiConfig() {
    String json = "{\"provider\":\""      + aiProvider
                + "\",\"model\":\""        + jsonEscape(aiModel)
                + "\",\"localUrl\":\""     + jsonEscape(localUrl)
                + "\",\"webhook_url\":\""  + jsonEscape(webhookUrl)
                + "\",\"ssid\":\""         + jsonEscape(wifiSSID)
                + "\",\"ap_ssid\":\""      + jsonEscape(apSSID.length() ? apSSID : String(AP_SSID_DEFAULT))
                + "\",\"el_key_hint\":\""       + jsonEscape(keyHint(elApiKey))
                + "\",\"openai_key_hint\":\""   + jsonEscape(keyHint(openaiApiKey))
                + "\",\"anthropic_key_hint\":\"" + jsonEscape(keyHint(anthropicKey))
                + "\",\"gemini_key_hint\":\""   + jsonEscape(keyHint(geminiKey))
                + "\",\"tz_min\":"         + String(tzOffsetMin)
                + "}";
    server.send(200, "application/json", json);
}

// ─── POST /api/webhook/test ──────────────────────────────────────────────────
// Fires a synthetic payload at the configured webhook so the whole delivery
// chain can be checked without recording a note.  Runs in processTask (TLS
// needs more stack than webTask has); the dashboard polls /api/status for
// the result.
static void handleApiWebhookTest() {
    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "POST only");
        return;
    }
    if (webhookUrl.isEmpty()) {
        server.send(400, "application/json",
                    "{\"ok\":false,\"error\":\"no webhook URL configured\"}");
        return;
    }
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    webhookLastResult = "testing...";
    xSemaphoreGive(stateMutex);
    needWebhookTest = true;
    server.send(200, "application/json", "{\"ok\":true,\"scheduled\":true}");
}

// ─── Captive portal ──────────────────────────────────────────────────────────
// A phone that joins the hotspot immediately fetches a known URL to test for
// internet.  The DNS hijack (see meet.cpp) points those lookups at us; if we
// answer with a redirect instead of the expected response, the OS decides it
// is behind a captive portal and opens the dashboard in its sign-in sheet.
//
// Requests that arrive with our own IP in the Host header are ordinary
// dashboard traffic and must fall through to a real 404, otherwise a missing
// asset would bounce the SPA back to "/".
static bool requestIsForUs() {
    String host = server.hostHeader();
    if (host.length() == 0) return true;
    int colon = host.indexOf(':');            // strip an explicit :port
    if (colon > 0) host = host.substring(0, colon);
    host.toLowerCase();
    return host == WiFi.softAPIP().toString()
        || host == WiFi.localIP().toString()
        || host.startsWith("notemeet");       // .local, .lan, or a bare name
}

// Send the client back to whichever interface it actually reached us on —
// a probe arriving over the station link must not be pointed at 192.168.4.1,
// which is unreachable from the home LAN.
static void redirectToPortal() {
    IPAddress local = server.client().localIP();
    String target = (uint32_t)local != 0 ? local.toString() : WiFi.softAPIP().toString();
    server.sendHeader("Location", "http://" + target + "/", true);
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.send(302, "text/plain", "");
}

// ─── 404 ─────────────────────────────────────────────────────────────────────
static void handle404() {
    if (!requestIsForUs()) { redirectToPortal(); return; }
    server.send(404, "text/plain", "Not found");
}

// ─── startWebServer ───────────────────────────────────────────────────────────
void startWebServer() {
    server.on("/",                   HTTP_GET,  handleRoot);
    server.on("/setup",              HTTP_GET,  handleSetup);
    server.on("/api/start",          HTTP_POST, handleApiStart);
    server.on("/api/stop",           HTTP_POST, handleApiStop);
    server.on("/api/config",         HTTP_POST, handleApiConfig);
    server.on("/api/ai-config",      HTTP_GET,  handleApiAiConfig);
    server.on("/api/history",            HTTP_GET,  handleApiHistory);
    server.on("/api/history/delete",             HTTP_POST, handleApiHistoryDelete);
    server.on("/api/history/delete-transcript",  HTTP_POST, handleApiHistoryDeleteTranscript);
    server.on("/api/history/regenerate", HTTP_POST, handleApiHistoryRegenerate);
    server.on("/api/summary/regenerate", HTTP_POST, handleApiSummaryRegenerate);
    server.on("/api/factory-reset",      HTTP_POST, handleApiFactoryReset);
    server.on("/api/reset-credentials",  HTTP_POST, handleApiResetCredentials);
    server.on("/api/wifi/scan",          HTTP_GET,  handleApiWifiScan);
    server.on("/api/webhook/test",       HTTP_POST, handleApiWebhookTest);

    // Captive-portal probe URLs — each OS checks a different one, and each
    // expects a specific success response.  Redirecting instead is what
    // triggers the "sign in to network" sheet.
    server.on("/generate_204",       HTTP_GET, redirectToPortal);  // Android
    server.on("/gen_204",            HTTP_GET, redirectToPortal);  // Android (older)
    server.on("/hotspot-detect.html",HTTP_GET, redirectToPortal);  // iOS / macOS
    server.on("/library/test/success.html", HTTP_GET, redirectToPortal);
    server.on("/ncsi.txt",           HTTP_GET, redirectToPortal);  // Windows
    server.on("/connecttest.txt",    HTTP_GET, redirectToPortal);  // Windows
    server.on("/canonical.html",     HTTP_GET, redirectToPortal);  // Firefox

    // Extra routes from web_extras
    server.on("/api/status",  HTTP_GET,  handleApiStatus);
    server.on("/api/chat",    HTTP_POST, handleApiChat);
    server.on("/api/settime", HTTP_POST, handleApiSetTime);

    // Notes portal routes (/notes/*, /api/notes, /api/notes/delete)
    registerNoteRoutes();

    server.onNotFound(handle404);
    server.begin();
    Serial.println("[Web] Server started on port 80.");
}