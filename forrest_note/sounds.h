#pragma once

#include <Arduino.h>
#include <math.h>
#include "esp_heap_caps.h"

#ifndef SAMPLE_RATE
#define SAMPLE_RATE 16000
#endif

extern "C" {
#include "src/audio/audio_bsp.h"
}

enum SoundTheme : uint8_t {
  SOUND_THEME_OFF = 0,
  SOUND_THEME_WATER,
  SOUND_THEME_CLASSIC,
  SOUND_THEME_WOOD,
  SOUND_THEME_COUNT
};

enum SoundCue : uint8_t {
  CUE_NEXT,
  CUE_SELECT,
  CUE_BACK,
  CUE_RECORDING_START,
  CUE_SAVED,
  CUE_SUCCESS,
  CUE_DELETE
};

// Theme is persisted in NVS. Mute is temporary during recording, playback and
// sleep transitions. Playback volume remains independent from this setting.
extern uint8_t memoryBreadSoundTheme;
extern bool memoryBreadSoundMuted;

inline uint8_t palaSoundTheme() {
  return min((uint8_t)(SOUND_THEME_COUNT - 1), memoryBreadSoundTheme);
}

inline const char* palaSoundThemeName(uint8_t theme) {
  static const char* names[SOUND_THEME_COUNT] = { "关闭", "水滴", "经典", "木质" };
  return names[min((uint8_t)(SOUND_THEME_COUNT - 1), theme)];
}

inline void palaSoundSetTheme(uint8_t theme) {
  memoryBreadSoundTheme = min((uint8_t)(SOUND_THEME_COUNT - 1), theme);
  if (memoryBreadSoundTheme == SOUND_THEME_OFF) audio_playback_set_vol(0);
}

inline void palaSoundSetEnabled(bool enabled) {
  memoryBreadSoundMuted = !enabled;
  if (!enabled) audio_playback_set_vol(0);
}

inline bool palaSoundIsEnabled() {
  return !memoryBreadSoundMuted && palaSoundTheme() != SOUND_THEME_OFF;
}

inline void soundEnable() {
  if (palaSoundIsEnabled()) audio_playback_set_vol(75); // fixed comfortable cue volume
}

inline void soundDisable() {
  delay(22);
  audio_playback_set_vol(0);
}

// Small procedural synthesizer: pitch sweep + optional harmonic/noise content.
// It avoids storing PCM samples in flash while keeping each theme distinctive.
inline void playSynthUI(float freqStart, float freqEnd, int durationMs, float volume,
                        float noiseMix = 0.0f, float harmonicMix = 0.0f) {
  if (!palaSoundIsEnabled()) return;

  const int frames = max(1, (SAMPLE_RATE * durationMs) / 1000);
  const size_t bytes = frames * 2 * sizeof(int16_t);
  int16_t* buffer = (int16_t*)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  if (!buffer) return;

  float phase = 0.0f;
  const int attackFrames = max(1, SAMPLE_RATE / 250); // about 4 ms
  for (int i = 0; i < frames; i++) {
    float progress = (float)i / (float)frames;
    float freq = freqStart + (freqEnd - freqStart) * progress;
    phase += 2.0f * PI * freq / (float)SAMPLE_RATE;

    float attack = min(1.0f, (float)i / (float)attackFrames);
    float decay = 1.0f - progress;
    float envelope = attack * decay * decay;
    float fundamental = sinf(phase);
    float harmonic = sinf(phase * 2.0f) * harmonicMix;
    float noise = ((float)random(-1000, 1000) / 1000.0f) * noiseMix;
    float mixed = fundamental * (1.0f - noiseMix) + harmonic + noise;
    int32_t raw = (int32_t)(mixed * 32767.0f * volume * envelope);
    int16_t sample = (int16_t)constrain(raw, -32767, 32767);
    buffer[i * 2] = sample;
    buffer[i * 2 + 1] = sample;
  }

  audio_playback_write((void*)buffer, bytes);
  heap_caps_free(buffer);
}

inline void cuePause(int ms) { delay(ms); }

