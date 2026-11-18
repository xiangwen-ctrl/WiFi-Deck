/*
 * command.h — Unified command parser & dispatcher for Serial CLI, BLE NUS, and WebUI HTTP API
 * Part of WiFi-Deck multi-file refactor.
 */
#pragma once

#include <Arduino.h>
#include "sniff.h"

// Parse 'sniff_time <duration_ms> [all|beacon|probe|deauth|eapol|pwnagotchi]'
bool parseSniffTimeCmd(const String &command, unsigned long &durOut, SniffMode &modeOut, String &errMsg);

// Unified command execution entry point.
// - errOut: optional human-readable error message output (used by HTTP API)
// - fromHttp: true when invoked from WebUI HTTP API (enforces Decision B: rejects 'ap off')
bool executeCommand(String command, String *errOut = nullptr, bool fromHttp = false);

// Convenience wrapper for Serial / BLE command queues
void handleCommand(String command);
