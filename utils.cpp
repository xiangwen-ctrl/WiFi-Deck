#include "utils.h"
#include <ctype.h>
#include <strings.h>

// ── String helpers ──────────────────────────────────────────

bool startsWithIgnoreCase(const String &s, const char *prefix) {
  if (!prefix) return false;
  size_t plen = strlen(prefix);
  if (s.length() < plen) return false;
  return strncasecmp(s.c_str(), prefix, plen) == 0;
}

// ── MAC helpers ─────────────────────────────────────────────

// Generate a random, locally-administered MAC for flood frames
void randomMAC(uint8_t *mac) {
  for (int i = 0; i < 6; i++) {
    mac[i] = random(0, 256);
  }
  mac[0] = (mac[0] & 0xFE) | 0x02;  // locally administered bit
}

// ── UTF-8 helpers ───────────────────────────────────────────

size_t validUtf8CharLen(const uint8_t *p, size_t rem) {
  if (!p || rem == 0) return 0;
  uint8_t b0 = p[0];
  if (b0 < 0x80) return 1;
  if (b0 >= 0xC2 && b0 <= 0xDF) {
    if (rem >= 2 && (p[1] & 0xC0) == 0x80) return 2;
    return 0;
  }
  if (b0 >= 0xE0 && b0 <= 0xEF) {
    if (rem >= 3 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      if (b0 == 0xE0 && p[1] < 0xA0) return 0;  // overlong
      if (b0 == 0xED && p[1] > 0x9F) return 0;  // UTF-16 surrogate range
      return 3;
    }
    return 0;
  }
  if (b0 >= 0xF0 && b0 <= 0xF4) {
    if (rem >= 4 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
      if (b0 == 0xF0 && p[1] < 0x90) return 0;  // overlong
      if (b0 == 0xF4 && p[1] > 0x8F) return 0;  // > U+10FFFF
      return 4;
    }
    return 0;
  }
  return 0;
}

void sanitizeUtf8BytesToCstr(const uint8_t *src, size_t srcLen, char *out, size_t outSize, bool stripCtrl) {
  if (!out || outSize == 0) return;
  out[0] = '\0';
  if (!src || srcLen == 0) return;

  size_t pos = 0;
  size_t i = 0;
  while (i < srcLen && pos + 1 < outSize) {
    uint8_t c = src[i];
    if (c == 0x00) {
      i++;
      continue;
    }
    if (c < 0x80) {
      if (!stripCtrl || (c >= 0x20 && c != 0x7F)) {
        out[pos++] = (char)c;
      }
      i++;
      continue;
    }
    size_t seqLen = validUtf8CharLen(&src[i], srcLen - i);
    if (seqLen > 0) {
      if (pos + seqLen >= outSize) break;
      for (size_t k = 0; k < seqLen; k++) {
        out[pos++] = (char)src[i + k];
      }
      i += seqLen;
    } else {
      out[pos++] = '?';
      i++;
    }
  }
  out[pos] = '\0';
}

