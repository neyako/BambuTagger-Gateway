"""Offline regression check for the real TcpProxy loop; requires a C++ compiler.

Run: python3 tools/check-proxy.py
No printer or gateway connections are made.
"""

from pathlib import Path
import subprocess
import tempfile


root = Path(__file__).resolve().parents[1]
stub = r"""
#pragma once
#include <cstdint>
#include <cstring>
#include <memory>
#include <deque>
inline unsigned long clockMs = 0;
inline unsigned long millis() { return clockMs; }
inline int attempts = 0;
inline bool connectOK = true;
inline std::shared_ptr<bool> remote;
struct MockWiFi { bool up = true; bool isConnected() { return up; } };
inline MockWiFi WiFi;
struct WiFiClient {
  std::shared_ptr<bool> live = std::make_shared<bool>(false);
  bool connected() { return *live; }
  void stop() { *live = false; }
  void setTimeout(int) {}
  void setNoDelay(bool) {}
  bool connect(const char*, uint16_t) {
    attempts++; clockMs += 1000; *live = connectOK; remote = live; return *live;
  }
  int available() { return 0; }
  int read(uint8_t*, size_t) { return 0; }
  size_t write(const uint8_t*, size_t n) { return n; }
};
inline std::deque<WiFiClient> pending;
struct WiFiServer {
  WiFiServer(uint16_t) {}
  void begin() {}
  void setNoDelay(bool) {}
  WiFiClient accept() {
    if (pending.empty()) return WiFiClient();
    auto c = pending.front(); pending.pop_front(); return c;
  }
};
"""
check = r"""
#include <cassert>
#include "tcp_proxy.h"
WiFiClient arrive() {
  WiFiClient c; *c.live = true; pending.push_back(c); return c;
}
int main() {
  TcpProxy proxy("camera", 6000, "printer", 6000, 2);
  proxy.begin();
  clockMs = 120000;
  proxy.loop();
  assert(attempts == 0); // Idle proxy never occupies a printer slot.
  auto first = arrive();
  proxy.loop();
  assert(attempts == 1 && *remote);
  first.stop();
  proxy.loop();
  assert(!*remote); // Last local client leaving releases upstream.
  connectOK = false;
  clockMs += UPSTREAM_RETRY_MS + 1;
  auto second = arrive();
  proxy.loop();
  assert(attempts == 2 && !*remote);
  clockMs += UPSTREAM_RETRY_MS;
  proxy.loop();
  assert(attempts == 2); // Cooldown includes the failed connect duration.
  clockMs++;
  connectOK = true;
  proxy.loop();
  assert(attempts == 3 && *remote);
  WiFi.up = false;
  proxy.loop();
  assert(!*remote && !second.connected()); // Wi-Fi loss releases both sides.
  WiFi.up = true;
  clockMs += UPSTREAM_RETRY_MS + 1;
  proxy.loop();
  assert(attempts == 3); // Restoring Wi-Fi alone cannot reopen upstream.
}
"""
with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    (temp / "ESP8266WiFi.h").write_text(stub)
    (temp / "check.cpp").write_text(check)
    subprocess.run([
        "c++", "-std=c++17", "-I", str(temp), "-I", str(root / "src"),
        str(temp / "check.cpp"), str(root / "src/tcp_proxy.cpp"),
        "-o", str(temp / "check"),
    ], check=True)
    subprocess.run([str(temp / "check")], check=True)
print("PASS: idle connections, release, retry cooldown, Wi-Fi loss and recovery")
