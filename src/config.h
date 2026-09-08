#pragma once

#ifdef ESP32
#include <WiFi.h>
#else
#include <ESP8266WiFi.h>
#endif

#define VERSION "1.0.0"

// ── Runtime config (stored in EEPROM) ─────────────────────────────────
#pragma pack(push, 1)
struct GatewayConfig {
  uint8_t magic;
  char stationSsid[32];
  char stationPass[64];
  char printerHost[64];
  char printerCode[32];
  char printerSerial[32];
  char gatewaySerial[32];
  char printerModel[24];
};
#pragma pack(pop)

// ── Compile-time defaults ────────────────────────────────────────────
#define PRINTER_HOST_DFLT     ""


#define PRINTER_CODE_DFLT     "12345678"
#define PRINTER_SERIAL_DFLT   "22E8BJ5B0000000"
#define PRINTER_MODEL_DFLT    "P1S"

// Gateway WiFi AP (clients connect here)
#define GATEWAY_AP_SSID       "BambuTagger-Gateway"
#define GATEWAY_AP_PASS       ""
#define GATEWAY_AP_CHANNEL    1
#define GATEWAY_AP_IP         IPAddress(192, 168, 4, 1)
#define GATEWAY_AP_SUBNET     IPAddress(255, 255, 255, 0)

// Ports
#define MQTT_LOCAL_PORT       1883
#define MQTT_PRINTER_PORT     8883
#define FTPS_LOCAL_PORT       990
#define FTPS_PRINTER_PORT     990
#define CAM_LOCAL_PORT        6000
#define CAM_PRINTER_PORT      6000

// Limits
#define MAX_MQTT_CLIENTS      8
#define MAX_TCP_CLIENTS       6
#define MQTT_BUFFER_SIZE      20480
#define UPSTREAM_RETRY_MS     10000UL  // recover quickly after printer-side disconnect

// MQTT topics
#define MQTT_REPORT_TOPIC     "device/%s/report"
#define MQTT_REQUEST_TOPIC    "device/%s/request"
