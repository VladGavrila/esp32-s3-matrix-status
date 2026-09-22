// ESP32-S3-Matrix WiFi + BLE LED color server.
// Connects to WiFi and exposes an HTTP API, and simultaneously advertises
// a BLE GATT service, to set the 8x8 WS2812 matrix to a solid color or a
// pre-programmed pulsing mode.
//
// Setup: copy secrets.h.example to secrets.h and fill in your WiFi
// credentials and API key before compiling. See README.md for full
// Arduino IDE setup instructions.

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Adafruit_NeoPixel.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "secrets.h"

#define LED_PIN 14
#define LED_COUNT 64
#define LED_MAX_BRIGHTNESS 40  // keep low: board overheats at high brightness
#define BOOT_INDICATOR_BRIGHTNESS 3  // 1% of 255
#define BOOT_INDICATOR_DURATION_MS 60000  // auto-transitions to "free" after this, unless overridden first
#define WIFI_CONNECT_TIMEOUT_MS 5000
#define HOSTNAME "VGS3A"
#define ANIM_INTERVAL_MS 30  // pulse animation frame rate
#define PULSE_MIN_FRACTION 0.25f  // pulse dims to this fraction of peak, never fully off

// Pre-programmed /mode presets. Peak brightness here is the mode's own
// spec, independent of the LED_MAX_BRIGHTNESS safety cap applied to
// user-supplied /color and /brightness values.
#define MODE_BUSY_R 0
#define MODE_BUSY_G 0
#define MODE_BUSY_B 255
#define MODE_BUSY_PEAK_BRIGHTNESS 5  // 2% of 255
#define MODE_BUSY_PERIOD_MS 3000

#define MODE_DND_R 255
#define MODE_DND_G 0
#define MODE_DND_B 0
#define MODE_DND_PEAK_BRIGHTNESS 5  // 2% of 255
#define MODE_DND_PERIOD_MS 3000

#define MODE_FREE_R 0
#define MODE_FREE_G 255
#define MODE_FREE_B 0
#define MODE_FREE_BRIGHTNESS 5  // 2% of 255

#define MODE_DNDMIC_R 255
#define MODE_DNDMIC_G 255
#define MODE_DNDMIC_B 0
#define MODE_DNDMIC_PEAK_BRIGHTNESS 5  // 2% of 255
#define MODE_DNDMIC_PERIOD_MS 3000

// Matrix wiring: many 8x8 WS2812 panels are wired serpentine (row 0
// left-to-right, row 1 right-to-left, ...) rather than plain raster order.
// Confirmed false (plain raster, every row left-to-right) on this board via
// the "test" mode's icon.
#define MATRIX_SERPENTINE false

// BLE: open connection, no OS-level pairing. The API key is sent as part
// of each command payload instead, same credential as the HTTP API.
#define BLE_SERVICE_UUID "8f2b1000-2c4a-4bfa-93be-0a8b3e2a9a01"
#define BLE_COMMAND_CHAR_UUID "8f2b1001-2c4a-4bfa-93be-0a8b3e2a9a01"
#define BLE_STATUS_CHAR_UUID "8f2b1002-2c4a-4bfa-93be-0a8b3e2a9a01"

// An icon is 8 bytes, one per row, MSB-first within each row: bit 7 is
// column 0 (left), bit 0 is column 7 (right). 1 = lit, 0 = off.
typedef uint8_t Icon[8];

const Icon ICON_FULL = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Asymmetric test icon (a letter "F", rotated 180°) for verifying the
// matrix's physical wiring order/orientation — see the "test" /mode entry
// below.
const Icon ICON_TEST_F = {
    0b00000001,
    0b00000001,
    0b00000001,
    0b00000001,
    0b00111111,
    0b00000001,
    0b00000001,
    0b01111111,
};

// Boot indicator: shown once at startup depending on WiFi connect result.
// All icons in this file are rotated 180° from how they were originally
// drawn.
const Icon ICON_WIFI = {
    0b00011000,
    0b00000000,
    0b00100100,
    0b00011000,
    0b01000010,
    0b00111100,
    0b10000001,
    0b01111110,
};
const Icon ICON_BLUETOOTH = {
    0b00011010,
    0b00101100,
    0b01001000,
    0b00111000,
    0b00111000,
    0b01001000,
    0b00101100,
    0b00011010,
};

