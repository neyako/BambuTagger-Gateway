"""Offline check of the real CONNECT parser that gates MQTT logins.

Run: python3 tools/check-mqtt-connect.py   (needs a C++ compiler; no hardware)
Every truncation of a valid packet is parsed under AddressSanitizer.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "src/mqtt_bridge.cpp").read_text()
parser = source[source.index("bool parseConnect("):source.index("// end parseConnect")]
readers = source[source.index("bool MqttBridge::readByte("):source.index("void MqttBridge::writeRemainingLength(")]

stub = r"""
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
using String = std::string;
unsigned long clockMs = 0;
unsigned long millis() { return clockMs; }
void delay(unsigned long ms) { clockMs += ms; }
#define MQTT_PACKET_MS 2000UL
static unsigned long pktStart;
struct WiFiClient {  // sends one byte every `gap` ms
  unsigned long gap = 0, next = 0; bool stopped = false;
  int read(uint8_t *b, size_t n) {
    if (clockMs < next) return -1;
    next = clockMs + gap; b[0] = 'a';
    if (gap) return 1;
    for (size_t i = 0; i < n; i++) b[i] = 'a';
    return n;
  }
  uint8_t connected() { return !stopped; }
  void stop() { stopped = true; }
};
struct MqttBridge {
  static bool readByte(WiFiClient &c, uint8_t &b);
  static bool readRemainingLength(WiFiClient &c, uint32_t &len);
  static bool readBytes(WiFiClient &c, uint8_t *buf, uint32_t len);
};
"""

check = r"""
using Bytes = std::vector<uint8_t>;
void str(Bytes &b, const std::string &s) { b.push_back(s.size() >> 8); b.push_back(s.size()); b.insert(b.end(), s.begin(), s.end()); }
Bytes connect(const char *proto, uint8_t level, uint8_t flags, bool will, const char *user, const char *pass) {
  Bytes b; str(b, proto); b.push_back(level); b.push_back(flags); b.push_back(0); b.push_back(60);
  str(b, "client-1");
  if (will) { str(b, "device/x/status"); str(b, "offline"); }
  if (user) str(b, user);
  if (pass) str(b, pass);
  return b;
}
bool parse(const Bytes &b, String &pass, uint16_t &ka) {
  String cid;
  uint8_t *heap = new uint8_t[b.size() ? b.size() : 1];  // exact size, so ASan sees over-reads
  std::copy(b.begin(), b.end(), heap);
  bool ok = parseConnect(heap, b.size(), ka, cid, pass);
  delete[] heap;
  return ok;
}
int main() {
  String pass; uint16_t ka = 0;
  Bytes ha = connect("MQTT", 4, 0xC2, false, "bblp", "12345678");  // HA / Studio style
  assert(parse(ha, pass, ka) && pass == "12345678" && ka == 60);
  Bytes willed = connect("MQTT", 4, 0xC6, true, "bblp", "87654321");  // password after a will
  assert(parse(willed, pass, ka) && pass == "87654321");
  Bytes v31 = connect("MQIsdp", 3, 0xC2, false, "bblp", "11112222");  // MQTT 3.1
  assert(parse(v31, pass, ka) && pass == "11112222");
  Bytes anon = connect("MQTT", 4, 0x02, false, nullptr, nullptr);  // no credentials
  assert(parse(anon, pass, ka) && pass.empty());
  for (const Bytes *b : {&ha, &willed, &v31, &anon})
    for (size_t n = 0; n < b->size(); n++)
      assert(!parse(Bytes(b->begin(), b->begin() + n), pass, ka));  // truncated: rejected, no over-read
  Bytes lying = ha; lying[1] = 200;  // protocol-name length past the end
  assert(!parse(lying, pass, ka));

  // A client dribbling a topic byte every 900 ms must not hold loop() past the
  // watchdog: the read budget covers the whole packet, not each call.
  WiFiClient slow; slow.gap = 900; pktStart = clockMs = 0;
  uint8_t ch; int got = 0;
  while (got < 128 && MqttBridge::readByte(slow, ch)) got++;
  assert(got < 128 && slow.stopped && clockMs <= MQTT_PACKET_MS + 1000);
  WiFiClient fast; pktStart = clockMs;  // a whole 20 KiB payload at once is fine
  std::vector<uint8_t> big(20480);
  assert(MqttBridge::readBytes(fast, big.data(), big.size()) && !fast.stopped);
}
"""

with tempfile.TemporaryDirectory() as tmp:
    p = Path(tmp)
    (p / "check.cpp").write_text(stub + parser + readers + check)
    subprocess.run(["c++", "-std=c++17", "-fsanitize=address,undefined", str(p / "check.cpp"),
                    "-o", str(p / "check")], check=True)
    subprocess.run([str(p / "check")], check=True)
print("PASS: CONNECT credentials (3.1/3.1.1, will, anonymous), every truncation, per-packet read deadline")
