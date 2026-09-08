#include "mqtt_bridge.h"
#ifdef ESP32
#include <ESPmDNS.h>
#include <mbedtls/base64.h>
#include <Preferences.h>
#include "mbedtls_compat.h"
#include <mbedtls/cipher.h>
#include <aes/esp_aes.h>
#include <aes/esp_aes_gcm.h>

// Derived class to access WiFiClientSecure's protected sslclient member
struct UpstreamClient : public WiFiClientSecure {
  mbedtls_ssl_context *sslCtx() { return &sslclient->ssl_ctx; }
};

// Invalidate AES key-in-hardware flags for all transforms in the given SSL
// context.  The ESP32's single AES hardware register is shared across all
// TLS contexts.  When one context operates it loads its key into the
// shared register and sets its own key_in_hardware=1; the other context's
// flag stays 1 (stale) so it skips the reload on the next operation and
// silently uses the wrong key.
//
// Instead of loading a key (which leaves key_in_hardware=1, immediately
// stale), we clear key_in_hardware to 0.  This forces the next AES
// operation to reload from the stored context, regardless of which key is
// currently in the hardware register.
//
// IMPORTANT: only AES-based ciphers (GCM, CCM, CBC, etc.) go through the
// shared hardware register.  Non-AES ciphers (ChaCha20-Poly1305) are
// skipped to avoid corrupting their different cipher_ctx struct layout.
static void invalidateAesKeys(mbedtls_ssl_context *ssl) {
  if (!ssl) return;
  for (auto *t : { ssl->transform_in, ssl->transform_out }) {
    if (!t) continue;
    for (auto *ctx : { &t->cipher_ctx_dec, &t->cipher_ctx_enc }) {
      if (!ctx->cipher_info || !ctx->cipher_ctx) continue;
      mbedtls_cipher_type_t ct = ctx->cipher_info->type;
      // Only AES-based ciphers use the shared hardware AES key register.
      // Non-AES ciphers (e.g. ChaCha20-Poly1305) have a different
      // cipher_ctx layout and must not be cast to esp_gcm_context.
      if (ct < MBEDTLS_CIPHER_AES_128_ECB || ct > MBEDTLS_CIPHER_AES_256_KWP)
        continue;
      if (ctx->cipher_info->mode == MBEDTLS_MODE_GCM ||
          ctx->cipher_info->mode == MBEDTLS_MODE_CCM) {
        // GCM/CCM: cipher_ctx is esp_gcm_context with embedded aes_ctx
        esp_gcm_context *gcm = (esp_gcm_context *)ctx->cipher_ctx;
        gcm->aes_ctx.key_in_hardware = 0;
      } else {
        // Non-AEAD AES (CBC, ECB, CTR, etc.): cipher_ctx is esp_aes_context
        esp_aes_context *aes = (esp_aes_context *)ctx->cipher_ctx;
        aes->key_in_hardware = 0;
      }
    }
  }
}
#endif

static char reportTopic[64];
static char requestTopic[64];
static char localReportTopic[64];
static char localRequestTopic[64];

// DER to PEM conversion using mbedTLS base64
static void derToPem(const uint8_t *der, size_t derLen, char *pem, size_t pemSize) {
  size_t olen = 0;
  mbedtls_base64_encode(NULL, 0, &olen, der, derLen);
  size_t b64Len = (olen / 64) * 65 + olen + 128;
  if (b64Len > pemSize) { pem[0] = 0; return; }
  size_t outLen;
  mbedtls_base64_encode((unsigned char *)pem, pemSize, &outLen, der, derLen);
  // Re-wrap with 64-char lines and PEM headers
  char tmp[3072];
  size_t pos = 0;
  pos += snprintf(tmp + pos, sizeof(tmp) - pos, "-----BEGIN CERTIFICATE-----\n");
  for (size_t off = 0; off < outLen; off += 64) {
    size_t chunk = (outLen - off > 64) ? 64 : (outLen - off);
    memcpy(tmp + pos, pem + off, chunk); pos += chunk;
    tmp[pos++] = '\n';
  }
  pos += snprintf(tmp + pos, sizeof(tmp) - pos, "-----END CERTIFICATE-----\n");
  tmp[pos] = 0;
  memcpy(pem, tmp, pos + 1);
}

MqttBridge::MqttBridge()
  : _localServer(MQTT_LOCAL_PORT),
    _tlsServer(MQTT_PRINTER_PORT),
    _lastReconnect(0), _cfg(nullptr) {
  for (int i = 0; i < MAX_MQTT_CLIENTS; i++) {
    _clients[i].active = false;
    _clients[i].isTls = false;
    _clients[i].client = nullptr;
    _clients[i].subCount = 0;
    _clients[i].lastPid = 0;
    _clients[i].lastActivity = 0;
  }

  snprintf(reportTopic, sizeof(reportTopic), MQTT_REPORT_TOPIC, PRINTER_SERIAL_DFLT);
  snprintf(requestTopic, sizeof(requestTopic), MQTT_REQUEST_TOPIC, PRINTER_SERIAL_DFLT);
  snprintf(localReportTopic, sizeof(localReportTopic), MQTT_REPORT_TOPIC, PRINTER_SERIAL_DFLT);
  snprintf(localRequestTopic, sizeof(localRequestTopic), MQTT_REQUEST_TOPIC, PRINTER_SERIAL_DFLT);

  _pubsub.setBufferSize(MQTT_BUFFER_SIZE);
  _pubsub.setSocketTimeout(3);  // seconds; MQTT reads share the web UI loop
  _pubsub.setCallback([this](char *t, uint8_t *p, unsigned int l) {
    onUpstreamMessage(t, p, l);
  });
}

