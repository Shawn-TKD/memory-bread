#include "Arduino.h"
#include "../../config.h"
#include "../../globals.h"
#include "../../types.h"
#include "record.h"
#include "SD_MMC.h"
#include "esp_heap_caps.h"
#include "notes.h"
#include "ui.h"
#include "battery.h"
#include "../../sounds.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

extern "C" {
#include "../../src/audio/audio_bsp.h"
}

// Capture (producer) and SD-write (consumer) run on separate cores connected by
// a PSRAM ring buffer. The producer keeps draining the I2S DMA at line rate so a
// slow SD write only grows the ring instead of dropping samples.
struct RecCtx {
  RingbufHandle_t   ring;
  volatile bool     running;    // consumer -> producer: keep capturing
  volatile bool     finished;   // producer -> consumer: capture loop exited
};

static void recProducerTask(void* arg) {
  RecCtx* ctx = (RecCtx*)arg;
  int16_t* sbuf = (int16_t*)heap_caps_malloc(REC_BUF,   MALLOC_CAP_8BIT);
  int16_t* mbuf = (int16_t*)heap_caps_malloc(REC_BUF/2, MALLOC_CAP_8BIT);
  const int monoSamples = REC_BUF / 4;   // stereo int16 in -> mono int16 out

  if (sbuf && mbuf) {
    while (ctx->running) {
      audio_playback_read((void*)sbuf, REC_BUF);   // blocking read from codec DMA
      for (int i = 0; i < monoSamples; i++) mbuf[i] = sbuf[i * 2];  // left channel
      // Block briefly if the ring is full (SD catching up); never silently drop.
      xRingbufferSend(ctx->ring, mbuf, monoSamples * 2, pdMS_TO_TICKS(1000));
    }
  }

  if (sbuf) heap_caps_free(sbuf);
  if (mbuf) heap_caps_free(mbuf);
  ctx->finished = true;
  vTaskDelete(NULL);
}

static void writeWavHeader(File& f, uint32_t dataBytes, bool flushNow) {
  uint32_t fileSize = dataBytes + 36;
  uint32_t byteRate = SAMPLE_RATE * 2;
  uint32_t fmtLen = 16;
  uint32_t sampleRate = SAMPLE_RATE;
  uint16_t blockAlign = 2, audioFormat = 1, channels = 1, bitsPerSample = 16;

  f.seek(0);
  f.write((uint8_t*)"RIFF", 4); f.write((uint8_t*)&fileSize, 4);
  f.write((uint8_t*)"WAVE", 4); f.write((uint8_t*)"fmt ", 4);
  f.write((uint8_t*)&fmtLen, 4); f.write((uint8_t*)&audioFormat, 2);
  f.write((uint8_t*)&channels, 2); f.write((uint8_t*)&sampleRate, 4);
  f.write((uint8_t*)&byteRate, 4); f.write((uint8_t*)&blockAlign, 2);
  f.write((uint8_t*)&bitsPerSample, 2);
  f.write((uint8_t*)"data", 4); f.write((uint8_t*)&dataBytes, 4);
  f.seek(44 + dataBytes);
  if (flushNow) f.flush();
}

