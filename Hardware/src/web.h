#pragma once
#include <Arduino.h>

// Phone sync, in one of two modes:
//   Hotspot: the display joins the phone's hotspot (saved under Settings). The
//            phone keeps its internet, opens the display at its hotspot IP, and
//            rounds can be sent to GolfBox.
//   Wifi:    the display hosts its own "GolfTracker" network (golf.local).
//            No internet; for when there's no hotspot or to fix its settings.
namespace web {

enum class Mode { Hotspot, Wifi };

bool hotspotConfigured();
// Falls back to Wifi when no hotspot is saved.
void start(Mode mode);
void stop();
void handle();
Mode mode();

// Wifi mode: a phone is connected to the GolfTracker network.
bool phoneConnected();
// Hotspot mode: the display's address on the hotspot, or "" until joined.
String homeIp();
// True once after a round was marked "send again" on the phone page.
bool takeResendRequest();

// Hotspot mode: why joining failed ("Hotspot not found", ...), or "".
String homeError();

enum class HomeState { Off, Joining, Online, Failed };  // Off = Wifi mode
HomeState homeState();

}  // namespace web
