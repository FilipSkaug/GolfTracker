#include "ui.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "config.h"

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;

 public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 80000000;
      cfg.freq_read = 20000000;
      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = PIN_LCD_SCLK;
      cfg.pin_mosi = PIN_LCD_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_dc = PIN_LCD_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = PIN_LCD_CS;
      cfg.pin_rst = PIN_LCD_RST;
      cfg.pin_busy = -1;
      cfg.memory_width = 240;
      cfg.memory_height = 240;
      cfg.panel_width = 240;
      cfg.panel_height = 240;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = PIN_LCD_BL;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

namespace ui {

static LGFX lcd;
static LGFX_Sprite cv(&lcd);

static constexpr int CX = 120;
static constexpr int CY = 120;

static constexpr uint32_t C_BG = 0x0A0F0Cu;
static constexpr uint32_t C_TEXT = 0xF1F5F2u;
static constexpr uint32_t C_MUTED = 0x8A968Eu;
static constexpr uint32_t C_DIM = 0x26302Au;
static constexpr uint32_t C_ACCENT = 0x4ADE80u;  // fairway green
static constexpr uint32_t C_PILL = 0x1E3A2Au;

static constexpr uint32_t C_EAGLE = 0xFACC15u;
static constexpr uint32_t C_BIRDIE = 0x38BDF8u;
static constexpr uint32_t C_PAR = 0xF1F5F2u;
static constexpr uint32_t C_BOGEY = 0xFB923Cu;
static constexpr uint32_t C_DOUBLE = 0xF43F5Eu;

uint32_t scoreColor(int diff) {
  if (diff <= -2) return C_EAGLE;
  if (diff == -1) return C_BIRDIE;
  if (diff == 0) return C_PAR;
  if (diff == 1) return C_BOGEY;
  return C_DOUBLE;
}

static const char* scoreName(int strokes, int diff) {
  if (strokes == 1) return "Hole in one!";
  switch (diff) {
    case -3: return "Albatross";
    case -2: return "Eagle";
    case -1: return "Birdie";
    case 0: return "Par";
    case 1: return "Bogey";
    case 2: return "Double bogey";
    case 3: return "Triple bogey";
  }
  return diff < 0 ? "Amazing" : "";
}

static String toPar(int diff) {
  if (diff == 0) return "E";
  return diff > 0 ? "+" + String(diff) : String(diff);
}

// The built-in fonts are ASCII only, so fold Nordic/European letters for display.
static String ascii(const String& s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (c < 0x80) {
      out += (char)c;
      continue;
    }
    if (c == 0xC3 && i + 1 < s.length()) {
      uint8_t d = s[++i];
      switch (d) {
        case 0xA6: out += "ae"; continue;
        case 0x86: out += "AE"; continue;
        case 0xB8: case 0xB6: case 0xB3: case 0xB4: out += 'o'; continue;
        case 0x98: case 0x96: case 0x93: case 0x94: out += 'O'; continue;
        case 0xA5: case 0xA4: case 0xA1: case 0xA0: case 0xA2: out += 'a'; continue;
        case 0x85: case 0x84: case 0x81: case 0x80: case 0x82: out += 'A'; continue;
        case 0xA9: case 0xA8: case 0xAA: case 0xAB: out += 'e'; continue;
        case 0x89: case 0x88: out += 'E'; continue;
        case 0xBC: case 0xBA: out += 'u'; continue;
        case 0x9C: case 0x9A: out += 'U'; continue;
      }
      continue;
    }
    // Skip other multi-byte sequences.
    while (i + 1 < s.length() && (s[i + 1] & 0xC0) == 0x80) i++;
    out += '?';
  }
  return out;
}

static void text(const String& s, int y, const lgfx::IFont* font, uint32_t color, float size = 1.0f) {
  cv.setFont(font);
  cv.setTextSize(size);
  cv.setTextColor(color);
  cv.setTextDatum(lgfx::middle_center);
  cv.drawString(s, CX, y);
}

// Shortens s with "..." until it is at most maxW pixels wide in font.
static String truncated(String s, const lgfx::IFont* font, int maxW) {
  cv.setFont(font);
  cv.setTextSize(1);
  if (cv.textWidth(s) <= maxW) return s;
  while (s.length() > 1 && cv.textWidth(s + "...") > maxW) s.remove(s.length() - 1);
  s.trim();
  return s + "...";
}