bool record(bool longMode) {
  int num = nextNoteNumber();
  char path[64]; snprintf(path, sizeof(path), "%s/note_%03d.wav", NOTES_DIR, num);
  Serial.printf("[Rec] %s mode=%s\n", path, longMode ? "long" : "quick");

  uint64_t totalCapacity = SD_MMC.totalBytes();
  uint64_t usedAtStart = SD_MMC.usedBytes();
  uint64_t freeAtStart = totalCapacity > usedAtStart ? totalCapacity - usedAtStart : 0;
  if (longMode && freeAtStart < LONG_REC_MIN_FREE_BYTES) {
    Serial.printf("[Rec] insufficient free space: %llu bytes\n",
                  (unsigned long long)freeAtStart);
    return false;
  }

  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) return false;

  uint8_t header[44]={}; f.write(header, 44);

  RecCtx ctx;
  ctx.ring = xRingbufferCreateWithCaps(REC_RING_LEN, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
  if (!ctx.ring) { f.close(); return false; }
  ctx.running  = true;
  ctx.finished = false;

  TaskHandle_t producer = NULL;
  if (xTaskCreatePinnedToCore(recProducerTask, "recprod", 4096, &ctx, 6, &producer, 0) != pdPASS) {
    vRingbufferDeleteWithCaps(ctx.ring);
    f.close();
    return false;
  }

  uint32_t totalMono = 0, t0 = millis();
  bool     writeFailed = false;
  int      recPeak = 0;   // peak |sample| since the last UI update

  auto drain = [&](TickType_t wait) -> bool {
    size_t got = 0;
    void* item = xRingbufferReceive(ctx.ring, &got, wait);
    if (!item) return false;
    int16_t* sp = (int16_t*)item;
    int ns = got / 2;
    for (int i = 0; i < ns; i++) { int a = abs(sp[i]); if (a > recPeak) recPeak = a; }
    size_t written = f.write((uint8_t*)item, got);
    vRingbufferReturnItem(ctx.ring, item);
    totalMono += written;
    if (written != got) writeFailed = true;
    return true;
  };

  // Quick mode records while held. Long mode streams one continuous WAV until
  // the next clean REC tap, the safety cap, low battery, or low free space.
  uint32_t lastUi = 0, lastCheckpoint = 0, lastBattery = 0;
  bool stopRequested = false, stopTapDown = false;
  uint32_t stopTapStarted = 0;
  if (longMode) {
    uint32_t remainingMinutes = (uint32_t)(freeAtStart / (SAMPLE_RATE * 2ULL * 60ULL));
    showLongRecordingLive(0, remainingMinutes);
  }

  while (!stopRequested) {
    drain(pdMS_TO_TICKS(40));
    uint32_t now = millis();
    uint32_t elapsed = now - t0;
    if (writeFailed) stopRequested = true;

    if (!longMode) {
      if ((digitalRead(BTN_REC) != LOW && elapsed >= 500) || elapsed >= MAX_REC_MS)
        stopRequested = true;
    } else {
      if (elapsed >= LONG_REC_STOP_ARM_MS) {
        bool down = digitalRead(BTN_REC) == LOW;
        if (down && !stopTapDown) {
          stopTapDown = true;
          stopTapStarted = now;
        }
        if (!down && stopTapDown) {
          stopTapDown = false;
          if (now - stopTapStarted >= BTN_DEBOUNCE_MS) stopRequested = true;
        }
      }

      uint64_t remaining = freeAtStart > totalMono ? freeAtStart - totalMono : 0;
      if (remaining <= LONG_REC_RESERVE_BYTES || elapsed >= LONG_REC_MAX_MS)
        stopRequested = true;

      if (elapsed - lastCheckpoint >= LONG_REC_CHECKPOINT_MS) {
        lastCheckpoint = elapsed;
        writeWavHeader(f, totalMono, true);
      }
      if (elapsed - lastBattery >= LONG_REC_BATTERY_MS) {
        lastBattery = elapsed;
        int battery = readBatteryPercent();
        if (battery >= 0 && battery <= 5) stopRequested = true;
      }
    }

    uint32_t uiCadence = longMode ? LONG_REC_UI_MS : 100UL;
    if (now - lastUi >= uiCadence) {
      lastUi = now;
      if (longMode) {
        uint64_t remaining = freeAtStart > totalMono ? freeAtStart - totalMono : 0;
        uint32_t remainingMinutes = (uint32_t)(remaining / (SAMPLE_RATE * 2ULL * 60ULL));
        showLongRecordingLive(elapsed, remainingMinutes);
      } else {
        int lvl = (int)((long)recPeak * 152L * 3L / 32767L); // x3 boost for speech
        if (lvl > 152) lvl = 152;
        showRecordingLive(elapsed, lvl);
      }
      recPeak = 0;
    }
  }

  // Stop the producer and flush everything still buffered.
  ctx.running = false;
  while (!ctx.finished) drain(pdMS_TO_TICKS(50));
  while (drain(0)) { /* final drain */ }

  vRingbufferDeleteWithCaps(ctx.ring);

  writeWavHeader(f, totalMono, true);
  f.close();

  if (totalMono <= 1000) {
    SD_MMC.remove(path);
    return false;
  }
  lastRecNum = num;
  Serial.printf("[Rec] done: %lu bytes\n", (unsigned long)totalMono);
  return true;
}

bool playWavFile(const char* path) {
  File f = SD_MMC.open(path);
  if (!f) return false;
  if (f.size() <= 44) { f.close(); return false; }

  f.seek(44);

  const int monoBytes = 1024;
  uint8_t* monoBuf   = (uint8_t*)heap_caps_malloc(monoBytes,     MALLOC_CAP_8BIT);
  int16_t* stereoBuf = (int16_t*)heap_caps_malloc(monoBytes * 2, MALLOC_CAP_8BIT);

  if (!monoBuf || !stereoBuf) {
    if (monoBuf)   heap_caps_free(monoBuf);
    if (stereoBuf) heap_caps_free(stereoBuf);
    f.close();
    return false;
  }

  audioPlaying  = true;
  stopPlayback  = false;

  palaSoundSetEnabled(false);
  audio_playback_set_vol(85);

  while (f.available() && !stopPlayback) {
    int readBytes = f.read(monoBuf, monoBytes);
    if (readBytes <= 0) break;
    if (readBytes & 1) readBytes--;

    int samples = readBytes / 2;
    int16_t* mono = (int16_t*)monoBuf;
    for (int i = 0; i < samples; i++) {
      int16_t s = mono[i];
      stereoBuf[i * 2 + 0] = s;
      stereoBuf[i * 2 + 1] = s;
    }
    audio_playback_write((void*)stereoBuf, (uint32_t)(samples * 2 * sizeof(int16_t)));

    if (digitalRead(BTN_REC) == LOW) {
      delay(20);
      if (digitalRead(BTN_REC) == LOW) {
        while (digitalRead(BTN_REC) == LOW) delay(5);
        stopPlayback = true;
      }
    }
  }

  audio_playback_set_vol(0);
  palaSoundSetEnabled(true);

  heap_caps_free(monoBuf);
  heap_caps_free(stereoBuf);
  f.close();

  audioPlaying = false;
  stopPlayback = false;
  return true;
}
