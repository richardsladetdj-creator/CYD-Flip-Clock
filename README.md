# CYD Flip Clock

A 24-hour HH:MM:SS flip clock with a weather bar, for the ESP32 "Cheap Yellow Display" (ESP32-2432S028, 2.8").

- Seconds flip every second; minutes and hours flip only when they change.
- The weather bar shows current temperature, conditions and an icon from [Open-Meteo](https://open-meteo.com) (free, no API key), refreshed every 15 minutes.
- Gets the time over NTP. UK time is the default, with automatic GMT/BST changes.
- WiFi is set up from your phone through a captive portal, so no credentials are kept in the repo.
- Firmware updates can be sent over WiFi (ArduinoOTA, password protected).
- Tapping the screen cycles the digit colour: default, blue, green, red, yellow, then back to default.
- Holding the screen for 1.5 s opens Settings, where you can change Wi-Fi and the weather town (opens the setup portal) and adjust brightness (saved across reboots).
- The backlight dims at night (23:00 to 07:00).

Inspired by [ThomDyson/CYDFlipClock](https://github.com/ThomDyson/CYDFlipClock).

## Hardware

- An ESP32-2432S028 "CYD" board, 2.8" 320×240. The default build is for the two-USB (USB-C + micro-USB) version with the ST7789 display. See the tuning knobs below for the micro-USB-only version.
- A USB cable for the first flash. Later updates can go over WiFi.

## Build & flash

Needs [PlatformIO](https://platformio.org) (`uv tool install platformio` or `pip install platformio`).

```sh
export FLIPCLOCK_OTA_PASS='pick-a-long-password'   # required: compiled in, and used for uploads
pio run -e usb -t upload                            # first time, over USB
pio run -e ota -t upload                            # after that, over WiFi (flipclock.local)
```

## WiFi setup

1. On first boot the screen shows **Join WiFi network FlipClock-Setup**.
2. Join that network from your phone. If the portal page doesn't open on its own, go to http://192.168.4.1.
3. Pick your network, enter the password and your **town for weather** (e.g. `Manchester`), then save. The clock reboots onto your WiFi.

If nobody configures it within 5 minutes, it reboots and tries the saved WiFi again. To change networks, hold the screen for 1.5 s and tap **Wi-Fi & location**. The same page lets you change the weather town (default: Bournemouth). Your current WiFi is kept unless you save a new one.

## Tuning knobs

| What | Where |
|---|---|
| Timezone (POSIX TZ string) | `TZ_UK` in `src/main.cpp` |
| Night dimming hours and level | `NIGHT_START`, `NIGHT_END`, `NIGHT_LEVEL` |
| Touch calibration (taps land in the wrong place) | `TS_X0`, `TS_X1`, `TS_Y0`, `TS_Y1` (raw values are printed on serial) |
| Flip speed | `STEPS`, `FRAME_MS` |
| Weather refresh | `WEATHER_MS` |
| Digit size | `MAX_W`, `MAX_H`, `SY` in `tools/gen_digits.py`, then regenerate |
| Colours | `COLOURS` (the tap cycle), `TOP`, `BOT`, `HDR`, ... |
| Inverted or negative display | change `TFT_INVERSION_OFF` to `TFT_INVERSION_ON` in `platformio.ini` |
| Micro-USB-only board (ILI9341) | change `ST7789_DRIVER` to `ILI9341_2_DRIVER` in `platformio.ini` |

## Digit font

`include/digits.h` is generated from [Bebas Neue](https://github.com/dharmatype/Bebas-Neue) (SIL Open Font License, see `fonts/BebasNeue-OFL.txt`):

```sh
python tools/gen_digits.py BebasNeue-Regular.ttf include/digits.h   # needs Pillow
```

## Licence

[MIT](LICENSE). The Bebas Neue font is under the [SIL Open Font License](fonts/BebasNeue-OFL.txt).
