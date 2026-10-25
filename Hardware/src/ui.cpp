#include "ui.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "config.h"
#include "fonts/score_digits.h"

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
static constexpr uint32_t C_PARTY = 0xE879F9u;  // hole in one / albatross where it can't animate

// Ring geometry shared by all screens.
static constexpr int RING_OUT = 118;
static constexpr int RING_IN = 111;
static constexpr int RING_SEL_IN = 104;  // selected / current segment is thicker

bool isParty(int strokes, int par) { return strokes == 1 || strokes - par <= -3; }

uint32_t holeColor(int strokes, int par) {
  int diff = strokes - par;
  if (isParty(strokes, par)) return C_PARTY;
  if (diff == -2) return C_EAGLE;
  if (diff == -1) return C_BIRDIE;
  if (diff == 0) return C_PAR;
  if (diff == 1) return C_BOGEY;
  return C_DOUBLE;
}

uint32_t rainbow(uint32_t ms, int offset) {
  // Hue wheel, full saturation: one turn every 1.5 s.
  int h = ((ms * 360 / 1500) + offset) % 360;
  int x = 255 * (60 - abs(h % 120 - 60)) / 60;
  uint8_t r, g, b;
  switch (h / 60) {
    case 0: r = 255, g = x, b = 0; break;
    case 1: r = x, g = 255, b = 0; break;
    case 2: r = 0, g = 255, b = x; break;
    case 3: r = 0, g = x, b = 255; break;
    case 4: r = x, g = 0, b = 255; break;
    default: r = 255, g = 0, b = x; break;
  }
  return (uint32_t)r << 16 | g << 8 | b;
}