// Usable text width at height y inside the round screen.
static int chordWidth(int y, int margin = 30) {
  int dy = y - CY;
  return (int)(2 * sqrtf(115.0f * 115.0f - dy * dy)) - margin;
}

// Draws a name as large as it fits: one line, or two lines split at a space.
static void fittedTitle(const String& raw, int y, uint32_t color) {
  String s = ascii(raw);
  const int maxW = 186;
  cv.setTextSize(1);
  for (const lgfx::IFont* f : {(const lgfx::IFont*)&fonts::FreeSansBold18pt7b,
                               (const lgfx::IFont*)&fonts::FreeSansBold12pt7b}) {
    cv.setFont(f);
    if (cv.textWidth(s) <= maxW) {
      text(s, y, f, color);
      return;
    }
  }
  int mid = s.length() / 2, split = -1;
  for (int d = 0; d <= mid && split < 0; d++) {
    if (mid - d > 0 && s[mid - d] == ' ') split = mid - d;
    else if (mid + d < (int)s.length() && s[mid + d] == ' ') split = mid + d;
  }
  const lgfx::IFont* f = &fonts::FreeSansBold12pt7b;
  if (split < 0) {
    text(s, y, &fonts::FreeSansBold9pt7b, color);
    return;
  }
  text(s.substring(0, split), y - 14, f, color);
  text(s.substring(split + 1), y + 14, f, color);
}

static void bigNumber(int n, int y, uint32_t color) {
  float size = n >= 100 ? 1.4f : 2.0f;
  text(String(n), y, &fonts::Font7, color, size);
}

static void begin_frame() { cv.fillScreen(C_BG); }
static void end_frame() { cv.pushSprite(0, 0); }

void begin() {
  pinMode(PIN_PWR_LCD, OUTPUT);
  digitalWrite(PIN_PWR_LCD, HIGH);
  pinMode(PIN_PWR_EXT, OUTPUT);
  digitalWrite(PIN_PWR_EXT, HIGH);
  delay(20);

  lcd.init();
  lcd.setRotation(0);
  lcd.setBrightness(BRIGHTNESS_ON);
  lcd.fillScreen(TFT_BLACK);
  cv.setColorDepth(16);
  cv.setPsram(false);
  if (!cv.createSprite(240, 240)) Serial.println("Sprite allocation failed");
}

void setBrightness(uint8_t level) { lcd.setBrightness(level); }

void drawHome(const char* kicker, const String& title, const String& sub, int index, int count) {
  begin_frame();
  cv.drawCircle(CX, CY, 117, C_DIM);
  text(kicker, 52, &fonts::FreeSansBold9pt7b, C_ACCENT);
  fittedTitle(title, 112, C_TEXT);
  text(ascii(sub), 160, &fonts::FreeSans9pt7b, C_MUTED);

  // Turn hints.
  cv.fillTriangle(14, CY, 24, CY - 9, 24, CY + 9, count > 1 ? C_MUTED : C_DIM);
  cv.fillTriangle(226, CY, 216, CY - 9, 216, CY + 9, count > 1 ? C_MUTED : C_DIM);

  if (count <= 12) {
    int x0 = CX - (count - 1) * 6;
    for (int i = 0; i < count; i++) {
      if (i == index) cv.fillCircle(x0 + i * 12, 196, 4, C_ACCENT);
      else cv.fillCircle(x0 + i * 12, 196, 2, C_MUTED);
    }
  } else {
    text(String(index + 1) + " / " + String(count), 196, &fonts::FreeSans9pt7b, C_MUTED);
  }
  end_frame();
}

static void ring(const Round& r, uint8_t current) {
  float seg = 360.0f / r.holes;
  float gap = r.holes > 9 ? 3.0f : 4.0f;
  for (int i = 0; i < r.holes; i++) {
    float a0 = -90.0f + i * seg + gap / 2;
    float a1 = a0 + seg - gap;
    if (i == r.hole) {
      cv.fillArc(CX, CY, 104, 119, a0, a1, scoreColor(current - r.par[i]));
    } else if (r.strokes[i]) {
      cv.fillArc(CX, CY, 110, 117, a0, a1, scoreColor(r.strokes[i] - r.par[i]));
    } else {
      cv.fillArc(CX, CY, 110, 117, a0, a1, C_DIM);
    }
  }
}

