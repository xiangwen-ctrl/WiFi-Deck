/*
 * utils.h — Watchdog, MAC formatting, UTF-8 safe truncation, JSON escaping & URL/form helpers
 * Part of WiFi-Deck multi-file refactor.
 */
#pragma once

#include <Arduino.h>
#include <cstdint>
#include <cstddef>

#include "wiring_watchdog.h"

// ── Watchdog ────────────────────────────────────────────────
static inline void feedWatchdog() {
  wdt_reset();
}

// ── MAC helpers ─────────────────────────────────────────────
void randomMAC(uint8_t *mac);

static inline void formatMac(const uint8_t *mac, char *out, size_t outSize) {
  snprintf(out, outSize, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ── String helpers ──────────────────────────────────────────
bool startsWithIgnoreCase(const String &s, const char *prefix);

// ── UTF-8 helpers ───────────────────────────────────────────
size_t validUtf8CharLen(const uint8_t *p, size_t rem);
void   sanitizeUtf8BytesToCstr(const uint8_t *src, size_t srcLen,
                               char *out, size_t outSize, bool stripCtrl);
void   utf8SafeCopy(char *dst, size_t dstSize, const char *src);

// ── JSON escape ─────────────────────────────────────────────
size_t jsonEscapeTruncatedToBuf(const char *src, char *out, size_t outSize,
                                size_t maxEscapedBytes,
                                const char *ellipsis = "...");
size_t jsonEscapeToBuf(const char *src, char *out, size_t outSize);
String jsonEscapeTruncated(const String &s, size_t maxEscapedBytes,
                           const char *ellipsis = "...");
String jsonEscape(const String &s);

// ── URL / form helpers ──────────────────────────────────────
String urlDecode(const String &s);
String getQueryParam(const String &path, const String &key);
String getPostValue(const String &body, const String &key);
bool   hasFormKey(const String &body, const String &key);
