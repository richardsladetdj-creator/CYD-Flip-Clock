// CYD flip clock: Open-Meteo weather header + HH:MM:SS flip cards, NTP (UK), WiFiManager portal, ArduinoOTA.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
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
const int NIGHT_LEVEL = 20, MIN_BRIGHT = 10; // backlight PWM 0-255; daytime level is set in Settings
const int STEPS = 5, FRAME_MS = 10;          // flip animation per half (must stay well under 1s)
const uint32_t HOLD_MS = 1500;               // long-press opens Settings
const uint32_t SETTINGS_IDLE_MS = 30000;     // Settings closes itself after this
const uint32_t WEATHER_MS = 15 * 60 * 1000, WEATHER_RETRY_MS = 60 * 1000;
const int TS_X0 = 200, TS_X1 = 3700, TS_Y0 = 240, TS_Y1 = 3800; // touch calibration (raw -> screen)

// ---- colours (from the design) ----
constexpr uint16_t rgb(uint32_t c) { return ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c & 0xFF) >> 3); }
const uint16_t BG = rgb(0x0B1019), HDR = rgb(0x182534), HDR_LINE = rgb(0x304152);
const uint16_t BORDER = rgb(0x27303B), TOP = rgb(0x343F4A), BOT = rgb(0x242D37), SHADOW = rgb(0x03060B);
const uint16_t HINGE = rgb(0x080C12), PIVOT = rgb(0x111820), COLON = rgb(0xDDE4E9), FG = rgb(0xF3F4F3);
const uint16_t TEMP_C = rgb(0xFAFAFA), OUTLOOK = rgb(0xB9C8D7), LABEL = rgb(0x859AAC);
const uint16_t SUN = rgb(0xF6C45C), CLOUD = rgb(0xD4E0E9), RAIN = rgb(0x6FB7FF), MOON = rgb(0xE8E6D0);
// tap cycles digit colour: default, blue, green, red, yellow
const uint16_t COLOURS[] = {FG, 0x353F, 0x07E0, 0xF8A2, 0xFFE0};
const int N_COLOURS = sizeof COLOURS / sizeof *COLOURS;
int colour = 0;

// ---- layout (320x240 landscape): header 0-80, clock below ----
const int CW = DIG_W, CH = DIG_H, HALF = CH / 2, CARD_Y = 121, RADIUS = 4;
const int CARD_X[6] = {13, 54, 117, 158, 221, 262};
const int SHADES = 6; // darkness levels for the moving flap

TFT_eSPI tft;
TFT_eSprite spr(&tft);
uint16_t *buf;
uint16_t pal[2][SHADES][16]; // [top/bottom half][shade][alpha] -> byte-swapped RGB565 (sprite buffer order)
uint8_t inset[CH];           // rounded-corner inset per row

SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(33, 36);
WiFiManager wm;
WiFiManagerParameter townParam("town", "Town for weather (e.g. Manchester)", "London", 40);
Preferences prefs;
int bright = 255; // daytime backlight, persisted

int shown[6];
bool redraw = true; // full-screen redraw on next loop
int lastSec = -1, lastMin = -1;

// weather: written by weatherTask (core 0), drawn by loop()
// ponytail: plain volatile flags, no mutex; worst case one header frame shows a half-updated value
volatile bool wDirty = false, wValid = false;
volatile float wTemp;
volatile int wCode, wDay = 1;
char place[41] = "";
float lat = NAN, lon = NAN;

