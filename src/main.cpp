/*
  ESP8266 Apple TV 2 IR Remote -> Kodi JSON-RPC Controller

  Notes
  - IR receiver output on D5 (GPIO 14) with a typical demodulating IR receiver
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <base64.h>

// ===== Pin configuration =====
#define IR_PIN 14  // D5 on most ESP8266 boards

// ===== NEC capture timing (microseconds) =====
#define SAMPLE_SIZE        200
#define MIN_PULSE_US        40
#define MAX_PULSE_US     24000
#define IDLE_TIMEOUT_US  50000
#define NEC_HDR_MARK_US    9000
#define NEC_HDR_SPACE_US   4500
#define NEC_BIT_MARK_US     560
#define NEC_ONE_SPACE_US   1690
#define NEC_ZERO_SPACE_US   560
#define NEC_TOLERANCE_US    220

// ===== WiFi and Kodi configuration =====
const char* WIFI_SSID   = "yourssid";
const char* WIFI_PASS   = "yourpass";
const char* KODI_HOST   = "192.168.178.148";
const int   KODI_PORT   = 8080;
const bool  KODI_AUTH   = false;
const char* KODI_USER   = "kodi";
const char* KODI_PASS   = "kodi";

// ===== UX timings =====
const uint32_t HOLD_DELAY_MS      = 250;  
const uint32_t REPEAT_RATE_MS     = 110;  
const uint32_t RELEASE_TIMEOUT_MS = 220;  
const uint32_t DOUBLECLICK_MS     = 300;  
const uint32_t HTTP_TIMEOUT_MS    = 600;

// ===== JSON buffer sizes =====
const size_t JSON_SMALL = 256;
const size_t JSON_MED   = 512;

// ===== IR capture state (ISR filled) =====
volatile unsigned long sTimings[SAMPLE_SIZE];
volatile int           sTimingIdx = 0;
volatile unsigned long sLastEdgeUs = 0;
volatile bool          sCapturing = false;
volatile bool          sFrameReady = false;

// ===== Button and hold state =====
static const char*  gPressedName = nullptr;
static unsigned long gPressStartMs = 0;
static unsigned long gLastRepeatMs = 0;
static bool          gHoldActive   = false;

// one shot flags for holds
static bool gPlayPauseHoldDone = false;
static bool gSelectHoldDone    = false;

// double click memory
static unsigned long gLastLeftMs  = 0;
static unsigned long gLastRightMs = 0;

// ===== Networking =====
WiFiClient gWifi;
HTTPClient gHttp;

// ===== Key map =====
struct IRButton {
  uint8_t addr;
  uint8_t cmd;
  const char* name;
  const char* shortAction;  
  bool holdRepeat;      
};

IRButton kButtons[] = {
  {0x03, 0x87, "MENU",       "back",      false},
  {0x5F, 0x87, "PLAY_PAUSE", "playpause", false}, 
  {0x0A, 0x87, "UP",         "up",        true },
  {0x0C, 0x87, "DOWN",       "down",      true },
  {0x09, 0x87, "LEFT",       "left",      true },
  {0x06, 0x87, "RIGHT",      "right",     true },
  {0x5C, 0x87, "SELECT",     "select",    false} 
};
const int kNumButtons = sizeof(kButtons) / sizeof(kButtons[0]);

// ===== ISR =====
void ICACHE_RAM_ATTR onIrEdge() {
  unsigned long now = micros();
  if (sLastEdgeUs != 0) {
    unsigned long d = now - sLastEdgeUs;
    if (d >= MIN_PULSE_US && d <= MAX_PULSE_US) {
      int idx = sTimingIdx;
      if (idx < SAMPLE_SIZE) {
        sTimings[idx] = d;
        sTimingIdx = idx + 1;
        sCapturing = true;
      }
    }
  }
  sLastEdgeUs = now;
}

// ===== Helpers =====
static inline bool within(unsigned long v, unsigned long ref, unsigned long tol) {
  return (v + tol >= ref) && (v <= ref + tol);
}

static inline void resetCapture() {
  sTimingIdx = 0;
  sCapturing = false;
  sFrameReady = false;
}

const IRButton* lookupButton(uint8_t addr, uint8_t cmd) {
  for (int i = 0; i < kNumButtons; i++) {
    if (kButtons[i].addr == addr && kButtons[i].cmd == cmd) return &kButtons[i];
  }
  return nullptr;
}

// Returns 0 if no valid frame, else 32 bit NEC value
uint32_t decodeNEC() {
  if (sTimingIdx < 34) return 0;
  int i = 0;
  if (!within(sTimings[i++], NEC_HDR_MARK_US,  NEC_TOLERANCE_US)) return 0;
  if (!within(sTimings[i++], NEC_HDR_SPACE_US, NEC_TOLERANCE_US)) return 0;

  uint32_t v = 0;
  for (int bit = 0; bit < 32; bit++) {
    if (i >= sTimingIdx - 1) return 0;
    if (!within(sTimings[i++], NEC_BIT_MARK_US, NEC_TOLERANCE_US)) return 0;

    if      (within(sTimings[i], NEC_ONE_SPACE_US,  NEC_TOLERANCE_US))  v |= (1UL << bit);
    else if (within(sTimings[i], NEC_ZERO_SPACE_US, NEC_TOLERANCE_US))  { /* zero bit */ }
    else return 0;
    i++;
  }
  return v;
}

