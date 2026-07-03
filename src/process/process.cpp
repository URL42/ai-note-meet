/*
 * process.cpp
 * ─────────────────────────────────────────────────────────────────
 * processTask (Core 1): drains the chunk queue (STT + rolling
 * summary), generates the final summary when a meeting stops, and
 * handles the SD-heavy jobs offloaded from webTask (factory reset,
 * meeting delete, summary regeneration) — this task has the 20 KB
 * stack those jobs need; webTask only has 6 KB.
 * ─────────────────────────────────────────────────────────────────
 */

#include "process.h"
#include "../core/globals.h"
#include "../api/api.h"
#include "../time/ntp_time.h"
#include "FS.h"

void postWebhookMeeting(const String& title, const String& summary, const String& transcript, const String& timestamp);
#include "SD_MMC.h"

// Single-call vs map-reduce threshold.  With OPI PSRAM, large String
// allocations fall back to PSRAM, so big transcripts fit in memory.
// gpt-4o-mini accepts up to 128 K tokens (~500 KB plain text) of input;
// 380 KB leaves room for the prompt + JSON escaping (~1.3× growth).
// Meetings up to ~12 hours get a single GPT call — best quality.
static const size_t SINGLE_CALL_CAP = 380000;
static const size_t SEGMENT_SIZE    = 25000;   // each map-reduce chunk

// ─── deleteDirRecursive ───────────────────────────────────────────────────────
// CRITICAL: never call SD_MMC.remove() / SD_MMC.rmdir() / recurse while a parent
// directory handle is still open.  The FAT layer can leave the card in
// an inconsistent state — the bug surfaced as "SD init FAILED" forever
// on the next boot after a factory reset.
//
// Two-phase, batched approach:
//   Phase 1 — open the dir, scan up to BATCH_SIZE entries into a local
//             array, close the dir handle.  Remember the first subdir
//             (if any) for later recursion.
//   Phase 2 — with the dir handle CLOSED, SD_MMC.remove() every collected
//             file path, then recurse into the saved subdir.
//   Repeat   until the dir reports empty, then SD_MMC.rmdir() it.
void deleteDirRecursive(const String& path) {
    const int BATCH_SIZE = 30;

    while (true) {
        File dir = SD_MMC.open(path.c_str());
        if (!dir) return;

        String batch[BATCH_SIZE];
        int    fileCount   = 0;
        String firstSubdir;     // only one subdir recursed per pass — safer

        File entry = dir.openNextFile();
        while (entry && fileCount < BATCH_SIZE) {
            String n = entry.name();
            int slash = n.lastIndexOf('/');
            if (slash >= 0) n = n.substring(slash + 1);
            String full = path + "/" + n;

            if (entry.isDirectory()) {
                if (firstSubdir.length() == 0) firstSubdir = full;
            } else {
                batch[fileCount++] = full;
            }
            entry.close();
            entry = dir.openNextFile();
        }
        dir.close();    // ← critical: close iterator BEFORE any deletes

        // Delete the files in this batch
        for (int i = 0; i < fileCount; i++) {
            SD_MMC.remove(batch[i].c_str());
        }

        // Recurse into one subdirectory per pass (after parent is closed)
        if (firstSubdir.length() > 0) {
            deleteDirRecursive(firstSubdir);
        }

        // Loop again to pick up either the next batch of files in this
        // dir, or the next subdir — until both are exhausted.
        if (fileCount == 0 && firstSubdir.length() == 0) break;
    }

    SD_MMC.rmdir(path.c_str());
    Serial.printf("[SD] Deleted: %s\n", path.c_str());
}

// ─── createMeetingDir ─────────────────────────────────────────────────────────
void createMeetingDir() {
    stampNow();
    wordCount = 0;

    meetingDir = meetingTimestamp.length() > 0
        ? "/meeting_" + meetingTimestamp
        : "/meeting_" + String(millis());

    bool ok = SD_MMC.mkdir(meetingDir.c_str());
    Serial.printf("[SD] Meeting dir: %s  (%s)\n",
                  meetingDir.c_str(), ok ? "OK" : "FAIL");
}

