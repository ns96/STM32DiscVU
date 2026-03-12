#include "VisualizerApp.h"
#include "SimpleFFT.h"
#include "FSKModem.h"
#include "DatabaseManager.h"
#include "heat565.h"
#include "stm32746g_discovery_lcd.h"
#include "stm32746g_discovery_ts.h"
#include "stm32746g_discovery_audio.h"
#include "stm32746g_discovery_sd.h"
#include "cmsis_os.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "fatfs.h" // Required for DCT File Mapping

// --- DCT Mapping Helper State (RAM Based) ---
typedef struct {
    int totalTime;
    char hash[12]; // 10 chars + null + padding
    char fullLine[32]; // Original record (e.g. DCT2A_1_c1176104dd_320_320)
} SideMapEntry;

// SDRAM Layout:
// FB: 0xC0000000 (Layer 1)
// FB: 0xC0100000 (Layer 2)
// WF: 0xC0200000 (Waterfall - 384KB)
// ... Gap ...
// Side A Map: 0xC0300000 (Allocated 1MB)
// Side B Map: 0xC0400000 (Allocated 1MB)
// Audio DB:   0xC0500000 (Allocated remaining)

#define SIDE_MAP_A_ADDR  ((SideMapEntry*)0xC0300000)
#define SIDE_MAP_B_ADDR  ((SideMapEntry*)0xC0400000)
#define MAX_SIDE_MAP_ENTRIES 20000 // Increased from 10k to 20k (320KB usage per side)

static int g_SideACount = 0;
static int g_SideBCount = 0;
static int g_CachedTotalTime = -1;
static char g_CachedHash[16];

// Helper to load a side file into SDRAM (with Binary Cache)
static int LoadSideMap(char side, SideMapEntry* mapAddr) {
    char pathBin[32];
    snprintf(pathBin, sizeof(pathBin), "/side%c.bin", side);
    
    FIL file;
    // 1. Try Binary Map first (Fast)
    if (f_open(&file, pathBin, FA_READ) == FR_OK) {
        uint32_t fsize = f_size(&file);
        if (fsize > 0 && (fsize % sizeof(SideMapEntry)) == 0) {
            printf("[DCT] Loading Binary Side %c map...\r\n", side);
            UINT br;
            if (f_read(&file, mapAddr, fsize, &br) == FR_OK && br == fsize) {
                f_close(&file);
                return br / sizeof(SideMapEntry);
            }
        }
        f_close(&file);
    }

    // 2. Fallback to Text Map (Original Logic)
    char pathTxt[32];
    snprintf(pathTxt, sizeof(pathTxt), "/side%c.txt", side);
    
    // Fallback: Try lowercase if uppercase fails
    if (f_open(&file, pathTxt, FA_READ) != FR_OK) {
        char sideLower = (side >= 'A' && side <= 'Z') ? (side + 32) : side;
        snprintf(pathTxt, sizeof(pathTxt), "/side%c.txt", sideLower);
        if (f_open(&file, pathTxt, FA_READ) != FR_OK) {
            printf("[DCT] Failed to open %s\r\n", pathTxt);
            return 0;
        }
    }
    
    printf("[DCT] Parsing %s into SDRAM...\r\n", pathTxt);
    
    char lineBuf[128];
    int count = 0;
    while (f_gets(lineBuf, sizeof(lineBuf), &file) && count < MAX_SIDE_MAP_ENTRIES) {
        if (strlen(lineBuf) < 20) continue;

        // Manual Tokenize
        char* p = lineBuf;
        int underscores = 0;
        char* hashStart = NULL;
        char* totalStart = NULL;
        
        while (*p) {
            if (*p == '_') {
                underscores++;
                if (underscores == 2) hashStart = p + 1;
                else if (underscores == 4) totalStart = p + 1;
            }
            p++;
        }
        
        if (hashStart && totalStart) {
            char rawLine[32];
            strncpy(rawLine, lineBuf, sizeof(rawLine));
            rawLine[sizeof(rawLine)-1] = 0;
            // Clean newline for rawLine
            char* nl = strpbrk(rawLine, "\r\n");
            if (nl) *nl = 0;

            char* hEnd = hashStart; while (*hEnd != '_' && *hEnd != 0) hEnd++; *hEnd = 0;
            char* tEnd = totalStart; while (*tEnd >= '0' && *tEnd <= '9') tEnd++; *tEnd = 0;
            
            if (strlen(hashStart) <= 10) {
                mapAddr[count].totalTime = atoi(totalStart);
                strncpy(mapAddr[count].hash, hashStart, 11);
                strncpy(mapAddr[count].fullLine, rawLine, 32);
                count++;
            }
        }
    }
    f_close(&file);
    printf("[DCT] Parsed %d entries.\r\n", count);
    
    return count;
}

// Helper to find the "Real" Audio ID and Full Line from SDRAM
static bool GetMappedRecord(char side, int totalTime, char* outHash, char* outFullLine) {
    // 1. Check Cache
    if (totalTime == g_CachedTotalTime && g_CachedHash[0] != 0) {
        strcpy(outHash, g_CachedHash);
        // Cache fullLine as well? For now we just re-search if needed, but it's already in the struct.
    }

    SideMapEntry* map = (side == 'A' || side == 'a') ? SIDE_MAP_A_ADDR : SIDE_MAP_B_ADDR;
    int count = (side == 'A' || side == 'a') ? g_SideACount : g_SideBCount;
    
    if (count == 0) return false;

    int low = 0, high = count - 1;
    while (low <= high) {
        int mid = low + (high - low) / 2;
        if (map[mid].totalTime == totalTime) {
            strcpy(outHash, map[mid].hash);
            if (outFullLine) strcpy(outFullLine, map[mid].fullLine);
            // Update Cache
            g_CachedTotalTime = totalTime;
            strcpy(g_CachedHash, outHash);
            return true;
        }
        if (map[mid].totalTime < totalTime) low = mid + 1;
        else high = mid - 1;
    }
    
    return false;
}

// --- FSK Debug & Config ---
static float g_BaudRates[] = {300.0f, 600.0f, 1200.0f};
static int g_BaudIdx = 2; // Default to 1200
static float g_BaudRate = 1200.0f;
static uint32_t g_LastDebugTime = 0;
float g_MaxSignalLevel = 0.0f;

// --- FSK Audio Output FIFO ---
#define FSK_FIFO_SIZE 131072
// Map the colossal 262KB FIFO to unused external SDRAM at 0xC0300000 to save internal SRAM
#define g_FSK_FIFO ((int16_t*)0xC0300000)
static volatile uint32_t g_FSK_FIFO_ReadIdx = 0;
static volatile uint32_t g_FSK_FIFO_WriteIdx = 0;

void FSK_FIFO_Push(int16_t sample) {
    uint32_t next = (g_FSK_FIFO_WriteIdx + 1) % FSK_FIFO_SIZE;
    if (next != g_FSK_FIFO_ReadIdx) {
        g_FSK_FIFO[g_FSK_FIFO_WriteIdx] = sample;
        g_FSK_FIFO_WriteIdx = next;
    }
}

int16_t FSK_FIFO_Pop(void) {
    if (g_FSK_FIFO_ReadIdx == g_FSK_FIFO_WriteIdx) return 0;
    int16_t sample = g_FSK_FIFO[g_FSK_FIFO_ReadIdx];
    g_FSK_FIFO_ReadIdx = (g_FSK_FIFO_ReadIdx + 1) % FSK_FIFO_SIZE;
    return sample;
}

void FSK_FIFO_Reset(void) {
    g_FSK_FIFO_ReadIdx = 0;
    g_FSK_FIFO_WriteIdx = 0;
}

void addFSKChar(char c);
void addFSKDisplayChar(char c);
void addFSKDisplayString(const char* s);

// --- access to the BSP LTDC handle for double-buffering hack ---
extern LTDC_HandleTypeDef hLtdcHandler;
extern DMA2D_HandleTypeDef hdma2d;

// --- DMA2D Helper Prototypes ---
static void FillRectDMA2D(uint32_t* fb, int x, int y, int w, int h, uint32_t color);
static void CopyBlockDMA2D(uint32_t* src, uint32_t* dst, int width, int height);

// --- Global State ---
bool g_ShowSpectrum = true;
bool g_ShowVUMeter = true;
bool g_ShowWaterfall = false;
bool g_SimulationMode = true;
bool g_InputLineIn = false;
bool g_EnablePeakHold = true;
bool g_ShowFSK = false;
bool g_ShowFSKEncode = false;
char g_EncodeSide = 'A';
int g_EncodeDuration = 45;
volatile bool g_IsEncoding = false;
int g_EncodeSeconds = 0;
uint32_t g_LastEncodeTick = 0;
FSK_Modem g_TxModem;
int g_SpectrumMode = 0;
static const int splitX = 240;
static void UpdateFSKEncode(void);

// --- Aesthetics Settings ---
bool g_EnableAesthetics = true;

// EMA filter state for each of the 62 ISO bands. Max bands is 62.
static float g_BarLevels[62] = {0.0f};

// A-Weighting curve approximations scaled for the 62 bands.
// Attenuates sub-bass (bands 0-10), neutral lower-mids, boosts upper-mids (bands 30-50), attenuates extreme highs.
static const float g_AWeightingCurve[62] = {
    0.10f, 0.12f, 0.15f, 0.18f, 0.22f, 0.26f, 0.31f, 0.36f, 0.42f, 0.49f, // 1-10 (Sub-bass: heavy cut)
    0.56f, 0.64f, 0.72f, 0.81f, 0.90f, 0.99f, 1.09f, 1.19f, 1.29f, 1.39f, // 11-20 (Bass: moderate cut to neutral)
    1.49f, 1.58f, 1.67f, 1.76f, 1.84f, 1.91f, 1.98f, 2.05f, 2.11f, 2.16f, // 21-30 (Low-mids to mids: smooth boost)
    2.21f, 2.25f, 2.28f, 2.30f, 2.32f, 2.34f, 2.35f, 2.36f, 2.36f, 2.35f, // 31-40 (Upper-mids: peak sensitivity)
    2.34f, 2.32f, 2.29f, 2.25f, 2.20f, 2.14f, 2.08f, 2.00f, 1.92f, 1.83f, // 41-50 (Highs: gentle roll-off)
    1.73f, 1.62f, 1.50f, 1.38f, 1.25f, 1.11f, 0.97f, 0.82f, 0.66f, 0.50f, // 51-60 (Upper highs: steeper roll-off)
    0.33f, 0.15f // 61-62 (Air: cut)
};

// --- Spectrum Colors (Customizable) ---
#define COLOR_SPEC_L  LCD_COLOR_YELLOW
#define COLOR_SPEC_R  LCD_COLOR_RED

// --- UI Layout ---
#define UI_HEADER_H      30
#define UI_VIZ_TOP       30
#define UI_VIZ_BOTTOM    230
#define UI_FOOTER_Y      230

// --- Global System Tuning ---
// Baud Correction: If the measured baud rate is off, adjust this factor.
// Example: If measured baud is 5% high (e.g. 1260 instead of 1200), set this to 1.05f.
// Example: If measured baud is 5% low (e.g. 1140 instead of 1200), set this to 0.95f.
float g_BaudCorrectionFactor = 1.01f; 