// ===== JSON-RPC helpers =====
bool httpPostJson(const String& body, String* out = nullptr) {
  String url = String("http://") + KODI_HOST + ":" + KODI_PORT + "/jsonrpc";
  gHttp.setTimeout(HTTP_TIMEOUT_MS);
  gHttp.begin(gWifi, url);

  if (KODI_AUTH) {
    String auth = String(KODI_USER) + ":" + String(KODI_PASS);
    gHttp.addHeader("Authorization", "Basic " + base64::encode(auth));
  }
  gHttp.addHeader("Content-Type", "application/json");

  int code = gHttp.POST(body);
  if (code == 200) {
    if (out) *out = gHttp.getString();
    gHttp.end();
    return true;
  }
  Serial.printf("HTTP %d for %s\n", code, body.c_str());
  gHttp.end();
  return false;
}

bool actionExecute(const char* action) {
  StaticJsonDocument<JSON_SMALL> d;
  d["jsonrpc"] = "2.0";
  d["method"]  = "Input.ExecuteAction";
  d["id"]      = 1;
  d["params"]["action"] = action;
  String body; serializeJson(d, body);
  return httpPostJson(body, nullptr);
}

bool guiActivate(const char* window) {
  StaticJsonDocument<JSON_SMALL> d;
  d["jsonrpc"] = "2.0";
  d["method"]  = "GUI.ActivateWindow";
  d["id"]      = 1;
  d["params"]["window"] = window;
  String body; serializeJson(d, body);
  return httpPostJson(body, nullptr);
}

inline bool actPlayPause()   { return actionExecute("playpause"); }
inline bool actOSD()         { return actionExecute("osd"); }
inline bool actContextMenu() { return actionExecute("contextmenu"); }
inline bool actStepFwd()     { return actionExecute("stepforward"); }
inline bool actStepBack()    { return actionExecute("stepback"); }
inline bool actPowerMenu()   { return guiActivate("shutdownmenu"); }

bool rpcPing() {
  StaticJsonDocument<JSON_SMALL> d;
  d["jsonrpc"] = "2.0";
  d["method"]  = "JSONRPC.Ping";
  d["id"]      = 1;
  String body; serializeJson(d, body);
  return httpPostJson(body, nullptr);
}

int getActivePlayerId() {
  StaticJsonDocument<JSON_SMALL> d;
  d["jsonrpc"] = "2.0";
  d["method"]  = "Player.GetActivePlayers";
  d["id"]      = 1;
  String body; serializeJson(d, body);

  String resp;
  if (!httpPostJson(body, &resp)) return -1;

  StaticJsonDocument<JSON_MED> r;
  if (deserializeJson(r, resp)) return -1;

  JsonArray arr = r["result"].as<JsonArray>();
  if (arr.isNull() || arr.size() == 0) return -1;

  for (JsonVariant v : arr) {
    if (strcmp(v["type"] | "", "video") == 0) return v["playerid"].as<int>();
  }
  return arr[0]["playerid"].as<int>();
}

