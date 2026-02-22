/*
 * FSKModem.c
 * Unified FSK Decoder and Encoder
 */

#include "FSKModem.h"
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

static int16_t sineLUT[256];
static bool lutInit = false;

static void InitSineLUT() {
    if (lutInit) return;
    for(int i=0; i<256; i++) {
        sineLUT[i] = (int16_t)(sinf(2.0f * M_PI * i / 256.0f) * 30000);
    }
    lutInit = true;
}

static void Filter_Init(FSK_SlidingFilter* f, float freq, float sampleRate, int windowSize) {
    memset(f, 0, sizeof(FSK_SlidingFilter));
    f->size = (windowSize > FSK_MAX_FILTER_SIZE) ? FSK_MAX_FILTER_SIZE : windowSize;
    if (f->size < 2) f->size = 2;
    f->inc = 2.0f * M_PI * freq / sampleRate;
}

static void Filter_Process(FSK_SlidingFilter* f, float in) {
    float loI = cosf(f->phase);
    float loQ = sinf(f->phase);

    f->phase += f->inc;
    if (f->phase > 2.0f * M_PI) f->phase -= 2.0f * M_PI;

    float vi = in * loI;
    float vq = in * loQ;

    f->sumI = f->sumI - f->iBuf[f->ptr] + vi;
    f->sumQ = f->sumQ - f->qBuf[f->ptr] + vq;

    f->iBuf[f->ptr] = vi;
    f->qBuf[f->ptr] = vq;

    f->ptr++;
    if (f->ptr >= f->size) f->ptr = 0;
}

float Filter_GetMag(FSK_SlidingFilter* f) {
    return sqrtf(f->sumI * f->sumI + f->sumQ * f->sumQ);
}

void FSK_Modem_Init(FSK_Modem* m, FSK_Config cfg) {
    memset(m, 0, sizeof(FSK_Modem));
    m->cfg = cfg;
    if (m->cfg.baudCorrection <= 0.0f) m->cfg.baudCorrection = 1.0f;
    m->samplesPerBit = cfg.sampleRate / (cfg.baudRate * m->cfg.baudCorrection);
    
    Filter_Init(&m->filterMark, cfg.invert ? cfg.freqSpace : cfg.freqMark, cfg.sampleRate, (int)m->samplesPerBit);
    Filter_Init(&m->filterSpace, cfg.invert ? cfg.freqMark : cfg.freqSpace, cfg.sampleRate, (int)m->samplesPerBit);
    
    m->lastBitMark = true;
    
    // TX Init
    m->txPhase = 0.0f;
    m->txSampleAccum = 0.0f;
}