void msg(const char *a, const char *b = "", const char *c = "") {
  tft.fillScreen(BG);
  tft.setTextColor(FG, BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(a, 160, 85, 4);
  tft.drawString(b, 160, 120, 4);
  tft.drawString(c, 160, 155, 2);
  redraw = true;
}

// ================= flip cards =================

void initTables() {
  const uint16_t half[2] = {TOP, BOT};
  for (int h = 0; h < 2; h++)
    for (int s = 0; s < SHADES; s++)
      for (int a = 0; a < 16; a++) {
        uint16_t c = tft.alphaBlend(a * 17, COLOURS[colour], half[h]);
        c = tft.alphaBlend(255 - s * 150 / (SHADES - 1), c, HINGE); // up to ~60% darker
        pal[h][s][a] = (c >> 8) | (c << 8);
      }
  for (int y = 0; y < CH; y++) {
    int d = y < RADIUS ? RADIUS - y : (y >= CH - RADIUS ? y - (CH - RADIUS - 1) : 0);
    inset[y] = d ? RADIUS - (int)sqrtf(RADIUS * RADIUS - (d - 0.5f) * (d - 0.5f)) : 0;
  }
}

inline uint16_t sw(uint16_t c) { return (c >> 8) | (c << 8); }

// one sprite row from digit d's source row src, at shade s
void row(int y, int d, int src, int s) {
  uint16_t *dst = buf + y * CW;
  int in = inset[src];
  bool edge = src == 0 || src == CH - 1, hinge = src == HALF - 1 || src == HALF;
  const uint8_t *g = DIGITS[d] + src * CW / 2;
  const uint16_t *p = pal[src >= HALF][s];
  for (int x = 0; x < CW; x++) {
    if (x < in || x >= CW - in) dst[x] = sw(BG);
    else if (edge || x == in || x == CW - 1 - in) dst[x] = sw(BORDER);
    else if (hinge) dst[x] = sw(HINGE);
    else dst[x] = p[(x & 1) ? g[x >> 1] & 15 : g[x >> 1] >> 4];
  }
}

void pushCard(int c) {
  spr.fillCircle(1, HALF, 2, PIVOT);
  spr.fillCircle(CW - 2, HALF, 2, PIVOT);
  spr.pushSprite(CARD_X[c], CARD_Y);
}

void drawCard(int c, int d) {
  tft.fillRoundRect(CARD_X[c] + 1, CARD_Y + 4, CW, CH, RADIUS, SHADOW);
  for (int y = 0; y < CH; y++) row(y, d, y, 0);
  pushCard(c);
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
      else if (y < HALF + fh) row(y, nd, HALF + (y - HALF) * (CH - HALF) / fh, (STEPS - i) * (SHADES - 1) / STEPS);
      else row(y, od, y, 0);
    }
  }
  pushCard(c);
}

void flipTo(const int nd[6]) {
  for (int phase = 0; phase < 2; phase++)
    for (int i = 1; i <= STEPS; i++) {
      for (int c = 0; c < 6; c++)
        if (shown[c] != nd[c]) flipFrame(c, shown[c], nd[c], phase, i);
      delay(FRAME_MS);
    }
  for (int c = 0; c < 6; c++) shown[c] = nd[c];
}

// ================= weather =================

String httpGet(const String &url) {
  HTTPClient h;
  h.setTimeout(8000);
  h.begin(url);
  String body = h.GET() == 200 ? h.getString() : "";
  h.end();
  return body;
}

float numAfter(const String &s, const char *key, int from) {
  int i = s.indexOf(key, from);
  return i < 0 ? NAN : atof(s.c_str() + i + strlen(key));
}

String urlEncode(const String &s) {
  String o;
  for (char c : s) {
    if (isalnum((unsigned char)c) || c == '-' || c == '.') o += c;
    else { char b[4]; snprintf(b, sizeof b, "%%%02X", (uint8_t)c); o += b; }
  }
  return o;
}

void setPlace(const String &p) {
  String u = p;
  u.toUpperCase();
  strlcpy(place, u.c_str(), sizeof place);
  wDirty = true;
}

void weatherTask(void *) {
  String town = prefs.getString("town", "London");
  while (isnan(lat) || town != prefs.getString("geoTown", "")) { // geocode once per town change
    String r = httpGet("http://geocoding-api.open-meteo.com/v1/search?count=1&name=" + urlEncode(town));
    if (r.length()) {
      int i = r.indexOf("\"results\":[");
      if (i < 0) { setPlace("UNKNOWN TOWN"); vTaskDelete(NULL); }
      int n = r.indexOf("\"name\":\"", i) + 8;
      String name = r.substring(n, r.indexOf('"', n));
      lat = numAfter(r, "\"latitude\":", i);
      lon = numAfter(r, "\"longitude\":", i);
      prefs.putFloat("lat", lat);
      prefs.putFloat("lon", lon);
      prefs.putString("place", name);
      prefs.putString("geoTown", town);
      setPlace(name);
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(WEATHER_RETRY_MS));
  }
  for (;;) {
    String r = httpGet("http://api.open-meteo.com/v1/forecast?current=temperature_2m,weather_code,is_day&latitude=" +
                       String(lat, 4) + "&longitude=" + String(lon, 4));
    int c = r.indexOf("\"current\":{");
    float t = c < 0 ? NAN : numAfter(r, "\"temperature_2m\":", c);
    if (!isnan(t)) {
      wTemp = t;
      wCode = (int)numAfter(r, "\"weather_code\":", c);
      wDay = (int)numAfter(r, "\"is_day\":", c);
      wValid = true;
      wDirty = true;
      Serial.printf("weather: %.1fC code %d day %d\n", t, wCode, wDay);
    } else Serial.println("weather: fetch failed");
    vTaskDelay(pdMS_TO_TICKS(isnan(t) ? WEATHER_RETRY_MS : WEATHER_MS));
  }
}

