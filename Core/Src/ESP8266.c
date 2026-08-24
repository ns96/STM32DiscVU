#include "ESP8266.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cmsis_os.h"

// Internal State
static UART_HandleTypeDef* g_hUart = NULL;
static volatile uint8_t g_RxBuffer[ESP8266_RX_BUFFER_SIZE];
static volatile uint16_t g_RxHead = 0;
static volatile uint16_t g_RxTail = 0;

static ESP8266_Status g_CurrentStatus = ESP8266_STATUS_UNINITIALIZED;
static char g_CurrentIP[32] = "0.0.0.0";
static char g_CurrentMAC[32] = "";
static char g_ConnectedSSID[64] = "";
static ESP8266_PacketCallback_t g_PacketCallback = NULL;

// Ring buffer helpers
static inline uint16_t RxAvailable(void) {
    uint16_t head = g_RxHead;
    uint16_t tail = g_RxTail;
    if (head >= tail) {
        return head - tail;
    } else {
        return (ESP8266_RX_BUFFER_SIZE - tail) + head;
    }
}

static bool RxReadByte(uint8_t* byte) {
    if (g_RxHead == g_RxTail) return false;
    *byte = g_RxBuffer[g_RxTail];
    g_RxTail = (g_RxTail + 1) % ESP8266_RX_BUFFER_SIZE;
    return true;
}

static uint8_t RxPeek(uint16_t offset) {
    uint16_t idx = (g_RxTail + offset) % ESP8266_RX_BUFFER_SIZE;
    return g_RxBuffer[idx];
}

void ESP8266_ClearRxBuffer(void) {
    g_RxTail = g_RxHead;
}

volatile uint32_t g_ESP_RxByteCount = 0;

// Low-level ISR callback
void ESP8266_RxCallback(uint8_t byte) {
    g_ESP_RxByteCount++;
    uint16_t nextHead = (g_RxHead + 1) % ESP8266_RX_BUFFER_SIZE;
    if (nextHead != g_RxTail) {
        g_RxBuffer[g_RxHead] = byte;
        g_RxHead = nextHead;
    }
}

void ESP8266_SetPacketCallback(ESP8266_PacketCallback_t cb) {
    g_PacketCallback = cb;
}

static void PollUARTRegister(void) {
    if (g_hUart && g_hUart->Instance) {
        uint32_t isr = g_hUart->Instance->ISR;
        if (isr & (USART_ISR_ORE | USART_ISR_NE | USART_ISR_FE | USART_ISR_PE)) {
            g_hUart->Instance->ICR = (USART_ICR_ORECF | USART_ICR_NCF | USART_ICR_FECF | USART_ICR_PECF);
        }
        if (isr & USART_ISR_RXNE) {
            uint8_t byte = (uint8_t)(g_hUart->Instance->RDR & 0xFF);
            ESP8266_RxCallback(byte);
        }
    }
}

static void TxByte(uint8_t b) {
    if (g_hUart && g_hUart->Instance) {
        uint32_t start = HAL_GetTick();
        while (!(g_hUart->Instance->ISR & USART_ISR_TXE) && (HAL_GetTick() - start < 100));
        g_hUart->Instance->TDR = b;
    }
}

static void TxString(const char* s) {
    if (!s) return;
    while (*s) {
        TxByte((uint8_t)*s++);
    }
}

// Low-level command send
bool ESP8266_SendCommandResponse(const char* cmd, char* out_resp, uint16_t out_max, uint32_t timeout_ms) {
    if (!g_hUart || !g_hUart->Instance) return false;

    ESP8266_ClearRxBuffer();
    g_hUart->Instance->ICR = 0xFFFFFFFF;
    g_hUart->Instance->CR1 |= USART_CR1_RXNEIE;
    
    // Transmit command using direct register TX
    TxString(cmd);
    TxString("\r\n");

    uint32_t start = HAL_GetTick();
    uint16_t respIdx = 0;
    if (out_resp && out_max > 0) out_resp[0] = '\0';

    while (HAL_GetTick() - start < timeout_ms) {
        PollUARTRegister();
        uint8_t b;
        while (RxReadByte(&b)) {
            if (out_resp && respIdx < (out_max - 1)) {
                out_resp[respIdx++] = (char)b;
                out_resp[respIdx] = '\0';
            }
        }

        if (out_resp) {
            if (strstr(out_resp, "OK") != NULL) return true;
            if (strstr(out_resp, "ready") != NULL) return true;
            if (strstr(out_resp, "ERROR") != NULL) return false;
            if (strstr(out_resp, "FAIL") != NULL) return false;
            if (strstr(out_resp, "ALREADY CONNECTED") != NULL) return true;
            if (strstr(out_resp, "WIFI GOT IP") != NULL) return true;
        }

        osDelay(2);
    }

    return false;
}