// --- Optimization / Sensitivity Settings ---
float g_SpectrumGain = 6.0f;
float g_WaterfallGain = 6.0f; 
float g_MicGain = 24.0f;      // Standard gain for microphones
float g_LineInGain = 0.75f;   // lower default gain for line-in
float g_AudioGain = 24.0f;    // Active gain used in processing
float g_VUGain = 2.0f;
float g_PeakDecay = 10.0f;     // Pixels per frame to fall
float g_VUDecay = 0.80f;      // Exponential fall per frame (0.80-0.90 for fast response)
float g_FSKGain = 10.0f;       // Boost for FSK processing (5.0-10.0 typical)
int g_PeakHoldFrames = 5;    // Frames to hold peak before falling
static float vu_gain_vals[] = {0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
static const char* vu_gain_texts[] = {"x0.5", "x1", "x2", "x4", "x8"};
static int vu_gain_idx = 2; // Default to 2.0
static bool g_GainAlwaysActive = true;

// --- Buffers & Double Buffering State ---
static uint32_t fb_addresses[2] = { LCD_FB_START_ADDRESS, LCD_FB_START_ADDRESS + LCD_FB_OFFSET };
#define WFALL_FB_ADDRESS (LCD_FB_START_ADDRESS + 2 * LCD_FB_OFFSET)
#define WFALL_WIDTH  480
#define WFALL_HEIGHT 200 // UI_FOOTER_Y - UI_HEADER_H
static int g_WfallHead = 0; // Circular buffer head pointer
static int front_buffer_idx = 0;

// --- CPU Load State ---
extern volatile uint32_t g_IdleTicks;
static uint32_t cpu_baseline = 0;
static uint32_t last_idle_val = 0;
static uint32_t last_cpu_tick = 0;
static int current_cpu_load = 0;

// --- Audio / Viz State ---
static float vReal[FFT_SIZE];
static float vImag[FFT_SIZE];
static float vRealFFT[FFT_SIZE];
static float vImagFFT[FFT_SIZE];

// Separate buffers for Stereo Split mode
static float vRealL[FFT_SIZE];
static float vRealR[FFT_SIZE];
static float vRealFFTL[FFT_SIZE];
static float vRealFFTR[FFT_SIZE];

static uint16_t fftIdx = 0;
static float peakL = 0.0f;
static float peakR = 0.0f;
static uint8_t fft_mag_sim[64]; 

// --- Peak Hold State ---
static float g_SpectrumPeaks[128];
static uint8_t g_PeakHoldCount[128];
static float g_VUPeaks[2]; // 0=L, 1=R
static uint8_t g_VUPeakHoldCount[2];

// --- Database & Metadata ---
static AudioInfo g_CurrentTrack;
static bool g_HasMetadata = false;

// --- ISO Standard Frequency Mappings (256 FFT Bins -> Octave Bands) ---

// 31-Band (1/3 Octave) for Stereo Split (31 Left + 31 Right = 62)
static const uint8_t g_ISO31_Map[31][2] = {
    {1,2},   {2,3},   {3,4},   {4,5},   {5,6},   {6,7},   {7,8},   {8,10},
    {10,12}, {12,15}, {15,18}, {18,22}, {22,27}, {27,33}, {33,40}, {40,49}, 
    {49,60}, {60,73}, {73,89}, {89,109},{109,133},{133,163},{163,190},{190,210},
    {210,225},{225,235},{235,245},{245,250},{250,253},{253,254},{254,255}
};

// 62-Band (1/6 Octave) for Mono Mode (Full Display)
static const uint8_t g_ISO62_Map[62][2] = {
    {1,2},   {2,3},   {3,4},   {4,5},   {5,6},   {6,7},   {7,8},   {8,9},   {9,10},  {10,11},
    {11,12}, {12,13}, {13,14}, {14,15}, {15,17}, {17,19}, {19,21}, {21,23}, {23,26}, {26,29},
    {29,32}, {32,35}, {35,39}, {39,43}, {43,47}, {47,52}, {52,57}, {57,63}, {63,69}, {69,76},
    {76,84}, {84,92}, {92,101},{101,111},{111,122},{122,134},{134,147},{147,161},{161,176},{176,193},
    {193,200},{200,205},{205,210},{210,215},{215,220},{220,225},{225,230},{230,234},{234,238},{238,242},
    {242,245},{245,248},{248,251},{251,253},{253,254},{254,255},{255,255},{255,255},{255,255},{255,255},
    {255,255},{255,255}
};

// Helper to convert magnitude to Log-dB Bar Height
static int getLogBarHeight(float* fftData, int barIdx, int maxH, float gain, const uint8_t map[][2], int numBands) {
    if (barIdx < 0 || barIdx >= numBands) return 0;
    int bS = map[barIdx][0];
    int bE = map[barIdx][1];
    
    // Energy Estimation: Use RMS across the band
    float energySum = 0;
    if (bE > bS) {
        for(int b=bS; b<bE; b++) {
            float m = fftData[b];
            energySum += m * m;
        }
        energySum = sqrtf(energySum / (float)(bE - bS));
    } else {
        energySum = fftData[bS];
    }
    
    if (g_EnableAesthetics) {
        // Apply A-Weighting (perceived loudness contour)
        // If we are in 31-band stereo split, map it across the 62-band curve
        int curveIdx = (numBands == 31) ? (barIdx * 2) : barIdx;
        if (curveIdx > 61) curveIdx = 61;
        energySum *= g_AWeightingCurve[curveIdx];
    }
    
    float val = energySum * gain;
    float db = 0;
    
    if (val > 0.0001f) {
        db = 20.0f * log10f(val + 1.0f); 
    }
    
    // Adjust thresholds if aesthetics are on to compensate for the higher multipliers in the mid-range
    float maxDb = g_EnableAesthetics ? 55.0f : 48.0f; 
    float minDb = g_EnableAesthetics ? 18.0f : 12.0f; 
    
    float target_h = 0;
    if (db >= minDb) {
        if (db > maxDb) db = maxDb;
        target_h = ((db - minDb) / (maxDb - minDb)) * (float)maxH;
    }

    if (!g_EnableAesthetics) {
        return (int)target_h;
    }

    // --- Temporal Smoothing (Attack/Decay Envelope) ---
    // Make sure we have enough state allocated - we allocated 62 above. 
    // For 31 band split L/R, map right channel to indices 31-61.
    // However, getLogBarHeight doesn't inherently know if it's L or R because it's called per channel array.
    // A quick hack is to use a static pointer within the caller, but right now we only have barIdx.
    // Let's use a static rolling index or base it on memory address to uniquely track state.
    
    // Wait, getLogBarHeight is called consecutively for L then R.
    // We need unique state indices. Let's pass a true global state index.
    // Actually, modifying the signature of getLogBarHeight is better to give it an absolute index for state tracking.
    
    // Instead of changing signature right away, we can derive the absolute state index:
    // If numBands is 62 (Mono), 0-61.
    // If numBands is 31 (Split), determine if it's left or right based on the array pointer.
    extern float vRealFFTR[FFT_SIZE]; // Check if we are passing the right channel
    int stateIdx = barIdx;
    if (numBands == 31 && fftData == vRealFFTR) {
        stateIdx = barIdx + 31; // Shift right channel state up
    }
    
    float current_level = g_BarLevels[stateIdx];
    
    if (target_h > current_level) {
        // Fast Attack
        current_level = (target_h * 0.7f) + (current_level * 0.3f);
    } else {
        // Slow Decay (Gravity)
        current_level = (target_h * 0.10f) + (current_level * 0.90f);
    }
    
    g_BarLevels[stateIdx] = current_level;
    
    return (int)current_level;
}

// --- FSK Decoder State ---
FSK_Modem g_Modem;
static char g_FSKText[2048]; // Large scrollable buffer
static int g_FSKTextLen = 0;

// --- TapeTester Stats (Pure C) ---
typedef struct {
    int logLineCount;
    int dataErrors;
    int dataLengthErrors;
    int numberConversionErrors;
    int invalidCharacterErrors;
    int stopRecords;
    int totalStops;
    int sideAErrors;
    int sideALineCount;
    int sideBErrors;
    int sideBLineCount;
    char currentSide;
    bool isDctMode;
    int lastTotalTime;
} TapeStats;

static TapeStats g_TapeStats = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'A', false};
static char g_CurrentLineBuf[64];
static int g_CurrentLineIdx = 0;

// Offload State for Main Thread
volatile bool g_LineReady = false;
char g_ReadyLineBuf[64];
char g_CurrentMappedRecord[32] = ""; // Fixed header for DCT mapping

static void Tape_ProcessLine(char* line) {
    if (!line || line[0] == 0) return;
    
    // Trim trailing whitespace for robust statistics counting
    int len = strlen(line);
    while (len > 0 && (line[len-1] == ' ' || line[len-1] == '\t' || line[len-1] == '\r' || line[len-1] == '\n')) {
        line[--len] = '\0';
    }
    
    // 1. Carrier Lost Check
    if (strstr(line, "### NOCARRIER") != NULL) {
        if (g_TapeStats.stopRecords == 0) g_TapeStats.totalStops++;
        g_TapeStats.stopRecords++;
        g_HasMetadata = false; // Clear on stop
        g_CurrentMappedRecord[0] = '\0'; // Clear fixed header on stop
        return;
    }

    // 2. Valid Data Record Check (Strictly 29 chars to match generic records)
    if (len == 29 && line[0] != '#') {
        g_TapeStats.logLineCount++;
        if (g_TapeStats.currentSide == 'A') g_TapeStats.sideALineCount++;
        else g_TapeStats.sideBLineCount++;
        g_TapeStats.stopRecords = 0;

        // Split fields (ID_TRACK_HASH_TIME_TOTAL)
        char lineCopy[32];
        strncpy(lineCopy, line, sizeof(lineCopy));
        char* parts[5];
        int pCount = 0;
        char* tok = strtok(lineCopy, "_");
        while(tok && pCount < 5) {
            parts[pCount++] = tok;
            tok = strtok(NULL, "_");
        }

        if (pCount == 5) {
            char* tapeId = parts[0];
            char* hash = parts[2];
            
            // Side Detection
            int idLen = strlen(tapeId);
            if (idLen > 0) {
                char side = tapeId[idLen-1];
                if (side == 'A' || side == 'a') g_TapeStats.currentSide = 'A';
                else if (side == 'B' || side == 'b') g_TapeStats.currentSide = 'B';
            }

            // DCT Auto-Detect Check
            if (strcmp(tapeId, "DCT0A") == 0 || strcmp(tapeId, "DCT0B") == 0) {
                if (!g_TapeStats.isDctMode) {
                     printf("[DCT] Auto-Detected! Mode Enabled.\r\n");
                     g_TapeStats.isDctMode = true;
                }
            } else {
                if (g_TapeStats.isDctMode) {
                     printf("[DCT] Tape Changed. Mode Disabled.\r\n");
                     g_TapeStats.isDctMode = false;
                }
            }

            // Database Lookup (DCT vs Generic)
            char lookupHash[16] = {0};
            bool readyToLookup = false;

            if (g_TapeStats.isDctMode) {
                // DCT Mode: Use Timecode to find Real Hash and Full Line from side file
                int totalTime = atoi(parts[4]);
                g_TapeStats.lastTotalTime = totalTime;
                char mappedFullLine[32] = {0};
                if (GetMappedRecord(g_TapeStats.currentSide, totalTime, lookupHash, mappedFullLine)) {
                    readyToLookup = true;
                    // Update the fixed header instead of interspersing in the log
                    strncpy(g_CurrentMappedRecord, mappedFullLine, sizeof(g_CurrentMappedRecord)-1);
                    g_CurrentMappedRecord[sizeof(g_CurrentMappedRecord)-1] = '\0';
                } else {
                    // Optional: Print only if changed or verbose?
                    // printf("[DCT] No Map for Time: %d\r\n", totalTime);
                }
            } else {
                // Generic Mode: Use the Hash directly from the line
                g_TapeStats.lastTotalTime = atoi(parts[4]);
                if (strlen(hash) == 10) {
                    strncpy(lookupHash, hash, 11);
                    readyToLookup = true;
                }
            }

            if (readyToLookup) {
                static char lastLookupHash[16] = "";
                // Check if the TRACK changed (hash changed) OR if we don't have metadata yet
                if (strcmp(lookupHash, lastLookupHash) != 0 || !g_HasMetadata) {
                    strcpy(lastLookupHash, lookupHash);
                    
                    if (DBM_GetAudioInfo(lookupHash, &g_CurrentTrack)) {
                        g_HasMetadata = true;
                        printf("[DBM] Found: %s\r\n", g_CurrentTrack.sourcePath);
                    } else {
                        g_HasMetadata = false;
                    }
                }
            }
        } else {
            g_TapeStats.dataErrors++;
        }
    } else if (strchr(line, '_') != NULL && strstr(line, "###") == NULL) {
        g_TapeStats.dataLengthErrors++;
        g_TapeStats.dataErrors++;
        if (g_TapeStats.currentSide == 'A') g_TapeStats.sideAErrors++;
        else g_TapeStats.sideBErrors++;
    }
}

