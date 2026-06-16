# pala_meet — QA Fixes & Feature Plan

Status: **plan only — not yet implemented.** Hand to Sonnet 4.6 for implementation.
Each item lists the problem, the root cause with `file:line`, and the concrete fix.
Implement in the order given. Items 1–3 un-brick the device and are small.
Items 4–5 are real features and should each be planned/reviewed before building.

---

## 1. Revert experimental partial refresh (CRITICAL — un-bricks device)

**Problem:** First navigation to menu / note list / tag browser / settings / recording
screen hangs the device.

**Root cause:** The partial-refresh code ported from the pala_note SSD1681 driver is
wrong for this G panel's IC7/UC8151 controller in two ways:
- **BUSY polarity inverted.** This panel signals "ready" with BUSY **HIGH**; every
  working path uses `read_busy_H()` ([epaper_driver_bsp.cpp:151](src/display/epaper_driver_bsp.cpp:151)).
  The partial path uses the legacy `read_busy()` (waits for BUSY **LOW**) at
  [:380](src/display/epaper_driver_bsp.cpp:380), `:392`, `:399`, `:418`, `:430`.
  Panel idles HIGH → infinite busy-wait → hang.
- **Wrong controller commands.** SSD1681 commands (0x24/0x26/0x32/0x22/0x20) are not
  understood by the IC7/UC8151 controller.

**Fix:**
1. In [ui.cpp](src/app/ui.cpp), change every `refreshFast()` back to `refresh()`.
   Current sites (7 total): lines **280, 334, 342, 354, 372, 385, 525**.
2. In [draw.cpp](src/app/draw.cpp) / [draw.h](src/app/draw.h), remove `refreshFast()`
   and `resetPartialMode()` (and the `refresh()` no longer needs to call
   `EPD_ResetPartial()` — simplify back to just `display->EPD_Display();`).
3. In the driver ([epaper_driver_bsp.cpp](src/display/epaper_driver_bsp.cpp) /
   [.h](src/display/epaper_driver_bsp.h)), remove the partial-refresh API and helpers:
   `EPD_DisplayFast`, `EPD_ResetPartial`, `EPD_DisplayPart`, `EPD_DisplayPartBaseImage`,
   `EPD_Init_Partial`, `EPD_SetWindows`, `EPD_SetCursor`, `EPD_SetLut`,
   `EPD_TurnOnDisplay`, `EPD_TurnOnDisplayPart`, the `WF_PARTIAL[]` LUT, and the
   `partialReady` member.

**KEEP:** the dirty-flag in `EPD_Display()`
([epaper_driver_bsp.cpp:259](src/display/epaper_driver_bsp.cpp:259)) and the
`lastBuffer` snapshot. That optimization is safe and correct — it skips the 3–5s
refresh when the buffer is unchanged. Keep `lastBuffer` alloc in the constructor and
the `memcpy` after refresh.

**Note:** Real partial refresh on this panel needs the actual IC7/UC8151 partial
command set — out of scope here, separate research task.

---

## 2. Fix boot/wake → recording (CRITICAL)

