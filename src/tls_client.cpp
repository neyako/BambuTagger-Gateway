#include "tls_client.h"
#include <string.h>
#include <mbedtls/error.h>
#include <mbedtls/cipher.h>
#include "mbedtls_compat.h"
#include <aes/esp_aes.h>
#include <aes/esp_aes_gcm.h>

#define TAG "TLS"

TlsWiFiClient::TlsWiFiClient() {
  _feedLen = 0;
  _rlen = 0;
  _rpos = 0;
  mbedtls_ssl_init(&_ssl);
  mbedtls_ssl_config_init(&_conf);
  mbedtls_x509_crt_init(&_cert);
  mbedtls_pk_init(&_pkey);
  mbedtls_entropy_init(&_entropy);
  mbedtls_ctr_drbg_init(&_drbg);
}

TlsWiFiClient::~TlsWiFiClient() {
  stop();
}

bool TlsWiFiClient::begin(WiFiClient *tcp, const char *certPem, const char *keyPem) {
  _tcp = tcp;
  _hsDone = false;
  _hsRetries = 0;
  _ok = false;

  int r;

  r = mbedtls_ctr_drbg_seed(&_drbg, mbedtls_entropy_func, &_entropy, (const uint8_t *)TAG, strlen(TAG));
  if (r != 0) { Serial.printf("TLS: drbg seed failed: -0x%x\n", -r); return false; }

  r = mbedtls_x509_crt_parse(&_cert, (const uint8_t *)certPem, strlen(certPem) + 1);
  if (r != 0) { Serial.printf("TLS: cert parse failed: -0x%x\n", -r); return false; }

  r = mbedtls_pk_parse_key(&_pkey, (const uint8_t *)keyPem, strlen(keyPem) + 1, NULL, 0);
  if (r != 0) { Serial.printf("TLS: key parse failed: -0x%x\n", -r); return false; }

  r = mbedtls_ssl_config_defaults(&_conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
  if (r != 0) { Serial.printf("TLS: config defaults failed: -0x%x\n", -r); return false; }

  mbedtls_ssl_conf_max_version(&_conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
  mbedtls_ssl_conf_min_version(&_conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
  mbedtls_ssl_conf_authmode(&_conf, MBEDTLS_SSL_VERIFY_NONE);
  mbedtls_ssl_conf_renegotiation(&_conf, MBEDTLS_SSL_RENEGOTIATION_ENABLED);

  mbedtls_ssl_conf_rng(&_conf, mbedtls_ctr_drbg_random, &_drbg);
  mbedtls_ssl_conf_own_cert(&_conf, &_cert, &_pkey);

  // Enable mbedtls debug logging
  mbedtls_ssl_conf_dbg(&_conf, [](void *ctx, int level, const char *file, int line, const char *str) {
    if (level <= 1) { // only most important messages
      Serial.printf("MBEDTLS: %s:%d: %s", file, line, str);
    }
  }, NULL);

  r = mbedtls_ssl_setup(&_ssl, &_conf);
  if (r != 0) { Serial.printf("TLS: ssl setup failed: -0x%x\n", -r); return false; }

  mbedtls_ssl_set_bio(&_ssl, this, bio_send, bio_recv, NULL);

  _ok = true;
  _dumpLen = 0;
  _sndDumpLen = 0;
  _readWantCnt = 0;
  _readCloseNotify = 0;
  return true;
}

// Return the DER length of a single ASN.1 SEQUENCE at buf, or 0 on error
static size_t der_seq_length(const uint8_t *buf, size_t maxlen) {
  if (maxlen < 2 || buf[0] != 0x30) return 0;
  size_t len;
  if (buf[1] < 0x80) {
    len = buf[1];
    return 2 + len;
  }
  size_t nbytes = buf[1] & 0x7f;
  if (nbytes == 0 || 2 + nbytes > maxlen) return 0;
  len = 0;
  for (size_t i = 0; i < nbytes; i++)
    len = (len << 8) | buf[2 + i];
  return 2 + nbytes + len;
}

bool TlsWiFiClient::beginDer(WiFiClient *tcp, const uint8_t *certDer, size_t certLen,
                             const uint8_t *keyDer, size_t keyLen) {
  _tcp = tcp;
  _hsDone = false;
  _hsRetries = 0;
  _hsStart = 0;
  _hsState = 0;
  _ok = false;

  int r;
  r = mbedtls_ctr_drbg_seed(&_drbg, mbedtls_entropy_func, &_entropy, (const uint8_t *)TAG, strlen(TAG));
  if (r != 0) { Serial.printf("TLS: drbg seed failed: -0x%x\n", -r); return false; }

  // Re-init cert/key in case constructor didn't fully reset
  mbedtls_x509_crt_free(&_cert);
  mbedtls_x509_crt_init(&_cert);
  mbedtls_pk_free(&_pkey);
  mbedtls_pk_init(&_pkey);

  // Parse chain of concatenated DER certs (leaf + intermediates)
  size_t off = 0;
  while (off < certLen) {
    size_t derLen = der_seq_length(certDer + off, certLen - off);
    if (derLen == 0) {
      Serial.printf("TLS: bad DER at offset %u\n", (unsigned)off);
      return false;
    }
    r = mbedtls_x509_crt_parse(&_cert, certDer + off, derLen);
    if (r != 0) {
      Serial.printf("TLS: cert parse failed at offset %u: -0x%x\n", (unsigned)off, -r);
      return false;
    }
    off += derLen;
  }

  r = mbedtls_pk_parse_key(&_pkey, keyDer, keyLen, NULL, 0);
  if (r != 0) { Serial.printf("TLS: key parse failed: -0x%x\n", -r); return false; }

  // ... remainder identical to begin()
  r = mbedtls_ssl_config_defaults(&_conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
  if (r != 0) { Serial.printf("TLS: config defaults failed: -0x%x\n", -r); return false; }

  mbedtls_ssl_conf_max_version(&_conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
  mbedtls_ssl_conf_min_version(&_conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
  mbedtls_ssl_conf_authmode(&_conf, MBEDTLS_SSL_VERIFY_NONE);
  mbedtls_ssl_conf_renegotiation(&_conf, MBEDTLS_SSL_RENEGOTIATION_ENABLED);

  mbedtls_ssl_conf_rng(&_conf, mbedtls_ctr_drbg_random, &_drbg);
  mbedtls_ssl_conf_own_cert(&_conf, &_cert, &_pkey);

  // Enable mbedtls debug logging
  mbedtls_ssl_conf_dbg(&_conf, [](void *ctx, int level, const char *file, int line, const char *str) {
    if (level <= 1) {
      Serial.printf("MBEDTLS: %s:%d: %s", file, line, str);
    }
  }, NULL);

  r = mbedtls_ssl_setup(&_ssl, &_conf);
  if (r != 0) { Serial.printf("TLS: ssl setup failed: -0x%x\n", -r); return false; }

  mbedtls_ssl_set_bio(&_ssl, this, bio_send, bio_recv, NULL);

  _ok = true;
  _dumpLen = 0;
  _sndDumpLen = 0;
  _readWantCnt = 0;
  _readCloseNotify = 0;
  return true;
}

int TlsWiFiClient::continueHandshake() {
  if (!_ok || _hsDone) return _hsDone ? 1 : -1;

  if (_hsStart == 0) _hsStart = millis();

  // 30-second total timeout for entire handshake
  if (millis() - _hsStart > 30000) {
    Serial.printf("TLS: handshake timed out\n");
    _ok = false;
    return -1;
  }

  _hsRetries++;

  int c = (_tcp ? _tcp->connected() : 0);
  int a = (_tcp ? _tcp->available() : 0);

  if (_hsRetries <= 3 || a > 0) {
    Serial.printf("TLS: handshake attempt %d, connected=%d available=%d dumpLen=%d\n",
                  _hsRetries, c, a, _dumpLen);
  }

  clearKeyInHardware();
  int r = mbedtls_ssl_handshake(&_ssl);
  if (r == 0) {
    _hsDone = true; _hsRetries = 0;
    const char *cs = mbedtls_ssl_get_ciphersuite(&_ssl);
    if (cs) Serial.printf("TLS: handshake done, cipher=%s\n", cs);
    return 1;
  }
  if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
    _hsState = _ssl.state;
    return 0;
  }
  _hsState = _ssl.state;
  int cs = _ssl.session_negotiate->ciphersuite;
  char errbuf[128];
  mbedtls_strerror(r, errbuf, sizeof(errbuf));
  int alertLvl = _ssl.in_msg[0], alertDesc = _ssl.in_msg[1];
  Serial.printf("TLS: handshake error: -0x%x (%s) state=%d alert=%d/%d cs=%04x\n",
                -r, errbuf, _hsState, alertLvl, alertDesc, cs);
  if (_dumpLen > 0) {
    Serial.printf("TLS: recv hex (%d bytes): ", _dumpLen);
    for (int i = 0; i < (int)_dumpLen && i < 100; i++)
      Serial.printf("%02x", _dumpBuf[i]);
    Serial.printf("\n");
  }
  if (_sndDumpLen > 0) {
    Serial.printf("TLS: sent hex (%d bytes): ", _sndDumpLen);
    for (int i = 0; i < (int)_sndDumpLen && i < 512; i++)
      Serial.printf("%02x", _sndDumpBuf[i]);
    Serial.printf("\n");
  }
  _ok = false;
  return -1;
}

void TlsWiFiClient::stop() {
  if (_ok) {
    mbedtls_ssl_close_notify(&_ssl);
    _ok = false;
  }
  if (_tcp) {
    _tcp->stop();
    delete _tcp;
    _tcp = nullptr;
  }
  mbedtls_ssl_free(&_ssl);
  mbedtls_ssl_config_free(&_conf);
  mbedtls_x509_crt_free(&_cert);
  mbedtls_pk_free(&_pkey);
  mbedtls_ctr_drbg_free(&_drbg);
  mbedtls_entropy_free(&_entropy);
  // Re-init for potential reuse
  mbedtls_ssl_init(&_ssl);
  mbedtls_ssl_config_init(&_conf);
  mbedtls_x509_crt_init(&_cert);
  mbedtls_pk_init(&_pkey);
  mbedtls_entropy_init(&_entropy);
  mbedtls_ctr_drbg_init(&_drbg);
  _hsDone = false;
  _rlen = 0;
  _rpos = 0;
}

size_t TlsWiFiClient::write(uint8_t b) { return write(&b, 1); }

// Let mbedTLS own record lengths, sequence numbers, and partial writes.
size_t TlsWiFiClient::write(const uint8_t *buf, size_t size) {
  if (!_ok || !_hsDone) return 0;
  size_t total = 0;
  unsigned long start = millis();
  while (total < size) {
    clearKeyInHardware();
    int r = mbedtls_ssl_write(&_ssl, buf + total, size - total);
    if (r > 0) {
      total += r;
    } else if ((r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
               && millis() - start < 3000) {
      delay(1);
    } else {
      Serial.printf("TLS: write failed: %d\n", r);
      _ok = false;
      break;
    }
  }
  return total;
}

int TlsWiFiClient::available() {
  if (!_ok || !_tcp) return 0;
  if (_rlen > 0) return _rlen;
  int avail = mbedtls_ssl_get_bytes_avail(&_ssl);
  if (avail > 0) return avail;
  // Only fetch from TCP after handshake is done
  if (_hsDone && _tcp->available() > 0) return 1;
  return 0;
}

int TlsWiFiClient::read() {
  // Refill internal buffer if empty
  if (_rlen == 0) {
    int r = read(_rbuf, sizeof(_rbuf));
    if (r <= 0) return -1;
    _rlen = (size_t)r;
    _rpos = 0;
  }
  uint8_t b = _rbuf[_rpos++];
  _rlen--;
  return b;
}

int TlsWiFiClient::read(uint8_t *buf, size_t size) {
  if (!_ok || !_hsDone) { return -1; }
  size_t total = 0;
  // Serve from internal buffer first
  if (_rlen > 0) {
    size_t cp = (size < _rlen) ? size : _rlen;
    memcpy(buf, _rbuf + _rpos, cp);
    _rpos += cp;
    _rlen -= cp;
    buf += cp;
    size -= cp;
    total += cp;
  }
    // Read remaining from mbedtls
    if (size > 0) {
      // Skip the mbedtls_ssl_read call entirely if no data exists at any layer.
      // This avoids a WANT_READ round-trip when the connection is idle.
      if (_ssl.in_left == 0 && mbedtls_ssl_get_bytes_avail(&_ssl) == 0 &&
          (_tcp && _tcp->available() == 0)) {
        return total > 0 ? (int)total : -1;
      }
      clearKeyInHardware();
      int r = mbedtls_ssl_read(&_ssl, buf, size);
      if (r >= 0) {
        total += r;
      } else if (r == MBEDTLS_ERR_SSL_WANT_READ) {
        // Log internal state after WANT_READ to understand the bug
        if (_readWantCnt == 0 || _readWantCnt % 5 == 0) {
        // Dump first bytes of in_buf to check for lingering decrypted data
        uint32_t ib0 = 0, ib4 = 0, ib8 = 0;
        if (_ssl.in_buf) {
          ib0 = _ssl.in_buf[0] | (_ssl.in_buf[1]<<8) | (_ssl.in_buf[2]<<16) | (_ssl.in_buf[3]<<24);
          ib4 = _ssl.in_buf[4] | (_ssl.in_buf[5]<<8) | (_ssl.in_buf[6]<<16) | (_ssl.in_buf[7]<<24);
          ib8 = _ssl.in_buf[8] | (_ssl.in_buf[9]<<8) | (_ssl.in_buf[10]<<16) | (_ssl.in_buf[11]<<24);
        }
        Serial.printf("TLS: ssl_read=WANT_READ state=%d off=%p msglen=%d left=%d hslen=%d t_in=%p ol=%d hs=%p type=%d inbuf=%08x %08x %08x\n",
                      _ssl.state, (void*)_ssl.in_offt, _ssl.in_msglen,
                      _ssl.in_left, _ssl.in_hslen, (void*)_ssl.transform_in,
                      _ssl.out_left, (void*)_ssl.handshake, _ssl.in_msgtype,
                      (unsigned)ib0, (unsigned)ib4, (unsigned)ib8);
      }
      // Retry only if mbedTLS has buffered record data or TCP has raw data.
      // If neither has data, no point retrying — nothing will arrive.
      bool gotData = false;
      int maxRetry = (_ssl.in_left > 0 || (_tcp && _tcp->available() > 0)) ? 10 : 0;
      for (int retry = 0; retry < maxRetry; retry++) {
        delay(5);
        r = mbedtls_ssl_read(&_ssl, buf, size);
        if (r >= 0) { total += r; gotData = true; break; }
        if (r != MBEDTLS_ERR_SSL_WANT_READ) {
          char errbuf[128];
          mbedtls_strerror(r, errbuf, sizeof(errbuf));
          Serial.printf("TLS: ssl_read error (not WANT_READ): -0x%x (%s)\n", -r, errbuf);
          break;
        }
      }
      if (!gotData) {
        _readWantCnt++;
        if (_readWantCnt <= 5 || _readWantCnt % 10 == 0) {
          Serial.printf("TLS: read WANT_READ cnt=%d avail=%d tcpAvail=%d inBuf=%d feed=%d\n",
                        _readWantCnt, mbedtls_ssl_get_bytes_avail(&_ssl),
                        _tcp ? _tcp->available() : -1,
                        _ssl.in_left, _feedLen);
        }
        return total > 0 ? (int)total : -1;
      }
    } else if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
      _readCloseNotify++;
      return total > 0 ? (int)total : -1;
    } else {
      char errbuf[128];
      mbedtls_strerror(r, errbuf, sizeof(errbuf));
      Serial.printf("TLS: ssl_read error: -0x%x (%s)\n", -r, errbuf);
      _ok = false;
      return total > 0 ? (int)total : -1;
    }
  }
  return total > 0 ? (int)total : -1;
}

int TlsWiFiClient::peek() { return -1; }
// TCP writes are already submitted. WiFiClient::flush() clears RECEIVED
// bytes on this SDK; calling it here loses TLS records and their sequence.
void TlsWiFiClient::flush() {}
void TlsWiFiClient::flushWrites() {}
void TlsWiFiClient::clearKeyInHardware() {
  for (auto *t : { _ssl.transform_in, _ssl.transform_out }) {
    if (!t) continue;
    for (auto *ctx : { &t->cipher_ctx_dec, &t->cipher_ctx_enc }) {
      if (!ctx->cipher_info || !ctx->cipher_ctx) continue;
      mbedtls_cipher_type_t ct = ctx->cipher_info->type;
      if (ct < MBEDTLS_CIPHER_AES_128_ECB || ct > MBEDTLS_CIPHER_AES_256_KWP)
        continue;
      if (ctx->cipher_info->mode == MBEDTLS_MODE_GCM ||
          ctx->cipher_info->mode == MBEDTLS_MODE_CCM) {
        esp_gcm_context *gcm = (esp_gcm_context *)ctx->cipher_ctx;
        gcm->aes_ctx.key_in_hardware = 0;
      } else {
        esp_aes_context *aes = (esp_aes_context *)ctx->cipher_ctx;
        aes->key_in_hardware = 0;
      }
    }
  }
}

uint8_t TlsWiFiClient::connected() {
  return _ok && _tcp && _tcp->connected();
}

TlsWiFiClient::operator bool() { return _ok && _tcp && (*_tcp); }

int TlsWiFiClient::bio_recv(void *ctx, unsigned char *buf, size_t len) {
  TlsWiFiClient *c = (TlsWiFiClient *)ctx;

  // Return pre-fed data first
  if (c->_feedLen > 0) {
    size_t cp = (len < c->_feedLen) ? len : c->_feedLen;
    memcpy(buf, c->_feedBuf, cp);
    c->_feedLen -= cp;
    if (c->_feedLen > 0) memmove(c->_feedBuf, c->_feedBuf + cp, c->_feedLen);
    if (c->_dumpLen < sizeof(c->_dumpBuf)) {
      size_t dcp = (cp > sizeof(c->_dumpBuf) - c->_dumpLen) ? sizeof(c->_dumpBuf) - c->_dumpLen : cp;
      memcpy(c->_dumpBuf + c->_dumpLen, buf, dcp);
      c->_dumpLen += dcp;
    }
    return cp;
  }

  if (!c->_tcp || !c->_tcp->connected()) return MBEDTLS_ERR_SSL_CONN_EOF;
  int r = c->_tcp->read(buf, len);
  if (r > 0) {
    if (c->_dumpLen < sizeof(c->_dumpBuf)) {
      size_t cp = (size_t)r > sizeof(c->_dumpBuf) - c->_dumpLen ? sizeof(c->_dumpBuf) - c->_dumpLen : (size_t)r;
      memcpy(c->_dumpBuf + c->_dumpLen, buf, cp);
      c->_dumpLen += cp;
    }
    if (c->_bioLogCnt < 20) {
      c->_bioLogCnt++;
      int tcpA = c->_tcp ? c->_tcp->available() : -1;
      int hd = (r >= 5) ? (buf[0] << 16 | buf[1] << 8 | buf[2]) : -1;
      Serial.printf("TLS: bio_recv[%d] len=%d ret=%d tcpAvail=%d hd=%06x\n",
                    c->_bioLogCnt, (int)len, r, tcpA, hd);
    }
    return r;
  }
  // WiFiClient::read() returns -1 (or 0) when the buffer is empty — not an
  // error.  Only return CONN_EOF if the TCP connection has actually closed.
  if (!c->_tcp->connected()) return MBEDTLS_ERR_SSL_CONN_EOF;
  return MBEDTLS_ERR_SSL_WANT_READ;
}

int TlsWiFiClient::bio_send(void *ctx, const unsigned char *buf, size_t len) {
  TlsWiFiClient *c = (TlsWiFiClient *)ctx;
  if (!c->_tcp || !c->_tcp->connected()) return MBEDTLS_ERR_SSL_CONN_EOF;
  // Accumulate sent data for debugging
  if (c->_sndDumpLen < sizeof(c->_sndDumpBuf)) {
    size_t cp = len > sizeof(c->_sndDumpBuf) - c->_sndDumpLen ? sizeof(c->_sndDumpBuf) - c->_sndDumpLen : len;
    memcpy(c->_sndDumpBuf + c->_sndDumpLen, buf, cp);
    c->_sndDumpLen += cp;
  }
  // Retry until all bytes are written.  WiFiClient::write() may return fewer
  // bytes than requested when the lwIP send buffer is full; returning
  // WANT_WRITE to mbedTLS leaves out_left > 0, causing every subsequent
  // mbedtls_ssl_read() to return WANT_READ (because it tries to flush
  // output first).
  size_t total = 0;
  unsigned long start = millis();
  while (total < len) {
    int r = c->_tcp->write(buf + total, len - total);
    if (r > 0) {
      total += (size_t)r;
    } else {
      if (!c->_tcp->connected()) return MBEDTLS_ERR_SSL_CONN_EOF;
      if (millis() - start > 3000) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
      delay(1);
    }
  }
  return (int)len;
}
