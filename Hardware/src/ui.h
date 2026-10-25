#pragma once
#include <Arduino.h>

#include <vector>

#include "storage.h"

// Screens for the 240x240 round display. Menus are dials: one ring segment per
// option around the edge (the knob moves the highlight), the selection's icon
// and name in the middle.
namespace ui {

enum class Icon : uint8_t { None, Play, Flag, Clock, Phone, Wifi, Hotspot, Back, Check, Cross, Trash, Upload, Power, Home };

struct DialItem {
  const char* label;
  Icon icon;
  const char* badge;  // short text shown instead of the icon (e.g. "18"), or nullptr
};

void begin();
void setBrightness(uint8_t level);
// Screen off (panel power cut) / back on with the last frame redrawn by the caller.
void sleep();
void wake();

// Hole in one or albatross (or better): the "party" celebration.
bool isParty(int strokes, int par);
// Colour (0xRRGGBB) for a hole: gold eagle, blue birdie, white par, orange bogey,
// red double bogey or worse, magenta for party holes (where it can't animate).
uint32_t holeColor(int strokes, int par);
// Rainbow colour that cycles with time, for the party animation.
uint32_t rainbow(uint32_t ms, int offset);

// label/sub override the selected item's name and add a line under it.
void drawDial(const String& title, const DialItem* items, int count, int selected, const String& label = "",
              const String& sub = "");
// Scrolling list with the selection in the middle and a scroll arc on the right.
void drawList(const char* title, const std::vector<String>& items, int selected);

// dotColor: small status dot at the top (live GameBook sync), 0 = none.
void drawHole(const Round& r, uint8_t strokes, uint32_t dotColor = 0);
void drawSummary(const Round& r);
// Scorecard of a finished round as a ring of holes; index/count for "n / N".
void drawRound(const Round& r, int index, int count);

// Sync via the display's own "GolfTracker" network.
void drawSyncWifi(bool phoneConnected);

// Sync via the phone's hotspot. Joining shows a spinning ring (redraw ~10x/s).
enum class NetState { Joining, Online, Failed };
// netText: e.g. "Online"/"Hotspot not found"; ip: address to open ("" until online);
// golfbox: upload result ("" = none).
void drawSyncHotspot(const String& ssid, NetState net, const String& netText, const String& ip,
                     const String& golfbox);

// Debug: writes the current frame over serial as "FRAME 240 240\n" + RGB565 big-endian.
void dumpFrame(Stream& out);

}  // namespace ui
