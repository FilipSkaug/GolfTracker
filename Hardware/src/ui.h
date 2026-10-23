#pragma once
#include <Arduino.h>

#include <vector>

#include "storage.h"

namespace ui {

void begin();
void setBrightness(uint8_t level);

// Colour (0xRRGGBB) for a hole score relative to par.
uint32_t scoreColor(int diff);

void drawHome(const char* kicker, const String& title, const String& sub, int index, int count);
void drawHole(const Round& r, uint8_t strokes);
void drawMenu(const String& title, const char* const* items, int count, int selected);
// Scrolling list with the selection in the middle (for long lists such as courses).
void drawList(const char* title, const std::vector<String>& items, int selected);
void drawSummary(const Round& r);
// Scorecard of a finished round; index/count for the "n / N" footer.
void drawRound(const Round& r, int index, int count);
// status: GolfBox upload result ("" = none), shown instead of the connection line.
void drawSync(bool phoneConnected, const String& homeIp, const String& status);

}  // namespace ui
