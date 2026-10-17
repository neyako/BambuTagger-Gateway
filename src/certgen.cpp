#include "certgen.h"
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pem.h>
#include <mbedtls/version.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define OID_BAMBU_1     "\x2B\x06\x01\x04\x01\x86\x9A\xC3\xED\x49\x01"
#define OID_BAMBU_2     "\x2B\x06\x01\x04\x01\x86\x9A\xC3\xED\x49\x02"
#define OID_BAMBU_1_3   "\x2B\x06\x01\x04\x01\x86\x9A\xC3\xED\x49\x01\x03"
#define OID_BAMBU_1_4   "\x2B\x06\x01\x04\x01\x86\x9A\xC3\xED\x49\x01\x04"
#define OID_BAMBU_1_999 "\x2B\x06\x01\x04\x01\x86\x9A\xC3\xED\x49\x01\x87\x67"

#define CA_SUBJECT "C=CN, O=BBL Technologies Co. Ltd, CN=BBL Device CA N7-V2"  // original subject for testing
// The device has no RTC, so validity can't be renewed in the field. 1.0.0 used
// 2026-2027, which every date-checking client would reject from 2027-01-01.
// 2049 is the last year X.509 encodes as UTCTime.
#define CERT_NOT_BEFORE "20260101000000"
#define CERT_NOT_AFTER  "20491231235959"

static int addBambuExtensions(mbedtls_x509write_cert *crt) {
  int ret;
  static const uint8_t v1_val[] = { 0x0C, 0x02, 0x76, 0x31 };
  ret = mbedtls_x509write_crt_set_extension(crt,
    OID_BAMBU_1, sizeof(OID_BAMBU_1) - 1, 0, v1_val, sizeof(v1_val));
  if (ret) return ret;

  static const uint8_t false_val[] = { 0x0C, 0x05, 0x66, 0x61, 0x6C, 0x73, 0x65 };
  ret = mbedtls_x509write_crt_set_extension(crt,
    OID_BAMBU_2, sizeof(OID_BAMBU_2) - 1, 0, false_val, sizeof(false_val));
  if (ret) return ret;

  static const uint8_t printer_val[] = { 0x0C, 0x07, 0x50, 0x72, 0x69, 0x6E, 0x74, 0x65, 0x72 };
  ret = mbedtls_x509write_crt_set_extension(crt,
    OID_BAMBU_1_3, sizeof(OID_BAMBU_1_3) - 1, 0, printer_val, sizeof(printer_val));
  if (ret) return ret;

  static const uint8_t n7v2_val[] = { 0x0C, 0x05, 0x4E, 0x37, 0x2D, 0x56, 0x32 };
  ret = mbedtls_x509write_crt_set_extension(crt,
    OID_BAMBU_1_4, sizeof(OID_BAMBU_1_4) - 1, 0, n7v2_val, sizeof(n7v2_val));
  if (ret) return ret;

  static const uint8_t int0_val[] = { 0x02, 0x01, 0x00 };
  ret = mbedtls_x509write_crt_set_extension(crt,
    OID_BAMBU_1_999, sizeof(OID_BAMBU_1_999) - 1, 0, int0_val, sizeof(int0_val));
  return ret;
}

static int addSanExtension(mbedtls_x509write_cert *crt, const char *cn, const uint8_t *ip) {
  uint8_t buf[128];
  uint8_t *p = buf;
  *p++ = 0x30; // SEQUENCE
  uint8_t *lenp = p++;

  // DNS:localhost
  *p++ = 0x82; *p++ = 9; memcpy(p, "localhost", 9); p += 9;

  // DNS:{cn}
  size_t cnl = strlen(cn);
  *p++ = 0x82; *p++ = cnl; memcpy(p, cn, cnl); p += cnl;

  // IP:{ip} — ip is binary bytes {a,b,c,d}, not a string
  if (ip) {
    *p++ = 0x87; *p++ = 4;
    *p++ = ip[0]; *p++ = ip[1]; *p++ = ip[2]; *p++ = ip[3];
  }

  // IP:127.0.0.1
  *p++ = 0x87; *p++ = 4; *p++ = 127; *p++ = 0; *p++ = 0; *p++ = 1;

  *lenp = (uint8_t)(p - buf - 2);
  size_t total = p - buf;

  static const char sanOid[] = { 0x55, 0x1D, 0x11 }; // 2.5.29.17
  return mbedtls_x509write_crt_set_extension(crt, sanOid, sizeof(sanOid),
                                             0, buf, total);
}

