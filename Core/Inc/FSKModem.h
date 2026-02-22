/*
 * FSKModem.h
 * Unified FSK Decoder and Encoder
 */

#ifndef FSK_MODEM_H
#define FSK_MODEM_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#define FSK_MAX_FILTER_SIZE 100 

typedef struct {
    float freqMark;
    float freqSpace;
    float baudRate;
    float sampleRate;
    float noiseFloor;
    float baudCorrection; 
    bool invert;
} FSK_Config;

typedef struct {
    float iBuf[FSK_MAX_FILTER_SIZE];
    float qBuf[FSK_MAX_FILTER_SIZE];
    int ptr;
    int size;
    float sumI;
    float sumQ;
    float phase;
    float inc;
} FSK_SlidingFilter;

typedef struct {
    FSK_Config cfg;
    FSK_SlidingFilter filterMark;
    FSK_SlidingFilter filterSpace;
    
    // RX State
    int state; // 0=IDLE, 1=VERIFY, 2=DATA
    float timer;
    int bitIndex;
    int currentByte;
    bool carrier;
    int carrierCounter;
    bool lastBitMark;
    int framingErrorCount;
    float dcOffset;
    float lastSNR;
    float samplesPerBit;
    
    // TX State
    float txPhase;
    float txSampleAccum;
    
    // Diagnostics
    uint32_t sampleCount;
    uint32_t lastEdgeSample;
    float lastMeasuredBaud;
} FSK_Modem;

void FSK_Modem_Init(FSK_Modem* m, FSK_Config cfg);
char FSK_Modem_ProcessRX(FSK_Modem* m, float sample);
size_t FSK_Modem_GenerateTX(FSK_Modem* m, const char* text, int16_t* buffer, size_t max_samples);
float Filter_GetMag(FSK_SlidingFilter* f);
int16_t FSK_GetSineSample(uint8_t phaseIdx);

#endif // FSK_MODEM_H
