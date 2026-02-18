#ifndef SIMPLE_FFT_H
#define SIMPLE_FFT_H

#include <stdint.h>

void SimpleFFT_Init(void);
void SimpleFFT_Compute(float* vReal, float* vImag, uint16_t samples);
void SimpleFFT_Windowing(float* vData, uint16_t samples);
void SimpleFFT_Windowing_Fast(float* vData, uint16_t samples);
void SimpleFFT_ComplexToMagnitude(float* vReal, float* vImag, uint16_t samples);

#endif // SIMPLE_FFT_H