const char *outlook(int c) {
  if (c == 0) return "CLEAR";
  if (c == 1) return "MAINLY CLEAR";
  if (c == 2) return "PARTLY CLOUDY";
  if (c == 3) return "OVERCAST";
  if (c <= 48) return "FOG";
  if (c <= 57) return "DRIZZLE";
  if (c <= 67) return "RAIN";
  if (c <= 77) return "SNOW";
  if (c <= 82) return "SHOWERS";
  if (c <= 86) return "SNOW SHOWERS";
  return "THUNDERSTORM";
}

void sun(int cx, int cy, int r) {
  tft.fillSmoothCircle(cx, cy, r, SUN, HDR);
  for (int k = 0; k < 8; k++) {
    float a = k * PI / 4, ca = cosf(a), sa = sinf(a);
    tft.drawWideLine(cx + ca * (r + 5), cy + sa * (r + 5), cx + ca * (r + 10), cy + sa * (r + 10), 2.3, SUN, HDR);
  }
}

void cloud(int ox, int oy) { // ~50x24 cloud with its top-left at ox,oy
  tft.fillSmoothCircle(ox + 12, oy + 15, 9, CLOUD, HDR);
  tft.fillSmoothCircle(ox + 25, oy + 11, 12, CLOUD, HDR);
  tft.fillSmoothCircle(ox + 38, oy + 16, 9, CLOUD, HDR);
  tft.fillSmoothRoundRect(ox + 3, oy + 14, 45, 11, 5, CLOUD, HDR);
}

void icon(int c, bool day) {
  if (c <= 1) {
    if (day) sun(37, 30, c ? 12 : 14);
    else { tft.fillSmoothCircle(37, 30, 15, MOON, HDR); tft.fillSmoothCircle(45, 24, 13, HDR, HDR); }
    return;
  }
  if (c == 2) {
    if (day) sun(37, 26, 12);
    else { tft.fillSmoothCircle(32, 24, 12, MOON, HDR); tft.fillSmoothCircle(39, 19, 10, HDR, HDR); }
    cloud(16, 32);
    return;
  }
  if (c == 45 || c == 48) {
    for (int k = 0; k < 4; k++) tft.drawWideLine(14 + (k & 1) * 6, 16 + k * 12, 56 + (k & 1) * 6, 16 + k * 12, 3, CLOUD, HDR);
    return;
  }
  cloud(12, 10);
  if (c == 3) return;
  bool snow = (c >= 71 && c <= 77) || c == 85 || c == 86;
  for (int k = 0; k < 3; k++) {
    int x = 24 + k * 12;
    if (snow) tft.fillSmoothCircle(x, 50 + (k & 1) * 6, 2, CLOUD, HDR);
    else tft.drawWideLine(x, 46 + (k & 1) * 4, x - 3, 54 + (k & 1) * 4, 2.5, RAIN, HDR);
  }
  if (c >= 95) {
    tft.fillTriangle(40, 38, 30, 54, 38, 54, SUN);
    tft.fillTriangle(38, 50, 46, 50, 33, 66, SUN);
  }
}

