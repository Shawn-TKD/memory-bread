#include "Arduino.h"
#include "ideashell.h"
#include "../../config.h"
#include "config_store.h"
#include "notes.h"
#include "ui.h"
#include "../../globals.h"
#include "WiFi.h"
#include "WiFiClientSecure.h"
#include "SD_MMC.h"
#include <ArduinoJson.h>

extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");

namespace {
const char* IDEA_HOST = "api.ideashell.cn";
const char* IDEA_PATH = "/ideashell/mcp";

struct HttpReply {
  int status = 0;
  String body;
  String session;
};

String dechunk(const String& in) {
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

bool postJson(const char* host, const char* path, const String& bearer,
              const String& session, const String& body, HttpReply& reply,
              bool acceptSse = false, uint32_t timeoutMs = 45000) {
  WiFiClientSecure client;
  client.setCACertBundle(x509_crt_bundle_start,
                         (size_t)(x509_crt_bundle_end - x509_crt_bundle_start));
  client.setHandshakeTimeout(15);
  if (!client.connect(host, 443, 15000)) return false;

  client.printf("POST %s HTTP/1.1\r\nHost: %s\r\n", path, host);
  client.printf("Authorization: Bearer %s\r\n", bearer.c_str());
  client.print("Content-Type: application/json\r\n");
  if (acceptSse) client.print("Accept: application/json, text/event-stream\r\n");
  else           client.print("Accept: application/json\r\n");
  if (session.length()) client.printf("Mcp-Session-Id: %s\r\n", session.c_str());
  client.printf("Content-Length: %u\r\nConnection: close\r\n\r\n", (unsigned)body.length());
  client.print(body);

  uint32_t deadline = millis() + timeoutMs;
  while (!client.available() && millis() < deadline) delay(10);

  bool inBody = false, chunked = false;
  reply = HttpReply();
  while (client.available() || (client.connected() && millis() < deadline)) {
    if (!client.available()) { delay(5); continue; }
    if (!inBody) {
      String line = client.readStringUntil('\n');
      if (line.startsWith("HTTP/")) {
        int sp = line.indexOf(' ');
        if (sp > 0) reply.status = line.substring(sp + 1, sp + 4).toInt();
      }
      String low = line; low.toLowerCase();
      if (low.startsWith("mcp-session-id:")) {
        reply.session = line.substring(line.indexOf(':') + 1); reply.session.trim();
      }
      if (low.startsWith("transfer-encoding:") && low.indexOf("chunked") >= 0) chunked = true;
      if (line == "\r" || line == "") inBody = true;
    } else {
      while (client.available() && reply.body.length() < 131072) reply.body += (char)client.read();
    }
  }
  client.stop();
  if (chunked) reply.body = dechunk(reply.body);
  return reply.status > 0;
}

bool beginMcpSession(String& session, String& error) {
  DynamicJsonDocument doc(512);
  doc["jsonrpc"] = "2.0";
  doc["id"] = 1;
  doc["method"] = "initialize";
  JsonObject params = doc.createNestedObject("params");
  params["protocolVersion"] = "2025-06-18";
  params.createNestedObject("capabilities");
  JsonObject info = params.createNestedObject("clientInfo");
  info["name"] = "MemoryBread";
  info["version"] = "1.0";
  String body; serializeJson(doc, body);

  HttpReply init;
  if (!postJson(IDEA_HOST, IDEA_PATH, cfg::ideashellToken(), "", body, init, true)) {
    error = "无法连接 MCP"; return false;
  }
  if (init.status < 200 || init.status >= 300 || !init.session.length()) {
    error = "MCP 初始化失败 HTTP " + String(init.status); return false;
  }
  session = init.session;

  body = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\",\"params\":{}}";
  HttpReply ready;
  if (!postJson(IDEA_HOST, IDEA_PATH, cfg::ideashellToken(), session, body, ready, true) ||
      ready.status < 200 || ready.status >= 300) {
    error = "MCP 会话确认失败"; return false;
  }
  return true;
}

bool mcpCall(const String& session, const char* tool, JsonObjectConst args,
             String& response, String& error) {
  DynamicJsonDocument doc(2048 + measureJson(args));
  doc["jsonrpc"] = "2.0";
  doc["id"] = 2;
  doc["method"] = "tools/call";
  JsonObject params = doc.createNestedObject("params");
  params["name"] = tool;
  params["arguments"] = args;
  String body; serializeJson(doc, body);

  HttpReply reply;
  if (!postJson(IDEA_HOST, IDEA_PATH, cfg::ideashellToken(), session, body, reply, true)) {
    error = "MCP 请求失败"; return false;
  }
  response = reply.body;
  if (reply.status < 200 || reply.status >= 300) {
    error = "MCP HTTP " + String(reply.status); return false;
  }
  if (reply.body.indexOf("\"isError\":true") >= 0 ||
      reply.body.indexOf("\"error\":") >= 0) {
    error = "闪念贝壳返回错误"; return false;
  }
  return true;
}

String readTranscript(int num) {
  char path[64];
  snprintf(path, sizeof(path), "%s/note_%03d.txt", NOTES_DIR, num);
  File f = SD_MMC.open(path);
  if (!f) return "";
  String text;
  while (f.available() && text.length() < 20000) text += (char)f.read();
  f.close(); text.trim();
  return text;
}

String markerPath(int num) {
  char path[64]; snprintf(path, sizeof(path), "%s/note_%03d.idea", NOTES_DIR, num);
  return String(path);
}

bool alreadyPushed(int num) { return SD_MMC.exists(markerPath(num).c_str()); }

void markPushed(int num, const String& response) {
  File f = SD_MMC.open(markerPath(num).c_str(), FILE_WRITE);
  if (!f) return;
  f.println("synced=1");
  int p = response.indexOf("note_id");
  if (p >= 0) { f.print("response="); f.println(response.substring(p, min(p + 180, (int)response.length()))); }
  f.close();
}

void safeUtf8Limit(String& s, size_t maxBytes) {
  if (s.length() <= maxBytes) return;
  size_t cut = maxBytes;
  while (cut > 0 && (((uint8_t)s[cut] & 0xC0) == 0x80)) cut--;
  s = s.substring(0, cut);
}

bool enrich(const String& raw, int num, String& title, String& summary, String& cleaned) {
  title = "记忆面包 #" + String(num);
  summary = "";
  cleaned = raw;
  if (!cfg::hasSiliconFlowKey()) return false;

  String input = raw; safeUtf8Limit(input, 6000);
  DynamicJsonDocument req(11000);
  req["model"] = "deepseek-ai/DeepSeek-V3.2";
  req["temperature"] = 0;
  req["enable_thinking"] = false;
  req.createNestedObject("response_format")["type"] = "json_object";
  JsonArray messages = req.createNestedArray("messages");
  JsonObject sys = messages.createNestedObject();
  sys["role"] = "system";
  sys["content"] =
    "整理语音闪念，只返回 JSON：{\"title\":\"2-8个汉字\",\"summary\":\"一句话摘要\","
    "\"cleaned\":\"忠于原意的精炼 Markdown 正文\"}。去掉口头禅和重复，不得增加未说过的信息。";
  JsonObject user = messages.createNestedObject();
  user["role"] = "user";
  user["content"] = input;
  String body; serializeJson(req, body);

  HttpReply reply;
  if (!postJson("api.siliconflow.cn", "/v1/chat/completions", cfg::siliconFlowKey(), "", body, reply))
    return false;
  if (reply.status != 200) return false;
  DynamicJsonDocument outer(reply.body.length() + 1024);
  if (deserializeJson(outer, reply.body)) return false;
  String content = outer["choices"][0]["message"]["content"] | "";
  DynamicJsonDocument inner(content.length() + 1024);
  if (!content.length() || deserializeJson(inner, content)) return false;
  String t = inner["title"] | "";
  String s = inner["summary"] | "";
  String c = inner["cleaned"] | "";
  t.trim(); s.trim(); c.trim();
  if (t.length()) title = t;
  if (s.length()) summary = s;
  if (c.length()) cleaned = c;
  safeUtf8Limit(title, 80);
  safeUtf8Limit(summary, 1400);
  return true;
}

bool createNote(const String& session, int num, const char* tag, String& error) {
  String transcript = readTranscript(num);
  if (!transcript.length()) { error = "转写为空"; return false; }
  String title, summary, cleaned;
  enrich(transcript, num, title, summary, cleaned);

  String content = cleaned;
  if (transcript != cleaned) content += "\n\n---\n\n<details><summary>原始转写</summary>\n\n" + transcript + "\n\n</details>";
  DynamicJsonDocument argsDoc(2048 + content.length() * 2);
  JsonObject args = argsDoc.to<JsonObject>();
  args["title"] = title;
  args["content"] = content;
  if (summary.length()) args["summary"] = summary;
  if (tag && strlen(tag)) args["tag"] = tag;
  JsonArray aiTags = args.createNestedArray("ai_tags");
  if (tag && strlen(tag)) aiTags.add(tag);

  String response;
  if (!mcpCall(session, "note_create", args, response, error)) return false;
  markPushed(num, response);
  return true;
}
}  // namespace

bool ideashellTestConnection(String& error) {
  error = "";
  if (WiFi.status() != WL_CONNECTED) { error = "Wi-Fi 未连接"; return false; }
  if (!cfg::ideashellToken().length()) { error = "未填写闪念贝壳 Token"; return false; }
  String session;
  if (!beginMcpSession(session, error)) return false;
  DynamicJsonDocument argsDoc(64);
  JsonObject args = argsDoc.to<JsonObject>();
  String response;
  // recent_notes is read-only and proves authorization + tool calls work.
  args["limit"] = 1;
  return mcpCall(session, "recent_notes", args, response, error);
}

SyncResult ideashellSyncAll() {
  SyncResult result = {0, 0, 0};
  if (!cfg::hasIdeashell() || WiFi.status() != WL_CONNECTED) return result;
  int pending = 0;
  for (int i = 0; i < (int)noteIndex.size(); i++)
    if (noteIndex[i].hasText && !alreadyPushed(noteIndex[i].num)) pending++;
  result.pending = pending;
  if (!pending) return result;

  String session, error;
  if (!beginMcpSession(session, error)) {
    Serial.printf("[ideaShell] %s\n", error.c_str());
    result.failed = pending;
    return result;
  }
  for (int i = 0; i < (int)noteIndex.size(); i++) {
    if (!noteIndex[i].hasText || alreadyPushed(noteIndex[i].num)) continue;
    showCloudSync(result.success, result.failed, pending);
    if (createNote(session, noteIndex[i].num, noteIndex[i].tag, error)) result.success++;
    else {
      result.failed++;
      Serial.printf("[ideaShell] note %d: %s\n", noteIndex[i].num, error.c_str());
    }
  }
  Serial.printf("[ideaShell] synced %d/%d, failed=%d\n",
                result.success, pending, result.failed);
  return result;
}
