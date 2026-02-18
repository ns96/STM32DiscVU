#include "DatabaseManager.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "fatfs.h"
#include "cmsis_os.h"

// Global state
static char _dbPath[64];
static uint32_t _destRamAddr = 0;
static AudioIndexEntry* _indexData = NULL;
static uint32_t _indexCount = 0;

// Logging helper (uses UART1 from main)
extern UART_HandleTypeDef huart1;
static void db_log(const char* msg) {
    HAL_UART_Transmit(&huart1, (uint8_t*)"[DBM] ", 6, 100);
    HAL_UART_Transmit(&huart1, (uint8_t*)msg, strlen(msg), 1000);
    HAL_UART_Transmit(&huart1, (uint8_t*)"\r\n", 2, 100);
}

static int compareIndexEntries(const void* a, const void* b) {
    const AudioIndexEntry* entryA = (const AudioIndexEntry*)a;
    const AudioIndexEntry* entryB = (const AudioIndexEntry*)b;
    return strncmp(entryA->hash, entryB->hash, 10);
}

static void listRootFiles(void) {
    DIR dir;
    FILINFO fno;
    FRESULT res;
    int count = 0;

    db_log("Listing first 10 files in root:");
    res = f_opendir(&dir, "/");
    if (res == FR_OK) {
        for (;;) {
            res = f_readdir(&dir, &fno);
            if (res != FR_OK || fno.fname[0] == 0 || count >= 10) break;
            
            char fileLog[320];
            snprintf(fileLog, sizeof(fileLog), " - %s (%lu bytes)%s", 
                     fno.fname, (unsigned long)fno.fsize, (fno.fattrib & AM_DIR) ? " [DIR]" : "");
            db_log(fileLog);
            count++;
        }
        f_closedir(&dir);
    } else {
        char errBuf[64];
        snprintf(errBuf, sizeof(errBuf), "Failed to open root dir: %d", res);
        db_log(errBuf);
    }
}



static AudioInfo parseLine(char* line) {
    AudioInfo info;
    memset(&info, 0, sizeof(AudioInfo));
    
    char* token;
    char* rest = line;
    
    // Hash
    token = strtok_r(rest, "\t", &rest);
    if (token) strncpy(info.hash, token, 10);
    
    // Duration
    token = strtok_r(NULL, "\t", &rest);
    if (token) info.duration = atoi(token);
    
    // Bitrate
    token = strtok_r(NULL, "\t", &rest);
    if (token) info.bitrate = atoi(token);
    
    // Device Path
    token = strtok_r(NULL, "\t", &rest);
    if (token) strncpy(info.devicePath, token, sizeof(info.devicePath)-1);
    
    // Source Path (rest of the line)
    if (rest) strncpy(info.sourcePath, rest, sizeof(info.sourcePath)-1);
    
    return info;
}

static bool buildIndex(void) {
    FIL dbFile;
    FRESULT res = f_open(&dbFile, _dbPath, FA_READ);
    if (res != FR_OK) {
        char errBuf[64];
        snprintf(errBuf, sizeof(errBuf), "Scan failed: Open error %d", res);
        db_log(errBuf);
        return false;
    }

    AudioIndexEntry* entries = (AudioIndexEntry*)_destRamAddr;
    const uint32_t maxEntries = 50000; // Increased from 20k to handle 27k+ entries
    uint32_t count = 0;
    
    UINT bytesRead;
    uint32_t currentFileOffset = 0;
    uint32_t lineStartOffset = 0;
    char hashAccum[12];
    int hashIdx = 0;
    bool inHash = true;
    
    f_lseek(&dbFile, 0);

    static char readBuf[8192]; // Use 8KB buffer for internal RAM
    
    db_log("Scanning database...");
    while (f_read(&dbFile, readBuf, sizeof(readBuf), &bytesRead) == FR_OK && bytesRead > 0) {
        for (UINT i = 0; i < bytesRead; i++) {
            char c = readBuf[i];
            if (c == '\n') {
                if (hashIdx >= 10 && count < maxEntries) {
                    AudioIndexEntry* entry = &entries[count];
                    memcpy(entry->hash, hashAccum, 10);
                    entry->hash[10] = '\0';
                    entry->offset = lineStartOffset;
                    count++;
                }
                hashIdx = 0;
                inHash = true;
                lineStartOffset = currentFileOffset + i + 1;
            } else if (c == '\r' || c == '\t' || c == ' ') {
                inHash = false;
            } else if (inHash && hashIdx < 10) {
                hashAccum[hashIdx++] = c;
            }
        }
        currentFileOffset += bytesRead;
        osDelay(1); 
    }
    f_close(&dbFile);
    db_log("Scan complete.");

    // Print first 10 entries diagnostic
    db_log("First 10 Database Entries (Diagnostic):");
    uint32_t diagnosticCount = (count < 10) ? count : 10;
    FIL dbFileDiag;
    if (f_open(&dbFileDiag, _dbPath, FA_READ) == FR_OK) {
        for (uint32_t i = 0; i < diagnosticCount; i++) {
            if (f_lseek(&dbFileDiag, entries[i].offset) == FR_OK) {
                char lineBuf[256];
                if (f_gets(lineBuf, sizeof(lineBuf), &dbFileDiag)) {
                     // Clean line endings
                     char* end = strpbrk(lineBuf, "\r\n");
                     if (end) *end = '\0';
                     
                     char diagMsg[384];
                     snprintf(diagMsg, sizeof(diagMsg), " [#%lu] Hash:%s Path: %s", 
                               (unsigned long)i, entries[i].hash, lineBuf);
                     db_log(diagMsg);
                }
            }
        }
        f_close(&dbFileDiag);
    }

    if (count == 0) {
        db_log("No records found!");
        return false;
    }

    char logBuf[64];
    snprintf(logBuf, sizeof(logBuf), "Sorting %lu entries...", (unsigned long)count);
    db_log(logBuf);

    qsort(entries, count, sizeof(AudioIndexEntry), compareIndexEntries);

    _indexData = entries;
    _indexCount = count;

    db_log("Index built in SDRAM successfully.");
    return true;
}

