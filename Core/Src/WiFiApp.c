#include "WiFiApp.h"
#include "ESP8266.h"
#include "VisualizerApp.h"
#include "fatfs.h"
#include "cmsis_os.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char g_SSID[64] = DEFAULT_WIFI_SSID;
static char g_Password[64] = DEFAULT_WIFI_PASS;
static char g_DisplayStatus[48] = "WiFi: Offline";

extern UART_HandleTypeDef huart6;
extern int current_cpu_load;
extern float g_BaudRate;

// Streaming raw line message structure & FreeRTOS queue handle
typedef struct {
    char text[64];
} RawLineMsg;

static osMessageQId s_RawLineQueue = NULL;
static int8_t s_RawClientLinkId = -1;
static uint32_t s_LastKeepaliveTick = 0;

// Static response and scratch buffers - dedicated and non-overlapping to avoid aliasing & stack overflow
static char s_HttpTxBuf[2048]; // EXCLUSIVELY used inside SendHTTPResponse
static char s_RxTextRaw[768];
static char s_RxTextJson[600];
static char s_StatsText[128];
static char s_JsonBody[1024];
static char s_InfoBuf[1024];

// Case-insensitive string comparison helpers
static int ci_strncasecmp(const char* s1, const char* s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c1 = (unsigned char)s1[i];
        unsigned char c2 = (unsigned char)s2[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 += ('a' - 'A');
        if (c2 >= 'A' && c2 <= 'Z') c2 += ('a' - 'A');
        if (c1 != c2) return c1 - c2;
        if (c1 == '\0') break;
    }
    return 0;
}

static const char* ci_strstr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return NULL;
    size_t nlen = strlen(needle);
    if (nlen == 0) return haystack;
    while (*haystack) {
        if (ci_strncasecmp(haystack, needle, nlen) == 0) {
            return haystack;
        }
        haystack++;
    }
    return NULL;
}

// Push a newly decoded FSK line to the live streaming queue
void WiFiApp_PushRawLine(const char* line) {
    if (s_RawLineQueue && line && strlen(line) > 0) {
        RawLineMsg msg;
        strncpy(msg.text, line, sizeof(msg.text) - 1);
        msg.text[sizeof(msg.text) - 1] = '\0';
        xQueueSend((QueueHandle_t)s_RawLineQueue, &msg, 0);
    }
}

// Try to load credentials from /WIFI.TXT on SD card
static void LoadWiFiCredentialsFromSD(void) {
    FIL file;
    if (f_open(&file, "/WIFI.TXT", FA_READ) == FR_OK) {
        printf("[WIFI_APP] Found /WIFI.TXT on SD card. Loading credentials...\r\n");
        char line[128];
        while (f_gets(line, sizeof(line), &file)) {
            char* p = line;
            while (*p) {
                if (*p == '\r' || *p == '\n') { *p = '\0'; break; }
                p++;
            }
            if (strncmp(line, "SSID=", 5) == 0) {
                strncpy(g_SSID, line + 5, sizeof(g_SSID) - 1);
                g_SSID[sizeof(g_SSID) - 1] = '\0';
            } else if (strncmp(line, "PASS=", 5) == 0) {
                strncpy(g_Password, line + 5, sizeof(g_Password) - 1);
                g_Password[sizeof(g_Password) - 1] = '\0';
            }
        }
        f_close(&file);
        printf("[WIFI_APP] Configured SSID: '%s'\r\n", g_SSID);
    } else {
        printf("[WIFI_APP] No /WIFI.TXT found. Using default SSID: '%s'\r\n", g_SSID);
    }
}

