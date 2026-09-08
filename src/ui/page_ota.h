#pragma once

static const char _PAGE_OTA[] PROGMEM = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BambuTagger-Gateway — Update</title>
<style>
.ver{color:#8b949e;font-size:.95em;margin:9px 0 4px}
.ver span{color:#f0f6fc;font-weight:bold}
.ver .new{color:#3fb950}
.ver .old{color:#8b949e}
button:disabled{background:#484f58;cursor:not-allowed;color:#8b949e}
#status{display:none;margin-top:14px;padding:12px;border-radius:6px;font-size:.9em;text-align:center}
#status.info{display:block;background:rgba(88,166,255,0.1);color:#58a6ff}
#status.ok{display:block;background:rgba(63,185,80,0.1);color:#3fb950}
#status.err{display:block;background:rgba(218,54,51,0.1);color:#da3633}
)html";

static const char _PAGE_OTA2[] PROGMEM = R"html(
</style>
</head>
<body>
<header><div class="logo"><img src="/Logo/bambutagger.png" alt="B"><h1>BambuTagger-Gateway</h1></div></header>
<nav>
<a href="/">Dashboard</a>
<a href="/setup">Guide</a>
<a href="/config/settings">Printer</a>
<a href="/config/wifi">WiFi</a>
<a href="/config/ota" class="active">Update</a>
</nav>
<div class="wrapper">
<div class="card">
  <h2>Firmware Update</h2>
  <div class="ver">Current: <span class="old" id="cur-ver">%%VERSION%%</span></div>
  <div class="ver">Latest:  <span class="new" id="lat-ver">&mdash;</span></div>
  <div id="status"></div>
  <button id="up-btn">Check&hellip;</button>
</div>
</div>
<footer>&copy; 2026 — <a href="https://github.com/VID-PRO/BambuTagger-Gateway" target="_blank">BambuTagger-Gateway</a></footer>
<script>
(function(){
  var btn=document.getElementById('up-btn'),
      st=document.getElementById('status'),
      lv=document.getElementById('lat-ver');
  btn.textContent='Checking\u2026'; btn.disabled=true;
  st.className='info'; st.textContent='Checking for latest release\u2026';
  st.style.display='block';
  var x=new XMLHttpRequest();
  x.open('GET','/api/release',true);
  x.onload=function(){
    if(x.status==200){
      try {
        var r=JSON.parse(x.responseText);
        lv.textContent=r.tag;
        if(r.update){
          st.className='info'; st.textContent='Version '+r.tag+' is available!';
          btn.textContent='Install Version '+r.tag;
          btn.disabled=false;
        }else{
          st.className='ok'; st.textContent='Already up to date.';
          btn.textContent='Re-install '+r.tag;
          btn.disabled=false;
        }
      } catch(e) {
        st.className='err'; st.textContent='Failed to parse response.';
        btn.textContent='Update from GitHub';
        btn.disabled=false;
      }
    }else{
      st.className='err'; st.textContent='Failed to check for updates.';
      btn.textContent='Update from GitHub';
      btn.disabled=false;
    }
  };
  x.onerror=function(){
    st.className='err'; st.textContent='Network error checking for updates.';
    btn.textContent='Update from GitHub';
    btn.disabled=false;
  };
  x.send();
  btn.onclick=function(){
    if(btn.disabled)return;
    location.href='/update/github';
  };
})();
</script>
</body>
</html>
)html";

static String buildOta() {
  String page = buildPage(_PAGE_OTA, _PAGE_OTA2);
  page.replace("%%VERSION%%", VERSION);
  return page;
}

static void handleOta() {
  _server.send(200, "text/html; charset=utf-8", buildOta());
}

static void handleApiRelease() {
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
  bool canCheck = (strlen(_cfg.stationSsid) > 0) && WiFi.isConnected();
  if (canCheck) {
    String latestVer = fetchLatestVersion();
    if (latestVer.length() > 0 && compareVersions(latestVer, VERSION) <= 0) {
      String html = F("<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>No Update Available</title><style>");
      html += FPSTR(_SITE_STYLE);
      html += F("</style></head><body>");
      html += F("<header><div class=\"logo\"><img src=\"/Logo/bambutagger.png\" alt=\"B\">"
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
      html += F("<a href=\"/config/ota\" style=\"display:inline-block;margin-top:16px;"
        "padding:10px 20px;background:#21262d;color:#c9d1d9;border-radius:6px;"
        "text-decoration:none\">&larr; Back</a>");
      html += F("</div></div></body></html>");
      _server.send(200, "text/html; charset=utf-8", html);
      return;
    }
  }

  String html = F("<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Updating&hellip;</title><style>");
  html += FPSTR(_SITE_STYLE);
  html += F("</style></head><body>");
  html += F("<header><div class=\"logo\"><img src=\"/Logo/bambutagger.png\" alt=\"B\"><h1>BambuTagger-Gateway</h1></div></header>");
  html += F("<div class=\"wrapper\"><div class=\"card\" style=\"text-align:center\">");
  html += F("<h1 style=\"color:#58a6ff\">Updating&hellip;</h1>");
  html += F("<p style=\"color:#8b949e;margin-top:8px\">Downloading from GitHub and flashing</p>");
  html += F("</div></div></body></html>");
  _server.send(200, "text/html; charset=utf-8", html);
  _server.client().flush();
  _server.client().stop();

  delay(100);

  WiFiClientSecure client;
  client.setInsecure();
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

  String url = F("https://github.com/VID-PRO/BambuTagger-Gateway/releases/latest/download/BambuTagger-Gateway.ino.bin");

  http.begin(client, url);
  int code = http.GET();

  if (code == 200) {
    int len = http.getSize();
    if (len > 0) {
      WiFiClient *stream = http.getStreamPtr();
      if (Update.begin(len)) {
        size_t written = 0;
        uint8_t buf[256];
        while (written < (size_t)len) {
          int avail = stream->available();
          if (avail > 0) {
            int rd = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
            Update.write(buf, rd);
            written += rd;
          } else {
            delay(1);
          }
        }
        if (Update.end(true)) {
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
