#pragma once

static const char _PAGE_SAVED[] PROGMEM = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="15;url=/">
<title>BambuTagger-Gateway</title>
<link rel="icon" href="/Logo/bambutagger.png" type="image/png">
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0d1117;color:#c9d1d9;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;min-height:100vh;padding-bottom:28px}
header{background:#161b22;border-bottom:1px solid #30363d;padding:12px 24px}
header .logo{display:flex;align-items:center;gap:10px}
header .logo img{width:28px;height:28px;flex-shrink:0;border-radius:4px}
header h1{font-size:18px;color:#58a6ff}
.wrapper{display:flex;align-items:center;justify-content:center;padding:40px 16px;text-align:center}
.card{background:#161b22;border:1px solid #30363d;border-radius:6px;padding:40px 32px;max-width:480px}
h1{color:#3fb950;font-size:2em;margin-bottom:10px}
p{color:#8b949e}
footer{position:fixed;bottom:0;left:0;right:0;text-align:center;padding:6px;font-size:10px;color:#484f58;background:#0d1117;border-top:1px solid #30363d}
</style>
</head>
<body>
<header><div class="logo"><img src="/Logo/bambutagger.png" alt="B"><h1>BambuTagger-Gateway</h1></div></header>
<div class="wrapper">
<div class="card">
  <h1>&#10003; Saved!</h1>
  <p>Settings stored. Rebooting&hellip;</p>
  <p style="margin-top:12px;font-size:.85em">Reconnecting in 15 seconds&hellip;</p>
</div>
</div>
<footer>BambuTagger-Gateway v%%VERSION%% &mdash; MIT License</footer>
</body>
</html>
)html";

static bool isValidHost(const String &host) {
  if (host.length() == 0) return true;  // printer not configured yet

  bool ipv4Like = true;
  int dots = 0;
  for (unsigned int i = 0; i < host.length(); i++) {
    char c = host[i];
    if (c == '.') dots++;
    else if (c < '0' || c > '9') { ipv4Like = false; break; }
  }

  if (ipv4Like && dots == 3) {
    int seen = 0, val = 0;
    for (unsigned int i = 0; i <= host.length(); i++) {
      char c = (i < host.length()) ? host[i] : '.';
      if (c >= '0' && c <= '9') {
        val = val * 10 + (c - '0');
      } else if (c == '.') {
        if (i == 0 || host[i-1] == '.') return false;
        if (val > 255) return false;
        seen++;
        val = 0;
      }
    }
    return seen == 4;
  }

  return true;
}

static void sendSaveError(const String &title, const String &msg) {
  String err = FPSTR(_PAGE_SAVED);
  err.replace("&#10003; Saved!", "&#10007; " + title);
  err.replace("<p>Settings stored. Rebooting&hellip;</p>", "<p>" + msg + "</p>");
  err.replace("<meta http-equiv=\"refresh\" content=\"15;url=/\">", "");
  err.replace("<p style=\"margin-top:12px;font-size:.85em\">Reconnecting in 15 seconds&hellip;</p>",
    "<p style=\"margin-top:12px\"><a href=\"/\" style=\"color:#58a6ff\">&larr; Back</a></p>");
  err.replace("%%VERSION%%", VERSION);
  _server.send(400, "text/html; charset=utf-8", err);
}

static void handleSave() {
  if (!authorized()) return;
  String host = _server.arg("printer_host");
  host.trim();
  if (!isValidHost(host)) {
    sendSaveError("Invalid Host", "The printer host &ldquo;" + htmlEsc(host.c_str()) +
      "&rdquo; is not a valid IP address or hostname.");
    return;
  }
  String ssid = _server.arg("sta_ssid");
  if (ssid.length() >= sizeof(_cfg.stationSsid)) {  // bytes: a UTF-8 SSID can pass maxlength
    sendSaveError("SSID Too Long", "WiFi network names are at most 32 bytes.");
    return;
  }
  // Doubles as the setup AP's WPA2 passphrase (8-63 chars); never truncate it.
  String admin = _server.arg("admin_pass");
  if (admin.length() && (admin.length() < 8 || admin.length() >= sizeof(_cfg.adminPass))) {
    sendSaveError("Bad Password Length", "The admin password needs 8 to 63 characters.");
    return;
  }
  if (_server.hasArg("admin_clear")) _cfg.adminPass[0] = 0;
  else if (admin.length()) strlcpy(_cfg.adminPass, admin.c_str(), sizeof(_cfg.adminPass));

  // Blank secret fields keep the saved value; the page never sends them out.
  String pass = _server.arg("sta_pass");
  if (pass.length() || ssid != _cfg.stationSsid)  // new network: blank means open
    strlcpy(_cfg.stationPass, pass.c_str(), sizeof(_cfg.stationPass));
  strlcpy(_cfg.stationSsid, ssid.c_str(), sizeof(_cfg.stationSsid));

  strlcpy(_cfg.printerHost, host.c_str(), sizeof(_cfg.printerHost));
  if (_server.arg("printer_code").length())
    strlcpy(_cfg.printerCode, _server.arg("printer_code").c_str(), sizeof(_cfg.printerCode));
  strlcpy(_cfg.printerSerial, _server.arg("printer_serial").c_str(), sizeof(_cfg.printerSerial));
  strlcpy(_cfg.printerModel, _server.arg("printer_model").c_str(), sizeof(_cfg.printerModel));

  configSave();

  String page = FPSTR(_PAGE_SAVED);
  page.replace("%%VERSION%%", VERSION);
  _server.send(200, "text/html; charset=utf-8", page);
  _server.client().flush();
  _server.client().stop();
  delay(1500);
  ESP.restart();
}
