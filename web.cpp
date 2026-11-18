/*
 * web.cpp — Direct lwIP BSD-socket HTTP server, zero-heap JSON streaming & gzipped WebUI
 * Part of WiFi-Deck multi-file refactor (P1 optimized).
 */
#include "web.h"
#include "webui_gz.h"
#include "config.h"
#include "utils.h"
#include "sniff.h"
#include "wifi_scan.h"
#include "ble.h"
#include "wifi_attack.h"
#include "command.h"

#include <ctype.h>
#include <errno.h>
#include <strings.h>
#include "wifi_conf.h"
#include "osdep_service.h"

#include <sys/types.h>
#ifndef _IN_ADDR_T_DECLARED
typedef uint32_t in_addr_t;
#define _IN_ADDR_T_DECLARED
#endif

extern "C" {
#include <lwip/sockets.h>
}

// Strip lwIP BSD/POSIX socket compatibility macros so C++ methods (Serial.read, write, connect, etc.) are untouched
#undef accept
#undef bind
#undef shutdown
#undef getpeername
#undef getsockname
#undef setsockopt
#undef getsockopt
#undef closesocket
#undef connect
#undef listen
#undef recv
#undef recvfrom
#undef send
#undef sendmsg
#undef sendto
#undef socket
#undef select
#undef ioctlsocket
#undef read
#undef write
#undef writev
#undef close
#undef fcntl
#undef ioctl

#ifndef SHUT_WR
#define SHUT_WR 1
#endif

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

//==========================================================
// Web Server (Direct lwIP BSD Sockets with SO_REUSEADDR)
//==========================================================
// Bypasses AmebaD WiFiServer/ard_socket start_server() which lacks SO_REUSEADDR,
// leaks _sock on bind() failure, and causes UB in get_sock_errno(-1) / Spin on EAGAIN.
static int webListenSock = -1;

bool webServerRunning() {
  return webListenSock >= 0;
}

void stopWebServer() {
  if (webListenSock >= 0) {
    lwip_close(webListenSock);
    webListenSock = -1;
  }
}

bool startWebServer(uint8_t maxRetries) {
  if (maxRetries == 0) maxRetries = 1;
  stopWebServer();

  for (uint8_t attempt = 0; attempt < maxRetries; attempt++) {
    feedWatchdog();
    int fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
      Serial.println("[ERROR] WebServer lwip_socket() failed");
    } else {
      int reuse = 1;
      int rcvTimeoutMs = 3000;
      lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
      lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvTimeoutMs, sizeof(rcvTimeoutMs));

      int flags = lwip_fcntl(fd, F_GETFL, 0);
      if (flags >= 0) {
        lwip_fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      }

      struct sockaddr_in localAddr;
      memset(&localAddr, 0, sizeof(localAddr));
      localAddr.sin_family = AF_INET;
      localAddr.sin_port = htons(80);
      localAddr.sin_addr.s_addr = INADDR_ANY;

      if (lwip_bind(fd, (struct sockaddr *)&localAddr, sizeof(localAddr)) < 0) {
        int err = lwip_getsocklasterr(fd);
        lwip_close(fd);
        Serial.print("[ERROR] WebServer bind(80) failed errno=");
        Serial.print(err);
        Serial.print(" attempt=");
        Serial.println((unsigned)(attempt + 1));
      } else if (lwip_listen(fd, 4) < 0) {
        int err = lwip_getsocklasterr(fd);
        lwip_close(fd);
        Serial.print("[ERROR] WebServer listen(80) failed errno=");
        Serial.print(err);
        Serial.print(" attempt=");
        Serial.println((unsigned)(attempt + 1));
      } else {
        webListenSock = fd;
        return true;
      }
    }
    if (attempt + 1 < maxRetries) {
      unsigned long backoffMs = 50UL * (unsigned long)(attempt + 1);
      delay(backoffMs);
      feedWatchdog();
    }
  }
  return false;
}

static int acceptWebClient() {
  if (webListenSock < 0) return -1;
  struct sockaddr_in peerAddr;
  socklen_t peerLen = sizeof(peerAddr);
  int cli = lwip_accept(webListenSock, (struct sockaddr *)&peerAddr, &peerLen);
  if (cli < 0) {
    return -1;
  }
  int flags = lwip_fcntl(cli, F_GETFL, 0);
  if (flags >= 0) {
    lwip_fcntl(cli, F_SETFL, flags & ~O_NONBLOCK);
  }
  int reuse = 1;
  int rcvTimeoutMs = 25;
  int sndTimeoutMs = 1500;
  int nodelay = 1;
  // Inherit SO_REUSEADDR on client sockets too so lingering FIN_WAIT PCBs never block bind(80) after AP restart
  lwip_setsockopt(cli, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  lwip_setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &rcvTimeoutMs, sizeof(rcvTimeoutMs));
  lwip_setsockopt(cli, SOL_SOCKET, SO_SNDTIMEO, &sndTimeoutMs, sizeof(sndTimeoutMs));
  lwip_setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  return cli;
}

static int webClientRecv(int sock, uint8_t *buf, size_t len, bool &peerClosedOrErr) {
  peerClosedOrErr = false;
  if (sock < 0 || !buf || len == 0) {
    peerClosedOrErr = true;
    return -1;
  }
  int r = lwip_recv(sock, buf, len, 0);
  if (r > 0) return r;
  if (r == 0) {
    peerClosedOrErr = true;
    return 0;
  }
  int err = lwip_getsocklasterr(sock);
  if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
    return 0;
  }
  peerClosedOrErr = true;
  return -1;
}

static void closeWebClient(int &sock, bool gracefulDrain) {
  if (sock < 0) return;
  if (gracefulDrain) {
    lwip_shutdown(sock, SHUT_WR);
    int flags = lwip_fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
      lwip_fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }
    uint8_t sink[64];
    for (int i = 0; i < 4; i++) {
      int r = lwip_recv(sock, sink, sizeof(sink), 0);
      if (r <= 0) break;
    }
    delay(5);
  }
  lwip_close(sock);
  sock = -1;
}

//==========================================================
// HTTP Helpers
//==========================================================
static const char *httpStatusReason(int code) {
  switch (code) {
    case 200: return "OK";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "OK";
  }
}