void addFSKChar(char c) {
    // Thread Safety: This can be called from Modem Task (background) 
    // and from Main Loop (Tape_ProcessLine injections).
    vTaskSuspendAll();

    // 1. Add to scroll buffer (Simple char array, safe for single-writer/single-reader)
    if (g_FSKTextLen < sizeof(g_FSKText) - 1) {
        g_FSKText[g_FSKTextLen++] = c;
        g_FSKText[g_FSKTextLen] = '\0';
    } else {
        memmove(g_FSKText, g_FSKText + 1, sizeof(g_FSKText) - 1);
        g_FSKText[sizeof(g_FSKText) - 2] = c;
        g_FSKText[sizeof(g_FSKText) - 1] = '\0';
    }

    // 2. Add to line buffer for TapeStats (Only called from Modem task / Main loop)
    if (c == '\n' || c == '\r') {
        if (g_CurrentLineIdx > 0) {
            g_CurrentLineBuf[g_CurrentLineIdx] = '\0';
            
            // Offload: Copy to ready buffer and signal main thread
            // This prevents blocking the modem task with SD Card I/O
            if (!g_LineReady) {
                strncpy(g_ReadyLineBuf, g_CurrentLineBuf, sizeof(g_ReadyLineBuf));
                // Only signal if not a control message (unless we want to log it)
                g_LineReady = true; 
            }
            
            g_CurrentLineIdx = 0;
        }
    } else if (g_CurrentLineIdx < sizeof(g_CurrentLineBuf) - 1) {
        g_CurrentLineBuf[g_CurrentLineIdx++] = c;
    }

    xTaskResumeAll();
}

void addFSKDisplayChar(char c) {
    vTaskSuspendAll();
    // Only update the scroll buffer, NEVER the line buffer used for stats
    if (g_FSKTextLen < sizeof(g_FSKText) - 1) {
        g_FSKText[g_FSKTextLen++] = c;
        g_FSKText[g_FSKTextLen] = '\0';
    } else {
        memmove(g_FSKText, g_FSKText + 1, sizeof(g_FSKText) - 1);
        g_FSKText[sizeof(g_FSKText) - 2] = c;
        g_FSKText[sizeof(g_FSKText) - 1] = '\0';
    }
    xTaskResumeAll();
}

void addFSKDisplayString(const char* s) {
    if (!s) return;
    while (*s) {
        addFSKDisplayChar(*s++);
    }
}


// --- Custom Waterfall Color Scheme (Light Blue -> Brown -> Orange) ---
static uint32_t wfall_lut[256];
static void initWaterfallLUT(void) {
    for (int i = 0; i < 256; i++) {
        // Simple linear interpolation
        // 0: Light Blue (173, 216, 230)
        // 128: Brown (139, 69, 19)
        // 255: Orange (255, 165, 0)
        int r, g, b;
        if (i < 128) {
            // 0: Deep Blue (0, 80, 255) - Reduced green for "more blue" look
            // 128: Rich Brown (139, 69, 19)
            float f = i / 128.0f;
            r = (int)(0   * (1.0f - f) + 139 * f);
            g = (int)(80  * (1.0f - f) + 69  * f);
            b = (int)(255 * (1.0f - f) + 19  * f);
        } else {
            // 128: Brown (139, 69, 19)
            // 255: Orange (255, 140, 0)
            float f = (i - 128) / 127.0f;
            r = (int)(139 * (1.0f - f) + 255 * f);
            g = (int)(69  * (1.0f - f) + 140 * f);
            b = (int)(19  * (1.0f - f) + 0   * f);
        }
        wfall_lut[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
    }
}

// --- UI State ---
#define MAX_BUTTONS 7
static Button buttons[MAX_BUTTONS];
static int buttonCount = 0;
static bool ts_enabled = false;

// --- Prototypes ---
static void drawSpectrum(void);
static void drawVU(void);
static void drawWaterfall(void);
static void cycleBaudRate(void);
static void initFSKModems(void); // New helper to sync both
static void initButtons(void);
static void handleTouch(void);
static void toggleSpectrum(void);
static void toggleWaterfall(void);
static void toggleVU(void);
static void toggleSim(void);
static void toggleInput(void);
static void cycleVUGain(void);
static void toggleFSK(void);
static void drawFSKText(void);
static void Visualizer_InitAudio(void);
static uint32_t Color565ToARGB(uint16_t rgb565);
static void FillRectDMA2D(uint32_t* fb, int x, int y, int w, int h, uint32_t color);

static uint32_t heatARGB[256];

static void initSpectrumLUT(void) {
    for (int i = 0; i < 256; i++) {
        heatARGB[i] = Color565ToARGB(heat565[i]);
    }
}

static void FillRectCPU(uint32_t* fb, int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 480) w = 480 - x;
    if (y + h > 272) h = 272 - y;
    if (w <= 0 || h <= 0) return;

    for (int py = 0; py < h; py++) {
        uint32_t* row = &fb[(y + py) * 480 + x];
        for (int px = 0; px < w; px++) {
            row[px] = color;
        }
    }
}

// --- Initialization ---
void Visualizer_Init(void) {
    uint8_t status;
    
    printf("Visualizer_Init: Starting LCD/TS Init...\r\n");
    status = BSP_LCD_Init();
    if (status != LCD_OK) printf("BSP_LCD_Init FAILED\r\n");

    BSP_LCD_LayerDefaultInit(0, LCD_FB_START_ADDRESS);

    BSP_LCD_SelectLayer(0);
    BSP_LCD_DisplayOn();
    BSP_LCD_Clear(LCD_COLOR_BLACK);
    
    // Initialize FFT LUTs
    SimpleFFT_Init();
    
    status = BSP_TS_Init(BSP_LCD_GetXSize(), BSP_LCD_GetYSize());
    ts_enabled = (status == TS_OK);
    
    // FSK Modems initialization: Force default to 1200 baud
    g_BaudIdx = 2;
    g_BaudRate = 1200.0f;
    initFSKModems();
    printf("VisualizerApp: FSK Decoder ready (1200 Baud)...\r\n");
    
    printf("VisualizerApp: Clearing Waterfall Buffer in SDRAM...\r\n");
    initWaterfallLUT(); // CRITICAL: Restore LUT initialization
    initSpectrumLUT();  // Precompute spectrum ARGB colors
    uint32_t bgColor = wfall_lut[0];
    uint32_t* ptr = (uint32_t*)WFALL_FB_ADDRESS;
    for(int i=0; i<WFALL_WIDTH * WFALL_HEIGHT; i++) ptr[i] = bgColor;
    // Commit to SDRAM so DMA2D doesn't see garbage on first scroll
    SCB_CleanDCache_by_Addr((uint32_t*)WFALL_FB_ADDRESS, WFALL_WIDTH * WFALL_HEIGHT * 4);

    // Calibrate CPU baseline (idle ticks over 100ms)
    printf("VisualizerApp: Calibrating CPU...\r\n");
    g_IdleTicks = 0;
    osDelay(100); // Use osDelay to yield to Idle task
    cpu_baseline = g_IdleTicks;
    if (cpu_baseline == 0) cpu_baseline = 1; // Prevent div by zero
    last_idle_val = g_IdleTicks;
    last_cpu_tick = HAL_GetTick();
    
    printf("VisualizerApp: initButtons...\r\n");
    initButtons();
    
    printf("VisualizerApp: Visualizer_InitAudio...\r\n");
    Visualizer_InitAudio();

    printf("VisualizerApp: Checking SD Card status...\r\n");
    
    // Set Font and Background for loading messages
    BSP_LCD_SetFont(&Font20);
    BSP_LCD_SetTextColor(LCD_COLOR_YELLOW);
    BSP_LCD_SetBackColor(LCD_COLOR_BLACK);

    if (BSP_SD_IsDetected() == SD_PRESENT) {
        printf("VisualizerApp: SD Card DETECTED. Proceeding with DB Init.\r\n");
        
        BSP_LCD_SetTextColor(LCD_COLOR_BLACK);
        BSP_LCD_FillRect(0, 120, 480, Font20.Height);
        BSP_LCD_SetTextColor(LCD_COLOR_YELLOW);
        BSP_LCD_DisplayStringAt(0, 120, (uint8_t*)"Loading AUDIODB...", CENTER_MODE);
        printf("VisualizerApp: DBM_Init (SDRAM @ 0xC0500000)...\r\n");
        // Use Uppercase for 8.3 compatibility as LFN is disabled in ffconf.h
        if (!DBM_Init("/AUDIODB.TXT", 0xC0500000)) {
            printf("VisualizerApp: DBM_Init FAILED!\r\n");
            BSP_LCD_SetTextColor(LCD_COLOR_RED);
            BSP_LCD_DisplayStringAt(0, 150, (uint8_t*)"DB Load Failed!", CENTER_MODE);
        }

        BSP_LCD_SetTextColor(LCD_COLOR_BLACK);
        BSP_LCD_FillRect(0, 120, 480, Font20.Height);
        BSP_LCD_SetTextColor(LCD_COLOR_YELLOW);
        BSP_LCD_DisplayStringAt(0, 120, (uint8_t*)"Loading Side Maps...", CENTER_MODE);
        printf("VisualizerApp: Loading Side Maps (RAM)...\r\n");
        g_SideACount = LoadSideMap('A', SIDE_MAP_A_ADDR);
        g_SideBCount = LoadSideMap('B', SIDE_MAP_B_ADDR);
        
    } else {
        printf("VisualizerApp: SD Card NOT DETECTED! Skipping Database Load.\r\n");
        printf("VisualizerApp: System will run in Visualizer-Only mode.\r\n");
        
        BSP_LCD_SetTextColor(LCD_COLOR_BLACK);
        BSP_LCD_FillRect(0, 120, 480, Font20.Height);
        BSP_LCD_SetTextColor(LCD_COLOR_RED);
        BSP_LCD_DisplayStringAt(0, 120, (uint8_t*)"SD Card NOT DETECTED!", CENTER_MODE);
        
        g_SideACount = 0;
        g_SideBCount = 0;
        // DBM is static/global, so it naturally stays uninitialized (empty index)
    }

    // Final Cleanup: Clear screen one more time to Layer 0 before visualizer loop starts
    // This ensures no artifacts like "LLoading" remain.
    BSP_LCD_Clear(LCD_COLOR_BLACK);
    
    printf("VisualizerApp: Init sequence complete.\r\n");
}

