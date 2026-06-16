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
    apSSID       = doc["ap_ssid"]      | AP_SSID_DEFAULT;
    apPass       = doc["ap_pass"]      | AP_PASS_DEFAULT;
    tzOffsetMin  = doc["tz_min"]       | 0;
    aiProvider   = doc["ai_provider"]  | "openai";
    aiModel      = doc["ai_model"]     | "gpt-5.4-mini";
    anthropicKey = doc["anthropic_key"]| "";
    geminiKey    = doc["gemini_key"]   | "";
    localUrl     = doc["local_url"]    | "http://192.168.1.147:11434";

    Serial.printf("[Config] Loaded — SSID: %s  Provider: %s  Model: %s\n",
        wifiSSID.c_str(), aiProvider.c_str(), aiModel.c_str());

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
    doc["ap_ssid"]       = apSSID;
    doc["ap_pass"]       = apPass;
    doc["tz_min"]        = tzOffsetMin;
    doc["ai_provider"]   = aiProvider;
    doc["ai_model"]      = aiModel;
    doc["anthropic_key"] = anthropicKey;
    doc["gemini_key"]    = geminiKey;
    doc["local_url"]     = localUrl;

    serializeJson(doc, f);
    f.close();
    Serial.println("[Config] Saved to SD.");
}