// ─── readFullTranscriptFromSD ────────────────────────────────────────────────
// The in-RAM `fullTranscript` is trimmed at MAX_TRANSCRIPT_RAM (12 KB) so the
// live UI doesn't blow up memory on long meetings.  The complete transcript
// lives in full_transcript.md on the SD card (appended chunk-by-chunk in
// processChunkFile) — this reads the ENTIRE file, no truncation.
static String readFullTranscriptFromSD(const String& dir) {
    String result;
    String path = dir + "/full_transcript.md";
    File f = SD_MMC.open(path.c_str(), FILE_READ);
    if (!f) {
        Serial.println("[ProcessTask] full_transcript.md missing on SD — falling back to in-RAM transcript");
        return result;
    }

    size_t fsize = f.size();
    result.reserve(fsize + 1);

    char buf[256];
    while (f.available()) {
        int n = f.readBytes(buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        result += buf;
    }
    f.close();

    Serial.printf("[ProcessTask] Loaded %u chars from SD transcript (free heap: %u)\n",
                  (unsigned)result.length(), (unsigned)ESP.getFreeHeap());
    return result;
}

// ─── summarizeTranscriptFile ─────────────────────────────────────────────────
// Shared final-summary pipeline used by both the live meeting-end path and
// summary regeneration.  Reads <dir>/full_transcript.md and picks a strategy:
//   • size ≤ SINGLE_CALL_CAP — one GPT call with the full transcript.
//   • larger — MAP-REDUCE: stream SEGMENT_SIZE segments off SD (one at a
//     time, bounded RAM), summarise each, then synthesise one final summary
//     from the segment summaries.
// Returns the summary; "" ONLY when the transcript is missing/too short.
// GPT failures return a "["-prefixed sentinel (which summaryIsValid
// rejects) so callers can tell "no transcript" from "summarisation failed"
// — the live path must not fall back to the 12 KB RAM tail in the latter
// case.  Does not write anything to SD.
static String summarizeTranscriptFile(const String& dir, const char* logTag) {
    String txPath = dir + "/full_transcript.md";
    size_t fsize = 0;
    {
        File f = SD_MMC.open(txPath.c_str(), FILE_READ);
        if (!f) {
            Serial.printf("[%s] full_transcript.md missing for %s\n", logTag, dir.c_str());
            return "";
        }
        fsize = f.size();
        f.close();
    }
    if (fsize < 50) {
        Serial.printf("[%s] Transcript too short (%u bytes) for %s\n",
                      logTag, (unsigned)fsize, dir.c_str());
        return "";
    }

    Serial.printf("[%s] %s — transcript %u bytes, free heap %u\n",
                  logTag, dir.c_str(), (unsigned)fsize, (unsigned)ESP.getFreeHeap());

    if (fsize <= SINGLE_CALL_CAP) {
        // ── Fast path: one GPT call with the full transcript ────────────
        String transcript = readFullTranscriptFromSD(dir);
        if (transcript.length() < 50) return "";
        Serial.printf("[%s] Single-call summary: %u chars\n",
                      logTag, (unsigned)transcript.length());
        return generateSummary(transcript, true);
    }

    // ── Map-reduce path: stream segments off SD ─────────────────────────
    int totalSegments = (fsize + SEGMENT_SIZE - 1) / SEGMENT_SIZE;
    Serial.printf("[%s] Long meeting (%u KB) — map-reduce in %d segments\n",
                  logTag, (unsigned)(fsize / 1024), totalSegments);

    String combined;
    combined.reserve(totalSegments * 1500 + 256);

    File f = SD_MMC.open(txPath.c_str(), FILE_READ);
    if (f) {
        int segNum = 0;
        while (f.available()) {
            segNum++;
            String segment;
            segment.reserve(SEGMENT_SIZE + 512);
            char buf[256];
            while (f.available() && segment.length() < SEGMENT_SIZE) {
                size_t want = sizeof(buf) - 1;
                if (segment.length() + want > SEGMENT_SIZE) {
                    want = SEGMENT_SIZE - segment.length();
                }
                int n = f.readBytes(buf, want);
                if (n <= 0) break;
                buf[n] = '\0';
                segment += buf;
            }
            // Extend to next newline so we don't cut a chunk transcript in half
            while (f.available()) {
                char c = (char)f.read();
                segment += c;
                if (c == '\n') break;
                if (segment.length() > SEGMENT_SIZE + 2048) break;  // safety
            }
            if (segment.length() < 80) {
                Serial.printf("[%s] Skipping tiny tail segment %d (%u chars)\n",
                              logTag, segNum, (unsigned)segment.length());
                continue;
            }

            Serial.printf("[%s] Map %d/%d: %u chars, free heap %u\n",
                          logTag, segNum, totalSegments,
                          (unsigned)segment.length(), (unsigned)ESP.getFreeHeap());
            String segSum = generateSegmentSummary(segment, segNum, totalSegments);
            segment = "";  // free the segment immediately

            if (segSum.length() > 40 && !segSum.startsWith("[")) {
                combined += "## Segment ";
                combined += String(segNum);
                combined += " of ";
                combined += String(totalSegments);
                combined += "\n";
                combined += segSum;
                combined += "\n\n";
            } else {
                Serial.printf("[%s] Segment %d summary rejected (%u chars)\n",
                              logTag, segNum, (unsigned)segSum.length());
            }
        }
        f.close();
    }

    if (combined.length() <= 100) {
        Serial.printf("[%s] All map steps failed — nothing to synthesise\n", logTag);
        return "[summary failed — all segment summaries failed]";
    }

    Serial.printf("[%s] Reduce step: synthesising final from %u chars, free heap %u\n",
                  logTag, (unsigned)combined.length(), (unsigned)ESP.getFreeHeap());
    return synthesizeFinalSummary(combined);
}

static bool summaryIsValid(const String& s) {
    return s.length() > 80 && !s.startsWith("[") && !s.startsWith("⚠");
}

// ─── regenerateSummaryForMeeting ─────────────────────────────────────────────
// Re-runs the final-summary pipeline for a past (or the current) meeting and
// overwrites <dir>/summary_final.md.  Returns the new summary or "" on
// failure.  Does not touch any live meeting state — the caller decides
// whether finalSummaryText should be updated.
String regenerateSummaryForMeeting(const String& dir) {
    String finalSum = summarizeTranscriptFile(dir, "Regen");

    if (!summaryIsValid(finalSum)) {
        Serial.println("[Regen] GPT result invalid — not saving");
        return "";
    }

    // Truncate the existing summary file before writing — Arduino's
    // FILE_WRITE is append-mode, so without SD_MMC.remove() first the new
    // regenerated summary would be appended after the old content.
    String outPath = dir + "/summary_final.md";
    if (SD_MMC.exists(outPath.c_str())) SD_MMC.remove(outPath.c_str());
    File sf = SD_MMC.open(outPath, FILE_WRITE);
    if (sf) {
        size_t want = finalSum.length();
        size_t got  = sf.print(finalSum);
        sf.flush();
        sf.close();
        Serial.printf("[Regen] Saved %u of %u chars to %s/summary_final.md\n",
                      (unsigned)got, (unsigned)want, dir.c_str());
        if (got != want) Serial.println("[Regen] WARNING: short write — disk may be full");
    } else {
        Serial.println("[Regen] ERROR: could not open summary_final.md for write");
    }

    return finalSum;
}

// ─── saveSummaryToSD — always writes summary_final.md ───────────────────────
// This is the filename the /api/history endpoint reads.
// Also writes a timestamped copy for human browsing on the card.
//
// IMPORTANT: Arduino SD's FILE_WRITE is APPEND mode, not truncate — remove
// any existing file first to guarantee fresh contents.
static void saveSummaryToSD(const String& summary) {
    String finalPath = meetingDir + "/summary_final.md";

    if (SD_MMC.exists(finalPath.c_str())) SD_MMC.remove(finalPath.c_str());

    File f1 = SD_MMC.open(finalPath, FILE_WRITE);
    if (f1) {
        // Prepend a Markdown title so the file opens cleanly in any MD viewer
        String header = "# Meeting Summary\n*" + meetingTimestamp + "*\n\n";
        f1.print(header);
        size_t want = summary.length();
        size_t got  = f1.print(summary);
        f1.flush();
        f1.close();
        Serial.printf("[SD] summary_final.md written: %u of %u bytes\n",
                      (unsigned)got, (unsigned)want);
        if (got != want) Serial.println("[SD] WARNING: short write — disk may be full");
    } else {
        Serial.println("[SD] ERROR: could not open summary_final.md for write");
    }

    // Remove the rolling-summary file now that the final is in place,
    // so /api/history's fallback path can't accidentally surface it.
    String rollingPath = meetingDir + "/summary.md";
    if (SD_MMC.exists(rollingPath.c_str())) {
        SD_MMC.remove(rollingPath.c_str());
    }

    // Optional: timestamped copy for human readability on card
    if (meetingTimestamp.length() > 0) {
        String tsPath = meetingDir + "/summary_" + meetingTimestamp + ".md";
        if (SD_MMC.exists(tsPath.c_str())) SD_MMC.remove(tsPath.c_str());
        File f2 = SD_MMC.open(tsPath, FILE_WRITE);
        if (f2) { f2.print(summary); f2.flush(); f2.close(); }
    }
}

// ─── processChunkFile ────────────────────────────────────────────────────────
// STT one WAV chunk, append its transcript to the in-RAM tail and the
// full_transcript.md file, then delete the WAV.  Rolling-summary generation
// only runs for live chunks (updateRolling=true) — the catch-up pass skips
// it since the final summary follows immediately anyway.
static void processChunkFile(const String& path, bool updateRolling) {
    Serial.println("\n[ProcessTask] ─── Processing: " + path + " ───");

    File chk = SD_MMC.open(path.c_str(), FILE_READ);
    uint32_t fsize = chk ? chk.size() : 0;
    if (chk) chk.close();

    if (fsize <= WAV_HEADER_SIZE) {
        // Delete (don't just skip): the catch-up sweep re-scans the dir and
        // would otherwise find this same header-only file forever.
        Serial.println("[ProcessTask] SKIP: file too small.");
        SD_MMC.remove(path.c_str());
        return;
    }

    Serial.println("[ProcessTask] Calling ElevenLabs STT...");
    String transcript = transcribeAudio(path);
    Serial.println("[ProcessTask] Transcript: " + transcript);

    // Save per-chunk transcript
    String txtPath = path;
    txtPath.replace(".wav", ".md");
    File tf = SD_MMC.open(txtPath.c_str(), FILE_WRITE);
    if (tf) { tf.println(transcript); tf.close(); }

    // WAV chunk served its purpose — delete it now that the transcript is
    // saved. full_transcript.md is the source of truth; keeping the WAVs
    // would fill the card fast (~480 KB per 15s chunk = ~112 MB/hour).
    SD_MMC.remove(path.c_str());

    bool hasContent = transcript.length() > 3
                   && !transcript.startsWith("[STT failed")
                   && !transcript.startsWith("[no speech");
    if (!hasContent) return;

    // ── Critical section: update shared transcript state ──
    // Hold the lock only for the in-RAM string mutations,
    // then snapshot for the slow SD + HTTP work below.
    String transcriptSnap;
    int    curWordCount;
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    fullTranscript += transcript + "\n";

    // Count word-START transitions so the first word is also counted
    // ("hello world" → 2, not 1).
    if (transcript.length() > 0 && transcript[0] != ' ') wordCount++;
    for (size_t i = 1; i < transcript.length(); i++)
        if (transcript[i] != ' ' && transcript[i-1] == ' ') wordCount++;

    if ((int)fullTranscript.length() > MAX_TRANSCRIPT_RAM) {
        int cutAt = (int)fullTranscript.length() - MAX_TRANSCRIPT_RAM;
        int nl = fullTranscript.indexOf('\n', cutAt);
        if (nl > 0) cutAt = nl + 1;
        fullTranscript.remove(0, cutAt);
        Serial.println("[ProcessTask] Transcript trimmed.");
    }
    transcriptSnap = fullTranscript;
    curWordCount   = wordCount;
    xSemaphoreGive(stateMutex);

    // APPEND just this chunk's transcript to the running full file.
    // FILE_APPEND opens in "a" mode so the file grows chunk-by-chunk and
    // stays complete even when the in-RAM `fullTranscript` gets trimmed.
    // This is what the final-summary step reads back.
    // First chunk: write a Markdown header before any content.
    bool isFirstChunk = !SD_MMC.exists((meetingDir + "/full_transcript.md").c_str());
    File ff = SD_MMC.open(meetingDir + "/full_transcript.md", FILE_APPEND);
    if (ff) {
        if (isFirstChunk) {
            ff.print("# Meeting Transcript\n");
            ff.print("*" + meetingTimestamp + "*\n\n");
        }
        ff.print(transcript);
        ff.print('\n');
        ff.close();
    }

    if (!updateRolling) return;

    // Rolling summary (only after 20+ words to avoid trivial summaries)
    if (curWordCount < 20) {
        Serial.printf("[ProcessTask] Skipping rolling summary — only %d words so far.\n", curWordCount);
        return;
    }

    Serial.println("[ProcessTask] Calling GPT for rolling summary...");
    String summary = generateSummary(transcriptSnap, false);

    bool valid = summary.length() > 60
              && !summary.startsWith("[")
              && !summary.startsWith("⚠");
    if (valid) {
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        rollingSummary = summary;
        xSemaphoreGive(stateMutex);
        Serial.println("\n┌────────── ROLLING SUMMARY ──────────────┐");
        Serial.println(summary);
        Serial.println("└─────────────────────────────────────────┘\n");
        // Remove before write — FILE_WRITE is append-mode.
        String rollPath = meetingDir + "/summary.md";
        if (SD_MMC.exists(rollPath.c_str())) SD_MMC.remove(rollPath.c_str());
        File sf = SD_MMC.open(rollPath, FILE_WRITE);
        if (sf) { sf.print(summary); sf.flush(); sf.close(); }
    } else {
        Serial.println("[ProcessTask] Rolling summary rejected: " + summary.substring(0, 80));
    }
}

// ─── catchUpUnprocessedChunks ────────────────────────────────────────────────
// Recover chunks that never made it through the queue: the queue holds 8
// paths (~2 min of backlog), so sustained STT failures or a WiFi outage
// mid-meeting cause recordTask to drop paths — the WAVs stay on the SD card
// but were previously lost from the transcript forever.  Before the final
// summary we sweep the meeting dir for leftover chunk_N.wav files and
// process them in recording order.
static void catchUpUnprocessedChunks() {
    if (meetingDir.length() == 0) return;

    int recovered   = 0;
    int prevLowest  = -1;
    const int SAFETY_LIMIT = 500;
    while (recovered < SAFETY_LIMIT) {
        // Scan for the lowest-numbered remaining chunk WAV.  One directory
        // pass per chunk keeps memory bounded (no path arrays), matching
        // the one-at-a-time pattern used by factory reset.
        int lowest = -1;
        {
            File dir = SD_MMC.open(meetingDir.c_str());
            if (!dir) return;
            File entry = dir.openNextFile();
            while (entry) {
                String n = entry.name();
                int slash = n.lastIndexOf('/');
                if (slash >= 0) n = n.substring(slash + 1);
                bool isChunkWav = !entry.isDirectory()
                               && n.startsWith("chunk_") && n.endsWith(".wav");
                entry.close();
                if (isChunkWav) {
                    int num = n.substring(6, n.length() - 4).toInt();
                    if (lowest < 0 || num < lowest) lowest = num;
                }
                entry = dir.openNextFile();
            }
            dir.close();
        }
        if (lowest < 0) break;

        // Progress guarantee: if the same chunk shows up twice, its WAV
        // wasn't removed (SD error, or a malformed name that parses to the
        // same number) — bail rather than loop forever on paid STT calls.
        if (lowest == prevLowest) {
            Serial.printf("[ProcessTask] Catch-up stuck on chunk %d — aborting sweep\n", lowest);
            break;
        }
        prevLowest = lowest;

        String path = meetingDir + "/chunk_" + String(lowest) + ".wav";
        Serial.printf("[ProcessTask] Catch-up: recovering dropped %s\n", path.c_str());
        processChunkFile(path, false);   // no rolling summary — final follows
        recovered++;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (recovered > 0)
        Serial.printf("[ProcessTask] Catch-up recovered %d dropped chunk(s)\n", recovered);
}

// ─── processTask (Core 1) ─────────────────────────────────────────────────────
void processTask(void* pv) {
    Serial.println("[ProcessTask] Started on core " + String(xPortGetCoreID()));

    for (;;) {
        // ── 0. Factory reset request from /api/factory-reset ────────────────
        if (needFactoryReset) {
            needFactoryReset = false;
            Serial.println("\n[FactoryReset] ── BEGIN ─────────────────────────────");

            // Cancel any in-flight meeting / processing state
            meetingActive   = false;
            finalStop       = false;
            processingFinal = false;
            xQueueReset(chunkQueue);

            // Delete meetings ONE AT A TIME.  Each pass opens the root,
            // finds the first meeting_* directory, closes the iterator,
            // then deletes that directory.  Repeats until no more remain.
            // Keeping only one String alive in the loop avoids the stack
            // pressure that crashed the previous (web-task) implementation.
            int deletedCount = 0;
            const int SAFETY_LIMIT = 500;
            while (deletedCount < SAFETY_LIMIT) {
                String target;
                {
                    File root = SD_MMC.open("/");
                    if (!root || !root.isDirectory()) {
                        if (root) root.close();
                        break;
                    }
                    File entry = root.openNextFile();
                    while (entry) {
                        String n = entry.name();
                        int slash = n.lastIndexOf('/');
                        if (slash >= 0) n = n.substring(slash + 1);
                        bool isMeetingDir = entry.isDirectory() && n.startsWith("meeting_");
                        entry.close();
                        if (isMeetingDir) {
                            target = "/" + n;
                            break;
                        }
                        entry = root.openNextFile();
                    }
                    root.close();
                }
                if (target.length() == 0) break;
                Serial.printf("[FactoryReset] Deleting %s\n", target.c_str());
                deleteDirRecursive(target);
                deletedCount++;
                // Brief yield so the watchdog stays happy on large wipes
                vTaskDelay(5 / portTICK_PERIOD_MS);
            }
            Serial.printf("[FactoryReset] %d meeting directories removed\n", deletedCount);

            // Wipe credentials (config.json holds WiFi creds, API keys, AP creds)
            if (SD_MMC.exists(CONFIG_FILE)) {
                SD_MMC.remove(CONFIG_FILE);
                Serial.println("[FactoryReset] config.json deleted");
            }

            Serial.println("[FactoryReset] ── COMPLETE — rebooting in 1 s ─────");
            // Cleanly release the SD card so the next boot finds it on the
            // first try.  Without this the SPI/SD state carries over after
            // ESP.restart() and the next setup() sees "SD init FAILED".
            SD_MMC.end();
            delay(1000);
            ESP.restart();
        }

        // ── 0b. Single-meeting delete from /api/history/delete ──────────────
        if (needHistoryDelete) {
            needHistoryDelete = false;
            String target = pendingHistoryDeleteDir;
            if (target.length() > 0 && target.startsWith("/meeting_")) {
                Serial.printf("[History] Deleting: %s\n", target.c_str());
                deleteDirRecursive(target);
                Serial.printf("[History] Deleted: %s\n", target.c_str());
            } else {
                Serial.printf("[History] Delete skipped (bad path): '%s'\n", target.c_str());
            }
        }

        // ── 0c. Summary regeneration from the web UI ────────────────────────
        // Runs here (not in the web handler) so the dashboard stays
        // responsive and TLS never runs on webTask's 6 KB stack.  The
        // dashboard polls regenState via /api/status.
        if (needSummaryRegen && !meetingActive && !finalStop) {
            needSummaryRegen = false;
            String dir = pendingRegenDir;
            Serial.printf("[Regen] Starting for %s\n", dir.c_str());
            regenState = REGEN_RUNNING;

            String newSum = regenerateSummaryForMeeting(dir);

            if (newSum.length() > 0) {
                // If this is the most recent meeting, refresh the live
                // Summary tab too.
                if (dir == meetingDir) {
                    xSemaphoreTake(stateMutex, portMAX_DELAY);
                    finalSummaryText = newSum;
                    xSemaphoreGive(stateMutex);
                }
                regenState = REGEN_DONE;
                Serial.println("[Regen] Complete");
            } else {
                regenState = REGEN_FAILED;
                Serial.println("[Regen] FAILED — check API key / network");
            }
        }

        // ── 1. Pick up a ready chunk ────────────────────────────────────────
        // Block for up to 100 ms so the task yields instead of spin-polling.
        // xQueueReceive is thread-safe — no extra mutex needed.
        char pathBuf[CHUNK_PATH_LEN];
        if (xQueueReceive(chunkQueue, pathBuf, pdMS_TO_TICKS(100)) == pdTRUE) {
            processChunkFile(String(pathBuf), true);
        }

        // ── 2. Final summary when meeting stops ─────────────────────────────
        // Wait until all queued chunks are drained AND recordTask has closed
        // and enqueued its in-flight chunk (recordChunkBusy) — otherwise the
        // catch-up scan below would find a WAV that is still open for
        // writing, transcribe a half-written file, and delete it out from
        // under recordTask (open-handle deletes corrupt the FAT layer).
        if (finalStop && !meetingActive && !recordChunkBusy
            && uxQueueMessagesWaiting(chunkQueue) == 0) {
            finalStop       = false;
            processingFinal = true;   // keeps LED blinking through the whole job
            Serial.println("\n[ProcessTask] Meeting stopped — generating FINAL summary...");

            // Recover any chunks that were dropped from the queue (WiFi
            // outage / STT backlog) before summarising, so the transcript
            // is complete.
            catchUpUnprocessedChunks();

            // ── Set up Transcript tab content from the SD file ─────────────
            // The UI only renders the last ~4 KB anyway (see web_extras), so
            // we keep just the tail in RAM.  The complete transcript lives
            // on the SD card at full_transcript.md as the source of truth.
            {
                String tailOnly;
                String txPath = meetingDir + "/full_transcript.md";
                File   f = SD_MMC.open(txPath.c_str(), FILE_READ);
                if (f) {
                    size_t fsize = f.size();
                    size_t start = fsize > 12000 ? fsize - 12000 : 0;
                    f.seek(start);
                    tailOnly.reserve(fsize - start + 1);
                    char buf[256];
                    while (f.available()) {
                        int n = f.readBytes(buf, sizeof(buf) - 1);
                        if (n <= 0) break;
                        buf[n] = '\0';
                        tailOnly += buf;
                    }
                    f.close();
                }
                if (tailOnly.length() < 10) {
                    // SD unreadable — fall back to in-RAM
                    xSemaphoreTake(stateMutex, portMAX_DELAY);
                    tailOnly = fullTranscript;
                    xSemaphoreGive(stateMutex);
                }
                xSemaphoreTake(stateMutex, portMAX_DELAY);
                finalTranscriptText = tailOnly;
                xSemaphoreGive(stateMutex);
            }

            // ── Generate the final summary ─────────────────────────────────
            String finalSum = summarizeTranscriptFile(meetingDir, "ProcessTask");

            bool gotFinalFromGPT = finalSum.length() > 0;
            if (!gotFinalFromGPT) {
                // SD transcript missing/too short — fall back to whatever's
                // in RAM (covers the SD-failure case).
                String ramCopy;
                xSemaphoreTake(stateMutex, portMAX_DELAY);
                ramCopy = fullTranscript;
                xSemaphoreGive(stateMutex);
                if (ramCopy.length() > 10) {
                    Serial.println("[ProcessTask] No SD transcript — using in-RAM fallback");
                    finalSum = generateSummary(ramCopy, true);
                    gotFinalFromGPT = true;
                }
            }

            if (gotFinalFromGPT && finalSum.length() > 10) {
                bool valid = summaryIsValid(finalSum);

                String finalSummarySnap;
                xSemaphoreTake(stateMutex, portMAX_DELAY);
                if (valid) {
                    finalSummaryText = finalSum;
                } else {
                    Serial.println("[ProcessTask] Final summary invalid — using rolling summary.");
                    finalSummaryText = rollingSummary.length() > 60
                        ? rollingSummary
                        : "Meeting recorded but summary failed. Check your API key and model name in Settings, then use the Retry button.";
                }
                finalSummarySnap = finalSummaryText;
                xSemaphoreGive(stateMutex);
                String transcriptSnap = readFullTranscriptFromSD(meetingDir);

                Serial.println("\n╔══════════════════════════════════════════╗");
                Serial.println("║         FINAL MEETING SUMMARY           ║");
                Serial.println("╚══════════════════════════════════════════╝");
                Serial.println(finalSummarySnap);
                Serial.println("════════════════════════════════════════════\n");

                // SD write outside the lock — uses the snapshot, not the live String.
                saveSummaryToSD(finalSummarySnap);
                Serial.println("[ProcessTask] Final summary saved to SD.");
                postWebhookMeeting(meetingDisplayTime, finalSummarySnap, transcriptSnap, meetingTimestamp);

            } else {
                Serial.println("[ProcessTask] Not enough transcript for final summary.");
                xSemaphoreTake(stateMutex, portMAX_DELAY);
                finalSummaryText    = "Not enough speech was recorded for a summary.";
                finalTranscriptText = fullTranscript;
                xSemaphoreGive(stateMutex);
            }

            // Reset working state for next meeting.
            // NOTE: finalTranscriptText and finalSummaryText are intentionally
            // kept alive so the Transcript/Summary tabs remain readable until
            // the NEXT meeting starts (handleApiStart clears them).
            xSemaphoreTake(stateMutex, portMAX_DELAY);
            fullTranscript = "";
            rollingSummary = "";
            chunkIndex     = 0;
            wordCount      = 0;
            xSemaphoreGive(stateMutex);

            // ── Power-down: meeting fully wrapped up ─────────────────────
            // Drop the CPU back to 80 MHz so the device sips power while
            // waiting for the next meeting (saves ~20 mA over 240 MHz).
            //
            // Safety check: a user could start the NEXT meeting while we
            // were still generating this final summary (a long map-reduce
            // can take 2-3 minutes).  In that case the button handler /
            // handleApiStart already bumped the CPU back to 240, so we
            // must NOT drop it back to 80 here.
            processingFinal = false;   // LED can stop blinking now
            if (!meetingActive) {
                setCpuFrequencyMhz(80);
                Serial.println("[Power] Idle: CPU back to 80 MHz");
            } else {
                Serial.println("[Power] Next meeting already started — keeping CPU at 240 MHz");
            }
        }

        // No extra vTaskDelay — xQueueReceive already blocks for 100 ms.
    }
}
