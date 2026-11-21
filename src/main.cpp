// CYD flip clock: 24h HH:MM, NTP time (UK), WiFiManager portal, ArduinoOTA.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>
#include "digits.h"

static_assert(sizeof(OTA_PASS) > 8, "export FLIPCLOCK_OTA_PASS (8+ chars) before building");

// ---- tuning knobs ----
const char *TZ_UK = "GMT0BST,M3.5.0/1,M10.5.0";
const int NIGHT_START = 23, NIGHT_END = 7;   // dim from 23:00 to 06:59
const int DAY_LEVEL = 255, NIGHT_LEVEL = 20; // backlight PWM 0-255
const int STEPS = 8, FRAME_MS = 12;          // flip animation per half
const uint32_t RESET_HOLD_MS = 5000;         // long-press to forget WiFi

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
  else if (now - since > RESET_HOLD_MS) {
    msg("Resetting WiFi", "", "release the screen");
    wm.resetSettings();
    delay(1000);
    ESP.restart();
  }
}

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);
  analogWrite(TFT_BL, DAY_LEVEL);
  initTables();
  spr.setColorDepth(16);
  buf = (uint16_t *)spr.createSprite(CW, CH);

  touchSPI.begin(25, 39, 32, 33);
  ts.begin(touchSPI);

  msg("Connecting WiFi...");
  wm.setHostname("flipclock");
  wm.setConfigPortalTimeout(300); // no-one configured it: reboot and retry saved WiFi
  wm.setAPCallback([](WiFiManager *) { msg("Join WiFi network", "FlipClock-Setup", "then open http://192.168.4.1"); });
  if (!wm.autoConnect("FlipClock-Setup")) ESP.restart();

  configTzTime(TZ_UK, "pool.ntp.org", "time.google.com");

  ArduinoOTA.setHostname("flipclock");
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([] { analogWrite(TFT_BL, DAY_LEVEL); msg("Updating..."); });
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
    analogWrite(TFT_BL, night ? NIGHT_LEVEL : DAY_LEVEL);
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
