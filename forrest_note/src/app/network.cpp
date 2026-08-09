#include "Arduino.h"
#include "../../config.h"
#include "../../globals.h"
#include "../../types.h"
#include "network.h"
#include "notes.h"
#include "rtc.h"
#include "ui.h"
#include "config_store.h"
#include "ideashell.h"
#include "WiFi.h"
#include "WiFiClientSecure.h"
#include <WebServer.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include "SD_MMC.h"
#include "esp_heap_caps.h"
#include "../../secrets.h"

// IDF built-in Mozilla CA root bundle (libmbedtls.a). Auto-maintained with the
// esp32 core, so server certs validate without shipping/rotating a pinned PEM.
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");

static String dechunkAsrBody(const String& in) {
  String out;
  int i = 0;
  while (i < (int)in.length()) {
    int eol = in.indexOf('\n', i);
    if (eol < 0) break;
    String sizeLine = in.substring(i, eol);
    int semi = sizeLine.indexOf(';');
    if (semi >= 0) sizeLine = sizeLine.substring(0, semi);
    sizeLine.trim();
    long size = strtol(sizeLine.c_str(), nullptr, 16);
    i = eol + 1;
    if (size <= 0) break;
    if (i + size > (int)in.length()) size = in.length() - i;
    out += in.substring(i, i + size);
    i += size;
    while (i < (int)in.length() && (in[i] == '\r' || in[i] == '\n')) i++;
  }
  return out;
}

static bool transcribeOnce(const String& wavPath, int noteNum) {
  String sfKey = cfg::siliconFlowKey();
  if (sfKey.length() == 0) { Serial.println("[ASR] no SiliconFlow API key set"); return false; }

  File f = SD_MMC.open(wavPath.c_str());
  if (!f) return false;
  size_t fileSize = f.size();

  String bnd = "----PalaBoundary";
  String pre = "--" + bnd + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\nFunAudioLLM/SenseVoiceSmall\r\n"
               "--" + bnd + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"note.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  String post = "\r\n--" + bnd + "--\r\n";
  size_t totalLen = pre.length() + fileSize + post.length();

  WiFiClientSecure client;
  client.setCACertBundle(x509_crt_bundle_start,
                         (size_t)(x509_crt_bundle_end - x509_crt_bundle_start));
  client.setHandshakeTimeout(15);  // seconds

  if (!client.connect("api.siliconflow.cn", 443, 15000 /* ms */)) { f.close(); return false; }

  client.printf("POST /v1/audio/transcriptions HTTP/1.1\r\n"
                "Host: api.siliconflow.cn\r\n"
                "Authorization: Bearer %s\r\n"
                "Content-Type: multipart/form-data; boundary=%s\r\n"
                "Content-Length: %u\r\n"
                "Connection: close\r\n\r\n",
                sfKey.c_str(), bnd.c_str(), (unsigned)totalLen);
  client.print(pre);

  uint8_t* chunk = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_8BIT);
  if (!chunk) { f.close(); client.stop(); return false; }
  while (f.available()) {
    int n = f.read(chunk, 4096);
    if (n <= 0) break;
    client.write(chunk, n);
  }
  heap_caps_free(chunk);
  f.close();
  client.print(post);

  uint32_t deadline = millis() + 90000;
  while (!client.available() && millis() < deadline) delay(20);

  String resp = "";
  bool inBody = false, chunked = false;
  while (client.available() || (client.connected() && millis() < deadline)) {
    if (!client.available()) { delay(10); continue; }
    if (!inBody) {
      String line = client.readStringUntil('\n');
      if (line == "\r" || line == "") inBody = true;
      String low = line; low.toLowerCase();
      if (low.startsWith("transfer-encoding:") && low.indexOf("chunked") >= 0) chunked = true;
      if (line.startsWith("HTTP/") && line.indexOf(" 200 ") < 0) {
        Serial.printf("[ASR] %s\n", line.c_str());
        client.stop(); return false;
      }
    } else {
      while (client.available() && resp.length() <= 131072) resp += (char)client.read();
      if (resp.length() > 131072) break;
    }
  }
  client.stop();
  if (chunked) resp = dechunkAsrBody(resp);

  // Robust JSON parse of {"text":"..."} — handles \uXXXX, escapes, and long
  // transcripts that the old hand-rolled scanner would corrupt or truncate.
  DynamicJsonDocument doc(resp.length() + 1024);
  DeserializationError jerr = deserializeJson(doc, resp);
  if (jerr) { Serial.printf("[ASR] json: %s\n", jerr.c_str()); return false; }
  String text = doc["text"] | "";
  if (text.length() == 0) { Serial.println("[ASR] empty response"); return false; }

  String tp = wavPath; tp.replace(".wav", ".txt");
  File tf = SD_MMC.open(tp.c_str(), FILE_WRITE);
  if (tf) { tf.print(text); tf.close(); }

  updateIndexHasText(noteNum);
  return true;
}

bool transcribe(const String& wavPath, int noteNum) {
  for (int attempt = 0; attempt < 3; attempt++) {
    if (WiFi.status() != WL_CONNECTED) return false;   // offline: keep note queued, don't burn retries
    if (transcribeOnce(wavPath, noteNum)) return true;
    if (attempt < 2) { Serial.printf("[ASR] retry %d/2\n", attempt + 1); delay(3000); }
  }
  return false;
}

