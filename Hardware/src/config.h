#pragma once

// ---- CrowPanel 1.28" Rotary pinout -------------------------------------------
#define PIN_LCD_SCLK 10
#define PIN_LCD_MOSI 11
#define PIN_LCD_DC 3
#define PIN_LCD_CS 9
#define PIN_LCD_RST 14
#define PIN_LCD_BL 46

#define PIN_ENC_A 45
#define PIN_ENC_B 42
#define PIN_BTN 41  // pressing the knob/display down

#define PIN_LED 48  // WS2812 ring
#define LED_COUNT 5

#define PIN_PWR_LCD 1  // MOSFET powering LCD + touch
#define PIN_PWR_EXT 2  // 5V on the expansion connectors

// ---- Tuning -------------------------------------------------------------------
// Encoder transitions per click. If one click moves the count by 2, set this to
// 2 x the current value; if you need two clicks per step, halve it.
#define ENC_COUNTS_PER_DETENT 4
// Set to 1 if turning clockwise lowers the number.
#define ENC_REVERSE 0

#define LONG_PRESS_MS 700
#define DIM_AFTER_MS (3UL * 60UL * 1000UL)  // dim screen after 3 min idle
#define BRIGHTNESS_ON 255
#define BRIGHTNESS_DIM 12
#define LED_BRIGHTNESS 30

// ---- Golf -----------------------------------------------------------------------
#define MAX_HOLES 18
#define MAX_STROKES 15
#define MAX_COURSES 50

// ---- Phone sync (Wi-Fi access point hosted by the display) -------------------
#define WIFI_SSID "GolfTracker"
#define WIFI_PASS "golf1234"  // min. 8 characters
#define MDNS_NAME "golf"       // -> http://golf.local
