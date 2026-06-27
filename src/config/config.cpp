/*
 * config.cpp
 * ─────────────────────────────────────────────────────────────────
 * Reads/writes /config.json from the SD card.
 * JSON keys: ssid, pass, el_key, openai_key
 * ─────────────────────────────────────────────────────────────────
 */

#include "config.h"
#include "../core/globals.h"
#include "FS.h"
#include "SD_MMC.h"
#include "../json/ShubhJson.h"
#include <string.h>

// ─── loadConfig ───────────────────────────────────────────────────────────────
bool loadConfig() {
    File f = SD_MMC.open(CONFIG_FILE, FILE_READ);
    if (!f) {
        Serial.println("[Config] No config.json found — using defaults.");
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.println("[Config] JSON parse error: " + String(err.c_str()));
        return false;
    }

    wifiSSID     = doc["ssid"]         | "";
    wifiPass     = doc["pass"]         | "";
    elApiKey     = doc["el_key"]       | "";
    openaiApiKey = doc["openai_key"]   | "";
    webhookUrl   = doc["webhook_url"]  | "";
    apSSID       = doc["ap_ssid"]      | AP_SSID_DEFAULT;
    apPass       = doc["ap_pass"]      | AP_PASS_DEFAULT;
    tzOffsetMin  = doc["tz_min"]       | 0;
    aiProvider   = doc["ai_provider"]  | "openai";
    aiModel      = doc["ai_model"]     | "gpt-4o-mini";
    anthropicKey = doc["anthropic_key"]| "";
    geminiKey    = doc["gemini_key"]   | "";
    localUrl     = doc["local_url"]    | "http://192.168.1.147:11434";

    // Load default tags array if present
    configDefaultTagCount = 0;
    JsonArray tagsArr = doc["tags"].as<JsonArray>();
    for (JsonVariant v : tagsArr) {
        if (configDefaultTagCount >= CONFIG_MAX_DEFAULT_TAGS) break;
        String t = v.as<String>(); t.trim();
        if (t.length() == 0 || t.length() > 31) continue;
        strncpy(configDefaultTags[configDefaultTagCount], t.c_str(), 31);
        configDefaultTags[configDefaultTagCount][31] = 0;
        configDefaultTagCount++;
    }

    Serial.printf("[Config] Loaded — SSID: %s  Provider: %s  Model: %s  Tags: %d\n",
        wifiSSID.c_str(), aiProvider.c_str(), aiModel.c_str(), configDefaultTagCount);

    return wifiSSID.length() > 0;
}

// ─── saveConfig ───────────────────────────────────────────────────────────────
void saveConfig() {
    File f = SD_MMC.open(CONFIG_FILE, FILE_WRITE);
    if (!f) {
        Serial.println("[Config] ERROR: cannot write config.json");
        return;
    }

    JsonDocument doc;
    doc["ssid"]          = wifiSSID;
    doc["pass"]          = wifiPass;
    doc["el_key"]        = elApiKey;
    doc["openai_key"]    = openaiApiKey;
    doc["webhook_url"]   = webhookUrl;
    doc["ap_ssid"]       = apSSID;
    doc["ap_pass"]       = apPass;
    doc["tz_min"]        = tzOffsetMin;
    doc["ai_provider"]   = aiProvider;
    doc["ai_model"]      = aiModel;
    doc["anthropic_key"] = anthropicKey;
    doc["gemini_key"]    = geminiKey;
    doc["local_url"]     = localUrl;

    if (configDefaultTagCount > 0) {
        JsonArray tagsArr = doc["tags"].to<JsonArray>();
        for (int i = 0; i < configDefaultTagCount; i++)
            tagsArr.add(String(configDefaultTags[i]));
    }

    serializeJson(doc, f);
    f.close();
    Serial.println("[Config] Saved to SD.");
}