void MqttBridge::rebind() {
  _localServer.close();
  _localServer.begin();
  _localServer.setNoDelay(true);
  WiFiClient drain = _localServer.accept();
  _tlsServer.close();
  _tlsServer.begin();
  _tlsServer.setNoDelay(true);
  WiFiClient drainTls = _tlsServer.accept();
  Serial.println("MQTT: servers rebound (drained)");
}

void MqttBridge::begin(GatewayConfig *cfg) {
  _cfg = cfg;

  // update topic buffers from runtime config
  snprintf(reportTopic, sizeof(reportTopic), MQTT_REPORT_TOPIC, _cfg->printerSerial);
  snprintf(requestTopic, sizeof(requestTopic), MQTT_REQUEST_TOPIC, _cfg->printerSerial);
  snprintf(localReportTopic, sizeof(localReportTopic), MQTT_REPORT_TOPIC, _cfg->gatewaySerial);
  snprintf(localRequestTopic, sizeof(localRequestTopic), MQTT_REQUEST_TOPIC, _cfg->gatewaySerial);

  Serial.printf("MQTT: topics report='%s' request='%s' localReport='%s' localRequest='%s'\n",
                reportTopic, requestTopic, localReportTopic, localRequestTopic);

  _localServer.begin();
  _localServer.setNoDelay(true);

  _tlsServer.begin();
  _tlsServer.setNoDelay(true);

  // Generate or load self-signed device cert for TLS server (CA=FALSE, EKU+SAN)
#ifdef ESP32
  {
    // Free up old NVS namespaces
    // Clear legacy NVS namespaces — intentionally excludes "mqttg40" (current)
    for (const char *ns : {"mqttgate", "mqttg2", "mqttg3", "mqttg4", "mqttg5", "mqttg6", "mqttg7", "mqttg8", "mqttg9", "mqttg10", "mqttg11", "mqttg12", "mqttg13", "mqttg14", "mqttg15", "mqttg16", "mqttg17", "mqttg18", "mqttg19", "mqttg20", "mqttg21", "mqttg22", "mqttg23", "mqttg24", "mqttg25", "mqttg26", "mqttg27", "mqttg28", "mqttg29", "mqttg30", "mqttg31", "mqttg32", "mqttg33", "mqttg34", "mqttg35", "mqttg36", "mqttg37", "mqttg38", "mqttg39"}) {
      Preferences oldPrefs;
      oldPrefs.begin(ns, false);
      oldPrefs.clear();
      oldPrefs.end();
    }

    Preferences prefs;
            prefs.begin("mqttg40", false);
    _certLen = prefs.getBytes("certDer", _certDer, sizeof(_certDer));
    _keyLen = prefs.getBytes("keyDer", _keyDer, sizeof(_keyDer));
    _caLen = prefs.getBytes("caDer", _caDer, sizeof(_caDer));
    bool loaded = (_certLen > 0 && _keyLen > 0 && _caLen > 0);
    prefs.end();

    if (!loaded) {
      // Wait for STA IP if station mode is configured
      if (strlen(_cfg->stationSsid) > 0) {
        unsigned long start = millis();
        while (!WiFi.isConnected() && millis() - start < 15000) delay(100);
      }
      IPAddress localIP = WiFi.isConnected() ? WiFi.localIP() : WiFi.softAPIP();
      Serial.printf("MQTT: generating cert with IP %s\n", localIP.toString().c_str());
      uint8_t ipBytes[4] = { localIP[0], localIP[1], localIP[2], localIP[3] };
    if (!generateCertChain(_cfg->printerSerial, _certDer, &_certLen,
                              _keyDer, &_keyLen,
                              _caDer, &_caLen,
                              ipBytes)) {
        Serial.println("MQTT: cert generation failed!");
      } else {
        prefs.begin("mqttg40", false);
        prefs.putBytes("certDer", _certDer, _certLen);
        prefs.putBytes("keyDer", _keyDer, _keyLen);
        prefs.putBytes("caDer", _caDer, _caLen);
        prefs.end();
    Serial.printf("MQTT: generated & saved cert chain (%d bytes) + CA (%d bytes) SN=%s\n",
                  _certLen, _caLen, _cfg->gatewaySerial);
      }
    } else {
      Serial.printf("MQTT: loaded cert chain from flash (%d bytes) + CA (%d bytes) SN=%s\n",
                    _certLen, _caLen, _cfg->gatewaySerial);
    }
    derToPem(_certDer, _certLen, _certPem, sizeof(_certPem));
    derToPem(_caDer, _caLen, _caPem, sizeof(_caPem));
    Serial.printf("MQTT: CA PEM follows\n%s\nMQTT: CA PEM END\n", _caPem);
    Serial.printf("MQTT: Server cert PEM follows\n%s\nMQTT: Server cert PEM END\n", _certPem);
  }
#endif

  Serial.println("MQTT: local server ready on port 1883");
}