inline void playWaterCue(SoundCue cue) {
  switch (cue) {
    case CUE_NEXT:            playSynthUI(1700, 1120, 52, .18f, .015f, .05f); break;
    case CUE_SELECT:          playSynthUI(1950, 1180, 70, .20f, .015f, .06f); break;
    case CUE_BACK:            playSynthUI(1050, 1480, 55, .16f, .010f, .04f); break;
    case CUE_RECORDING_START: playSynthUI(1250, 1900, 62, .18f, .010f, .05f); cuePause(10);
                              playSynthUI(1850, 1200, 48, .16f, .010f, .04f); break;
    case CUE_SAVED:           playSynthUI(1400, 900, 52, .18f, .015f, .05f); cuePause(14);
                              playSynthUI(1900, 1160, 72, .19f, .015f, .05f); break;
    case CUE_SUCCESS:         playSynthUI(1350, 920, 44, .16f); cuePause(10);
                              playSynthUI(1750, 1100, 52, .17f); cuePause(10);
                              playSynthUI(2150, 1360, 66, .18f); break;
    case CUE_DELETE:          playSynthUI(820, 380, 92, .17f, .025f, .04f); break;
  }
}

inline void playClassicCue(SoundCue cue) {
  switch (cue) {
    case CUE_NEXT:            playSynthUI(1120, 1120, 18, .15f, .03f, .10f); break;
    case CUE_SELECT:          playSynthUI(1480, 1480, 28, .17f, .02f, .12f); break;
    case CUE_BACK:            playSynthUI(680, 620, 34, .14f, .03f, .10f); break;
    case CUE_RECORDING_START: playSynthUI(760, 760, 28, .16f, .02f, .10f); cuePause(10);
                              playSynthUI(1180, 1180, 38, .15f, .02f, .10f); break;
    case CUE_SAVED:           playSynthUI(1040, 1040, 46, .16f, .02f, .10f); cuePause(16);
                              playSynthUI(1560, 1560, 64, .15f, .02f, .10f); break;
    case CUE_SUCCESS:         playSynthUI(880, 880, 35, .15f); cuePause(12);
                              playSynthUI(1320, 1320, 48, .14f); cuePause(12);
                              playSynthUI(1760, 1760, 58, .13f); break;
    case CUE_DELETE:          playSynthUI(520, 480, 34, .16f, .04f, .12f); cuePause(10);
                              playSynthUI(270, 230, 62, .14f, .05f, .12f); break;
  }
}

inline void playWoodCue(SoundCue cue) {
  switch (cue) {
    case CUE_NEXT:            playSynthUI(520, 300, 30, .20f, .24f, .32f); break;
    case CUE_SELECT:          playSynthUI(680, 350, 44, .21f, .22f, .35f); break;
    case CUE_BACK:            playSynthUI(390, 230, 42, .18f, .25f, .32f); break;
    case CUE_RECORDING_START: playSynthUI(480, 280, 38, .20f, .20f, .34f); cuePause(12);
                              playSynthUI(760, 420, 48, .20f, .18f, .35f); break;
    case CUE_SAVED:           playSynthUI(560, 310, 44, .20f, .21f, .34f); cuePause(16);
                              playSynthUI(840, 450, 58, .20f, .18f, .35f); break;
    case CUE_SUCCESS:         playSynthUI(520, 300, 36, .18f, .20f, .32f); cuePause(12);
                              playSynthUI(690, 390, 42, .19f, .19f, .34f); cuePause(12);
                              playSynthUI(880, 500, 52, .19f, .17f, .35f); break;
    case CUE_DELETE:          playSynthUI(430, 170, 76, .20f, .28f, .38f); break;
  }
}

inline void playThemeCue(SoundCue cue) {
  if (!palaSoundIsEnabled()) return;
  soundEnable();
  switch (palaSoundTheme()) {
    case SOUND_THEME_WATER:   playWaterCue(cue);   break;
    case SOUND_THEME_CLASSIC: playClassicCue(cue); break;
    case SOUND_THEME_WOOD:    playWoodCue(cue);    break;
    default: break;
  }
  soundDisable();
}

inline void soundNext()           { playThemeCue(CUE_NEXT); }
inline void soundSelect()         { playThemeCue(CUE_SELECT); }
inline void soundBack()           { playThemeCue(CUE_BACK); }
inline void soundRecordingStart() { playThemeCue(CUE_RECORDING_START); }
inline void soundSaved()          { playThemeCue(CUE_SAVED); }
inline void soundSuccess()        { playThemeCue(CUE_SUCCESS); }
inline void soundDelete()         { playThemeCue(CUE_DELETE); }
inline void soundThemePreview()   { playThemeCue(CUE_SAVED); }