static int writeDer(mbedtls_x509write_cert *crt, mbedtls_ctr_drbg_context *drbg,
                    uint8_t *out, size_t outSize, size_t *outLen) {
  uint8_t *buf = (uint8_t *)malloc(outSize);
  if (!buf) return -1;
  int len = mbedtls_x509write_crt_der(crt, buf, outSize,
                                       mbedtls_ctr_drbg_random, drbg);
  if (len <= 0) { free(buf); return len; }
  memcpy(out, buf + outSize - len, len);
  *outLen = len;
  free(buf);
  return 0;
}

static int genRsaKey(mbedtls_pk_context *pk, mbedtls_ctr_drbg_context *drbg) {
  int ret = mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
  if (ret) return ret;
  mbedtls_rsa_context *rsa = mbedtls_pk_rsa(*pk);
  return mbedtls_rsa_gen_key(rsa, mbedtls_ctr_drbg_random, drbg, 2048, 65537);
}

static int writeKeyDer(mbedtls_pk_context *pk, uint8_t *out, size_t outSize, size_t *outLen) {
  uint8_t *buf = (uint8_t *)malloc(outSize);
  if (!buf) return -1;
  int wr = mbedtls_pk_write_key_der(pk, buf, outSize);
  if (wr <= 0) { free(buf); return wr; }
  memcpy(out, buf + outSize - wr, wr);
  *outLen = wr;
  free(buf);
  return 0;
}

