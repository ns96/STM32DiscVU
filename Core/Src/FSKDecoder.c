/*
 * FSKDecoder.c
 * Ported from JMinimodem / ModemDSP (C++) to C
 */

#include "FSKDecoder.h"
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

static void Filter_Init(FSK_SlidingFilter* f, float freq, float sampleRate, int windowSize) {
    memset(f, 0, sizeof(FSK_SlidingFilter));
    f->size = (windowSize > FSK_MAX_FILTER_SIZE) ? FSK_MAX_FILTER_SIZE : windowSize;
    if (f->size < 2) f->size = 2; // Safety
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

void FSK_Init(FSK_Modem* m, FSK_Config cfg) {
    memset(m, 0, sizeof(FSK_Modem));
    m->cfg = cfg;
    if (m->cfg.baudCorrection <= 0.0f) m->cfg.baudCorrection = 1.0f; // Safety
    m->samplesPerBit = cfg.sampleRate / (cfg.baudRate * m->cfg.baudCorrection);
    
    Filter_Init(&m->filterMark, cfg.invert ? cfg.freqSpace : cfg.freqMark, cfg.sampleRate, (int)m->samplesPerBit);
    Filter_Init(&m->filterSpace, cfg.invert ? cfg.freqMark : cfg.freqSpace, cfg.sampleRate, (int)m->samplesPerBit);
    
    m->lastBitMark = true;
}

char FSK_Process(FSK_Modem* m, float sample) {
    // DC Bias Removal
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

    // Carrier Squelch Logic (Match Reference verbatim)
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

    // High-precision Edge Tracking
    m->sampleCount++;
    if (isMark != m->lastBitMark) {
        if (m->lastEdgeSample > 0) {
            uint32_t delta = m->sampleCount - m->lastEdgeSample;
            float bits = (float)delta / m->samplesPerBit;
            int k = (int)(bits + 0.5f);
            if (k >= 1 && k <= 12) {
                // Apply correction factor to measured baud
                float instBaud = ((float)k * m->cfg.sampleRate / (float)delta) / m->cfg.baudCorrection;
                if (m->lastMeasuredBaud < 1.0f) m->lastMeasuredBaud = instBaud;
                else m->lastMeasuredBaud = (m->lastMeasuredBaud * 0.98f) + (instBaud * 0.02f);
            }
        }
        m->lastEdgeSample = m->sampleCount;
    }

    // Soft PLL
    if (isMark != m->lastBitMark) {
        if (m->state == 0 && !isMark) { // IDLE -> Start Bit
            m->timer = m->samplesPerBit * 0.5f;
            m->state = 1; // VERIFY
        }
    }
    m->lastBitMark = isMark;

    char decodedChar = 0;
    switch (m->state) {
        case 1: // VERIFY
            m->timer -= 1.0f;
            if (m->timer <= 0) {
                if (!isMark) {
                    m->state = 2; // DATA
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
                    if (isMark) { // Stop bit OK
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
