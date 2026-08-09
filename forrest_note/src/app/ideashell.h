#pragma once
#include <Arduino.h>

// Direct, server-free text sync to ideaShell MCP. Audio stays on microSD;
// transcribeAll() must run first so each pending note has a local TXT file.
void ideashellSyncAll();

// Read-only handshake + tools/list check, used by the configuration portal.
bool ideashellTestConnection(String& error);
