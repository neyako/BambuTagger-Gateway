# BambuTagger-Gateway

Multi-client bridge for Bambu Lab printers — breaks the 3-connection limit by multiplexing MQTT, camera, and FTPS traffic through a single ESP32-S3 gateway.

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/G8M220JASY)

<p align="center">
<img alt="BambuTagger" src="Pics/status.png" width="400">
</p>

---

## Quick Start

### 1. Flash the Gateway

```bash
# Install PlatformIO
pip install platformio

# Build and flash
pio run -e esp32-s3 -t upload --upload-port /dev/ttyACM0
```

### 2. Connect to the Gateway

On first boot (no WiFi saved), the gateway creates an open WiFi network named **`BambuTagger-Gateway`**. Connect your computer or phone to it, then open **http://192.168.4.1** in a browser.

Once home WiFi is saved, the gateway only uses it and keeps retrying it forever (every 30 s). The setup network is a fallback: it appears only while home WiFi has been down for 2 minutes after boot (10 minutes after a connection loss), stays for 5 minutes, and disappears as soon as home WiFi connects.

### 3. Configure Printer Settings

The web UI is a single page. Under **Settings**, enter your home WiFi and:
- **Hostname / IP** — your printer's LAN IP address
- **Access Code** — 8-digit code from your printer's Settings → Network page
- **Serial Number** — found in Settings → General → Machine Info
- **Printer Model** — your printer model (P1S, X1C, A1, etc.)

Click **Save & Reboot**. After reboot, open the gateway's new IP — **Printer link** should show "connected" (if not, the last connection error is shown under it) and the gateway's LED should turn off. Saved passwords and the access code are never shown again; leave those fields blank to keep them.

### 4. Set Up Bambu Studio

Open Bambu Studio → **Prepare** tab → click the printer icon → **Add Printer**. Select your model, choose **LAN** mode, and enter:
- **IP Address**: the gateway's IP (shown on the web page)
- **Access Code**: your printer's access code

The gateway relays MQTT, camera and file transfer to the real printer. MQTT clients must log in with the printer's access code (user `bblp`), exactly as with a real printer. On port 8883 the gateway presents its own TLS certificate; if a client insists on verifying it, `tools/trust-gateway.sh <gateway-ip>` adds the gateway's CA (`/ca.pem`) to Bambu Studio's trust store.

---

## Features

| Category | Details |
|---|---|
| **MQTT multiplexer** | One upstream MQTT connection to the printer shared by up to 8 local clients, on 1883 (plain) and 8883 (TLS, terminated by the gateway). Clients log in with the printer's access code |
| **Camera / FTPS relay** | Ports 6000 and 990 are end-to-end TLS, so each local client gets its own printer connection, opened only while that client is connected |
| **WiFi setup AP** | Open `BambuTagger-Gateway` access point for first-time setup, and a time-limited fallback when home WiFi is down |
| **Station mode** | Joins your WiFi and retries it forever with backoff; the setup AP is never on while connected |
| **mDNS** | Advertises as `BambuTagger-Gateway.local` with `_bambu._tcp` (TXT: serial, model, version) for Bambu Studio discovery |
| **Auto-resubscribe** | Tracks all client subscriptions; re-subscribes upstream on reconnect |
| **Topic-aware routing** | Correctly handles `#`, `+` wildcards when forwarding printer reports to local clients |
| **Web UI** | One page: status, gateway identity, settings, firmware update. Secrets are never sent back to the browser |
| **Auto-update** | One-click firmware update from the latest GitHub release, over TLS verified against pinned root CAs |
| **Security** | Optional admin password protects the web page (user `admin`), ArduinoOTA and the setup AP (WPA2) |
| **Self-healing** | 30 s loop watchdog, socket reserve so a full gateway can still reach the printer and serve the page, keepalive reaping of dead clients |
| **Diagnostics** | Page shows the last printer-link error, client and socket counts, and the last reset reason with crash backtrace |
| **Fake printer mode** | Responds to Bambu Studio MQTT queries (model, serial, idle status) on any `device/X/request` topic when no real printer is connected; sends printer identity immediately on subscribe for auto-detection |
| **Status LED** | Built-in LED indicates WiFi/MQTT connection state at a glance |