bool DBM_Init(const char* dbPath, uint32_t destRamAddr) {
    strncpy(_dbPath, dbPath, sizeof(_dbPath)-1);
    _destRamAddr = destRamAddr;

    db_log("Initializing database...");

    // 1. Try to load Binary Index (.idx) first
    char idxPath[128];
    strncpy(idxPath, dbPath, sizeof(idxPath)-1);
    char* dot = strrchr(idxPath, '.');
    if (dot) strcpy(dot, ".idx");
    else strcat(idxPath, ".idx");

    FIL fidx;
    if (f_open(&fidx, idxPath, FA_READ) == FR_OK) {
        uint32_t fsize = f_size(&fidx);
        if (fsize > 0 && (fsize % sizeof(AudioIndexEntry)) == 0) {
            db_log("Loading pre-sorted binary index...");
            UINT br;
            if (f_read(&fidx, (void*)_destRamAddr, fsize, &br) == FR_OK && br == fsize) {
                _indexCount = br / sizeof(AudioIndexEntry);
                _indexData = (AudioIndexEntry*)_destRamAddr;
                f_close(&fidx);
                db_log("Binary Index loaded successfully.");
                return true;
            }
        }
        f_close(&fidx);
    }

    // 2. Fallback: Text Scan (Original Logic)
    FIL f;
    FRESULT res = f_open(&f, _dbPath, FA_READ);
    if (res != FR_OK) {
        char errBuf[128];
        snprintf(errBuf, sizeof(errBuf), "Database file error: %d (Path: %s)", res, _dbPath);
        db_log(errBuf);
        return false;
    }
    f_close(&f);

    listRootFiles();

    return buildIndex();
}

bool DBM_GetAudioInfo(const char* hash, AudioInfo *info) {
    if (!_indexData || _indexCount == 0 || !hash || !info) return false;

    AudioIndexEntry target;
    memset(target.hash, 0, 11);
    memcpy(target.hash, hash, 10);

    AudioIndexEntry* result = (AudioIndexEntry*)bsearch(&target, _indexData, _indexCount, sizeof(AudioIndexEntry), compareIndexEntries);

    if (result) {
        FIL dbFile;
        if (f_open(&dbFile, _dbPath, FA_READ) != FR_OK) return false;

        if (f_lseek(&dbFile, result->offset) == FR_OK) {
            char lineBuf[512];
            if (f_gets(lineBuf, sizeof(lineBuf), &dbFile)) {
                size_t len = strlen(lineBuf);
                while (len > 0 && (lineBuf[len-1] == '\r' || lineBuf[len-1] == '\n')) {
                    lineBuf[len-1] = '\0';
                    len--;
                }
                *info = parseLine(lineBuf);
                f_close(&dbFile);
                if (strncmp(info->hash, hash, 10) == 0) return true;
                return false;
            }
        }
        f_close(&dbFile);
    }
    return false;
}

uint32_t DBM_GetEntryCount(void) {
    return _indexCount;
}
