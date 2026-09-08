#pragma once

static const char _PAGE_ROOT[] PROGMEM = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BambuTagger-Gateway</title>
<link rel="icon" href="/Logo/bambutagger.png" type="image/png">
<style>
)html";

static const char _PAGE_ROOT2[] PROGMEM = R"html(
</style>
</head>
<body>
<header><div class="logo"><img src="/Logo/bambutagger.png" alt="B"><h1>BambuTagger-Gateway</h1></div></header>
<nav>
<a href="/" class="active">Dashboard</a>
<a href="/setup">Guide</a>
<a href="/config/settings">Printer</a>
<a href="/config/wifi">WiFi</a>
<a href="/config/ota">Update</a>
</nav>
<div class="wrapper">
<div class="card" style="max-width:560px">
  <h2>Gateway Status</h2>
  <div style="margin:12px 0">
    <div class="info-row"><span class="lbl">WiFi</span><span class="val">%%STA_STATUS%%</span></div>
    <div class="info-row"><span class="lbl">Printer Link</span><span class="val">%%MQTT_STATUS%%</span></div>
    <div class="info-row"><span class="lbl">Uptime</span><span class="val">%%UPTIME%%</span></div>
    <div class="info-row"><span class="lbl">Free Heap</span><span class="val">%%HEAP%%</span></div>
    <div class="info-row"><span class="lbl">Version</span><span class="val">%%VERSION%%</span></div>
  </div>
  </div>
</div>
<div class="wrapper">
<div class="card" style="max-width:560px">
  <h2>Gateway Identity</h2>
  <div style="margin:12px 0">
    <div class="info-row"><span class="lbl">IP</span><span class="val">%%STA_IP%%</span></div>
    <div class="info-row"><span class="lbl">Gateway Name</span><span class="val">%%PRINTER_NAME%%</span></div>
    <div class="info-row"><span class="lbl">Printer Serial</span><span class="val">%%PRINTER_SERIAL%%</span></div>
    <div class="info-row"><span class="lbl">Model</span><span class="val">%%PRINTER_MODEL%%</span></div>
    <div class="info-row"><span class="lbl">Access Code</span><span class="val">%%PRINTER_CODE%%</span></div>
  </div>
</div>
</div>
<style>
.info-row{display:flex;justify-content:space-between;padding:6px 0;font-size:13px;border-bottom:1px solid #1c2128}
.info-row:last-child{border:none}
.info-row .lbl{color:#8b949e}
.info-row .val{color:#f0f6fc;font-weight:600;text-align:right}
.info-row .val.up{color:#3fb950}
.info-row .val.mid{color:#d29922}
.info-row .val.down{color:#da3633}
</style>
<footer><a href="https://github.com/VID-PRO/BambuTagger-Gateway" target="_blank">BambuTagger-Gateway v%%VERSION%%</a> &mdash; MIT License</footer>
</body>
</html>
)html";

static String buildRoot() {
  String page = buildPage(_PAGE_ROOT, _PAGE_ROOT2);
  page.replace("%%AP_SSID%%", GATEWAY_AP_SSID);

  if (strlen(_cfg.stationSsid) > 0) {
    bool staUp = WiFi.isConnected();
    page.replace("%%STA_STATUS%%", String("<span class=\"") + (staUp ? "up" : "down") + "\">"
      + _cfg.stationSsid + " " + (staUp ? "&#10003;" : "&#10007;") + "</span>");
    page.replace("%%STA_IP%%", staUp ? WiFi.localIP().toString() : "<span class=\"down\">inactive</span>");
  } else {
    page.replace("%%STA_STATUS%%", "<span class=\"down\">disabled</span>");
    page.replace("%%STA_IP%%", "<span class=\"down\">disabled</span>");
  }

  page.replace("%%PRINTER_HOST%%", _cfg.printerHost);
  page.replace("%%PRINTER_SERIAL%%", _cfg.printerSerial);
  page.replace("%%PRINTER_MODEL%%", _cfg.printerModel);
  page.replace("%%PRINTER_CODE%%", _cfg.printerCode);
  page.replace("%%PRINTER_NAME%%", _displayName);
  {
    const char *mqttCls, *mqttLabel;
    switch (mqtt.getStatus()) {
      case MQTT_UP:     mqttCls = "up";   mqttLabel = "connected";    break;
      case MQTT_TRYING: mqttCls = "mid";  mqttLabel = "connecting";   break;
      default:          mqttCls = "down"; mqttLabel = "disconnected"; break;
    }
    page.replace("%%MQTT_STATUS%%", String("<span class=\"") + mqttCls + "\">" + mqttLabel + "</span>");
  }
  unsigned long secs = millis() / 1000;
  char upt[32];
  snprintf(upt, sizeof(upt), "%dd %02d:%02d:%02d",
    (int)(secs / 86400), (int)((secs % 86400) / 3600),
    (int)((secs % 3600) / 60), (int)(secs % 60));
  page.replace("%%UPTIME%%", upt);
  page.replace("%%HEAP%%", String(ESP.getFreeHeap()) + " B");
  page.replace("%%VERSION%%", VERSION);
  return page;
}

static void handleRoot() {
  _server.send(200, "text/html; charset=utf-8", buildRoot());
}