---

## How It Works

Bambu Lab printers accept **3 simultaneous connections**. Once that limit is hit, new clients are rejected. This gateway connects as a **single client per service** and lets many local clients share those connections.

```
┌──────────────────┐      ┌──────────────────────────┐      ┌─────────────────┐
│ Bambu Studio     │─────>│                          │─────>│                 │
│ (TLS 8883)       │──┬──>│  ESP32-S3 Gateway        │──┬──>│  Bambu Lab      │
├──────────────────┤  │   │  ┌────────────────────┐  │  │   │  Printer        │
│ MQTT Client 1    │  │   │  │ MqttBridge         │  │  │   │  ┌───────────┐  │
│ MQTT Client 2    │──┼──>│  │ (plain 1883,       │──┼──┼──>│  │ MQTT 8883 │  │
│ MQTT Client 3    │  │   │  │  upstream TLS)     │  │  │   │  ├───────────┤  │
│      ...         │  │   │  ├────────────────────┤  │  │   │  │ Camera    │  │
│ MQTT Client 8    │  │   │  │ (TLS 8883, same    │  │  │   │  │ Stream    │  │
├──────────────────┤  │   │  │  bridge)           │──┘  │   │  ├───────────┤  │
│ Camera Client    │──┼──>│  ├────────────────────┤     │   │  │ FTPS 990  │  │
│ Camera Client    │  │   │  │ Camera Proxy       │─────┼──>│  └───────────┘  │
│      ...         │  │   │  │ (6000 → printer)   │     │   │                 │
│ Camera Client    │  │   │  ├────────────────────┤     │   │                 │
├──────────────────┤  │   │  │ FTPS Proxy         │─────┼──>│                 │
│ FTP Client       │──┼──>│  │ (990 → printer)    │     │   │                 │
│ FTP Client       │  │   │  └────────────────────┘     │   │                 │
│      ...         │  │   │                             │   │                 │
└──────────────────┘      └─────────────────────────────┘   └─────────────────┘
```