void drawHole(const Round& r, uint8_t strokes) {
  begin_frame();
  ring(r, strokes);

  int par = r.par[r.hole];
  int diff = strokes - par;
  text("HOLE " + String(r.firstHole + r.hole), 42, &fonts::FreeSansBold12pt7b, C_TEXT);
  text("Par " + String(par), 66, &fonts::FreeSans9pt7b, C_MUTED);
  bigNumber(strokes, 122, scoreColor(diff));
  text(scoreName(strokes, diff), 172, &fonts::FreeSansBold9pt7b, scoreColor(diff));

  // Running total including the value currently on the wheel.
  Totals t = totals(r);
  if (r.strokes[r.hole]) {
    t.strokes -= r.strokes[r.hole];
    t.par -= par;
  }
  t.strokes += strokes;
  t.par += par;
  text(String(t.strokes) + "  (" + toPar(t.strokes - t.par) + ")", 198, &fonts::FreeSans9pt7b, C_MUTED);
  end_frame();
}

void drawMenu(const String& title, const char* const* items, int count, int selected) {
  begin_frame();
  cv.drawCircle(CX, CY, 117, C_DIM);
  const lgfx::IFont* tf = &fonts::FreeSansBold9pt7b;
  text(truncated(ascii(title), tf, chordWidth(52)), 52, tf, C_ACCENT);
  const int spacing = 32;
  int y0 = CY + 8 - (count - 1) * spacing / 2;
  for (int i = 0; i < count; i++) {
    int y = y0 + i * spacing;
    if (i == selected) cv.fillRoundRect(CX - 80, y - 14, 160, 28, 14, C_PILL);
    text(items[i], y, i == selected ? &fonts::FreeSansBold9pt7b : &fonts::FreeSans9pt7b,
         i == selected ? C_TEXT : C_MUTED);
  }
  end_frame();
}

void drawList(const char* title, const std::vector<String>& items, int selected) {
  begin_frame();
  cv.drawCircle(CX, CY, 117, C_DIM);
  text(title, 32, &fonts::FreeSansBold9pt7b, C_ACCENT);
  const int n = items.size();
  const int spacing = 34;
  for (int d = -2; d <= 2; d++) {
    int i = selected + d;
    if (i < 0 || i >= n) continue;
    int y = CY + 4 + d * spacing;
    String s = ascii(items[i]);
    if (d == 0) {
      cv.fillRoundRect(CX - 104, y - 17, 208, 34, 17, C_PILL);
      const lgfx::IFont* f = &fonts::FreeSansBold12pt7b;
      cv.setFont(f);
      cv.setTextSize(1);
      if (cv.textWidth(s) > 190) f = &fonts::FreeSansBold9pt7b;
      text(truncated(s, f, 190), y, f, C_TEXT);
    } else {
      const lgfx::IFont* f = &fonts::FreeSans9pt7b;
      text(truncated(s, f, chordWidth(y)), y, f, abs(d) == 1 ? C_MUTED : C_DIM);
    }
  }
  text(String(selected + 1) + " / " + String(n) + "   hold: back", 214, &fonts::Font2, C_MUTED);
  end_frame();
}

void drawSummary(const Round& r) {
  begin_frame();
  cv.drawCircle(CX, CY, 117, C_ACCENT);
  Totals t = totals(r);
  int diff = t.strokes - t.par;
  text("ROUND SAVED", 40, &fonts::FreeSansBold9pt7b, C_ACCENT);
  text(truncated(ascii(r.course), &fonts::FreeSansBold9pt7b, chordWidth(64)), 64, &fonts::FreeSansBold9pt7b, C_TEXT);
  bigNumber(t.strokes, 114, C_TEXT);
  text(toPar(diff), 160, &fonts::FreeSansBold12pt7b, diff <= 0 ? C_BIRDIE : C_BOGEY);
  String detail;
  if (r.holes == 18) {
    detail = "Out " + String(totals(r, 0, 9).strokes) + "  In " + String(totals(r, 9, 18).strokes);
  } else {
    detail = String(t.played) + " holes";
  }
  if (t.played < r.holes) detail = String(t.played) + "/" + String(r.holes) + " holes played";
  text(detail, 186, &fonts::FreeSans9pt7b, C_MUTED);
  text("Press: sync to phone", 210, &fonts::Font2, C_MUTED);
  end_frame();
}