// /mode icons.
const Icon ICON_FREE = {
    0b11111111,
    0b10000001,
    0b10111101,
    0b10110101,
    0b10000101,
    0b11111101,
    0b00000001,
    0b11111111,
};
const Icon ICON_BUSY = {
    0b10000001,
    0b01000010,
    0b00100100,
    0b00011000,
    0b00011000,
    0b00100100,
    0b01000010,
    0b10000001,
};
const Icon ICON_DND = {
    0b00000000,
    0b11011111,
    0b10100001,
    0b10000001,
    0b10000001,
    0b10100001,
    0b11011111,
    0b00000000,
};
const Icon ICON_DNDMIC = {
    0b00111100,
    0b00011000,
    0b00111100,
    0b01011010,
    0b00100100,
    0b00100100,
    0b00100100,
    0b00011000,
};

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_RGB + NEO_KHZ800);
WebServer server(80);
BLECharacteristic* bleStatusChar = nullptr;
bool wifiConnected = false;

uint8_t currentR = 0, currentG = 0, currentB = 0;
uint8_t currentBrightness = LED_MAX_BRIGHTNESS;
const uint8_t* currentIcon = ICON_FULL;

bool pulsing = false;
uint8_t pulsePeakBrightness = 0;
unsigned long pulsePeriodMs = 0;
unsigned long pulseStartMillis = 0;
unsigned long lastAnimUpdate = 0;

// True while the boot (WiFi/Bluetooth connection) indicator is showing and
// due to auto-transition to "free" — cancelled early if a command arrives.
bool bootIndicatorPending = false;
unsigned long bootIndicatorDeadline = 0;

void cancelBootIndicator() {
  bootIndicatorPending = false;
}

// Maps an (row, col) icon coordinate to a physical pixel index in the
// 64-LED chain, accounting for serpentine wiring if MATRIX_SERPENTINE.
int pixelIndexFor(int row, int col) {
  int effectiveCol = col;
  if (MATRIX_SERPENTINE && (row % 2 == 1)) {
    effectiveCol = 7 - col;
  }
  return row * 8 + effectiveCol;
}

// Re-renders the matrix from currentIcon in currentR/G/B at currentBrightness.
void render() {
  strip.setBrightness(currentBrightness);
  uint32_t litColor = strip.Color(currentR, currentG, currentB);
  uint32_t offColor = strip.Color(0, 0, 0);
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      bool lit = (currentIcon[row] >> (7 - col)) & 0x01;
      strip.setPixelColor(pixelIndexFor(row, col), lit ? litColor : offColor);
    }
  }
  strip.show();
}

void setColor(uint8_t r, uint8_t g, uint8_t b) {
  currentR = r;
  currentG = g;
  currentB = b;
  render();
}

void setBrightnessLevel(uint8_t brightness) {
  currentBrightness = brightness;
  render();
}

void setStaticMode(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness, const uint8_t* icon = ICON_FULL) {
  pulsing = false;
  currentIcon = icon;
  currentBrightness = brightness;
  setColor(r, g, b);
}

void setPulseMode(uint8_t r, uint8_t g, uint8_t b, uint8_t peakBrightness, unsigned long periodMs,
                   const uint8_t* icon = ICON_FULL) {
  pulsing = true;
  currentIcon = icon;
  pulsePeakBrightness = peakBrightness;
  pulsePeriodMs = periodMs;
  pulseStartMillis = millis();
  currentR = r;
  currentG = g;
  currentB = b;
}

// Advances the pulse animation; call frequently from loop().
void updatePulse() {
  unsigned long now = millis();
  if (now - lastAnimUpdate < ANIM_INTERVAL_MS) {
    return;
  }
  lastAnimUpdate = now;

  float phase = fmod((float)(now - pulseStartMillis), (float)pulsePeriodMs) / (float)pulsePeriodMs;
  float factor = (1.0f - cos(2.0f * PI * phase)) / 2.0f;  // eases floor -> peak -> floor

  uint8_t floorBrightness = (uint8_t)(pulsePeakBrightness * PULSE_MIN_FRACTION);
  if (floorBrightness < 1) {
    floorBrightness = 1;
  }
  currentBrightness = floorBrightness + (uint8_t)(factor * (pulsePeakBrightness - floorBrightness));
  render();
}

bool isAuthorized() {
  return server.header("X-API-Key") == API_KEY;
}

