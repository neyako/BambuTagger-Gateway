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
#include <esp_task_wdt.h>
#include <esp_core_dump.h>
#else
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <Updater.h>
using WebServer = ESP8266WebServer;
#endif
#include "mqtt_bridge.h"
#include "tcp_proxy.h"  // freeSockets()
#include "logo_png.h"
#include "github_ca.h"


#define EEPROM_SIZE 512
#define EEPROM_MAGIC 0x46
#define EEPROM_MAGIC_V1 0x45  // 1.0.0 layout, migrated on load
static_assert(sizeof(GatewayConfig) <= EEPROM_SIZE, "config outgrew EEPROM");

extern MqttBridge mqtt;
extern char _displayName[];
class TcpProxy;
extern TcpProxy camProxy, ftpsProxy;

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
[hidden]{display:none!important}
.wrapper{display:flex;align-items:center;justify-content:center;padding:16px}
.card{background:#161b22;border:1px solid #30363d;border-radius:6px;padding:28px;width:100%;max-width:480px}
h2{color:#58a6ff;font-size:.9em;letter-spacing:.05em;text-transform:uppercase;margin:0 0 16px;padding-bottom:4px;border-bottom:1px solid #30363d}
label{display:block;color:#8b949e;font-size:.8em;margin-bottom:3px}
input{display:block;width:100%;padding:10px 12px;margin-bottom:10px;background:#0d1117;color:#c9d1d9;border:1px solid #30363d;border-radius:6px;font-size:.95em;outline:none;transition:border .2s}
select{display:block;width:100%;padding:10px 12px;margin-bottom:10px;background:#0d1117;color:#c9d1d9;border:1px solid #30363d;border-radius:6px;font-size:.95em;outline:none;transition:border .2s;cursor:pointer}
input:focus{border-color:#58a6ff}
button{display:block;width:100%;padding:13px;margin-top:6px;background:#238636;color:#fff;border:none;border-radius:6px;font-size:1.05em;font-weight:bold;cursor:pointer;transition:background .2s}
button:hover{background:#2ea043}
button:disabled{background:#484f58;cursor:not-allowed}
.hint{color:#484f58;font-size:.78em;margin-top:-7px;margin-bottom:10px}
footer{position:fixed;bottom:0;left:0;right:0;text-align:center;padding:6px;font-size:10px;color:#484f58;background:#0d1117;border-top:1px solid #30363d}
footer a{color:#484f58;text-decoration:none}
footer a:hover{color:#c9d1d9}
)";

// ── EEPROM helpers ─────────────────────────────────────────────────────

// 1.0.0 layout: SSID one byte short, no admin password.
#pragma pack(push, 1)
struct GatewayConfigV1 {
  uint8_t magic;
  char stationSsid[32], stationPass[64], printerHost[64], printerCode[32],
       printerSerial[32], gatewaySerial[32], printerModel[24];
};
#pragma pack(pop)

static void configLoad() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, _cfg);
  if (_cfg.magic == EEPROM_MAGIC_V1) {
    GatewayConfigV1 v1;
    EEPROM.get(0, v1);
    memset(&_cfg, 0, sizeof(_cfg));
    _cfg.magic = EEPROM_MAGIC;
    memcpy(_cfg.stationSsid, v1.stationSsid, sizeof(v1.stationSsid));
    memcpy(_cfg.stationPass, v1.stationPass, sizeof(v1.stationPass));
    memcpy(_cfg.printerHost, v1.printerHost, sizeof(v1.printerHost));
    memcpy(_cfg.printerCode, v1.printerCode, sizeof(v1.printerCode));
    memcpy(_cfg.printerSerial, v1.printerSerial, sizeof(v1.printerSerial));
    memcpy(_cfg.gatewaySerial, v1.gatewaySerial, sizeof(v1.gatewaySerial));
    memcpy(_cfg.printerModel, v1.printerModel, sizeof(v1.printerModel));
  }
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
  client.setCACert(GITHUB_CA);
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

// ── page builder helpers ───────────────────────────────────────────────

// With an admin password saved, every page and action asks for it (user
// "admin"). Without one the UI stays open for first-time setup.
static bool authorized() {
  if (!_cfg.adminPass[0] || _server.authenticate("admin", _cfg.adminPass)) return true;
  _server.requestAuthentication(BASIC_AUTH, "BambuTagger-Gateway");
  return false;
}

// Config values are user input; escape before putting them into HTML.
static String htmlEsc(const char *s) {
  String out;
  for (; *s; s++) {
    switch (*s) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      default:  out += *s;
    }
  }
  return out;
}


static String buildPage(const char *tmpl1, const char *tmpl2) {
  String page = FPSTR(tmpl1);
  page += FPSTR(_SITE_STYLE);
  page += FPSTR(tmpl2);
  return page;
}

// ── page files ──────────────────────────────────────────────────────────

#include "page_dashboard.h"
#include "page_ota.h"
#include "page_saved.h"

// ── handlers ───────────────────────────────────────────────────────────

static void handleNotFound() {
  // Redirect to the interface the request arrived on (AP or STA); old
  // multi-page URLs land here too.
  String loc = "http://" + _server.client().localIP().toString() + "/";
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
  _server.on("/ca.pem", []() {  // used by tools/trust-gateway.sh
    _server.send(200, "application/x-pem-file", mqtt.getTlsCert());
  });
  _server.on("/api/release", handleApiRelease);
  _server.on("/update/github", HTTP_POST, handleUpdateFromGithub);
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
