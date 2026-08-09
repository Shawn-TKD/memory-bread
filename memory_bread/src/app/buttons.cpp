#include "Arduino.h"
#include "../../config.h"
#include "../../globals.h"
#include "../../types.h"
#include "buttons.h"

extern void startRecordFlow();
extern void startLongRecordFlow();
extern void resetActivity();

bool isDown(int pin) { return digitalRead(pin) == LOW; }

// Non-blocking per-button state machine, sampled once per loop (no busy-wait).
// Emits:
//   EV_SINGLE  on release, if held < BTN_LONG_MS  (acts the instant you let go)
//   EV_LONG    the moment the hold crosses BTN_LONG_MS (fires once, no wait-for-release)
// Double-press was removed: it forced every tap to wait ~200 ms for a possible second
// press, which is what made the UI feel laggy.
namespace {
  enum Phase { PH_IDLE, PH_DEBOUNCE, PH_DOWN, PH_LONGFIRED };
  struct BtnState { Phase phase; uint32_t tDown; };
  BtnState st[2] = {{PH_IDLE, 0}, {PH_IDLE, 0}};
  inline int idx(int pin) { return pin == BTN_REC ? 0 : 1; }
}

ButtonEvent readButtonEvent(int pin) {
  BtnState& b = st[idx(pin)];
  bool down = isDown(pin);
  uint32_t now = millis();

  switch (b.phase) {
    case PH_IDLE:
      if (down) { b.phase = PH_DEBOUNCE; b.tDown = now; }
      return EV_NONE;

    case PH_DEBOUNCE:
      if (!down) { b.phase = PH_IDLE; return EV_NONE; }      // bounce / too brief
      if (now - b.tDown >= BTN_DEBOUNCE_MS) b.phase = PH_DOWN;
      return EV_NONE;

    case PH_DOWN:
      if (now - b.tDown >= BTN_LONG_MS) {                    // crossed the long threshold
        b.phase = PH_LONGFIRED;
        resetActivity();
        return EV_LONG;
      }
      if (!down) {                                           // released as a tap
        b.phase = PH_IDLE;
        resetActivity();
        return EV_SINGLE;
      }
      return EV_NONE;

    case PH_LONGFIRED:                                       // long already fired; await release
      if (!down) b.phase = PH_IDLE;
      return EV_NONE;
  }
  return EV_NONE;
}

// IDLE gestures: hold for a quick note, or use three short taps for one long WAV.
// A hold wins as soon as it crosses REC_HOLD_MS, so triple-tap detection never
// delays the primary quick-note gesture. One or two accidental taps do nothing.
bool handleIdleRec() {
  static bool tracking = false;
  static uint32_t t0 = 0;
  static uint32_t lastRelease = 0;
  static uint8_t tapCount = 0;

  uint32_t now = millis();
  if (tapCount > 0 && now - lastRelease > REC_TRIPLE_GAP_MS) tapCount = 0;

  if (!isDown(BTN_REC)) {
    if (!tracking) return false;

    tracking = false;
    if (now - t0 < BTN_DEBOUNCE_MS) return true;
    tapCount = (tapCount > 0 && now - lastRelease <= REC_TRIPLE_GAP_MS)
                 ? tapCount + 1 : 1;
    lastRelease = now;
    resetActivity();
    if (tapCount >= REC_TRIPLE_COUNT) {
      tapCount = 0;
      startLongRecordFlow();
    }
    return true;
  }

  if (!tracking) { tracking = true; t0 = now; resetActivity(); return true; }
  if (now - t0 >= REC_HOLD_MS) {
    tracking = false;
    tapCount = 0;
    startRecordFlow();
  }
  return true;   // consume the press while it's held
}