// Helper to send HTTP Response with CORS enabled in a single atomic AT+CIPSEND transaction
static void SendHTTPResponse(uint8_t link_id, const char* content_type, const char* body) {
    int bodyLen = body ? strlen(body) : 0;
    int headerLen = snprintf(s_HttpTxBuf, sizeof(s_HttpTxBuf),
        "HTTP/1.1 200 OK\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        content_type ? content_type : "text/plain", bodyLen);

    if (headerLen < 0 || headerLen >= (int)sizeof(s_HttpTxBuf)) return;

    if (bodyLen > 0 && (headerLen + bodyLen < (int)sizeof(s_HttpTxBuf))) {
        memcpy(s_HttpTxBuf + headerLen, body, bodyLen);
        headerLen += bodyLen;
    }

    ESP8266_SendTCPData(link_id, (const uint8_t*)s_HttpTxBuf, (uint16_t)headerLen);
    osDelay(20);
    ESP8266_CloseConnection(link_id);
}

// Sanitize string for JSON (escape quotes and newlines)
static void EscapeForJSON(const char* src, char* dst, uint16_t maxLen) {
    uint16_t d = 0;
    while (*src && d < maxLen - 4) {
        if (*src == '\"') {
            dst[d++] = '\\'; dst[d++] = '\"';
        } else if (*src == '\\') {
            dst[d++] = '\\'; dst[d++] = '\\';
        } else if (*src == '\n') {
            dst[d++] = '\\'; dst[d++] = 'n';
        } else if (*src == '\r') {
            // skip carriage return in json
        } else {
            dst[d++] = *src;
        }
        src++;
    }
    dst[d] = '\0';
}

// Incoming Packet Dispatcher (Supports /info, /raw, /rawdct, /dct, /api/status, /api/cmd, /api/tx, etc.)
static void WiFiApp_HandleIncomingPacket(uint8_t link_id, const uint8_t* data, uint16_t len) {
    static char req[512];
    uint16_t copyLen = (len < sizeof(req) - 1) ? len : (sizeof(req) - 1);
    memcpy(req, data, copyLen);
    req[copyLen] = '\0';

    // 1. GET /info (Connection and handshake check for Java CassetteFlow client - case-insensitive)
    if (ci_strncasecmp(req, "GET /info", 9) == 0) {
        if (Visualizer_IsDCTMode()) {
            Visualizer_GetFSKText(s_RxTextRaw, sizeof(s_RxTextRaw));
            if (strlen(s_RxTextRaw) > 0) {
                snprintf(s_InfoBuf, sizeof(s_InfoBuf), "DECODE %s", s_RxTextRaw);
                int l = strlen(s_InfoBuf);
                if (l > 0 && s_InfoBuf[l - 1] != '\n') {
                    s_InfoBuf[l] = '\n';
                    s_InfoBuf[l + 1] = '\0';
                }
            } else {
                snprintf(s_InfoBuf, sizeof(s_InfoBuf), "DECODE \n");
            }
            SendHTTPResponse(link_id, "text/plain", s_InfoBuf);
        } else {
            SendHTTPResponse(link_id, "text/plain", "PASS THROUGH\n");
        }
        return;
    }

    // 2. GET /raw and GET /rawdct (Continuous HTTP live streaming - connection remains open)
    if (ci_strncasecmp(req, "GET /raw", 8) == 0 || ci_strncasecmp(req, "GET /rawdct", 11) == 0) {
        s_RawClientLinkId = link_id;
        s_LastKeepaliveTick = HAL_GetTick();

        // Send initial streaming HTTP 200 Header (do not close connection)
        const char* streamHeader =
            "HTTP/1.1 200 OK\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: keep-alive\r\n\r\n";

        ESP8266_SendTCPData(link_id, (const uint8_t*)streamHeader, strlen(streamHeader));
        
        // Flush any stale items from queue so client starts with fresh lines
        if (s_RawLineQueue) {
            RawLineMsg dummy;
            while (xQueueReceive((QueueHandle_t)s_RawLineQueue, &dummy, 0) == pdTRUE);
        }
        return;
    }

    // 3. GET / (Root endpoint - handles ?mode=decode or ?mode=pass query param)
    if (strncmp(req, "GET / ", 6) == 0 || strncmp(req, "GET /?", 6) == 0 || ci_strncasecmp(req, "GET /index.html", 15) == 0) {
        if (ci_strstr(req, "mode=decode")) {
            Visualizer_SetDCTMode(true);
        } else if (ci_strstr(req, "mode=pass")) {
            Visualizer_SetDCTMode(false);
        }
        SendHTTPResponse(link_id, "text/html",
            "<!DOCTYPE html><html><head><title>STM32DiscVu Modem</title></head>"
            "<body style=\"font-family:sans-serif;background:#111;color:#eee;padding:20px;\">"
            "<h2>STM32DiscVu FSK Modem Online</h2>"
            "<p>API Endpoints: <a style=\"color:#0ff;\" href=\"/info\">/info</a> | "
            "<a style=\"color:#0ff;\" href=\"/raw\">/raw</a> | "
            "<a style=\"color:#0ff;\" href=\"/api/status\">/api/status</a> | "
            "<a style=\"color:#0ff;\" href=\"/dct\">/dct</a></p>"
            "</body></html>\n");
        return;
    }

    // 4. GET /dct (Enable DCT Mode)
    if (ci_strncasecmp(req, "GET /dct", 8) == 0) {
        Visualizer_SetDCTMode(true);
        SendHTTPResponse(link_id, "text/plain", "DCT Mode Enabled\n");
        return;
    }

    // 5. GET /mp3db, /tapedb, /create, /start, /stop, /play (CassetteFlow control stubs)
    if (ci_strncasecmp(req, "GET /mp3db", 10) == 0) {
        SendHTTPResponse(link_id, "text/plain", "MP3DB OK\n");
        return;
    }
    if (ci_strncasecmp(req, "GET /tapedb", 11) == 0) {
        SendHTTPResponse(link_id, "text/plain", "TAPEDB OK\n");
        return;
    }
    if (ci_strncasecmp(req, "GET /create", 11) == 0) {
        SendHTTPResponse(link_id, "text/plain", "CREATE OK\n");
        return;
    }
    if (ci_strncasecmp(req, "GET /start", 10) == 0) {
        SendHTTPResponse(link_id, "text/plain", "START OK\n");
        return;
    }
    if (ci_strncasecmp(req, "GET /stop", 9) == 0) {
        SendHTTPResponse(link_id, "text/plain", "STOP OK\n");
        return;
    }
    if (ci_strncasecmp(req, "GET /play", 9) == 0) {
        SendHTTPResponse(link_id, "text/plain", "PLAY OK\n");
        return;
    }

    // 6. GET /api/status (JSON Telemetry with FSK & System Stats)
    if (ci_strncasecmp(req, "GET /api/status", 15) == 0) {
        Visualizer_GetFSKText(s_RxTextRaw, sizeof(s_RxTextRaw));
        Visualizer_GetStatsText(s_StatsText, sizeof(s_StatsText));
        EscapeForJSON(s_RxTextRaw, s_RxTextJson, sizeof(s_RxTextJson));

        snprintf(s_JsonBody, sizeof(s_JsonBody),
            "{\"param_bat\":100,\"param_cpu\":%d,\"param_baud\":%d,\"param_measured_baud\":%.1f,"
            "\"param_speed_error\":%.2f,\"param_wf_pct\":0.0,\"param_test\":false,\"param_dct\":%s,"
            "\"param_tape_scale\":%s,\"param_spk\":false,\"param_ip\":\"%s\",\"param_version\":\"v0.7.0\",\"rx_text\":\"%s\",\"stats_text\":\"%s\"}",
            current_cpu_load,
            (int)g_BaudRate,
            Visualizer_GetMeasuredBaud(),
            Visualizer_GetSpeedError(),
            Visualizer_IsDCTMode() ? "true" : "false",
            Visualizer_IsProportionalMode() ? "true" : "false",
            ESP8266_GetStatusString(),
            s_RxTextJson,
            s_StatsText);

        SendHTTPResponse(link_id, "application/json", s_JsonBody);
        return;
    }

    // 7. POST /api/cmd (Commands such as reset, baud, dct_mode)
    if (ci_strncasecmp(req, "POST /api/cmd", 13) == 0) {
        if (ci_strstr(req, "\"reset\"")) {
            Visualizer_ResetFSK();
        }
        if (ci_strstr(req, "\"dct_mode\"")) {
            if (ci_strstr(req, "\"val\":1") || ci_strstr(req, "\"val\": 1") || ci_strstr(req, "true")) {
                Visualizer_SetDCTMode(true);
            } else {
                Visualizer_SetDCTMode(false);
            }
        }
        SendHTTPResponse(link_id, "text/plain", "OK\n");
        return;
    }

    // 8. POST /api/tx (Send TX text)
    if (ci_strncasecmp(req, "POST /api/tx", 12) == 0) {
        char* body = strstr(req, "\r\n\r\n");
        if (body) {
            body += 4;
            Visualizer_TransmitFSKText(body);
        }
        SendHTTPResponse(link_id, "text/plain", "OK\n");
        return;
    }

    // Default 404
    SendHTTPResponse(link_id, "text/plain", "404 Not Found\n");
}

void WiFiApp_Init(void) {
    LoadWiFiCredentialsFromSD();
    if (s_RawLineQueue == NULL) {
        s_RawLineQueue = (osMessageQId)xQueueCreate(16, sizeof(RawLineMsg));
    }
}

void StartWiFiTask(void const* argument) {
    printf("[WIFI_TASK] Started\r\n");

    // Initialize FreeRTOS live streaming queue
    if (s_RawLineQueue == NULL) {
        s_RawLineQueue = (osMessageQId)xQueueCreate(16, sizeof(RawLineMsg));
    }

    // Initialize ESP8266 Hardware Driver
    if (!ESP8266_Init(&huart6)) {
        printf("[WIFI_TASK] ESP8266 Hardware Init Failed!\r\n");
        snprintf(g_DisplayStatus, sizeof(g_DisplayStatus), "WiFi: Init Error");
        for (;;) {
            osDelay(2000);
        }
    }

    ESP8266_SetPacketCallback(WiFiApp_HandleIncomingPacket);

    for (;;) {
        // Step 1: Connect to Wi-Fi
        snprintf(g_DisplayStatus, sizeof(g_DisplayStatus), "WiFi: Connecting...");
        printf("[WIFI_TASK] Connecting to '%s'...\r\n", g_SSID);

        if (ESP8266_ConnectWiFi(g_SSID, g_Password, 25000)) {
            char ip[32];
            if (ESP8266_GetIP(ip, sizeof(ip))) {
                snprintf(g_DisplayStatus, sizeof(g_DisplayStatus), "WiFi: %s", ip);
            } else {
                snprintf(g_DisplayStatus, sizeof(g_DisplayStatus), "WiFi: Connected");
            }

            // Step 2: Start TCP Server on Port 80 for FSK API endpoints
            if (ESP8266_StartServer(80)) {
                printf("[WIFI_TASK] FSK HTTP API Server active at http://%s/\r\n", ip);
            }

            // Step 3: Run Server loop & monitor connection
            uint32_t lastHealthCheck = HAL_GetTick();
            bool isConnected = true;
            while (isConnected) {
                ESP8266_Process();

                // Live Streaming for /raw client (keeps connection open and streams decoded lines)
                if (s_RawClientLinkId >= 0) {
                    RawLineMsg msg;
                    if (s_RawLineQueue && xQueueReceive((QueueHandle_t)s_RawLineQueue, &msg, 0) == pdTRUE) {
                        char chunk[80];
                        int clen = snprintf(chunk, sizeof(chunk), "%s\n", msg.text);
                        if (!ESP8266_SendTCPData((uint8_t)s_RawClientLinkId, (const uint8_t*)chunk, clen)) {
                            // Client disconnected
                            ESP8266_CloseConnection((uint8_t)s_RawClientLinkId);
                            s_RawClientLinkId = -1;
                        } else {
                            s_LastKeepaliveTick = HAL_GetTick();
                        }
                    } else if (HAL_GetTick() - s_LastKeepaliveTick > 1000) {
                        // 1s idle -> Send NOCARRIER keepalive so client knows immediately when tape stops
                        s_LastKeepaliveTick = HAL_GetTick();
                        const char* keepalive = "### NOCARRIER ###\n";
                        if (!ESP8266_SendTCPData((uint8_t)s_RawClientLinkId, (const uint8_t*)keepalive, strlen(keepalive))) {
                            // Client disconnected
                            ESP8266_CloseConnection((uint8_t)s_RawClientLinkId);
                            s_RawClientLinkId = -1;
                        }
                    }
                }

                osDelay(10);

                if (HAL_GetTick() - lastHealthCheck > 15000) {
                    lastHealthCheck = HAL_GetTick();
                    if (!ESP8266_IsConnected()) {
                        osDelay(500);
                        if (!ESP8266_IsConnected()) {
                            isConnected = false;
                        }
                    }
                }
            }

            s_RawClientLinkId = -1;
            printf("[WIFI_TASK] Wi-Fi Disconnected!\r\n");
            snprintf(g_DisplayStatus, sizeof(g_DisplayStatus), "WiFi: Offline");
        } else {
            s_RawClientLinkId = -1;
            printf("[WIFI_TASK] Wi-Fi Connection failed. Retrying in 10 seconds...\r\n");
            snprintf(g_DisplayStatus, sizeof(g_DisplayStatus), "WiFi: Retry...");
            osDelay(10000);
        }
    }
}

const char* WiFiApp_GetDisplayStatus(void) {
    return g_DisplayStatus;
}

bool WiFiApp_IsConnected(void) {
    return ESP8266_GetStatus() == ESP8266_STATUS_GOT_IP || ESP8266_GetStatus() == ESP8266_STATUS_SERVER_RUNNING;
}

const char* WiFiApp_GetIP(void) {
    return ESP8266_GetStatusString();
}

const char* WiFiApp_GetSSID(void) {
    return g_SSID;
}