static String scoreName(int strokes, int par) {
  int diff = strokes - par;
  if (strokes == 1) return "Hole in one!";
  if (diff <= -4) return "Condor!";
  switch (diff) {
    case -3: return "Albatross!";
    case -2: return "Eagle";
    case -1: return "Birdie";
    case 0: return "Par";
    case 1: return "Bogey";
    case 2: return "Double bogey";
  }
  return "+" + String(diff);
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

// ---- Text helpers ------------------------------------------------------------------

static void textAt(const String& s, int x, int y, const lgfx::IFont* font, uint32_t color, float size = 1.0f) {
  cv.setFont(font);
  cv.setTextSize(size);
  cv.setTextColor(color);
  cv.setTextDatum(lgfx::middle_center);
  cv.drawString(s, x, y);
}

static void text(const String& s, int y, const lgfx::IFont* font, uint32_t color, float size = 1.0f) {
  textAt(s, CX, y, font, color, size);
}

static int widthOf(const String& s, const lgfx::IFont* font, float size = 1.0f) {
  cv.setFont(font);
  cv.setTextSize(size);
  return cv.textWidth(s);
}

// Shortens s with "..." until it is at most maxW pixels wide in font.
static String truncated(String s, const lgfx::IFont* font, int maxW) {
  if (widthOf(s, font) <= maxW) return s;
  while (s.length() > 1 && widthOf(s + "...", font) > maxW) s.remove(s.length() - 1);
  s.trim();
  return s + "...";
}

// Usable text width at height y inside a circle of radius r (default: inside the ring).
static int chordWidth(int y, int margin = 16, int r = RING_SEL_IN - 2) {
  int dy = y - CY;
  if (abs(dy) >= r) return 0;
  return (int)(2 * sqrtf((float)(r * r - dy * dy))) - margin;
}

// Small title at the top of a screen.
static void title(const String& s) {
  const lgfx::IFont* f = &fonts::Font2;
  text(truncated(ascii(s), f, chordWidth(36, 6, RING_SEL_IN)), 36, f, C_ACCENT);
}

// Draws a name as large as it fits around y: one line, or two lines split at a space.
static void fittedName(const String& raw, int y, uint32_t color) {
  String s = ascii(raw);
  const int maxW = chordWidth(y, 24);
  for (const lgfx::IFont* f : {(const lgfx::IFont*)&fonts::FreeSansBold18pt7b,
                               (const lgfx::IFont*)&fonts::FreeSansBold12pt7b}) {
    if (widthOf(s, f) <= maxW) {
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
    text(truncated(s, &fonts::FreeSansBold9pt7b, maxW), y, &fonts::FreeSansBold9pt7b, color);
    return;
  }
  text(truncated(s.substring(0, split), f, maxW), y - 13, f, color);
  text(truncated(s.substring(split + 1), f, maxW), y + 13, f, color);
}

// A status line with a coloured dot in front, centred as a whole.
static void statusLine(const String& s, int y, uint32_t color) {
  const lgfx::IFont* f = &fonts::Font2;
  String t = truncated(ascii(s), f, chordWidth(y, 28));
  int w = widthOf(t, f);
  int x = CX - (w + 12) / 2;
  cv.fillCircle(x + 4, y, 4, color);
  cv.setTextDatum(lgfx::middle_left);
  cv.setTextColor(color);
  cv.drawString(t, x + 12, y);
}

// ---- Rings -----------------------------------------------------------------------

// Angles: 0 = top, clockwise, in degrees. LovyanGFX's 0 is 3 o'clock.
static void segment(float a0, float a1, int rIn, int rOut, uint32_t color) {
  cv.fillArc(CX, CY, rIn, rOut, a0 - 90.0f, a1 - 90.0f, color);
}

// n equal segments around the bezel; returns their angular size.
static float segmentsGap(int n) { return n > 12 ? 2.5f : n > 6 ? 4.0f : 6.0f; }

static void dialRing(int n, int selected) {
  float seg = 360.0f / n, gap = segmentsGap(n);
  for (int i = 0; i < n; i++) {
    float a0 = i * seg + gap / 2, a1 = a0 + seg - gap;
    if (i == selected) segment(a0, a1, RING_SEL_IN, RING_OUT, C_ACCENT);
    else segment(a0, a1, RING_IN + 2, RING_OUT - 2, C_DIM);
  }
}

static void fullRing(uint32_t color, int rIn = RING_IN + 2, int rOut = RING_OUT - 2) {
  cv.fillArc(CX, CY, rIn, rOut, 0.0f, 360.0f, color);
}

// Holes as segments, coloured by score. current = index drawn thick (or -1) in currentColor.
// slim: a thinner ring hugging the edge, leaving more room for text (hole screen).
static void holeRing(const Round& r, int current, uint32_t currentColor, bool slim = false) {
  float seg = 360.0f / r.holes, gap = segmentsGap(r.holes);
  const int in = slim ? 113 : RING_IN, selIn = slim ? 109 : RING_SEL_IN, out = slim ? 119 : RING_OUT;
  for (int i = 0; i < r.holes; i++) {
    float a0 = i * seg + gap / 2, a1 = a0 + seg - gap;
    if (i == current) segment(a0, a1, selIn, out, currentColor);
    else if (r.strokes[i]) segment(a0, a1, in, out, holeColor(r.strokes[i], r.par[i]));
    else segment(a0, a1, in + 2, out - 2, C_DIM);
  }
}

// ---- Icons (drawn from primitives, centred on x,y, about s pixels big) -----------

static void icon(Icon id, int x, int y, int s, uint32_t c) {
  float h = s / 2.0f;
  switch (id) {
    case Icon::None:
      break;
    case Icon::Play:
      cv.fillTriangle(x - h * 0.55f, y - h * 0.8f, x - h * 0.55f, y + h * 0.8f, x + h * 0.85f, y, c);
      break;
    case Icon::Flag:  // golf flag on a green
      cv.fillEllipse(x, y + h * 0.82f, h * 0.8f, h * 0.18f, C_PILL);
      cv.fillRect(x - h * 0.35f, y - h * 0.9f, 3, h * 1.75f, c);
      cv.fillTriangle(x - h * 0.35f + 3, y - h * 0.9f, x - h * 0.35f + 3, y - h * 0.2f, x + h * 0.7f, y - h * 0.55f,
                      c);
      cv.fillCircle(x + h * 0.35f, y + h * 0.62f, h * 0.14f, C_TEXT);
      break;
    case Icon::Clock:
      cv.fillArc(x, y, h * 0.78f, h * 0.95f, 0, 360, c);
      cv.drawWideLine(x, y, x, y - h * 0.55f, 1.6f, c);
      cv.drawWideLine(x, y, x + h * 0.4f, y + h * 0.15f, 1.6f, c);
      break;
    case Icon::Phone:
      cv.fillRoundRect(x - h * 0.5f, y - h * 0.9f, h, h * 1.8f, h * 0.18f, c);
      cv.fillRoundRect(x - h * 0.38f, y - h * 0.72f, h * 0.76f, h * 1.3f, 2, C_BG);
      cv.fillCircle(x, y + h * 0.72f, h * 0.08f + 1, C_BG);
      break;
    case Icon::Wifi:
      for (int i = 0; i < 3; i++) {
        float r1 = h * (0.45f + i * 0.4f);
        cv.fillArc(x, y + h * 0.6f, r1 - h * 0.17f, r1, 225, 315, c);
      }
      cv.fillCircle(x, y + h * 0.6f, h * 0.14f, c);
      break;
    case Icon::Hotspot:  // broadcast: dot with waves both sides
      cv.fillCircle(x, y, h * 0.18f, c);
      for (int i = 0; i < 2; i++) {
        float r1 = h * (0.5f + i * 0.4f);
        cv.fillArc(x, y, r1 - h * 0.15f, r1, -40, 40, c);
        cv.fillArc(x, y, r1 - h * 0.15f, r1, 140, 220, c);
      }
      break;
    case Icon::Back:
      cv.fillTriangle(x - h * 0.85f, y, x - h * 0.1f, y - h * 0.65f, x - h * 0.1f, y + h * 0.65f, c);
      cv.fillRect(x - h * 0.15f, y - h * 0.22f, h * 0.95f, h * 0.44f, c);
      break;
    case Icon::Check:
      cv.drawWideLine(x - h * 0.65f, y, x - h * 0.15f, y + h * 0.5f, h * 0.13f, c);
      cv.drawWideLine(x - h * 0.15f, y + h * 0.5f, x + h * 0.7f, y - h * 0.55f, h * 0.13f, c);
      break;
    case Icon::Cross:
      cv.drawWideLine(x - h * 0.55f, y - h * 0.55f, x + h * 0.55f, y + h * 0.55f, h * 0.13f, c);
      cv.drawWideLine(x - h * 0.55f, y + h * 0.55f, x + h * 0.55f, y - h * 0.55f, h * 0.13f, c);
      break;
    case Icon::Trash:
      cv.fillRect(x - h * 0.75f, y - h * 0.72f, h * 1.5f, h * 0.16f, c);
      cv.fillRect(x - h * 0.25f, y - h * 0.9f, h * 0.5f, h * 0.2f, c);
      cv.fillRoundRect(x - h * 0.55f, y - h * 0.48f, h * 1.1f, h * 1.38f, 3, c);
      for (int i = -1; i <= 1; i++) cv.fillRect(x + i * h * 0.3f - 1, y - h * 0.3f, 2, h * 1.0f, C_BG);
      break;
    case Icon::Home:  // house: roof, walls, door
      cv.fillTriangle(x, y - h * 0.9f, x - h * 0.9f, y - h * 0.05f, x + h * 0.9f, y - h * 0.05f, c);
      cv.fillRect(x - h * 0.62f, y - h * 0.1f, h * 1.24f, h * 0.95f, c);
      cv.fillRect(x - h * 0.17f, y + h * 0.3f, h * 0.34f, h * 0.55f, C_BG);
      break;
    case Icon::Power:
      // Ring with a gap at the top (LovyanGFX: 0 deg = 3 o'clock, clockwise).
      cv.fillArc(x, y, h * 0.62f, h * 0.82f, 310, 360, c);
      cv.fillArc(x, y, h * 0.62f, h * 0.82f, 0, 230, c);
      cv.fillRoundRect(x - h * 0.1f, y - h * 0.95f, h * 0.2f, h * 0.85f, 2, c);
      break;
    case Icon::Upload:
      cv.fillTriangle(x, y - h * 0.9f, x - h * 0.6f, y - h * 0.2f, x + h * 0.6f, y - h * 0.2f, c);
      cv.fillRect(x - h * 0.17f, y - h * 0.25f, h * 0.34f, h * 0.75f, c);
      cv.fillRect(x - h * 0.75f, y + h * 0.7f, h * 1.5f, h * 0.18f, c);
      break;
  }
}

// ---- Frame -----------------------------------------------------------------------

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

void sleep() {
  lcd.setBrightness(0);
  lcd.sleep();
  digitalWrite(PIN_PWR_LCD, LOW);  // cut power to the panel
}

void wake() {
  digitalWrite(PIN_PWR_LCD, HIGH);
  delay(20);
  lcd.init();  // the panel lost its state without power
  lcd.setRotation(0);
  lcd.setBrightness(BRIGHTNESS_ON);
}

void dumpFrame(Stream& out) {
  out.printf("FRAME %d %d\n", 240, 240);
  uint8_t line[480];
  for (int y = 0; y < 240; y++) {
    for (int x = 0; x < 240; x++) {
      uint16_t c = cv.readPixel(x, y);
      line[x * 2] = c >> 8;
      line[x * 2 + 1] = c & 0xFF;
    }
    out.write(line, sizeof(line));
  }
  out.println();
}

// ---- Screens ---------------------------------------------------------------------

void drawDial(const String& heading, const DialItem* items, int count, int selected, const String& label,
              const String& sub) {
  begin_frame();
  dialRing(count, selected);
  title(heading);
  const DialItem& it = items[selected];
  if (it.badge) {
    text(it.badge, 84, &fonts::FreeSansBold24pt7b, C_ACCENT);
  } else {
    cv.fillCircle(CX, 84, 31, C_PILL);
    icon(it.icon, CX, 84, 34, C_ACCENT);
  }
  fittedName(label.isEmpty() ? String(it.label) : label, 142, C_TEXT);
  if (!sub.isEmpty()) {
    const lgfx::IFont* f = &fonts::Font2;
    text(truncated(ascii(sub), f, chordWidth(176)), 176, f, C_MUTED);
  }
  end_frame();
}

void drawList(const char* heading, const std::vector<String>& items, int selected) {
  begin_frame();
  const int n = items.size();
  // Scrollbar along the right edge: a dim track with a thumb for the position.
  if (n > 1) {
    const float span = 100.0f, thumb = max(14.0f, span / n);
    segment(90.0f - span / 2, 90.0f + span / 2, RING_IN + 2, RING_OUT - 2, C_DIM);
    float a0 = 90.0f - span / 2 + (span - thumb) * selected / (n - 1);
    segment(a0, a0 + thumb, RING_SEL_IN + 4, RING_OUT, C_ACCENT);
  }
  title(heading);
  // Centre a little low so the top row clears the title.
  const int spacing = 34;
  for (int d = -2; d <= 2; d++) {
    int i = selected + d;
    if (i < 0 || i >= n) continue;
    int y = CY + 8 + d * spacing;
    String s = ascii(items[i]);
    if (d == 0) {
      cv.fillRoundRect(CX - 96, y - 18, 192, 36, 18, C_PILL);
      const lgfx::IFont* f = widthOf(s, &fonts::FreeSansBold12pt7b) <= 172 ? &fonts::FreeSansBold12pt7b
                                                                             : &fonts::FreeSansBold9pt7b;
      text(truncated(s, f, 172), y, f, C_TEXT);
    } else {
      const lgfx::IFont* f = &fonts::FreeSans9pt7b;
      text(truncated(s, f, chordWidth(y, 30)), y, f, abs(d) == 1 ? C_MUTED : C_DIM);
    }
  }
  end_frame();
}

// Falling confetti for a hole in one / albatross.
static void confetti(uint32_t ms) {
  for (int k = 0; k < 28; k++) {
    int speed = 40 + (k * 37) % 50;  // px per second
    int x = 24 + (k * 67) % 192 + (int)(6 * sinf(ms / 300.0f + k));
    int y = (int)((ms * speed / 1000 + k * 53) % 260) - 10;
    if ((x - CX) * (x - CX) + (y - CY) * (y - CY) > 98 * 98) continue;
    int w = k % 3 == 0 ? 6 : 4, h = k % 3 == 0 ? 3 : 6;
    cv.fillRect(x, y, w, h, rainbow(ms, k * 47));
  }
}

void drawHole(const Round& r, uint8_t strokes, uint32_t dotColor) {
  begin_frame();
  int par = r.par[r.hole];
  uint32_t ms = millis();
  bool party = isParty(strokes, par);
  uint32_t c = party ? rainbow(ms, 0) : holeColor(strokes, par);
  holeRing(r, r.hole, c, true);
  if (party) confetti(ms);
  if (dotColor) cv.fillCircle(CX, 20, 4, dotColor);

  // Layout from the mockup: the number centred on the screen, "Hole N" and the score
  // name in the same regular font, the same distance above and below it. Placed by
  // baseline so the glyphs themselves are centred, not the font's line box.
  const lgfx::IFont* f = &fonts::FreeSans9pt7b;  // capital height 13 px
  const int capH = 13, gap = 84;                 // label centres 84 px from the centre
  const int numH = 98;                           // digit height in fonts/score_digits.h
  cv.setTextDatum(lgfx::baseline_center);
  cv.setFont(f);
  cv.setTextSize(1);
  cv.setTextColor(C_TEXT);
  cv.drawString("Hole " + String(r.firstHole + r.hole), CX, CY - gap + capH / 2);
  // Anti-aliased digits rendered at exactly this size (tools/make_vlw.py), so the
  // edges stay smooth instead of a scaled-up bitmap font's stair-steps.
  cv.loadFont(kScoreDigitsVlw);
  cv.setTextDatum(lgfx::baseline_center);
  cv.setTextColor(c);  // no background colour: blends with what's behind (e.g. confetti)
  cv.drawString(String(strokes), CX, CY + numH / 2);
  cv.unloadFont();
  String name = scoreName(strokes, par);  // Hole in one!, Albatross!, Eagle, ... +3
  cv.setFont(f);
  cv.setTextSize(1);
  cv.setTextColor(c);
  cv.drawString(name, CX, CY + gap + capH / 2);
  end_frame();
}

static String outIn(const Round& r, const Totals& t) {
  if (t.played < r.holes) return String(t.played) + "/" + String(r.holes) + " holes";
  if (r.holes == 18) return "Out " + String(totals(r, 0, 9).strokes) + "  In " + String(totals(r, 9, 18).strokes);
  return String(r.holes) + " holes";
}

void drawSummary(const Round& r) {
  begin_frame();
  holeRing(r, -1, 0);
  Totals t = totals(r);
  int diff = t.strokes - t.par;
  title("ROUND SAVED");
  const lgfx::IFont* nf = &fonts::FreeSansBold9pt7b;
  text(truncated(ascii(r.course), nf, chordWidth(54)), 54, nf, C_TEXT);
  text(String(t.strokes), 106, &fonts::Font7, C_TEXT, t.strokes >= 100 ? 1.2f : 1.5f);
  text(toPar(diff), 156, &fonts::FreeSansBold12pt7b, diff <= 0 ? C_BIRDIE : C_BOGEY);
  text(outIn(r, t), 180, &fonts::Font2, C_MUTED);
  text("PRESS", 202, &fonts::Font2, C_ACCENT);
  end_frame();
}

void drawRound(const Round& r, int index, int count) {
  begin_frame();
  // Ring of holes with the strokes written in each segment, hole 1 at the top.
  float seg = 360.0f / r.holes, gap = segmentsGap(r.holes);
  const int rIn = 97, rOut = RING_OUT, rText = (rIn + rOut) / 2;
  for (int i = 0; i < r.holes; i++) {
    float a0 = i * seg + gap / 2, a1 = a0 + seg - gap;
    int s = r.strokes[i];
    segment(a0, a1, rIn, rOut, s ? holeColor(s, r.par[i]) : C_DIM);
    float mid = (a0 + a1) / 2 * DEG_TO_RAD;
    int x = CX + sinf(mid) * rText, y = CY - cosf(mid) * rText;
    textAt(s ? String(s) : String("-"), x, y + 1, &fonts::Font2, s ? C_BG : C_MUTED);
  }
  // Marker at hole 1.
  cv.fillTriangle(CX - 5, CY - rIn + 2, CX + 5, CY - rIn + 2, CX, CY - rIn + 9, C_MUTED);

  Totals t = totals(r);
  int diff = t.strokes - t.par;
  const int inner = rIn - 4;
  text("ROUND #" + String(r.id), 50, &fonts::Font2, C_ACCENT);
  const lgfx::IFont* nf = &fonts::FreeSansBold9pt7b;
  text(truncated(ascii(r.course), nf, chordWidth(72, 16, inner)), 72, nf, C_TEXT);
  text(String(t.strokes), 116, &fonts::Font7, C_TEXT, t.strokes >= 100 ? 1.0f : 1.25f);
  text(toPar(diff), 156, &fonts::FreeSansBold12pt7b, diff <= 0 ? C_BIRDIE : C_BOGEY);
  text(outIn(r, t), 178, &fonts::Font2, C_MUTED);
  text(String(index + 1) + " / " + String(count), 196, &fonts::Font2, C_MUTED);
  end_frame();
}

void drawSyncWifi(bool phoneConnected) {
  begin_frame();
  fullRing(phoneConnected ? C_ACCENT : C_BIRDIE);
  title("WI-FI SYNC");
  icon(Icon::Wifi, CX, 64, 34, phoneConnected ? C_ACCENT : C_BIRDIE);
  text(WIFI_SSID, 104, &fonts::FreeSansBold12pt7b, C_TEXT);
  text(String("Password ") + WIFI_PASS, 128, &fonts::Font2, C_MUTED);
  text(MDNS_NAME ".local", 152, &fonts::FreeSansBold12pt7b, C_ACCENT);
  statusLine(phoneConnected ? "Phone connected" : "Waiting for phone", 182, phoneConnected ? C_ACCENT : C_MUTED);
  text("Press to stop", 206, &fonts::Font2, C_MUTED);
  end_frame();
}

void drawSyncHotspot(const String& ssid, NetState net, const String& netText, const String& ip,
                     const String& golfbox) {
  begin_frame();
  uint32_t c = net == NetState::Online ? C_ACCENT : net == NetState::Failed ? C_BOGEY : C_EAGLE;
  if (net == NetState::Joining) {
    // Spinner: a bright arc travelling round a dim ring.
    fullRing(C_DIM);
    float a = (millis() / 4) % 360;
    segment(a, a + 70, RING_SEL_IN, RING_OUT, c);
  } else {
    fullRing(c);
  }
  title("HOTSPOT SYNC");
  icon(Icon::Hotspot, CX, 60, 34, c);
  const lgfx::IFont* nf = &fonts::FreeSansBold9pt7b;
  text(truncated(ascii(ssid), nf, chordWidth(92)), 92, nf, C_TEXT);
  statusLine(netText, 114, c);
  if (net == NetState::Online) {
    text("Open in Safari", 140, &fonts::Font2, C_MUTED);
    text(ip, 162, &fonts::FreeSansBold12pt7b, C_ACCENT);
  } else {
    text("Turn on Personal Hotspot", 142, &fonts::Font2, C_MUTED);
    text("and keep that screen open", 160, &fonts::Font2, C_MUTED);
  }
  if (!golfbox.isEmpty()) {
    bool ok = golfbox.indexOf("sent") >= 0 || golfbox.indexOf("sending") >= 0;
    statusLine(golfbox, 188, ok ? C_ACCENT : C_BOGEY);
  }
  text("Press to stop", 210, &fonts::Font2, C_MUTED);
  end_frame();
}

}  // namespace ui