void Visualizer_ProcessAudio(int16_t* inBuf, uint32_t samples) {
    extern volatile bool g_IsEncoding;
    if (g_IsEncoding) g_SimulationMode = false; // Force real view during encoding
    if (g_SimulationMode && !g_IsEncoding) return;

    // Cache management is handled by the caller (main.c)

// 1. FSK Processing moved to StartModemTask in main.c

    // Debug Print every 1s (approx)
    if (HAL_GetTick() - g_LastDebugTime > 1000) {
        g_LastDebugTime = HAL_GetTick();
        // FSK DIAGNOSTICS (moved from interrupt context)
        if (g_ShowFSK) {
            // FSK Diagnostics could be added here using public members like g_Modem.lastSNR
            // printf("[FSK] SNR: %d.%02d\n", (int)g_Modem.lastSNR, (int)(g_Modem.lastSNR * 100) % 100);
        }
        g_MaxSignalLevel = 0.0f; // Reset peak hold
    }

    // 2. Full-Sample VU Energy Detection (Raw 48kHz RMS Scan)
    // Handle 4-slot TDM (I2S alignment on STM32)
    // Left channel occupies the first half of the frame (Slots 0 and 1 = indices i, i+1)
    // Right channel occupies the second half of the frame (Slots 2 and 3 = indices i+2, i+3)
    // LineIn uses Slots 0 and 2. Mic2 uses Slots 1 and 3. Summing them handles either source seamlessly.
    uint32_t step = 4;
    float sumSqL = 0, sumSqR = 0;
    for (uint32_t i = 0; i < samples; i += step) {
        float sL = ((float)inBuf[i] + (float)inBuf[i + 1]) / 32768.0f;
        float sR = ((float)inBuf[i + 2] + (float)inBuf[i + 3]) / 32768.0f;
        sumSqL += sL * sL;
        sumSqR += sR * sR;
    }
    
    // Instant Attack for RMS (with gain alignment)
    uint32_t numFrames = samples / step;
    float currentGain = g_IsEncoding ? 1.0f : g_AudioGain;
    float instantRmsL = sqrtf(sumSqL / numFrames) * currentGain;
    float instantRmsR = sqrtf(sumSqR / numFrames) * currentGain;
    
    if (instantRmsL > peakL) peakL = instantRmsL;
    if (instantRmsR > peakR) peakR = instantRmsR;

    // 3. Process decimated stream for FFT
    for (uint32_t i = 0; i < samples; i += step * RX_DECIMATION) {
        int32_t sumL = 0, sumR = 0;
        for (int j = 0; j < RX_DECIMATION; j++) {
            sumL += inBuf[i + j * step] + inBuf[i + j * step + 1];
            sumR += inBuf[i + j * step + 2] + inBuf[i + j * step + 3];
        }
        
        float rawL = (float)(sumL / RX_DECIMATION) / 32768.0f;
        float rawR = (float)(sumR / RX_DECIMATION) / 32768.0f;
        float mono = (rawL + rawR) * 0.5f;
        
        static float dc_offset = 0.0f;
        dc_offset = 0.999f * dc_offset + 0.001f * mono;
        float clean = (mono - dc_offset) * g_AudioGain;
        
        if (fftIdx < FFT_SIZE) {
            vReal[fftIdx] = clean;
            vImag[fftIdx] = 0.0f;
            vRealL[fftIdx] = rawL * currentGain;
            vRealR[fftIdx] = rawR * currentGain;
            fftIdx++;
        }
    }
}

void Visualizer_Update(void) {
    handleTouch();
    osDelay(5); // Reduce CPU load
    UpdateFSKEncode(); // Background encoding (even if panel hidden)
    
    int back_idx = 1 - front_buffer_idx;
    uint32_t back_addr = fb_addresses[back_idx];
    uint32_t* back_fb = (uint32_t*)back_addr;

    // --- Prepare Hidden Buffer for Drawing ---
    // We ONLY update the internal pointers here. 
    // DO NOT trigger a hardware reload yet! This fixes the flickering.
    hLtdcHandler.LayerCfg[0].FBStartAdress = back_addr; 
    
    // Invalidate D-Cache for the HUD and Viz area only
    SCB_InvalidateDCache_by_Addr((uint32_t*)back_addr, 480 * UI_VIZ_BOTTOM * 4);

    // Targeted Screen Clear (DMA2D) - Full Height (272)
    // We restore full clear to ensure double-buffer consistency until partial updates are perfect.
    FillRectDMA2D(back_fb, 0, 0, 480, 272, LCD_COLOR_BLACK);

    if (g_SimulationMode) {
        static float simP = 0.0f; simP += 0.04f; // Slower simulation for smoother look
        peakL = (sinf(simP) + 1.0f) * 0.5f; peakR = (cosf(simP * 1.3f) + 1.0f) * 0.5f;
        for(int i=0; i<64; i++) fft_mag_sim[i] = (uint8_t)((sinf(simP + i*0.2f) + 1.0f) * 127.0f);
    } else if (fftIdx >= FFT_SIZE) {
        // 1. Process FFT with 50% overlap (every 256 samples)
        if (g_SpectrumMode == 1 || g_SpectrumMode == 3) {
            // Compute Left FFT
            memcpy(vRealFFT, vRealL, sizeof(vRealFFT)); 
            memset(vImagFFT, 0, sizeof(vImagFFT));
            SimpleFFT_Windowing_Fast(vRealFFT, FFT_SIZE);
            SimpleFFT_Compute(vRealFFT, vImagFFT, FFT_SIZE);
            SimpleFFT_ComplexToMagnitude(vRealFFT, vImagFFT, FFT_SIZE);
            memcpy(vRealFFTL, vRealFFT, sizeof(vRealFFTL));

            // Compute Right FFT
            memcpy(vRealFFT, vRealR, sizeof(vRealFFT)); 
            memset(vImagFFT, 0, sizeof(vImagFFT));
            SimpleFFT_Windowing_Fast(vRealFFT, FFT_SIZE);
            SimpleFFT_Compute(vRealFFT, vImagFFT, FFT_SIZE);
            SimpleFFT_ComplexToMagnitude(vRealFFT, vImagFFT, FFT_SIZE);
            memcpy(vRealFFTR, vRealFFT, sizeof(vRealFFTR));
            
            // OPTIMIZATION: Synthesize Mono for Waterfall from L/R to avoid 3rd FFT
            if (g_ShowWaterfall) {
                for(int i=0; i<FFT_SIZE/2; i++) {
                    vRealFFT[i] = (vRealFFTL[i] + vRealFFTR[i]) * 0.5f;
                }
            }
        } else {
            // Standard Mono FFT
            memcpy(vRealFFT, vReal, sizeof(vRealFFT)); 
            memset(vImagFFT, 0, sizeof(vImagFFT));
            SimpleFFT_Windowing_Fast(vRealFFT, FFT_SIZE);
            SimpleFFT_Compute(vRealFFT, vImagFFT, FFT_SIZE);
            SimpleFFT_ComplexToMagnitude(vRealFFT, vImagFFT, FFT_SIZE);
        }
        
        // --- 50% Overlap Buffer Shift ---
        int shift = 256;
        memmove(vReal, vReal + shift, (FFT_SIZE - shift) * sizeof(float));
        memmove(vRealL, vRealL + shift, (FFT_SIZE - shift) * sizeof(float));
        memmove(vRealR, vRealR + shift, (FFT_SIZE - shift) * sizeof(float));
        fftIdx = 256;
    }

    // Decay: Smoothly drop meters in UI loop
    peakL *= g_VUDecay;
    peakR *= g_VUDecay;
    
    // Global Spectrum Peak Decay
    int totalPeaks = 62; // Always 62 bands in both Mono and Split
    for(int i=0; i<totalPeaks; i++) {
        if (g_PeakHoldCount[i] > 0) g_PeakHoldCount[i]--;
        else if (g_SpectrumPeaks[i] > 0) g_SpectrumPeaks[i] -= g_PeakDecay;
    }

    // VU Peak Decay
    for(int i=0; i<2; i++) {
        if (g_VUPeakHoldCount[i] > 0) g_VUPeakHoldCount[i]--;
        else if (g_VUPeaks[i] > 0) g_VUPeaks[i] -= (g_PeakDecay * 2.0f); // Fast horizontal fall
    }

    // 2. Draw Visualizations (Background layer)
    if (g_ShowWaterfall) drawWaterfall();
    else if (g_ShowSpectrum) drawSpectrum();
    
    if (g_ShowFSK) drawFSKText();

    // Calculate CPU Load every 500ms
    uint32_t now = HAL_GetTick();
    if (now - last_cpu_tick >= 500) {
        uint32_t diff = g_IdleTicks - last_idle_val;
        float expected = (float)cpu_baseline * (float)(now - last_cpu_tick) / 100.0f;
        if (expected > 0) {
            float load = 100.0f * (1.0f - (float)diff / expected);
            if (load < 0) load = 0;
            if (load > 100) load = 100;
            current_cpu_load = (int)load;
        }
        last_idle_val = g_IdleTicks;
        last_cpu_tick = now;
    }

    // 3. Draw HUD elements (Header) - ALWAYS draw to avoid flicker in double buffer
    BSP_LCD_SetTextColor(LCD_COLOR_BLUE);
    FillRectDMA2D(back_fb, 0, 0, 480, UI_HEADER_H, LCD_COLOR_BLUE);
    while (hdma2d.Instance->CR & DMA2D_CR_START);

    BSP_LCD_SetTextColor(LCD_COLOR_WHITE);
    BSP_LCD_SetBackColor(LCD_COLOR_BLUE);
    BSP_LCD_SetFont(&Font16);
    char buf[64]; 
    int freq_khz = SAMPLING_FREQ / 1000;
    
    // Calculate FPS
    static uint32_t frame_count = 0;
    static uint32_t last_fps_time = 0;
    static int current_fps = 0;
    
    frame_count++;
    if (HAL_GetTick() - last_fps_time >= 1000) {
        current_fps = frame_count;
        frame_count = 0;
        last_fps_time = HAL_GetTick();
        printf("[SYS] FPS:%d | CPU:%d%%\r\n", current_fps, current_cpu_load);
        
        // FSK DIAGNOSTICS
        if (g_ShowFSK) {
            float mMag = Filter_GetMag(&g_Modem.filterMark);
            float sMag = Filter_GetMag(&g_Modem.filterSpace);
            printf("[FSK DIAG] Baud:%d | Sig:%d%% | M:%d S:%d | SNR:%f | Carr:%d\r\n", 
                   (int)g_Modem.cfg.baudRate, 
                   (int)(g_MaxSignalLevel * 100.0f), 
                   (int)(mMag * 100.0f), (int)(sMag * 100.0f),
                   g_Modem.lastSNR, 
                   g_Modem.carrier);
             g_MaxSignalLevel = 0.0f; // Reset peak hold
        }
    }

    float dispL = (peakL / g_AudioGain) * 100.0f;
    float dispR = (peakR / g_AudioGain) * 100.0f;
    if (dispL > 100.0f) dispL = 100.0f;
    if (dispR > 100.0f) dispR = 100.0f;

    snprintf(buf, sizeof(buf), "DiscVU %dkHz / L:%03d%% R:%03d%% CPU:%d%% FPS:%d", 
             freq_khz, (int)dispL, (int)dispR, current_cpu_load, current_fps);
    BSP_LCD_DisplayStringAt(10, 7, (uint8_t*)buf, LEFT_MODE);

    // Final safety wait: ensure all DMA2D operations completed before returning
    while (hdma2d.Instance->CR & DMA2D_CR_START);
    
    // 4. Draw VU Meter (Overlay at top-ish)
    if (g_ShowVUMeter) drawVU();

    // 5. Draw Buttons (Overlay at bottom)
    for(int i=0; i<buttonCount; i++) {
        Button* b = &buttons[i];
        uint32_t color = *b->toggleMode ? b->color : LCD_COLOR_DARKGRAY;
        FillRectDMA2D(back_fb, b->x, b->y, b->w, b->h, color);
        BSP_LCD_SetTextColor(LCD_COLOR_WHITE);
        BSP_LCD_SetBackColor(color);
        BSP_LCD_SetFont(&Font12);
        BSP_LCD_DisplayStringAt(b->x + 5, b->y + 10, (uint8_t*)b->text, LEFT_MODE);
    }

    // 6. Commit Frame (VSync-Locked Flip)
    SCB_CleanDCache_by_Addr((uint32_t*)back_addr, 480 * 272 * 4);
    
    // Ensure all DMA2D operations are finished
    while (hdma2d.Instance->CR & DMA2D_CR_START);
    
    // 6. Handle Background Tasks (Main Thread Context)
    // Check for FSK lines offloaded from Modem Task
    if (g_LineReady) {
        Tape_ProcessLine(g_ReadyLineBuf);
        g_LineReady = false;
    }

    // 7. Flip Buffers (Hardware VSync)

    // Update hardware address and request Vertical Blanking Reload
    // Use NoReload variant to ensure Atomic switch at VSync
    HAL_LTDC_SetAddress_NoReload(&hLtdcHandler, back_addr, 0);
    HAL_LTDC_Reload(&hLtdcHandler, LTDC_SRCR_VBR); 
    
    // IMPORTANT: Wait for hardware to acknowledge the flip.
    // This prevents racing the scanline and eliminates all flickering.
    uint32_t reload_start = HAL_GetTick();
    while ((hLtdcHandler.Instance->SRCR & LTDC_SRCR_VBR) && (HAL_GetTick() - reload_start < 20)); 

    front_buffer_idx = back_idx;
}


