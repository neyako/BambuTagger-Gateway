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

stub = r"""
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
using String = std::string;
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
}
"""

with tempfile.TemporaryDirectory() as tmp:
    p = Path(tmp)
    (p / "check.cpp").write_text(stub + parser + check)
    subprocess.run(["c++", "-std=c++17", "-fsanitize=address,undefined", str(p / "check.cpp"),
                    "-o", str(p / "check")], check=True)
    subprocess.run([str(p / "check")], check=True)
print("PASS: CONNECT credentials (3.1/3.1.1, will, anonymous) and every truncation")
