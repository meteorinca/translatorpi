# EasyRun Guide — Running the Go + Python Translator Bridge

This guide explains how to start the high-performance host bridge that connects your **ESP32-C3 / Dogbot** to the AI models (Moonshine STT, Gemma translation, and TTS).

---

## 🚀 1-Click Startup (Recommended)

### On Windows (CMD or File Explorer)
Double-click or run:
```cmd
start_bridge.bat
```

### On Windows (PowerShell)
```powershell
.\start_bridge.ps1
```

### What happens automatically:
1. **Python ML Microservice** launches on `http://127.0.0.1:9379`:
   - Loads Moonshine Multilingual STT
   - Connects to Gemma LLM translation
   - Loads Moonshine Voice TTS engine
2. **Go WebSocket Bridge** launches on `ws://0.0.0.0:8765`:
   - Connects to the Dogbot / ESP32-C3 over Wi-Fi
   - Streams live 16 kHz audio packets directly to the pipeline
   - Sends real-time transcripts, translations, and synthesized audio chunks back to the Dogbot

---

## 🏗️ System Architecture & Data Flow

```
+---------------------+           Wi-Fi WebSocket           +------------------------+
|      DOGBOT C3      | <=================================> |    Go WS Bridge        |
|  (ST7789 Eyes, Mic, |         ws://HOST_IP:8765           |  (backend/bridge)      |
|   Speaker, WS2812)  |                                     |  Port: 8765            |
+---------------------+                                     +-----------+------------+
                                                                        | HTTP REST
                                                                        v
                                                            +------------------------+
                                                            |  Python ML Service     |
                                                            | (FastAPI / Uvicorn)    |
                                                            | Port: 9379             |
                                                            | - Moonshine STT        |
                                                            | - Gemma Translation    |
                                                            | - Moonshine TTS        |
                                                            +------------------------+
```

---

## 🛠️ Running Services Manually (Optional)

If you prefer running the components in separate terminal windows:

### Terminal 1: Start Python ML Microservice
```powershell
# Activate the virtual environment
.\venv\Scripts\Activate.ps1

# Start the ML FastAPI service
python backend/pyworkers/ml_service.py
```
*Health check:* Open `http://127.0.0.1:9379/health` in your browser.

### Terminal 2: Start Go WebSocket Bridge
```powershell
cd backend/bridge
go run .
```
*Port:* Listens on `ws://0.0.0.0:8765`.

---

## 🧪 Testing Without Hardware

You can simulate an ESP32 client sending speech audio using the automated test script:

```powershell
python tests/test_ws_pipeline.py
```

### Expected Output:
```
Connecting to ws://localhost:8765 ...
Connected! Waiting for welcome frame...
< Received: {"type": "status", "state": "ready", ...}
> Sent: {'type': 'hello', 'device': 'dogbot_c3', 'src': 'en', 'dst': 'zh'}
> Sent: record_start
> Sent: record_stop
< JSON Response: {'type': 'status', 'state': 'recording'}
< JSON Response: {'type': 'status', 'state': 'transcribing'}
< JSON Response: {'type': 'done'}
Test completed successfully!
```

---

## ⚙️ Configuration & Network Settings

- **Host IP Address**: Check with `ipconfig` (e.g. `10.0.0.45`).
- **Dogbot Config**: Defined in [`firmware/main/config.h`](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/translatorpi/firmware/main/config.h):
  ```c
  #define CONFIG_WS_SERVER_URI "ws://10.0.0.45:8765"
  ```
- **Wi-Fi Credentials**: In [`firmware/main/secrets.h`](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/translatorpi/firmware/main/secrets.h).