static void drawSpectrum() {
    int bottomY = UI_VIZ_BOTTOM; int maxH = UI_VIZ_BOTTOM - UI_VIZ_TOP;
    uint32_t* back_fb = (uint32_t*)hLtdcHandler.LayerCfg[0].FBStartAdress;
    
    // WAIT for DMA2D background clear to finish before CPU starts drawing arrays over it
    while (hdma2d.Instance->CR & DMA2D_CR_START);
    
    if (g_SpectrumMode == 1) {
        // --- 31/31 ISO Standard Stereo Split ---
        int barW = 7; 
        int totalW = 31 * barW;
        int leftX = (240 - totalW) / 2;
        int rightX = 240 + (240 - totalW) / 2;

        // Draw Divider
        for(int i=UI_VIZ_TOP; i<UI_VIZ_BOTTOM; i++) back_fb[i*480 + 240] = 0xFF555555;

        for(int i=0; i<31; i++) {
            // LEFT CHANNEL
            int hL = (g_SimulationMode) ? (fft_mag_sim[i] * maxH) / 255 : getLogBarHeight(vRealFFTL, i, maxH, g_SpectrumGain, g_ISO31_Map, 31);
            
            if (g_EnablePeakHold) {
                if ((float)hL >= g_SpectrumPeaks[i]) { g_SpectrumPeaks[i] = (float)hL; g_PeakHoldCount[i] = g_PeakHoldFrames; }
            }
            FillRectCPU(back_fb, leftX + i*barW, bottomY - hL, barW-1, hL, COLOR_SPEC_L);
            if (g_EnablePeakHold) {
                int peakY = bottomY - (int)g_SpectrumPeaks[i];
                if (peakY < UI_VIZ_TOP) peakY = UI_VIZ_TOP;
                for(int py=0; py<2; py++) {
                    uint32_t* row = &back_fb[(peakY + py) * 480 + (leftX + i*barW)];
                    for(int px=0; px<(barW-1); px++) row[px] = COLOR_SPEC_L;
                }
            }

            // RIGHT CHANNEL
            int hR = (g_SimulationMode) ? (fft_mag_sim[i] * maxH) / 255 : getLogBarHeight(vRealFFTR, i, maxH, g_SpectrumGain, g_ISO31_Map, 31);
            int rIdx = 31 + i;
            if (g_EnablePeakHold) {
                if ((float)hR >= g_SpectrumPeaks[rIdx]) { g_SpectrumPeaks[rIdx] = (float)hR; g_PeakHoldCount[rIdx] = g_PeakHoldFrames; }
            }
            FillRectCPU(back_fb, rightX + i*barW, bottomY - hR, barW-1, hR, COLOR_SPEC_R);
             if (g_EnablePeakHold) {
                int peakY = bottomY - (int)g_SpectrumPeaks[rIdx];
                if (peakY < UI_VIZ_TOP) peakY = UI_VIZ_TOP;
                for(int py=0; py<2; py++) {
                    uint32_t* row = &back_fb[(peakY + py) * 480 + (rightX + i*barW)];
                    for(int px=0; px<(barW-1); px++) row[px] = COLOR_SPEC_R;
                }
            }
        }
    } else if (g_SpectrumMode == 2) {
        // --- 62-Band LED Mono Mode ---
        int barW = 7; 
        int totalW = 62 * barW;
        int startX = (480 - totalW) / 2;
        int segH = 4;
        int gap = 1;
        int totalSegH = segH + gap;
        
        for(int i=0; i<62; i++) {
            int h = (g_SimulationMode) ? (fft_mag_sim[i] * maxH) / 255 : getLogBarHeight(vRealFFT, i, maxH, g_SpectrumGain, g_ISO62_Map, 62);
            
            if (g_EnablePeakHold) {
                if ((float)h >= g_SpectrumPeaks[i]) { g_SpectrumPeaks[i] = (float)h; g_PeakHoldCount[i] = g_PeakHoldFrames; }
            }

            // Draw LED Segments
            int numSegs = h / totalSegH;
            for (int s = 0; s < numSegs; s++) {
                uint32_t segColor;
                float percent = (float)(s * totalSegH) / (float)maxH;
                if (percent < 0.6f) segColor = LCD_COLOR_GREEN;
                else if (percent < 0.85f) segColor = LCD_COLOR_YELLOW;
                else segColor = LCD_COLOR_RED;
                
                FillRectCPU(back_fb, startX + i*barW, bottomY - (s+1)*totalSegH + gap, barW-1, segH, segColor);
            }

            if (g_EnablePeakHold) {
                int peakY = bottomY - (int)g_SpectrumPeaks[i];
                if (peakY < UI_VIZ_TOP) peakY = UI_VIZ_TOP;
                // Peak dot color matches the segment level
                uint32_t peakColor;
                float pPercent = (float)(g_SpectrumPeaks[i]) / (float)maxH;
                if (pPercent < 0.6f) peakColor = LCD_COLOR_GREEN;
                else if (pPercent < 0.85f) peakColor = LCD_COLOR_YELLOW;
                else peakColor = LCD_COLOR_RED;

                for(int py=0; py<2; py++) {
                    uint32_t* row = &back_fb[(peakY + py) * 480 + (startX + i*barW)];
                    for(int px=0; px<(barW-1); px++) row[px] = peakColor;
                }
            }
        }
    } else if (g_SpectrumMode == 3) {
        // --- 31/31 ISO Standard Stereo Split LED Mode ---
        int barW = 7; 
        int totalW = 31 * barW;
        int leftX = (240 - totalW) / 2;
        int rightX = 240 + (240 - totalW) / 2;
        int segH = 4;
        int gap = 1;
        int totalSegH = segH + gap;

        // Draw Divider
        for(int i=UI_VIZ_TOP; i<UI_VIZ_BOTTOM; i++) back_fb[i*480 + 240] = 0xFF555555;

        for(int i=0; i<31; i++) {
            // LEFT CHANNEL LED
            int hL = (g_SimulationMode) ? (fft_mag_sim[i] * maxH) / 255 : getLogBarHeight(vRealFFTL, i, maxH, g_SpectrumGain, g_ISO31_Map, 31);
            if (g_EnablePeakHold) {
                if ((float)hL >= g_SpectrumPeaks[i]) { g_SpectrumPeaks[i] = (float)hL; g_PeakHoldCount[i] = g_PeakHoldFrames; }
            }
            int numSegsL = hL / totalSegH;
            for (int s = 0; s < numSegsL; s++) {
                uint32_t segColor;
                float percent = (float)(s * totalSegH) / (float)maxH;
                if (percent < 0.6f) segColor = LCD_COLOR_GREEN;
                else if (percent < 0.85f) segColor = LCD_COLOR_YELLOW;
                else segColor = LCD_COLOR_RED;
                FillRectCPU(back_fb, leftX + i*barW, bottomY - (s+1)*totalSegH + gap, barW-1, segH, segColor);
            }
            if (g_EnablePeakHold) {
                int peakY = bottomY - (int)g_SpectrumPeaks[i];
                if (peakY < UI_VIZ_TOP) peakY = UI_VIZ_TOP;
                uint32_t peakColor;
                float pPercent = (float)(g_SpectrumPeaks[i]) / (float)maxH;
                if (pPercent < 0.6f) peakColor = LCD_COLOR_GREEN;
                else if (pPercent < 0.85f) peakColor = LCD_COLOR_YELLOW;
                else peakColor = LCD_COLOR_RED;
                for(int py=0; py<2; py++) {
                    uint32_t* row = &back_fb[(peakY + py) * 480 + (leftX + i*barW)];
                    for(int px=0; px<(barW-1); px++) row[px] = peakColor;
                }
            }

            // RIGHT CHANNEL LED
            int hR = (g_SimulationMode) ? (fft_mag_sim[i] * maxH) / 255 : getLogBarHeight(vRealFFTR, i, maxH, g_SpectrumGain, g_ISO31_Map, 31);
            int rIdx = 31 + i;
            if (g_EnablePeakHold) {
                if ((float)hR >= g_SpectrumPeaks[rIdx]) { g_SpectrumPeaks[rIdx] = (float)hR; g_PeakHoldCount[rIdx] = g_PeakHoldFrames; }
            }
            int numSegsR = hR / totalSegH;
            for (int s = 0; s < numSegsR; s++) {
                uint32_t segColor;
                float percent = (float)(s * totalSegH) / (float)maxH;
                if (percent < 0.6f) segColor = LCD_COLOR_GREEN;
                else if (percent < 0.85f) segColor = LCD_COLOR_YELLOW;
                else segColor = LCD_COLOR_RED;
                FillRectCPU(back_fb, rightX + i*barW, bottomY - (s+1)*totalSegH + gap, barW-1, segH, segColor);
            }
            if (g_EnablePeakHold) {
                int peakY = bottomY - (int)g_SpectrumPeaks[rIdx];
                if (peakY < UI_VIZ_TOP) peakY = UI_VIZ_TOP;
                uint32_t peakColor;
                float pPercent = (float)(g_SpectrumPeaks[rIdx]) / (float)maxH;
                if (pPercent < 0.6f) peakColor = LCD_COLOR_GREEN;
                else if (pPercent < 0.85f) peakColor = LCD_COLOR_YELLOW;
                else peakColor = LCD_COLOR_RED;
                for(int py=0; py<2; py++) {
                    uint32_t* row = &back_fb[(peakY + py) * 480 + (rightX + i*barW)];
                    for(int px=0; px<(barW-1); px++) row[px] = peakColor;
                }
            }
        }
    } else {
        // --- 62-Band ISO Standard Mono Mode ---
        int barW = 7; 
        int totalW = 62 * barW;
        int startX = (480 - totalW) / 2;
        
        for(int i=0; i<62; i++) {
            int h = (g_SimulationMode) ? (fft_mag_sim[i] * maxH) / 255 : getLogBarHeight(vRealFFT, i, maxH, g_SpectrumGain, g_ISO62_Map, 62);
            
            if (g_EnablePeakHold) {
                if ((float)h >= g_SpectrumPeaks[i]) { g_SpectrumPeaks[i] = (float)h; g_PeakHoldCount[i] = g_PeakHoldFrames; }
            }

            uint32_t barColor = heatARGB[i*4%256];
            FillRectCPU(back_fb, startX + i*barW, bottomY - h, barW-1, h, barColor);
            if (g_EnablePeakHold) {
                int peakY = bottomY - (int)g_SpectrumPeaks[i];
                if (peakY < UI_VIZ_TOP) peakY = UI_VIZ_TOP;
                for(int py=0; py<2; py++) {
                    uint32_t* row = &back_fb[(peakY + py) * 480 + (startX + i*barW)];
                    for(int px=0; px<(barW-1); px++) row[px] = barColor;
                }
            }
        }
    }
}