// One row of up to 9 holes: small hole numbers above coloured score cells.
static void scoreRow(const Round& r, int from, int to, int y) {
  const int cell = 20, gap = 1;
  int n = to - from;
  int x0 = CX - (n * (cell + gap) - gap) / 2;
  for (int i = from; i < to; i++) {
    int x = x0 + (i - from) * (cell + gap);
    cv.setFont(&fonts::Font0);
    cv.setTextSize(1);
    cv.setTextDatum(lgfx::middle_center);
    cv.setTextColor(C_MUTED);
    cv.drawString(String(r.firstHole + i), x + cell / 2, y - 16);
    if (!r.strokes[i]) {
      cv.drawRoundRect(x, y - cell / 2, cell, cell, 4, C_DIM);
      continue;
    }
    int diff = r.strokes[i] - r.par[i];
    uint32_t c = scoreColor(diff);
    if (diff == 0) {
      cv.drawRoundRect(x, y - cell / 2, cell, cell, 4, C_DIM);
    } else {
      cv.fillRoundRect(x, y - cell / 2, cell, cell, 4, c);
    }
    cv.setFont(&fonts::Font2);
    cv.setTextColor(diff == 0 ? C_TEXT : C_BG);
    cv.drawString(String(r.strokes[i]), x + cell / 2, y + 1);
  }
}

void drawRound(const Round& r, int index, int count) {
  begin_frame();
  cv.drawCircle(CX, CY, 117, C_DIM);
  Totals t = totals(r);
  int diff = t.strokes - t.par;
  text("ROUND #" + String(r.id), 26, &fonts::Font2, C_ACCENT);
  const lgfx::IFont* nf = &fonts::FreeSansBold9pt7b;
  text(truncated(ascii(r.course), nf, chordWidth(48)), 48, nf, C_TEXT);

  // Total and score to par side by side.
  String total = String(t.strokes), par = toPar(diff);
  cv.setTextSize(1);
  cv.setFont(&fonts::Font7);
  int wTotal = cv.textWidth(total);
  cv.setFont(&fonts::FreeSansBold12pt7b);
  int wPar = cv.textWidth(par);
  int x = CX - (wTotal + 12 + wPar) / 2;
  cv.setTextDatum(lgfx::middle_left);
  cv.setFont(&fonts::Font7);
  cv.setTextColor(C_TEXT);
  cv.drawString(total, x, 88);
  cv.setFont(&fonts::FreeSansBold12pt7b);
  cv.setTextColor(diff <= 0 ? C_BIRDIE : C_BOGEY);
  cv.drawString(par, x + wTotal + 12, 90);

  if (r.holes > 9) {
    scoreRow(r, 0, 9, 134);
    scoreRow(r, 9, r.holes, 172);
  } else {
    scoreRow(r, 0, r.holes, 150);
  }
  text(String(index + 1) + " / " + String(count), 210, &fonts::Font2, C_MUTED);
  end_frame();
}

void drawSync(bool phoneConnected, const String& homeIp, const String& status) {
  begin_frame();
  bool reachable = phoneConnected || !homeIp.isEmpty();
  cv.drawCircle(CX, CY, 117, reachable ? C_ACCENT : C_BIRDIE);
  text("PHONE SYNC", 44, &fonts::FreeSansBold9pt7b, C_BIRDIE);
  text("Wi-Fi", 74, &fonts::Font2, C_MUTED);
  text(WIFI_SSID, 94, &fonts::FreeSansBold12pt7b, C_TEXT);
  text("Password", 122, &fonts::Font2, C_MUTED);
  text(WIFI_PASS, 142, &fonts::FreeSansBold12pt7b, C_TEXT);
  if (!status.isEmpty()) text(status, 168, &fonts::Font2, status.indexOf("sent") >= 0 ? C_ACCENT : C_BOGEY);
  else if (phoneConnected) text("Phone connected", 168, &fonts::Font2, C_ACCENT);
  else if (!homeIp.isEmpty()) text("Home Wi-Fi " + homeIp, 168, &fonts::Font2, C_ACCENT);
  else text("Waiting for phone...", 168, &fonts::Font2, C_MUTED);
  text(MDNS_NAME ".local", 190, &fonts::FreeSansBold9pt7b, reachable ? C_ACCENT : C_MUTED);
  text("Press when done", 214, &fonts::Font2, C_MUTED);
  end_frame();
}

}  // namespace ui
