"""Compile the firmware's actual packet writer and check MQTT length boundaries."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/mqtt_bridge.cpp').read_text()
writer = source[source.index('void MqttBridge::sendPublish('):source.index('// wire-level helpers')]
stub = r'''
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <cassert>
using String = std::string;
struct WiFiClient {
  std::vector<uint8_t> bytes;
  size_t write(const uint8_t* p, size_t n) { bytes.insert(bytes.end(), p, p+n); return n; }
};
struct MqttBridge {
  void sendPublish(WiFiClient&, const String&, const uint8_t*, uint32_t, uint8_t);
};
'''
check = r'''
int main() {
  for (unsigned length : {0u, 94u, 127u, 128u, 224u, 255u, 256u, 2048u, 16384u, 20480u}) {
    MqttBridge bridge; WiFiClient client;
    std::string topic = "device/22E8BJ5B0C00AD2/report";
    std::vector<uint8_t> payload(length, 0x5a);
    bridge.sendPublish(client, topic, payload.data(), length, 0);
    auto& b = client.bytes;
    assert(b[0] == 0x30);
    unsigned n = 0, factor = 1, i = 1, d;
    do { d = b.at(i++); n += (d & 127) * factor; factor *= 128; } while (d & 128);
    assert(n == 2 + topic.size() + length);
    assert(b.size() == i + n);
    assert(((b[i] << 8) | b[i+1]) == topic.size());
    assert(std::memcmp(b.data()+i+2, topic.data(), topic.size()) == 0);
    assert(std::memcmp(b.data()+i+2+topic.size(), payload.data(), length) == 0);
  }
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p = Path(tmp)
    (p / 'check.cpp').write_text(stub + writer + check)
    subprocess.run(['c++', '-std=c++17', str(p / 'check.cpp'), '-o', str(p / 'check')], check=True)
    subprocess.run([str(p / 'check')], check=True)
print('PASS: MQTT publish lengths and contents through 20 KiB')