**Problem:** After flashing, and after waking from sleep, the device jumps straight to
the recording screen and (combined with #1) hangs there.

**Root cause:** `BTN_REC` is **GPIO0** ([config.h:46](config.h:46)) — the ESP32-S3
strapping/boot button and the deep-sleep wake pin
([config.h:30](config.h:30), [sleep.cpp:31](src/app/sleep.cpp:31)). The IDLE handler
does a raw level-read of GPIO0 ([pala_meet.ino:393](pala_meet.ino:393)) and `record()`
loops `while (digitalRead(BTN_REC)==LOW)` ([record.cpp:34](src/app/record.cpp:34)).
After a flash-reset or a REC-button wake, GPIO0 is still/momentarily LOW → recording
fires. The `delay(500)` debounce currently in the IDLE handler is unreliable.

**Fix — release latch:** In the `STATE_IDLE` block of [pala_meet.ino](pala_meet.ino):
- Remove the `idleDebounced` / `delay(500)` hack.
- Add a static `recArmed` latch: do not accept a REC press until BTN_REC has been
  observed HIGH (released) at least once since entering idle. Pattern:
  ```
  static bool recArmed = false;
  if (digitalRead(BTN_REC) == HIGH) recArmed = true;
  // only run hold-detection / startRecordFlow when recArmed == true
  ```
- Reset `recArmed = false` whenever you leave idle (or re-enter), so the next idle
  entry re-arms cleanly.
- Apply the same "must see released first" guard to PWR if it shows the same wake
  bounce (PWR=GPIO18 is also a wake pin in [sleep.cpp:31](src/app/sleep.cpp:31)).

This deterministically eats the boot/wake press regardless of timing.

---

## 3. Add max-duration cap to `record()` (HIGH — safety)

**Problem:** `while (digitalRead(BTN_REC)==LOW …)` ([record.cpp:34](src/app/record.cpp:34))
has no upper bound. If GPIO0 ever sticks LOW (the strapping-pin risk), it records until
the SD card fills.

**Fix:** Add a max-duration guard to the loop (suggest 5 min; make it a `#define
REC_MAX_MS` in [config.h](config.h)). Exit the loop when `millis() - t0 > REC_MAX_MS`.
Keep the existing 500ms warmup behavior.

---

## 4. Recording UX (toggle) + meeting capture (FEATURE — decisions confirmed)

### 4a. Recording UX on a slow panel
**Problem:** With partial refresh reverted, `showRecording()` blocks 3–5s *before*
`record()` starts capturing ([pala_meet.ino:162-176](pala_meet.ino:162)). Hold-to-record
fights the slow refresh: release during the refresh → falls into the 5s "wait for press"
fallback. The crisp hold-to-talk in the pala_note video relies on working partial
refresh, which this panel does not have.

**Decision (confirmed): TOGGLE recording.** Tap REC to start (screen shows recording),
tap REC again to stop. Decouples recording duration from the slow e-paper refresh, so it
is foolproof on this panel. Applies to BOTH single notes and meetings.

Implementation notes:
- Single notes ([record.cpp](src/app/record.cpp)): replace the
  `while (digitalRead(BTN_REC)==LOW …)` hold loop with a toggle. Enter recording on a
  REC tap from idle/menu; the capture loop runs until the next REC tap (debounced edge,
  not level). The release-latch from item 2 still applies so the start tap isn't
  double-counted.
- The max-duration cap from item 3 still applies as a safety ceiling.
- Drop the "wait up to 5s for press" fallback in `startRecordFlow()`
  ([pala_meet.ino:162-176](pala_meet.ino:162)) — no longer needed with toggle.

### 4b. Meeting audio capture is missing (the real gap)
**Problem:** `meetStart()` sets `meetingActive=true` and creates a directory
([meet.cpp:84](src/meet/meet.cpp:84)); `processTask` waits on
`xQueueReceive(chunkQueue, …)` ([process.cpp:467](src/process/process.cpp:467)) — but
**nothing anywhere calls `xQueueSend`.** There is no I2S chunk-recorder task and no
`/api/chunk` upload route. The only mic read in the project is in
[record.cpp](src/app/record.cpp) for single notes. So meeting mode shows a counting
timer, captures no audio, and "stop" summarizes an empty transcript.

The audio-capture-to-chunk producer from the original `MeetingRecorder_Waveshare` was
never ported.

**Decision (confirmed): ON-DEVICE chunked recording — port the original `recordTask`.**

Why chunk (best practice for this hardware): SD capacity is NOT the limit — 16 kHz mono
16-bit is ~1.9 MB/min, ~112 MB/hr, trivial for a multi-GB card. Chunking is for: (1) RAM
— can't hold minutes of audio in ESP32 RAM, stream to SD regardless; (2) rolling-summary
UX — transcribe each chunk live as the meeting runs; (3) robustness — a failed upload
loses one chunk, not the meeting; (4) upload reliability — small HTTP bodies vs. one
multi-hundred-MB request. Browser-upload is rejected: this is a standalone mic device,
requiring an external recorder defeats the purpose.

Reference implementation (ran on this exact board): the original `recordTask` in
`MeetingRecorder_Waveshare/src/audio/audio.cpp` —
- Core 0, priority 2, 8 KB stack (capture stays off Core 1 where processTask/webTask
  run, avoiding CPU/SD/WiFi contention).
- Reads mic, writes `meetingDir/chunk_N.wav`, splits every `CHUNK_SECONDS` (original = 15s),
  backfills the WAV header on chunk close, then `xQueueSend(chunkQueue, path, 0)`
  (non-blocking; drops if the 8-deep queue is full).

**Port adaptation:** the original reads the ES8311 codec via raw `i2s_read`
(`es8311_init.cpp`). pala_meet's mic goes through `audio_playback_read()` — the SAME call
[record.cpp](src/app/record.cpp) already uses for notes. Reuse the original's
chunk/WAV/queue logic but swap the mic read to `audio_playback_read()`.

**Control:** start/stop the recorder task (or a `meetingActive` capture flag it polls)
from the toggle in `STATE_MEETING_RECORDING` ([pala_meet.ino](pala_meet.ino)) via
`meetStart()` / `meetStop()` ([meet.cpp:84](src/meet/meet.cpp:84)).

**Optional polish:** delete each `chunk_N.wav` after successful transcription (notes
already drop the WAV post-transcribe), keeping only per-chunk transcripts + final summary
so the card stays lean.

Then verify the full pipeline end-to-end: capture → chunk WAV → `chunkQueue` →
transcribe (ElevenLabs) → rolling summary → final summary on stop.

---

## 5. Standardize STT on ElevenLabs Scribe (FEATURE)

**Decision (confirmed):** ElevenLabs Scribe for ALL speech-to-text; OpenAI used ONLY for
GPT summarization/titles. No runtime toggle. Two keys, one engine per job.

**Current state:**
- Notes transcribe via hand-rolled OpenAI Whisper client (`whisper-1`,
  [network.cpp:44-122](src/app/network.cpp:44)) — weaker code, loads/streams via manual
  `WiFiClientSecure`, 2 retries.
- Meetings transcribe via ElevenLabs Scribe (`scribe_v1`,
  [api.cpp:97-165](src/api/api.cpp:97)) — streams the WAV off SD via
  `MultipartUploadStream` (memory-safe), 3 retries + WiFi reconnect, 180s timeout. This
  is the better implementation.

**Fix:**
1. Migrate the notes path to use `transcribeAudio()` from [api.cpp](src/api/api.cpp):
   - In [network.cpp](src/app/network.cpp), have `transcribe()` / `transcribeAll()` call
     `transcribeAudio(wavPath)` and write the returned text to the note's `.md`/index,
     instead of the Whisper client.
   - Delete `transcribeOnce()`, `parseWhisperText()`, and the Whisper-specific code in
     [network.cpp](src/app/network.cpp).
2. Remove the OpenAI **Whisper** key usage for STT. Keep the OpenAI key for GPT only.
   Update [config.h](config.h)/globals and the Setup web page
   ([html_pages.h:537](src/web/html_pages.h:537)) so the key fields read clearly:
   ElevenLabs = STT, OpenAI = GPT/summaries.
3. Confirm both notes and meetings now hit the same `transcribeAudio()` path and inherit
   the streaming upload + retry behavior.

**Result:** notes and meetings transcribe identically; one STT engine; less code.

---

## 6. Update default GPT model to gpt-5.4-mini (SMALL — 2 lines)

**Decision (confirmed):** Switch from `gpt-4o-mini` to `gpt-5.4-mini`. Same 128K output
cap, so no context window logic changes needed anywhere.

**Two places to change:**
1. `src/config/config.cpp:40` — default fallback:
   `aiModel = doc["ai_model"] | "gpt-4o-mini";` → `"gpt-5.4-mini"`
2. `src/api/api_chat.cpp:202` — hardcoded chat model:
   `"{\"model\":\"gpt-4o-mini\",…"` → `"gpt-5.4-mini"`

Also update the stale comments in `api_chat.cpp` (lines 6, 32, 37, 201) and
`process.cpp` that reference `gpt-4o-mini` by name.

The model is also shown as a placeholder/example in `html_pages.h:527` — update that
line too so the Setup page reflects current options.

Note: `aiModel` is runtime-configurable via the Setup web page, so users who already
have a `config.json` will keep their existing setting. This only changes the fallback
default for fresh setups.

---

## Suggested implementation order
1. Item 1 (revert partial refresh) — un-bricks device.
2. Item 2 (release latch) — fixes boot/wake→record.
3. Item 3 + 4a together (toggle recording in `record.cpp`) — single coherent change.
4. Item 6 (gpt-5.4-mini default) — trivial, do with items 1–3.
5. Item 5 (standardize STT on ElevenLabs) — mostly deletion + reroute.
6. Item 4b (meeting audio capture) — real feature build, plan/review before building.

Items 1–3+4a+6 are all small and make the device foolproof. Do them first, flash,
verify, then tackle 5 and 4b.