bool ESP8266_SendCommand(const char* cmd, const char* expected_resp, uint32_t timeout_ms) {
    char respBuf[ESP8266_LINE_BUF_SIZE];
    respBuf[0] = '\0';

    if (!g_hUart || !g_hUart->Instance) return false;

    ESP8266_ClearRxBuffer();
    g_hUart->Instance->ICR = 0xFFFFFFFF;
    g_hUart->Instance->CR1 |= USART_CR1_RXNEIE;
    
    TxString(cmd);
    TxString("\r\n");

    uint32_t start = HAL_GetTick();
    uint16_t respIdx = 0;

    while (HAL_GetTick() - start < timeout_ms) {
        PollUARTRegister();
        uint8_t b;
        while (RxReadByte(&b)) {
            if (respIdx < (sizeof(respBuf) - 1)) {
                respBuf[respIdx++] = (char)b;
                respBuf[respIdx] = '\0';
            }
        }

        if (expected_resp != NULL) {
            if (strstr(respBuf, expected_resp) != NULL) return true;
        } else {
            if (strstr(respBuf, "OK") != NULL) return true;
        }

        if (strstr(respBuf, "ERROR") != NULL) return false;
        if (strstr(respBuf, "FAIL") != NULL) return false;

        osDelay(2);
    }

    return false;
}

// Reset
bool ESP8266_Reset(void) {
    printf("[ESP8266] Sending AT+RST...\r\n");
    ESP8266_SendCommand("AT+RST", "ready", 3000);
    osDelay(500);
    return true;
}

// Set Echo
bool ESP8266_SetEcho(bool enable) {
    return ESP8266_SendCommand(enable ? "ATE1" : "ATE0", "OK", 1000);
}

// Set Station Mode
bool ESP8266_SetStationMode(void) {
    ESP8266_SendCommand("AT+CWMODE=1", "OK", 1000);
    ESP8266_SendCommand("AT+CWDHCP=1,1", "OK", 1000);
    ESP8266_SendCommand("AT+RFPOWER=50", "OK", 1000);
    return true;
}

// Multiple Connections
bool ESP8266_EnableMultipleConnections(bool enable) {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CIPMUX=%d", enable ? 1 : 0);
    return ESP8266_SendCommand(cmd, "OK", 1000);
}

// Connect to Wi-Fi AP
bool ESP8266_ConnectWiFi(const char* ssid, const char* pass, uint32_t timeout_ms) {
    char cmd[160];
    char resp[ESP8266_LINE_BUF_SIZE];
    
    g_CurrentStatus = ESP8266_STATUS_CONNECTING;
    printf("[ESP8266] Connecting to AP: %s...\r\n", ssid);
    
    for (int attempt = 1; attempt <= 3; attempt++) {
        ESP8266_SendCommand("AT+CWQAP", "OK", 500);
        osDelay(300);

        if (attempt == 1) {
            snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", ssid, pass);
        } else if (attempt == 2) {
            snprintf(cmd, sizeof(cmd), "AT+CWJAP_CUR=\"%s\",\"%s\"", ssid, pass);
        } else {
            // Attempt 3 with explicit BSSID if SA906
            if (strcmp(ssid, "SA906") == 0) {
                snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\",\"20:c0:47:c7:f2:10\"", ssid, pass);
            } else {
                snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", ssid, pass);
            }
        }

        bool ok = ESP8266_SendCommandResponse(cmd, resp, sizeof(resp), timeout_ms);

        if (ok || strstr(resp, "WIFI CONNECTED") != NULL || strstr(resp, "WIFI GOT IP") != NULL) {
            strncpy(g_ConnectedSSID, ssid, sizeof(g_ConnectedSSID));
            g_CurrentStatus = ESP8266_STATUS_CONNECTED;
            printf("[ESP8266] Wi-Fi Connected!\r\n");
            osDelay(1000);
            ESP8266_GetIP(g_CurrentIP, sizeof(g_CurrentIP));
            return true;
        }

        printf("[ESP8266] Attempt %d Failed: %s\r\n", attempt, resp);
        if (attempt < 3) {
            osDelay(2000);
        }
    }

    // Diagnostic scan if connection failed
    printf("[ESP8266] Scanning available 2.4GHz Wi-Fi networks (AT+CWLAP)...\r\n");
    char scanResp[1024] = {0};
    if (ESP8266_SendCommandResponse("AT+CWLAP", scanResp, sizeof(scanResp), 8000)) {
        printf("[ESP8266] Visible Networks:\r\n%s\r\n", scanResp);
    }

    g_CurrentStatus = ESP8266_STATUS_DISCONNECTED;
    return false;
}

