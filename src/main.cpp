// CYD flip clock: 24h HH:MM, NTP time (UK), WiFiManager portal, ArduinoOTA.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>
#include <time.h>
#include "digits.h"

static_assert(sizeof(OTA_PASS) > 8, "export FLIPCLOCK_OTA_PASS (8+ chars) before building");

// ---- tuning knobs ----
const char *TZ_UK = "GMT0BST,M3.5.0/1,M10.5.0";
const int NIGHT_START = 23, NIGHT_END = 7;   // dim from 23:00 to 06:59
const int NIGHT_LEVEL = 20, MIN_BRIGHT = 10;  // backlight PWM 0-255; daytime level is set in Settings
const int STEPS = 8, FRAME_MS = 12;          // flip animation per half
const uint32_t HOLD_MS = 1500;               // long-press opens Settings
const uint32_t SETTINGS_IDLE_MS = 30000;     // Settings closes itself after this
const int TS_X0 = 200, TS_X1 = 3700, TS_Y0 = 240, TS_Y1 = 3800; // touch calibration (raw -> screen)

// ---- layout (320x240 landscape) ----
const int CW = DIG_W, CH = DIG_H, HALF = CH / 2, CARD_Y = 20, RADIUS = 6;
const int CARD_X[4] = {4, 80, 170, 246};
const uint16_t BG = TFT_BLACK, CARD = 0x2104, FG = 0xEF7D, COLON = 0x8410;
// tap cycles digit colour: default, blue, green, red, yellow
const uint16_t COLOURS[] = {FG, 0x353F, 0x07E0, 0xF8A2, 0xFFE0};
const int N_COLOURS = sizeof COLOURS / sizeof *COLOURS;
int colour = 0;
const int SHADES = 8; // darkness levels for the moving flap

TFT_eSPI tft;
TFT_eSprite spr(&tft);
uint16_t *buf;
uint16_t pal[SHADES][16]; // [shade][alpha] -> byte-swapped RGB565 (sprite buffer order)
uint8_t inset[CH];        // rounded-corner inset per row

SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(33, 36);
WiFiManager wm;
Preferences prefs;
int bright = 255; // daytime backlight, persisted

int shown[4] = {-1, -1, -1, -1};
int lastMin = -1; // -1 forces a full redraw
int lastSec = -1;