const char *MqttBridge::getTlsCert() {
#ifdef ESP32
  if (_caPem[0]) return _caPem;
#endif
  return "-----BEGIN CERTIFICATE-----\n"
         "-----END CERTIFICATE-----\n";
}

void MqttBridge::loop() {
  if (!_upTcp || !_pubsub.connected()) {
    // Attempt upstream MQTT connection (only if printer is configured and WiFi is up)
    if (WiFi.isConnected() && _cfg && strlen(_cfg->printerHost) > 0) {
      unsigned long now = millis();
      if (now - _lastReconnect > UPSTREAM_RETRY_MS) {
        if (connectUpstream()) {
#ifdef ESP32
          invalidateAesKeys(_upSslCtx);
#endif
          char sn[64];
          strncpy(sn, _cfg->printerSerial, sizeof(sn) - 1);
          sn[sizeof(sn) - 1] = 0;
          char topic[64];
          snprintf(topic, sizeof(topic), "device/%s/report", sn);
          _pubsub.subscribe(topic, 0);
        }
        _lastReconnect = millis();  // allow a full retry delay after blocking I/O
      }
    }
  } else {
#ifdef ESP32
    // Clear upstream AES key in hardware before any upstream TLS operation
    invalidateAesKeys(_upSslCtx);
#endif
    _pubsub.loop();
  }

  int ac;
  while ((ac = acceptClient()) >= 0) {}
  if (ac < -1) Serial.printf("MQTT: acceptClient returned %d\n", ac);

  static unsigned long lastDiag = 0;
  unsigned long now = millis();
  int active = 0;
  for (int i = 0; i < MAX_MQTT_CLIENTS; i++) {
    if (!_clients[i].active) continue;
    active++;
  }
  if (active > 0 && now - lastDiag > 10000) {
    lastDiag = now;
    Serial.printf("MQTT: %d active client(s) upConn=%d\n", active, _pubsub.connected());
  }

#ifdef ESP32
  // Clear downstream AES keys so each client reloads after upstream ops
  for (int i = 0; i < MAX_MQTT_CLIENTS; i++) {
    if (!_clients[i].active) continue;
    if (_clients[i].isTls) {
      TlsWiFiClient *tls = (TlsWiFiClient *)_clients[i].client;
      tls->clearKeyInHardware();
    }
  }
#endif

  for (int i = 0; i < MAX_MQTT_CLIENTS; i++) {
    if (!_clients[i].active) continue;

    // Continue TLS handshake for pending clients
    if (_clients[i].isTls) {
      TlsWiFiClient *tls = (TlsWiFiClient *)_clients[i].client;
      if (!tls->handshakeDone()) {
        int hs = tls->continueHandshake();
        if (hs == 0) continue;   // still handshaking
        if (hs < 0) {            // handshake failed
          Serial.printf("MQTT: TLS handshake failed for client %d\n", i);
          disconnectClient(i);
          continue;
        }
        Serial.printf("MQTT: TLS handshake done for client %d\n", i);
        // Fall through to handle MQTT once handshake completes
      }
    }

    if (!_clients[i].client->connected()) {
      disconnectClient(i);
      continue;
    }
    handleClient(i);
  }
}

bool MqttBridge::isConnected() {
  return _upTcp && _pubsub.connected();
}

MqttStatus MqttBridge::getStatus() {
  if (_upTcp && _pubsub.connected()) return MQTT_UP;
  // Printer is on LAN — unreachable when station WiFi is down
  if (!WiFi.isConnected()) return MQTT_IDLE;
  if (_cfg && strlen(_cfg->printerHost) > 0) {
    return MQTT_TRYING;
  }
  return MQTT_IDLE;
}