// Shared by the HTTP /mode handler and the BLE command handler.
bool applyMode(const String& name) {
  if (name == "busy") {
    setPulseMode(MODE_BUSY_R, MODE_BUSY_G, MODE_BUSY_B, MODE_BUSY_PEAK_BRIGHTNESS, MODE_BUSY_PERIOD_MS, ICON_BUSY);
  } else if (name == "dnd") {
    setPulseMode(MODE_DND_R, MODE_DND_G, MODE_DND_B, MODE_DND_PEAK_BRIGHTNESS, MODE_DND_PERIOD_MS, ICON_DND);
  } else if (name == "free") {
    setStaticMode(MODE_FREE_R, MODE_FREE_G, MODE_FREE_B, MODE_FREE_BRIGHTNESS, ICON_FREE);
  } else if (name == "dndmic") {
    setPulseMode(MODE_DNDMIC_R, MODE_DNDMIC_G, MODE_DNDMIC_B, MODE_DNDMIC_PEAK_BRIGHTNESS, MODE_DNDMIC_PERIOD_MS,
                 ICON_DNDMIC);
  } else if (name == "test") {
    // Cyan "F" icon, for verifying matrix wiring/orientation. See
    // MATRIX_SERPENTINE above if it renders mirrored or rotated.
    setStaticMode(0, 255, 255, MODE_FREE_BRIGHTNESS, ICON_TEST_F);
  } else if (name == "off") {
    setStaticMode(0, 0, 0, 0, ICON_FULL);
  } else {
    return false;
  }
  cancelBootIndicator();
  return true;
}

void handleRoot() {
  server.send(200, "text/plain", "ESP32-S3-Matrix online");
}

void handleColor() {
  if (!isAuthorized()) {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }

  if (!server.hasArg("r") || !server.hasArg("g") || !server.hasArg("b")) {
    server.send(400, "application/json", "{\"error\":\"missing r, g, or b query parameter\"}");
    return;
  }

  int r = constrain(server.arg("r").toInt(), 0, 255);
  int g = constrain(server.arg("g").toInt(), 0, 255);
  int b = constrain(server.arg("b").toInt(), 0, 255);

  if (server.hasArg("brightness")) {
    currentBrightness = constrain(server.arg("brightness").toInt(), 0, LED_MAX_BRIGHTNESS);
  }

  cancelBootIndicator();
  pulsing = false;
  currentIcon = ICON_FULL;
  setColor(r, g, b);

  char body[96];
  snprintf(body, sizeof(body), "{\"status\":\"ok\",\"r\":%d,\"g\":%d,\"b\":%d,\"brightness\":%d}",
           r, g, b, currentBrightness);
  server.send(200, "application/json", body);
}

void handleBrightness() {
  if (!isAuthorized()) {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }

  if (!server.hasArg("level")) {
    server.send(400, "application/json", "{\"error\":\"missing level query parameter\"}");
    return;
  }

  int level = constrain(server.arg("level").toInt(), 0, LED_MAX_BRIGHTNESS);
  cancelBootIndicator();
  pulsing = false;
  setBrightnessLevel(level);

  char body[48];
  snprintf(body, sizeof(body), "{\"status\":\"ok\",\"brightness\":%d}", level);
  server.send(200, "application/json", body);
}

void handleMode() {
  if (!isAuthorized()) {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }

  if (!server.hasArg("name")) {
    server.send(400, "application/json", "{\"error\":\"missing name query parameter\"}");
    return;
  }

  String name = server.arg("name");
  if (!applyMode(name)) {
    server.send(400, "application/json", "{\"error\":\"unknown mode, expected busy, dnd, free, dndmic, test, or off\"}");
    return;
  }

  char body[48];
  snprintf(body, sizeof(body), "{\"status\":\"ok\",\"mode\":\"%s\"}", name.c_str());
  server.send(200, "application/json", body);
}

void setBleStatus(const String& json) {
  if (bleStatusChar == nullptr) {
    return;
  }
  bleStatusChar->setValue(json.c_str());
  bleStatusChar->notify();
}

// Splits a comma-separated list of ints, e.g. "255,0,0,15" -> [255,0,0,15].
// Returns how many values were parsed (up to maxCount).
int parseCsvInts(const String& s, int* out, int maxCount) {
  int count = 0;
  int start = 0;
  while (count < maxCount) {
    int comma = s.indexOf(',', start);
    String token = comma < 0 ? s.substring(start) : s.substring(start, comma);
    if (token.length() == 0) {
      break;
    }
    out[count++] = token.toInt();
    if (comma < 0) {
      break;
    }
    start = comma + 1;
  }
  return count;
}

