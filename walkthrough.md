# ESP32-C3 Portable Translator — Walkthrough & Documentation

The Raspberry Pi kiosk architecture has been converted into an ultra-low latency, battery-friendly **ESP32-C3 thin client** handheld translator. Heavy ML models (Moonshine STT, Gemma 4 Translation, and moonshine-voice TTS) run on the host computer, while the ESP32-C3 handles audio capture, playback, display, buttons, time sync, and a local Web Dashboard over Wi-Fi.

---

## 1. Project Directory Structure

```
translatorpi/
├── backend/
│   ├── bridge/               # High-Performance Go WebSocket Bridge
│   │   ├── go.mod            # Go module definition
│   │   ├── main.go           # CLI flags & HTTP server startup
│   │   ├── server.go         # WebSocket connection & session management
│   │   ├── protocol.go       # Client & Server JSON frame structures
│   │   ├── audio.go          # PCM16 ↔ Float32 and 22 kHz → 16 kHz resampler
│   │   ├── pipeline.go       # STT → Translate → TTS orchestration
│   │   └── pyclient.go       # HTTP client to Python ML microservice
│   ├── pyworkers/
│   │   └── ml_service.py     # FastAPI ML microservice (Moonshine STT, Gemma, TTS)
│   ├── esp32_ws.py           # Standalone Python WebSocket bridge (all-in-one alternative)
│   └── server.py             # Existing model cache & Web UI backend
├── firmware/                 # Native ESP-IDF v5.5.x (FreeRTOS)
│   ├── CMakeLists.txt        # Root ESP-IDF CMake project with -DDEVICE_NUMBER support
│   ├── sdkconfig.defaults    # Pre-tuned settings for C3 (unicore, buffers, websocket)
│   └── main/
│       ├── CMakeLists.txt    # Component registration (httpd, mdns, i2s, wifi, sntp)
│       ├── config.h          # Hardware profiles (breadboard_c3 & dogbot_c3), pinouts
│       ├── main.c            # Firmware entrypoint (app_main)
│       ├── mdns_manager.c/.h # mDNS hostname discovery (http://translator-N.local)
│       ├── time_sync.c / .h  # SNTP background clock + browser epoch sync
│       ├── web_server.c / .h # Embedded HTTP Web Dashboard with touch controls
│       ├── i2s_audio.c / .h  # Full-duplex STD I2S (shared BCLK/WS on I2S_NUM_0)
│       ├── oled_display.c/.h # SSD1306 0.91" (128x32) I2C OLED driver & framebuffer
│       ├── button.c / .h     # Multi-click detector (single, double, triple, PTT hold)
│       ├── led.c / .h        # Visual status LED indicator
│       ├── wifi_manager.c/.h # Wi-Fi station manager with auto-reconnect
│       ├── ws_client.c / .h  # WebSocket client with binary/JSON routing
│       └── state_machine.c/.h# FreeRTOS queue-driven system state machine
├── tests/
│   └── test_ws_pipeline.py   # Test client simulating ESP32 audio stream & verifying pipeline
├── start_bridge.bat          # Windows CMD launch script
└── start_bridge.ps1          # Windows PowerShell launch script
```

---

## 2. Hardware Profiles (`firmware/main/config.h`)

You can switch between hardware profiles by toggling the `#define` in [`firmware/main/config.h`](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/translatorpi/firmware/main/config.h):

### Profile 1: `breadboard_c3` (Default Handheld Unit)
- **I2S Bus** (`I2S_NUM_0` Full-Duplex @ 16,000 Hz Mono):
  - **BCLK**: `GPIO 4` (Shared)
  - **WS / LRCK**: `GPIO 5` (Shared)
  - **DIN** (INMP441 Mic SD): `GPIO 6`
  - **DOUT** (MAX98357A Amp DIN): `GPIO 7`
- **OLED Display** (SSD1306 0.91" 128×32 I2C):
  - **SDA**: `GPIO 0`
  - **SCL**: `GPIO 1`
- **Buttons**:
  - **PTT Button**: `GPIO 10` (Hold to speak, release to send)
  - **Language Swap Button**: `GPIO 3` (Click to swap `EN ↔ ZH`)
- **Status LED**: `GPIO 8`
- **mDNS Hostname**: `http://translator-N.local`

### Profile 2: `dogbot_c3` (Dogbot v1 Quadruped Platform)
- **I2S Audio**: Speaker `GPIO 6, 7`, Amp control `GPIO 3`, Mic `GPIO 2`
- **Single Top Button**: `GPIO 0` (or `GPIO 9`):
  - **Double Click**: Toggle recording ("say something")
  - **Triple Click**: Swap language pair
  - **Long Press / Hold**: Standard Push-To-Talk
- **Display**: SPI LCD/OLED (`MOSI: GPIO 4`, `CLK: GPIO 5`, `DC: GPIO 10`)
- **LED Strip**: `GPIO 8`
- ** Hostname**: `http://dogbot-N.local`

---

## 3. Onboard Web UI & mDNS Access

Each device runs an embedded web server accessible directly by its mDNS name on your local network:

- **URL**: `http://translator-1.local` (or `http://translator-3.local` depending on `DEVICE_NUMBER`)
- **Features**:
  1. **Status Cards**: Real-time IP, Wi-Fi RSSI, Free Heap, and Uptime stats.
  2. **Time & Epoch Sync**: Live RTC clock display with a 1-click **Sync Browser Time** button (and auto-sync on page load).
  3. **Language Switcher**: Active language pair pill with instant **Swap** button and source/destination selectors (`en`, `zh`, `es`, `ja`, `ar`, `ko`).
  4. **Web Push-To-Talk**: Test mic recording directly from your phone or browser.
  5. **Remote Reboot**: 1-click device restart button.

---

## 4. How to Run

### Step 1: Start the Host Backend (Windows / Mac / Linux)

#### Option A: High-Performance Go Bridge (Recommended)
Run the Windows startup script:
```powershell
.\start_bridge.bat
# Or using PowerShell:
.\start_bridge.ps1
```
This automatically starts:
1. Python ML microservice on `http://127.0.0.1:9379`
2. Go WebSocket Bridge on `ws://0.0.0.0:8765`

#### Option B: Standalone Python WebSocket Bridge
```bash
python backend/esp32_ws.py
```

### Step 2: Test Host Pipeline (Optional)
Run the simulated audio test script:
```bash
python tests/test_ws_pipeline.py
```

### Step 3: Flash ESP32-C3 Firmware

Open your ESP-IDF environment (e.g. `idf_v5.5.4_Powershell` on Windows):

```powershell
cd firmware

# Set Wi-Fi credentials and Host IP in main/config.h:
# #define CONFIG_WIFI_SSID "YourSSID"
# #define CONFIG_WIFI_PASSWORD "YourPass"
# #define CONFIG_WS_SERVER_URI "ws://192.168.1.50:8765"

idf.py set-target esp32c3

# Build with a specific device number (e.g. node #3 -> http://translator-3.local):
idf.py -DDEVICE_NUMBER=3 build

# Flash and open serial monitor:
idf.py -p COMx flash monitor
```
