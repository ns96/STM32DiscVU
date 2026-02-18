#ifndef VISUALIZER_APP_H
#define VISUALIZER_APP_H

#include <stdint.h>
#include "main.h"

// --- Audio Configuration ---
#include <stdbool.h>

#define FFT_SIZE 512
#define RX_DECIMATION 4

// Audio Config
#define AUDIO_BUFFER_SIZE  4096
#define SAMPLING_FREQ      48000

// UI Constants
#define LCD_FB_START_ADDRESS ((uint32_t)0xC0000000)
#define LCD_FB_OFFSET        (1024 * 1024) // 1MB offset for back buffer

// Colors
#define UI_COLOR_BG           LCD_COLOR_BLACK
#define UI_COLOR_FRAME        LCD_COLOR_DARKGRAY
#define UI_COLOR_HEADER_BG    LCD_COLOR_BLUE
#define UI_COLOR_HEADER_TEXT  LCD_COLOR_WHITE
#define UI_COLOR_BUTTON_IDLE  LCD_COLOR_GRAY
#define UI_COLOR_BUTTON_ACTIVE LCD_COLOR_GREEN
#define UI_COLOR_BUTTON_TEXT  LCD_COLOR_BLACK

// Button Structure
typedef struct {
    int x;
    int y;
    int w;
    int h;
    const char* text;  // Changed from label to text to match .c usage
    void (*onClick)(void);
    uint32_t color;    // Changed from int/uint16_t to uint32_t for ARGB
    bool* toggleMode;  // Changed to pointer to bool to controls global state
} Button;

// Global State
extern bool g_ShowVUMeter;
extern bool g_ShowWaterfall;
extern bool g_SimulationMode; // Changed from int to bool
extern bool g_InputLineIn;

// Audio context needed for switching inputs
extern int16_t audio_buffer[AUDIO_BUFFER_SIZE];

// API
void Visualizer_Init(void);
void Visualizer_ProcessAudio(int16_t* inBuf, uint32_t samples);
void Visualizer_Update(void);

#endif // VISUALIZER_APP_H
