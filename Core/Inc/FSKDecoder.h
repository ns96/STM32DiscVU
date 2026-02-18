/*
 * FSKDecoder.h
 * Ported from JMinimodem / ModemDSP (C++) to C
 */

#ifndef FSK_DECODER_H
#define FSK_DECODER_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#define FSK_MAX_FILTER_SIZE 100 // Sufficient for 48kHz / 1200 baud -> 40 samples

typedef struct {
    float freqMark;
    float freqSpace;
    float baudRate;
    float sampleRate;
    float noiseFloor;
    float baudCorrection; // Scale factor (e.g. 1.05 for 5% speed increase)
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
    
    // Tape Speed Diagnostic (Transition-based)
    uint32_t sampleCount;
    uint32_t lastEdgeSample;
    float lastMeasuredBaud;
} FSK_Modem;

void FSK_Init(FSK_Modem* modem, FSK_Config cfg);
char FSK_Process(FSK_Modem* modem, float sample);
float Filter_GetMag(FSK_SlidingFilter* f);

#endif // FSK_DECODER_H