char FSK_Modem_ProcessRX(FSK_Modem* m, float sample) {
    m->dcOffset = (sample * 0.01f) + (m->dcOffset * 0.99f);
    sample -= m->dcOffset;

    Filter_Process(&m->filterMark, sample);
    Filter_Process(&m->filterSpace, sample);

    float mMag = Filter_GetMag(&m->filterMark);
    float sMag = Filter_GetMag(&m->filterSpace);

    bool isMark = mMag > sMag;
    float total = mMag + sMag;
    float conf = isMark ? mMag / (sMag + 0.0001f) : sMag / (mMag + 0.0001f);
    m->lastSNR = conf;

    if (total > m->cfg.noiseFloor && conf > 0.5f) {
        if (m->carrierCounter < (int)m->samplesPerBit * 2) m->carrierCounter++;
    } else {
        if (m->carrierCounter > -(int)m->samplesPerBit * 2) m->carrierCounter--;
    }

    if (!m->carrier && m->carrierCounter > (int)m->samplesPerBit) {
        m->carrier = true;
        m->state = 0;
        m->framingErrorCount = 0;
    } else if (m->carrier && m->carrierCounter < 0) {
        m->carrier = false;
        m->state = 0;
    }

    if (!m->carrier) {
        m->sampleCount = 0;
        m->lastEdgeSample = 0;
        return 0;
    }

    m->sampleCount++;
    if (isMark != m->lastBitMark) {
        if (m->lastEdgeSample > 0) {
            uint32_t delta = m->sampleCount - m->lastEdgeSample;
            float bits = (float)delta / m->samplesPerBit;
            int k = (int)(bits + 0.5f);
            if (k >= 1 && k <= 12) {
                float instBaud = ((float)k * m->cfg.sampleRate / (float)delta) / m->cfg.baudCorrection;
                if (m->lastMeasuredBaud < 1.0f) m->lastMeasuredBaud = instBaud;
                else m->lastMeasuredBaud = (m->lastMeasuredBaud * 0.98f) + (instBaud * 0.02f);
            }
        }
        m->lastEdgeSample = m->sampleCount;
    }

    if (isMark != m->lastBitMark) {
        if (m->state == 0 && !isMark) {
            m->timer = m->samplesPerBit * 0.5f;
            m->state = 1;
        }
    }
    m->lastBitMark = isMark;

    char decodedChar = 0;
    switch (m->state) {
        case 1: // VERIFY
            m->timer -= 1.0f;
            if (m->timer <= 0) {
                if (!isMark) {
                    m->state = 2;
                    m->timer = m->samplesPerBit;
                    m->bitIndex = 0;
                    m->currentByte = 0;
                } else m->state = 0;
            }
            break;
        case 2: // DATA
            m->timer -= 1.0f;
            if (m->timer <= 0) {
                int bit = isMark ? 1 : 0;
                if (m->bitIndex < 8) {
                    m->currentByte |= (bit << m->bitIndex);
                    m->bitIndex++;
                    m->timer = m->samplesPerBit;
                } else {
                    if (isMark) {
                        m->framingErrorCount = 0;
                        int ascii = m->currentByte & 0xFF;
                        if (ascii >= 32 || ascii == 10 || ascii == 13 || ascii == 9) {
                            decodedChar = (char)ascii;
                        }
                    } else {
                        m->framingErrorCount++;
                        if (m->framingErrorCount > 6) {
                            m->carrier = false;
                            m->carrierCounter = -((int)m->samplesPerBit * 2);
                        }
                    }
                    m->state = 0;
                }
            }
            break;
    }
    return decodedChar;
}

int16_t FSK_GetSineSample(uint8_t phaseIdx) {
    InitSineLUT();
    return sineLUT[phaseIdx];
}

static void FSK_Modem_Emit(FSK_Modem* m, float freq, float samples, int16_t** buffer, size_t* remaining) {
    InitSineLUT();
    
    // Calculate precise phase increment per sample
    float inc = 256.0f * freq / m->cfg.sampleRate;
    
    // We must accumulate fractional samples across bit boundaries!
    // If samples=160.5, we emit 160 this time, and carry 0.5 over to the next bit.
    m->txSampleAccum += samples;
    int nToEmit = (int)(m->txSampleAccum);
    m->txSampleAccum -= (float)nToEmit;

    for (int i = 0; i < nToEmit && *remaining > 0; i++) {
        // Safe wrap the float phase to prevent precision loss at large numbers
        while (m->txPhase >= 256.0f) {
            m->txPhase -= 256.0f;
        }
        
        // Convert to integer lookup index
        uint8_t idx = (uint8_t)m->txPhase;
        **buffer = sineLUT[idx];
        
        (*buffer)++;
        (*remaining)--;
        
        // Increment precise phase
        m->txPhase += inc;
    }
}

size_t FSK_Modem_GenerateTX(FSK_Modem* m, const char* text, int16_t* buffer, size_t max_samples) {
    if (!text || !buffer || max_samples == 0) return 0;
    
    int16_t* p = buffer;
    size_t rem = max_samples;
    float samplesPerBit = m->cfg.sampleRate / m->cfg.baudRate;

    while (*text && rem > 0) {
        uint8_t byte = (uint8_t)(*text++);
        
        // Start Bit (Space)
        FSK_Modem_Emit(m, m->cfg.freqSpace, samplesPerBit, &p, &rem);
        
        // 8 Data Bits (LSB first)
        for (int i = 0; i < 8; i++) {
            float f = (byte & (1 << i)) ? m->cfg.freqMark : m->cfg.freqSpace;
            FSK_Modem_Emit(m, f, samplesPerBit, &p, &rem);
        }
        
        // Stop Bit (Mark)
        FSK_Modem_Emit(m, m->cfg.freqMark, samplesPerBit, &p, &rem);
    }
    
    return max_samples - rem;
}