// Offline-first queue: notes with hasText==false are the pending work. This drains
// them while online; any that fail (no Wi-Fi, API error) simply stay pending and
// their WAV is preserved for the next sync. Nothing is ever lost on failure.
void transcribeAll() {
  if (!cfg::hasSiliconFlowKey()) { Serial.println("[ASR] no SiliconFlow key; skipping sync"); return; }

  int pending = 0;
  for (int i=0; i<(int)noteIndex.size(); i++) if(!noteIndex[i].hasText) pending++;
  int done = 0;
  for (int i=0; i<(int)noteIndex.size(); i++) {
    if (noteIndex[i].hasText) continue;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.printf("[ASR] wifi lost; %d note(s) stay pending\n", pending - done);
      break;
    }
    showTranscribing(done, pending);
    char wp[64]; snprintf(wp, sizeof(wp), "%s/note_%03d.wav", NOTES_DIR, noteIndex[i].num);
    if (transcribe(String(wp), noteIndex[i].num)) done++;
  }
  Serial.printf("[ASR] synced %d/%d pending\n", done, pending);
}

// ─── Portal helpers ────────────────────────────────────────────────────────

String htmlEscape(const String& s) {
  String out = s;
  out.replace("&", "&amp;"); out.replace("<", "&lt;");
  out.replace(">", "&gt;"); out.replace("\"", "&quot;");
  out.replace("'", "&#39;");
  return out;
}

String readSmallFile(const char* path, size_t maxLen) {
  File f = SD_MMC.open(path);
  if (!f) return "";
  String out;
  while (f.available() && out.length() < maxLen) out += (char)f.read();
  f.close();
  return out;
}

