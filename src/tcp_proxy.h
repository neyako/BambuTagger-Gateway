#pragma once

#ifdef ESP32
#include <WiFi.h>
#else
#include <ESP8266WiFi.h>
#endif
#include "config.h"

// lwIP in this core has 16 sockets for everything: listeners, the printer link
// and every client. Local clients are admitted only while socketReserve() stay
// free, so a full gateway can still reconnect to the printer and serve the UI.
int freeSockets();
int socketReserve();  // main.cpp: 1 (web UI) while the printer link holds its own socket, else 2

// This core's WiFiClient::connected() never notices an orderly close (FIN):
// recv(len=0) returns 0 and leaves errno stale. A peer that hung up cleanly
// (e.g. FTPS after QUIT) would keep its sockets forever. Unread data counts
// as open so a final packet is still delivered.
bool peerOpen(WiFiClient &c);
bool fdOpen(int fd);  // the raw-socket part of peerOpen()
void shutWrite(WiFiClient &c);  // send FIN, keep receiving (half-close)

// Camera (6000) and FTPS (990) are end-to-end TLS, so each local client gets
// its own printer socket; TLS sessions cannot share one upstream stream.
struct ProxyClient {
  WiFiClient local;
  WiFiClient up;
  bool localShut = false, upShut = false;  // FIN already passed on to that side
  unsigned long lastData = 0;              // last byte relayed either way
};

class TcpProxy {
public:
  // shortLived: sessions end on their own (FTPS uploads), so they may borrow
  // the web UI's reserved socket, but never the printer reconnect's.
  TcpProxy(const char *name, uint16_t localPort, const char *remoteHost,
           uint16_t remotePort, uint8_t maxClients, bool shortLived = false);
  void begin();
  void loop();
  void setRemote(const char *host, uint16_t port);
  int clientCount();

private:
  const char *_name;
  uint16_t _remotePort;
  const char *_remoteHost;
  uint8_t _maxClients;
  bool _shortLived;
  WiFiServer _server;
  ProxyClient *_clients;

  bool connectUpstream(WiFiClient &up);
  static bool pump(WiFiClient &from, WiFiClient &to);
};