void msg(const char *a, const char *b = "", const char *c = "") {
  tft.fillScreen(BG);
  tft.setTextColor(FG, BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(a, 160, 85, 4);
  tft.drawString(b, 160, 120, 4);
  tft.drawString(c, 160, 155, 2);
  lastMin = -1;
}

void initTables() {
  for (int s = 0; s < SHADES; s++)
    for (int a = 0; a < 16; a++) {
      uint16_t c = tft.alphaBlend(a * 17, COLOURS[colour], CARD);
      c = tft.alphaBlend(255 - s * 160 / (SHADES - 1), c, BG); // up to ~63% darker
      pal[s][a] = (c >> 8) | (c << 8);
    }
  for (int y = 0; y < CH; y++) {
    int d = y < RADIUS ? RADIUS - y : (y >= CH - RADIUS ? y - (CH - RADIUS - 1) : 0);
    inset[y] = d ? RADIUS - (int)sqrtf(RADIUS * RADIUS - (d - 0.5f) * (d - 0.5f)) : 0;
  }
}

// one sprite row from digit d's source row src, at shade s
void row(int y, int d, int src, int s) {
  uint16_t *dst = buf + y * CW;
  if (src == HALF - 1 || src == HALF) { // hinge gap
    for (int x = 0; x < CW; x++) dst[x] = BG;
    return;
  }
  const uint8_t *g = DIGITS[d] + src * CW / 2;
  int in = inset[src];
  for (int x = 0; x < CW; x++) {
    int a = (x & 1) ? g[x >> 1] & 15 : g[x >> 1] >> 4;
    dst[x] = (x < in || x >= CW - in) ? BG : pal[s][a];
  }
}

void drawCard(int c, int d) {
  for (int y = 0; y < CH; y++) row(y, d, y, 0);
  spr.pushSprite(CARD_X[c], CARD_Y);
}

// phase 0: old top flap falls to the hinge revealing new top; phase 1: new bottom flap falls over old bottom
void flipFrame(int c, int od, int nd, int phase, int i) {
  for (int y = 0; y < CH; y++) {
    if (phase == 0) {
      int fh = HALF * (STEPS - i) / STEPS; // flap height above hinge
      if (y >= HALF) row(y, od, y, 0);
      else if (y >= HALF - fh) row(y, od, (y - (HALF - fh)) * HALF / fh, i * (SHADES - 1) / STEPS);
      else row(y, nd, y, 0);
    } else {
      int fh = HALF * i / STEPS; // flap height below hinge
      if (y < HALF) row(y, nd, y, 0);
      else if (y < HALF + fh) row(y, nd, HALF + (y - HALF) * HALF / fh, (STEPS - i) * (SHADES - 1) / STEPS);
      else row(y, od, y, 0);
    }
  }
  spr.pushSprite(CARD_X[c], CARD_Y);
}

void showTime(const int nd[4]) {
  if (lastMin < 0) { // full redraw
    tft.fillScreen(BG);
    for (int c = 0; c < 4; c++) drawCard(c, shown[c] = nd[c]);
    return;
  }
  for (int phase = 0; phase < 2; phase++)
    for (int i = 1; i <= STEPS; i++) {
      for (int c = 0; c < 4; c++)
        if (shown[c] != nd[c]) flipFrame(c, shown[c], nd[c], phase, i);
      delay(FRAME_MS);
    }
  for (int c = 0; c < 4; c++) shown[c] = nd[c];
}

// ---- settings screen ----
const uint16_t S_BG = 0x0841, S_ROW = 0x2104, S_LINE = 0x31A6, S_ACC = 0x2C5F, S_TXT = TFT_WHITE, S_SUB = 0x9CF3;
const int SL_X0 = 30, SL_X1 = 290, SL_Y = 181; // brightness slider track

bool touchAt(int &x, int &y) {
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();
  x = constrain(map(p.x, TS_X0, TS_X1, 0, 319), 0, 319);
  y = constrain(map(p.y, TS_Y0, TS_Y1, 0, 239), 0, 239);
  Serial.printf("touch raw %d,%d -> %d,%d\n", p.x, p.y, x, y); // for calibrating TS_*
  return true;
}

void drawSlider() {
  int kx = map(bright, MIN_BRIGHT, 255, SL_X0, SL_X1);
  tft.fillRect(18, SL_Y - 12, 284, 25, S_ROW);
  tft.fillRoundRect(SL_X0, SL_Y - 3, SL_X1 - SL_X0, 6, 3, S_LINE);
  tft.fillRoundRect(SL_X0, SL_Y - 3, kx - SL_X0 + 3, 6, 3, S_ACC);
  tft.fillSmoothCircle(kx, SL_Y, 10, S_TXT, S_ROW);
  char pct[8];
  snprintf(pct, sizeof pct, "%d%%", bright * 100 / 255);
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(S_SUB, S_ROW);
  tft.setTextPadding(70);
  tft.drawString(pct, 296, 149, 4);
  tft.setTextPadding(0);
}

void drawSettings() {
  tft.fillScreen(S_BG);
  tft.setTextColor(S_TXT, S_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Settings", 16, 23, 4);
  tft.fillSmoothRoundRect(240, 9, 68, 28, 14, S_ACC, S_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(S_TXT, S_ACC);
  tft.drawString("Done", 274, 23, 2);
  tft.drawFastHLine(0, 46, 320, S_LINE);

  tft.fillSmoothRoundRect(12, 56, 296, 60, 10, S_ROW, S_BG); // Wi-Fi row
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(S_TXT, S_ROW);
  tft.drawString("Wi-Fi", 24, 63, 4);
  tft.setTextColor(S_SUB, S_ROW);
  tft.drawString(WiFi.SSID().c_str(), 24, 93, 2);
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(S_ACC, S_ROW);
  tft.drawString("Change  >", 296, 86, 2);

  tft.fillSmoothRoundRect(12, 126, 296, 80, 10, S_ROW, S_BG); // brightness row
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(S_TXT, S_ROW);
  tft.drawString("Brightness", 24, 136, 4);
  drawSlider();

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(S_SUB, S_BG);
  tft.drawString(("flipclock.local  -  " + WiFi.localIP().toString()).c_str(), 160, 224, 2);
}

void settings() {
  drawSettings();
  for (uint32_t up = millis(); millis() - up < 150;) // wait for the long-press finger to lift
    if (ts.touched()) up = millis();
  int saved = bright;
  bool was = false;
  for (uint32_t idle = millis(); millis() - idle < SETTINGS_IDLE_MS; delay(15)) {
    ArduinoOTA.handle();
    int x, y;
    bool down = touchAt(x, y);
    if (down) {
      idle = millis();
      if (y >= 150 && y <= 215) { // slider: tap or drag
        int b = map(constrain(x, SL_X0, SL_X1), SL_X0, SL_X1, MIN_BRIGHT, 255);
        if (b != bright) { bright = b; analogWrite(TFT_BL, bright); drawSlider(); }
      } else if (!was && x >= 228 && y <= 46) break; // Done
      else if (!was && y >= 56 && y <= 116) {        // Change Wi-Fi: portal, old WiFi kept until replaced
        if (bright != saved) prefs.putUChar("bright", bright);
        wm.setConfigPortalTimeout(300);
        wm.startConfigPortal("FlipClock-Setup");
        ESP.restart();
      }
    }
    was = down;
  }
  if (bright != saved) prefs.putUChar("bright", bright);
  lastMin = -1; // back to the clock: full redraw, backlight re-evaluated
}

void checkTouch() {
  static uint32_t since = 0, seen = 0; // touch start, last time still touched
  uint32_t now = millis();
  if (!ts.touched()) {
    if (since && now - seen > 100) { // released (100ms debounce: pressure readings flicker)
      if (seen - since < 1000) {     // short tap: next colour, redraw everything
        colour = (colour + 1) % N_COLOURS;
        initTables();
        lastMin = -1;
      }
      since = 0;
    }
    return;
  }
  seen = now;
  if (!since) since = now;
  else if (now - since > HOLD_MS) {
    settings();
    since = 0;
  }
}

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);
  prefs.begin("clock");
  bright = prefs.getUChar("bright", 255);
  analogWrite(TFT_BL, bright);
  initTables();
  spr.setColorDepth(16);
  buf = (uint16_t *)spr.createSprite(CW, CH);

  touchSPI.begin(25, 39, 32, 33);
  ts.begin(touchSPI);
  ts.setRotation(1);

  msg("Connecting WiFi...");
  wm.setHostname("flipclock");
  wm.setConfigPortalTimeout(300); // no-one configured it: reboot and retry saved WiFi
  wm.setAPCallback([](WiFiManager *) { msg("Join WiFi network", "FlipClock-Setup", "open http://192.168.4.1 (closes in 5 min)"); });
  if (!wm.autoConnect("FlipClock-Setup")) ESP.restart();

  WiFi.setSleep(false); // modem sleep makes OTA uploads drop out
  configTzTime(TZ_UK, "pool.ntp.org", "time.google.com");

  ArduinoOTA.setHostname("flipclock");
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([] { analogWrite(TFT_BL, bright); msg("Updating..."); });
  ArduinoOTA.onProgress([](unsigned p, unsigned t) {
    char s[8];
    snprintf(s, sizeof s, "%u%%", p * 100 / t);
    tft.drawString(s, 160, 120, 4);
  });
  ArduinoOTA.onError([](ota_error_t) { msg("Update failed"); delay(2000); });
  ArduinoOTA.begin();

  msg("Syncing time...", WiFi.localIP().toString().c_str());
}

void loop() {
  ArduinoOTA.handle();
  checkTouch();
  struct tm t;
  if (!getLocalTime(&t, 0)) return;
  if (t.tm_min != lastMin) {
    bool night = NIGHT_START > NIGHT_END ? (t.tm_hour >= NIGHT_START || t.tm_hour < NIGHT_END)
                                         : (t.tm_hour >= NIGHT_START && t.tm_hour < NIGHT_END);
    analogWrite(TFT_BL, night ? min(NIGHT_LEVEL, bright) : bright);
    int nd[4] = {t.tm_hour / 10, t.tm_hour % 10, t.tm_min / 10, t.tm_min % 10};
    showTime(nd);
    lastMin = t.tm_min;
    lastSec = -1; // full redraw wipes the colon
  }
  if (t.tm_sec != lastSec) { // colon blinks: on for even seconds, off for odd
    lastSec = t.tm_sec;
    uint16_t c = (t.tm_sec & 1) ? BG : colour ? COLOURS[colour] : COLON;
    tft.fillRoundRect(155, 90, 10, 10, 2, c);
    tft.fillRoundRect(155, 140, 10, 10, 2, c);
  }
  delay(20);
}
