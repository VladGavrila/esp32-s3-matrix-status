# esp32-s3-matrix-status

Flash a Waveshare ESP32-S3-Matrix (8x8 onboard WS2812 RGB LED matrix) so it
joins your WiFi *and* advertises over BLE at the same time, and exposes an
API over both to set the whole matrix to a solid color or a pre-programmed
mode. The WiFi/HTTP API is controllable via `curl`; the BLE API via the
included [`ble-client`](ble-client/main.go) Go program.

## Arduino IDE setup

1. **Add the ESP32 board package.**
   File → Preferences → "Additional boards manager URLs":
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   Then Tools → Board → Boards Manager → install **esp32 by Espressif Systems**.

2. **Select the board.** Tools → Board → esp32 → **ESP32S3 Dev Module**.
   Recommended settings:
   - USB CDC On Boot: **Enabled** (so Serial output shows up over the USB-C port)
   - Flash Size: **4MB**
   - Partition Scheme: **Huge APP (3MB No OTA/1MB SPIFFS)** — WiFi + BLE
     together produce a much larger binary than WiFi alone; the default
     4MB-with-spiffs scheme's ~1.3MB app partition can be too tight for it.
     We don't need OTA updates here, so this trade is fine.

3. **Install the library.** Sketch → Include Library → Manage Libraries →
   install **Adafruit NeoPixel**. (The BLE libraries used — `BLEDevice`,
   `BLEServer`, etc. — ship bundled with the esp32 board package, so no
   separate install is needed for those.)

4. **Configure secrets.** Copy [`secrets.h.example`](secrets.h.example) to
   `secrets.h` in this same folder and fill in your WiFi SSID/password and a
   random API key:
   ```bash
   cp secrets.h.example secrets.h
   ```
   `secrets.h` is gitignored and never committed.

5. **Open and flash.** Open `esp32-s3-matrix-status.ino` in Arduino IDE,
   select the board's serial port, and click Upload.

6. **Find the device.** The board advertises itself via mDNS as
   `VGS3A.local`, so you don't need to look up its IP — `http://VGS3A.local`
   works directly on the same network. (The Serial Monitor at 115200 baud
   also prints the IP address as a fallback, e.g. if your network/OS doesn't
   support mDNS — Windows may need [Bonjour](https://support.apple.com/kb/DL999) installed.)

## API

All `/color`, `/brightness`, and `/mode` requests require an `X-API-Key`
header matching `API_KEY` from your `secrets.h`.

**Set the matrix to a solid color:**
```bash
curl -X POST "http://VGS3A.local/color?r=255&g=0&b=0" \
  -H "X-API-Key: <your-api-key>"
```

`r`, `g`, `b` are 0-255. Optionally include `brightness` (0-255, clamped to
`LED_MAX_BRIGHTNESS`) to set the color and brightness together:
```bash
curl -X POST "http://VGS3A.local/color?r=255&g=0&b=0&brightness=15" \
  -H "X-API-Key: <your-api-key>"
```

Response:
```json
{"status":"ok","r":255,"g":0,"b":0,"brightness":40}
```

**Change brightness only, keeping the current color:**
```bash
curl -X POST "http://VGS3A.local/brightness?level=10" \
  -H "X-API-Key: <your-api-key>"
```

`level` is 0-255, clamped to `LED_MAX_BRIGHTNESS`. Response:
```json
{"status":"ok","brightness":10}
```

**Set a pre-programmed mode:**
```bash
curl -X POST "http://VGS3A.local/mode?name=busy" \
  -H "X-API-Key: <your-api-key>"
```

`name` is one of:
| Mode      | Icon         | Color  | Peak brightness | Behavior |
|-----------|--------------|--------|------------------|----------|
| `busy`    | X shape      | blue   | 2%               | pulses in and out over 3s |
| `dnd`     | camera icon  | red    | 2%               | pulses in and out over 3s |
| `free`    | maze         | green  | 2%               | solid, no pulse |
| `dndmic`  | mic          | yellow | 2%               | pulses in and out over 3s |
| `test`    | letter "F"   | cyan   | 2%               | solid — verifies matrix wiring/orientation |
| `off`     | —            | —      | 0%               | all LEDs off |

Every mode's peak brightness is 2% (except `off`, which is 0%), and every
pulsing mode (`busy`/`dnd`/`dndmic`) shares the same 3s cycle, breathing
between 25% and 100% of that peak rather than fading fully off. Each mode
draws its own icon shape instead of filling the whole matrix —
see `ICON_BUSY`, `ICON_DND`, `ICON_FREE`, `ICON_DNDMIC` in the `.ino` if
you want to tweak one. The boot indicator (green/orange at startup) also
draws a WiFi or Bluetooth icon depending on whether WiFi connected, and
shows for 60 seconds before automatically switching to `free` — unless a
`/color`, `/brightness`, or `/mode` command arrives first, which cancels
the auto-switch and takes over immediately.

Response:
```json
{"status":"ok","mode":"busy"}
```

A `/color` or `/brightness` request cancels any active mode and takes
direct manual control instead.

**Health check (no auth required):**
```bash
curl "http://VGS3A.local/"
```

## Client program (BLE and WiFi)

`ble-client` is a small Go program in [`ble-client/`](ble-client) that
talks to the board over **BLE by default**, or over WiFi/HTTP with `-wifi`.
Build it once (needs [Go](https://go.dev) installed; on macOS it also needs
Xcode's command line tools, since the BLE path links against CoreBluetooth):
```bash
cd ble-client
go build -o ble-client .
```

Then, with `MATRIX_API_KEY` set to your `secrets.h` API key (or passed via
`-api-key` each time):
```bash
export MATRIX_API_KEY=<your-api-key>

# BLE (default) — no OS-level Bluetooth pairing needed, it's an open
# connection secured by the API key in each command payload
./ble-client color 255 0 0
./ble-client color 255 0 0 -brightness 15
./ble-client brightness 10
./ble-client mode busy

# WiFi/HTTP instead — same subcommands, add -wifi
./ble-client mode busy -wifi
./ble-client color 255 0 0 -wifi -host VGS3A.local   # -host defaults to VGS3A.local

# -timeout bounds how long it waits for the device (default 3s)
./ble-client mode busy -timeout 5s
```

Flags can go anywhere on the command line — before or after the
positional arguments.

Each BLE command scans for the device by name, connects, sends the
command, prints the resulting status, then disconnects — no need to keep a
connection open between commands. `-wifi` mode is a thinner wrapper: it
just issues the same HTTP requests documented above with `curl`.

## Notes

- Brightness is capped in firmware at `LED_MAX_BRIGHTNESS` (currently 40 of
  255, ~16%) — Waveshare warns the board can overheat at full brightness on
  all 64 LEDs. Any `brightness`/`level` value you send via `/color` or
  `/brightness` is clamped to this ceiling; raise it in the `.ino` only if
  you know your power supply can handle it. The `/mode` presets use their
  own fixed 2% peak brightness (as specified in the table above), since
  they're fixed firmware constants rather than arbitrary user input.
- WiFi gets up to 5 seconds to connect at boot (`WIFI_CONNECT_TIMEOUT_MS`).
  If it fails within that window, the board continues in BLE-only mode —
  the HTTP server and mDNS never start, but BLE still works normally.
- The boot indicator (WiFi/Bluetooth icon) renders at 1% brightness
  (`BOOT_INDICATOR_BRIGHTNESS`), dimmer than any `/mode` preset.
- The device gets its IP from your router's DHCP; if you want a stable
  address, reserve one for its MAC in your router settings.
