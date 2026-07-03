/*
 * api.cpp
 * ─────────────────────────────────────────────────────────────────
 * All outbound HTTPS calls:
 *   • ElevenLabs Scribe v1 — speech-to-text (notes + meeting chunks)
 *   • Summaries via the configured AI provider — OpenAI, Anthropic,
 *     Gemini, or a local OpenAI-compatible server (Ollama/LM Studio)
 *
 * All use a 3-attempt retry loop with WiFi reconnect between tries.
 * ─────────────────────────────────────────────────────────────────
 */

#include "api.h"
#include "../core/globals.h"
#include "SD_MMC.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include "../json/ShubhJson.h"

// ─── MultipartUploadStream ────────────────────────────────────────────────────
MultipartUploadStream::MultipartUploadStream(File f, const String& h, const String& ft)
    : _file(f), _header(h), _footer(ft), _pos(0) {
    _hLen    = h.length();
    _fileLen = f.size();
    _fLen    = ft.length();
    _total   = _hLen + _fileLen + _fLen;
}

int MultipartUploadStream::read() {
    if (_pos < _hLen)              return (uint8_t)_header[_pos++];
    if (_pos < _hLen + _fileLen)  { _pos++; return _file.read(); }
    if (_pos < _total)             return (uint8_t)_footer[_pos++ - _hLen - _fileLen];
    return -1;
}

// Bulk copy per section — HTTPClient drives uploads through readBytes, and
// per-byte File::read() calls make chunk uploads take several times longer.
size_t MultipartUploadStream::readBytes(char* buf, size_t len) {
    size_t n = 0;
    while (n < len && _pos < _total) {
        if (_pos < _hLen) {
            size_t take = _hLen - _pos;
            if (take > len - n) take = len - n;
            memcpy(buf + n, _header.c_str() + _pos, take);
            _pos += take; n += take;
        } else if (_pos < _hLen + _fileLen) {
            size_t take = _hLen + _fileLen - _pos;
            if (take > len - n) take = len - n;
            int got = _file.read((uint8_t*)(buf + n), take);
            if (got <= 0) break;
            _pos += got; n += got;
        } else {
            size_t off  = _pos - _hLen - _fileLen;
            size_t take = _fLen - off;
            if (take > len - n) take = len - n;
            memcpy(buf + n, _footer.c_str() + off, take);
            _pos += take; n += take;
        }
    }
    return n;
}

int    MultipartUploadStream::available() { return (int)(_total - _pos); }
int    MultipartUploadStream::peek()      { return -1; }
void   MultipartUploadStream::flush()     {}
size_t MultipartUploadStream::write(uint8_t) { return 0; }

// ─── ensureWiFi ───────────────────────────────────────────────────────────────
// Returns true if WiFi is connected, false if reconnect fails.
bool ensureWiFi() {
    if (WiFi.status() == WL_CONNECTED) return true;
    Serial.println("[WiFi] Lost connection — reconnecting...");
    WiFi.disconnect(false);   // false = keep softAP alive
    delay(200);
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500); Serial.print(".");
        if (millis() - start > 20000) {
            Serial.println("\n[WiFi] Reconnect TIMEOUT.");
            return false;
        }
    }
    WiFi.setSleep(true);   // WiFi modem sleep for power saving
    Serial.println("\n[WiFi] Reconnected: " + WiFi.localIP().toString()
                   + "  RSSI: " + String(WiFi.RSSI()) + " dBm");
    return true;
}

