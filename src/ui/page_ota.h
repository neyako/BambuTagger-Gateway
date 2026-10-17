#pragma once

static void handleApiRelease() {
  if (!authorized()) return;
  String tag, json;
  bool canCheck = (strlen(_cfg.stationSsid) > 0) && WiFi.isConnected();
  if (canCheck) tag = fetchLatestVersion();
  if (tag.length() == 0) {
    json = F("{\"tag\":\"?\",\"update\":false}");
  } else {
    bool update = compareVersions(tag, VERSION) > 0;
    String safeTag = tag;
    safeTag.replace("\"", "\\\"");
    json = "{\"tag\":\"" + safeTag + "\",\"update\":" + (update ? "true" : "false") + "}";
  }
  _server.send(200, "application/json", json);
}

static void handleUpdateFromGithub() {
  if (!authorized()) return;
  bool canCheck = (strlen(_cfg.stationSsid) > 0) && WiFi.isConnected();
  if (canCheck) {
    String latestVer = fetchLatestVersion();
    if (latestVer.length() > 0 && compareVersions(latestVer, VERSION) <= 0) {
      String html = F("<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>No Update Available</title><style>");
      html += FPSTR(_SITE_STYLE);
      html += F("</style></head><body>");
      html += F("<header><div class=\"logo\"><img src=\"/Logo/bambutagger.png\" alt=\"\">"
        "<h1>BambuTagger-Gateway</h1></div></header>");
      html += F("<div class=\"wrapper\"><div class=\"card\" style=\"text-align:center\">");
      html += F("<h1 style=\"color:#3fb950\">Already Up to Date</h1>");
      html += F("<p style=\"color:#8b949e;margin-top:8px\">Current: <strong>v");
      html += VERSION;
      html += F("</strong> &mdash; Latest: <strong>");
      html += latestVer;
      html += F("</strong></p>");
      html += F("<p style=\"color:#484f58;font-size:12px;margin-top:12px\">"
        "Downgrade is not allowed.</p>");
      html += F("<a href=\"/\" style=\"display:inline-block;margin-top:16px;"
        "padding:10px 20px;background:#21262d;color:#c9d1d9;border-radius:6px;"
        "text-decoration:none\">&larr; Back</a>");
      html += F("</div></div></body></html>");
      _server.send(200, "text/html; charset=utf-8", html);
      return;
    }
  }

  String html = F("<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<meta http-equiv=\"refresh\" content=\"60;url=/\">"
    "<title>Updating&hellip;</title><style>");
  html += FPSTR(_SITE_STYLE);
  html += F("</style></head><body>");
  html += F("<header><div class=\"logo\"><img src=\"/Logo/bambutagger.png\" alt=\"B\"><h1>BambuTagger-Gateway</h1></div></header>");
  html += F("<div class=\"wrapper\"><div class=\"card\" style=\"text-align:center\">");
  html += F("<h1 style=\"color:#58a6ff\">Updating&hellip;</h1>");
  html += F("<p style=\"color:#8b949e;margin-top:8px\">Downloading from GitHub and flashing. "
    "This page reloads in a minute; if the version is unchanged, the update failed.</p>");
  html += F("</div></div></body></html>");
  _server.send(200, "text/html; charset=utf-8", html);
  _server.client().flush();
  _server.client().stop();

  delay(100);

  WiFiClientSecure client;
  client.setCACert(GITHUB_CA);
#ifdef ESP32
  client.setTimeout(10);  // WiFiClientSecure uses seconds; HTTPClient uses milliseconds
  client.setHandshakeTimeout(10);
#else
  client.setTimeout(10000);
#endif

  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(30000);
  http.setUserAgent(F("BambuTagger-Gateway"));

  // Must match the asset name published by .github/workflows/release.yml.
  String url = F("https://github.com/VID-PRO/BambuTagger-Gateway/releases/latest/download/firmware.bin");

  http.begin(client, url);
  int code = http.GET();

  if (code == 200) {
    int len = http.getSize();
    if (len > 0) {
      WiFiClient *stream = http.getStreamPtr();
      if (Update.begin(len)) {
        size_t written = 0;
        uint8_t buf[256];
        unsigned long lastData = millis();
        // A stalled or dropped download must not hang the gateway forever.
        while (written < (size_t)len && millis() - lastData < 15000) {
          int avail = stream->available();
          esp_task_wdt_reset();  // the download outlasts the loop watchdog
          if (avail > 0) {
            int rd = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
            if (rd <= 0 || Update.write(buf, rd) != (size_t)rd) break;
            written += rd;
            lastData = millis();
          } else if (!stream->connected()) {
            break;
          } else {
            delay(1);
          }
        }
        if (written < (size_t)len) Update.abort();
        else if (Update.end(true)) {
          Serial.println("GitHub update successful, rebooting");
          http.end();
          delay(500);
          ESP.restart();
        }
      }
    }
  }
  http.end();
  Serial.println("GitHub update failed");
}