bool guiIsFullscreenVideo() {
  StaticJsonDocument<JSON_SMALL> d;
  d["jsonrpc"] = "2.0";
  d["method"]  = "GUI.GetProperties";
  d["id"]      = 1;
  JsonArray props = d["params"].createNestedArray("properties");
  props.add("currentwindow");

  String body; serializeJson(d, body);
  String resp;
  if (!httpPostJson(body, &resp)) return false;

  StaticJsonDocument<JSON_MED> r;
  if (deserializeJson(r, resp)) return false;

  const char* name = r["result"]["currentwindow"]["name"] | "";
  int id = r["result"]["currentwindow"]["id"] | 0;
  return (name && strcmp(name, "fullscreenvideo") == 0) || (id == 12005);
}

bool isPureFullscreenPlayback() {
  if (getActivePlayerId() < 0) return false;

  StaticJsonDocument<JSON_SMALL> d;
  d["jsonrpc"] = "2.0";
  d["method"]  = "GUI.GetProperties";
  d["id"]      = 1;
  JsonArray props = d["params"].createNestedArray("properties");
  props.add("currentwindow");
  props.add("currentcontrol");

  String body; serializeJson(d, body);
  String resp;
  if (!httpPostJson(body, &resp)) return false;

  StaticJsonDocument<JSON_MED> r;
  if (deserializeJson(r, resp)) return false;

  const char* wname = r["result"]["currentwindow"]["name"] | "";
  int wid = r["result"]["currentwindow"]["id"] | 0;
  bool fs = (wname && strcmp(wname, "fullscreenvideo") == 0) || (wid == 12005);
  if (!fs) return false;

  const char* ctype  = r["result"]["currentcontrol"]["type"]  | "";
  const char* clabel = r["result"]["currentcontrol"]["label"] | "";
  bool hasFocus = (ctype && *ctype) || (clabel && *clabel);
  return !hasFocus;
}

bool playerIsForeground() {
  if (getActivePlayerId() < 0) return false;
  return guiIsFullscreenVideo();
}

// ===== Behavior =====
void printMap() {
  Serial.println("=== Mappings ===");
  for (int i = 0; i < kNumButtons; i++) {
    Serial.printf("%-11s short: %s", kButtons[i].name, kButtons[i].shortAction ? kButtons[i].shortAction : "(none)");
    if (kButtons[i].holdRepeat) Serial.print(" | hold: repeat");
    if (strcmp(kButtons[i].name, "LEFT") == 0 || strcmp(kButtons[i].name, "RIGHT") == 0)
      Serial.print(" | double click: skip");
    if (strcmp(kButtons[i].name, "DOWN") == 0)
      Serial.print(" | fullscreen: OSD");
    if (strcmp(kButtons[i].name, "PLAY_PAUSE") == 0)
      Serial.print(" | hold: power menu");
    if (strcmp(kButtons[i].name, "SELECT") == 0)
      Serial.print(" | hold in UI: context menu; pure fullscreen short: play/pause");
    Serial.println();
  }
  Serial.println("================");
}

void handleShortPress(const IRButton* b) {
  if (!b || !b->shortAction) return;

  if (strcmp(b->name, "PLAY_PAUSE") == 0) return;
  if (strcmp(b->name, "SELECT") == 0)     return;

  if (strcmp(b->name, "DOWN") == 0) {
    if (playerIsForeground()) { actOSD(); return; }
  }

  if (strcmp(b->name, "RIGHT") == 0 || strcmp(b->name, "LEFT") == 0) {
    unsigned long now = millis();
    bool isRight = strcmp(b->name, "RIGHT") == 0;
    bool dbl = false;
    if (getActivePlayerId() >= 0) {
      if (isRight) { dbl = (now - gLastRightMs) <= DOUBLECLICK_MS; gLastRightMs = now; }
      else         { dbl = (now - gLastLeftMs)  <= DOUBLECLICK_MS; gLastLeftMs  = now; }
      if (dbl) { if (isRight) actStepFwd(); else actStepBack(); return; }
    }
  }

  actionExecute(b->shortAction);
}