// ─── jsonEscape ───────────────────────────────────────────────────────────────
String jsonEscape(const String& s) {
    String out;
    // 1.3× growth + headroom for control-char escapes (\u00XX = 6 chars).
    // Pre-reserving large slabs lets the ESP32 allocator hand them off to
    // PSRAM in one shot, avoiding reallocation churn on huge transcripts.
    out.reserve((s.length() * 130) / 100 + 256);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((uint8_t)c < 0x20) {
                    char hex[8];
                    snprintf(hex, sizeof(hex), "\\u%04x", (uint8_t)c);
                    out += hex;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// ─── _sttOnce (internal, single attempt) ─────────────────────────────────────
static String _sttOnce(const String& path) {
    File file = SD_MMC.open(path.c_str(), FILE_READ);
    if (!file) return "";

    uint32_t fileSize = file.size();
    Serial.printf("[STT] File size: %u bytes\n", fileSize);

    HTTPClient http;
    http.begin(EL_STT_URL);
    http.setTimeout(180000);   // 3 min — large bodies + long thinking
    http.setReuse(false);
    http.addHeader("xi-api-key", elApiKey);

    String boundary = "----ESP32Bound";
    http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

    String head =
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"model_id\"\r\n\r\n"
        "scribe_v1\r\n"
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";

    size_t totalLen = head.length() + fileSize + tail.length();
    Serial.printf("[STT] Uploading %u bytes...\n", (unsigned)totalLen);

    MultipartUploadStream stream(file, head, tail);
    int    code = http.sendRequest("POST", &stream, totalLen);
    String resp = http.getString();
    http.end();
    file.close();

    Serial.printf("[STT] HTTP %d\n", code);
    if (code == 200) {
        JsonDocument doc;
        if (deserializeJson(doc, resp)) return "";
        String text = doc["text"].as<String>();
        text.replace("<|nb|>", "");
        text.trim();
        Serial.printf("[STT] %d chars: %s\n", text.length(), text.c_str());
        return text.length() > 0 ? text : "[no speech detected]";
    }
    Serial.println("[STT] Error: " + resp.substring(0, 200));
    return "";
}

// ─── transcribeAudio (public, with retry) ────────────────────────────────────
String transcribeAudio(const String& path) {
    if (elApiKey.length() < 10) {
        Serial.println("[STT] No ElevenLabs API key — visit Setup page.");
        return "[No ElevenLabs API key configured]";
    }
    for (int attempt = 1; attempt <= 3; attempt++) {
        Serial.printf("[STT] Attempt %d/3\n", attempt);
        if (!ensureWiFi()) { delay(3000); continue; }
        String result = _sttOnce(path);
        if (result.length() > 0) return result;
        if (attempt < 3) {
            uint32_t wait = attempt * 3000;
            Serial.printf("[STT] Retrying in %us...\n", wait / 1000);
            WiFi.disconnect(false); delay(500);   // false = keep softAP alive
            WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
            delay(wait);
        }
    }
    return "[STT failed after retries]";
}

// ─── _buildOpenAIBody ─────────────────────────────────────────────────────────
// Builds OpenAI-compatible JSON body. Used for OpenAI and Local (Ollama).
static String _buildOpenAIBody(const String& sys, const String& user,
                               const String& model, int maxTokens) {
    String body;
    body.reserve((sys.length() + user.length()) * 140 / 100 + 4096);
    body += "{\"model\":\"";
    body += jsonEscape(model);
    body += "\",\"messages\":[{\"role\":\"system\",\"content\":\"";
    body += jsonEscape(sys);
    body += "\"},{\"role\":\"user\",\"content\":\"";
    body += jsonEscape(user);
    body += "\"}],\"temperature\":0.2,\"max_completion_tokens\":";
    body += String(maxTokens);
    body += "}";
    return body;
}

// ─── _buildAnthropicBody ──────────────────────────────────────────────────────
static String _buildAnthropicBody(const String& sys, const String& user,
                                  const String& model, int maxTokens) {
    String body;
    body.reserve((sys.length() + user.length()) * 140 / 100 + 4096);
    body += "{\"model\":\"";
    body += jsonEscape(model);
    body += "\",\"max_tokens\":";
    body += String(maxTokens);
    body += ",\"system\":\"";
    body += jsonEscape(sys);
    body += "\",\"messages\":[{\"role\":\"user\",\"content\":\"";
    body += jsonEscape(user);
    body += "\"}]}";
    return body;
}

// ─── _buildGeminiBody ─────────────────────────────────────────────────────────
static String _buildGeminiBody(const String& sys, const String& user,
                               int maxTokens) {
    String body;
    body.reserve((sys.length() + user.length()) * 140 / 100 + 4096);
    body += "{\"system_instruction\":{\"parts\":[{\"text\":\"";
    body += jsonEscape(sys);
    body += "\"}]},\"contents\":[{\"parts\":[{\"text\":\"";
    body += jsonEscape(user);
    body += "\"}]}],\"generationConfig\":{\"maxOutputTokens\":";
    body += String(maxTokens);
    body += ",\"temperature\":0.2}}";
    return body;
}

// ─── _parseOpenAIResponse ─────────────────────────────────────────────────────
static String _parseOpenAIResponse(const String& resp) {
    JsonDocument doc;
    if (deserializeJson(doc, resp)) return "";
    return doc["choices"][0]["message"]["content"].as<String>();
}

// ─── _parseAnthropicResponse ──────────────────────────────────────────────────
static String _parseAnthropicResponse(const String& resp) {
    JsonDocument doc;
    if (deserializeJson(doc, resp)) return "";
    return doc["content"][0]["text"].as<String>();
}

// ─── _parseGeminiResponse ─────────────────────────────────────────────────────
static String _parseGeminiResponse(const String& resp) {
    JsonDocument doc;
    if (deserializeJson(doc, resp)) return "";
    return doc["candidates"][0]["content"]["parts"][0]["text"].as<String>();
}

// ─── _aiCallOnce (internal, single attempt) ───────────────────────────────────
// Dispatches to the correct provider based on aiProvider global.
// Handles URL, headers, body format, and response parsing per provider.
static String _aiCallOnce(const String& sys, const String& user,
                          int maxTokens, int timeoutMs) {
    String body, url;
    HTTPClient http;

    if (aiProvider == "anthropic") {
        body = _buildAnthropicBody(sys, user, aiModel, maxTokens);
        http.begin(ANTHROPIC_URL);
        http.setTimeout(timeoutMs);
        http.setReuse(false);
        http.addHeader("Content-Type",    "application/json");
        http.addHeader("x-api-key",       anthropicKey);
        http.addHeader("anthropic-version", "2023-06-01");

        int    code = http.POST(body);
        String resp = http.getString();
        http.end();
        Serial.printf("[AI/Anthropic] HTTP %d\n", code);
        if (code == 200) {
            String result = _parseAnthropicResponse(resp);
            Serial.printf("[AI/Anthropic] %d chars\n", result.length());
            return result;
        }
        Serial.println("[AI/Anthropic] Error: " + resp.substring(0, 200));
        return "";

    } else if (aiProvider == "gemini") {
        body = _buildGeminiBody(sys, user, maxTokens);
        url  = String(GEMINI_URL_BASE) + aiModel + ":generateContent?key=" + geminiKey;
        http.begin(url);
        http.setTimeout(timeoutMs);
        http.setReuse(false);
        http.addHeader("Content-Type", "application/json");

        int    code = http.POST(body);
        String resp = http.getString();
        http.end();
        Serial.printf("[AI/Gemini] HTTP %d\n", code);
        if (code == 200) {
            String result = _parseGeminiResponse(resp);
            Serial.printf("[AI/Gemini] %d chars\n", result.length());
            return result;
        }
        Serial.println("[AI/Gemini] Error: " + resp.substring(0, 200));
        return "";

    } else if (aiProvider == "local") {
        // Ollama / LM Studio — OpenAI-compatible endpoint, plain HTTP, no auth
        body = _buildOpenAIBody(sys, user, aiModel, maxTokens);
        url  = localUrl + "/v1/chat/completions";
        http.begin(url);
        http.setTimeout(timeoutMs);
        http.setReuse(false);
        http.addHeader("Content-Type", "application/json");

        int    code = http.POST(body);
        String resp = http.getString();
        http.end();
        Serial.printf("[AI/Local] HTTP %d\n", code);
        if (code == 200) {
            String result = _parseOpenAIResponse(resp);
            Serial.printf("[AI/Local] %d chars\n", result.length());
            return result;
        }
        Serial.println("[AI/Local] Error: " + resp.substring(0, 200));
        return "";

    } else {
        // Default: OpenAI
        body = _buildOpenAIBody(sys, user, aiModel, maxTokens);
        http.begin(OPENAI_URL);
        http.setTimeout(timeoutMs);
        http.setReuse(false);
        http.addHeader("Content-Type",  "application/json");
        http.addHeader("Authorization", "Bearer " + openaiApiKey);

        int    code = http.POST(body);
        String resp = http.getString();
        http.end();
        Serial.printf("[AI/OpenAI] HTTP %d\n", code);
        if (code == 200) {
            String result = _parseOpenAIResponse(resp);
            Serial.printf("[AI/OpenAI] %d chars\n", result.length());
            return result;
        }
        Serial.println("[AI/OpenAI] Error: " + resp.substring(0, 200));
        return "";
    }
}

// ─── _aiCallRetry (internal, 3-attempt + WiFi-bounce) ────────────────────────
static String _aiCallRetry(const String& sys, const String& user,
                           int maxTokens, int timeoutMs) {
    // Check the active provider has a key configured
    if (aiProvider == "openai" && openaiApiKey.length() < 10)
        return "[No OpenAI API key — visit Settings]";
    if (aiProvider == "anthropic" && anthropicKey.length() < 10)
        return "[No Anthropic API key — visit Settings]";
    if (aiProvider == "gemini" && geminiKey.length() < 10)
        return "[No Gemini API key — visit Settings]";
    // local needs no key

    // Skip WiFi check for local provider (LAN, always reachable)
    bool needsWifi = (aiProvider != "local");

    for (int attempt = 1; attempt <= 3; attempt++) {
        Serial.printf("[AI/%s] Attempt %d/3 — model: %s\n",
                      aiProvider.c_str(), attempt, aiModel.c_str());
        if (needsWifi && !ensureWiFi()) { delay(3000); continue; }
        String result = _aiCallOnce(sys, user, maxTokens, timeoutMs);
        if (result.length() > 0) return result;
        if (attempt < 3) {
            uint32_t wait = attempt * 3000;
            Serial.printf("[AI] Retrying in %us...\n", wait / 1000);
            if (needsWifi) {
                WiFi.disconnect(false); delay(500);
                WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
            }
            delay(wait);
        }
    }
    return "[AI call failed after 3 attempts]";
}

// ─── generateSummary (public, with retry) ────────────────────────────────────
// Used for:
//   - rolling summaries during a meeting (isFinal=false, lightweight)
//   - final summary on stop for SHORT meetings that fit in one GPT call
//     (the chunked/synthesised path takes over for long ones — see
//     generateSegmentSummary + synthesizeFinalSummary)
String generateSummary(const String& transcript, bool isFinal) {
    if (transcript.length() < 10) return "[Not enough transcript yet]";

    String sys = isFinal
        ? "You are a meeting summariser.  Write a detailed summary "
          "covering every meaningful topic from the transcript.  Use "
          "2-6 content-driven '##' headings named after the actual "
          "topics (not generic 'Overview' / 'Introduction' / 'Conclusion').  "
          "After the topic sections, add '## Decisions Made', "
          "'## Action Items', and/or '## People & Key Details' only if "
          "they have real content.\n\n"
          "The transcript is from speech-to-text — use context and your "
          "world knowledge to silently correct misheard proper nouns "
          "(places, brands, products, events).  Don't preserve obvious "
          "ASR errors even if the transcript repeats them."
        : "Write a brief plain-prose rolling summary of the transcript "
          "below.  No headings, no templates.";

    String user = isFinal
        ? "Below is the FULL transcript of a meeting.\n\n"
          "Write a thorough summary that covers every meaningful topic.  "
          "Use 2-6 '##' headings named after the ACTUAL topics from this "
          "meeting (NOT generic 'Introduction', 'Overview', 'Conclusion', "
          "'Closing Remarks').  Under each heading, write a few "
          "sentences or bullets describing what was said.\n\n"
          "AFTER the topic sections, include '## Decisions Made', "
          "'## Action Items', and/or '## People & Key Details' — but "
          "ONLY if they have real content.  Skip empty sections.\n\n"
          "Use your world knowledge to silently correct mis-heard proper "
          "nouns from the speech-to-text (places, brands, products, "
          "events that don't exist or don't fit the meeting's context).  "
          "Use '-' for bullets.\n\n"
          "---\n\nTRANSCRIPT:\n" + transcript
        : "Write a brief rolling summary (max 120 words) of what has been "
          "discussed in the meeting so far, based ONLY on the transcript "
          "below.  Use plain paragraphs — no '##' headings, no category "
          "labels.  Just say naturally what's been talked about, the way "
          "you'd brief someone joining the meeting late.\n\n"
          "TRANSCRIPT:\n" + transcript;

    // User wants UNLIMITED — set to gpt-4o-mini's hard ceiling (16 K
    // tokens ≈ 12 K words).  The model won't exceed this no matter what
    // we ask for, but giving it the full ceiling lets it write as much
    // as the meeting warrants.
    int maxTok  = isFinal ? 16000 : 512;
    int timeout = isFinal ? 90000 : 30000;
    return _aiCallRetry(sys, user, maxTok, timeout);
}

// ─── generateSegmentSummary — one segment of a long meeting ──────────────────
// Map step of the map-reduce.  Asked to be THOROUGH (not concise), because
// detail at this stage is what makes the final synthesis comprehensive.
String generateSegmentSummary(const String& transcriptSegment,
                              int segNum, int totalSeg) {
    if (transcriptSegment.length() < 10) return "";

    String sys =
        "You are extracting facts from ONE segment of a longer meeting "
        "transcript. The goal is detail — every topic, name, vendor, number, "
        "decision and action that actually appears in this segment.\n\n"
        "ABSOLUTE RULES:\n"
        "1. NEVER use square brackets [ ] for placeholders. No [Insert Date], "
        "no [Team Member Name], no [List items]. If something wasn't said, "
        "write 'someone' / 'a team member' / 'TBD' — never square brackets.\n"
        "2. NEVER use generic template sections like 'Opening Remarks' that "
        "are not in the transcript.\n"
        "3. Use ## for headings — never '1.', '2.', '3.' for section titles.\n"
        "4. Use - (dash) for bullet items.\n"
        "5. If a section has nothing in this segment, write 'None in this "
        "segment.' rather than inventing content.";

    String user;
    user.reserve(transcriptSegment.length() + 1024);
    user += "This is segment ";
    user += String(segNum);
    user += " of ";
    user += String(totalSeg);
    user += " from a meeting transcript. Extract everything that was "
            "actually said in this segment using EXACTLY this format:\n\n"
            "## Topics Discussed\n"
            "- Every topic with sub-bullets for specifics\n\n"
            "## Decisions\n"
            "- Every decision reached, or 'None in this segment.'\n\n"
            "## Action Items\n"
            "- Who/what/when, or 'None in this segment.'\n\n"
            "## Names, Dates & Numbers\n"
            "- Real names, vendors, products, dates, metrics actually said\n\n"
            "Do NOT use placeholders. Do NOT use 1./2. as headings.\n\n"
            "---\n\nSEGMENT TRANSCRIPT:\n";
    user += transcriptSegment;

    // 1500 tokens (~1100 words) per segment is plenty for a detailed extract.
    // Bumped from 1500 to 4000 — each segment extraction can capture
    // every detail without truncation.
    return _aiCallRetry(sys, user, 4000, 90000);
}

// ─── synthesizeFinalSummary — reduce step ────────────────────────────────────
// Takes the concatenated segment summaries and produces the single, polished
// final meeting summary that the user sees in the UI.
String synthesizeFinalSummary(const String& combinedSegmentSummaries) {
    if (combinedSegmentSummaries.length() < 30) return "";

    String sys =
        "You are merging multiple segment summaries of ONE meeting into a "
        "single polished final summary. Use ONLY the information present in "
        "the segment summaries below.\n\n"
        "ABSOLUTE RULES:\n"
        "1. NEVER use square brackets [ ] in your output. No [Insert Name], "
        "no [Team Member Name], no [Counselor's Name]. If a name wasn't "
        "specified in the segments, write 'someone' / 'a team member' / "
        "'TBD' — never square brackets.\n"
        "2. NEVER use generic meeting-minutes templates with sections like "
        "'Opening Remarks', 'Review of Previous Minutes', 'Open Forum', "
        "'Closing Remarks' unless those were actually mentioned.\n"
        "3. Use ## for ALL section headings — never '1.', '2.', '3.'.\n"
        "4. Use - (dash) for bullets.\n"
        "5. Merge duplicate items across segments, but do NOT drop any "
        "topic that appears in any segment.";

    String user;
    user.reserve(combinedSegmentSummaries.length() + 1024);
    user += "Below are detailed extractions from consecutive segments of a "
            "single meeting in chronological order.  Merge them into ONE "
            "clear final summary that covers the entire flow of the meeting "
            "from start to finish.\n\n"
            "STRUCTURE:\n"
            "1) Main body — 2-6 headings (use '## Heading Name') where "
            "each heading is the NAME OF AN ACTUAL TOPIC from THIS meeting "
            "(e.g. '## Friday attendance trend', '## John Smith's "
            "situation').  NEVER use generic boilerplate headings like "
            "'Overview', 'Core Objective', 'Contest Details', 'Factual "
            "Audit', 'Opening Remarks', 'Closing Remarks', 'Conclusion', "
            "'Wrap-Up', 'Other Discussion', 'Miscellaneous'.\n\n"
            "2) Tail sections — AFTER the topic headings, include any of "
            "these THREE that actually have content (skip a section if it "
            "would be empty — do NOT write 'None mentioned' filler):\n\n"
            "    ## Decisions Made\n"
            "    - Concrete decisions actually reached, one per bullet.\n\n"
            "    ## Action Items\n"
            "    - Who is doing what, by when (if mentioned).\n\n"
            "    ## People & Key Details\n"
            "    - Real people with their role (host / presenter / "
            "audience / participant / student / etc.)\n"
            "    - Specific numbers, dates, deadlines, products, places.\n\n"
            "If a tail section has nothing in this meeting, OMIT the "
            "heading entirely.  No filler.\n\n"
            "Cover every meaningful topic from every segment — opening, "
            "middle and end.  Deduplicate items that appear in more than "
            "one segment.  Weight coverage by time spent discussing.\n\n"
            "Preserve any unusual names exactly as they appear when "
            "the transcription is clearly correct.  Keep distinct roles "
            "distinct.  Use - (dash) for bullets.  No 1./2./3.  No "
            "[Insert ...] placeholders.\n\n"
            "---\n\nSEGMENT SUMMARIES:\n";
    user += combinedSegmentSummaries;

    // 3000 tokens (~2200 words) for the final polished output.
    // 16 K tokens — model maximum.  Long meetings get fully detailed
    // synthesis with no truncation.
    return _aiCallRetry(sys, user, 16000, 180000);
}

