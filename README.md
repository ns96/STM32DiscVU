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
- **Wi-Fi Shield (Optional)**: [RobotShop ESP8266 Wi-Fi Shield (RB-Mfk-14)](https://www.robotshop.com/) (Arduino Uno form-factor shield stacked directly onto the DISCO Arduino headers, communicating over USART6).
  - *Jumper Configuration*: Ensure the jumpers are set to the default positions connecting `ESP_TX` / `ESP_RX` to digital pins **0 and 1** (hardware UART TX/RX on the Discovery Arduino headers).
- **Audio Input**: Onboard MEMS microphones or Line-In via the 3.5mm jack.
- **Storage**: MicroSD card (FAT32) for metadata database, mapping files, and Wi-Fi configuration.

## 📡 Wi-Fi Configuration & HTTP API

When an ESP8266 Wi-Fi shield is installed, DiscVU automatically launches an embedded HTTP REST API & telemetry server on port 80.

### Wi-Fi Configuration (`/WIFI.TXT`)

Create a `/WIFI.TXT` file in the root of the microSD card with your network credentials:

```ini
SSID=YourNetworkSSID
PASS=YourNetworkPassword
```

*(If `/WIFI.TXT` is omitted, the firmware falls back to default credentials configured in `WiFiApp.h`)*.

### HTTP Endpoints

| Endpoint | Method | Description |
| :--- | :--- | :--- |
| `/info` | `GET` | **CassetteFlow Handshake**: Returns `DECODE <rx_text>\n` when in DCT mode or `PASS THROUGH\n` otherwise. Case-insensitive (`/INFO`). |
| `/raw` or `/rawdct` | `GET` | **Live Streaming Stream**: Open-ended, real-time HTTP stream sending decoded FSK lines as they are demodulated. Sends `### NOCARRIER ###\n` every **1 second** when idle. |
| `/api/status` | `GET` | **Telemetry (JSON)**: Returns real-time system metrics (CPU load, measured baud, speed error %, DCT mode, IP address, stats, and FSK log). |
| `/` | `GET` | **Status Page / Mode Selector**: HTML status dashboard. Supports query parameters `?mode=decode` (enable DCT) and `?mode=pass` (pass-through). |
| `/dct` | `GET` | Enables DCT mode and returns `DCT Mode Enabled\n`. |
| `/mp3db`, `/tapedb`, `/create`, `/start`, `/stop`, `/play` | `GET` | CassetteFlow tape deck control and status endpoints. |
| `/api/cmd` | `POST` | Executes JSON commands (e.g. `{"cmd": "reset"}`, `{"cmd": "dct_mode", "val": 1}`). |
| `/api/tx` | `POST` | Transmits raw text via FSK modulation. |

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
- `Core/Src/WiFiApp.c`: Embedded HTTP REST server, `/raw` streaming, and CassetteFlow integration.
- `Core/Src/ESP8266.c`: Low-level AT command driver for ESP8266 Wi-Fi shield over USART6.
- `Core/Src/FSKDecoder.c`: Bell 202 FSK modem implementation.
- `Core/Src/SimpleFFT.c`: Optimized FFT calculation.
- `Core/Src/DatabaseManager.c`: Binary and text-based metadata lookup system.

## 📄 License

This project is licensed under the MIT License - see the LICENSE file for details.