// Commands look like "<api-key>|<command>|<args>", e.g.:
//   "<key>|color|255,0,0" or "<key>|color|255,0,0,15"
//   "<key>|brightness|10"
//   "<key>|mode|busy"
void handleBleCommand(const String& raw) {
  int firstSep = raw.indexOf('|');
  if (firstSep < 0) {
    setBleStatus("{\"error\":\"malformed command\"}");
    return;
  }

  String key = raw.substring(0, firstSep);
  if (key != API_KEY) {
    setBleStatus("{\"error\":\"unauthorized\"}");
    return;
  }

  String rest = raw.substring(firstSep + 1);
  int secondSep = rest.indexOf('|');
  String cmd = secondSep < 0 ? rest : rest.substring(0, secondSep);
  String args = secondSep < 0 ? "" : rest.substring(secondSep + 1);

  if (cmd == "color") {
    int vals[4];
    int n = parseCsvInts(args, vals, 4);
    if (n < 3) {
      setBleStatus("{\"error\":\"expected args r,g,b[,brightness]\"}");
      return;
    }
    int r = constrain(vals[0], 0, 255);
    int g = constrain(vals[1], 0, 255);
    int b = constrain(vals[2], 0, 255);
    if (n >= 4) {
      currentBrightness = constrain(vals[3], 0, LED_MAX_BRIGHTNESS);
    }
    cancelBootIndicator();
    pulsing = false;
    currentIcon = ICON_FULL;
    setColor(r, g, b);
    char body[96];
    snprintf(body, sizeof(body), "{\"status\":\"ok\",\"r\":%d,\"g\":%d,\"b\":%d,\"brightness\":%d}",
             r, g, b, currentBrightness);
    setBleStatus(body);
  } else if (cmd == "brightness") {
    int level = constrain(args.toInt(), 0, LED_MAX_BRIGHTNESS);
    cancelBootIndicator();
    pulsing = false;
    setBrightnessLevel(level);
    char body[48];
    snprintf(body, sizeof(body), "{\"status\":\"ok\",\"brightness\":%d}", level);
    setBleStatus(body);
  } else if (cmd == "mode") {
    if (!applyMode(args)) {
      setBleStatus("{\"error\":\"unknown mode, expected busy, dnd, free, dndmic, test, or off\"}");
      return;
    }
    char body[48];
    snprintf(body, sizeof(body), "{\"status\":\"ok\",\"mode\":\"%s\"}", args.c_str());
    setBleStatus(body);
  } else {
    setBleStatus("{\"error\":\"unknown command, expected color, brightness, or mode\"}");
  }
}

class BleCommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    String value = String(characteristic->getValue().c_str());
    handleBleCommand(value);
  }
};

class BleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* bleServer) override {
    Serial.println("BLE client connected");
  }
  void onDisconnect(BLEServer* bleServer) override {
    Serial.println("BLE client disconnected, restarting advertising");
    BLEDevice::startAdvertising();
  }
};

void setupBle() {
  BLEDevice::init(HOSTNAME);
  BLEServer* bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new BleServerCallbacks());

  BLEService* bleService = bleServer->createService(BLE_SERVICE_UUID);

  BLECharacteristic* commandChar = bleService->createCharacteristic(
      BLE_COMMAND_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
  commandChar->setCallbacks(new BleCommandCallbacks());

  bleStatusChar = bleService->createCharacteristic(
      BLE_STATUS_CHAR_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  bleStatusChar->addDescriptor(new BLE2902());
  bleStatusChar->setValue("{\"status\":\"ready\"}");

  bleService->start();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  Serial.printf("BLE advertising started as \"%s\"\n", HOSTNAME);
}

void setup() {
  Serial.begin(115200);

  strip.begin();
  strip.clear();
  strip.show();

  setupBle();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to WiFi \"%s\"", WIFI_SSID);
  unsigned long connectStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - connectStart < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  wifiConnected = WiFi.status() == WL_CONNECTED;

  currentBrightness = BOOT_INDICATOR_BRIGHTNESS;
  if (wifiConnected) {
    currentIcon = ICON_WIFI;
    setColor(0, 255, 0);  // green: WiFi connected
    Serial.print("Connected. IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    currentIcon = ICON_BLUETOOTH;
    setColor(255, 165, 0);  // orange: WiFi failed, BLE only
    Serial.println("WiFi connection failed");
  }
  currentBrightness = LED_MAX_BRIGHTNESS;

  bootIndicatorPending = true;
  bootIndicatorDeadline = millis() + BOOT_INDICATOR_DURATION_MS;

  if (!wifiConnected) {
    return;
  }

  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS responder started: http://%s.local\n", HOSTNAME);
  } else {
    Serial.println("mDNS responder failed to start");
  }

  const char* headerKeys[] = {"X-API-Key"};
  server.collectHeaders(headerKeys, 1);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/color", HTTP_POST, handleColor);
  server.on("/color", HTTP_GET, handleColor);
  server.on("/brightness", HTTP_POST, handleBrightness);
  server.on("/brightness", HTTP_GET, handleBrightness);
  server.on("/mode", HTTP_POST, handleMode);
  server.on("/mode", HTTP_GET, handleMode);
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  if (wifiConnected) {
    server.handleClient();
  }
  if (pulsing) {
    updatePulse();
  }
  if (bootIndicatorPending && millis() >= bootIndicatorDeadline) {
    bootIndicatorPending = false;
    applyMode("free");
  }
}