static void drawWaterfall() {
    int topY = UI_VIZ_TOP;
    int height = UI_VIZ_BOTTOM - UI_VIZ_TOP; // Exactly 200
    int width = WFALL_WIDTH; // 480
    uint32_t* wfall_fb = (uint32_t*)WFALL_FB_ADDRESS;

    // 1. Advance Circular Head
    g_WfallHead--;
    if (g_WfallHead < 0) g_WfallHead = height - 1;
    
    uint32_t* newRow = &wfall_fb[g_WfallHead * width];

    // 2. Fill new row with FFT data
    if (g_SimulationMode) {
        for(int x=0; x<width; x++) {
            int bin = (x * 64) / width;
            newRow[x] = wfall_lut[fft_mag_sim[bin % 64]];
        }
    } else {
        float aMax = 0.5f;
        for(int i=2; i<FFT_SIZE/2; i++) if(vRealFFT[i] > aMax) aMax = vRealFFT[i];

        for(int x=0; x<width; x++) {
            int bin = (x * (FFT_SIZE/2)) / width;
            if (bin < 2) bin = 2;
            float mag = vRealFFT[bin];
            int cIdx = (int)((mag * g_WaterfallGain / aMax) * 255.0f);
            if(cIdx > 255) cIdx = 255;
            newRow[x] = wfall_lut[cIdx];
        }
    }

    // Clean ONLY the new row in D-Cache
    SCB_CleanDCache_by_Addr((uint32_t*)newRow, width * 4);

    // 3. Draw Circular Buffer to Screen using DMA2D (Two chunks)
    uint32_t back_fb_addr = hLtdcHandler.LayerCfg[0].FBStartAdress;
    uint32_t* screen_ptr = (uint32_t*)(back_fb_addr + topY * 480 * 4);
    
    // Part A: From Head to Bottom of buffer
    int rowsA = height - g_WfallHead;
    CopyBlockDMA2D(&wfall_fb[g_WfallHead * width], screen_ptr, width, rowsA);
    
    // Part B: Wrap around from top of buffer to (Head - 1)
    if (g_WfallHead > 0) {
        int rowsB = g_WfallHead;
        CopyBlockDMA2D(wfall_fb, &screen_ptr[rowsA * width], width, rowsB);
    }
}

static void drawVU() {
    int w = g_ShowFSKEncode ? 200 : 400; 
    int h = 12; int x = 40;
    int yL = UI_HEADER_H + 5; int yR = UI_HEADER_H + 20;

    // Logarithmic (dB) Scaling for "Sensitive" Meters
    // Range: -70dB to +3dB (Headroom for FSK pinning)
    // Using log10f(rms + offset) for stability. 0dB = 1.0f (Target)
    float dbL = 20.0f * log10f((peakL * g_VUGain) / g_AudioGain + 0.000001f);
    float dbR = 20.0f * log10f((peakR * g_VUGain) / g_AudioGain + 0.000001f);
    
    // Calibration: map -60dB -> 0%, 0dB -> 90%, +6dB -> 100%
    // This ensures headroom and prevents pinning.
    const float minDb = -60.0f;
    const float maxDb = 6.0f;
    
    float normL = (dbL - minDb) / (maxDb - minDb);
    if (normL < 0) normL = 0.0f; 
    if (normL > 1.0f) normL = 1.0f;
    float normR = (dbR - minDb) / (maxDb - minDb);
    if (normR < 0) normR = 0.0f; 
    if (normR > 1.0f) normR = 1.0f;

    int lL = (int)(normL * w);
    int rL = (int)(normR * w);
    
    uint32_t* back_fb = (uint32_t*)hLtdcHandler.LayerCfg[0].FBStartAdress;

    if (g_EnablePeakHold) {
        // Attack Logic: Keep instant response in draw loop
        if ((float)lL >= g_VUPeaks[0]) {
            g_VUPeaks[0] = (float)lL;
            g_VUPeakHoldCount[0] = g_PeakHoldFrames; 
        }
        
        if ((float)rL >= g_VUPeaks[1]) {
            g_VUPeaks[1] = (float)rL;
            g_VUPeakHoldCount[1] = g_PeakHoldFrames;
        }
    }

    // Draw L Channel (Green)
    if(lL > 0) FillRectDMA2D(back_fb, x, yL, lL, h, LCD_COLOR_GREEN);
    // Draw L Peak (Green, 4px wide, CPU)
    if(g_EnablePeakHold && g_VUPeaks[0] > 0) {
        int pxBase = x + (int)g_VUPeaks[0];
        for(int py=0; py<h; py++) {
            uint32_t* row = &back_fb[(yL + py) * 480 + pxBase];
            for(int px=0; px<4; px++) row[px] = LCD_COLOR_GREEN;
        }
    }

    // Draw R Channel (Red)
    if(rL > 0) FillRectDMA2D(back_fb, x, yR, rL, h, LCD_COLOR_RED);
    // Draw R Peak (Red, 4px wide, CPU)
    if(g_EnablePeakHold && g_VUPeaks[1] > 0) {
        int pxBase = x + (int)g_VUPeaks[1];
        for(int py=0; py<h; py++) {
            uint32_t* row = &back_fb[(yR + py) * 480 + pxBase];
            for(int px=0; px<4; px++) row[px] = LCD_COLOR_RED;
        }
    }
}

// --- FSK Buffering State Reset ---
// This ensures that when encoding starts, we immediately buffer Sec 0
static uint32_t g_lastGenSec = 0xFFFFFFFF;
static int g_repeatCount = 0;
void FSK_Reset_Buffering_State(void) {
    g_lastGenSec = 0xFFFFFFFF;
    g_repeatCount = 0;
    // Also clear the FIFO to prevent stale audio from previous runs
    g_FSK_FIFO_ReadIdx = 0;
    g_FSK_FIFO_WriteIdx = 0;
}

static void UpdateFSKEncode(void) {
    if (!g_IsEncoding) return;
    
    // Auto-Stop Check
    if (g_EncodeSeconds >= g_EncodeDuration * 60) {
        g_IsEncoding = false;
        addFSKDisplayString("\n### ENCODING COMPLETE\n");
        return;
    }

    // Update real-time clock for UI display
    uint32_t now = HAL_GetTick();
    if (now - g_LastEncodeTick >= 1000) {
        g_LastEncodeTick = now;
        g_EncodeSeconds++;
    }

    // FIFO Management: Buffer ahead
    uint32_t used = (g_FSK_FIFO_WriteIdx >= g_FSK_FIFO_ReadIdx) ? 
                    (g_FSK_FIFO_WriteIdx - g_FSK_FIFO_ReadIdx) :
                    (FSK_FIFO_SIZE - (g_FSK_FIFO_ReadIdx - g_FSK_FIFO_WriteIdx));
    
    // Fill the buffer when we have at least 66000 empty spots in the FIFO
    // A 1-second block at 300 baud is roughly 49600 samples.
    if (used <= (FSK_FIFO_SIZE - 66000)) {
        uint32_t secToGen;
        if (g_lastGenSec == 0xFFFFFFFF) {
            secToGen = 0;
            g_repeatCount = 1;
        } else {
            if (g_repeatCount < 4) {
                secToGen = g_lastGenSec;
                g_repeatCount++;
            } else {
                secToGen = g_lastGenSec + 1;
                g_repeatCount = 1;
            }
        }
        
        if (secToGen < g_EncodeDuration * 60) {
            g_lastGenSec = secToGen;
            
            char dctBuf[48];
            snprintf(dctBuf, sizeof(dctBuf), "DCT0%c_01_aaaaaaaaaa_%04d_%04d\n", g_EncodeSide, (int)secToGen, (int)secToGen);
            addFSKDisplayString(dctBuf);
            
            // 65536 is absolutely required to prevent buffer truncation at 300 baud!
            // Map the colossal 131KB temporary block generator to unused external SDRAM at 0xC0380000
            int16_t* txBuf = (int16_t*)0xC0380000; 
            size_t samplesGenerated = FSK_Modem_GenerateTX(&g_TxModem, dctBuf, txBuf, 65536);
            if (samplesGenerated > 0) {
                for(size_t i=0; i<samplesGenerated; i++) {
                    FSK_FIFO_Push(txBuf[i]);
                }
                printf("FSK: Buffered %d samples for Sec %d\n", (int)samplesGenerated, (int)secToGen);
            }
        }
    }
}