// ------------------------------------------------------------------
// upstream
// ------------------------------------------------------------------
bool MqttBridge::connectUpstream() {
  if (!_cfg) return false;
  if (!_upTcp) {
#ifdef ESP32
    _upTcp = new UpstreamClient();
#else
    _upTcp = new WiFiClientSecure();
#endif
    _pubsub.setClient(*_upTcp);
  }
  // Keep PubSubClient's client reference valid and reset its old MQTT state
  // before opening a new TLS session.
  _upTcp->stop();
  _pubsub.connected();
#ifdef ESP32
  _upSslCtx = nullptr;
#endif
  _upTcp->setInsecure();
  // ponytail: synchronous connects pause HTTP; use async I/O if these bounds are too slow.
#ifdef ESP32
  _upTcp->setTimeout(3);  // ESP32 WiFiClientSecure uses seconds, not milliseconds
  _upTcp->setHandshakeTimeout(5);  // independent TLS deadline, in seconds
#else
  _upTcp->setTimeout(3000);
#endif

  const char *host = _cfg->printerHost;
  char resolved[64];
#ifdef ESP32
  size_t hl = strlen(host);
  if (hl > 6 && strcmp(host + hl - 6, ".local") == 0 && WiFi.isConnected()) {
    char name[64];
    size_t nl = hl - 6;
    memcpy(name, host, nl);
    name[nl] = '\0';
    IPAddress ip = MDNS.queryHost(name, 3000);
    if (ip != IPAddress(0, 0, 0, 0)) {
      snprintf(resolved, sizeof(resolved), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
      host = resolved;
      Serial.printf("mqtt: resolved %s.local -> %s\n", name, host);
    }
  }
#endif
  if (!_upTcp->connect(host, MQTT_PRINTER_PORT)) {
#ifdef ESP32
    char error[160] = {};
    int code = _upTcp->lastError(error, sizeof(error));
    Serial.printf("MQTT: TCP/TLS connection failed (%d): %s; retry in 60s\n", code, error);
#else
    Serial.println("MQTT: TCP/TLS connection failed; retry in 60s");
#endif
    _upTcp->stop();
    return false;
  }
#ifdef ESP32
  _upSslCtx = static_cast<UpstreamClient*>(_upTcp)->sslCtx();
  const char *upCs = mbedtls_ssl_get_ciphersuite(_upSslCtx);
  Serial.printf("TLS: upstream cipher=%s\n", upCs ? upCs : "?");
#endif
  char clientId[48];
  // Identify this gateway, not the shared printer, to avoid client ID collisions.
  snprintf(clientId, sizeof(clientId), "BambuTagger-%s", WiFi.macAddress().c_str());

  // The printer owns its availability topic. No retained status or last will.
  bool ok = _pubsub.connect(clientId, "bblp", _cfg->printerCode);
  Serial.printf("MQTT: upstream CONNECT result=%d (0=connected, -4=timeout, 4=credentials, 5=unauthorized)\n",
                _pubsub.state());
  return ok;
}

static bool topicMatchesSub(const String &topic, const String &sub) {
  int ti = 0, si = 0;
  int tl = topic.length(), sl = sub.length();

  while (ti < tl && si < sl) {
    if (sub[si] == '#') return true;
    if (sub[si] == '+') {
      // skip this level in topic
      while (ti < tl && topic[ti] != '/') ti++;
      si++;
      // skip '/' delimiter in both
      if (ti < tl && topic[ti] == '/') ti++;
      if (si < sl && sub[si] == '/') si++;
      continue;
    }
    if (topic[ti] != sub[si]) return false;
    ti++;
    si++;
  }

  // both exhausted
  if (ti == tl && si == sl) return true;
  // sub has trailing "/#"  (e.g. "device/#")
  if (si <= sl - 2 && sub.substring(si) == "/#") return true;
  // sub has bare "#" at current position
  if (si < sl && sub[si] == '#') return true;

  return false;
}

void MqttBridge::onUpstreamMessage(char *topic, uint8_t *payload, unsigned int len) {
  // Translate upstream topic (printer serial) to local topic (gateway serial)
  String t = topic;
  if (_cfg && strcmp(_cfg->printerSerial, _cfg->gatewaySerial) != 0) {
    String prefix = String("device/") + _cfg->printerSerial;
    if (t.startsWith(prefix)) {
      t = String("device/") + _cfg->gatewaySerial + t.substring(prefix.length());
    }
  }
  for (int i = 0; i < MAX_MQTT_CLIENTS; i++) {
    if (!_clients[i].active) continue;
    for (uint8_t s = 0; s < _clients[i].subCount; s++) {
      // Clients may subscribe using either gateway or physical-printer
      // serial. Preserve each client's requested topic in forwarded reports.
      if (topicMatchesSub(topic, _clients[i].subs[s].topic) ||
          topicMatchesSub(t, _clients[i].subs[s].topic)) {
#ifdef ESP32
        // AES hardware is shared by all TLS clients; reload this client's key
        // after the previous client's encrypted write.
        if (_clients[i].isTls)
          ((TlsWiFiClient *)_clients[i].client)->clearKeyInHardware();
#endif
        const String &filter = _clients[i].subs[s].topic;
        String outTopic = topicMatchesSub(topic, filter) ? String(topic) : t;
        sendPublish(*_clients[i].client, outTopic, payload, len, 0);
        break;
      }
    }
  }
}

// ------------------------------------------------------------------
// downstream client management
// ------------------------------------------------------------------
int MqttBridge::acceptClient() {
  // Try TLS server (port 8883)
  {
    WiFiClient raw = _tlsServer.accept();
    if (raw) {
      if (!raw.connected()) { raw.stop(); return -1; }
      raw.setNoDelay(true);
      Serial.printf("MQTT: accept from %s:%d -> %s:%d\n",
                    raw.remoteIP().toString().c_str(), raw.remotePort(),
                    raw.localIP().toString().c_str(), raw.localPort());

      // Wait for first byte — need to delay slightly to let Bambu Studio
      // send the TLS ClientHello before we try to do TLS handshake.
      int peeked = -1;
      for (int w = 0; w < 20; w++) {
        if (!raw.connected()) { raw.stop(); return -1; }
        if (raw.available() > 0) {
          peeked = raw.read();
          break;
        }
        delay(5);
      }

      if (peeked < 0) {
        Serial.printf("MQTT: probe from %s:%d (closing)\n",
                      raw.remoteIP().toString().c_str(), raw.remotePort());
        raw.stop();
        return -1;
      }

      // Client sent data — restore the byte and do TLS
      WiFiClient *rawTcp = new WiFiClient(raw);
      TlsWiFiClient *tls = new TlsWiFiClient();
      if (!tls->beginDer(rawTcp, _certDer, _certLen, _keyDer, _keyLen)) {
        Serial.println("MQTT: TLS beginDer failed");
        delete tls;
        return -1;
      }

      // Feed the peeked byte back through the TLS client
      tls->feedData((uint8_t)peeked);

      for (int i = 0; i < MAX_MQTT_CLIENTS; i++) {
        if (!_clients[i].active) {
          _clients[i].client = tls;
          _clients[i].active = true;
          _clients[i].isTls = true;
          _clients[i].subCount = 0;
          _clients[i].lastPid = 0;
          _clients[i].lastActivity = millis();
          Serial.printf("MQTT: TLS client %d from %s\n", i, raw.remoteIP().toString().c_str());
          return i;
        }
      }
      delete tls;
      return -2;
    }
  }

  // Try plain server (port 1883)
  {
    WiFiClient plain = _localServer.accept();
    if (!plain) return -1;

    WiFiClient *c = new WiFiClient(plain);
    if (!c || !c->connected()) {
      delete c;
      return -1;
    }
    c->setNoDelay(true);
    Serial.printf("MQTT: plain client from %s\n", c->remoteIP().toString().c_str());

    for (int i = 0; i < MAX_MQTT_CLIENTS; i++) {
      if (!_clients[i].active) {
        _clients[i].client = c;
        _clients[i].active = true;
        _clients[i].isTls = false;
        _clients[i].subCount = 0;
        _clients[i].lastPid = 0;
        _clients[i].lastActivity = millis();
        return i;
      }
    }
    c->stop();
    delete c;
    return -2;
  }
}

void MqttBridge::disconnectClient(int idx) {
  if (!_clients[idx].active) return;
  _clients[idx].subCount = 0;
  if (_clients[idx].client) {
    _clients[idx].client->stop();
    delete _clients[idx].client;
    _clients[idx].client = nullptr;
  }
  _clients[idx].active = false;
  _clients[idx].isTls = false;
}

// ------------------------------------------------------------------
// downstream MQTT packet handling
// ------------------------------------------------------------------
void MqttBridge::handleClient(int idx) {
  MqttClientCtx &cl = _clients[idx];
  WiFiClient &c = *cl.client;

#ifdef ESP32
  // Clear upstream AES keys so publish/subscribe/unsubscribe use correct keys
  invalidateAesKeys(_upSslCtx);
#endif

  uint8_t header;
  int headerRead = c.read();
  if (headerRead < 0) { return; }
  header = (uint8_t)headerRead;
  cl.lastActivity = millis();

  // Log first MQTT packet type from each client
  static bool logged[8] = {};
  if (idx >= 0 && idx < 8 && !logged[idx]) {
    logged[idx] = true;
    uint8_t type = (header >> 4) & 0x0F;
    Serial.printf("MQTT: client %d first pkt type=%d\n", idx, type);
  }

  uint8_t type = (header >> 4) & 0x0F;
  uint32_t remaining = 0;
  if (!readRemainingLength(c, remaining)) return;

  switch (type) {
    case 1: { // CONNECT
      if (remaining > 4096) { disconnectClient(idx); return; }
      // Dump CONNECT variable header + payload
      uint8_t *connBuf = new uint8_t[remaining + 1];
      size_t connPos = 0;
      while (remaining > 0) {
        uint32_t chunk = (remaining > 32) ? 32 : remaining;
        if (!readBytes(c, connBuf + connPos, chunk)) { delete[] connBuf; return; }
        connPos += chunk;
        remaining -= chunk;
      }
      connBuf[connPos] = 0;

      // Parse CONNECT fields
      if (connPos >= 6) {
        int off = 0;
        uint16_t pnLen = (connBuf[off] << 8) | connBuf[off+1]; off += 2;
        String protoName = String((char *)(connBuf + off), pnLen); off += pnLen;
        uint8_t level = connBuf[off++];
        uint8_t flags = connBuf[off++];  // bit 0: reserved, 1: clean, 2: will, 3: willQos1, 4: willQos2, 5: willRetain, 6: pwd, 7: user
        uint16_t keepalive = (connBuf[off] << 8) | connBuf[off+1]; off += 2;
        // Client ID
        uint16_t cidLen = connPos > off + 1 ? (connBuf[off] << 8) | connBuf[off+1] : 0; off += 2;
        String clientId;
        if (cidLen > 0 && off + cidLen <= connPos) {
          clientId = String((char *)(connBuf + off), cidLen); off += cidLen;
        }
        Serial.printf("MQTT: CONNECT proto=%s level=%u flags=0x%02x keepalive=%u cid='%s'\n",
                      protoName.c_str(), level, flags, keepalive, clientId.c_str());
      }
      delete[] connBuf;

      size_t wret = sendConnAck(c, false, 0);
      Serial.printf("MQTT: ConnAck write returned %u bytes\n", (unsigned)wret);
      // Flush pending TLS writes so the client receives ConnAck promptly
      if (cl.isTls) {
        TlsWiFiClient *tls = (TlsWiFiClient *)&c;
        tls->flushWrites();
        delay(100);
        // Do not probe for client data here. HA deliberately waits for CONNACK;
        // a speculative TLS read reports WANT_READ as a disconnect in this client.
        Serial.println("MQTT: ConnAck flushed");
      }
      break;
    }

    case 3: { // PUBLISH
      uint8_t tlenBuf[2];
      if (!readBytes(c, tlenBuf, 2)) return;
      uint16_t tlen = (tlenBuf[0] << 8) | tlenBuf[1];
      char topic[128];
      uint16_t rlen = (tlen < sizeof(topic) - 1) ? tlen : sizeof(topic) - 1;
      for (uint16_t i = 0; i < rlen; i++) {
        uint8_t ch;
        if (!readByte(c, ch)) return;
        topic[i] = (char)ch;
      }
      topic[rlen] = 0;
      for (uint16_t i = rlen; i < tlen; i++) {
        uint8_t ch;
        if (!readByte(c, ch)) return;
      }

      uint8_t qos = (header >> 1) & 0x03;
      uint16_t pid = 0;
      if (qos > 0) {
        uint8_t pidBuf[2];
        if (!readBytes(c, pidBuf, 2)) return;
        pid = (pidBuf[0] << 8) | pidBuf[1];
      }

      uint32_t overhead = 2 + tlen + (qos > 0 ? 2 : 0);
      if (remaining < overhead || remaining - overhead > MQTT_BUFFER_SIZE) {
        Serial.printf("MQTT: reject invalid PUBLISH len=%lu topicLen=%u\n",
                      (unsigned long)remaining, tlen);
        disconnectClient(idx);
        return;
      }
      uint32_t payloadLen = remaining - overhead;
      uint32_t toRead = payloadLen;
      uint8_t *payload = new uint8_t[toRead ? toRead : 1];
      if (toRead > 0 && !readBytes(c, payload, toRead)) { delete[] payload; return; }

      // Fake printer responses when no real printer is connected.
      if (!_pubsub.connected()) {
        int tlen = strlen(topic);
        static const char reqSuffix[] = "/request";
        static const int slen = sizeof(reqSuffix) - 1;
        if (tlen > slen && strcmp(topic + tlen - slen, reqSuffix) == 0) {
          char respTopic[128];
          memcpy(respTopic, topic, tlen - slen);
          memcpy(respTopic + tlen - slen, "/report", 7);
          respTopic[tlen - slen + 7] = '\0';

          if (strstr((const char *)payload, "\"get_version\"")) {
            char seq[16] = "0";
            const char *s = strstr((const char *)payload, "\"sequence_id\"");
            if (s) {
              s = strchr(s, ':');
              if (s) {
                s++;
                while (*s == ' ' || *s == '\"') s++;
                const char *e = s;
                while (*e && *e != '\"' && *e != ' ' && *e != ',' && *e != '}') e++;
                size_t slen = e - s;
                if (slen > 0 && slen < sizeof(seq)) {
                  memcpy(seq, s, slen);
                  seq[slen] = 0;
                }
              }
            }

            char resp[384];
            const char *model = _cfg ? _cfg->printerModel : PRINTER_MODEL_DFLT;
            const char *serial = _cfg ? _cfg->printerSerial : PRINTER_SERIAL_DFLT;
            int rlen2 = snprintf(resp, sizeof(resp),
              "{\"info\":{\"command\":\"get_version\",\"sequence_id\":\"%s\","
              "\"version\":\"" VERSION "\",\"module\":\"%s\",\"model\":\"%s\","
              "\"serial\":\"%s\",\"online\":\"true\"}}",
              seq, model, model, serial);
            sendPublish(c, respTopic, (uint8_t *)resp, rlen2, 0);
          }

          if (strstr((const char *)payload, "\"pushall\"")) {
            char resp[512];
            const char *serial = _cfg ? _cfg->printerSerial : PRINTER_SERIAL_DFLT;
            int rlen2 = snprintf(resp, sizeof(resp),
              "{\"print\":{\"command\":\"pushall\",\"sequence_id\":\"0\","
              "\"serial\":\"%s\","
              "\"gcode_state\":\"IDLE\",\"subtask_name\":\"\",\"project_id\":\"\","
              "\"gcode_file\":\"\",\"mc_percent\":0,\"mc_remaining_time\":0,"
              "\"layer_num\":0,\"total_layer_num\":0,"
              "\"nozzle_temper\":25.1,\"nozzle_target_temper\":0,"
              "\"bed_temper\":25.2,\"bed_target_temper\":0,"
              "\"chamber_temper\":25.0,\"spd_mag\":100,"
              "\"wifi_signal\":\"-50dBm\",\"sdcard\":true}}",
              serial);
            sendPublish(c, respTopic, (uint8_t *)resp, rlen2, 0);
          }
        }
      }

      bool pubOk = _pubsub.publish(requestTopic, payload, toRead, false);
      Serial.printf("MQTT: pub topic='%s' len=%u ok=%d\n",
                    requestTopic, toRead, pubOk);

      if (qos == 1) {
        uint8_t ackBuf[2] = {(uint8_t)(pid >> 8), (uint8_t)(pid & 0xFF)};
        c.write((uint8_t)0x40);
        writeRemainingLength(c, 2);
        c.write(ackBuf, 2);
      }
      delete[] payload;
      break;
    }

    case 8: { // SUBSCRIBE
      uint8_t pidBuf[2];
      if (!readBytes(c, pidBuf, 2)) return;
      uint16_t pid = (pidBuf[0] << 8) | pidBuf[1];

      uint8_t topicCount = 0;
      uint32_t rp = remaining - 2;
      while (rp > 0) {
        uint8_t tlenBuf[2];
        if (!readBytes(c, tlenBuf, 2)) return;
        uint16_t tlen = (tlenBuf[0] << 8) | tlenBuf[1];
        String subTopic;
        subTopic.reserve(tlen + 1);
        for (uint16_t i = 0; i < tlen; i++) {
          uint8_t ch;
          if (!readByte(c, ch)) return;
          subTopic += (char)ch;
        }
        uint8_t subOpts;
        if (!readByte(c, subOpts)) return;
        rp -= 3 + tlen;
        topicCount++;

        if (cl.subCount < MAX_MQTT_CLIENTS) {
          cl.subs[cl.subCount].topic = subTopic;
          cl.subs[cl.subCount].qos = subOpts & 0x03;
          cl.subCount++;
        }
        // Translate gateway serial to printer serial in the subscription topic
        String upTopic = subTopic;
        if (_cfg && strcmp(_cfg->gatewaySerial, _cfg->printerSerial) != 0) {
          String pfx = String("device/") + _cfg->gatewaySerial;
          if (upTopic.startsWith(pfx))
            upTopic = String("device/") + _cfg->printerSerial + upTopic.substring(pfx.length());
        }
        bool subOk = _pubsub.subscribe(upTopic.c_str(), subOpts & 0x03);
        Serial.printf("MQTT: sub topic='%s' -> up='%s' ok=%d\n",
                      subTopic.c_str(), upTopic.c_str(), subOk);
      }

      sendSubAck(c, pid, topicCount);

      // In fake printer mode (upstream disconnected), send printer identity
      // immediately after subscribe so Bambu Studio auto-detects serial/model.
      if (!_pubsub.connected() && _cfg) {
        static const char reportPrefix[] = "device/";
        String r1 = String(reportPrefix) + _cfg->gatewaySerial + "/report";
        String r2 = String(reportPrefix) + _cfg->printerSerial + "/report";
        for (uint8_t s = 0; s < cl.subCount; s++) {
          String rt;
          if (topicMatchesSub(r1, cl.subs[s].topic))
            rt = r1;
          else if (topicMatchesSub(r2, cl.subs[s].topic))
            rt = r2;
          if (rt.length() > 0) {
            char buf[384];
            int n = snprintf(buf, sizeof(buf),
              "{\"info\":{\"command\":\"get_version\",\"sequence_id\":\"0\","
              "\"version\":\"" VERSION "\",\"module\":\"%s\",\"model\":\"%s\","
              "\"serial\":\"%s\",\"online\":\"true\"}}",
              _cfg->printerModel, _cfg->printerModel, _cfg->printerSerial);
            sendPublish(c, rt, (uint8_t *)buf, n, 0);
            break;
          }
        }
      }

      break;
    }

    case 10: { // UNSUBSCRIBE
      uint8_t pidBuf[2];
      if (!readBytes(c, pidBuf, 2)) return;
      uint16_t pid = (pidBuf[0] << 8) | pidBuf[1];

      uint32_t rp = remaining - 2;
      while (rp > 0) {
        uint8_t tlenBuf[2];
        if (!readBytes(c, tlenBuf, 2)) return;
        uint16_t tlen = (tlenBuf[0] << 8) | tlenBuf[1];
        String unsubTopic;
        unsubTopic.reserve(tlen + 1);
        for (uint16_t i = 0; i < tlen; i++) {
          uint8_t ch;
          if (!readByte(c, ch)) return;
          unsubTopic += (char)ch;
        }
        rp -= 2 + tlen;

        for (uint8_t s = 0; s < cl.subCount; s++) {
          if (cl.subs[s].topic == unsubTopic) {
            for (uint8_t k = s; k < cl.subCount - 1; k++) cl.subs[k] = cl.subs[k + 1];
            cl.subCount--;
            break;
          }
        }

        bool stillWanted = false;
        for (int j = 0; j < MAX_MQTT_CLIENTS && !stillWanted; j++) {
          if (!_clients[j].active || j == idx) continue;
          for (uint8_t s = 0; s < _clients[j].subCount; s++) {
            if (_clients[j].subs[s].topic == unsubTopic) {
              stillWanted = true;
              break;
            }
          }
        }
        if (!stillWanted) {
          String upTopic = unsubTopic;
          if (_cfg && strcmp(_cfg->gatewaySerial, _cfg->printerSerial) != 0) {
            String pfx = String("device/") + _cfg->gatewaySerial;
            if (upTopic.startsWith(pfx))
              upTopic = String("device/") + _cfg->printerSerial + upTopic.substring(pfx.length());
          }
          _pubsub.unsubscribe(upTopic.c_str());
        }
      }

      sendUnsubAck(c, pid);
      break;
    }

    case 12: // PINGREQ
      sendPingResp(c);
      break;

    case 14: // DISCONNECT
      disconnectClient(idx);
      break;

    default:
      uint8_t buf[32];
      while (remaining > 0) {
        uint32_t chunk = (remaining > sizeof(buf)) ? sizeof(buf) : remaining;
        if (!readBytes(c, buf, chunk)) return;
        remaining -= chunk;
      }
      break;
  }
}

// ------------------------------------------------------------------
// MQTT packet writers
// ------------------------------------------------------------------
size_t MqttBridge::sendConnAck(WiFiClient &c, bool sp, uint8_t rc) {
  uint8_t pkt[4] = {0x20, 0x02, (uint8_t)(sp ? 1 : 0), rc};
  return c.write(pkt, 4);
}

void MqttBridge::sendSubAck(WiFiClient &c, uint16_t pid, uint8_t count) {
  uint8_t vh[2] = {(uint8_t)(pid >> 8), (uint8_t)(pid & 0xFF)};
  c.write((uint8_t)0x90);
  writeRemainingLength(c, 2 + count);
  c.write(vh, 2);
  for (uint8_t i = 0; i < count; i++) {
    c.write((uint8_t)0x00);
  }
}

void MqttBridge::sendUnsubAck(WiFiClient &c, uint16_t pid) {
  uint8_t vh[2] = {(uint8_t)(pid >> 8), (uint8_t)(pid & 0xFF)};
  c.write((uint8_t)0xB0);
  writeRemainingLength(c, 2);
  c.write(vh, 2);
}

void MqttBridge::sendPingResp(WiFiClient &c) {
  c.write((uint8_t)0xD0);
  writeRemainingLength(c, 0);
}

void MqttBridge::sendPublish(WiFiClient &c, const String &topic,
                             const uint8_t *payload, uint32_t len,
                             uint8_t qos) {
  uint16_t tlen = topic.length();
  uint32_t totalRemaining = 2 + tlen + len;
  uint8_t rem[4]; uint8_t rlen = 0;
  uint32_t n = totalRemaining;
  do { uint8_t b = n % 128; n /= 128; if (n) b |= 0x80; rem[rlen++] = b; } while (n);
  uint32_t packetLen = 1 + rlen + totalRemaining;
  uint8_t *packet = new uint8_t[packetLen];
  uint32_t p = 0;
  packet[p++] = (uint8_t)((3 << 4) | (qos << 1));
  memcpy(packet + p, rem, rlen); p += rlen;
  packet[p++] = (uint8_t)(tlen >> 8); packet[p++] = (uint8_t)tlen;
  memcpy(packet + p, topic.c_str(), tlen); p += tlen;
  if (len) memcpy(packet + p, payload, len);
  c.write(packet, packetLen);
  delete[] packet;
#ifdef ESP32
  // Downstream encrypt (c.write for TLS clients) loaded the downstream AES key
  // into the shared hardware register, leaving upstream's key_in_hardware stale.
  // Clear it so the next upstream AES op forces a reload.
  invalidateAesKeys(_upSslCtx);
#endif
}

// ------------------------------------------------------------------
// wire-level helpers
// ------------------------------------------------------------------
bool MqttBridge::readByte(WiFiClient &c, uint8_t &b) {
  int v = c.read();
  if (v >= 0) { b = (uint8_t)v; return true; }
  // Retry with short delays — gives TLS data time to arrive and decrypt
  for (int i = 0; i < 10; i++) {
    delay(5);
    v = c.read();
    if (v >= 0) { b = (uint8_t)v; return true; }
    if (!c.connected()) break;
  }
  return false;
}

bool MqttBridge::readRemainingLength(WiFiClient &c, uint32_t &len) {
  len = 0;
  uint32_t multiplier = 1;
  for (int i = 0; i < 4; i++) {
    uint8_t b;
    if (!readByte(c, b)) return false;
    len += (b & 0x7F) * multiplier;
    multiplier *= 128;
    if (!(b & 0x80)) return true;
  }
  return false;
}

bool MqttBridge::readBytes(WiFiClient &c, uint8_t *buf, uint32_t len) {
  while (len > 0) {
    int got = c.read(buf, len);
    if (got <= 0) return false;
    buf += got;
    len -= got;
  }
  return true;
}

void MqttBridge::writeRemainingLength(WiFiClient &c, uint32_t len) {
  do {
    uint8_t b = len % 128;
    len /= 128;
    if (len > 0) b |= 0x80;
    c.write(b);
  } while (len > 0);
}
