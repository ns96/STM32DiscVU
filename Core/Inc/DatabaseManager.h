#ifndef DATABASE_MANAGER_H
#define DATABASE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

// Structure to hold audio file information
typedef struct {
    char hash[12];         // 10 chars + padding
    int duration;
    int bitrate;
    char devicePath[128];
    char sourcePath[128];
} AudioInfo;

// Fixed size index entry for binary search
typedef struct __attribute__((packed)) {
    char hash[11];      // 10 chars + null
    uint8_t padding;    // align to 12
    uint32_t offset;    // 4 bytes -> 16 bytes TOTAL
} AudioIndexEntry;

// API
bool DBM_Init(const char* dbPath, uint32_t destRamAddr);
bool DBM_GetAudioInfo(const char* hash, AudioInfo *info);
uint32_t DBM_GetEntryCount(void);

#endif // DATABASE_MANAGER_H
