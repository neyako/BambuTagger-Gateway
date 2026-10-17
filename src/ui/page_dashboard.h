#pragma once

// The whole web UI is this one page. Secrets (WiFi password, printer access
// code) are never sent back to the browser: the UI has no login and the setup
// AP is open.

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
.row{display:flex;justify-content:space-between;gap:12px;padding:6px 0;font-size:13px;border-bottom:1px solid #1c2128}
.row>span:first-child{color:#8b949e}
.row>span:last-child{color:#f0f6fc;font-weight:600;text-align:right;word-break:break-word}
.up{color:#3fb950}.mid{color:#d29922}.down{color:#da3633}
section+section{margin-top:28px}
</style>
</head>
<body>
<header><div class="logo"><img src="/Logo/bambutagger.png" alt=""><h1>BambuTagger-Gateway</h1></div></header>
<div class="wrapper"><div class="card">
<section>
  <h2>Status</h2>
  <div class="row"><span>WiFi</span><span>%%STA_STATUS%%</span></div>
  <div class="row"><span>Printer link</span><span>%%MQTT_STATUS%%</span></div>
  <div class="row"><span>Local clients</span><span>%%CLIENTS%%</span></div>
  <div class="row"><span>Uptime</span><span>%%UPTIME%%</span></div>
  <div class="row"><span>Last reset</span><span>%%RESET%%</span></div>
  <div class="row"><span>Free heap</span><span>%%HEAP%%</span></div>
</section>
<section>
  <h2>Gateway</h2>
  <div class="row"><span>IP</span><span>%%IP%%</span></div>
  <div class="row"><span>Name</span><span>%%NAME%%</span></div>
  <div class="row"><span>Serial</span><span>%%GW_SERIAL%%</span></div>
  <div class="row"><span>Model</span><span>%%MODEL%%</span></div>
  <p class="hint" style="margin-top:10px">In Bambu Studio add a LAN printer with this IP and your printer's access code.
  Ports: 8883 MQTT (TLS), 1883 MQTT (plain), 6000 camera, 990 FTPS. The printer needs LAN mode on.</p>
</section>
<section>
  <h2>Settings</h2>
  <form method="post" action="/save">
    <label>WiFi SSID</label>
    <input name="sta_ssid" value="%%STA_SSID%%" placeholder="Empty = setup AP only" maxlength="32">
    <label>WiFi password</label>
    <input name="sta_pass" type="password" placeholder="%%PASS_HINT%%" maxlength="63" autocomplete="new-password">
    <label>Printer IP / hostname</label>
    <input name="printer_host" value="%%PRINTER_HOST%%" placeholder="e.g. 192.168.1.100" maxlength="63">
    <p class="hint">Printer: Settings &rarr; Network &rarr; IP Address</p>
    <label>Printer access code</label>
    <input name="printer_code" type="password" placeholder="%%CODE_HINT%%" maxlength="31" autocomplete="new-password">
    <p class="hint">Printer: Settings &rarr; Network &rarr; Access Code</p>
    <label>Printer serial</label>
    <input name="printer_serial" value="%%PRINTER_SERIAL%%" maxlength="31">
    <p class="hint">Printer: Settings &rarr; General &rarr; Machine Info</p>
    <label>Printer model</label>
    <select name="printer_model">%%MODEL_OPTIONS%%</select>
    <label>Admin password</label>
    <input name="admin_pass" type="password" placeholder="%%ADMIN_HINT%%" minlength="8" maxlength="63" autocomplete="new-password">
    <p class="hint">Protects this page (user <b>admin</b>), OTA updates and the setup WiFi. Min 8 characters. Forgot it? Reflash over USB.</p>
    %%ADMIN_CLEAR%%
    <button type="submit">Save &amp; Reboot</button>
  </form>
</section>
<section>
  <h2>Firmware</h2>
  <div class="row"><span>Installed</span><span>%%VERSION%%</span></div>
  <div class="row"><span>Latest</span><span id="lat">&mdash;</span></div>
  <button type="button" id="chk">Check for update</button>
  <form method="post" action="/update/github" onsubmit="return confirm('Install the update? The gateway reboots.')">
    <button id="inst" hidden>Install</button>
  </form>
</section>
</div></div>
<footer><a href="https://github.com/VID-PRO/BambuTagger-Gateway" target="_blank">BambuTagger-Gateway v%%VERSION%%</a> &mdash; MIT License</footer>
<script>
document.getElementById('chk').onclick=function(){
  var b=this,i=document.getElementById('inst'),l=document.getElementById('lat');
  b.disabled=true;b.textContent='Checking…';
  fetch('/api/release').then(function(r){return r.json()}).then(function(r){
    l.textContent=r.tag;
    if(r.update){i.textContent='Install '+r.tag;i.hidden=false;b.hidden=true}
    else{b.textContent=r.tag=='?'?'Check failed (needs WiFi)':'Up to date';b.disabled=false}
  }).catch(function(){b.textContent='Check failed';b.disabled=false});
};
</script>
</body>
</html>
)html";

// Why the gateway last restarted; after a crash also the stored dump summary
// (decode with xtensa-esp32s3-elf-addr2line -pfiaC -e firmware.elf <bt...>).
static String lastReset() {
  static const char *names[] = {"unknown", "power-on", "reset pin", "restart", "crash",
    "interrupt watchdog", "task watchdog", "watchdog", "deep sleep", "brownout", "SDIO"};
  esp_reset_reason_t r = esp_reset_reason();
  String s = r < sizeof(names) / sizeof(*names) ? names[r] : names[0];
  bool crashed = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
  esp_core_dump_summary_t d;
  if (crashed && esp_core_dump_get_summary(&d) == ESP_OK) {  // dumps persist; show only this one
    char elf[9] = {};
    memcpy(elf, d.app_elf_sha256, 8);
    s += String("<br><small class=\"down\">") + htmlEsc(d.exc_task) + " pc 0x" + String(d.exc_pc, HEX)
       + " cause " + d.ex_info.exc_cause + " bt";
    for (uint32_t i = 0; i < d.exc_bt_info.depth && i < 16; i++) s += " 0x" + String(d.exc_bt_info.bt[i], HEX);
    s += String(" (build ") + elf + ")</small>";
  }
  return s;
}

static String buildRoot() {
  String page = buildPage(_PAGE_ROOT, _PAGE_ROOT2);
  bool staUp = WiFi.isConnected();
  bool apUp = WiFi.getMode() & WIFI_MODE_AP;

  String sta;
  if (_cfg.stationSsid[0]) {
    sta = String("<span class=\"") + (staUp ? "up\">" : "down\">") + htmlEsc(_cfg.stationSsid)
        + (staUp ? " &#10003;" : " &#10007; retrying") + "</span>";
  } else {
    sta = "<span class=\"down\">not configured</span>";
  }
  if (apUp) sta += " &middot; setup AP on";
  page.replace("%%STA_STATUS%%", sta);

  {
    String link;
    switch (mqtt.getStatus()) {
      case MQTT_UP:     link = "<span class=\"up\">connected</span>"; break;
      case MQTT_TRYING: link = "<span class=\"mid\">connecting</span>"; break;
      default:          link = "<span class=\"down\">disconnected</span>"; break;
    }
    if (mqtt.getStatus() != MQTT_UP && mqtt.upstreamError()[0])
      link += String("<br><small class=\"down\">") + htmlEsc(mqtt.upstreamError()) + "</small>";
    page.replace("%%MQTT_STATUS%%", link);
  }

  {
    // MQTT clients must log in with the printer's access code.
    String cl = String(mqtt.clientCount()) + " &middot; camera " + camProxy.clientCount() + " &middot; FTPS "
              + ftpsProxy.clientCount() + " &middot; " + freeSockets() + " of 16 sockets free";
    if (mqtt.authRejects())
      cl += String(" &middot; <span class=\"down\">") + mqtt.authRejects() + " rejected login(s), last from "
          + mqtt.lastRejectIp().toString() + "</span>";
    page.replace("%%CLIENTS%%", cl);
  }

  unsigned long secs = millis() / 1000;
  char upt[32];
  snprintf(upt, sizeof(upt), "%dd %02d:%02d:%02d",
    (int)(secs / 86400), (int)((secs % 86400) / 3600),
    (int)((secs % 3600) / 60), (int)(secs % 60));
  page.replace("%%UPTIME%%", upt);
  page.replace("%%RESET%%", lastReset());
  page.replace("%%HEAP%%", String(ESP.getFreeHeap() / 1024) + " KB internal &middot; "
                         + String(ESP.getFreePsram() / 1024) + " KB PSRAM");

  page.replace("%%IP%%", staUp ? WiFi.localIP().toString()
                       : apUp ? WiFi.softAPIP().toString() + " (setup AP)" : String("&mdash;"));
  page.replace("%%NAME%%", htmlEsc(_displayName));
  page.replace("%%GW_SERIAL%%", htmlEsc(_cfg.gatewaySerial));
  page.replace("%%MODEL%%", htmlEsc(_cfg.printerModel));

  page.replace("%%STA_SSID%%", htmlEsc(_cfg.stationSsid));
  page.replace("%%PASS_HINT%%", _cfg.stationPass[0] ? "Saved &mdash; leave blank to keep" : "WiFi password");
  page.replace("%%PRINTER_HOST%%", htmlEsc(_cfg.printerHost));
  page.replace("%%CODE_HINT%%", _cfg.printerCode[0] ? "Saved &mdash; leave blank to keep" : "8-digit access code");
  page.replace("%%PRINTER_SERIAL%%", htmlEsc(_cfg.printerSerial));
  page.replace("%%ADMIN_CLEAR%%", _cfg.adminPass[0]
    ? "<label style=\"display:flex;gap:8px;align-items:center;margin-bottom:10px\"><input type=\"checkbox\" name=\"admin_clear\" style=\"width:auto;margin:0\">Remove admin password</label>"
    : "");
  page.replace("%%ADMIN_HINT%%", _cfg.adminPass[0] ? "Saved &mdash; leave blank to keep" : "Not set &mdash; anyone on the network can change settings");

  String opts;
  const char *models[] = {"P1S", "P1P", "P2S", "X1C", "X1E", "A1", "A1 Mini", "A2L"};
  for (auto m : models) {
    opts += "<option";
    if (strcmp(_cfg.printerModel, m) == 0) opts += " selected";
    opts += ">";
    opts += m;
    opts += "</option>";
  }
  page.replace("%%MODEL_OPTIONS%%", opts);

  page.replace("%%VERSION%%", VERSION);
  return page;
}

static void handleRoot() {
  if (!authorized()) return;
  _server.send(200, "text/html; charset=utf-8", buildRoot());
}
