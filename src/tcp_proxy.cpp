#include "tcp_proxy.h"
#include <errno.h>
#ifdef ESP32
#include <ESPmDNS.h>
#include <lwip/sockets.h>
#else
#include <sys/socket.h>
#endif

// An orderly close (FIN) shows up only as recv() == 0.
bool fdOpen(int fd) {
  uint8_t b;
  int r = recv(fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
  return r > 0 || (r < 0 && (errno == EWOULDBLOCK || errno == EAGAIN));
}

#ifdef ESP32
void shutWrite(WiFiClient &c) { shutdown(c.fd(), SHUT_WR); }

bool peerOpen(WiFiClient &c) {
  return c.connected() && (c.available() > 0 || fdOpen(c.fd()));
}

int freeSockets() {
  int n = 0;
  for (int i = 0; i < CONFIG_LWIP_MAX_SOCKETS; i++)
    n += lwip_fcntl(LWIP_SOCKET_OFFSET + i, F_GETFL, 0) < 0;
  return n;
}
#endif

TcpProxy::TcpProxy(const char *name, uint16_t localPort, const char *remoteHost,
                   uint16_t remotePort, uint8_t maxClients, bool shortLived)
  : _name(name), _remotePort(remotePort), _remoteHost(remoteHost),
    _maxClients(maxClients), _shortLived(shortLived), _server(localPort) {
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
    int reserve = _shortLived ? socketReserve() - 1 : socketReserve();
    if (slot && ready && freeSockets() > reserve && connectUpstream(slot->up)) {
      c.setNoDelay(true);
      slot->local = c;
      slot->localShut = slot->upShut = false;
      slot->lastData = millis();
    } else {
      c.stop();
    }
  }

  for (uint8_t i = 0; i < _maxClients; i++) {
    ProxyClient &p = _clients[i];
    if (!p.local.connected() && !p.up.connected()) continue;
    if (pump(p.up, p.local) | pump(p.local, p.up)) p.lastData = millis();
    // Relay half-closes like TCP: once a side has sent everything (EOF), pass
    // its FIN on and keep the other direction open until that ends too. A
    // dead socket (failed write) ends the pair; so does a peer that vanished
    // without RST, after HALF_CLOSE_IDLE_MS with nothing moving. The grace
    // period starts at the FIN, not at the last byte of an idle connection.
    bool localEof = !peerOpen(p.local), upEof = !peerOpen(p.up);
    if (localEof && !p.upShut) { shutWrite(p.up); p.upShut = true; p.lastData = millis(); }
    if (upEof && !p.localShut) { shutWrite(p.local); p.localShut = true; p.lastData = millis(); }
    bool stale = (localEof || upEof) && millis() - p.lastData > HALF_CLOSE_IDLE_MS;
    if (!ready || !p.local.connected() || !p.up.connected() || (localEof && upEof) || stale) {
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

bool TcpProxy::pump(WiFiClient &from, WiFiClient &to) {
  if (!from.available()) return false;
  uint8_t buf[512];
  int len = from.read(buf, sizeof(buf));
  if (len <= 0) return false;
  to.write(buf, len);
  return true;
}