bool generateCertChain(const char *cn, uint8_t *certDer, size_t *certLen,
                       uint8_t *keyDer, size_t *keyLen,
                       uint8_t *caDer, size_t *caLen,
                       const uint8_t *ip) {
  mbedtls_pk_context caKey, devKey;
  mbedtls_x509write_cert caCrt, devCrt;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_entropy_context entropy;
  int ret;
  bool ok = false;

  mbedtls_pk_init(&caKey);
  mbedtls_pk_init(&devKey);
  mbedtls_x509write_crt_init(&caCrt);
  mbedtls_x509write_crt_init(&devCrt);
  mbedtls_ctr_drbg_init(&drbg);
  mbedtls_entropy_init(&entropy);

  do {
    ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, NULL, 0);
    if (ret) { printf("CERT: drbg seed failed -0x%x\n", -ret); break; }

    // Generate CA key pair
    ret = genRsaKey(&caKey, &drbg);
    if (ret) { printf("CERT: CA key gen failed -0x%x\n", -ret); break; }

    // Generate device key pair
    ret = genRsaKey(&devKey, &drbg);
    if (ret) { printf("CERT: dev key gen failed -0x%x\n", -ret); break; }

    // Write device private key DER
    {
      uint8_t *keyBuf = (uint8_t *)malloc(2048);
      if (!keyBuf) { printf("CERT: OOM keyBuf\n"); break; }
      int wr = mbedtls_pk_write_key_der(&devKey, keyBuf, 2048);
      if (wr <= 0) { free(keyBuf); printf("CERT: write key der failed %d\n", wr); break; }
      memcpy(keyDer, keyBuf + 2048 - wr, wr);
      *keyLen = wr;
      free(keyBuf);
    }

    // ── Build CA certificate ──
    mbedtls_x509write_crt_set_subject_key(&caCrt, &caKey);
    mbedtls_x509write_crt_set_issuer_key(&caCrt, &caKey);
    mbedtls_x509write_crt_set_md_alg(&caCrt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_version(&caCrt, MBEDTLS_X509_CRT_VERSION_3);

#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    { unsigned char s[] = {1}; ret = mbedtls_x509write_crt_set_serial_raw(&caCrt, s, sizeof(s)); if (ret) break; }
#else
    { mbedtls_mpi s; mbedtls_mpi_init(&s); mbedtls_mpi_lset(&s, 1); ret = mbedtls_x509write_crt_set_serial(&caCrt, &s); mbedtls_mpi_free(&s); if (ret) break; }
#endif

    ret = mbedtls_x509write_crt_set_validity(&caCrt, CERT_NOT_BEFORE, CERT_NOT_AFTER);
    if (ret) { printf("CERT: CA validity failed -0x%x\n", -ret); break; }

    ret = mbedtls_x509write_crt_set_issuer_name(&caCrt, CA_SUBJECT);
    if (ret) { printf("CERT: CA issuer failed -0x%x\n", -ret); break; }
    ret = mbedtls_x509write_crt_set_subject_name(&caCrt, CA_SUBJECT);
    if (ret) { printf("CERT: CA subject failed -0x%x\n", -ret); break; }

    ret = mbedtls_x509write_crt_set_basic_constraints(&caCrt, 1, -1);
    if (ret) { printf("CERT: CA basic constraints failed -0x%x\n", -ret); break; }

    ret = mbedtls_x509write_crt_set_key_usage(&caCrt,
      MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN);
    if (ret) { printf("CERT: CA key usage failed -0x%x\n", -ret); break; }

    // SKI + AKI (self-signed: both use same key)
    ret = mbedtls_x509write_crt_set_subject_key_identifier(&caCrt);
    if (ret) { printf("CERT: CA SKI failed -0x%x\n", -ret); break; }
    ret = mbedtls_x509write_crt_set_authority_key_identifier(&caCrt);
    if (ret) { printf("CERT: CA AKI failed -0x%x\n", -ret); break; }

    size_t caDerLen = 0;
    ret = writeDer(&caCrt, &drbg, caDer, 2048, &caDerLen);
    if (ret) { printf("CERT: CA write der failed %d\n", ret); break; }
    *caLen = caDerLen;

    // ── Build device certificate (signed by CA) ──
    char subjStr[256];
    snprintf(subjStr, sizeof(subjStr), "CN=%s", cn);  // real printer certs are CN-only

    mbedtls_x509write_crt_set_subject_key(&devCrt, &devKey);
    mbedtls_x509write_crt_set_issuer_key(&devCrt, &caKey);
    mbedtls_x509write_crt_set_md_alg(&devCrt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_version(&devCrt, MBEDTLS_X509_CRT_VERSION_3);

#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    { unsigned char s[] = {2}; ret = mbedtls_x509write_crt_set_serial_raw(&devCrt, s, sizeof(s)); if (ret) break; }
#else
    { mbedtls_mpi s; mbedtls_mpi_init(&s); mbedtls_mpi_lset(&s, 2); ret = mbedtls_x509write_crt_set_serial(&devCrt, &s); mbedtls_mpi_free(&s); if (ret) break; }
#endif

    ret = mbedtls_x509write_crt_set_validity(&devCrt, CERT_NOT_BEFORE, CERT_NOT_AFTER);
    if (ret) { printf("CERT: dev validity failed -0x%x\n", -ret); break; }

    ret = mbedtls_x509write_crt_set_issuer_name(&devCrt, CA_SUBJECT);
    if (ret) { printf("CERT: dev issuer failed -0x%x\n", -ret); break; }
    ret = mbedtls_x509write_crt_set_subject_name(&devCrt, subjStr);
    if (ret) { printf("CERT: dev subject failed -0x%x\n", -ret); break; }

    // Device cert: explicit CA:FALSE (required by some TLS stacks)
    ret = mbedtls_x509write_crt_set_basic_constraints(&devCrt, 0, -1);
    if (ret) { printf("CERT: dev basic constraints failed -0x%x\n", -ret); break; }
    ret = mbedtls_x509write_crt_set_key_usage(&devCrt,
      MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_ENCIPHERMENT);
    if (ret) { printf("CERT: dev key usage failed -0x%x\n", -ret); break; }

    // SKI + AKI (AKI references the CA key — required by Security.framework)
    ret = mbedtls_x509write_crt_set_subject_key_identifier(&devCrt);
    if (ret) { printf("CERT: dev SKI failed -0x%x\n", -ret); break; }
    ret = mbedtls_x509write_crt_set_authority_key_identifier(&devCrt);
    if (ret) { printf("CERT: dev AKI failed -0x%x\n", -ret); break; }

    // Netscape certificate type for SSL server
    ret = mbedtls_x509write_crt_set_ns_cert_type(&devCrt, MBEDTLS_X509_NS_CERT_TYPE_SSL_SERVER);
    if (ret) { printf("CERT: dev ns cert type failed -0x%x\n", -ret); break; }

    // Extended Key Usage: TLS Web Server + Client Authentication
    static const char eku_oid[] = { 0x55, 0x1D, 0x25, 0x00 };
    static const uint8_t eku_val[] = {
      0x30, 0x14,           // SEQUENCE, length 20
      0x06, 0x08,           // OID serverAuth (1.3.6.1.5.5.7.3.1)
      0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01,
      0x06, 0x08,           // OID clientAuth (1.3.6.1.5.5.7.3.2)
      0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x02
    };
    ret = mbedtls_x509write_crt_set_extension(&devCrt, eku_oid, 3,
                                               0, eku_val, sizeof(eku_val));
    if (ret) { printf("CERT: dev EKU failed -0x%x\n", -ret); break; }

    // Bambu device-type extensions — Studio expects these
    ret = addBambuExtensions(&devCrt);
    if (ret) { printf("CERT: dev Bambu extensions failed -0x%x\n", -ret); break; }

    // Subject Alternative Name: IP, localhost, serial (required by modern TLS)
    ret = addSanExtension(&devCrt, cn, ip);
    if (ret) { printf("CERT: dev SAN failed -0x%x\n", -ret); break; }

    size_t devDerLen = 0;
    ret = writeDer(&devCrt, &drbg, certDer, 2048, &devDerLen);
    if (ret) { printf("CERT: dev write der failed %d\n", ret); break; }

    // Do NOT include CA in chain — OpenSSL 3.x rejects self-signed CAs
    // in the chain as X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN (fatal).
    // The CA is installed in printer.cer as a trust anchor instead.
    *certLen = devDerLen;

    printf("CERT: generated CA (%d bytes) + device cert (%d bytes) CN=%s\n",
           (int)caDerLen, (int)devDerLen, cn);

    // Self-test: parse the device cert
    {
      mbedtls_x509_crt testCrt;
      mbedtls_x509_crt_init(&testCrt);
      ret = mbedtls_x509_crt_parse(&testCrt, certDer, *certLen);
      if (ret != 0) {
        printf("CERT: SELF-TEST FAILED -0x%x\n", -ret);
      } else {
        printf("CERT: self-test OK\n");
      }
      mbedtls_x509_crt_free(&testCrt);
    }

    // Print CA PEM for user
    {
      size_t pemLen = 0;
      uint8_t *pemBuf = (uint8_t *)malloc(2048);
      if (pemBuf) {
        ret = mbedtls_pem_write_buffer("-----BEGIN CERTIFICATE-----\n", "-----END CERTIFICATE-----\n",
                                       caDer, caDerLen, pemBuf, 2048, &pemLen);
        if (ret == 0 && pemLen > 0) {
          printf("\n=== Append to Bambu Studio's printer.cer via: ./tools/trust-gateway.sh <gateway-ip> ===\n");
          printf("%s", (char *)pemBuf);
          printf("=== Gateway CA cert (PEM) ===\n\n");
        }
        free(pemBuf);
      }
    }

    ok = true;
  } while (0);

  mbedtls_x509write_crt_free(&devCrt);
  mbedtls_x509write_crt_free(&caCrt);
  mbedtls_pk_free(&devKey);
  mbedtls_pk_free(&caKey);
  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);

  if (!ok) { *certLen = 0; *keyLen = 0; *caLen = 0; }
  return ok;
}
