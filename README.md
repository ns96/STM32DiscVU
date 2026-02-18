# DiscVU - Advanced Audio Visualizer for STM32F746G-DISCO

DiscVU is a simple audio visualizer and FSK data decoder for the STM32F746-Discovery development board. It features multiple spectrum analyzer modes, real-time waterfall displays, and FSK data decoding with metadata lookup to perform realtime testing of vintage audio gear such as Reel to Reel and Cassette Players.

![Project Overview](https://img.shields.io/badge/Platform-STM32F746-blue.svg)
![Language-C](https://img.shields.io/badge/Language-C-green.svg)

## 🚀 Features

- **62-Band Classic Spectrogram**: Smooth, heatmap-colored bars (Heat565 palette).
- **1/3 Octave Stereo Split**: Dual 31-band displays for Left and Right channels.
- **Classic LED Spectrogram**: Vintage hardware-style segmented bars with Green-Yellow-Red logic.
- **Real-Time Waterfall**: Scrolling frequency history with customizable gain and color mapping.
- **Logarithmic VU Meters**: Accurate RMS power meters with peak hold and adjustable sensitivity (x0.5 to x8).
- **FSK Decoder (Bell 202)**: Real-time decoding of tape-based data streams (1200/2200Hz).
- **Metadata Mapping**: Automatic lookup of "Now Playing" information from an SD card database (`audiodb.txt`).
- **Low Latency Graphics**: DMA2D-accelerated rendering with flicker-free double buffering on the 480x272 LCD.

## 🛠 Hardware Required

This project is specifically designed for the **STM32F746G-Discovery** kit.

- **Purchase from Mouser**: [STM32F746G-DISCO](https://mou.sr/3MRNQe1)
- **Audio Input**: Onboard MEMS microphones or Line-In via the 3.5mm jack.
- **Storage**: MicroSD card (FAT32) for metadata database and mapping files.

## 💻 Software Setup & Compilation

### Prerequisites

1. **STM32CubeIDE**: The recommended IDE for development and debugging. [Download here](https://www.st.com/en/development-tools/stm32cubeide.html).
2. **STM32CubeMX**: (Optional) For peripheral configuration and code generation.

### Installation

1. **Clone the Repository**:
   
   ```bash
   git clone https://github.com/YourUsername/STM32DiscVu.git
   cd STM32DiscVu
   ```
2. **Open in STM32CubeIDE**:
   - Select `File` -> `Import...`
   - Choose `Existing Projects into Workspace` under `General`.
   - Select the project root directory and click `Finish`.
3. **Generate Code (Optional)**:
   - Open `STM32DiscVu.ioc` with STM32CubeMX to modify clock or peripheral settings.
   - Click `Generate Code` to update the HAL configuration.
4. **Build and Flash**:
   - Connect the board via the ST-LINK USB port.
   - Right-click the project -> `Build Project`.
   - Click the `Run` (Play) button to flash the firmware.

## 📁 Project Structure

- `Core/Src/VisualizerApp.c`: Main application logic and rendering engine.
- `Core/Src/FSKDecoder.c`: Bell 202 FSK modem implementation.
- `Core/Src/SimpleFFT.c`: Optimized FFT calculation.
- `Core/Src/DatabaseManager.c`: Binary and text-based metadata lookup system.

## 📄 License

This project is licensed under the MIT License - see the LICENSE file for details.
