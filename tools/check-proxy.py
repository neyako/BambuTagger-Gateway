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
#include <string>
inline int attempts = 0;
inline bool connectOK = true;
inline std::shared_ptr<bool> remote;
inline std::shared_ptr<std::string> remoteRx, remoteTx;
inline std::shared_ptr<bool> remoteFin;
struct { template <class... A> void printf(const char*, A...) {} } Serial;
struct MockWiFi { bool up = true; bool isConnected() { return up; } };
inline MockWiFi WiFi;
struct WiFiClient {
  std::shared_ptr<bool> live = std::make_shared<bool>(false);
  std::shared_ptr<std::string> rx = std::make_shared<std::string>();
  std::shared_ptr<std::string> tx = std::make_shared<std::string>();
  std::shared_ptr<bool> fin = std::make_shared<bool>(false);  // peer closed cleanly
  bool connected() { return *live; }  // like the real core: blind to FIN
  explicit operator bool() { return *live; }
  void stop() { *live = false; }
  void setTimeout(int) {}
  void setNoDelay(bool) {}
  bool connect(const char*, uint16_t) {
    attempts++;
    live = std::make_shared<bool>(connectOK);
    rx = std::make_shared<std::string>(); tx = std::make_shared<std::string>();
    fin = std::make_shared<bool>(false);
    remote = live; remoteRx = rx; remoteTx = tx; remoteFin = fin;
    return *live;
  }
  int available() { return rx->size(); }
  int read(uint8_t* b, size_t n) {
    n = std::min(n, rx->size()); std::memcpy(b, rx->data(), n); rx->erase(0, n); return n;
  }
  size_t write(const uint8_t* b, size_t n) { tx->append((const char*)b, n); return n; }
};
inline std::deque<WiFiClient> pending;
inline int mockFree = 8;
inline int freeSockets() { return mockFree; }
inline bool peerOpen(WiFiClient &c) { return c.connected() && (c.available() || !*c.fin); }
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
  proxy.loop();
  assert(attempts == 0); // Idle proxy never occupies a printer slot.

  auto a = arrive();
  proxy.loop();
  assert(attempts == 1 && *remote && a.connected());
  auto upA = remote; auto upARx = remoteRx; auto upATx = remoteTx;
  *upARx = "frame"; *a.rx = "auth";
  proxy.loop();
  assert(*a.tx == "frame" && *upATx == "auth"); // Bytes relay both ways.

  auto b = arrive();
  proxy.loop();
  assert(attempts == 2 && remote != upA && *remote && *upA); // TLS sessions never share a socket.
  assert(a.tx->size() == 5); // A second client sees none of the first client's stream.

  a.stop();
  proxy.loop();
  assert(!*upA && b.connected()); // A client leaving releases only its own upstream.

  connectOK = false;
  auto c = arrive();
  proxy.loop();
  assert(attempts == 3 && !c.connected()); // Printer refusal closes the client at once.
  connectOK = true;

  auto d = arrive();
  proxy.loop();
  auto e = arrive();
  proxy.loop();
  assert(attempts == 4 && d.connected() && !e.connected()); // Full: reject without dialing.

  d.stop(); proxy.loop();
  mockFree = SOCKET_RESERVE;
  auto g = arrive();
  proxy.loop();
  assert(attempts == 4 && !g.connected()); // Socket reserve kept for printer MQTT + web UI.
  mockFree = 8;

  // FTPS after QUIT: printer and client both close cleanly, nothing is written.
  auto h = arrive();
  proxy.loop();
  auto upH = remote; auto upHRx = remoteRx; auto upHFin = remoteFin;
  *upHRx = "221 Goodbye"; *upHFin = true; *h.fin = true;
  proxy.loop();
  assert(*h.tx == "221 Goodbye"); // Final reply still delivered...
  proxy.loop();
  assert(!h.connected() && !*upH); // ...then both sockets are released.

  WiFi.up = false;
  proxy.loop();
  assert(!b.connected() && !*remote); // Wi-Fi loss releases both sides.
  auto f = arrive();
  proxy.loop();
  assert(attempts == 5 && !f.connected()); // No printer dial without Wi-Fi.
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
print("PASS: idle, per-client upstream, relay, refusal, full, socket reserve, clean close, Wi-Fi loss")
