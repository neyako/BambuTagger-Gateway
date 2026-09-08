#pragma once

#include <EEPROM.h>
#include "config.h"

class MqttBridge;
extern MqttBridge mqtt;

#ifdef ESP32
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#else
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <Updater.h>
using WebServer = ESP8266WebServer;
#endif
#include "mqtt_bridge.h"
#include "logo_png.h"


#define EEPROM_SIZE 512
#define EEPROM_MAGIC 0x45

extern MqttBridge mqtt;
extern char _displayName[];

static GatewayConfig _cfg;
static WebServer _server(80);
static bool _webconfigActive = false;

// ── shared styles ───────────────────────────────────────────────────────

static const char _SITE_STYLE[] PROGMEM = R"(
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0d1117;color:#c9d1d9;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;min-height:100vh;padding-bottom:28px}
header{background:#161b22;border-bottom:1px solid #30363d;padding:12px 24px}
header .logo{display:flex;align-items:center;gap:10px}
header .logo img{width:28px;height:28px;flex-shrink:0;border-radius:4px}
header h1{font-size:18px;color:#58a6ff}
nav{background:#161b22;border-bottom:1px solid #30363d;display:flex;gap:0}
nav a{padding:10px 20px;color:#8b949e;text-decoration:none;font-size:14px;border-bottom:2px solid transparent;transition:all .2s}
nav a:hover{color:#c9d1d9}
nav a.active{color:#58a6ff;border-bottom-color:#58a6ff}
.wrapper{display:flex;align-items:center;justify-content:center;padding:16px}
.card{background:#161b22;border:1px solid #30363d;border-radius:6px;padding:28px;width:100%;max-width:480px}
h2{color:#58a6ff;font-size:.9em;letter-spacing:.05em;text-transform:uppercase;margin:0 0 16px;padding-bottom:4px;border-bottom:1px solid #30363d}
label{display:block;color:#8b949e;font-size:.8em;margin-bottom:3px}
input{display:block;width:100%;padding:10px 12px;margin-bottom:10px;background:#0d1117;color:#c9d1d9;border:1px solid #30363d;border-radius:6px;font-size:.95em;outline:none;transition:border .2s}
select{display:block;width:100%;padding:10px 12px;margin-bottom:10px;background:#0d1117;color:#c9d1d9;border:1px solid #30363d;border-radius:6px;font-size:.95em;outline:none;transition:border .2s;cursor:pointer}
input:focus{border-color:#58a6ff}
button{display:block;width:100%;padding:13px;margin-top:6px;background:#238636;color:#fff;border:none;border-radius:6px;font-size:1.05em;font-weight:bold;cursor:pointer;transition:background .2s}
button:hover{background:#2ea043}
.hint{color:#484f58;font-size:.78em;margin-top:-7px;margin-bottom:10px}
footer{position:fixed;bottom:0;left:0;right:0;text-align:center;padding:6px;font-size:10px;color:#484f58;background:#0d1117;border-top:1px solid #30363d}
footer a{color:#484f58;text-decoration:none}
footer a:hover{color:#c9d1d9}
)";

// ── EEPROM helpers ─────────────────────────────────────────────────────

static void configLoad() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, _cfg);
  if (_cfg.magic != EEPROM_MAGIC) {
    memset(&_cfg, 0, sizeof(_cfg));
    _cfg.magic = EEPROM_MAGIC;
    strlcpy(_cfg.printerHost, PRINTER_HOST_DFLT, sizeof(_cfg.printerHost));
    strlcpy(_cfg.printerCode, PRINTER_CODE_DFLT, sizeof(_cfg.printerCode));
    strlcpy(_cfg.printerSerial, PRINTER_SERIAL_DFLT, sizeof(_cfg.printerSerial));
    char gwSerial[32];
#if defined(ESP32)
    uint64_t mac = ESP.getEfuseMac();
    uint32_t chipId = (uint32_t)(mac >> 24);
#else
    uint32_t chipId = ESP.getChipId();
#endif
    snprintf(gwSerial, sizeof(gwSerial), "22E8BJ5B%07X", chipId & 0xFFFFFF);
    strlcpy(_cfg.gatewaySerial, gwSerial, sizeof(_cfg.gatewaySerial));
    strlcpy(_cfg.printerModel, PRINTER_MODEL_DFLT, sizeof(_cfg.printerModel));
    _cfg.stationSsid[0] = '\0';
    _cfg.stationPass[0] = '\0';
  }
  EEPROM.end();
}

static void configSave() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, _cfg);
  EEPROM.commit();
  EEPROM.end();
}

// ── version helpers ────────────────────────────────────────────────────

static int compareVersions(const String &a, const String &b) {
  int ma = 0, mi = 0, pa = 0;
  int mb = 0, ni = 0, pb = 0;
  String va = a, vb = b;
  if (va.length() > 0 && va[0] == 'v') va = va.substring(1);
  if (vb.length() > 0 && vb[0] == 'v') vb = vb.substring(1);
  sscanf(va.c_str(), "%d.%d.%d", &ma, &mi, &pa);
  sscanf(vb.c_str(), "%d.%d.%d", &mb, &ni, &pb);
  if (ma != mb) return ma < mb ? -1 : 1;
  if (mi != ni) return mi < ni ? -1 : 1;
  if (pa != pb) return pa < pb ? -1 : 1;
  return 0;
}

static String fetchLatestVersion() {
  WiFiClientSecure client;
  client.setInsecure();
#ifdef ESP32
  client.setTimeout(5);  // WiFiClientSecure uses seconds; HTTPClient uses milliseconds
  client.setHandshakeTimeout(5);
#else
  client.setTimeout(5000);
#endif

  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(10000);
  http.setUserAgent(F("BambuTagger-Gateway"));

  http.begin(client, F("https://api.github.com/repos/VID-PRO/BambuTagger-Gateway/releases/latest"));
  int code = http.GET();

  String version;
  if (code == 200) {
    String body = http.getString();
    const char *key = "\"tag_name\":\"";
    int start = body.indexOf(key);
    if (start >= 0) {
      start += strlen(key);
      int end = body.indexOf('"', start);
      if (end > start) version = body.substring(start, end);
    }
  }
  http.end();
  return version;
}

// ── page builder helper ────────────────────────────────────────────────

static String buildPage(const char *tmpl1, const char *tmpl2) {
  String page = FPSTR(tmpl1);
  page += FPSTR(_SITE_STYLE);
  page += FPSTR(tmpl2);
  return page;
}

// ── page files ──────────────────────────────────────────────────────────

#include "page_dashboard.h"
#include "page_settings.h"
#include "page_wifi.h"
#include "page_ota.h"
#include "page_saved.h"
#include "page_cert.h"
#include "page_setup.h"

// ── handlers ───────────────────────────────────────────────────────────

static void handleNotFound() {
  String loc;
  if (WiFi.getMode() == WIFI_AP) {
    loc = "http://" + WiFi.softAPIP().toString();
  } else {
    loc = "http://" + WiFi.localIP().toString();
  }
  _server.sendHeader("Location", loc, true);
  _server.send(302, "text/html",
    "<html><body><a href=\"" + loc + "\">Continue</a></body></html>");
}

static void handleLogo() {
  _server.sendHeader("Cache-Control", "max-age=86400");
  _server.send_P(200, "image/png", (const char*)logo_png, sizeof(logo_png));
}

// ── Public API ─────────────────────────────────────────────────────────

inline void webconfigInit() {
  configLoad();
}

inline void webconfigBegin() {
  _server.on("/", handleRoot);
  _server.on("/Logo/bambutagger.png", handleLogo);
  _server.on("/cert", handleCert);
  _server.on("/ca.pem", handleCaPem);
  _server.on("/config", []() {
    _server.sendHeader("Location", "/config/settings", true);
    _server.send(302, "text/plain", "");
  });
  _server.on("/config/settings", handleSettings);
  _server.on("/config/wifi", handleWifi);
  _server.on("/setup", handleSetup);
  _server.on("/config/ota", handleOta);
  _server.on("/api/release", handleApiRelease);
  _server.on("/update/github", handleUpdateFromGithub);
  _server.on("/save", HTTP_POST, handleSave);
  _server.onNotFound(handleNotFound);

  _server.begin();
  _webconfigActive = true;
}

inline void webconfigLoop() {
  if (_webconfigActive) {
    _server.handleClient();
  }
}

inline GatewayConfig *webconfigGetConfig() {
  return &_cfg;
}
