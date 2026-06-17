/*
 * record_task.cpp
 * ─────────────────────────────────────────────────────────────────
 * recordTask — Core 0, priority 2
 *
 * Watches meetingActive.  When true, captures audio from the ES8311
 * codec via audio_playback_read(), downsamples stereo→mono, writes
 * 15-second WAV chunks to the meeting directory on the SD card, and
 * sends each completed chunk path to chunkQueue for processTask to
 * transcribe and summarise.
 *
 * Audio format (matches pala_note voice notes):
 *   Input:  stereo interleaved 16-bit PCM at MEET_SAMPLE_RATE via audio_playback_read()
 *   Output: mono 16-bit PCM WAV at MEET_SAMPLE_RATE (left channel only)
 * ─────────────────────────────────────────────────────────────────
 */

#include "meet.h"
#include "../core/globals.h"
#include "FS.h"
#include "SD_MMC.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

extern "C" {
#include "../audio/audio_bsp.h"
}

// Read buffer size — matches pala_note's REC_BUF so both paths use the
// same underlying I2S call cadence (128ms per call at 16 kHz stereo).
#define MEET_STEREO_BUF  (8 * 1024)   // bytes; stereo 16-bit from audio_playback_read
#define MEET_MONO_BUF    (MEET_STEREO_BUF / 2)  // bytes; after stereo→mono downsample

// Write a 44-byte WAV header. Called twice per chunk: once with dataBytes=0
// as a placeholder, once after recording to patch in the real size.
static void writeWavHeader(File& f, uint32_t dataBytes) {
    const uint32_t sr  = MEET_SAMPLE_RATE;
    const uint16_t ch  = 1;
    const uint16_t bps = 16;
    const uint32_t br  = sr * ch * (bps / 8);
    const uint16_t ba  = ch * (bps / 8);
    const uint32_t fS  = dataBytes + 36;
    const uint32_t fL  = 16;
    const uint16_t aF  = 1;

    f.seek(0);
    f.write((const uint8_t*)"RIFF", 4); f.write((const uint8_t*)&fS,  4);
    f.write((const uint8_t*)"WAVE", 4); f.write((const uint8_t*)"fmt ", 4);
    f.write((const uint8_t*)&fL,  4);   f.write((const uint8_t*)&aF,  2);
    f.write((const uint8_t*)&ch,  2);   f.write((const uint8_t*)&sr,  4);
    f.write((const uint8_t*)&br,  4);   f.write((const uint8_t*)&ba,  2);
    f.write((const uint8_t*)&bps, 2);
    f.write((const uint8_t*)"data", 4); f.write((const uint8_t*)&dataBytes, 4);
}

void recordTask(void* pv) {
    Serial.println("[RecordTask] Started on core " + String(xPortGetCoreID()));

    int16_t* sbuf = (int16_t*)malloc(MEET_STEREO_BUF);
    int16_t* mbuf = (int16_t*)malloc(MEET_MONO_BUF);

    if (!sbuf || !mbuf) {
        Serial.println("[RecordTask] FATAL: malloc failed!");
        if (sbuf) free(sbuf);
        if (mbuf) free(mbuf);
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        // Idle until a meeting starts
        if (!meetingActive) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        String wavPath = meetingDir + "/chunk_" + String(chunkIndex) + ".wav";
        Serial.printf("[RecordTask] Opening chunk: %s\n", wavPath.c_str());

        File file = SD_MMC.open(wavPath.c_str(), FILE_WRITE);
        if (!file) {
            Serial.println("[RecordTask] SD open failed — retrying in 200ms");
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // Write placeholder header; patched with real size after recording.
        writeWavHeader(file, 0);
        file.flush();

        uint32_t totalMono = 0;
        uint32_t startMs   = millis();
        uint32_t lastHbMs  = millis();

        Serial.printf("[RecordTask] ● Recording chunk %d...\n", chunkIndex);

        while (meetingActive) {
            // Blocking read — returns when the I2S DMA has MEET_STEREO_BUF bytes ready.
            audio_playback_read((void*)sbuf, MEET_STEREO_BUF);

            // Stereo interleaved 16-bit → mono: take left channel (even indices)
            int monoSamples = MEET_STEREO_BUF / 4;
            for (int i = 0; i < monoSamples; i++) mbuf[i] = sbuf[i * 2];

            size_t written = file.write((const uint8_t*)mbuf, monoSamples * 2);
            if (written == 0) {
                Serial.println("[RecordTask] SD write returned 0 — SD full or error");
                break;
            }
            totalMono += written;

            if (millis() - lastHbMs >= 5000) {
                lastHbMs = millis();
                Serial.printf("[RecordTask] ♪ chunk %d: %lus / %ds  (%lu KB)\n",
                    chunkIndex,
                    (unsigned long)((millis() - startMs) / 1000),
                    CHUNK_SECONDS,
                    (unsigned long)(totalMono / 1024));
            }

            if ((millis() - startMs) >= (uint32_t)(CHUNK_SECONDS * 1000)) {
                Serial.printf("[RecordTask] Chunk %d split at %ds\n",
                              chunkIndex, CHUNK_SECONDS);
                break;
            }
        }

        // Patch WAV header with real data length
        writeWavHeader(file, totalMono);
        file.flush();
        file.close();

        Serial.printf("[RecordTask] Chunk %d done: %lu bytes mono\n",
                      chunkIndex, (unsigned long)totalMono);

        if (totalMono < 1000) {
            Serial.println("[RecordTask] WARNING: tiny chunk — mic issue or very short?");
        }

        // Send path to processTask.  Non-blocking: if the queue is full
        // (> CHUNK_QUEUE_SIZE chunks backed up) we drop this one rather than
        // block the next recording cycle.
        char pathBuf[CHUNK_PATH_LEN];
        strncpy(pathBuf, wavPath.c_str(), CHUNK_PATH_LEN - 1);
        pathBuf[CHUNK_PATH_LEN - 1] = '\0';
        if (xQueueSend(chunkQueue, pathBuf, 0) != pdTRUE) {
            Serial.println("[RecordTask] WARNING: chunk queue full — chunk dropped!");
        } else {
            Serial.printf("[RecordTask] Enqueued %s\n", pathBuf);
        }

        chunkIndex++;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    free(sbuf);
    free(mbuf);
}