// Disconnect Wi-Fi
bool ESP8266_DisconnectWiFi(void) {
    bool ok = ESP8266_SendCommand("AT+CWQAP", "OK", 2000);
    g_CurrentStatus = ESP8266_STATUS_DISCONNECTED;
    g_CurrentIP[0] = '\0';
    g_ConnectedSSID[0] = '\0';
    return ok;
}

// Get assigned IP address
bool ESP8266_GetIP(char* ip_str, uint16_t max_len) {
    char resp[ESP8266_LINE_BUF_SIZE];
    uint32_t start = HAL_GetTick();
    
    while (HAL_GetTick() - start < 8000) {
        if (ESP8266_SendCommandResponse("AT+CIFSR", resp, sizeof(resp), 1500)) {
            char* pSta = strstr(resp, "STAIP");
            if (pSta) {
                char* pStart = strchr(pSta, '\"');
                if (pStart) {
                    pStart++;
                } else {
                    pStart = strchr(pSta, ':');
                    if (pStart) pStart++;
                }
                while (*pStart == ' ' || *pStart == '\"') pStart++;
                
                char* pEnd = pStart;
                while (*pEnd && *pEnd != '\"' && *pEnd != '\r' && *pEnd != '\n' && *pEnd != ' ') {
                    pEnd++;
                }
                int len = pEnd - pStart;
                if (len > 0 && len < max_len) {
                    char tempIp[32] = {0};
                    strncpy(tempIp, pStart, len);
                    tempIp[len] = '\0';
                    if (strcmp(tempIp, "0.0.0.0") != 0 && strchr(tempIp, '.')) {
                        strncpy(ip_str, tempIp, max_len);
                        strncpy(g_CurrentIP, ip_str, sizeof(g_CurrentIP));
                        g_CurrentStatus = ESP8266_STATUS_GOT_IP;
                        printf("[ESP8266] Got IP: %s\r\n", g_CurrentIP);
                        return true;
                    }
                }
            }
        }
        osDelay(500);
    }
    return false;
}

// Get MAC
bool ESP8266_GetMAC(char* mac_str, uint16_t max_len) {
    char resp[ESP8266_LINE_BUF_SIZE];
    if (ESP8266_SendCommandResponse("AT+CIFSR", resp, sizeof(resp), 2000)) {
        char* pMac = strstr(resp, "+CIFSR:STAMAC,\"");
        if (pMac) {
            pMac += 15;
            char* pEnd = strchr(pMac, '\"');
            if (pEnd) {
                int len = pEnd - pMac;
                if (len < max_len) {
                    strncpy(mac_str, pMac, len);
                    mac_str[len] = '\0';
                    strncpy(g_CurrentMAC, mac_str, sizeof(g_CurrentMAC));
                    return true;
                }
            }
        }
    }
    return false;
}

// Check if currently associated
bool ESP8266_IsConnected(void) {
    char resp[ESP8266_LINE_BUF_SIZE];
    if (ESP8266_SendCommandResponse("AT+CIPSTATUS", resp, sizeof(resp), 1000)) {
        if (strstr(resp, "STATUS:2") || strstr(resp, "STATUS:3") || strstr(resp, "STATUS:4")) {
            return true;
        }
    }
    return false;
}

// Start TCP Server
bool ESP8266_StartServer(uint16_t port) {
    char cmd[32];
    ESP8266_EnableMultipleConnections(true);
    snprintf(cmd, sizeof(cmd), "AT+CIPSERVER=1,%d", port);
    if (ESP8266_SendCommand(cmd, "OK", 2000)) {
        printf("[ESP8266] TCP Server listening on port %d\r\n", port);
        g_CurrentStatus = ESP8266_STATUS_SERVER_RUNNING;
        return true;
    }
    return false;
}

// Close TCP Server
bool ESP8266_CloseServer(void) {
    return ESP8266_SendCommand("AT+CIPSERVER=0", "OK", 2000);
}

