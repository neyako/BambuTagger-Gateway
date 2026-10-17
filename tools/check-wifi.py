"""Offline check of the real wifiLoop() policy against the power-cut acceptance tests.

Run: python3 tools/check-wifi.py   (needs a C++ compiler; no hardware)
The mock core never auto-reconnects, i.e. the worst case where it has given up.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "src/main.cpp").read_text()
policy = source[source.index("// WiFi policy."):source.index("// XIAO ESP32-S3")]

stub = r"""
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
using std::min;
unsigned long clockMs = 0;
unsigned long millis() { return clockMs; }
struct { template <class... A> void printf(const char*, A...) {} void println(const char*) {} } Serial;
struct GatewayConfig { char stationSsid[33]; char stationPass[64]; char adminPass[64]; };
GatewayConfig cfg = {"iotshit", "pw", ""};
GatewayConfig *webconfigGetConfig() { return &cfg; }
#define GATEWAY_AP_SSID "BambuTagger-Gateway"
#define GATEWAY_AP_PASS ""
#define GATEWAY_AP_CHANNEL 1
enum { WIFI_MODE_STA = 1, WIFI_MODE_AP = 2 };
bool routerUp = false;
struct Ip { const char *c_str() { return "192.168.4.1"; } };
struct { Ip toString() { return {}; } } ipObj;
struct MockWiFi {
  bool sta = false, ap = false;
  long joinAt = -1;  // a begin() while the router is up joins 5 s later
  int begins = 0;
  bool failNext = false;  // next attempt fails even though the router is up
  std::string apPass;
  int getMode() { return WIFI_MODE_STA | (ap ? WIFI_MODE_AP : 0); }
  bool isConnected() { return sta; }
  int status() { return sta ? 3 : 1; }
  void disconnect() { sta = false; joinAt = -1; }
  void begin(const char*, const char*) {
    begins++;
    if (routerUp && !failNext) joinAt = clockMs + 5000;
    if (routerUp) failNext = false;
  }
  bool softAP(const char*, const char* pass, int) { ap = true; apPass = pass ? pass : ""; return true; }
  void softAPdisconnect(bool) { ap = false; }
  decltype(ipObj) &softAPIP() { return ipObj; }
} WiFi;
"""

check = r"""
// One simulated second: radio state, then the firmware loop, then invariants.
void tick() {
  clockMs += 1000;
  if (!routerUp) { WiFi.sta = false; WiFi.joinAt = -1; }
  if (WiFi.joinAt >= 0 && (long)clockMs >= WiFi.joinAt) { WiFi.sta = true; WiFi.joinAt = -1; }
  wifiLoop();
  assert(!(WiFi.sta && WiFi.ap));  // the AP is never on while STA is connected
}
long secs() { return clockMs / 1000; }

int main() {
  // Boot with the router off; it comes back after 3 min (setup() ran begin() once).
  WiFi.begin("iotshit", "pw");
  bool apSeen = false;
  while (secs() < 180) { tick(); apSeen |= WiFi.ap; }
  assert(apSeen && WiFi.ap && WiFi.apPass.empty());  // fallback AP is up (open: no admin password)
  routerUp = true;
  WiFi.failNext = true;  // first attempt after the router returns fails (seen on hardware)
  long up = secs();
  while (!WiFi.sta && secs() < up + 600) tick();
  assert(WiFi.sta && secs() - up <= 70);  // still joins within about a minute
  tick();
  assert(!WiFi.ap);  // setup AP gone once joined

  // Router reboot (2 min): reconnects on its own, setup AP never opens.
  routerUp = false;
  for (int i = 0; i < 120; i++) { tick(); assert(!WiFi.ap); }
  routerUp = true;
  up = secs();
  while (!WiFi.sta && secs() < up + 600) { tick(); assert(!WiFi.ap); }
  assert(WiFi.sta && secs() - up <= 70);

  // Long outage: AP opens after 10 min, closes after 5, STA retries throughout.
  routerUp = false;
  long down = secs();
  int begins = WiFi.begins;
  while (!WiFi.ap) tick();
  assert(secs() - down >= 600 && secs() - down <= 602);
  long apOn = secs();
  while (WiFi.ap) tick();
  assert(secs() - apOn >= 300 && secs() - apOn <= 302);
  assert(WiFi.begins - begins >= 28);  // every 30 s, never gives up
  routerUp = true;
  up = secs();
  while (!WiFi.sta) tick();
  assert(secs() - up <= 70 && !WiFi.ap);

  // With an admin password the fallback AP is WPA2-protected by it.
  strcpy(cfg.adminPass, "hunter22");
  routerUp = false;
  while (!WiFi.ap) tick();
  assert(WiFi.apPass == "hunter22");

  // No saved credentials: the policy leaves the setup AP alone forever.
  cfg.stationSsid[0] = 0;
  WiFi.ap = true; WiFi.sta = false; routerUp = false;
  begins = WiFi.begins;
  for (int i = 0; i < 7200; i++) wifiLoop(), clockMs += 1000;
  assert(WiFi.ap && WiFi.begins == begins);
}
"""

with tempfile.TemporaryDirectory() as tmp:
    p = Path(tmp)
    (p / "check.cpp").write_text(stub + policy + check)
    subprocess.run(["c++", "-std=c++17", str(p / "check.cpp"), "-o", str(p / "check")], check=True)
    subprocess.run([str(p / "check")], check=True)
print("PASS: power-cut boot, router reboot, long outage AP window, no-credentials AP")
