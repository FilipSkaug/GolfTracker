#include "input.h"

#include <Arduino.h>

#include "config.h"

namespace input {

static volatile int32_t s_counts = 0;
static volatile uint8_t s_state = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Quadrature decoding table indexed by (previous AB << 2 | current AB).
static const int8_t kQuad[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

static void IRAM_ATTR onEncoder() {
  uint8_t ab = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  s_state = ((s_state << 2) | ab) & 0x0F;
  portENTER_CRITICAL_ISR(&s_mux);
  s_counts += kQuad[s_state];
  portEXIT_CRITICAL_ISR(&s_mux);
}

void begin() {
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_BTN, INPUT_PULLUP);
  s_state = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), onEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), onEncoder, CHANGE);
}

int takeSteps() {
  portENTER_CRITICAL(&s_mux);
  int steps = s_counts / ENC_COUNTS_PER_DETENT;
  s_counts -= steps * ENC_COUNTS_PER_DETENT;
  portEXIT_CRITICAL(&s_mux);
  return ENC_REVERSE ? -steps : steps;
}

Button takeButton() {
  static bool stable = false;  // true = pressed
  static bool lastRaw = false;
  static uint32_t rawChangedAt = 0;
  static uint32_t pressedAt = 0;
  static bool longFired = false;

  uint32_t now = millis();
  bool raw = digitalRead(PIN_BTN) == LOW;
  if (raw != lastRaw) {
    lastRaw = raw;
    rawChangedAt = now;
  }
  if (raw != stable && now - rawChangedAt > 25) {
    stable = raw;
    if (stable) {
      pressedAt = now;
      longFired = false;
    } else if (!longFired) {
      return Button::Short;
    }
  }
  if (stable && !longFired && now - pressedAt >= LONG_PRESS_MS) {
    longFired = true;
    return Button::Long;
  }
  return Button::None;
}

}  // namespace input
