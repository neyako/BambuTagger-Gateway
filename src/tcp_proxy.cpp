#include "tcp_proxy.h"
#ifdef ESP32
#include <ESPmDNS.h>
#include <lwip/sockets.h>

bool peerOpen(WiFiClient &c) {
  if (!c.connected()) return false;
  if (c.available() > 0) return true;
  uint8_t b;
  int r = recv(c.fd(), &b, 1, MSG_PEEK | MSG_DONTWAIT);
  return r > 0 || (r < 0 && (errno == EWOULDBLOCK || errno == EAGAIN));
}

int freeSockets() {
  int n = 0;
  for (int i = 0; i < CONFIG_LWIP_MAX_SOCKETS; i++)
    n += lwip_fcntl(LWIP_SOCKET_OFFSET + i, F_GETFL, 0) < 0;
  return n;
}
#endif

TcpProxy::TcpProxy(const char *name, uint16_t localPort, const char *remoteHost,
                   uint16_t remotePort, uint8_t maxClients)
  : _name(name), _remotePort(remotePort), _remoteHost(remoteHost),
    _maxClients(maxClients), _server(localPort) {
  _clients = new ProxyClient[maxClients];
}

void TcpProxy::begin() {
  _server.begin();
  _server.setNoDelay(true);
}

void TcpProxy::loop() {
  bool ready = WiFi.isConnected() && strlen(_remoteHost) > 0;

  // Idle proxies hold no printer slot: upstream opens only for a local client,
  // and a client the printer refuses is closed at once.
  WiFiClient c = _server.accept();
  if (c) {
    ProxyClient *slot = nullptr;
    for (uint8_t i = 0; i < _maxClients && !slot; i++)
      if (!_clients[i].local.connected() && !_clients[i].up.connected()) slot = &_clients[i];
    // The upstream needs one more socket on top of the reserve.
    if (slot && ready && freeSockets() > SOCKET_RESERVE && connectUpstream(slot->up)) {
      c.setNoDelay(true);
      slot->local = c;
    } else {
      c.stop();
    }
  }

  for (uint8_t i = 0; i < _maxClients; i++) {
    ProxyClient &p = _clients[i];
    if (!p.local.connected() && !p.up.connected()) continue;
    pump(p.up, p.local);
    pump(p.local, p.up);
    if (!ready || !peerOpen(p.local) || !peerOpen(p.up)) {
      p.local.stop();
      p.up.stop();
    }
  }
}

int TcpProxy::clientCount() {
  int n = 0;
  for (uint8_t i = 0; i < _maxClients; i++) n += _clients[i].local.connected();
  return n;
}

void TcpProxy::setRemote(const char *host, uint16_t port) {
  _remoteHost = host;
  _remotePort = port;
}

bool TcpProxy::connectUpstream(WiFiClient &up) {
  up.stop();
#ifdef ESP32
  up.setTimeout(1);  // ESP32 WiFiClient uses seconds, including TCP connect
#else
  up.setTimeout(1000);
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
  if (!up.connect(host, _remotePort)) {
    Serial.printf("%s: printer refused or unreachable\n", _name);
    return false;
  }
  up.setNoDelay(true);
  return true;
}

void TcpProxy::pump(WiFiClient &from, WiFiClient &to) {
  if (!from.available()) return;
  uint8_t buf[512];
  int len = from.read(buf, sizeof(buf));
  if (len > 0) to.write(buf, len);
}
