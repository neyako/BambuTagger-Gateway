"""Offline check that saved 1.0.0 settings survive the config layout change.

Run: python3 tools/check-config.py   (needs a C++ compiler; no hardware)
Compiles the real configLoad()/configSave() against an in-memory EEPROM.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
web = (root / "src/ui/webconfig.h").read_text()
defines = web[web.index("#define EEPROM_SIZE"):web.index('"config outgrew EEPROM");') + 27]
helpers = web[web.index("// 1.0.0 layout:"):web.index("// ── version helpers")]

stub = r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#define ESP32
#include "config.h"
struct {
  uint8_t buf[512];
  void begin(int) {}
  void end() {}
  bool commit() { return true; }
  template <class T> void get(int a, T &t) { memcpy(&t, buf + a, sizeof t); }
  template <class T> void put(int a, const T &t) { memcpy(buf + a, &t, sizeof t); }
} EEPROM;
struct { uint64_t getEfuseMac() { return 0x0000AD2C0B123456ull; } } ESP;
static GatewayConfig _cfg;
"""

check = r"""
int main() {
  // Bytes exactly as 1.0.0 firmware saved them.
  GatewayConfigV1 v1 = {};
  v1.magic = 0x45;
  strcpy(v1.stationSsid, "iotshit");
  strcpy(v1.stationPass, "a-63-character-passphrase-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
  strcpy(v1.printerHost, "10.60.2.50");
  strcpy(v1.printerCode, "40404040");
  strcpy(v1.printerSerial, "01P00A000000000");
  strcpy(v1.gatewaySerial, "22E8BJ5B0C00AD2");
  strcpy(v1.printerModel, "A1 Mini");
  memset(EEPROM.buf, 0xFF, sizeof EEPROM.buf);
  memcpy(EEPROM.buf, &v1, sizeof v1);
  configLoad();
  assert(_cfg.magic == EEPROM_MAGIC);
  assert(!strcmp(_cfg.stationSsid, v1.stationSsid) && !strcmp(_cfg.stationPass, v1.stationPass));
  assert(!strcmp(_cfg.printerHost, v1.printerHost) && !strcmp(_cfg.printerCode, v1.printerCode));
  assert(!strcmp(_cfg.printerSerial, v1.printerSerial) && !strcmp(_cfg.gatewaySerial, v1.gatewaySerial));
  assert(!strcmp(_cfg.printerModel, v1.printerModel));
  assert(_cfg.adminPass[0] == 0);  // 0xFF bytes past the old struct are not a password

  // New layout round-trips a 32-character SSID and the admin password.
  strcpy(_cfg.stationSsid, "0123456789abcdef0123456789abcdef");
  strcpy(_cfg.adminPass, "hunter22");
  configSave();
  memset(&_cfg, 0, sizeof _cfg);
  configLoad();
  assert(strlen(_cfg.stationSsid) == 32 && !strcmp(_cfg.adminPass, "hunter22"));
  assert(!strcmp(_cfg.printerHost, "10.60.2.50"));

  // Blank flash: defaults, no WiFi, no admin password.
  memset(EEPROM.buf, 0xFF, sizeof EEPROM.buf);
  configLoad();
  assert(_cfg.magic == EEPROM_MAGIC && !_cfg.stationSsid[0] && !_cfg.adminPass[0]);
  assert(!strcmp(_cfg.printerCode, PRINTER_CODE_DFLT) && strlen(_cfg.gatewaySerial) == 15);
}
"""

with tempfile.TemporaryDirectory() as tmp:
    p = Path(tmp)
    (p / "WiFi.h").write_text("#pragma once\n")
    (p / "check.cpp").write_text(stub + defines + helpers + check)
    subprocess.run(["c++", "-std=c++17", "-fsanitize=address,undefined", "-I", str(p),
                    "-I", str(root / "src"), str(p / "check.cpp"), "-o", str(p / "check")], check=True)
    subprocess.run([str(p / "check")], check=True)
print("PASS: 1.0.0 settings migrate, 32-char SSID + admin password round-trip, blank defaults")