void drawHeader() {
  tft.fillRect(0, 0, 320, 80, HDR);
  tft.drawFastHLine(0, 80, 320, HDR_LINE);

  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(LABEL, HDR);
  tft.drawString(place, 305, 12, 1);

  if (wValid) icon(wCode, wDay);
  tft.setFreeFont(&FreeSansBold18pt7b);
  tft.setTextDatum(L_BASELINE);
  tft.setTextColor(TEMP_C, HDR);
  char t[8];
  if (wValid) snprintf(t, sizeof t, "%ld", lroundf(wTemp));
  else strcpy(t, "--");
  int w = tft.drawString(t, 84, 43);
  tft.setFreeFont(nullptr);
  tft.drawCircle(84 + w + 7, 20, 4, TEMP_C); // degree sign: FreeSans has no glyph for it
  tft.drawCircle(84 + w + 7, 20, 3, TEMP_C);
  tft.setFreeFont(&FreeSansBold18pt7b);
  tft.drawString("C", 84 + w + 14, 43);
  tft.setFreeFont(nullptr);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(OUTLOOK, HDR);
  tft.drawString(wValid ? outlook(wCode) : "LOADING", 85, 52, 2);
}

// ================= settings screen =================
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
  tft.drawString("Wi-Fi & location", 24, 63, 4);
  tft.setTextColor(S_SUB, S_ROW);
  tft.drawString((WiFi.SSID() + "  -  " + prefs.getString("town", "London")).c_str(), 24, 93, 2);
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(S_ACC, S_ROW);
  tft.drawString(">", 296, 86, 4);

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
      else if (!was && y >= 56 && y <= 116) {        // Wi-Fi & location: portal, old WiFi kept until replaced
        if (bright != saved) prefs.putUChar("bright", bright);
        wm.setConfigPortalTimeout(300);
        wm.startConfigPortal("FlipClock-Setup");
        ESP.restart();
      }
    }
    was = down;
  }
  if (bright != saved) prefs.putUChar("bright", bright);
  redraw = true;
}

void checkTouch() {
  static uint32_t since = 0, seen = 0; // touch start, last time still touched
  uint32_t now = millis();
  if (!ts.touched()) {
    if (since && now - seen > 100) { // released (100ms debounce: pressure readings flicker)
      if (seen - since < 1000) {     // short tap: next colour, redraw everything
        colour = (colour + 1) % N_COLOURS;
        initTables();
        redraw = true;
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

// ================= main =================

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);
  prefs.begin("clock");
  bright = prefs.getUChar("bright", 255);
  lat = prefs.getFloat("lat", NAN);
  lon = prefs.getFloat("lon", NAN);
  setPlace(prefs.getString("place", ""));
  analogWrite(TFT_BL, bright);
  initTables();
  spr.setColorDepth(16);
  buf = (uint16_t *)spr.createSprite(CW, CH);

  touchSPI.begin(25, 39, 32, 33);
  ts.begin(touchSPI);
  ts.setRotation(1);

  msg("Connecting WiFi...");
  String town = prefs.getString("town", "London");
  townParam.setValue(town.c_str(), 40);
  wm.addParameter(&townParam);
  wm.setSaveParamsCallback([] { prefs.putString("town", townParam.getValue()); });
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

  xTaskCreatePinnedToCore(weatherTask, "weather", 8192, nullptr, 1, nullptr, 0);
  msg("Syncing time...", WiFi.localIP().toString().c_str());
}

void loop() {
  ArduinoOTA.handle();
  checkTouch();
  struct tm t;
  if (!getLocalTime(&t, 0)) return;
  int nd[6] = {t.tm_hour / 10, t.tm_hour % 10, t.tm_min / 10, t.tm_min % 10, t.tm_sec / 10, t.tm_sec % 10};
  if (redraw) {
    redraw = false;
    tft.fillScreen(BG);
    drawHeader();
    wDirty = false;
    for (int x : {104, 208})
      for (int y : {151, 182}) tft.fillSmoothCircle(x, y, 3, COLON, BG);
    for (int c = 0; c < 6; c++) drawCard(c, shown[c] = nd[c]);
    lastSec = t.tm_sec;
    lastMin = -1;
  }
  if (wDirty) { wDirty = false; drawHeader(); }
  if (t.tm_sec != lastSec) {
    lastSec = t.tm_sec;
    flipTo(nd);
  }
  if (t.tm_min != lastMin) {
    lastMin = t.tm_min;
    bool night = NIGHT_START > NIGHT_END ? (t.tm_hour >= NIGHT_START || t.tm_hour < NIGHT_END)
                                         : (t.tm_hour >= NIGHT_START && t.tm_hour < NIGHT_END);
    analogWrite(TFT_BL, night ? min(NIGHT_LEVEL, bright) : bright);
  }
  delay(5);
}