// Send TCP Data to specific link_id
bool ESP8266_SendTCPData(uint8_t link_id, const uint8_t* data, uint16_t len) {
    if (!g_hUart || !g_hUart->Instance || !data || len == 0) return false;

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d,%d", link_id, len);
    
    // Wait for '>' prompt
    if (ESP8266_SendCommand(cmd, ">", 2000)) {
        for (uint16_t i = 0; i < len; i++) {
            TxByte(data[i]);
        }
        
        // Wait for SEND OK
        static char resp[256];
        uint32_t start = HAL_GetTick();
        uint16_t idx = 0;
        resp[0] = '\0';

        while (HAL_GetTick() - start < 3000) {
            PollUARTRegister();
            uint8_t b;
            while (RxReadByte(&b)) {
                if (idx < sizeof(resp) - 1) {
                    resp[idx++] = (char)b;
                    resp[idx] = '\0';
                }
            }
            if (strstr(resp, "SEND OK") != NULL) return true;
            if (strstr(resp, "SEND FAIL") != NULL || strstr(resp, "ERROR") != NULL || strstr(resp, "link is not valid") != NULL) return false;
            osDelay(2);
        }
    }
    return false;
}

// Close connection
bool ESP8266_CloseConnection(uint8_t link_id) {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CIPCLOSE=%d", link_id);
    return ESP8266_SendCommand(cmd, "OK", 1000);
}

// Process incoming asynchronous packets (+IPD)
void ESP8266_Process(void) {
    PollUARTRegister();
    uint16_t avail = RxAvailable();
    if (avail < 9) return; // Minimum length for "+IPD,0,1:x"

    // Search for "+IPD,"
    for (uint16_t i = 0; i <= avail - 5; i++) {
        if (RxPeek(i) == '+' && RxPeek(i+1) == 'I' && RxPeek(i+2) == 'P' && RxPeek(i+3) == 'D' && RxPeek(i+4) == ',') {
            // Check if ':' delimiter is already present within header bounds
            bool colonFound = false;
            for (uint16_t c = i + 5; c < avail && c < i + 20; c++) {
                if (RxPeek(c) == ':') {
                    colonFound = true;
                    break;
                }
            }
            // If full header prefix hasn't arrived yet, wait for next cycle
            if (!colonFound) return;

            // Discard any bytes before +IPD
            for (uint16_t d = 0; d < i; d++) {
                uint8_t dummy;
                RxReadByte(&dummy);
            }
            
            // Read "+IPD,"
            for (int k = 0; k < 5; k++) {
                uint8_t dummy;
                RxReadByte(&dummy);
            }

            // Read link_id
            uint8_t b;
            if (!RxReadByte(&b)) return;
            uint8_t link_id = b - '0';

            // Next character must be ','
            if (!RxReadByte(&b) || b != ',') return;

            // Read length string until ':'
            char lenStr[8];
            int lIdx = 0;
            while (RxReadByte(&b)) {
                if (b == ':') break;
                if (lIdx < sizeof(lenStr) - 1) lenStr[lIdx++] = (char)b;
            }
            lenStr[lIdx] = '\0';
            uint16_t pktLen = (uint16_t)atoi(lenStr);

            if (pktLen > 0 && pktLen <= 1024) {
                // Static buffer to completely eliminate FreeRTOS task stack overflow
                static uint8_t s_packetData[1024];
                uint16_t readCount = 0;
                uint32_t tStart = HAL_GetTick();

                while (readCount < pktLen && (HAL_GetTick() - tStart < 2000)) {
                    PollUARTRegister();
                    while (RxReadByte(&b) && readCount < pktLen) {
                        s_packetData[readCount++] = b;
                    }
                    if (readCount < pktLen) osDelay(2);
                }

                if (readCount == pktLen && g_PacketCallback) {
                    g_PacketCallback(link_id, s_packetData, pktLen);
                }
            }
            return;
        }
    }
}

static void SetUARTBaudAndSwap(uint32_t baud, bool swap) {
    if (!g_hUart) return;
    HAL_UART_DeInit(g_hUart);
    g_hUart->Init.BaudRate = baud;
    g_hUart->Init.Mode = UART_MODE_TX_RX;
    if (swap) {
        g_hUart->AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_SWAP_INIT;
        g_hUart->AdvancedInit.Swap = UART_ADVFEATURE_SWAP_ENABLE;
    } else {
        g_hUart->AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_SWAP_INIT;
        g_hUart->AdvancedInit.Swap = UART_ADVFEATURE_SWAP_DISABLE;
    }
    HAL_UART_Init(g_hUart);
    if (g_hUart->Instance) {
        g_hUart->Instance->ICR = 0xFFFFFFFF;
        g_hUart->Instance->CR1 |= USART_CR1_RXNEIE;
    }
    osDelay(50);
}

