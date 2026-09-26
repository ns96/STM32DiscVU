#ifndef ESP8266_H
#define ESP8266_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32f7xx_hal.h"

#define ESP8266_RX_BUFFER_SIZE  2048
#define ESP8266_LINE_BUF_SIZE   512

typedef enum {
    ESP8266_STATUS_UNINITIALIZED = 0,
    ESP8266_STATUS_INIT_FAILED,
    ESP8266_STATUS_DISCONNECTED,
    ESP8266_STATUS_CONNECTING,
    ESP8266_STATUS_CONNECTED,
    ESP8266_STATUS_GOT_IP,
    ESP8266_STATUS_SERVER_RUNNING
} ESP8266_Status;

// Function pointer for handling incoming TCP packet (+IPD)
typedef void (*ESP8266_PacketCallback_t)(uint8_t link_id, const uint8_t* data, uint16_t len);

// Core Driver API
bool ESP8266_Init(UART_HandleTypeDef* huart);
void ESP8266_RxCallback(uint8_t byte);
void ESP8266_SetPacketCallback(ESP8266_PacketCallback_t cb);

// Low-level AT commands
bool ESP8266_SendCommand(const char* cmd, const char* expected_resp, uint32_t timeout_ms);
bool ESP8266_SendCommandResponse(const char* cmd, char* out_resp, uint16_t out_max, uint32_t timeout_ms);
void ESP8266_ClearRxBuffer(void);

// Wi-Fi Management
bool ESP8266_Reset(void);
bool ESP8266_SetEcho(bool enable);
bool ESP8266_SetStationMode(void);
bool ESP8266_ConnectWiFi(const char* ssid, const char* pass, uint32_t timeout_ms);
bool ESP8266_DisconnectWiFi(void);
bool ESP8266_GetIP(char* ip_str, uint16_t max_len);
bool ESP8266_GetMAC(char* mac_str, uint16_t max_len);
bool ESP8266_GetAPSSID(char* ssid_str, uint16_t max_len);
bool ESP8266_IsConnected(void);

// TCP / Server Operations
bool ESP8266_EnableMultipleConnections(bool enable);
bool ESP8266_StartServer(uint16_t port);
bool ESP8266_CloseServer(void);
bool ESP8266_SendTCPData(uint8_t link_id, const uint8_t* data, uint16_t len);
bool ESP8266_SendTCPDataChunked(uint8_t link_id, const uint8_t* data, uint32_t len, uint16_t chunkSize);
bool ESP8266_CloseConnection(uint8_t link_id);

// Periodic polling / worker function (called from RTOS task)
void ESP8266_Process(void);

// Status Access
ESP8266_Status ESP8266_GetStatus(void);
const char* ESP8266_GetStatusString(void);

#endif // ESP8266_H
