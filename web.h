/*
 * web.h — Direct lwIP BSD-socket HTTP server, zero-heap JSON streaming & gzipped WebUI
 * Part of WiFi-Deck multi-file refactor (P1 optimized).
 */
#pragma once

#include <Arduino.h>
#include <cstddef>
#include <cstdint>

// Web server lifecycle & request dispatcher
bool webServerRunning();
bool startWebServer(uint8_t maxRetries = 3);
void stopWebServer();
void handleWebClient();

// Zero-heap stack-buffer & streaming JSON builders (hot-path preferred)
size_t buildStatusJSON(char *out, size_t outSize);
size_t buildInfoJSON(char *out, size_t outSize);
bool sendScanResultsJSON(int clientSock);
bool sendLogsJSON(int clientSock, bool hasSince = false, uint32_t sinceSeq = 0);

// Compatibility String JSON wrappers
String getScanResultsJSON();
String getStatusJSON();
String getInfoJSON();
String getLogsJSON();

// Zero-allocation C-buffer HTTP header parsers
void parseHttpRequestLineCstr(const char *hdrBuf, size_t hdrLen,
                              char *methodOut, size_t methodCap,
                              char *fullPathOut, size_t pathCap);
int parseContentLengthHeaderCstr(const char *hdrBuf, size_t hdrLen, long &outLen);

// HTTP response & legacy String helpers
int formatHttpHeaderBuf(int code, const char *contentType, int contentLength, char *out, size_t outSize);
String httpHeader(int code, const String &contentType, int contentLength = -1);
bool sendLargeText(int clientSock, const char *data, size_t totalLen);
bool sendHttpResponse(int clientSock, int code, const char *contentType, const char *body, size_t bodyLen);
bool sendHttpResponseGzip(int clientSock, int code, const char *contentType, const uint8_t *gzBody, size_t gzLen);
String getHttpRequestLine(const String &request);
String parsePath(const String &request);
String parseMethod(const String &request);
String parseBody(const String &request);
int parseContentLengthHeader(const String &headers, long &outLen);
