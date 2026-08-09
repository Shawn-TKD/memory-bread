#include "config_store.h"
#include <Preferences.h>
#include <string.h>
#include "../../secrets.h"

namespace {
  Preferences prefs;
  const char* NS = "forrest";

  // secrets.h ships with "...." placeholders; treat all-dots or empty as unset.
  bool isPlaceholder(const char* s) {
    if (!s || s[0] == '\0') return true;
    return strspn(s, ".") == strlen(s);
  }
}

namespace cfg {

void begin() {
  prefs.begin(NS, false);
  // One-time migration: seed NVS from compiled secrets.h only for real (non
  // placeholder) values that aren't already stored. After this, secrets live in
  // NVS and can be rotated at runtime without reflashing.
  // Migrate the original single-network layout into slot 0 without losing the
  // user's current Wi-Fi. Fresh devices may still be seeded from secrets.h.
  if (!prefs.isKey("ssid0")) {
    String oldSsid = prefs.getString("ssid", "");
    String oldPass = prefs.getString("pass", "");
    if (oldSsid.length()) {
      prefs.putString("ssid0", oldSsid);
      prefs.putString("pass0", oldPass);
    } else if (!isPlaceholder(WIFI_SSID)) {
      prefs.putString("ssid0", WIFI_SSID);
      prefs.putString("pass0", WIFI_PASS);
    }
  }
  if (!prefs.isKey("sfkey") && !isPlaceholder(SILICONFLOW_KEY)) {
    prefs.putString("sfkey", SILICONFLOW_KEY);
  }

  // One-time correction (cfgv=1): the setup form persists the AI-enrich checkbox
  // state, so an older save with the box unchecked could leave "ghai" stuck false
  // — which silently disables note summaries/cleanup on sync. Force it back on once.
  if (!prefs.isKey("cfgv")) {
    prefs.putBool("ghai", true);
    prefs.putUInt("cfgv", 1);
  }
}

static String wifiKey(const char* prefix, uint8_t slot) {
  return String(prefix) + String(slot);
}

String wifiSsid(uint8_t slot) {
  if (slot >= MAX_WIFI_NETWORKS) return "";
  return prefs.getString(wifiKey("ssid", slot).c_str(), "");
}
String wifiPass(uint8_t slot) {
  if (slot >= MAX_WIFI_NETWORKS) return "";
  return prefs.getString(wifiKey("pass", slot).c_str(), "");
}
uint8_t wifiCount() {
  uint8_t count = 0;
  for (uint8_t i = 0; i < MAX_WIFI_NETWORKS; i++) if (wifiSsid(i).length()) count++;
  return count;
}
int8_t lastWifiSlot() {
  uint8_t slot = prefs.getUChar("wlast", 0);
  return slot < MAX_WIFI_NETWORKS && wifiSsid(slot).length() ? (int8_t)slot : -1;
}
void setLastWifiSlot(uint8_t slot) {
  if (slot < MAX_WIFI_NETWORKS) prefs.putUChar("wlast", slot);
}
String siliconFlowKey() { return prefs.getString("sfkey", ""); }

bool hasWifi()      { return wifiCount() > 0; }
bool hasSiliconFlowKey() { return siliconFlowKey().length() > 0; }

bool setWifi(uint8_t slot, const String& ssid, const String& pass) {
  if (slot >= MAX_WIFI_NETWORKS || ssid.length() == 0) return false;
  prefs.putString(wifiKey("ssid", slot).c_str(), ssid);
  prefs.putString(wifiKey("pass", slot).c_str(), pass);
  return true;
}

void clearWifi(uint8_t slot) {
  if (slot >= MAX_WIFI_NETWORKS) return;
  prefs.remove(wifiKey("ssid", slot).c_str());
  prefs.remove(wifiKey("pass", slot).c_str());
}

bool setSiliconFlowKey(const String& key) {
  prefs.putString("sfkey", key);
  return true;
}

// Keep the original NVS key so existing devices migrate without losing their
// selection: the former values now map to off/water/classic/wood.
uint8_t soundTheme() { return min((uint8_t)3, prefs.getUChar("sndlvl", 2)); }
void setSoundTheme(uint8_t theme) { prefs.putUChar("sndlvl", min((uint8_t)3, theme)); }

String ideashellToken() { return prefs.getString("ideatok", ""); }
bool ideashellEnabled() { return prefs.getBool("ideaon", false); }
bool hasIdeashell() { return ideashellEnabled() && ideashellToken().length() > 0; }
bool setIdeashellToken(const String& token) { prefs.putString("ideatok", token); return true; }
void setIdeashellEnabled(bool on) { prefs.putBool("ideaon", on); }

// ── GitHub / Obsidian vault ─────────────────────────────────────────────────
// NVS keys must be <=15 chars.
String githubToken()  { return prefs.getString("ghtok", ""); }
String githubRepo()   { return prefs.getString("ghrepo", ""); }
String githubBranch() { String b = prefs.getString("ghbranch", ""); return b.length() ? b : "main"; }
String githubDir()    { String d = prefs.getString("ghdir", "");    return d.length() ? d : "VoiceNotes"; }
bool   githubEnabled()   { return prefs.getBool("ghon", false); }
bool   githubAiEnrich()  { return prefs.getBool("ghai", true); }

bool hasGithub() {
  return githubEnabled() && githubToken().length() > 0 && githubRepo().indexOf('/') > 0;
}

bool setGithubToken(const String& token) { prefs.putString("ghtok", token); return true; }

bool setGithubRepo(const String& ownerRepo) {
  String r = ownerRepo; r.trim();
  while (r.endsWith("/")) r.remove(r.length() - 1);
  if (r.indexOf('/') <= 0 || r.indexOf('/') != r.lastIndexOf('/')) return false;  // exactly one '/'
  prefs.putString("ghrepo", r);
  return true;
}

bool setGithubBranch(const String& branch) {
  String b = branch; b.trim();
  prefs.putString("ghbranch", b);
  return true;
}

bool setGithubDir(const String& dir) {
  String d = dir; d.trim();
  while (d.endsWith("/"))   d.remove(d.length() - 1);
  while (d.startsWith("/")) d.remove(0, 1);
  prefs.putString("ghdir", d);
  return true;
}

void setGithubEnabled(bool on)  { prefs.putBool("ghon", on); }
void setGithubAiEnrich(bool on) { prefs.putBool("ghai", on); }

void factoryReset() { prefs.clear(); }

}  // namespace cfg
