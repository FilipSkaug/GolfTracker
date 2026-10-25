#pragma once
#include <stdint.h>

namespace input {

enum class Button : uint8_t { None, Short, Long };

void begin();
// Net encoder clicks since the last call (positive = clockwise).
int takeSteps();
// Call every loop; returns a press event at most once per press.
Button takeButton();
// Forget pending clicks/presses (e.g. the press that woke the display).
void reset();

}  // namespace input
