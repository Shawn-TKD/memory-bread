#pragma once
#include <Arduino.h>

// Runtime configuration / secrets, persisted in NVS (the `nvs` partition) rather
// than baked into the app image. Keeps Wi-Fi and API credentials out of the
// firmware binary that gets flashed/shared. Provisioned at runtime over the
// setup portal (SoftAP) or seeded once from secrets.h for backward compat.
namespace cfg {
  static constexpr uint8_t MAX_WIFI_NETWORKS = 3;
  void   begin();                                       // load NVS, one-time seed from secrets.h
  String wifiSsid(uint8_t slot);
  String wifiPass(uint8_t slot);
  uint8_t wifiCount();
  int8_t lastWifiSlot();
  void   setLastWifiSlot(uint8_t slot);
  String siliconFlowKey();
  bool   hasWifi();                                     // Wi-Fi credentials present
  bool   hasSiliconFlowKey();                           // ASR + summary key present
  bool   setWifi(uint8_t slot, const String& ssid, const String& pass);
  void   clearWifi(uint8_t slot);
  bool   setSiliconFlowKey(const String& key);
  uint8_t soundTheme();                                  // 0=off, 1=water, 2=classic, 3=wood
  void    setSoundTheme(uint8_t theme);

  String ideashellToken();
  bool   ideashellEnabled();
  bool   hasIdeashell();
  bool   setIdeashellToken(const String& token);
  void   setIdeashellEnabled(bool on);

  // GitHub / Obsidian vault sync
  String githubToken();
  String githubRepo();                                  // "owner/repo"
  String githubBranch();                                // default "main"
  String githubDir();                                   // vault subfolder, default "VoiceNotes"
  bool   githubEnabled();                               // master on/off
  bool   githubAiEnrich();                              // AI title/topics, default on
  bool   hasGithub();                                   // enabled + token + valid "owner/repo"
  bool   setGithubToken(const String& token);
  bool   setGithubRepo(const String& ownerRepo);        // validates one '/'
  bool   setGithubBranch(const String& branch);
  bool   setGithubDir(const String& dir);
  void   setGithubEnabled(bool on);
  void   setGithubAiEnrich(bool on);

  void   factoryReset();                                // wipe all stored config
}
