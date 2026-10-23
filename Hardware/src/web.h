#pragma once
#include <Arduino.h>

// Phone sync: the display hosts its own Wi-Fi network and a small web app
// (http://golf.local or http://192.168.4.1) for managing courses and rounds.
namespace web {

void start();
void stop();
void handle();
bool phoneConnected();
// IP on the home Wi-Fi, or "" when not joined (none configured / not connected yet).
String homeIp();

}  // namespace web