// Master Initialization
bool ESP8266_Init(UART_HandleTypeDef* huart) {
    g_hUart = huart;
    g_RxHead = 0;
    g_RxTail = 0;
    g_CurrentStatus = ESP8266_STATUS_UNINITIALIZED;

    printf("[ESP8266] Initializing ESP8266 Module on USART6...\r\n");

    // Probing pin orientations (Standard vs Swapped) and baud rates (115200, 9600, 57600, 74880)
    uint32_t testBauds[] = {115200, 9600, 57600, 74880};
    bool syncOk = false;
    uint32_t activeBaud = 115200;
    bool activeSwap = false;

    for (int swap = 0; swap < 2 && !syncOk; swap++) {
        bool isSwapped = (swap == 1);
        printf("[ESP8266] Testing %s pin configuration...\r\n", isSwapped ? "SWAPPED (TX=PC7, RX=PC6)" : "STANDARD (TX=PC6, RX=PC7)");

        for (int b = 0; b < 4 && !syncOk; b++) {
            uint32_t currentTestBaud = testBauds[b];
            printf("[ESP8266] Probing AT at %lu baud (%s) [RxTotal: %lu]...\r\n", currentTestBaud, isSwapped ? "swapped" : "std", g_ESP_RxByteCount);
            SetUARTBaudAndSwap(currentTestBaud, isSwapped);

            for (int retry = 0; retry < 5; retry++) {
                char resp[128] = {0};
                if (ESP8266_SendCommandResponse("AT", resp, sizeof(resp), 800)) {
                    syncOk = true;
                    activeBaud = currentTestBaud;
                    activeSwap = isSwapped;
                    printf("[ESP8266] Sync OK at %lu baud (%s)! Resp: %s\r\n", currentTestBaud, isSwapped ? "swapped" : "std", resp);
                    break;
                } else if (strstr(resp, "busy") != NULL) {
                    printf("[ESP8266] Module reported 'busy' at %lu baud! Waiting for it to settle...\r\n", currentTestBaud);
                    osDelay(1000);
                    ESP8266_ClearRxBuffer();
                    if (ESP8266_SendCommandResponse("AT", resp, sizeof(resp), 1500)) {
                        syncOk = true;
                        activeBaud = currentTestBaud;
                        activeSwap = isSwapped;
                        printf("[ESP8266] Sync OK at %lu baud (%s) after wait! Resp: %s\r\n", currentTestBaud, isSwapped ? "swapped" : "std", resp);
                        break;
                    }
                } else if (strlen(resp) > 0) {
                    printf("[ESP8266] Rx at %lu baud: '%s'\r\n", currentTestBaud, resp);
                }
                osDelay(200);
            }
        }
    }

    if (!syncOk) {
        printf("[ESP8266] ERROR: Module not responding! Check jumper headers on pins 0 & 1 (RB-Mfk-14).\r\n");
        g_CurrentStatus = ESP8266_STATUS_INIT_FAILED;
        return false;
    }

    if (activeBaud != 115200) {
        printf("[ESP8266] Switching module baud to 115200...\r\n");
        ESP8266_SendCommand("AT+UART_CUR=115200,8,1,0,0", "OK", 1000);
        SetUARTBaudAndSwap(115200, activeSwap);
        osDelay(100);
    }

    printf("[ESP8266] AT Communication OK\r\n");
    char gmr[256] = {0};
    if (ESP8266_SendCommandResponse("AT+GMR", gmr, sizeof(gmr), 1000)) {
        printf("[ESP8266 FW]\r\n%s\r\n", gmr);
    }

    ESP8266_SetEcho(false);
    ESP8266_SetStationMode();
    ESP8266_EnableMultipleConnections(true);
    
    g_CurrentStatus = ESP8266_STATUS_DISCONNECTED;
    return true;
}

ESP8266_Status ESP8266_GetStatus(void) {
    return g_CurrentStatus;
}

const char* ESP8266_GetStatusString(void) {
    switch (g_CurrentStatus) {
        case ESP8266_STATUS_UNINITIALIZED: return "Uninitialized";
        case ESP8266_STATUS_INIT_FAILED:   return "Init Failed";
        case ESP8266_STATUS_DISCONNECTED:  return "Offline";
        case ESP8266_STATUS_CONNECTING:    return "Connecting...";
        case ESP8266_STATUS_CONNECTED:     return "Connected";
        case ESP8266_STATUS_GOT_IP:        return g_CurrentIP;
        case ESP8266_STATUS_SERVER_RUNNING:return g_CurrentIP;
        default:                           return "Unknown";
    }
}