int formatHttpHeaderBuf(int code, const char *contentType, int contentLength, char *out, size_t outSize) {
  const char *reason = httpStatusReason(code);
  // 决策 C：同源访问，不返回 Access-Control-Allow-Origin: *
  if (contentLength >= 0) {
    return snprintf(out, outSize,
                    "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                    code, reason, contentType ? contentType : "text/plain", contentLength);
  }
  return snprintf(out, outSize,
                  "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                  code, reason, contentType ? contentType : "text/plain");
}

String httpHeader(int code, const String &contentType, int contentLength) {
  char buf[256];
  formatHttpHeaderBuf(code, contentType.c_str(), contentLength, buf, sizeof(buf));
  return String(buf);
}

bool sendLargeText(int clientSock, const char *data, size_t totalLen) {
  if (clientSock < 0 || (!data && totalLen > 0)) return false;
  const size_t CHUNK_SIZE = 1024;
  size_t sent = 0;
  unsigned long lastProgressMs = millis();
  while (sent < totalLen) {
    feedWatchdog();
    size_t toSend = (totalLen - sent > CHUNK_SIZE) ? CHUNK_SIZE : (totalLen - sent);
    int r = lwip_send(clientSock, data + sent, toSend, 0);
    if (r > 0) {
      sent += (size_t)r;
      lastProgressMs = millis();
      if (sent < totalLen) {
        delay(2);  // give lwIP time to drain its send window
      }
    } else {
      int err = (r < 0) ? lwip_getsocklasterr(clientSock) : 0;
      if (r < 0 && (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) &&
          (millis() - lastProgressMs < 1500)) {
        delay(5);
        continue;
      }
      return false;
    }
  }
  return (sent == totalLen);
}

bool sendHttpResponse(int clientSock, int code, const char *contentType, const char *body, size_t bodyLen) {
  char hdrBuf[256];
  int hn = formatHttpHeaderBuf(code, contentType, (int)bodyLen, hdrBuf, sizeof(hdrBuf));
  if (hn <= 0 || (size_t)hn >= sizeof(hdrBuf)) return false;
  if (!sendLargeText(clientSock, hdrBuf, (size_t)hn)) {
    return false;
  }
  if (bodyLen > 0 && body != nullptr) {
    return sendLargeText(clientSock, body, bodyLen);
  }
  return true;
}

bool sendHttpResponseGzip(int clientSock, int code, const char *contentType, const uint8_t *gzBody, size_t gzLen) {
  char hdrBuf[280];
  int hn = snprintf(hdrBuf, sizeof(hdrBuf),
                    "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Encoding: gzip\r\nCache-Control: no-store\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",
                    code, httpStatusReason(code),
                    contentType ? contentType : "text/html; charset=utf-8",
                    (unsigned long)gzLen);
  if (hn <= 0 || (size_t)hn >= sizeof(hdrBuf)) return false;
  if (!sendLargeText(clientSock, hdrBuf, (size_t)hn)) {
    return false;
  }
  if (gzLen > 0 && gzBody != nullptr) {
    return sendLargeText(clientSock, (const char *)gzBody, gzLen);
  }
  return true;
}

// Zero-allocation C-buffer HTTP request line parser (operates directly on httpReqBuf[0..hdrLen))
void parseHttpRequestLineCstr(const char *hdrBuf, size_t hdrLen,
                              char *methodOut, size_t methodCap,
                              char *fullPathOut, size_t pathCap) {
  if (methodOut && methodCap > 0) methodOut[0] = '\0';
  if (fullPathOut && pathCap > 0) {
    strncpy(fullPathOut, "/", pathCap - 1);
    fullPathOut[pathCap - 1] = '\0';
  }
  if (!hdrBuf || hdrLen == 0) return;

  size_t lineEnd = 0;
  while (lineEnd < hdrLen && hdrBuf[lineEnd] != '\n') lineEnd++;
  if (lineEnd > 0 && hdrBuf[lineEnd - 1] == '\r') lineEnd--;

  size_t sp1 = 0;
  while (sp1 < lineEnd && hdrBuf[sp1] != ' ') sp1++;
  if (sp1 == 0 || sp1 >= lineEnd) return;

  if (methodOut && methodCap > 0) {
    if (sp1 >= methodCap) {
      methodOut[0] = '\0';
      return;
    }
    for (size_t i = 0; i < sp1; i++) {
      methodOut[i] = (char)toupper((unsigned char)hdrBuf[i]);
    }
    methodOut[sp1] = '\0';
  }

  size_t pathStart = sp1 + 1;
  while (pathStart < lineEnd && hdrBuf[pathStart] == ' ') pathStart++;
  size_t pathEnd = pathStart;
  while (pathEnd < lineEnd && hdrBuf[pathEnd] != ' ') pathEnd++;

  if (fullPathOut && pathCap > 0 && pathEnd > pathStart) {
    size_t copyLen = pathEnd - pathStart;
    if (copyLen >= pathCap) copyLen = pathCap - 1;
    memcpy(fullPathOut, hdrBuf + pathStart, copyLen);
    fullPathOut[copyLen] = '\0';
  }
}

// 1 = valid Content-Length, 0 = not present, -1 = malformed/negative (zero heap allocation)
int parseContentLengthHeaderCstr(const char *hdrBuf, size_t hdrLen, long &outLen) {
  outLen = 0;
  if (!hdrBuf || hdrLen == 0) return 0;

  size_t pos = 0;
  while (pos < hdrLen) {
    size_t nl = pos;
    while (nl < hdrLen && hdrBuf[nl] != '\n') nl++;
    size_t lineEnd = nl;
    if (lineEnd > pos && hdrBuf[lineEnd - 1] == '\r') lineEnd--;

    size_t lineLen = lineEnd - pos;
    if (lineLen >= 15 && strncasecmp(hdrBuf + pos, "Content-Length:", 15) == 0) {
      size_t vStart = pos + 15;
      while (vStart < lineEnd && isspace((unsigned char)hdrBuf[vStart])) vStart++;
      size_t vEnd = lineEnd;
      while (vEnd > vStart && isspace((unsigned char)hdrBuf[vEnd - 1])) vEnd--;

      size_t vLen = vEnd - vStart;
      if (vLen == 0 || vLen >= 24) return -1;
      char valBuf[24];
      memcpy(valBuf, hdrBuf + vStart, vLen);
      valBuf[vLen] = '\0';

      char *endPtr = nullptr;
      errno = 0;
      long parsed = strtol(valBuf, &endPtr, 10);
      if (errno == ERANGE || endPtr == valBuf || *endPtr != '\0' || parsed < 0) {
        return -1;
      }
      outLen = parsed;
      return 1;
    }
    pos = nl + 1;
  }
  return 0;
}

static void parseIfNoneMatchHeaderCstr(const char *hdrBuf, size_t hdrLen, char *etagOut, size_t etagCap) {
  if (!etagOut || etagCap == 0) return;
  etagOut[0] = '\0';
  if (!hdrBuf || hdrLen == 0) return;

  size_t pos = 0;
  while (pos < hdrLen) {
    size_t nl = pos;
    while (nl < hdrLen && hdrBuf[nl] != '\n') nl++;
    size_t lineEnd = nl;
    if (lineEnd > pos && hdrBuf[lineEnd - 1] == '\r') lineEnd--;

    size_t lineLen = lineEnd - pos;
    if (lineLen >= 14 && strncasecmp(hdrBuf + pos, "If-None-Match:", 14) == 0) {
      size_t vStart = pos + 14;
      while (vStart < lineEnd && isspace((unsigned char)hdrBuf[vStart])) vStart++;
      size_t vEnd = lineEnd;
      while (vEnd > vStart && isspace((unsigned char)hdrBuf[vEnd - 1])) vEnd--;
      size_t vLen = vEnd - vStart;
      if (vLen > 0 && vLen < etagCap) {
        memcpy(etagOut, hdrBuf + vStart, vLen);
        etagOut[vLen] = '\0';
      }
      return;
    }
    pos = nl + 1;
  }
}

String getHttpRequestLine(const String &request) {
  int lineEnd = request.indexOf('\n');
  String firstLine = (lineEnd >= 0) ? request.substring(0, lineEnd) : request;
  if (firstLine.endsWith("\r")) {
    firstLine.remove(firstLine.length() - 1);
  }
  return firstLine;
}

String parsePath(const String &request) {
  char methodBuf[16];
  char pathBuf[256];
  parseHttpRequestLineCstr(request.c_str(), request.length(), methodBuf, sizeof(methodBuf), pathBuf, sizeof(pathBuf));
  return String(pathBuf);
}

String parseMethod(const String &request) {
  char methodBuf[16];
  char pathBuf[8];
  parseHttpRequestLineCstr(request.c_str(), request.length(), methodBuf, sizeof(methodBuf), pathBuf, sizeof(pathBuf));
  return String(methodBuf);
}

String parseBody(const String &request) {
  int idx = request.indexOf("\r\n\r\n");
  if (idx >= 0) return request.substring(idx + 4);
  idx = request.indexOf("\n\n");
  if (idx >= 0) return request.substring(idx + 2);
  return "";
}

int parseContentLengthHeader(const String &headers, long &outLen) {
  return parseContentLengthHeaderCstr(headers.c_str(), headers.length(), outLen);
}

//==========================================================
// Zero-Heap / Streaming JSON Builders
//==========================================================
static size_t formatScanItemBuf(const WiFiScanResult &ap, size_t idx, char *out, size_t outSize) {
  char escSsid[200];
  jsonEscapeToBuf(ap.ssid, escSsid, sizeof(escSsid));
  int n = snprintf(out, outSize,
                   "%s{\"index\":%lu,\"ssid\":\"%s\",\"bssid\":\"%s\",\"channel\":%u,\"rssi\":%d,\"band\":\"%s\",\"selected\":%s}",
                   (idx > 0) ? "," : "",
                   (unsigned long)idx,
                   escSsid,
                   ap.bssid_str,
                   (unsigned)ap.channel,
                   (int)ap.rssi,
                   (ap.channel >= 36) ? "5GHz" : "2.4GHz",
                   ap.selected ? "true" : "false");
  if (n <= 0) return 0;
  return ((size_t)n < outSize) ? (size_t)n : (outSize - 1);
}

static bool appendStreamChunk(int clientSock, char *chunkBuf, size_t chunkCap, size_t &chunkUsed,
                              const char *piece, size_t pieceLen) {
  if (pieceLen == 0) return true;
  if (chunkUsed + pieceLen > chunkCap) {
    if (chunkUsed > 0) {
      if (!sendLargeText(clientSock, chunkBuf, chunkUsed)) return false;
      chunkUsed = 0;
    }
    if (pieceLen >= chunkCap) {
      return sendLargeText(clientSock, piece, pieceLen);
    }
  }
  memcpy(chunkBuf + chunkUsed, piece, pieceLen);
  chunkUsed += pieceLen;
  return true;
}

bool sendScanResultsJSON(int clientSock) {
  std::vector<WiFiScanResult> snap = snapshotScanResults();
  char headBuf[48];
  int headN = snprintf(headBuf, sizeof(headBuf), "{\"count\":%lu,\"networks\":[", (unsigned long)snap.size());
  if (headN <= 0 || (size_t)headN >= sizeof(headBuf)) return false;

  // Pass 1: compute exact Content-Length on stack without allocating a multi-KB String
  size_t totalLen = (size_t)headN + 2; // + "]}"
  char itemBuf[320];
  for (size_t i = 0; i < snap.size(); i++) {
    totalLen += formatScanItemBuf(snap[i], i, itemBuf, sizeof(itemBuf));
  }

  char hdrBuf[256];
  int hn = formatHttpHeaderBuf(200, "application/json", (int)totalLen, hdrBuf, sizeof(hdrBuf));
  if (hn <= 0 || (size_t)hn >= sizeof(hdrBuf)) return false;
  if (!sendLargeText(clientSock, hdrBuf, (size_t)hn)) return false;

  // Pass 2: stream JSON payload through a 1KB stack buffer
  char chunkBuf[1024];
  size_t chunkUsed = 0;
  if (!appendStreamChunk(clientSock, chunkBuf, sizeof(chunkBuf), chunkUsed, headBuf, (size_t)headN)) return false;
  for (size_t i = 0; i < snap.size(); i++) {
    size_t itemLen = formatScanItemBuf(snap[i], i, itemBuf, sizeof(itemBuf));
    if (!appendStreamChunk(clientSock, chunkBuf, sizeof(chunkBuf), chunkUsed, itemBuf, itemLen)) return false;
  }
  if (!appendStreamChunk(clientSock, chunkBuf, sizeof(chunkBuf), chunkUsed, "]}", 2)) return false;
  if (chunkUsed > 0 && !sendLargeText(clientSock, chunkBuf, chunkUsed)) return false;
  return true;
}

String getScanResultsJSON() {
  std::vector<WiFiScanResult> snap = snapshotScanResults();
  String json;
  json.reserve(64 + snap.size() * 144);
  char headBuf[48];
  snprintf(headBuf, sizeof(headBuf), "{\"count\":%lu,\"networks\":[", (unsigned long)snap.size());
  json = headBuf;
  char itemBuf[320];
  for (size_t i = 0; i < snap.size(); i++) {
    formatScanItemBuf(snap[i], i, itemBuf, sizeof(itemBuf));
    json += itemBuf;
  }
  json += "]}";
  return json;
}

size_t buildStatusJSON(char *out, size_t outSize) {
  if (!out || outSize == 0) return 0;
  size_t scanCount = 0;
  size_t targetCount = 0;
  bool targetModeSnap = false;
  getScanAndTargetCountsSafe(scanCount, targetCount, targetModeSnap);

  bool sniffingSnap = false;
  bool deauthSniffSnap = false;
  int chSnap = WIFI_CHANNEL;
  rtw_enter_critical(NULL, NULL);
  sniffingSnap = isSniffing;
  deauthSniffSnap = deauth_sniff_enabled;
  chSnap = currentChannel;
  rtw_exit_critical(NULL, NULL);
  bool bleConnectedSnap = isBleConnectedSafe();

  char escSsid[80], escBle[64];
  jsonEscapeToBuf(ap_ssid, escSsid, sizeof(escSsid));
  jsonEscapeToBuf(ble_name, escBle, sizeof(escBle));

  char remField[48] = "";
  unsigned long nowMs = millis();
  if (timedAttackEnabled) {
    unsigned long elapsed = nowMs - attackStartTime;
    unsigned long remaining = (attackDuration > elapsed) ? (attackDuration - elapsed) : 0;
    snprintf(remField, sizeof(remField), "\"timed_remaining\":%lu,", remaining);
  } else if (timedSniffEnabled) {
    unsigned long elapsed = nowMs - sniffStartTime;
    unsigned long remaining = (sniffDuration > elapsed) ? (sniffDuration - elapsed) : 0;
    snprintf(remField, sizeof(remField), "\"timed_remaining\":%lu,", remaining);
  }

  int n = snprintf(out, outSize,
                   "{\"attack_enabled\":%s,\"disassoc_enabled\":%s,\"timed_attack\":%s,\"timed_sniff\":%s,"
                   "\"sniffing\":%s,\"hopping\":%s,\"target_mode\":%s,\"scan_count\":%lu,\"target_count\":%lu,"
                   "\"current_channel\":%d,\"beacon_flood\":%s,\"probe_flood\":%s,\"signal_jam\":%s,"
                   "\"deauth_sniff\":%s,\"tx_ok\":%lu,\"tx_fail\":%lu,\"ssid\":\"%s\",\"ble_name\":\"%s\","
                   "\"ap_enabled\":%s,\"ble_connected\":%s,%s\"uptime\":%lu}",
                   attack_enabled ? "true" : "false",
                   disassoc_enabled ? "true" : "false",
                   timedAttackEnabled ? "true" : "false",
                   timedSniffEnabled ? "true" : "false",
                   sniffingSnap ? "true" : "false",
                   isHopping ? "true" : "false",
                   targetModeSnap ? "true" : "false",
                   (unsigned long)scanCount,
                   (unsigned long)targetCount,
                   chSnap,
                   beacon_flood_enabled ? "true" : "false",
                   probe_flood_enabled ? "true" : "false",
                   signal_jam_enabled ? "true" : "false",
                   deauthSniffSnap ? "true" : "false",
                   (unsigned long)tx_success_count,
                   (unsigned long)tx_fail_count,
                   escSsid,
                   escBle,
                   apEnabled ? "true" : "false",
                   bleConnectedSnap ? "true" : "false",
                   remField,
                   nowMs);
  if (n <= 0) {
    out[0] = '\0';
    return 0;
  }
  return ((size_t)n < outSize) ? (size_t)n : (outSize - 1);
}

String getStatusJSON() {
  char buf[768];
  buildStatusJSON(buf, sizeof(buf));
  return String(buf);
}

static inline void fnv1aMixBytes(uint32_t &h, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 16777619UL;
  }
}

static inline void fnv1aMixU32(uint32_t &h, uint32_t v) {
  fnv1aMixBytes(h, &v, sizeof(v));
}

static void computeStatusEtag(char *etagOut, size_t etagCap) {
  size_t scanCount = 0, targetCount = 0;
  bool targetModeSnap = false;
  getScanAndTargetCountsSafe(scanCount, targetCount, targetModeSnap);

  bool sniffingSnap = false, deauthSniffSnap = false;
  int chSnap = WIFI_CHANNEL;
  rtw_enter_critical(NULL, NULL);
  sniffingSnap = isSniffing;
  deauthSniffSnap = deauth_sniff_enabled;
  chSnap = currentChannel;
  rtw_exit_critical(NULL, NULL);
  bool bleConnectedSnap = isBleConnectedSafe();

  uint32_t remSec = 0;
  unsigned long nowMs = millis();
  if (timedAttackEnabled) {
    unsigned long elapsed = nowMs - attackStartTime;
    unsigned long remaining = (attackDuration > elapsed) ? (attackDuration - elapsed) : 0;
    remSec = (uint32_t)((remaining + 999UL) / 1000UL);
  } else if (timedSniffEnabled) {
    unsigned long elapsed = nowMs - sniffStartTime;
    unsigned long remaining = (sniffDuration > elapsed) ? (sniffDuration - elapsed) : 0;
    remSec = (uint32_t)((remaining + 999UL) / 1000UL);
  }

  uint32_t h = 2166136261UL;
  uint32_t flags =
      (attack_enabled ? (1U << 0) : 0U) |
      (disassoc_enabled ? (1U << 1) : 0U) |
      (timedAttackEnabled ? (1U << 2) : 0U) |
      (timedSniffEnabled ? (1U << 3) : 0U) |
      (sniffingSnap ? (1U << 4) : 0U) |
      (isHopping ? (1U << 5) : 0U) |
      (targetModeSnap ? (1U << 6) : 0U) |
      (beacon_flood_enabled ? (1U << 7) : 0U) |
      (probe_flood_enabled ? (1U << 8) : 0U) |
      (signal_jam_enabled ? (1U << 9) : 0U) |
      (deauthSniffSnap ? (1U << 10) : 0U) |
      (apEnabled ? (1U << 11) : 0U) |
      (bleConnectedSnap ? (1U << 12) : 0U);
  fnv1aMixU32(h, flags);
  fnv1aMixU32(h, (uint32_t)scanCount);
  fnv1aMixU32(h, (uint32_t)targetCount);
  fnv1aMixU32(h, (uint32_t)chSnap);
  fnv1aMixU32(h, tx_success_count);
  fnv1aMixU32(h, tx_fail_count);
  fnv1aMixU32(h, remSec);
  fnv1aMixBytes(h, ap_ssid, strlen(ap_ssid));
  fnv1aMixBytes(h, ble_name, strlen(ble_name));

  snprintf(etagOut, etagCap, "\"wdk-%08lx\"", (unsigned long)h);
}

size_t buildInfoJSON(char *out, size_t outSize) {
  if (!out || outSize == 0) return 0;
  char escSsid[80], escBle[64];
  jsonEscapeToBuf(ap_ssid, escSsid, sizeof(escSsid));
  jsonEscapeToBuf(ble_name, escBle, sizeof(escBle));

  // 决策 C：HTTP /api/info 不返回明文 ap_pass，仅留空提示未修改
  int n = snprintf(out, outSize,
                   "{\"version\":\"%s\",\"cycle_delay\":%lu,\"scan_time\":%lu,\"num_frames\":%lu,\"start_channel\":%d,"
                   "\"scan_between_cycles\":%s,\"led\":%s,\"ssid\":\"%s\",\"pass\":\"\",\"ble_name\":\"%s\",\"channel\":%d}",
                   FIRMWARE_VERSION,
                   cycle_delay,
                   scan_time,
                   num_send_frames,
                   start_channel,
                   scan_between_cycles ? "true" : "false",
                   USE_LED ? "true" : "false",
                   escSsid,
                   escBle,
                   WIFI_CHANNEL);
  if (n <= 0) {
    out[0] = '\0';
    return 0;
  }
  return ((size_t)n < outSize) ? (size_t)n : (outSize - 1);
}

String getInfoJSON() {
  char buf[480];
  buildInfoJSON(buf, sizeof(buf));
  return String(buf);
}

bool sendLogsJSON(int clientSock, bool hasSince, uint32_t sinceSeq) {
  bool sniffingSnap = false;
  SniffMode modeSnap = SNIFF_STOP;
  bool hoppingSnap = false;
  int chSnap = WIFI_CHANNEL;
  uint32_t curSeq = 0;

  // Fast path: if client's sinceSeq already equals webLogTotalSeq, skip 4KB heap snapshot completely
  if (hasSince) {
    bool upToDate = false;
    rtw_enter_critical(NULL, NULL);
    if (webLogTotalSeq == sinceSeq) {
      upToDate = true;
      curSeq = webLogTotalSeq;
      sniffingSnap = isSniffing;
      modeSnap = currentMode;
      hoppingSnap = isHopping;
      chSnap = currentChannel;
    }
    rtw_exit_critical(NULL, NULL);

    if (upToDate) {
      char emptyResp[144];
      int en = snprintf(emptyResp, sizeof(emptyResp),
                        "{\"sniffing\":%s,\"mode\":%d,\"hopping\":%s,\"channel\":%d,\"seq\":%lu,\"reset\":false,\"logs\":[]}",
                        sniffingSnap ? "true" : "false",
                        (int)modeSnap,
                        hoppingSnap ? "true" : "false",
                        chSnap,
                        (unsigned long)curSeq);
      return (en > 0 && (size_t)en < sizeof(emptyResp)) &&
             sendHttpResponse(clientSock, 200, "application/json", emptyResp, (size_t)en);
    }
  }

  // Allocate transient snapshot on heap outside critical section (zero static .bss footprint)
  char *logSnapshot = (char *)malloc((size_t)WEB_LOG_HISTORY_SIZE * LOG_STR_LEN);
  uint8_t emitCount = 0;
  bool reset = true;

  rtw_enter_critical(NULL, NULL);
  sniffingSnap = isSniffing;
  modeSnap = currentMode;
  hoppingSnap = isHopping;
  chSnap = currentChannel;
  curSeq = webLogTotalSeq;
  uint8_t totalCount = webLogCount;
  if (totalCount > WEB_LOG_HISTORY_SIZE) totalCount = WEB_LOG_HISTORY_SIZE;

  if (hasSince && sinceSeq <= curSeq && (curSeq - sinceSeq) <= (uint32_t)totalCount) {
    reset = false;
    emitCount = (uint8_t)(curSeq - sinceSeq);
  } else {
    reset = true;
    emitCount = totalCount;
  }

  if (logSnapshot != nullptr && emitCount > 0) {
    uint8_t startIdx = (webLogHead + WEB_LOG_HISTORY_SIZE - emitCount) % WEB_LOG_HISTORY_SIZE;
    for (uint8_t i = 0; i < emitCount; i++) {
      uint8_t slot = (startIdx + i) % WEB_LOG_HISTORY_SIZE;
      char *dst = logSnapshot + ((size_t)i * LOG_STR_LEN);
      size_t cp = strlen(webLogHistory[slot]) + 1;
      if (cp > LOG_STR_LEN) cp = LOG_STR_LEN;
      memcpy(dst, webLogHistory[slot], cp);
      dst[LOG_STR_LEN - 1] = '\0';
    }
  } else if (logSnapshot == nullptr) {
    emitCount = 0;
  }
  rtw_exit_critical(NULL, NULL);

  char jsonHead[144];
  int headN = snprintf(jsonHead, sizeof(jsonHead),
                       "{\"sniffing\":%s,\"mode\":%d,\"hopping\":%s,\"channel\":%d,\"seq\":%lu,\"reset\":%s,\"logs\":[",
                       sniffingSnap ? "true" : "false",
                       (int)modeSnap,
                       hoppingSnap ? "true" : "false",
                       chSnap,
                       (unsigned long)curSeq,
                       reset ? "true" : "false");
  if (headN <= 0 || (size_t)headN >= sizeof(jsonHead)) {
    if (logSnapshot != nullptr) free(logSnapshot);
    return false;
  }

  size_t totalLen = (size_t)headN + 2; // + "]}"
  char escLog[256];
  for (uint8_t i = 0; i < emitCount; i++) {
    const char *src = logSnapshot + ((size_t)i * LOG_STR_LEN);
    size_t escLen = jsonEscapeToBuf(src, escLog, sizeof(escLog));
    totalLen += (i > 0 ? 1 : 0) + 2 + escLen;
  }

  char httpHdr[256];
  int hn = formatHttpHeaderBuf(200, "application/json", (int)totalLen, httpHdr, sizeof(httpHdr));
  bool ok = (hn > 0 && (size_t)hn < sizeof(httpHdr)) && sendLargeText(clientSock, httpHdr, (size_t)hn);

  if (ok) {
    char chunkBuf[1024];
    size_t chunkUsed = 0;
    ok = appendStreamChunk(clientSock, chunkBuf, sizeof(chunkBuf), chunkUsed, jsonHead, (size_t)headN);
    for (uint8_t i = 0; ok && i < emitCount; i++) {
      const char *src = logSnapshot + ((size_t)i * LOG_STR_LEN);
      size_t escLen = jsonEscapeToBuf(src, escLog, sizeof(escLog));
      if (i > 0) ok = appendStreamChunk(clientSock, chunkBuf, sizeof(chunkBuf), chunkUsed, ",", 1);
      if (ok) ok = appendStreamChunk(clientSock, chunkBuf, sizeof(chunkBuf), chunkUsed, "\"", 1);
      if (ok) ok = appendStreamChunk(clientSock, chunkBuf, sizeof(chunkBuf), chunkUsed, escLog, escLen);
      if (ok) ok = appendStreamChunk(clientSock, chunkBuf, sizeof(chunkBuf), chunkUsed, "\"", 1);
    }
    if (ok) ok = appendStreamChunk(clientSock, chunkBuf, sizeof(chunkBuf), chunkUsed, "]}", 2);
    if (ok && chunkUsed > 0) ok = sendLargeText(clientSock, chunkBuf, chunkUsed);
  }

  if (logSnapshot != nullptr) {
    free(logSnapshot);
  }
  return ok;
}

String getLogsJSON() {
  char *logSnapshot = (char *)malloc((size_t)WEB_LOG_HISTORY_SIZE * LOG_STR_LEN);
  uint8_t count = 0;
  bool sniffingSnap = false;
  SniffMode modeSnap = SNIFF_STOP;
  bool hoppingSnap = false;
  int chSnap = WIFI_CHANNEL;

  rtw_enter_critical(NULL, NULL);
  sniffingSnap = isSniffing;
  modeSnap = currentMode;
  hoppingSnap = isHopping;
  chSnap = currentChannel;
  if (logSnapshot != nullptr) {
    count = webLogCount;
    if (count > WEB_LOG_HISTORY_SIZE) count = WEB_LOG_HISTORY_SIZE;
    uint8_t startIdx = (webLogHead + WEB_LOG_HISTORY_SIZE - count) % WEB_LOG_HISTORY_SIZE;
    for (uint8_t i = 0; i < count; i++) {
      uint8_t slot = (startIdx + i) % WEB_LOG_HISTORY_SIZE;
      char *dst = logSnapshot + ((size_t)i * LOG_STR_LEN);
      size_t cp = strlen(webLogHistory[slot]) + 1;
      if (cp > LOG_STR_LEN) cp = LOG_STR_LEN;
      memcpy(dst, webLogHistory[slot], cp);
      dst[LOG_STR_LEN - 1] = '\0';
    }
  }
  rtw_exit_critical(NULL, NULL);

  String json;
  json.reserve(96 + (size_t)count * 160);
  char hdrBuf[96];
  snprintf(hdrBuf, sizeof(hdrBuf),
           "{\"sniffing\":%s,\"mode\":%d,\"hopping\":%s,\"channel\":%d,\"logs\":[",
           sniffingSnap ? "true" : "false",
           (int)modeSnap,
           hoppingSnap ? "true" : "false",
           chSnap);
  json = hdrBuf;
  for (uint8_t i = 0; i < count; i++) {
    char escLog[256];
    const char *src = logSnapshot + ((size_t)i * LOG_STR_LEN);
    jsonEscapeToBuf(src, escLog, sizeof(escLog));
    if (i > 0) json += ",";
    json += "\"";
    json += escLog;
    json += "\"";
  }
  if (logSnapshot != nullptr) {
    free(logSnapshot);
  }
  json += "]}";
  return json;
}

//==========================================================
// Web Client Handler
//==========================================================
void handleWebClient() {
  bool sniffingNow = false;
  rtw_enter_critical(NULL, NULL);
  sniffingNow = isSniffing;
  rtw_exit_critical(NULL, NULL);
  if (!apEnabled || sniffingNow) return;  // ap off 或抓包切出 SoftAP 期间不轮询已关闭的 WebServer
  if (!webServerRunning()) {
    // Self-heal WebServer / SoftAP if apEnabled=true outside active attacks/scans
    static unsigned long lastSelfHealMs = 0;
    static unsigned long lastApHealMs = 0;
    unsigned long nowMs = millis();
    if (nowMs - lastSelfHealMs >= 2000) {
      lastSelfHealMs = nowMs;
      if (wifi_is_up(RTW_AP_INTERFACE)) {
        softApActive = true;
        wlanRuntimeMode = WLAN_MODE_AP;
        if (startWebServer(1)) {
          Serial.println("[INFO] WebServer self-healed on port 80.");
        }
      } else if (!pendingApRestart && !scan_enabled &&
                 !attack_enabled && !disassoc_enabled &&
                 !beacon_flood_enabled && !probe_flood_enabled &&
                 !signal_jam_enabled &&
                 (nowMs - lastApHealMs >= 5000)) {
        lastApHealMs = nowMs;
        Serial.println("[WARN] SoftAP interface down while apEnabled=true; recovering...");
        startSoftAP();
      }
    }
    return;
  }

  int client = acceptWebClient();
  if (client < 0) return;

  const size_t MAX_HTTP_REQ_LEN = 4096;
  const unsigned long IDLE_FIRST_BYTE_MS = 250;
  const unsigned long HDR_READ_TIMEOUT_MS = 1000;
  const unsigned long BODY_READ_TIMEOUT_MS = 1200;

  // Direct lwIP non-blocking accept + bounded SO_RCVTIMEO=25ms recv into fixed buffer
  static char httpReqBuf[4098];
  size_t reqLen = 0;
  httpReqBuf[0] = '\0';

  unsigned long startMs = millis();
  int headerEnd = -1;
  int sepLen = 4;
  bool tooLarge = false;
  bool peerDead = false;

  auto findHeaderTerm = [&](size_t scanFrom) -> void {
    const char *crlf = strstr(httpReqBuf + scanFrom, "\r\n\r\n");
    const char *lflf = strstr(httpReqBuf + scanFrom, "\n\n");
    if (crlf && (!lflf || crlf < lflf)) {
      headerEnd = (int)(crlf - httpReqBuf);
      sepLen = 4;
    } else if (lflf) {
      headerEnd = (int)(lflf - httpReqBuf);
      sepLen = 2;
    }
  };

  while (!peerDead && (millis() - startMs < HDR_READ_TIMEOUT_MS)) {
    feedWatchdog();
    if (reqLen == 0 && (millis() - startMs >= IDLE_FIRST_BYTE_MS)) {
      break;
    }
    size_t remainCap = (MAX_HTTP_REQ_LEN + 1) - reqLen;
    if (remainCap == 0) {
      tooLarge = true;
      break;
    }
    size_t readWant = (remainCap > 512) ? 512 : remainCap;
    int n = webClientRecv(client, (uint8_t *)(httpReqBuf + reqLen), readWant, peerDead);
    if (n > 0) {
      size_t scanFrom = (reqLen > 4) ? (reqLen - 4) : 0;
      reqLen += (size_t)n;
      httpReqBuf[reqLen] = '\0';
      if (reqLen > MAX_HTTP_REQ_LEN) {
        tooLarge = true;
        break;
      }
      findHeaderTerm(scanFrom);
      if (headerEnd >= 0) break;
    }
  }

  if (reqLen == 0 && headerEnd < 0 && !tooLarge) {
    closeWebClient(client, false);
    return;
  }

  if (tooLarge) {
    static const char ERR_413[] = "{\"status\":\"error\",\"message\":\"Request entity too large\"}";
    bool sentOk = sendHttpResponse(client, 413, "application/json", ERR_413, sizeof(ERR_413) - 1);
    closeWebClient(client, sentOk);
    return;
  }

  if (reqLen == 0 || headerEnd < 0) {
    static const char ERR_400_TIMEOUT[] = "{\"status\":\"error\",\"message\":\"Malformed or timed-out HTTP request\"}";
    bool sentOk = sendHttpResponse(client, 400, "application/json", ERR_400_TIMEOUT, sizeof(ERR_400_TIMEOUT) - 1);
    closeWebClient(client, sentOk);
    return;
  }

  char methodBuf[16];
  char fullPathBuf[384];
  parseHttpRequestLineCstr(httpReqBuf, (size_t)headerEnd, methodBuf, sizeof(methodBuf), fullPathBuf, sizeof(fullPathBuf));
  if (methodBuf[0] == '\0') {
    static const char ERR_400_REQ[] = "{\"status\":\"error\",\"message\":\"Invalid HTTP request line\"}";
    bool sentOk = sendHttpResponse(client, 400, "application/json", ERR_400_REQ, sizeof(ERR_400_REQ) - 1);
    closeWebClient(client, sentOk);
    return;
  }

  char inmBuf[40];
  parseIfNoneMatchHeaderCstr(httpReqBuf, (size_t)headerEnd, inmBuf, sizeof(inmBuf));

  long contentLength = 0;
  int clStatus = parseContentLengthHeaderCstr(httpReqBuf, (size_t)headerEnd, contentLength);
  if (clStatus < 0) {
    static const char ERR_400_CL[] = "{\"status\":\"error\",\"message\":\"Invalid Content-Length header\"}";
    bool sentOk = sendHttpResponse(client, 400, "application/json", ERR_400_CL, sizeof(ERR_400_CL) - 1);
    closeWebClient(client, sentOk);
    return;
  }
  if ((size_t)contentLength > MAX_HTTP_REQ_LEN ||
      ((size_t)headerEnd + (size_t)sepLen + (size_t)contentLength) > MAX_HTTP_REQ_LEN) {
    static const char ERR_413_LIMIT[] = "{\"status\":\"error\",\"message\":\"Payload exceeds limit\"}";
    bool sentOk = sendHttpResponse(client, 413, "application/json", ERR_413_LIMIT, sizeof(ERR_413_LIMIT) - 1);
    closeWebClient(client, sentOk);
    return;
  }

  size_t bodyStart = (size_t)headerEnd + (size_t)sepLen;
  size_t targetTotalLen = bodyStart + (size_t)contentLength;
  if (contentLength > 0 && reqLen < targetTotalLen) {
    unsigned long bodyWaitStart = millis();
    bool bodyPeerDead = false;
    while (reqLen < targetTotalLen &&
           !bodyPeerDead &&
           (millis() - bodyWaitStart < BODY_READ_TIMEOUT_MS)) {
      feedWatchdog();
      size_t need = targetTotalLen - reqLen;
      int n = webClientRecv(client, (uint8_t *)(httpReqBuf + reqLen), need, bodyPeerDead);
      if (n > 0) {
        reqLen += (size_t)n;
        httpReqBuf[reqLen] = '\0';
      }
    }
    if (reqLen < targetTotalLen) {
      static const char ERR_408[] = "{\"status\":\"error\",\"message\":\"Incomplete request body\"}";
      bool sentOk = sendHttpResponse(client, 408, "application/json", ERR_408, sizeof(ERR_408) - 1);
      closeWebClient(client, sentOk);
      return;
    }
  }

  String body;
  if (contentLength > 0) {
    char savedEnd = httpReqBuf[targetTotalLen];
    httpReqBuf[targetTotalLen] = '\0';
    body = (httpReqBuf + bodyStart);
    httpReqBuf[targetTotalLen] = savedEnd;
  } else if (reqLen > bodyStart) {
    body = (httpReqBuf + bodyStart);
  }

  char *qMark = strchr(fullPathBuf, '?');
  char pathBuf[128];
  if (qMark != nullptr) {
    size_t pLen = (size_t)(qMark - fullPathBuf);
    if (pLen >= sizeof(pathBuf)) pLen = sizeof(pathBuf) - 1;
    memcpy(pathBuf, fullPathBuf, pLen);
    pathBuf[pLen] = '\0';
  } else {
    strncpy(pathBuf, fullPathBuf, sizeof(pathBuf) - 1);
    pathBuf[sizeof(pathBuf) - 1] = '\0';
  }

  Serial.print("[WEB] ");
  Serial.print(methodBuf);
  Serial.print(" ");
  Serial.println(fullPathBuf);

  if (USE_LED) digitalWrite(LED_G, HIGH);
  bool clientClosed = false;
  static const char ERR_405[] = "{\"status\":\"error\",\"message\":\"Method Not Allowed\"}";
  bool isGet = (strcmp(methodBuf, "GET") == 0);
  bool isPost = (strcmp(methodBuf, "POST") == 0);

  if (strcmp(pathBuf, "/api/scan") == 0) {
    if (isGet) {
      sendScanResultsJSON(client);
    } else if (isPost) {
      String errMsg;
      if (executeCommand("scan", &errMsg, true)) {
        char resp[64];
        int rn = snprintf(resp, sizeof(resp), "{\"status\":\"scan_complete\",\"count\":%lu}",
                          (unsigned long)getScanCountSafe());
        sendHttpResponse(client, 200, "application/json", resp, (rn > 0) ? (size_t)rn : 0);
      } else {
        char escErr[192], resp[256];
        jsonEscapeToBuf(errMsg.length() ? errMsg.c_str() : "Wi-Fi scan failed", escErr, sizeof(escErr));
        int rn = snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"%s\"}", escErr);
        sendHttpResponse(client, 500, "application/json", resp, (rn > 0) ? (size_t)rn : 0);
      }
    } else {
      sendHttpResponse(client, 405, "application/json", ERR_405, sizeof(ERR_405) - 1);
    }
  }
  else if (strcmp(pathBuf, "/api/status") == 0) {
    if (isGet) {
      char curEtag[32];
      computeStatusEtag(curEtag, sizeof(curEtag));
      if (inmBuf[0] != '\0' && strcmp(inmBuf, curEtag) == 0) {
        char hdr304[160];
        int hn = snprintf(hdr304, sizeof(hdr304),
                          "HTTP/1.1 304 Not Modified\r\nETag: %s\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",
                          curEtag);
        if (hn > 0 && (size_t)hn < sizeof(hdr304)) {
          sendLargeText(client, hdr304, (size_t)hn);
        }
      } else {
        char statusBuf[768];
        size_t statusLen = buildStatusJSON(statusBuf, sizeof(statusBuf));
        char hdr200[256];
        int hn = snprintf(hdr200, sizeof(hdr200),
                          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nETag: %s\r\nCache-Control: no-cache\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",
                          curEtag, (unsigned long)statusLen);
        if (hn > 0 && (size_t)hn < sizeof(hdr200) && sendLargeText(client, hdr200, (size_t)hn)) {
          sendLargeText(client, statusBuf, statusLen);
        }
      }
    } else {
      sendHttpResponse(client, 405, "application/json", ERR_405, sizeof(ERR_405) - 1);
    }
  }
  else if (strcmp(pathBuf, "/api/info") == 0) {
    if (isGet) {
      char infoBuf[480];
      size_t infoLen = buildInfoJSON(infoBuf, sizeof(infoBuf));
      sendHttpResponse(client, 200, "application/json", infoBuf, infoLen);
    } else {
      sendHttpResponse(client, 405, "application/json", ERR_405, sizeof(ERR_405) - 1);
    }
  }
  else if (strcmp(pathBuf, "/api/logs") == 0) {
    if (isGet) {
      bool hasSince = false;
      uint32_t sinceSeq = 0;
      if (qMark != nullptr) {
        String fullPath(fullPathBuf);
        String sinceParam = getQueryParam(fullPath, "since");
        sinceParam.trim();
        long parsedSince = -1;
        if (sinceParam.length() > 0 && parseStrictLong(sinceParam, parsedSince) && parsedSince >= 0) {
          hasSince = true;
          sinceSeq = (uint32_t)parsedSince;
        }
      }
      sendLogsJSON(client, hasSince, sinceSeq);
    } else {
      sendHttpResponse(client, 405, "application/json", ERR_405, sizeof(ERR_405) - 1);
    }
  }
  else if (strcmp(pathBuf, "/api/logs/clear") == 0) {
    if (isPost) {
      String errMsg;
      executeCommand("clear_logs", &errMsg, true);
      static const char OK_JSON[] = "{\"status\":\"ok\"}";
      sendHttpResponse(client, 200, "application/json", OK_JSON, sizeof(OK_JSON) - 1);
    } else {
      sendHttpResponse(client, 405, "application/json", ERR_405, sizeof(ERR_405) - 1);
    }
  }
  else if (strcmp(pathBuf, "/api/command") == 0) {
    if (!isPost) {
      sendHttpResponse(client, 405, "application/json", ERR_405, sizeof(ERR_405) - 1);
    } else {
      String fullPath(fullPathBuf);
      String cmd = hasFormKey(body, "cmd") ? getPostValue(body, "cmd") : getQueryParam(fullPath, "cmd");
      cmd.trim();
      if (cmd.length() == 0) {
        static const char ERR_EMPTY_CMD[] = "{\"status\":\"error\",\"message\":\"No command provided\"}";
        sendHttpResponse(client, 400, "application/json", ERR_EMPTY_CMD, sizeof(ERR_EMPTY_CMD) - 1);
      } else if (cmd.equalsIgnoreCase("ap off")) {
        // 决策 B：禁止通过 WebUI/HTTP 永久关闭 SoftAP
        static const char ERR_AP_OFF[] = "{\"status\":\"error\",\"message\":\"'ap off' is disabled over WebUI/HTTP; use BLE or Serial.\"}";
        sendHttpResponse(client, 403, "application/json", ERR_AP_OFF, sizeof(ERR_AP_OFF) - 1);
      } else {
        if (isStopCmdCstr(cmd.c_str())) {
          rtw_enter_critical(NULL, NULL);
          emergencyStopRequested = true;
          priorityStopPending = true;
          rtw_exit_critical(NULL, NULL);
        }
        bool entersPromisc = (cmd.equalsIgnoreCase("start sniff") ||
                              cmd.equalsIgnoreCase("sniff all") ||
                              cmd.equalsIgnoreCase("sniff beacon") ||
                              cmd.equalsIgnoreCase("sniff probe") ||
                              cmd.equalsIgnoreCase("sniff deauth") ||
                              cmd.equalsIgnoreCase("sniff eapol") ||
                              cmd.equalsIgnoreCase("sniff pwnagotchi") ||
                              startsWithIgnoreCase(cmd, "sniff_time ") ||
                              cmd.equalsIgnoreCase("deauth_sniff") ||
                              cmd.equalsIgnoreCase("deauth sniff"));
        if (entersPromisc) {
          String errMsg;
          bool preValid = true;
          if (startsWithIgnoreCase(cmd, "sniff_time ")) {
            unsigned long dummyDur = 0;
            SniffMode dummyMode = SNIFF_ALL;
            preValid = parseSniffTimeCmd(cmd, dummyDur, dummyMode, errMsg);
          }
          if (!preValid) {
            char escErr[192], resp[256];
            jsonEscapeToBuf(errMsg.c_str(), escErr, sizeof(escErr));
            int rn = snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"%s\"}", escErr);
            sendHttpResponse(client, 400, "application/json", resp, (rn > 0) ? (size_t)rn : 0);
          } else {
            // 先完整发送 HTTP 200 并等待刷出与单次关闭连接，再切入混杂/STA 模式
            char escCmd[192], resp[256];
            jsonEscapeToBuf(cmd.c_str(), escCmd, sizeof(escCmd));
            int rn = snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"command\":\"%s\"}", escCmd);
            bool sentOk = sendHttpResponse(client, 200, "application/json", resp, (rn > 0) ? (size_t)rn : 0);
            closeWebClient(client, sentOk);
            delay(35);
            clientClosed = true;
            executeCommand(cmd, &errMsg, true);
            // If no BLE client is connected, protect pure-WebUI users with an automatic 15s recovery timer
            bool sniffStarted = false;
            rtw_enter_critical(NULL, NULL);
            sniffStarted = isSniffing;
            rtw_exit_critical(NULL, NULL);
            if (sniffStarted && !isBleConnectedSafe() && !timedSniffEnabled) {
              timedSniffEnabled = true;
              sniffStartTime = millis();
              sniffDuration = 15000;
            }
          }
        } else {
          String errMsg;
          if (executeCommand(cmd, &errMsg, true)) {
            char escCmd[192], resp[256];
            jsonEscapeToBuf(cmd.c_str(), escCmd, sizeof(escCmd));
            int rn = snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"command\":\"%s\"}", escCmd);
            sendHttpResponse(client, 200, "application/json", resp, (rn > 0) ? (size_t)rn : 0);
          } else {
            int statusCode = (cmd.equalsIgnoreCase("scan")) ? 500 : 400;
            char escErr[192], resp[256];
            jsonEscapeToBuf(errMsg.length() ? errMsg.c_str() : "Command failed", escErr, sizeof(escErr));
            int rn = snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"%s\"}", escErr);
            sendHttpResponse(client, statusCode, "application/json", resp, (rn > 0) ? (size_t)rn : 0);
          }
        }
      }
    }
  }
  else if (strcmp(pathBuf, "/api/target") == 0) {
    if (!isPost) {
      sendHttpResponse(client, 405, "application/json", ERR_405, sizeof(ERR_405) - 1);
    } else {
      String fullPath(fullPathBuf);
      String queryStr = (qMark != nullptr) ? String(qMark + 1) : "";
      bool hasIndices = hasFormKey(body, "indices") || hasFormKey(queryStr, "indices");
      if (!hasIndices) {
        static const char ERR_IDX[] = "{\"status\":\"error\",\"message\":\"Missing 'indices' parameter\"}";
        sendHttpResponse(client, 400, "application/json", ERR_IDX, sizeof(ERR_IDX) - 1);
      } else {
        String indices = hasFormKey(body, "indices") ? getPostValue(body, "indices") : getQueryParam(fullPath, "indices");
        indices.trim();
        String errMsg;
        bool ok = applyTargetSpec(indices, true, errMsg);
        size_t scanCount = 0;
        size_t targetCount = 0;
        bool targetModeSnap = false;
        getScanAndTargetCountsSafe(scanCount, targetCount, targetModeSnap);
        if (!ok) {
          char escErr[192], resp[280];
          jsonEscapeToBuf(errMsg.c_str(), escErr, sizeof(escErr));
          int rn = snprintf(resp, sizeof(resp),
                            "{\"status\":\"error\",\"message\":\"%s\",\"targets\":%lu,\"target_mode\":%s}",
                            escErr, (unsigned long)targetCount, targetModeSnap ? "true" : "false");
          sendHttpResponse(client, 400, "application/json", resp, (rn > 0) ? (size_t)rn : 0);
        } else {
          blePushStatus();
          char resp[96];
          int rn = snprintf(resp, sizeof(resp),
                            "{\"status\":\"ok\",\"targets\":%lu,\"target_mode\":%s}",
                            (unsigned long)targetCount, targetModeSnap ? "true" : "false");
          sendHttpResponse(client, 200, "application/json", resp, (rn > 0) ? (size_t)rn : 0);
        }
      }
    }
  }
  else if (strcmp(pathBuf, "/api/config") == 0) {
    if (!isPost) {
      sendHttpResponse(client, 405, "application/json", ERR_405, sizeof(ERR_405) - 1);
    } else {
      static const char* const cfgKeys[] = {
        "ap_ssid", "ap_pass", "ble_name", "cycle_delay", "scan_time",
        "num_frames", "start_channel", "led", "scan_cycles"
      };
      const size_t numCfgKeys = sizeof(cfgKeys) / sizeof(cfgKeys[0]);
      String errMsg = "";
      bool valid = true;
      bool hasAnyKey = false;
      for (size_t k = 0; k < numCfgKeys; k++) {
        String key = cfgKeys[k];
        if (!hasFormKey(body, key)) continue;
        hasAnyKey = true;
        String val = getPostValue(body, key);
        val.trim();
        if (!validateConfigSetting(key, val, errMsg)) {
          valid = false;
          break;
        }
      }
      if (!hasAnyKey) {
        static const char ERR_NO_CFG[] = "{\"status\":\"error\",\"message\":\"No valid configuration parameters provided\"}";
        sendHttpResponse(client, 400, "application/json", ERR_NO_CFG, sizeof(ERR_NO_CFG) - 1);
      } else if (!valid) {
        char escErr[192], resp[256];
        jsonEscapeToBuf(errMsg.c_str(), escErr, sizeof(escErr));
        int rn = snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"%s\"}", escErr);
        sendHttpResponse(client, 400, "application/json", resp, (rn > 0) ? (size_t)rn : 0);
      } else {
        String result = "";
        bool apChanged = false;
        bool anyUpdated = false;
        for (size_t k = 0; k < numCfgKeys; k++) {
          String key = cfgKeys[k];
          if (!hasFormKey(body, key)) continue;
          String val = getPostValue(body, key);
          val.trim();
          if (applyConfigSetting(key, val, apChanged)) {
            anyUpdated = true;
            result += key + " updated ";
          }
        }
        if (anyUpdated) {
          saveConfigToFlash();
          blePushInfo();
          if (apChanged) {
            scheduleApRestart();
          }
        }
        char escRes[192], resp[280];
        jsonEscapeToBuf(result.c_str(), escRes, sizeof(escRes));
        int rn = snprintf(resp, sizeof(resp),
                          "{\"status\":\"ok\",\"ap_restarted\":%s,\"message\":\"%s\"}",
                          pendingApRestart ? "true" : "false", escRes);
        sendHttpResponse(client, 200, "application/json", resp, (rn > 0) ? (size_t)rn : 0);
      }
    }
  }
  else if (strcmp(pathBuf, "/") == 0) {
    if (isGet) {
      sendHttpResponseGzip(client, 200, "text/html; charset=utf-8", INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
    } else {
      sendHttpResponse(client, 405, "application/json", ERR_405, sizeof(ERR_405) - 1);
    }
  }
  else {
    static const char ERR_404[] = "404 Not Found";
    sendHttpResponse(client, 404, "text/plain", ERR_404, sizeof(ERR_404) - 1);
  }

  if (!clientClosed) {
    closeWebClient(client, true);
  }
  if (USE_LED) digitalWrite(LED_G, LOW);
}
