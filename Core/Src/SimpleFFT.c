#include "SimpleFFT.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FFT_MAX_SAMPLES 512
static float g_SinTable[FFT_MAX_SAMPLES];
static float g_HammingTable[FFT_MAX_SAMPLES];

void SimpleFFT_Init(void) {
    for (int i = 0; i < FFT_MAX_SAMPLES; i++) {
        g_SinTable[i] = sinf(2.0f * M_PI * i / FFT_MAX_SAMPLES);
        g_HammingTable[i] = 0.54f - 0.46f * cosf(2.0f * M_PI * i / (FFT_MAX_SAMPLES - 1));
    }
}

// Radix-2 FFT implementation with LUT optimization
void SimpleFFT_Compute(float* vReal, float* vImag, uint16_t samples) {
    uint16_t i, j, k, n1, n2;
    float c, s, t1, t2;

    // Bit-reversal permutation (unchanged as it's already fast)
    j = 0;
    n2 = samples / 2;
    for (i = 1; i < samples - 1; i++) {
        n1 = n2;
        while (j >= n1) {
            j -= n1;
            n1 /= 2;
        }
        j += n1;
        if (i < j) {
            t1 = vReal[i]; vReal[i] = vReal[j]; vReal[j] = t1;
            t1 = vImag[i]; vImag[i] = vImag[j]; vImag[j] = t1;
        }
    }

    // Butterfly computations with LUT
    n1 = 0;
    n2 = 1;
    int log2_samples = (int)log2(samples);
    for (i = 0; i < log2_samples; i++) {
        n1 = n2;
        n2 = n2 + n2;
        int step = FFT_MAX_SAMPLES / n2;
        
        for (j = 0; j < n1; j++) {
            // Use Sine LUT. cos(x) = sin(x + PI/2)
            // Index logic: a = j * (FFT_MAX_SAMPLES / n2)
            // c = cos(-2PI * j / n2) = cos(2PI * j / n2) = sin(2PI * j / n2 + PI/2)
            // s = sin(-2PI * j / n2) = -sin(2PI * j / n2)
            int lutIdx = (j * step) % FFT_MAX_SAMPLES;
            int cosIdx = (lutIdx + (FFT_MAX_SAMPLES/4)) % FFT_MAX_SAMPLES;
            
            c = g_SinTable[cosIdx];
            s = -g_SinTable[lutIdx];
            
            for (k = j; k < samples; k = k + n2) {
                t1 = c * vReal[k + n1] - s * vImag[k + n1];
                t2 = s * vReal[k + n1] + c * vImag[k + n1];
                vReal[k + n1] = vReal[k] - t1;
                vImag[k + n1] = vImag[k] - t2;
                vReal[k] = vReal[k] + t1;
                vImag[k] = vImag[k] + t2;
            }
        }
    }
}

void SimpleFFT_Windowing(float* vData, uint16_t samples) {
    SimpleFFT_Windowing_Fast(vData, samples);
}

void SimpleFFT_Windowing_Fast(float* vData, uint16_t samples) {
    if (samples > FFT_MAX_SAMPLES) samples = FFT_MAX_SAMPLES;
    for (uint16_t i = 0; i < samples; i++) {
        vData[i] *= g_HammingTable[i];
    }
}

void SimpleFFT_ComplexToMagnitude(float* vReal, float* vImag, uint16_t samples) {
    for (uint16_t i = 0; i < samples; i++) {
        // sqrtf is faster than sqrt on Cortex-M7
        vReal[i] = sqrtf(vReal[i] * vReal[i] + vImag[i] * vImag[i]);
    }
}
