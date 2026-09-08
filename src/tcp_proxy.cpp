#include "tcp_proxy.h"
#ifdef ESP32
#include <ESPmDNS.h>
#endif

TcpProxy::TcpProxy(const char *name, uint16_t localPort, const char *remoteHost,
                   uint16_t remotePort, uint8_t maxClients)
  : _name(name), _localPort(localPort), _remotePort(remotePort),
    _remoteHost(remoteHost), _maxClients(maxClients),
    _server(localPort), _lastReconnect(0), _disconnected(true) {
  _clients = new ProxyClient[maxClients];
  for (uint8_t i = 0; i < maxClients; i++) {
    _clients[i].active = false;
    _clients[i].local = nullptr;
  }
}

void TcpProxy::begin() {
  _server.begin();
  _server.setNoDelay(true);
}

void TcpProxy::loop() {
  if (!WiFi.isConnected() || strlen(_remoteHost) == 0) {
    disconnectAll();
    _upstream.stop();
    _disconnected = true;
    return;
  }

  if (!_upstream.connected() && !_disconnected) {
    disconnectAll();
    _disconnected = true;
  }
  while (acceptClient() >= 0) {}

  bool hasClients = false;
  for (uint8_t i = 0; i < _maxClients; i++) {
    if (_clients[i].active && !_clients[i].local->connected()) disconnectClient(i);
    if (_clients[i].active) hasClients = true;
  }
  // Idle camera/FTPS proxies must not occupy printer connection slots.
  if (!hasClients) {
    _upstream.stop();
    _disconnected = true;
    return;
  }

  if (!_upstream.connected()) {
    unsigned long now = millis();
    if (now - _lastReconnect > UPSTREAM_RETRY_MS) {
      if (connectUpstream()) {
        _disconnected = false;
      }
      _lastReconnect = millis();  // wait after the attempt, including its timeout
    }
    return;
  }

  // forward upstream -> all downstream (broadcast)
  forwardUpToDown();

  // forward downstream -> upstream (round-robin one client at a time)
  for (uint8_t i = 0; i < _maxClients; i++) {
    if (!_clients[i].active) continue;
    if (!_clients[i].local->connected()) {
      disconnectClient(i);
      continue;
    }
    forwardDownToUp(i);
  }
}

void TcpProxy::setRemote(const char *host, uint16_t port) {
  _remoteHost = host;
  _remotePort = port;
  // force reconnect on next loop
  if (_upstream.connected()) {
    _upstream.stop();
  }
  _lastReconnect = 0;
}

bool TcpProxy::connectUpstream() {
  if (_upstream.connected()) return true;
  _upstream.stop();
#ifdef ESP32
  _upstream.setTimeout(1);  // ESP32 WiFiClient uses seconds, including TCP connect
#else
  _upstream.setTimeout(1000);
#endif

  const char *host = _remoteHost;
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
      Serial.printf("%s: resolved %s.local -> %s\n", _name, name, host);
    }
  }
#endif
  if (!_upstream.connect(host, _remotePort)) return false;
  _upstream.setNoDelay(true);
  return true;
}

int TcpProxy::acceptClient() {
  WiFiClient *c = new WiFiClient(_server.accept());
  if (!c || !c->connected()) {
    delete c;
    return -1;
  }
  c->setNoDelay(true);

  for (uint8_t i = 0; i < _maxClients; i++) {
    if (!_clients[i].active) {
      _clients[i].local = c;
      _clients[i].active = true;
      return i;
    }
  }
  c->stop();
  delete c;
  return -2;
}

void TcpProxy::disconnectClient(int idx) {
  if (!_clients[idx].active) return;
  _clients[idx].local->stop();
  delete _clients[idx].local;
  _clients[idx].local = nullptr;
  _clients[idx].active = false;
}

void TcpProxy::disconnectAll() {
  for (uint8_t i = 0; i < _maxClients; i++) {
    if (_clients[i].active) {
      disconnectClient(i);
    }
  }
}

void TcpProxy::forwardUpToDown() {
  if (!_upstream.available()) return;
  uint8_t buf[512];
  int len = _upstream.read(buf, sizeof(buf));
  if (len <= 0) return;
  for (uint8_t i = 0; i < _maxClients; i++) {
    if (_clients[i].active) {
      _clients[i].local->write(buf, len);
    }
  }
}

void TcpProxy::forwardDownToUp(int idx) {
  if (!_clients[idx].local->available()) return;
  uint8_t buf[512];
  int len = _clients[idx].local->read(buf, sizeof(buf));
  if (len <= 0) return;
  _upstream.write(buf, len);
}