static void drawFSKText(void) {
    BSP_LCD_SetTextColor(LCD_COLOR_DARKGREEN);
    BSP_LCD_SetBackColor(LCD_COLOR_BLACK);
    BSP_LCD_SetFont(&Font12);
    // WAIT for any previous DMA2D operations to finish before CPU writes text
    while (hdma2d.Instance->CR & DMA2D_CR_START);

    BSP_LCD_SetTextColor(LCD_COLOR_GREEN); // Use bright green for high visibility
    BSP_LCD_SetBackColor(LCD_COLOR_BLACK);
    BSP_LCD_SetFont(&Font12);
    
    int startY = UI_VIZ_TOP + 40;
    int endY = UI_VIZ_BOTTOM;
    int lineHeight = 12;
    
    // Split screen logic: FSK Log on left (0-240), Stats on right (240-480)
    int wrapCount = 34; // Adjusted for Font12 (7px wide, 240/7 = 34)

    // Draw Vertical Separator
    uint32_t* back_fb = (uint32_t*)hLtdcHandler.LayerCfg[0].FBStartAdress;
    for(int i=startY; i<endY; i++) back_fb[i*480 + splitX] = 0xFF555555;

    // --- Left Side: FSK Log ---
    static int logicalLineStarts[128]; // Stack safety (static)
    int logicalLineCount = 0;
    int currentLineLen = 0;

    vTaskSuspendAll();
    int textLen = g_FSKTextLen;
    // Snapshot the text for rendering so we can release the scheduler early
    static char textSnap[2048];
    memcpy(textSnap, g_FSKText, textLen);
    xTaskResumeAll();

    logicalLineStarts[0] = 0;
    logicalLineCount = 1;

    for (int i = 0; i < textLen; i++) {
        char c = textSnap[i];
        currentLineLen++;
        if (c == '\n' || currentLineLen >= wrapCount) {
            if (logicalLineCount < 128) logicalLineStarts[logicalLineCount++] = i + 1;
            currentLineLen = 0;
        }
    }
    if (logicalLineCount > 0 && logicalLineStarts[logicalLineCount-1] >= textLen && textLen > 0) logicalLineCount--;

    int y = startY;
    BSP_LCD_SetFont(&Font12);
    
    // 1. Draw Static Header (Current Mapped Track)
    if (g_CurrentMappedRecord[0] != '\0') {
        BSP_LCD_SetTextColor(LCD_COLOR_YELLOW); // Highlights the mapped record
        BSP_LCD_DisplayStringAt(10, y, (uint8_t*)g_CurrentMappedRecord, LEFT_MODE);
        y += lineHeight;
        // Draw a small separator line
        FillRectDMA2D(back_fb, 10, y + 2, splitX - 20, 1, 0xFF555555);
        y += 6;
    }
    
    // 2. Draw Scrolling FSK Log
    BSP_LCD_SetTextColor(LCD_COLOR_GREEN);
    int availableHeight = endY - y;
    int scrollLinesToShow = availableHeight / lineHeight;
    if (scrollLinesToShow > 11) scrollLinesToShow = 11; // Safety limit
    
    int linesToShow = (logicalLineCount > scrollLinesToShow) ? scrollLinesToShow : logicalLineCount;
    int startLineIdx = logicalLineCount - linesToShow;
    
    for (int l = 0; l < linesToShow; l++) {
        int charStart = logicalLineStarts[startLineIdx + l];
        int charEnd = (startLineIdx + l + 1 < logicalLineCount) ? logicalLineStarts[startLineIdx + l + 1] : textLen;
        int len = charEnd - charStart;
        if (len <= 0) continue;
        
        char lineBuf[64];
        if (len > 60) len = 60;
        memcpy(lineBuf, &textSnap[charStart], len);
        lineBuf[len] = '\0';
        
        // Clean line endings
        for(int k=0; k<len; k++) if(lineBuf[k] == '\r' || lineBuf[k] == '\n') lineBuf[k] = '\0';

        if (lineBuf[0] != '\0') {
            BSP_LCD_DisplayStringAt(10, y, (uint8_t*)lineBuf, LEFT_MODE);
        }
        y += lineHeight;
    }

    // --- Right Side: TapeStats or FSK Encode Panel ---
    BSP_LCD_SetTextColor(LCD_COLOR_WHITE);
    y = startY;
    int statsX = splitX + 10;
    
    if (g_ShowFSKEncode) {
        // --- FSK Encode Panel (Optimized Layout) ---
        y = UI_VIZ_TOP + 5; // Start earlier to save space
        
        // Row 1: Baud Rate [ < ] [ BAUD ] [ > ]
        BSP_LCD_SetTextColor(LCD_COLOR_WHITE);
        BSP_LCD_DisplayStringAt(statsX, y + 8, (uint8_t*)"Baud:", LEFT_MODE);
        
        int bx = statsX + 45;
        // Left Arrow
        BSP_LCD_DrawRect(bx, y, 58, 30);
        BSP_LCD_DisplayStringAt(bx + (58 - 7)/2, y + 8, (uint8_t*)"<", LEFT_MODE); bx += 63;
        // Baud Value (Center)
        BSP_LCD_DrawRect(bx, y, 58, 30);
        char bBuf[16]; snprintf(bBuf, sizeof(bBuf), "%d", (int)g_BaudRate);
        BSP_LCD_DisplayStringAt(bx + (58 - strlen(bBuf)*7)/2, y + 8, (uint8_t*)bBuf, LEFT_MODE); bx += 63;
        // Right Arrow
        BSP_LCD_DrawRect(bx, y, 58, 30);
        BSP_LCD_DisplayStringAt(bx + (58 - 7)/2, y + 8, (uint8_t*)">", LEFT_MODE);
        y += 40;
        
        // Row 2: Side Selection [SIDE A] [SIDE B]
        BSP_LCD_SetTextColor(LCD_COLOR_WHITE);
        BSP_LCD_DisplayStringAt(statsX, y + 8, (uint8_t*)"Side:", LEFT_MODE);
        bx = statsX + 45;
        // Side A
        BSP_LCD_SetTextColor(g_EncodeSide == 'A' ? LCD_COLOR_GREEN : 0xFF555555);
        BSP_LCD_DrawRect(bx, y, 58, 30);
        BSP_LCD_DisplayStringAt(bx + 11, y + 8, (uint8_t*)"A", LEFT_MODE); bx += 63;
        // Side B
        BSP_LCD_SetTextColor(g_EncodeSide == 'B' ? LCD_COLOR_GREEN : 0xFF555555);
        BSP_LCD_DrawRect(bx, y, 58, 30);
        BSP_LCD_DisplayStringAt(bx + 11, y + 8, (uint8_t*)"B", LEFT_MODE);
        
        y += 40;
        
        // Duration Buttons
        int durations[] = {45, 90, 120, 240};
        const char* dLabels[] = {"45M", "90M", "120M", "240M"};
        bx = statsX;
        for(int i=0; i<4; i++) {
            bool active = g_IsEncoding && g_EncodeDuration == durations[i];
            BSP_LCD_SetTextColor(active ? LCD_COLOR_YELLOW : (g_IsEncoding ? 0xFF555555 : LCD_COLOR_WHITE));
            BSP_LCD_DrawRect(bx, y, 50, 30);
            BSP_LCD_DisplayStringAt(bx + (50 - strlen(dLabels[i])*7)/2, y + 8, (uint8_t*)dLabels[i], LEFT_MODE);
            bx += 58;
        }
        y += 40;
        
        if (g_IsEncoding) {
            BSP_LCD_SetTextColor(LCD_COLOR_ORANGE);
            char pBuf[32];
            int mins = g_EncodeSeconds / 60;
            int secs = g_EncodeSeconds % 60;
            snprintf(pBuf, sizeof(pBuf), "PROG: %02d:%02d / %d:00", mins, secs, g_EncodeDuration);
            BSP_LCD_DisplayStringAt(statsX, y, (uint8_t*)pBuf, LEFT_MODE);
            y += 15;
            BSP_LCD_SetTextColor(LCD_COLOR_RED);
            BSP_LCD_DrawRect(statsX, y, 190, 25);
            BSP_LCD_DisplayStringAt(statsX + 40, y + 5, (uint8_t*)"STOP ENCODING", LEFT_MODE);
        }

    } else {
        // --- Original TapeStats ---
        char sBuf[64];
        
        int snrInt = (int)g_Modem.lastSNR;
        int snrDec = (int)((g_Modem.lastSNR - snrInt) * 100);
        if(snrDec < 0) snrDec = -snrDec;
        
        // Speed Offset Calculation
        float speedOffset = 0;
        if (g_Modem.lastMeasuredBaud > 0) {
            speedOffset = (g_Modem.lastMeasuredBaud - g_BaudRate) * 100.0f / g_BaudRate;
        }
        int offInt = (int)speedOffset;
        int offDec = (int)((speedOffset - offInt) * 10);
        if(offDec < 0) offDec = -offDec;
        if(offInt < 0) offInt = -offInt;
        char sign = (speedOffset >= 0) ? '+' : '-';

        snprintf(sBuf, sizeof(sBuf), "Baud: %d (%c%d.%d%%) | SNR: %d.%02d", 
                 (int)g_Modem.lastMeasuredBaud, sign, (int)offInt, (int)offDec, (int)snrInt, (int)snrDec);
        BSP_LCD_DisplayStringAt(statsX, y, (uint8_t*)sBuf, LEFT_MODE); y += lineHeight;

        snprintf(sBuf, sizeof(sBuf), "Side: %c | Stops: %d", g_TapeStats.currentSide, g_TapeStats.totalStops);
        BSP_LCD_DisplayStringAt(statsX, y, (uint8_t*)sBuf, LEFT_MODE); y += lineHeight;

        snprintf(sBuf, sizeof(sBuf), "Mode: %s", g_TapeStats.isDctMode ? "DCT" : "Generic");
        BSP_LCD_SetTextColor(g_TapeStats.isDctMode ? LCD_COLOR_CYAN : LCD_COLOR_MAGENTA);
        BSP_LCD_DisplayStringAt(statsX, y, (uint8_t*)sBuf, LEFT_MODE); y += lineHeight + 4;

        BSP_LCD_SetTextColor(LCD_COLOR_WHITE);
        int h = g_TapeStats.lastTotalTime / 3600;
        int m = (g_TapeStats.lastTotalTime % 3600) / 60;
        int s = g_TapeStats.lastTotalTime % 60;
        snprintf(sBuf, sizeof(sBuf), "Total: %d (%02d:%02d:%02d)", g_TapeStats.logLineCount, h, m, s);
        BSP_LCD_DisplayStringAt(statsX, y, (uint8_t*)sBuf, LEFT_MODE); y += lineHeight;
        
        snprintf(sBuf, sizeof(sBuf), "Errors: %d", g_TapeStats.dataErrors);
        BSP_LCD_SetTextColor(g_TapeStats.dataErrors > 0 ? LCD_COLOR_RED : LCD_COLOR_GREEN);
        BSP_LCD_DisplayStringAt(statsX, y, (uint8_t*)sBuf, LEFT_MODE); y += lineHeight + 4;
        
        BSP_LCD_SetTextColor(LCD_COLOR_YELLOW);
        float sideAPerc = (g_TapeStats.sideALineCount > 0) ? ((float)g_TapeStats.sideAErrors * 100.0f / (float)g_TapeStats.sideALineCount) : 0.0f;
        int saInt = (int)sideAPerc; int saDec = (int)((sideAPerc - saInt) * 100); if(saDec < 0) saDec = -saDec;
        snprintf(sBuf, sizeof(sBuf), "Side A: %d/%d (%d.%02d%%)", g_TapeStats.sideAErrors, g_TapeStats.sideALineCount, saInt, saDec);
        BSP_LCD_DisplayStringAt(statsX, y, (uint8_t*)sBuf, LEFT_MODE); y += lineHeight;

        float sideBPerc = (g_TapeStats.sideBLineCount > 0) ? ((float)g_TapeStats.sideBErrors * 100.0f / (float)g_TapeStats.sideBLineCount) : 0.0f;
        int sbInt = (int)sideBPerc; int sbDec = (int)((sideBPerc - sbInt) * 100); if(sbDec < 0) sbDec = -sbDec;
        snprintf(sBuf, sizeof(sBuf), "Side B: %d/%d (%d.%02d%%)", g_TapeStats.sideBErrors, g_TapeStats.sideBLineCount, sbInt, sbDec);
        BSP_LCD_DisplayStringAt(statsX, y, (uint8_t*)sBuf, LEFT_MODE); y += lineHeight + 4;

        BSP_LCD_SetTextColor(LCD_COLOR_CYAN);
        snprintf(sBuf, sizeof(sBuf), "Format Err: L=%d N=%d", g_TapeStats.dataLengthErrors, g_TapeStats.dataErrors);
        BSP_LCD_DisplayStringAt(statsX, y, (uint8_t*)sBuf, LEFT_MODE);
        y += lineHeight;

        // --- Metadata HUD ---
        if (g_HasMetadata) {
            y += 4; 
            BSP_LCD_SetTextColor(LCD_COLOR_ORANGE);
            
            char* filename = strrchr(g_CurrentTrack.sourcePath, '/');
            if (!filename) filename = strrchr(g_CurrentTrack.sourcePath, '\\');
            if (filename) filename++; else filename = g_CurrentTrack.sourcePath;

            char displayBuf[160];
            snprintf(displayBuf, sizeof(displayBuf), "FILE: %s", filename);

            int remain = strlen(displayBuf);
            char* ptr = displayBuf;
            while (remain > 0 && y < 272 - lineHeight) {
                char chunk[32];
                int take = (remain > 22) ? 22 : remain; // Harder wrap for right side
                strncpy(chunk, ptr, take);
                chunk[take] = '\0';
                BSP_LCD_DisplayStringAt(statsX, y, (uint8_t*)chunk, LEFT_MODE);
                y += lineHeight; ptr += take; remain -= take;
        }
    }
    
    // No more global resume here, it's handled inside the logical blocks
    }
}

static void initButtons() {
    buttonCount = 0; int bY = 230, bW = 58, bG = 8;
    // Buttons 0-4
    buttons[buttonCount++] = (Button){10, bY, bW, 30, "SPEC", toggleSpectrum, LCD_COLOR_BLUE, &g_ShowSpectrum};
    buttons[buttonCount++] = (Button){10+(bW+bG), bY, bW, 30, "VU", toggleVU, LCD_COLOR_BLUE, &g_ShowVUMeter};
    buttons[buttonCount++] = (Button){10+(bW+bG)*2, bY, bW, 30, vu_gain_texts[vu_gain_idx], cycleVUGain, LCD_COLOR_MAGENTA, &g_GainAlwaysActive};
    buttons[buttonCount++] = (Button){10+(bW+bG)*3, bY, bW, 30, "WFALL", toggleWaterfall, LCD_COLOR_BLUE, &g_ShowWaterfall};
    buttons[buttonCount++] = (Button){10+(bW+bG)*4, bY, bW, 30, "FSK", toggleFSK, LCD_COLOR_YELLOW, &g_ShowFSK};
    
    // FSKe replacement for SIM when FSK is active
    if (g_ShowFSK) {
        const char* fskLabel = "FSKenc";
        buttons[buttonCount++] = (Button){10+(bW+bG)*5, bY, bW, 30, fskLabel, toggleSim, LCD_COLOR_BROWN, &g_ShowFSKEncode};
    } else {
        buttons[buttonCount++] = (Button){10+(bW+bG)*5, bY, bW, 30, "SIM", toggleSim, LCD_COLOR_BROWN, &g_SimulationMode};
    }
    
    // Button 6
    buttons[buttonCount++] = (Button){10+(bW+bG)*6, bY, bW, 30, "IN:MIC", toggleInput, LCD_COLOR_CYAN, &g_InputLineIn};
}