> **Printer sees:** 1 MQTT connection no matter how many local MQTT clients there are, plus one camera or FTPS connection per local client actually using them (those are end-to-end TLS, so they can't be shared).

---

## Hardware

### Bill of Materials

| Component | Notes | Buy |
|---|---|---|
| **ESP32-S3 board** | Seeed XIAO ESP32-S3 (standard, ≥8MB flash) | [Seeed Studio](https://www.seeedstudio.com/) |
| 5V power supply | USB-C (ESP32-S3) | — |

### Specifications

| | ESP32-S3 |
|---|---|
| MCU | Xtensa LX7 dual-core (240 MHz) |
| RAM | 512 KB SRAM + 8 MB PSRAM (TLS sessions live in PSRAM) |
| Flash | 8 MB (QIO 80 MHz) |
| WiFi | 802.11 b/g/n, AP + Station simultaneous |
| Status LED | GPIO21 (active low) |

### LED Status Reference

| Pattern | Meaning |
|---------|---------|
| Solid on (boot) | Starting up |
| Slow blink (500ms) | WiFi SSID configured but not connected |
| Fast blink (100ms) | WiFi up, MQTT connecting to printer |
| Off | Everything connected and working |

---

## Software

### Required Libraries (PlatformIO)

| Library | Version | Purpose |
|---|---|---|
| `PubSubClient` | 2.8 (vendored in `Lib/`) | Upstream MQTT connection to printer |

**ESP32-S3 built-in:** `WiFi`, `WebServer`, `ESPmDNS`, `HTTPClient`, `WiFiClientSecure`, `Update`, `ArduinoOTA`

### Building

```bash
# Install PlatformIO (if not already)
pip install platformio

# Build for ESP32-S3
pio run -e esp32-s3

# Flash ESP32-S3
pio run -e esp32-s3 -t upload --upload-port /dev/ttyACM0

# Monitor
pio device monitor -b 115200
```

### Board Settings (platformio.ini)

| Setting | ESP32-S3 |
|---|---|
| Board | `seeed_xiao_esp32s3` |
| Flash Mode | `qio` |
| Framework | Arduino |
| Monitor Speed | 115200 |
| USB CDC | `false` (USB-to-UART adapter) |
| Platform | `espressif32@7.1.2` (pinned: TLS code uses mbedTLS internals) |

---

## Configuration

Configuration is stored in EEPROM (512 bytes, magic `0x46`) and managed through the web UI. Settings saved by 1.0.0 (magic `0x45`) are migrated on boot.
Compile-time defaults live in `src/config.h`; they apply to a blank device.

### Web UI Pages

| Page | Path | Description |
|---|---|---|
| Home | `/` | Status (WiFi, printer link + last error), gateway identity (IP, name, serial, model), settings form (WiFi, printer host, access code, serial, model), firmware update. Other paths redirect here |

### Compile-time Defaults (`src/config.h`)

| Setting | Default | Description |
|---|---|---|
| `PRINTER_HOST_DFLT` | `""` | Printer hostname or IP |
| `PRINTER_CODE_DFLT` | `12345678` | Printer access code |
| `PRINTER_SERIAL_DFLT` | `22E8BJ5B0000000` | Real printer serial (upstream MQTT) |
| `PRINTER_MODEL_DFLT` | `P1S` | Printer model (used for Bambu Studio handshake); options: P1S, P1P, P2S, X1C, X1E, A1, A1 Mini, A2L |
| `GATEWAY_AP_SSID` | `BambuTagger-Gateway` | Gateway WiFi name |
| `GATEWAY_AP_PASS` | `""` (open) | Setup AP password when no admin password is set |
| `MAX_MQTT_CLIENTS` | `8` | Local MQTT client slots (the socket budget usually binds first, see Limitations) |
| `MQTT_BUFFER_SIZE` | `20480` | MQTT packet buffer (bytes) |
| `SOCKET_RESERVE` | `2` | Sockets kept free for the printer link and the web page |

---

## Client Connection

### MQTT

| Parameter | Port 1883 | Port 8883 |
|---|---|---|
| Host | Gateway IP | Same |
| TLS | None, trusted networks only | Yes, gateway's own certificate (`/ca.pem`) |
| Username / password | `bblp` / printer access code | Same |
| Topics | `device/<serial>/report` and `/request`, with either the gateway serial or the printer serial | Same |
| Best for | Local tools, Home Assistant | Bambu Studio / OrcaSlicer, Panda Status |

Both ports share one upstream connection. Wrong or missing credentials get CONNACK 5 and the connection is closed; nothing reaches the printer before a valid login.

### Camera and FTPS

| Service | Port | Notes |
|---|---|---|
| Camera | `6000` | End-to-end TLS with the printer; one printer connection per viewer |
| FTPS | `990` | Control channel relayed; the passive data connection goes straight to the printer's IP, so the client must be able to reach the printer |

---

## Project Structure

```
BambuTagger-Gateway/
├── platformio.ini              # Build configuration
├── src/
│   ├── config.h                # Settings layout, defaults, ports, limits
│   ├── main.cpp                # WiFi policy, LED, SSDP/mDNS/OTA, watchdog, PSRAM for TLS
│   ├── mqtt_bridge.h/.cpp      # MQTT multiplexer (upstream, local clients, login, fake printer)
│   ├── tls_client.h/.cpp       # TLS server side for port 8883
│   ├── certgen.h/.cpp          # Generates the gateway's TLS certificate chain
│   ├── tcp_proxy.h/.cpp        # Per-client camera/FTPS relay, socket helpers
│   └── ui/
│       ├── webconfig.h         # Web server, auth, EEPROM load/save/migration
│       ├── page_dashboard.h    # The single web page
│       ├── page_saved.h        # Save handler
│       ├── page_ota.h          # GitHub update check and install
│       └── github_ca.h         # Pinned root CAs for GitHub
└── tools/check-*.py            # Offline checks (WiFi policy, config migration, CONNECT parser, proxy, publish)
```

---

## Protocol Details: MQTT Bridge

A small MQTT broker in front of the printer. It:

1. **Connects upstream** to the printer's MQTT broker (port 8883) over **TLS** as a single client using `PubSubClient` (retry every 10 s, backing off to 60 s while the printer is off)
2. **Listens locally** on port 1883 (plain) and 8883 (TLS) and requires the printer's access code
3. **Parses incoming MQTT packets** (CONNECT, SUBSCRIBE, UNSUBSCRIBE, PUBLISH, PINGREQ, DISCONNECT) and responds appropriately
4. **Tracks subscriptions** per local client, including wildcard topics (`#`, `+`)
5. **Routes upstream messages** — when the printer publishes a report, the bridge matches all local client subscriptions and forwards the message
6. **Manages upstream subscriptions** — subscribes/unsubscribes on the printer connection based on aggregated local demand, and restores them after the printer reconnects
7. **Drops dead clients** after 1.5x their keepalive

#### Upstream MQTT

| Parameter | Value |
|---|---|
| Port | 8883 (TLS) |
| Username | `bblp` |
| Password | Printer access code |
| Subscribe | `device/<SERIAL>/report` |
| Publish | `device/<SERIAL>/request` |
| Last will | none (the printer owns its availability) |

---

## Certificates

Port 8883 is terminated by the gateway. On first boot it generates a CA and a device certificate (CN = gateway serial, valid 2026-2049, stored in NVS) and serves the CA at `http://<gateway-ip>/ca.pem`. Clients that don't verify certificates (most MQTT tools, Panda Status) need nothing. For Bambu Studio, run `tools/trust-gateway.sh <gateway-ip>` if it refuses the connection. Firmware 1.0.0's certificate expired on 2027-01-01, so it is regenerated once on upgrade; re-run the script if you had trusted the old one.

---

## Diagnostics

The web page shows:

- **Printer link** with the last connection error (e.g. "printer unreachable", "access code rejected")
- **Local clients**: MQTT, camera, FTPS, and free sockets out of 16
- **Last reset**: power-on, restart, crash, watchdog... After a crash it adds the task, PC and backtrace; decode with `xtensa-esp32s3-elf-addr2line -pfiaC -e .pio/build/esp32-s3/firmware.elf <addresses>` using the same build

Offline checks need only a C++ compiler: `for t in tools/check-*.py; do python3 $t; done` (`check-webui.py` takes the gateway URL instead).

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| Setup AP not visible | Normal once home WiFi is saved: it appears only after WiFi has been down for 2 min (boot) or 10 min (connection loss), for 5 min. With no WiFi saved it is always on |
| MQTT client rejected | It must log in with user `bblp` and the printer's access code. The page counts rejected logins and shows the last IP |
| Printer link "connecting" | Read the error under it. "printer unreachable": printer off or wrong IP. "access code rejected": update the access code in Settings |
| Camera/FTPS refused | Check "sockets free" on the page; the gateway keeps 2 free for the printer link and the page, see Limitations |
| Forgot admin password | Reflash over USB with a full erase |
| `DNS Failed for ...` in serial | `.local` names are resolved via `MDNS.queryHost()` once station WiFi is up (~3 s per attempt); prefer an IP |

---

## Limitations

- **16 sockets total.** This core's lwIP is compiled with 16 sockets for everything; 8 are taken by the listeners, SSDP, OTA and the printer link. With 2 kept in reserve, about 6 are left for clients: an MQTT client uses 1, a camera viewer or FTPS session uses 2 (local + printer). Raising the limit means rebuilding the Arduino libraries (e.g. pioarduino with a custom sdkconfig).
- **FTPS** data connections go directly to the printer's IP (the PASV reply is inside TLS and can't be rewritten).
- **Port 1883** is unencrypted; the access code crosses the network in clear. Use 8883 where possible.
- **ArduinoOTA** has no password until an admin password is set.

---

## Credits & References

- [Bambu-Research-Group](https://github.com/Bambu-Research-Group)
- MQTT client: [PubSubClient](https://github.com/knolleary/pubsubclient)
- PlatformIO: [https://platformio.org](https://platformio.org)