String urlDecodeSimple(String s) {
  s.replace("+", " ");
  String out = "";
  for (int i = 0; i < (int)s.length(); i++) {
    if (s[i] == '%' && i + 2 < (int)s.length()) {
      String hex = s.substring(i + 1, i + 3);
      out += (char)strtol(hex.c_str(), nullptr, 16);
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

String portalCss() {
  return String(
    "<style>"
    "*{box-sizing:border-box;}"
    ":root{font-family:-apple-system,BlinkMacSystemFont,'Inter','Segoe UI',sans-serif;color:#171714;background:#f3f0e9;font-size:15px;}"
    "body{margin:0;padding:18px;background:#f3f0e9;}"
    ".wrap{max-width:680px;margin:0 auto;padding-bottom:24px;}"
    ".top{display:flex;align-items:flex-end;justify-content:space-between;gap:14px;margin:2px 0 18px;}"
    "h1{font-size:38px;letter-spacing:-.055em;line-height:.95;margin:0;font-weight:800;}"
    "h2{font-size:18px;letter-spacing:-.02em;margin:0;}"
    ".sub{font-size:12px;letter-spacing:.06em;color:#6a665f;margin-top:7px;}"
    ".pill,.badge{display:inline-flex;align-items:center;border:1px solid #b9b4aa;border-radius:999px;padding:6px 10px;font-size:12px;background:#fffaf1;white-space:nowrap;}"
    ".badge.ok{border-color:#327454;color:#245d42;background:#edf8f1;}"
    ".badge.muted{color:#756f66;background:#f1eee7;}"
    ".grid{display:grid;grid-template-columns:1fr;gap:12px;}"
    ".card{background:#fffaf1;border:1.25px solid #26251f;border-radius:20px;padding:16px;margin:12px 0;box-shadow:3px 3px 0 #26251f;}"
    ".section-head{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:12px;}"
    ".status-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;}"
    ".status-item{background:#f5f1e8;border-radius:13px;padding:10px;}"
    ".status-label{display:block;color:#777168;font-size:11px;margin-bottom:4px;}"
    ".status-value{font-size:13px;font-weight:700;}"
    ".wifi-slot{border:1px solid #d5d0c7;border-radius:15px;padding:12px;margin-top:10px;background:#fff;}"
    ".wifi-slot.active{border-color:#111;box-shadow:0 0 0 1px #111;}"
    ".slot-head{display:flex;justify-content:space-between;align-items:center;margin-bottom:9px;font-size:13px;font-weight:700;}"
    ".field{display:block;margin:9px 0;}"
    ".field>span{display:block;font-size:12px;color:#5f5a52;margin:0 0 5px;}"
    "input:not([type=checkbox]){appearance:none;width:100%;font:inherit;font-size:16px;padding:11px 12px;border:1px solid #bdb8af;border-radius:12px;background:#fff;color:#111;outline:none;}"
    "input:not([type=checkbox]):focus{border-color:#111;box-shadow:0 0 0 2px rgba(17,17,17,.1);}"
    ".check{display:flex;align-items:center;gap:8px;font-size:13px;margin-top:9px;}"
    ".check input{width:18px;height:18px;margin:0;}"
    "button,.btn{font:inherit;font-weight:650;border:1px solid #111;border-radius:12px;padding:10px 13px;background:#111;color:#fff;text-decoration:none;display:inline-flex;align-items:center;justify-content:center;cursor:pointer;}"
    "button:disabled{opacity:.55;cursor:default;}"
    ".btn.secondary,button.secondary{background:#fffaf1;color:#111;}"
    ".scan-results{display:flex;flex-wrap:wrap;gap:7px;margin:10px 0;}"
    ".network{background:#fff;color:#111;border-color:#c8c2b8;padding:8px 10px;font-size:13px;font-weight:600;}"
    ".network small{font-weight:400;color:#777;margin-left:6px;}"
    ".service{border-top:1px solid #ded9d0;padding-top:13px;margin-top:13px;}"
    ".service:first-of-type{border-top:0;padding-top:0;margin-top:0;}"
    ".service-title{display:flex;align-items:center;justify-content:space-between;gap:8px;font-weight:750;}"
    ".savebar{position:sticky;bottom:10px;margin-top:14px;padding:8px;background:rgba(243,240,233,.94);border-radius:16px;backdrop-filter:blur(8px);}"
    ".savebar button{width:100%;padding:13px;}"
    "details{border-top:1px solid #d8d2c8;margin-top:14px;padding-top:12px;}"
    "summary{font-weight:700;cursor:pointer;}"
    ".row{display:flex;justify-content:space-between;gap:16px;align-items:flex-start;}"
    ".num{font-size:13px;letter-spacing:.08em;text-transform:uppercase;color:#6a665f;margin-bottom:8px;}"
    ".date{font-size:13px;color:#6a665f;margin:-4px 0 12px;}"
    ".title{font-size:24px;line-height:1.05;letter-spacing:-.04em;font-weight:750;margin:0 0 12px;}"
    ".tag{border:1px solid #111;border-radius:999px;padding:5px 9px;font-size:12px;white-space:nowrap;background:#111;color:#fff;}"
    ".text{font-size:15px;line-height:1.45;color:#222;margin:0 0 14px;white-space:pre-wrap;}"
    ".actions{display:flex;flex-wrap:wrap;gap:8px;margin-top:14px;}"
    "a.btn{font-size:13px;background:#f3f0e9;color:#111;}"
    "a.btn.primary{background:#111;color:#fff;}"
    ".empty{border:1.5px dashed #111;border-radius:24px;padding:34px;text-align:center;color:#6a665f;}"
    "audio{width:100%;margin-top:8px;}"
    ".hint{font-size:12px;color:#6f6960;line-height:1.5;margin:8px 0;}"
    "@media(max-width:520px){body{padding:12px}.wrap{max-width:none}.top{align-items:center;margin-bottom:12px}h1{font-size:28px;line-height:1}.sub{font-size:11px}.card{padding:14px;border-radius:17px;margin:10px 0;box-shadow:2px 2px 0 #26251f}.title{font-size:20px}.status-grid{grid-template-columns:1fr 1fr}.status-item:last-child{grid-column:1/-1}.row{gap:10px}.savebar{bottom:6px}}"
    "</style>"
  );
}

// ─── Portal handlers ───────────────────────────────────────────────────────

void handlePortalRoot() {
  loadIndex();

  Serial.println("[HTTP] GET /");
  String filter = "All";
  if (transferServer.hasArg("tag")) filter = transferServer.arg("tag");

  // Stream the page in bounded chunks so RAM use stays flat regardless of how
  // many notes exist (a single accumulated String would grow unboundedly).
  transferServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  transferServer.send(200, "text/html", "");

  String html = "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>记忆面包</title>" + portalCss() + "</head><body><div class='wrap'>";

  html += "<div class='top'><div><h1>记忆<br>面包</h1>"
          "<div class='sub'>本地笔记传输 · <a href=\"/tags\" style=\"color:inherit\">标签</a> · <a href=\"/provision\" style=\"color:inherit\">配置</a> · <a href=\"/ota\" style=\"color:inherit\">更新</a></div></div>"
          "<div class='pill'>" + String((int)noteIndex.size()) + " 条笔记</div></div>";

  html += "<div class='actions' style='margin-bottom:18px'>";
  html += "<a class='btn " + String(filter == "All" ? "primary" : "") + "' href='/'>全部</a>";
  for (int t = 0; t < tagCount; t++) {
    String tag = String(tags[t]);
    html += "<a class='btn " + String(filter == tag ? "primary" : "") + "' href='/?tag=" + tag + "'>" + htmlEscape(tag) + "</a>";
  }
  html += "</div>";

  html += "<div class='actions' style='margin-bottom:24px'>";
  html += "<a class='btn primary' href='/export.txt'>下载全部文字</a>";
  if (filter != "All")
    html += "<a class='btn' href='/export.txt?tag=" + filter + "'>下载“" + htmlEscape(filter) + "”文字</a>";
  html += "</div>";

  int visibleCount = 0;
  for (int i = 0; i < (int)noteIndex.size(); i++)
    if (filter == "All" || filter == String(noteIndex[i].tag)) visibleCount++;

  if (visibleCount <= 0) {
    html += "<div class='empty'>这个分类还没有笔记。</div>";
  } else {
    html += "<div class='grid'>";
    for (int v = 0; v < (int)noteIndex.size(); v++) {
      int i = (int)noteIndex.size() - 1 - v;
      if (!(filter == "All" || filter == String(noteIndex[i].tag))) continue;
      int num = noteIndex[i].num;

      char txtPath[64], wavPath[64];
      snprintf(txtPath, sizeof(txtPath), "%s/note_%03d.txt", NOTES_DIR, num);
      snprintf(wavPath, sizeof(wavPath), "%s/note_%03d.wav", NOTES_DIR, num);

      String transcript = readSmallFile(txtPath, 1200);
      if (transcript.length() == 0)
        transcript = noteIndex[i].hasText ? "（转写内容为空）" : "尚未转写。";

      String title = transcript; title.replace("\n", " "); title.trim();
      if (title.length() > 58) title = title.substring(0, 58) + "...";
      if (title.length() == 0 || title == "尚未转写。")
        title = String("语音笔记 ") + String(num);

      html += "<div class='card'>";
      html += "<div class='row'><div><div class='num'>#" + String(num) + "</div>";
      html += "<h2 class='title'>" + htmlEscape(title) + "</h2>";
      String createdUtc = noteCreatedUtc(num);
      if (createdUtc.length() > 0)
        html += "<div class='date' data-utc='" + createdUtc + "'>" + createdUtc + "</div>";
      else
        html += "<div class='date'>时间未设置</div>";
      html += "</div>";
      html += "<div class='tag'>" + htmlEscape(String(noteIndex[i].tag)) + "</div></div>";
      html += "<p class='text'>" + htmlEscape(transcript) + "</p>";
      if (SD_MMC.exists(wavPath))
        html += "<audio controls src='/audio?num=" + String(num) + "'></audio>";
      html += "<div class='actions'>";
      html += "<a class='btn primary' href='/txt?num=" + String(num) + "'>下载 TXT</a>";
      if (SD_MMC.exists(wavPath))
        html += "<a class='btn' href='/wav?num=" + String(num) + "'>下载 WAV</a>";
      html += "<a class='btn' style='margin-left:auto;color:#c0392b;border-color:#c0392b' "
              "href='/note/delete?num=" + String(num) + "' "
              "onclick=\"return confirm('确认删除笔记 #" + String(num) + "？此操作无法撤销。')\">删除</a>";
      html += "</div></div>";
      if (html.length() > 2048) { transferServer.sendContent(html); html = ""; }
    }
    html += "</div>";
  }

  html += "<script>"
          "document.querySelectorAll('[data-utc]').forEach(function(el){"
          "var d=new Date(el.dataset.utc);"
          "if(!isNaN(d)){el.textContent=d.toLocaleString([],{year:'numeric',month:'short',day:'2-digit',hour:'2-digit',minute:'2-digit'});}"
          "});"
          "</script>";
  html += "</div></body></html>";
  transferServer.sendContent(html);
  transferServer.sendContent("");   // terminate chunked response
}

void handlePortalJson() {
  loadIndex();
  String json = "[";
  for (int v = 0; v < (int)noteIndex.size(); v++) {
    int i = (int)noteIndex.size() - 1 - v;
    if (v > 0) json += ",";
    json += "{";
    json += "\"num\":" + String(noteIndex[i].num) + ",";
    json += "\"tag\":\"" + String(noteIndex[i].tag) + "\",";
    json += "\"hasText\":" + String(noteIndex[i].hasText ? "true" : "false");
    json += "}";
  }
  json += "]";
  transferServer.send(200, "application/json", json);
}

void handleExportTxt() {
  loadIndex();
  String filter = "All";
  if (transferServer.hasArg("tag")) filter = transferServer.arg("tag");

  String filename = "memory_bread_export";
  if (filter != "All") filename += "_" + filter;
  filename += ".txt";

  // Stream chunked so the full export never has to fit in RAM at once (the old
  // path capped at 55 KB and truncated). No cap now — all notes are exported.
  transferServer.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  transferServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  transferServer.send(200, "text/plain", "");

  String chunk = "记忆面包导出\n筛选：" + filter + "\n------------------------------\n\n";

  for (int v = 0; v < (int)noteIndex.size(); v++) {
    int i = (int)noteIndex.size() - 1 - v;
    if (!(filter == "All" || filter == String(noteIndex[i].tag))) continue;
    int num = noteIndex[i].num;
    char txtPath[64]; snprintf(txtPath, sizeof(txtPath), "%s/note_%03d.txt", NOTES_DIR, num);
    String transcript = readSmallFile(txtPath, 4000);
    if (transcript.length() == 0)
      transcript = noteIndex[i].hasText ? "(empty transcript)" : "Not transcribed yet.";
    chunk += "#";
    if (num < 100) chunk += "0";
    if (num < 10)  chunk += "0";
    chunk += String(num) + " · " + String(noteIndex[i].tag) + "\n";
    String createdUtc = noteCreatedUtc(num);
    if (createdUtc.length() > 0) chunk += createdUtc + "\n";
    chunk += "\n" + transcript + "\n\n------------------------------\n\n";
    if (chunk.length() > 2048) { transferServer.sendContent(chunk); chunk = ""; }
  }

  transferServer.sendContent(chunk);
  transferServer.sendContent("");   // terminate chunked response
}

void sendFileByNum(const char* ext, const char* mime, bool attachment) {
  if (!transferServer.hasArg("num")) { transferServer.send(400, "text/plain", "Missing num"); return; }
  int num = transferServer.arg("num").toInt();
  if (num <= 0) { transferServer.send(400, "text/plain", "Invalid num"); return; }
  char path[64]; snprintf(path, sizeof(path), "%s/note_%03d.%s", NOTES_DIR, num, ext);
  File f = SD_MMC.open(path);
  if (!f) { transferServer.send(404, "text/plain", "File not found"); return; }
  if (attachment) {
    String filename = String("note_") + String(num) + "." + String(ext);
    transferServer.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  }
  transferServer.streamFile(f, mime);
  f.close();
}

void handleTagAdd() {
  if (!transferServer.hasArg("name")) {
    transferServer.sendHeader("Location", "/tags?msg=missing");
    transferServer.send(303); return;
  }
  String name = urlDecodeSimple(transferServer.arg("name"));
  bool ok = addCustomTag(name.c_str());
  transferServer.sendHeader("Location", ok ? "/tags?msg=added" : "/tags?msg=exists");
  transferServer.send(303);
}

void handleTagDelete() {
  if (!transferServer.hasArg("name")) {
    transferServer.sendHeader("Location", "/tags?msg=missing");
    transferServer.send(303); return;
  }
  String name = urlDecodeSimple(transferServer.arg("name"));
  bool hadNotes = tagHasNotes(name.c_str());
  bool ok = deleteTag(name.c_str());
  if (ok && hadNotes) transferServer.sendHeader("Location", "/tags?msg=moved");
  else                transferServer.sendHeader("Location", ok ? "/tags?msg=deleted" : "/tags?msg=protected");
  transferServer.send(303);
}

void handleTagsPage() {
  loadTags();
  loadIndex();
  activeFilter = -1;

  String html = "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>记忆面包 · 标签</title>"
                "<style>"
                "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;margin:0;padding:24px;background:#f3f0e9;color:#111}"
                ".wrap{max-width:720px;margin:0 auto}"
                "h1{font-size:42px;line-height:.9;letter-spacing:-.05em;margin:0 0 22px;font-weight:800}"
                ".card{background:#fffaf1;border:1.5px solid #111;border-radius:24px;padding:18px;margin:14px 0;box-shadow:4px 4px 0 #111}"
                ".row{display:flex;justify-content:space-between;align-items:center;gap:12px;border-top:1px solid #ddd;padding:12px 0}"
                ".row:first-child{border-top:0}"
                ".tag{font-size:20px;font-weight:700}"
                ".meta{font-size:13px;color:#666;margin-top:4px}"
                "input{font:inherit;padding:12px;border:1.5px solid #111;border-radius:999px;background:#fff;width:100%;box-sizing:border-box}"
                "button,.btn{font:inherit;border:1.5px solid #111;border-radius:999px;padding:10px 14px;background:#111;color:#fff;text-decoration:none;white-space:nowrap}"
                ".danger{background:#fffaf1;color:#111}"
                ".msg{border:1.5px solid #111;border-radius:18px;padding:12px 14px;background:#fff;margin:12px 0}"
                ".hint{font-size:13px;color:#666;line-height:1.4}"
                "form.add{display:flex;gap:10px}"
                "</style></head><body><div class='wrap'>";

  html += "<h1>记忆<br>标签</h1>";
  html += "<a class='btn' href='/'>返回笔记</a>";

  if (transferServer.hasArg("msg")) {
    String msg = transferServer.arg("msg");
    html += "<div class='msg'>";
    if (msg == "added") html += "标签已添加。";
    else if (msg == "exists")    html += "标签已存在或无法添加。";
    else if (msg == "deleted")   html += "标签已删除。";
    else if (msg == "moved")     html += "标签已删除，原有笔记已移至未分类。";
    else if (msg == "protected") html += "这个标签不能删除。";
    else html += "请输入标签名。";
    html += "</div>";
  }

  html += "<div class='card'><form class='add' action='/tag/add' method='get'>"
          "<input name='name' maxlength='31' placeholder='新标签名称'>"
          "<button type='submit'>添加</button></form>"
          "<p class='hint'>录音后可在设备上选择标签。为了适配墨水屏，请尽量使用短名称。</p></div>";

  html += "<div class='card'>";
  for (int i = 0; i < tagCount; i++) {
    int cnt = 0;
    for (int n = 0; n < (int)noteIndex.size(); n++)
      if (strcmp(noteIndex[n].tag, tags[i]) == 0) cnt++;
    html += "<div class='row'><div><div class='tag'>" + htmlEscape(String(tags[i])) + "</div>";
    html += "<div class='meta'>" + String(cnt) + " 条笔记";
    if (cnt > 0) html += " · 删除标签后移至未分类";
    html += "</div></div>";
    if (strcasecmp(tags[i], "Untagged") != 0) {
      html += "<a class='btn danger' href='/tag/delete?name=" + htmlEscape(String(tags[i])) + "' "
              "onclick=\"return confirm('删除这个标签？笔记不会删除，并会移至未分类。');\">删除</a>";
    }
    html += "</div>";
  }
  html += "</div></div></body></html>";
  transferServer.send(200, "text/html", html);
}

void handleNoteDelete() {
  if (!transferServer.hasArg("num")) { transferServer.send(400, "text/plain", "Missing num"); return; }
  int num = transferServer.arg("num").toInt();
  if (num <= 0) { transferServer.send(400, "text/plain", "Invalid num"); return; }
  deleteNote(num);
  transferServer.sendHeader("Location", "/");
  transferServer.send(303);
}

void handleWifiScan() {
  Serial.println("[WiFi] scan requested");
  int count = WiFi.scanNetworks(false, false, false, 300U);

  DynamicJsonDocument doc(6144);
  doc["ok"] = count >= 0;
  JsonArray networks = doc.createNestedArray("networks");
  if (count > 0) {
    int limit = min(count, 24);
    for (int i = 0; i < limit; i++) {
      String ssid = WiFi.SSID(i);
      if (!ssid.length()) continue;
      bool duplicate = false;
      for (JsonObject existing : networks) {
        if (existing["ssid"].as<String>() == ssid) { duplicate = true; break; }
      }
      if (duplicate) continue;
      JsonObject network = networks.createNestedObject();
      network["ssid"] = ssid;
      network["rssi"] = WiFi.RSSI(i);
      network["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
  }
  WiFi.scanDelete();

  String json;
  serializeJson(doc, json);
  transferServer.sendHeader("Cache-Control", "no-store");
  transferServer.send(count >= 0 ? 200 : 503, "application/json", json);
}

void handleProvisionPage() {
  Serial.println("[HTTP] GET /provision");
  String html = "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>记忆面包 · 配置</title>" + portalCss() + "</head><body><div class='wrap'>";
  html += "<div class='top'><div><h1>设备配置</h1>"
          "<div class='sub'>记忆面包 · 网络与服务</div></div>"
          "<span class='pill'>手机端</span></div>";

  html += "<div class='card'><div class='status-grid'>";
  html += "<div class='status-item'><span class='status-label'>Wi-Fi</span><span class='status-value'>" +
          String(cfg::wifiCount()) + " / 3 已配置</span></div>";
  html += "<div class='status-item'><span class='status-label'>AI 转写</span><span class='status-value'>" +
          String(cfg::hasSiliconFlowKey() ? "已配置" : "未设置") + "</span></div>";
  html += "<div class='status-item'><span class='status-label'>闪念贝壳</span><span class='status-value'>" +
          String(cfg::hasIdeashell() ? "已开启" : (cfg::ideashellToken().length() ? "已设置" : "未设置")) +
          "</span></div></div></div>";

  html += "<form action='/provision/save' method='post'>";
  html += "<section class='card'><div class='section-head'><h2>Wi-Fi 网络</h2><span class='badge'>最多 3 组</span></div>";
  html += "<p class='hint'>先点选要填写的网络栏位，再从扫描结果中选择。密码留空会保留同名网络的原密码。</p>";
  html += "<button id='scanBtn' class='secondary' type='button'>扫描附近网络</button>";
  html += "<span id='scanStatus' class='hint' style='margin-left:8px'>准备扫描</span>";
  html += "<div id='wifiResults' class='scan-results' aria-live='polite'></div>";
  for (uint8_t i = 0; i < cfg::MAX_WIFI_NETWORKS; i++) {
    String n = String(i);
    String saved = cfg::wifiSsid(i);
    html += "<div class='wifi-slot' data-slot='" + n + "'><div class='slot-head'><span>网络 " + String(i + 1) + "</span>";
    html += saved.length() ? "<span class='badge ok'>已保存</span>" : "<span class='badge muted'>空</span>";
    html += "</div><label class='field'><span>Wi-Fi 名称</span>";
    html += "<input class='ssid-input' id='ssid" + n + "' name='ssid" + n + "' autocomplete='off' autocapitalize='none' placeholder='点击这里，再选择扫描结果' value='" + htmlEscape(saved) + "'></label>";
    html += "<label class='field'><span>密码</span><input id='pass" + n + "' name='pass" + n + "' type='password' autocomplete='new-password' placeholder='留空保持原密码'></label>";
    if (saved.length()) html += "<label class='check'><input type='checkbox' name='clear_wifi" + n + "' value='1'>删除这组网络</label>";
    html += "</div>";
  }
  html += "</section>";

  html += "<section class='card'><div class='section-head'><h2>AI 转写</h2><span class='badge " +
          String(cfg::hasSiliconFlowKey() ? "ok'>已配置" : "muted'>未设置") + "</span></div>";
  html += "<label class='field'><span>硅基流动 API Key</span><input name='siliconflow' type='password' autocomplete='new-password' placeholder='sk-...（留空保持不变）'></label>";
  html += "<p class='hint'>用于语音转写、标题和摘要生成。密钥只保存在设备 NVS 中。</p></section>";

  html += "<section class='card'><div class='section-head'><h2>同步服务</h2><span class='badge'>可选</span></div>";
  html += "<div class='service'><div class='service-title'><span>闪念贝壳 MCP</span><span class='badge " +
          String(cfg::hasIdeashell() ? "ok'>已开启" : "muted'>未开启") + "</span></div>";
  html += "<label class='field'><span>Bearer Token</span><input name='ideashell_token' type='password' autocomplete='new-password' placeholder='留空保持现有 Token'></label>";
  html += "<label class='check'><input type='checkbox' name='ideashell_on' value='1'" + String(cfg::ideashellEnabled() ? " checked" : "") + ">转写后自动写入闪念贝壳</label>";
  html += "<p class='hint'>上传标题、摘要、正文和标签；原始 WAV 继续保存在 microSD。</p>";
  html += "<a class='btn secondary' href='/ideashell/test'>测试连接</a></div>";
  html += "<div class='service'><div class='service-title'><span>飞书</span><span class='badge muted'>规划中</span></div>";
  html += "<p class='hint'>预留飞书文档/多维表格接入位置。当前版本尚未启用，不需要填写 Token。</p></div>";

  html += "<details><summary>旧版 GitHub / Obsidian 设置</summary>";
  html += "<p class='hint'>仅为兼容已有用户保留；新用户可以忽略。</p>";
  html += "<label class='field'><span>仓库</span><input name='gh_repo' placeholder='所有者/仓库名' value='" + htmlEscape(cfg::githubRepo()) + "'></label>";
  html += "<label class='field'><span>分支</span><input name='gh_branch' placeholder='main' value='" + htmlEscape(cfg::githubBranch()) + "'></label>";
  html += "<label class='field'><span>笔记目录</span><input name='gh_dir' placeholder='VoiceNotes' value='" + htmlEscape(cfg::githubDir()) + "'></label>";
  html += "<label class='field'><span>GitHub Token</span><input name='gh_token' type='password' autocomplete='new-password' placeholder='留空保持不变'></label>";
  html += "<label class='check'><input type='checkbox' name='gh_on' value='1'" + String(cfg::githubEnabled() ? " checked" : "") + ">开启 GitHub 同步</label>";
  html += "<label class='check'><input type='checkbox' name='gh_ai' value='1'" + String(cfg::githubAiEnrich() ? " checked" : "") + ">AI 生成标题和摘要</label></details></section>";

  html += "<div class='savebar'><button type='submit'>保存全部设置</button></div></form>";
  html += "<div class='actions'><a class='btn secondary' href='/'>返回笔记</a></div>";
  html += "<script>"
          "(()=>{const inputs=[...document.querySelectorAll('.ssid-input')];"
          "const slots=[...document.querySelectorAll('.wifi-slot')];let active=inputs.findIndex(i=>!i.value.trim());if(active<0)active=0;"
          "function setActive(i){active=i;slots.forEach((s,n)=>s.classList.toggle('active',n===i));}"
          "inputs.forEach((input,i)=>{input.addEventListener('focus',()=>setActive(i));input.addEventListener('click',()=>setActive(i));});setActive(active);"
          "const btn=document.getElementById('scanBtn'),status=document.getElementById('scanStatus'),box=document.getElementById('wifiResults');"
          "async function scan(){btn.disabled=true;btn.textContent='正在扫描…';status.textContent='请稍候';box.replaceChildren();"
          "try{const res=await fetch('/wifi/scan',{cache:'no-store'});const data=await res.json();if(!res.ok||!data.ok)throw new Error();"
          "if(!data.networks.length){status.textContent='没有发现网络';return;}status.textContent='发现 '+data.networks.length+' 个网络';"
          "data.networks.forEach(n=>{const b=document.createElement('button');b.type='button';b.className='network';"
          "const strength=n.rssi>=-55?'强':n.rssi>=-70?'中':'弱';b.textContent=n.ssid+'  '+strength+(n.secure?' · 加密':' · 开放');"
          "b.addEventListener('click',()=>{inputs[active].value=n.ssid;document.getElementById('pass'+active).focus();status.textContent='已填入网络 '+(active+1);});box.appendChild(b);});}"
          "catch(e){status.textContent='扫描失败，请点按钮重试';}finally{btn.disabled=false;btn.textContent='重新扫描';}}"
          "btn.addEventListener('click',scan);setTimeout(scan,350);})();"
          "</script>";
  html += "</div></body></html>";
  transferServer.send(200, "text/html", html);
}

void handleProvisionSave() {
  String key  = transferServer.hasArg("siliconflow") ? transferServer.arg("siliconflow") : "";
  String ideaToken = transferServer.hasArg("ideashell_token") ? transferServer.arg("ideashell_token") : "";
  key.trim(); ideaToken.trim();
  bool changed = false;
  for (uint8_t i = 0; i < cfg::MAX_WIFI_NETWORKS; i++) {
    String n = String(i);
    if (transferServer.hasArg("clear_wifi" + n)) {
      cfg::clearWifi(i); changed = true; continue;
    }
    if (!transferServer.hasArg("ssid" + n)) continue;
    String ssid = transferServer.arg("ssid" + n); ssid.trim();
    String pass = transferServer.hasArg("pass" + n) ? transferServer.arg("pass" + n) : "";
    if (!ssid.length()) continue;
    if (!pass.length() && ssid == cfg::wifiSsid(i)) pass = cfg::wifiPass(i);
    cfg::setWifi(i, ssid, pass); changed = true;
  }
  if (key.length()  > 0) { cfg::setSiliconFlowKey(key); changed = true; }
  if (ideaToken.length() > 0) { cfg::setIdeashellToken(ideaToken); changed = true; }
  cfg::setIdeashellEnabled(transferServer.hasArg("ideashell_on"));

  // GitHub vault fields
  if (transferServer.hasArg("gh_repo")) {
    String r = transferServer.arg("gh_repo"); r.trim();
    if (r.length() > 0) { cfg::setGithubRepo(r); changed = true; }
  }
  if (transferServer.hasArg("gh_branch")) {
    String b = transferServer.arg("gh_branch"); b.trim();
    if (b.length() > 0) { cfg::setGithubBranch(b); changed = true; }
  }
  if (transferServer.hasArg("gh_dir")) {
    String d = transferServer.arg("gh_dir"); d.trim();
    if (d.length() > 0) { cfg::setGithubDir(d); changed = true; }
  }
  if (transferServer.hasArg("gh_token")) {
    String t = transferServer.arg("gh_token"); t.trim();
    if (t.length() > 0) { cfg::setGithubToken(t); changed = true; }
  }
  // checkboxes only POST when checked → presence = on, absence = off
  cfg::setGithubEnabled(transferServer.hasArg("gh_on"));
  cfg::setGithubAiEnrich(transferServer.hasArg("gh_ai"));
  changed = true;

  String html = "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>记忆面包 · 配置</title>" + portalCss() + "</head><body><div class='wrap'>";
  html += "<div class='card'><h1>" + String(changed ? "已保存" : "没有更改") + "</h1>";
  html += "<p class='hint'>" + String(changed
            ? "设置已写入设备。重新进入传输或同步即可使用。"
            : "没有提交任何内容。") + "</p>";
  html += "<a class='btn' href='/provision'>返回配置</a></div></div></body></html>";
  transferServer.send(200, "text/html", html);
}

void handleIdeashellTest() {
  String error;
  bool ok = ideashellTestConnection(error);
  String html = "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>记忆面包 · 闪念贝壳测试</title>" + portalCss() + "</head><body><div class='wrap'>";
  html += "<div class='card'><h1>" + String(ok ? "连接成功" : "连接失败") + "</h1>";
  html += "<p class='hint'>" + String(ok ? "MCP 握手、认证和只读工具调用均正常。" : htmlEscape(error)) + "</p>";
  html += "<a class='btn' href='/provision'>返回配置</a></div></div></body></html>";
  transferServer.send(ok ? 200 : 502, "text/html", html);
}

void handleOtaPage() {
  String html = "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>记忆面包 · 固件更新</title>" + portalCss() + "</head><body><div class='wrap'>";
  html += "<div class='top'><div><h1>记忆<br>更新</h1>"
          "<div class='sub'>firmware " FW_VERSION "</div></div></div>";
  html += "<div class='card'>";
  html += "<p class='hint'>粘贴已编译固件 .bin 的 HTTPS 地址。设备会校验证书，写入备用分区并自动重启；启动失败时会自动回滚。</p>";
  html += "<form action='/ota/run' method='post'>"
          "<p><input name='url' placeholder='https://host/forrest-note.bin'></p>"
          "<button type='submit'>更新固件</button></form></div>";
  html += "<a class='btn' href='/'>返回笔记</a>";
  html += "</div></body></html>";
  transferServer.send(200, "text/html", html);
}

void handleOtaRun() {
  if (!transferServer.hasArg("url") || transferServer.arg("url").length() == 0) {
    transferServer.send(400, "text/plain", "Missing url");
    return;
  }
  String url = transferServer.arg("url");
  transferServer.send(200, "text/html",
    "<!doctype html><meta charset='utf-8'><h1>Updating&hellip;</h1>"
    "<p>Flashing firmware. The device reboots automatically if the update succeeds. "
    "If it fails it stays on the current version &mdash; reopen Transfer and retry.</p>");
  delay(250);

  WiFiClientSecure client;
  client.setCACertBundle(x509_crt_bundle_start,
                         (size_t)(x509_crt_bundle_end - x509_crt_bundle_start));
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return r = httpUpdate.update(client, url, FW_VERSION);
  if (r == HTTP_UPDATE_FAILED)
    Serial.printf("[OTA] failed (%d): %s\n",
                  httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
  else if (r == HTTP_UPDATE_NO_UPDATES)
    Serial.println("[OTA] no update available");
}

void setupTransferServer() {
  transferServer.on("/", HTTP_GET, handlePortalRoot);
  transferServer.on("/provision", HTTP_GET, handleProvisionPage);
  transferServer.on("/wifi/scan", HTTP_GET, handleWifiScan);
  transferServer.on("/provision/save", HTTP_POST, handleProvisionSave);
  transferServer.on("/ideashell/test", HTTP_GET, handleIdeashellTest);
  transferServer.on("/ota", HTTP_GET, handleOtaPage);
  transferServer.on("/ota/run", HTTP_POST, handleOtaRun);
  transferServer.on("/tags", HTTP_GET, handleTagsPage);
  transferServer.on("/tag/add", HTTP_GET, handleTagAdd);
  transferServer.on("/tag/delete", HTTP_GET, handleTagDelete);
  transferServer.on("/note/delete", HTTP_GET, handleNoteDelete);
  transferServer.on("/api/notes", HTTP_GET, handlePortalJson);
  transferServer.on("/export.txt", HTTP_GET, handleExportTxt);
  transferServer.on("/txt",   HTTP_GET, [](){ sendFileByNum("txt", "text/plain", true); });
  transferServer.on("/wav",   HTTP_GET, [](){ sendFileByNum("wav", "audio/wav",  true); });
  transferServer.on("/audio", HTTP_GET, [](){ sendFileByNum("wav", "audio/wav",  false); });
  transferServer.onNotFound([](){
    Serial.printf("[HTTP] miss: %s\n", transferServer.uri().c_str());
    if (captivePortalActive) {
      // Captive portal: bounce any unknown URL (incl. the OS connectivity probe)
      // to the setup page so it opens automatically.
      transferServer.sendHeader("Location", "http://" + transferUrl + "/provision", true);
      transferServer.send(302, "text/plain", "");
    } else {
      transferServer.send(404, "text/plain", "Not found");
    }
  });
}

void stopTransferMode() {
  if (transferServerActive) {
    transferServer.stop();
    transferServerActive = false;
  }
  if (captivePortalActive) {
    dnsServer.stop();
    captivePortalActive = false;
  }
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  transferUrl = "";
}