void handleHoldRepeat() {
  if (!gPressedName) return;

  if (strcmp(gPressedName, "PLAY_PAUSE") == 0) return;
  if (strcmp(gPressedName, "SELECT") == 0)     return;

  const IRButton* b = nullptr;
  for (int i = 0; i < kNumButtons; i++) {
    if (strcmp(kButtons[i].name, gPressedName) == 0) { b = &kButtons[i]; break; }
  }
  if (!b || !b->holdRepeat) return;

  if (millis() - gLastRepeatMs >= REPEAT_RATE_MS) {
    actionExecute(b->shortAction);
    gLastRepeatMs = millis();
  }
}

void handleRelease() {
  if (gPressedName && strcmp(gPressedName, "PLAY_PAUSE") == 0) {
    if (!gHoldActive && !gPlayPauseHoldDone) actPlayPause();
  }

  if (gPressedName && strcmp(gPressedName, "SELECT") == 0) {
    if (!gHoldActive && !gSelectHoldDone) {
      if (isPureFullscreenPlayback()) actPlayPause();
      else actionExecute("select");
    }
  }

  gPressedName = nullptr;
  gHoldActive = false;
  gPlayPauseHoldDone = false;
  gSelectHoldDone    = false;
}

// ===== Arduino lifecycle =====
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("Apple TV 2 IR -> Kodi JSON-RPC");
  Serial.printf("WiFi SSID: %s  Kodi: %s:%d\n", WIFI_SSID, KODI_HOST, KODI_PORT);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.printf("\nWiFi connected, IP %s\n", WiFi.localIP().toString().c_str());

  pinMode(IR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(IR_PIN), onIrEdge, CHANGE);

  Serial.println("Testing JSONRPC.Ping");
  if (rpcPing()) Serial.println("Kodi reachable");
  else Serial.println("Kodi unreachable. Enable Control in Kodi settings.");

  printMap();
}

void loop() {
  if (sCapturing && (micros() - sLastEdgeUs) > IDLE_TIMEOUT_US) {
    sCapturing = false;
    sFrameReady = true;
  }

  if (sFrameReady && sTimingIdx > 0) {
    uint32_t v = decodeNEC();
    if (v != 0) {
      uint8_t addr = (v >> 16) & 0xFF;
      uint8_t cmd  = (v >> 8)  & 0xFF;

      const IRButton* b = lookupButton(addr, cmd);
      Serial.printf("IR A=0x%02X C=0x%02X -> %s\n", addr, cmd, b ? b->name : "UNKNOWN");

      if (b) {
        handleShortPress(b);
        gPressedName   = b->name;
        gPressStartMs  = millis();
        gLastRepeatMs  = gPressStartMs;
        gHoldActive    = false;
        gPlayPauseHoldDone = false;
        gSelectHoldDone    = false;
      } else {
        handleRelease();
      }
    }
    resetCapture();
  }

  if (gPressedName && !gHoldActive) {
    if ((millis() - gPressStartMs) >= HOLD_DELAY_MS) {
      gHoldActive = true;
      gLastRepeatMs = millis();
      Serial.printf("%s HOLD start\n", gPressedName);

      if (strcmp(gPressedName, "PLAY_PAUSE") == 0) {
        actPowerMenu();            
        gPlayPauseHoldDone = true;
      } else if (strcmp(gPressedName, "SELECT") == 0) {
        if (!playerIsForeground()) {
          actContextMenu();       
          gSelectHoldDone = true;
        }
      }
    }
  }

  // Hold repeat
  if (gHoldActive) handleHoldRepeat();

  // Release inferred by inactivit
  if (gPressedName) {
    unsigned long sinceEdgeMs = (micros() - sLastEdgeUs) / 1000UL;
    if (sinceEdgeMs > RELEASE_TIMEOUT_MS) {
      if (gHoldActive) Serial.printf("%s RELEASE\n", gPressedName);
      handleRelease();
    }
  }

  delay(5);
}
