#ifndef WIFI_APP_H
#define WIFI_APP_H

#include <stdint.h>
#include <stdbool.h>

// Default Wi-Fi Credentials (overridden if WIFI.TXT is present on SD card)
#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID "SA906"
#endif

#ifndef DEFAULT_WIFI_PASS
#define DEFAULT_WIFI_PASS "458C60F416"
#endif

// Wi-Fi Application API
void WiFiApp_Init(void);
void StartWiFiTask(void const* argument);
void WiFiApp_PushRawLine(const char* line);

// UI & Telemetry Status Queries
const char* WiFiApp_GetDisplayStatus(void);
bool WiFiApp_IsConnected(void);
const char* WiFiApp_GetIP(void);
const char* WiFiApp_GetSSID(void);

#endif // WIFI_APP_H
