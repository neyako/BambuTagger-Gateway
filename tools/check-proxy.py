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
inline unsigned long clockMs = 0;
inline unsigned long millis() { return clockMs; }
inline int attempts = 0;
inline bool connectOK = true;
inline std::shared_ptr<bool> remote;
inline std::shared_ptr<std::string> remoteRx, remoteTx;
inline std::shared_ptr<bool> remoteFin, remoteShut;
struct { template <class... A> void printf(const char*, A...) {} } Serial;
struct MockWiFi { bool up = true; bool isConnected() { return up; } };
inline MockWiFi WiFi;
struct WiFiClient {
  std::shared_ptr<bool> live = std::make_shared<bool>(false);
  std::shared_ptr<std::string> rx = std::make_shared<std::string>();
  std::shared_ptr<std::string> tx = std::make_shared<std::string>();
  std::shared_ptr<bool> fin = std::make_shared<bool>(false);  // peer closed cleanly
  std::shared_ptr<bool> shut = std::make_shared<bool>(false); // we sent FIN
  bool connected() { return *live; }  // like the real core: blind to FIN
  explicit operator bool() { return *live; }
  void stop() { *live = false; }
  void setTimeout(int) {}
  void setNoDelay(bool) {}
  bool connect(const char*, uint16_t) {
    attempts++;
    live = std::make_shared<bool>(connectOK);
    rx = std::make_shared<std::string>(); tx = std::make_shared<std::string>();
    fin = std::make_shared<bool>(false); shut = std::make_shared<bool>(false);
    remote = live; remoteRx = rx; remoteTx = tx; remoteFin = fin; remoteShut = shut;
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
inline int mockReserve = 2;  // 2 = printer link down, 1 = up
inline int socketReserve() { return mockReserve; }
inline void shutWrite(WiFiClient &c) { *c.shut = true; }
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
#include <sys/socket.h>
#include <unistd.h>
#include "tcp_proxy.h"
WiFiClient arrive() {
  WiFiClient c; *c.live = true; pending.push_back(c); return c;
}
int main() {
  // The real FIN detector, on real sockets.
  int sv[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  assert(fdOpen(sv[0]));  // open, nothing pending
  assert(write(sv[1], "x", 1) == 1 && shutdown(sv[1], SHUT_WR) == 0);
  assert(fdOpen(sv[0]));  // FIN behind unread data: still open
  char ch;
  assert(read(sv[0], &ch, 1) == 1);
  assert(!fdOpen(sv[0]));  // orderly close detected
  close(sv[0]); close(sv[1]);

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
  TcpProxy ftps("ftps", 990, "printer", 990, 2, true);
  auto u = arrive();
  ftps.loop();
  assert(attempts == 5 && u.connected()); // A short-lived FTPS upload may use the reserve...
  u.stop(); ftps.loop();
  mockFree = 0;
  auto u2 = arrive();
  ftps.loop();
  assert(attempts == 5 && !u2.connected()); // ...but never the last socket,
  mockFree = 1;
  auto u3 = arrive();
  ftps.loop();
  assert(attempts == 5 && !u3.connected()); // ...nor the printer reconnect's while the link is down.
  mockReserve = 1;
  auto u4 = arrive();
  ftps.loop();
  assert(attempts == 6 && u4.connected()); // Link up: its own socket covers the reconnect.
  u4.stop(); ftps.loop();
  mockReserve = 2; mockFree = 8;

  // FTPS after QUIT: printer and client both close cleanly, nothing is written.
  auto h = arrive();
  proxy.loop();
  auto upH = remote; auto upHRx = remoteRx; auto upHFin = remoteFin;
  *upHRx = "221 Goodbye"; *upHFin = true; *h.fin = true;
  proxy.loop();
  assert(*h.tx == "221 Goodbye"); // Final reply still delivered...
  proxy.loop();
  assert(!h.connected() && !*upH); // ...then both sockets are released.

  // Client half-closes; the printer's reply arrives in two parts with a gap.
  auto k = arrive();
  proxy.loop();
  auto upK = remote; auto upKRx = remoteRx; auto upKFin = remoteFin; auto upKShut = remoteShut;
  *upKRx = std::string(512, 'r'); *k.fin = true;
  proxy.loop();
  assert(*upKShut && k.tx->size() == 512 && k.connected()); // FIN passed on, pair kept
  clockMs += 5000; proxy.loop();
  assert(k.connected() && *upK); // a gap in the reply doesn't end it
  *upKRx = std::string(512, 'r'); proxy.loop();
  *upKFin = true; proxy.loop();
  assert(k.tx->size() == 1024 && *k.shut && !k.connected() && !*upK); // whole reply, then closed

  // Client vanished without RST while the printer side sits idle.
  auto m = arrive();
  proxy.loop();
  auto upM = remote;
  *m.fin = true; proxy.loop();
  assert(m.connected());
  clockMs += HALF_CLOSE_IDLE_MS + 1; proxy.loop();
  assert(!m.connected() && !*upM); // reclaimed after the idle timeout

  WiFi.up = false;
  proxy.loop();
  assert(!b.connected() && !*remote); // Wi-Fi loss releases both sides.
  int dials = attempts;
  auto f = arrive();
  proxy.loop();
  assert(attempts == dials && !f.connected()); // No printer dial without Wi-Fi.
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
print("PASS: fdOpen on real sockets, idle, per-client upstream, relay, refusal, full, socket reserve, clean close, half-close across a gap, vanished peer, FTPS reserve, Wi-Fi loss")