void utf8SafeCopy(char *dst, size_t dstSize, const char *src) {
  if (!dst || dstSize == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t maxLen = dstSize - 1;
  size_t len = 0;
  while (len < maxLen && src[len] != '\0') {
    len++;
  }
  if (len > 0 && src[len] != '\0') {
    if (((uint8_t)src[len] & 0xC0) == 0x80) {
      size_t back = len;
      while (back > 0 && (((uint8_t)src[back - 1] & 0xC0) == 0x80)) {
        back--;
      }
      if (back > 0 && ((uint8_t)src[back - 1] & 0x80) != 0) {
        len = back - 1;
      } else {
        len = back;
      }
    }
  }
  if (len > 0) {
    memcpy(dst, src, len);
  }
  dst[len] = '\0';
}

// ── JSON escape ─────────────────────────────────────────────

size_t jsonEscapeTruncatedToBuf(const char *src, char *out, size_t outSize, size_t maxEscapedBytes, const char *ellipsis) {
  if (!out || outSize == 0) return 0;
  out[0] = '\0';
  if (!src || src[0] == '\0' || maxEscapedBytes == 0) return 0;

  size_t cap = outSize - 1;
  if (maxEscapedBytes != (size_t)-1 && maxEscapedBytes < cap) {
    cap = maxEscapedBytes;
  }
  size_t ellLen = (ellipsis != nullptr) ? strlen(ellipsis) : 0;
  if (ellLen > cap) ellLen = 0;

  const uint8_t *data = (const uint8_t *)src;
  size_t srcLen = strlen(src);
  size_t i = 0;
  size_t pos = 0;
  size_t lastEllipsisCut = 0;
  bool truncated = false;

  while (i < srcLen) {
    uint8_t c = data[i];
    if (c == 0x00) {
      i++;
      continue;
    }

    char seqBuf[8];
    size_t seqLen = 0;
    size_t consume = 1;

    if (c == '"') {
      seqBuf[0] = '\\'; seqBuf[1] = '"'; seqLen = 2;
    } else if (c == '\\') {
      seqBuf[0] = '\\'; seqBuf[1] = '\\'; seqLen = 2;
    } else if (c == '\b') {
      seqBuf[0] = '\\'; seqBuf[1] = 'b'; seqLen = 2;
    } else if (c == '\f') {
      seqBuf[0] = '\\'; seqBuf[1] = 'f'; seqLen = 2;
    } else if (c == '\n') {
      seqBuf[0] = '\\'; seqBuf[1] = 'n'; seqLen = 2;
    } else if (c == '\r') {
      seqBuf[0] = '\\'; seqBuf[1] = 'r'; seqLen = 2;
    } else if (c == '\t') {
      seqBuf[0] = '\\'; seqBuf[1] = 't'; seqLen = 2;
    } else if (c < 0x20 || c == 0x7F) {
      snprintf(seqBuf, sizeof(seqBuf), "\\u%04x", (unsigned)c);
      seqLen = 6;
    } else if (c < 0x80) {
      seqBuf[0] = (char)c;
      seqLen = 1;
    } else {
      size_t utf8Len = validUtf8CharLen(&data[i], srcLen - i);
      if (utf8Len > 0 && utf8Len <= 4) {
        memcpy(seqBuf, &data[i], utf8Len);
        seqLen = utf8Len;
        consume = utf8Len;
      } else {
        seqBuf[0] = '?';
        seqLen = 1;
        consume = 1;
      }
    }

    if (pos + seqLen > cap) {
      truncated = true;
      break;
    }
    memcpy(out + pos, seqBuf, seqLen);
    pos += seqLen;
    if (pos + ellLen <= cap) {
      lastEllipsisCut = pos;
    }
    i += consume;
  }

  if (truncated && ellLen > 0) {
    pos = lastEllipsisCut;
    memcpy(out + pos, ellipsis, ellLen);
    pos += ellLen;
  }
  out[pos] = '\0';
  return pos;
}

size_t jsonEscapeToBuf(const char *src, char *out, size_t outSize) {
  return jsonEscapeTruncatedToBuf(src, out, outSize, (size_t)-1, "");
}

String jsonEscapeTruncated(const String &s, size_t maxEscapedBytes, const char *ellipsis) {
  size_t len = s.length();
  if (len == 0 || maxEscapedBytes == 0) return "";
  size_t bufCap = len * 6 + 16;
  if (maxEscapedBytes != (size_t)-1 && bufCap > maxEscapedBytes + 8) {
    bufCap = maxEscapedBytes + 8;
  }
  if (bufCap <= 384) {
    char stackBuf[384];
    jsonEscapeTruncatedToBuf(s.c_str(), stackBuf, sizeof(stackBuf), maxEscapedBytes, ellipsis);
    return String(stackBuf);
  }
  char *dynBuf = (char *)malloc(bufCap);
  if (!dynBuf) {
    char fallbackBuf[384];
    jsonEscapeTruncatedToBuf(s.c_str(), fallbackBuf, sizeof(fallbackBuf),
                             (maxEscapedBytes < 380) ? maxEscapedBytes : 380, ellipsis);
    return String(fallbackBuf);
  }
  jsonEscapeTruncatedToBuf(s.c_str(), dynBuf, bufCap, maxEscapedBytes, ellipsis);
  String out(dynBuf);
  free(dynBuf);
  return out;
}

String jsonEscape(const String &s) {
  return jsonEscapeTruncated(s, (size_t)-1, "");
}

// ── URL / form helpers ──────────────────────────────────────

String urlDecode(const String &s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < s.length() &&
               isxdigit((unsigned char)s[i + 1]) &&
               isxdigit((unsigned char)s[i + 2])) {
      char hex[3] = {s[i + 1], s[i + 2], '\0'};
      unsigned long val = strtoul(hex, nullptr, 16);
      // 过滤 %00 空字节注入
      if (val != 0) {
        out += (char)val;
      }
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

String getQueryParam(const String &path, const String &key) {
  int q = path.indexOf('?');
  if (q < 0) return "";
  String query = path.substring(q + 1);
  String prefix = key + "=";
  int start = -1;
  for (int from = 0; from < (int)query.length(); ) {
    int pos = query.indexOf(prefix, from);
    if (pos < 0) break;
    // Accept the key only at the start or right after a '&', so that e.g.
    // "led" does not match inside "use_led".
    if (pos == 0 || query.charAt(pos - 1) == '&') {
      start = pos;
      break;
    }
    from = pos + 1;
  }
  if (start < 0) return "";
  start += prefix.length();
  int end = query.indexOf('&', start);
  if (end < 0) end = query.length();
  return urlDecode(query.substring(start, end));
}

String getPostValue(const String &body, const String &key) {
  String prefix = key + "=";
  int start = -1;
  for (int from = 0; from < (int)body.length(); ) {
    int pos = body.indexOf(prefix, from);
    if (pos < 0) break;
    if (pos == 0 || body.charAt(pos - 1) == '&') {
      start = pos;
      break;
    }
    from = pos + 1;
  }
  if (start < 0) return "";
  start += prefix.length();
  int end = body.indexOf('&', start);
  if (end < 0) end = body.length();
  return urlDecode(body.substring(start, end));
}

bool hasFormKey(const String &body, const String &key) {
  String prefix = key + "=";
  for (int from = 0; from < (int)body.length(); ) {
    int pos = body.indexOf(prefix, from);
    if (pos < 0) return false;
    if (pos == 0 || body.charAt(pos - 1) == '&') return true;
    from = pos + 1;
  }
  return false;
}