static void handleTouch() {
    TS_StateTypeDef ts; if(!ts_enabled) return;
    BSP_TS_GetState(&ts);
    if(ts.touchDetected) {
        static uint32_t lastT = 0; if(HAL_GetTick() - lastT < 200) return;
        lastT = HAL_GetTick();
        
        // 1. Header Zone (Top 30px) - Toggle Aesthetics
        if (ts.touchY[0] < UI_VIZ_TOP) {
            g_EnableAesthetics = !g_EnableAesthetics;
            return;
        }
        
        // 2. Visualizer Zone (Middle)
        if (ts.touchY[0] >= UI_VIZ_TOP && ts.touchY[0] < UI_VIZ_BOTTOM) {
            int zone = ts.touchX[0] / 160;
            if (zone == 0) { // Left third
                if (g_ShowFSK) {
                    vTaskSuspendAll();
                    memset(g_FSKText, 0, sizeof(g_FSKText));
                    g_FSKTextLen = 0;
                    memset(&g_TapeStats, 0, sizeof(g_TapeStats));
                    g_TapeStats.currentSide = 'A';
                    g_CurrentLineIdx = 0;
                    xTaskResumeAll();
                } else if (g_ShowSpectrum) {
                    g_SpectrumMode = (g_SpectrumMode + 1) % 4;
                }
            } else if (zone == 1 && !g_ShowFSKEncode) { // Middle third: Toggle Peak Hold (DISABLED if FSKe panel is open)
                g_EnablePeakHold = !g_EnablePeakHold;
                if (g_EnablePeakHold) {
                    memset(g_SpectrumPeaks, 0, sizeof(g_SpectrumPeaks));
                    memset(g_VUPeaks, 0, sizeof(g_VUPeaks));
                }
            } else if (zone == 2 || (zone == 1 && g_ShowFSKEncode)) { // Right + Overlap: FSK Encode Panel Interactions
                if (g_ShowFSK) {
                    if (g_ShowFSKEncode) {
                        int statsX = splitX + 10;
                        int tx = ts.touchX[0];
                        int ty = ts.touchY[0];
                        int ry = ty - (UI_VIZ_TOP + 5); // Relative Y from Panel Start
                        
                        // 1. Baud Rate [ < ] BAUD [ > ] (ry ~ 0-30)
                        if (ry >= 0 && ry <= 30) {
                            if (tx >= statsX + 45 && tx <= statsX + 45 + 58) { // Left arrow
                                g_BaudIdx = (g_BaudIdx + 2) % 3;
                            } else if (tx >= statsX + 45 + 126 && tx <= statsX + 45 + 126 + 58) { // Right arrow
                                g_BaudIdx = (g_BaudIdx + 1) % 3;
                            }
                            g_BaudRate = g_BaudRates[g_BaudIdx];
                            initFSKModems(); // Sync both RX and TX
                        }
                        // 2. Side Selection [SIDE A] [SIDE B] (ry ~ 40-70)
                        else if (ry >= 40 && ry <= 70) {
                            if (tx >= statsX + 45 && tx <= statsX + 45 + 58) g_EncodeSide = 'A';
                            else if (tx >= statsX + 45 + 63 && tx <= statsX + 45 + 63 + 58) g_EncodeSide = 'B';
                        }
                        // 3. Durations (ry ~ 80-150)
                        else if (ry >= 80 && ry <= 150) {
                            int dx = tx - statsX;
                            if (dx >= 0 && dx < 224) {
                                if (g_IsEncoding) {
                                    if (ry >= 115) { // STOP button area (y += 40 from 80 = 120)
                                        g_IsEncoding = false;
                                        addFSKDisplayString("\n### ENCODING STOPPED\n");
                                    }
                                } else {
                                    if (dx < 54) g_EncodeDuration = 45;
                                    else if (dx < 112) g_EncodeDuration = 90;
                                    else if (dx < 170) g_EncodeDuration = 120;
                                    else g_EncodeDuration = 240;
                                    
                                    g_IsEncoding = true;
                                    g_SimulationMode = false; // Disable sim for live view
                                    g_EncodeSeconds = 0;
                                    g_LastEncodeTick = HAL_GetTick();
                                    
                                    // Force Audio Output State
                                    BSP_AUDIO_OUT_SetVolume(100);
                                    BSP_AUDIO_OUT_SetMute(AUDIO_MUTE_OFF);
                                    
                                    // Trigger immediate buffering of Sec 0
                                    extern void FSK_Reset_Buffering_State(void);
                                    FSK_Reset_Buffering_State();
                                    
                                    // Reset RX modem state to prevent stale carrier logs
                                    FSK_Modem_Init(&g_Modem, g_Modem.cfg); 
                                    
                                    char msg[64];
                                    snprintf(msg, sizeof(msg), "\n### STARTING %d MIN DCT (SIDE %c)\n", g_EncodeDuration, g_EncodeSide);
                                    addFSKDisplayString(msg);
                                }
                            }
                        }
                    } else {
                        cycleBaudRate();
                    }
                }
            }
            return;
        }

        // 2. Check Buttons
        for(int i = 0; i < buttonCount; i++) {
            Button* b = &buttons[i];
            if(ts.touchX[0] >= b->x && ts.touchX[0] <= b->x + b->w && ts.touchY[0] >= b->y && ts.touchY[0] <= b->y + b->h) {
                if(b->onClick) b->onClick(); 
                break;
            }
        }
    }
}

static void toggleWaterfall() { g_ShowWaterfall = !g_ShowWaterfall; if(g_ShowWaterfall) { g_ShowSpectrum = false; if(g_ShowFSK) { g_ShowFSK = false; g_ShowFSKEncode = false; } } initButtons(); }
static void toggleSpectrum() { g_ShowSpectrum = !g_ShowSpectrum; if(g_ShowSpectrum) { g_ShowWaterfall = false; if(g_ShowFSK) { g_ShowFSK = false; g_ShowFSKEncode = false; } } initButtons(); }
static void toggleVU() { g_ShowVUMeter = !g_ShowVUMeter; }
static void toggleSim() { 
    if (g_ShowFSK) {
        g_ShowFSKEncode = !g_ShowFSKEncode;
    } else {
        g_SimulationMode = !g_SimulationMode; 
        fftIdx = 0; 
    }
    initButtons();
}

static void toggleFSK() {
    g_ShowFSK = !g_ShowFSK;
    if (g_ShowFSK) {
        g_ShowWaterfall = false;
        g_ShowSpectrum = false;
        // Reset text for fresh start
        memset(g_FSKText, 0, sizeof(g_FSKText));
        g_FSKTextLen = 0;
        memset(&g_TapeStats, 0, sizeof(g_TapeStats));
        g_TapeStats.currentSide = 'A';
        g_CurrentLineIdx = 0;
        initFSKModems();
        FSK_FIFO_Reset();
    } else {
        g_ShowFSKEncode = false; // Hide panel when FSK is off
        g_IsEncoding = false;    // Stop any active encoding
    }
    initButtons(); // Refresh button labels (SIM -> FSKe)
}

extern int16_t playback_buffer[AUDIO_BUFFER_SIZE];

static void Visualizer_InitAudio() {
    static bool first_init = true;
    if (!first_init) {
        BSP_AUDIO_IN_Stop(CODEC_PDWN_SW);
        BSP_AUDIO_OUT_Stop(CODEC_PDWN_SW);
    }
    first_init = false;
    
    uint16_t dev = g_InputLineIn ? INPUT_DEVICE_INPUT_LINE_1 : INPUT_DEVICE_DIGITAL_MICROPHONE_2;
    
    printf("[AUDIO] Calling BSP_AUDIO_IN_OUT_Init...\r\n");
    if (BSP_AUDIO_IN_OUT_Init(dev, OUTPUT_DEVICE_HEADPHONE, SAMPLING_FREQ, 16, 2) == AUDIO_OK) {
        printf("[AUDIO] Init OK. Starting streams...\r\n");
        
        memset(playback_buffer, 0, sizeof(playback_buffer));
        BSP_AUDIO_OUT_SetOutputMode(OUTPUT_DEVICE_HEADPHONE);
        BSP_AUDIO_OUT_SetVolume(90);
        BSP_AUDIO_OUT_SetMute(AUDIO_MUTE_OFF);
        // BSP_AUDIO_OUT_Play size is in BYTES.
        BSP_AUDIO_OUT_Play((uint16_t*)playback_buffer, AUDIO_BUFFER_SIZE * 2);
        
        BSP_AUDIO_IN_Record((uint16_t*)audio_buffer, AUDIO_BUFFER_SIZE);
        g_AudioGain = g_InputLineIn ? g_LineInGain : g_MicGain;
    } else {
        printf("[AUDIO] Init FAILED!\r\n");
    }
}

static void toggleInput() {
    g_InputLineIn = !g_InputLineIn;
    Visualizer_InitAudio();
    buttons[6].text = g_InputLineIn ? "IN:LINE" : "IN:MIC";
}

static void cycleVUGain() {
    vu_gain_idx = (vu_gain_idx + 1) % 5;
    g_VUGain = vu_gain_vals[vu_gain_idx];
    buttons[2].text = vu_gain_texts[vu_gain_idx];
}
static uint32_t Color565ToARGB(uint16_t rgb565) {
    uint32_t r = (rgb565 >> 11) & 0x1F; uint32_t g = (rgb565 >> 5) & 0x3F; uint32_t b = rgb565 & 0x1F;
    return 0xFF000000 | (((r*255)/31) << 16) | (((g*255)/63) << 8) | ((b*255)/31);
}
static void initFSKModems(void) {
    if (g_BaudRate < 300.0f) g_BaudRate = 1200.0f; // Safety
    
    // Standard PC decoders (like minimodem) expect specific Bell standards for different speeds!
    if (g_BaudRate == 300.0f) {
        // Bell 103 (300 baud standard)
        g_Modem.cfg.freqMark = 1270.0f;
        g_Modem.cfg.freqSpace = 1070.0f;
    } else {
        // Bell 202 (1200 baud standard)
        g_Modem.cfg.freqMark = 1200.0f;
        g_Modem.cfg.freqSpace = 2200.0f;
    }
    g_Modem.cfg.baudRate = g_BaudRate;
    g_Modem.cfg.sampleRate = 12000.0f;
    g_Modem.cfg.noiseFloor = 0.2f;
    g_Modem.cfg.invert = false;
    FSK_Modem_Init(&g_Modem, g_Modem.cfg);
    
    FSK_Config txCfg = g_Modem.cfg;
    txCfg.sampleRate = 48000.0f;
    FSK_Modem_Init(&g_TxModem, txCfg);
    
    printf("FSK: Modems Init to %d baud (RX=12k, TX=48k)\r\n", (int)g_BaudRate);
}

static void cycleBaudRate(void) {
    g_BaudIdx = (g_BaudIdx + 1) % 3; // Fixed: 3 entries, not 5
    g_BaudRate = g_BaudRates[g_BaudIdx];
    initFSKModems();
    
    // Clear text area and print new baud
    g_FSKTextLen = 0;
    g_FSKText[0] = '\0';
    
    char msg[32];
    snprintf(msg, sizeof(msg), "[BAUD: %d]\n", (int)g_BaudRate);
    addFSKDisplayString(msg);
}

static void FillRectDMA2D(uint32_t* fb, int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 480) w = 480 - x;
    if (y + h > 272) h = 272 - y;
    if (w <= 0 || h <= 0) return;

    // WAIT for previous operation at the START
    while (hdma2d.Instance->CR & DMA2D_CR_START);

    hdma2d.Instance->OPFCCR = DMA2D_OUTPUT_ARGB8888;
    hdma2d.Instance->OOR = 480 - w;
    hdma2d.Instance->OMAR = (uint32_t)&fb[y * 480 + x];
    hdma2d.Instance->NLR = (w << 16) | h;
    hdma2d.Instance->OCOLR = color;
    
    // Start Register-to-Memory fill
    hdma2d.Instance->CR = DMA2D_R2M | DMA2D_CR_START;
}

static void CopyBlockDMA2D(uint32_t* src, uint32_t* dst, int width, int height) {
    while (hdma2d.Instance->CR & DMA2D_CR_START);
    hdma2d.Instance->CR = DMA2D_M2M;
    hdma2d.Instance->OPFCCR = DMA2D_OUTPUT_ARGB8888;
    hdma2d.Instance->FGPFCCR = DMA2D_INPUT_ARGB8888;
    hdma2d.Instance->FGMAR = (uint32_t)src;
    hdma2d.Instance->OMAR = (uint32_t)dst;
    hdma2d.Instance->FGOR = 0;
    hdma2d.Instance->OOR = 480 - width;
    hdma2d.Instance->NLR = (width << 16) | height;
    hdma2d.Instance->CR |= DMA2D_CR_START;
}
