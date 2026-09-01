 # ESP32-C3 Portable Translator — Implementation Plan (Go + ESP-IDF)

> **Goal**: Replace the Raspberry Pi kiosk with an ESP32-C3 thin client that streams audio over WiFi to a host PC. The host runs STT (Moonshine), translation, and TTS (moonshine-voice). The backend bridge is written in **Go**; the device firmware is written in **ESP-IDF C** (FreeRTOS).
we'll be using idf_v5.5.4_Powershell on Windows to build (no need to build I'll do that myself)


Fixes to the implement_esp32.md:
1. **Hardware fix**: ESP32-C3 has **only one I2S peripheral** (`I2S_NUM_0`). Mic and amp **share BCLK and WS** in full-duplex mode.
2. **Host resampling**: `moonshine-voice` outputs 22,050 Hz. The Go bridge resamples to **16,000 Hz** before streaming to the device, so the ESP32 runs a single clock domain.
3. **Go backend**: All I/O, networking, protocol state machines, and audio buffering are written in Go. Python is used **only** for ML inference inside a minimal microservice.
4. **ESP-IDF firmware**: Native FreeRTOS tasks, `esp_websocket_client`, and the ESP-IDF 5.x `driver/i2s_std.h` driver. No Arduino abstractions.

---

## 1. System Architecture

```
┌─────────────────────────────────────────────┐         Wi-Fi          ┌─────────────────────────────────────────────┐
│           ESP32-C3 Handheld Unit            │◄──────────────────────►│           Host PC (Linux/macOS/Win)         │
│                                             │   ws://host:8765       │                                             │
│  ┌─────────────┐      ┌───────────────┐    │                        │  ┌─────────────────┐    ┌─────────────────┐  │
│  │ INMP441 Mic ├─I2S──┤               │    │  Raw PCM16 @ 16kHz    │  │  Go Bridge      │    │  Python ML Svc  │  │
│  └─────────────┘  RX  │   I2S_NUM_0   │◄───┼────────────────────────┼──┤  (port 8765)    │◄───┤  (port 9379)    │  │
│                        │  Full-Duplex  │    │                        │  │                 │    │                 │  │
│  ┌─────────────┐  TX  │  Shared BCLK  │◄───┼────────────────────────┼──┤  • Protocol     │    │  • Moonshine STT│  │
│  │MAX98357A Amp├─I2S──┤  Shared WS    │    │  Raw PCM16 @ 16kHz    │  │  • Buffering    │    │  • Translation  │  │
│  └─────────────┘      └───────────────┘    │                        │  │  • Resample     │    │  • Moonshine TTS│  │
│  ┌─────────────┐                            │   JSON Status/Control  │  │  • Pipeline     │    └─────────────────┘  │
│  │  PTT Button │────────────────────────────┼───────────────────────►│  └─────────────────┘                         │
│  └─────────────┘                            │                        └─────────────────────────────────────────────┘
│  ┌─────────────┐
│  │ WS2812B LED │
│  └─────────────┘
└─────────────────────────────────────────────┘
```

**Division of Labor**

| Task | Location | Technology |
|:---|:---|:---|
| Audio capture / playback | ESP32-C3 | ESP-IDF I2S STD driver (full-duplex) |
| WiFi / WebSocket client | ESP32-C3 | `esp_websocket_client` + FreeRTOS tasks |
| Button debounce / state machine | ESP32-C3 | FreeRTOS task + GPIO |
| Status LED | ESP32-C3 | WS2812B (RMT driver) or GPIO LED |
| WebSocket gateway | Host PC | **Go** (`gorilla/websocket`) |
| Audio format conversion | Host PC | **Go** (pcm16 ↔ float32, resampling) |
| STT / TTS / Translation | Host PC | Python microservice (reuses existing `server.py` engines) |

---

## 2. Hardware Bill of Materials & Pinout

### 2.1 Components

| Part | Role | Notes |
|:---|:---|:---|
| ESP32-C3 SuperMini / DevKitM-1 | MCU + Wi-Fi | 160 MHz RISC-V, 400 KB SRAM |
| INMP441 | I2S MEMS mic | 3.3 V, L/R pin tied to **GND** (left channel) |
| MAX98357A | I2S Class-D amp | 3–5 V VIN, SD pin pulled **high** |
| 4Ω 3 W speaker | Output | 28–40 mm |
| Tactile button × 2 | PTT + Language swap | Active-low, internal pull-up |
| WS2812B (1×) or plain LED | Status | Optional; GPIO8 if NeoPixel |
| SSD1306 0.91" 128×32 OLED | Display | Optional I²C |
| 3.7 V LiPo + TP4056 | Power | 1000–1800 mAh |

### 2.2 Pin Allocation (Corrected for Single I2S)

> **Critical**: The ESP32-C3 has **one I2S controller** (`I2S_NUM_0`). TX (speaker) and RX (mic) share the **bit clock** and **word select** lines.

| Function | GPIO | Direction | Notes |
|:---|:---:|:---:|:---|
| **I2S BCLK** | **GPIO 4** | Out | Shared by mic + amp |
| **I2S WS** (LRC) | **GPIO 5** | Out | Shared by mic + amp |
| **I2S DIN** (mic data) | **GPIO 6** | In | INMP441 `SD` |
| **I2S DOUT** (amp data) | **GPIO 7** | Out | MAX98357A `DIN` |
| **PTT Button** | **GPIO 10** | In | Active-low. **Avoid GPIO 9** (strapping pin). |
| **Swap Button** | **GPIO 3** | In | Active-low |
| **Status LED** | **GPIO 8** | Out | WS2812B data. **Strapping pin** — do not pull low at boot. |
| **I2C SDA** (OLED) | **GPIO 0** | Bidir | Optional |
| **I2C SCL** (OLED) | **GPIO 1** | Out | Optional |

> **Strapping Pin Warning**: GPIO 2, 8, and 9 are strapping pins on the C3. If GPIO 9 is held **low** at reset, the chip enters UART download mode and will not boot your app. **Do not use GPIO 9 for the PTT button** unless you are certain the user cannot press it during power-on.

---

## 3. Communication Protocol

A **single persistent WebSocket** on `ws://<host>:8765/`. Binary frames carry raw audio; text frames carry JSON metadata.

### 3.1 Device → Host (ESP32 → Go Bridge)

| Frame | Meaning |
|:---|:---|
| `{"type":"hello","device":"esp32c3","src":"en","dst":"zh"}` | Handshake + language pair |
| `{"type":"record_start"}` | PTT pressed |
| `{"type":"audio","format":"pcm16","rate":16000,"channels":1,"len":<N>}` | Header for next binary frame |
| `[binary: N bytes of int16 LE PCM]` | 100 ms chunk (1600 samples = 3200 bytes) |
| `{"type":"record_stop"}` | PTT released |
| `{"type":"swap"}` | Swap src/dst languages |
| `{"type":"ping"}` | Keep-alive |

### 3.2 Host → Device (Go Bridge → ESP32)

| Frame | Meaning |
|:---|:---|
| `{"type":"status","state":"recording\|transcribing\|translating\|synthesizing\|speaking\|ready\|error","message":"…"}` | OLED/LED driver |
| `{"type":"stt","text":"…"}` | Recognized source text |
| `{"type":"translation","text":"…"}` | Translated text |
| `{"type":"tts_audio","len":<N>,"rate":16000}` | Header before binary playback |
| `[binary: N bytes of int16 LE PCM]` | TTS audio chunks |
| `{"type":"done"}` | Round-trip complete |
| `{"type":"error","message":"…"}` | Failure |

---

## 4. Host Backend (Go)

All new code is Go. Python is restricted to a single microservice that wraps the existing Moonshine STT, moonshine-voice TTS, and translation engine.

### 4.1 Project Layout

```
backend/
├── bridge/              # Go module (go.mod: module translator/bridge)
│   ├── main.go
│   ├── server.go        # WebSocket acceptor, client registry
│   ├── protocol.go      # JSON message structs
│   ├── audio.go         # pcm16↔float32, 22→16 kHz resample
│   ├── pipeline.go      # STT → Translate → TTS orchestration
│   └── pyclient.go      # HTTP client to localhost:9379
├── pyworkers/
│   └── ml_service.py    # FastAPI wrapper around server.py engines
└── server.py            # EXISTING — unchanged, serves web UI on :3000
```

### 4.2 Go Protocol Types (`protocol.go`)

```go
package main

import "encoding/json"

type ClientMsg struct {
	Type string `json:"type"`
	Src  string `json:"src,omitempty"`
	Dst  string `json:"dst,omitempty"`
	Lane int    `json:"lane,omitempty"`
	Len  int    `json:"len,omitempty"`
}

type ServerMsg struct {
	Type        string `json:"type"`
	State       string `json:"state,omitempty"`
	Text        string `json:"text,omitempty"`
	Translation string `json:"translation,omitempty"`
	Len         int    `json:"len,omitempty"`
	Rate        int    `json:"rate,omitempty"`
	Message     string `json:"message,omitempty"`
}

func sendJSON(conn *websocket.Conn, v ServerMsg) error {
	b, _ := json.Marshal(v)
	return conn.WriteMessage(websocket.TextMessage, b)
}
```

### 4.3 Go Audio Utilities (`audio.go`)

```go
package main

import (
	"encoding/binary"
	"math"
)

// pcm16ToFloat32 converts little-endian int16 bytes to float32 [-1, 1].
func pcm16ToFloat32(pcm []byte) []float32 {
	n := len(pcm) / 2
	out := make([]float32, n)
	for i := 0; i < n; i++ {
		v := int16(binary.LittleEndian.Uint16(pcm[i*2:]))
		out[i] = float32(v) / 32768.0
	}
	return out
}

// float32ToPCM16 converts float32 [-1, 1] to little-endian int16 bytes.
func float32ToPCM16(samples []float32) []byte {
	out := make([]byte, len(samples)*2)
	for i, s := range samples {
		v := int16(math.Max(-1.0, math.Min(1.0, float64(s))) * 32767.0)
		binary.LittleEndian.PutUint16(out[i*2:], uint16(v))
	}
	return out
}

// resample22to16 linearly resamples 22050 Hz → 16000 Hz.
// moonshine-voice outputs 22050 Hz; the ESP32 I2S bus runs at 16000 Hz.
func resample22to16(input []float32) []float32 {
	ratio := 22050.0 / 16000.0
	outLen := int(float64(len(input)) / ratio)
	out := make([]float32, outLen)
	for i := 0; i < outLen; i++ {
		pos := float64(i) * ratio
		idx := int(pos)
		frac := pos - float64(idx)
		if idx+1 < len(input) {
			out[i] = input[idx]*(1-float32(frac)) + input[idx+1]*float32(frac)
		} else {
			out[i] = input[idx]
		}
	}
	return out
}
```

### 4.4 Go Pipeline (`pipeline.go`)

The Go bridge buffers the complete utterance, then calls the Python ML service via HTTP on `localhost:9379`.

```go
package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"io"
	"net/http"
	"time"
)

const mlBase = "http://127.0.0.1:9379"

func runPipeline(audioPCM []byte, src, dst string, conn *websocket.Conn) {
	// 1. STT
	sendJSON(conn, ServerMsg{Type: "status", State: "transcribing"})
	floatAudio := pcm16ToFloat32(audioPCM)
	text, err := callSTT(floatAudio, src)
	if err != nil {
		sendJSON(conn, ServerMsg{Type: "error", Message: "stt: " + err.Error()})
		return
	}
	sendJSON(conn, ServerMsg{Type: "stt", Text: text})
	if text == "" {
		sendJSON(conn, ServerMsg{Type: "done"})
		return
	}

	// 2. Translate
	sendJSON(conn, ServerMsg{Type: "status", State: "translating"})
	translation, err := callTranslate(text, src, dst)
	if err != nil {
		sendJSON(conn, ServerMsg{Type: "error", Message: "translate: " + err.Error()})
		return
	}
	sendJSON(conn, ServerMsg{Type: "translation", Text: translation})

	// 3. TTS
	sendJSON(conn, ServerMsg{Type: "status", State: "synthesizing"})
	ttsPCM, err := callTTS(translation, dst)
	if err != nil {
		sendJSON(conn, ServerMsg{Type: "error", Message: "tts: " + err.Error()})
		return
	}

	// 4. Stream back to ESP32 (already resampled to 16000 by Python service)
	sendJSON(conn, ServerMsg{Type: "tts_audio", Len: len(ttsPCM), Rate: 16000})
	conn.WriteMessage(websocket.BinaryMessage, ttsPCM)
	sendJSON(conn, ServerMsg{Type: "done"})
	sendJSON(conn, ServerMsg{Type: "status", State: "ready"})
}

func callSTT(audio []float32, lang string) (string, error) {
	buf := new(bytes.Buffer)
	for _, s := range audio {
		binary.Write(buf, binary.LittleEndian, s)
	}
	req, _ := http.NewRequest("POST", mlBase+"/stt?lang="+lang, buf)
	req.Header.Set("Content-Type", "application/octet-stream")
	resp, err := http.DefaultClient.Do(req)
	if err != nil { return "", err }
	defer resp.Body.Close()
	var r struct{ Text string `json:"text"` }
	json.NewDecoder(resp.Body).Decode(&r)
	return r.Text, nil
}

func callTranslate(text, src, dst string) (string, error) {
	resp, err := http.PostForm(mlBase+"/translate",
		map[string][]string{"text": {text}, "src": {src}, "dst": {dst}})
	if err != nil { return "", err }
	defer resp.Body.Close()
	var r struct{ Translation string `json:"translation"` }
	json.NewDecoder(resp.Body).Decode(&r)
	return r.Translation, nil
}

func callTTS(text, lang string) ([]byte, error) {
	resp, err := http.PostForm(mlBase+"/tts",
		map[string][]string{"text": {text}, "lang": {lang}})
	if err != nil { return nil, err }
	defer resp.Body.Close()
	return io.ReadAll(resp.Body)
}
```

### 4.5 Go WebSocket Server (`server.go`)

```go
package main

import (
	"log"
	"net/http"
	"sync"

	"github.com/gorilla/websocket"
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool { return true },
}

type clientState struct {
	src      string
	dst      string
	recording bool
	audioBuf []byte
	mu       sync.Mutex
}

func handleWS(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil { return }
	defer conn.Close()

	state := &clientState{src: "en", dst: "zh"}
	log.Printf("[bridge] client connected from %s", conn.RemoteAddr())

	for {
		mt, msg, err := conn.ReadMessage()
		if err != nil {
			log.Printf("[bridge] client disconnected: %v", err)
			return
		}

		if mt == websocket.TextMessage {
			var cm ClientMsg
			if json.Unmarshal(msg, &cm) != nil { continue }

			switch cm.Type {
			case "hello":
				if cm.Src != "" { state.src = cm.Src }
				if cm.Dst != "" { state.dst = cm.Dst }
				sendJSON(conn, ServerMsg{Type: "status", State: "ready"})
			case "swap":
				state.src, state.dst = state.dst, state.src
				sendJSON(conn, ServerMsg{Type: "status", State: "ready", Message: state.src + " -> " + state.dst})
			case "record_start":
				state.mu.Lock()
				state.recording = true
				state.audioBuf = state.audioBuf[:0]
				state.mu.Unlock()
				sendJSON(conn, ServerMsg{Type: "status", State: "recording"})
			case "record_stop":
				state.mu.Lock()
				state.recording = false
				buf := make([]byte, len(state.audioBuf))
				copy(buf, state.audioBuf)
				state.mu.Unlock()

				if len(buf) < 3200 { // < 100 ms
					sendJSON(conn, ServerMsg{Type: "error", Message: "audio too short"})
					continue
				}
				go runPipeline(buf, state.src, state.dst, conn)
			case "ping":
				sendJSON(conn, ServerMsg{Type: "status", State: "pong"})
			}
		} else if mt == websocket.BinaryMessage {
			state.mu.Lock()
			if state.recording {
				state.audioBuf = append(state.audioBuf, msg...)
			}
			state.mu.Unlock()
		}
	}
}

func main() {
	http.HandleFunc("/", handleWS)
	log.Println("[bridge] listening on ws://0.0.0.0:8765")
	log.Fatal(http.ListenAndServe(":8765", nil))
}
```

### 4.6 Python ML Microservice (`pyworkers/ml_service.py`)

This is the **only** Python process the Go bridge talks to. It reuses the existing model loaders from `server.py` but speaks efficient raw binary over localhost HTTP.

```python
"""ML inference microservice. Run this alongside server.py."""
import asyncio
import json
import numpy as np
import uvicorn
from fastapi import FastAPI, Query
from fastapi.responses import Response, PlainTextResponse

# Reuse existing model helpers (run from backend/ directory)
from server import get_stt_recognizer, get_tts_engine

app = FastAPI()

@app.post("/stt")
async def stt(audio: bytes, lang: str = "en"):
    """Accept raw float32 little-endian binary. Return JSON transcript."""
    float32 = np.frombuffer(audio, dtype=np.float32)
    recognizer = get_stt_recognizer(lang)
    transcript = recognizer.transcribe_without_streaming(float32, 16000)
    text = " ".join([l.text for l in transcript.lines]).strip()
    return {"text": text}

@app.post("/translate")
async def translate(text: str = "", src: str = "en", dst: str = "zh"):
    """Plug in your offline engine here (Argos, Google Offline, or LiteRT proxy)."""
    # Example LiteRT proxy call:
    import urllib.request
    prompt = f"Translate from {src} to {dst}. Return only the translation.\n\n{text}"
    payload = json.dumps({
        "model": "gemma4-e2b",
        "messages": [{"role": "user", "content": prompt}]
    }).encode()
    req = urllib.request.Request(
        "http://localhost:9379/v1/chat/completions",
        data=payload, headers={"Content-Type": "application/json"}, method="POST"
    )
    try:
        resp = urllib.request.urlopen(req, timeout=30).read()
        res = json.loads(resp)
        translated = res["choices"][0]["message"]["content"].strip()
    except Exception as e:
        translated = text  # fallback
    return {"translation": translated}

@app.post("/tts")
async def tts(text: str = "", lang: str = "zh"):
    """Synthesize speech. Returns raw 16 kHz pcm16 binary."""
    engine = get_tts_engine(lang)
    audio, sr = engine.synthesize(text)

    # Resample to 16000 Hz if the engine outputs 22050 Hz
    if sr != 16000:
        # Requires scipy or librosa in your venv:
        import librosa
        audio = librosa.resample(np.asarray(audio, dtype=np.float32),
                                 orig_sr=sr, target_sr=16000)
        sr = 16000

    pcm16 = (np.clip(np.asarray(audio, dtype=np.float32), -1.0, 1.0)
             * 32767.0).astype(np.int16).tobytes()
    return Response(content=pcm16, media_type="application/octet-stream")

if __name__ == "__main__":
    uvicorn.run(app, host="127.0.0.1", port=9379)
```

> **Why this split?** Go handles 10,000 concurrent connections without breaking a sweat. Python handles one inference at a time per model (due to GIL). The Go bridge can manage multiple ESP32 clients, timeout slow requests, and retry disconnects while the Python workers stay focused solely on tensor math.

---

## 5. ESP32-C3 Firmware (ESP-IDF)

Native ESP-IDF 5.x project. No Arduino. Uses FreeRTOS tasks for concurrency.

### 5.1 Project Layout

```
firmware/
├── CMakeLists.txt
├── sdkconfig.defaults
└── main/
    ├── CMakeLists.txt
    ├── main.c
    ├── wifi_manager.c / .h
    ├── ws_client.c    / .h
    ├── i2s_audio.c    / .h
    ├── button.c       / .h
    ├── state_machine.c/ .h
    ├── protocol.c     / .h
    └── led.c          / .h   (WS2812B or simple GPIO)
```

### 5.2 I2S Full-Duplex Configuration (`i2s_audio.c`)

This is the **critical fix** missing from DeepSeek and Claude. Both TX and RX channels are created on `I2S_NUM_0` and share BCLK/WS.

```c
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define I2S_BCLK_GPIO     GPIO_NUM_4
#define I2S_WS_GPIO       GPIO_NUM_5
#define I2S_DIN_GPIO      GPIO_NUM_6   // INMP441 SD
#define I2S_DOUT_GPIO     GPIO_NUM_7   // MAX98357A DIN

#define SAMPLE_RATE       16000
#define CHUNK_SAMPLES     1600          // 100 ms @ 16 kHz
#define CHUNK_BYTES       (CHUNK_SAMPLES * sizeof(int16_t))

static const char *TAG = "i2s";
static i2s_chan_handle_t i2s_tx_chan = NULL;
static i2s_chan_handle_t i2s_rx_chan = NULL;

void i2s_audio_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx_chan, &i2s_rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_GPIO,
            .ws   = I2S_WS_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din  = I2S_DIN_GPIO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    // INMP441 with L/R tied to GND transmits on the LEFT channel slot.
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx_chan, &std_cfg));

    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx_chan));

    ESP_LOGI(TAG, "I2S full-duplex initialized @ %d Hz mono", SAMPLE_RATE);
}

size_t i2s_audio_read(int16_t *buf, size_t samples, TickType_t timeout)
{
    size_t bytes_read = 0;
    i2s_channel_read(i2s_rx_chan, buf, samples * sizeof(int16_t), &bytes_read, timeout);
    return bytes_read / sizeof(int16_t);
}

size_t i2s_audio_write(const int16_t *buf, size_t samples, TickType_t timeout)
{
    size_t bytes_written = 0;
    i2s_channel_write(i2s_tx_chan, buf, samples * sizeof(int16_t), &bytes_written, timeout);
    return bytes_written / sizeof(int16_t);
}
```

### 5.3 WebSocket Client (`ws_client.c`)

Uses `esp_websocket_client`. Runs in its own task, posts received events to the state machine queue.

```c
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "cJSON.h"

#define WS_URI  "ws://192.168.1.50:8765"

static const char *TAG = "ws";
static esp_websocket_client_handle_t ws_client = NULL;

static void ws_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        // Send hello with default language pair
        esp_websocket_client_send_text(ws_client,
            "{\"type\":\"hello\",\"device\":\"esp32c3\",\"src\":\"en\",\"dst\":\"zh\"}", 0, portMAX_DELAY);
        state_machine_post_event(EVENT_WS_CONNECTED);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected, retrying...");
        state_machine_post_event(EVENT_WS_DISCONNECTED);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == WS_TRANSPORT_OPCODES_TEXT) {
            cJSON *root = cJSON_ParseWithLength(data->data_ptr, data->data_len);
            state_machine_post_json(root);  // transfers ownership
        } else if (data->op_code == WS_TRANSPORT_OPCODES_BINARY) {
            state_machine_post_audio(data->data_ptr, data->data_len);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "error");
        break;
    }
}

void ws_client_init(void)
{
    esp_websocket_client_config_t cfg = {
        .uri = WS_URI,
        .keep_alive_enable = true,
        .reconnect_timeout_ms = 2000,
    };
    ws_client = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_websocket_client_start(ws_client);
}

void ws_send_text(const char *json)
{
    if (esp_websocket_client_is_connected(ws_client)) {
        esp_websocket_client_send_text(ws_client, json, strlen(json), portMAX_DELAY);
    }
}

void ws_send_audio(const int16_t *samples, size_t len)
{
    if (esp_websocket_client_is_connected(ws_client)) {
        // Send JSON header first
        char hdr[128];
        snprintf(hdr, sizeof(hdr),
            "{\"type\":\"audio\",\"format\":\"pcm16\",\"rate\":16000,\"channels\":1,\"len\":%u}",
            (unsigned)(len * sizeof(int16_t)));
        esp_websocket_client_send_text(ws_client, hdr, strlen(hdr), portMAX_DELAY);

        // Send binary payload
        esp_websocket_client_send_bin(ws_client, (const char *)samples, len * sizeof(int16_t), portMAX_DELAY);
    }
}
```

### 5.4 State Machine & Tasks (`state_machine.c`)

FreeRTOS queue-based state machine. Three tasks run continuously:

1. **`audio_rx_task`** — reads I2S mic every 100 ms. If state == `RECORDING`, pushes to a ring buffer and calls `ws_send_audio()`.
2. **`audio_tx_task`** — waits on a ring buffer filled by WebSocket binary frames. Writes to I2S speaker.
3. **`button_task`** — polls GPIO every 20 ms, debounces, posts `EVENT_PTT_PRESS` / `EVENT_PTT_RELEASE` / `EVENT_SWAP`.
4. **`state_task`** — main state machine. Receives events from all other tasks.

```c
typedef enum {
    STATE_IDLE,
    STATE_RECORDING,
    STATE_WAITING,
    STATE_SPEAKING,
} system_state_t;

typedef enum {
    EVENT_PTT_PRESS,
    EVENT_PTT_RELEASE,
    EVENT_SWAP,
    EVENT_WS_CONNECTED,
    EVENT_WS_DISCONNECTED,
    EVENT_WS_STT,
    EVENT_WS_TRANSLATION,
    EVENT_WS_TTS_START,
    EVENT_WS_TTS_CHUNK,
    EVENT_WS_TTS_END,
    EVENT_WS_ERROR,
} event_type_t;

typedef struct {
    event_type_t type;
    char *text;      // owned, malloc'd, free after use
    uint8_t *audio;  // owned
    size_t audio_len;
} system_event_t;

static system_state_t g_state = STATE_IDLE;
static char g_src[4] = "en";
static char g_dst[4] = "zh";

static void state_task(void *arg)
{
    system_event_t ev;
    while (1) {
        if (xQueueReceive(state_queue, &ev, portMAX_DELAY)) {
            switch (g_state) {
            case STATE_IDLE:
                if (ev.type == EVENT_PTT_PRESS) {
                    g_state = STATE_RECORDING;
                    led_set(LED_RECORDING);  // solid red
                    ws_send_text("{\"type\":\"record_start\"}");
                } else if (ev.type == EVENT_SWAP) {
                    // swap languages
                    char tmp[4]; strcpy(tmp, g_src); strcpy(g_src, g_dst); strcpy(g_dst, tmp);
                    led_blink(LED_SWAP);  // brief yellow blink
                }
                break;

            case STATE_RECORDING:
                if (ev.type == EVENT_PTT_RELEASE) {
                    g_state = STATE_WAITING;
                    led_set(LED_WAITING);  // yellow
                    ws_send_text("{\"type\":\"record_stop\"}");
                }
                break;

            case STATE_WAITING:
                if (ev.type == EVENT_WS_STT) {
                    // optionally display source text on OLED
                } else if (ev.type == EVENT_WS_TRANSLATION) {
                    // display translated text
                } else if (ev.type == EVENT_WS_TTS_START) {
                    g_state = STATE_SPEAKING;
                    led_set(LED_SPEAKING);  // cyan
                } else if (ev.type == EVENT_WS_ERROR || ev.type == EVENT_WS_TTS_END) {
                    g_state = STATE_IDLE;
                    led_set(LED_IDLE);  // green heartbeat
                }
                break;

            case STATE_SPEAKING:
                if (ev.type == EVENT_WS_TTS_END) {
                    g_state = STATE_IDLE;
                    led_set(LED_IDLE);
                }
                break;
            }
            // cleanup event payload
            if (ev.text) free(ev.text);
            if (ev.audio) free(ev.audio);
        }
    }
}
```

### 5.5 Audio Tasks

```c
static void audio_rx_task(void *arg)
{
    int16_t buf[CHUNK_SAMPLES];
    while (1) {
        size_t n = i2s_audio_read(buf, CHUNK_SAMPLES, pdMS_TO_TICKS(150));
        if (g_state == STATE_RECORDING && n > 0) {
            ws_send_audio(buf, n);
        }
        // Small yield to prevent starving lower-priority tasks
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void audio_tx_task(void *arg)
{
    uint8_t buf[1024];
    while (1) {
        size_t len = 0;
        // Wait for audio from WebSocket handler
        if (xRingBufferReceive(audio_playback_rb, &len, sizeof(len), portMAX_DELAY)) {
            uint8_t *pcm = malloc(len);
            if (pcm && xRingBufferReceive(audio_playback_rb, pcm, len, pdMS_TO_TICKS(100))) {
                size_t written = 0;
                while (written < len) {
                    size_t s = i2s_audio_write((int16_t *)(pcm + written),
                                               (len - written) / 2, pdMS_TO_TICKS(100));
                    written += s * sizeof(int16_t);
                }
            }
            free(pcm);
        }
    }
}
```

### 5.6 Button Task

```c
#define BTN_PTT_GPIO   GPIO_NUM_10
#define BTN_SWAP_GPIO  GPIO_NUM_3

static void button_task(void *arg)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BTN_PTT_GPIO) | (1ULL << BTN_SWAP_GPIO),
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io_conf);

    bool last_ptt = false, last_swap = false;
    while (1) {
        bool ptt = (gpio_get_level(BTN_PTT_GPIO) == 0);
        if (ptt && !last_ptt)      state_machine_post_event(EVENT_PTT_PRESS);
        else if (!ptt && last_ptt) state_machine_post_event(EVENT_PTT_RELEASE);

        bool swap = (gpio_get_level(BTN_SWAP_GPIO) == 0);
        if (swap && !last_swap)    state_machine_post_event(EVENT_SWAP);

        last_ptt = ptt;
        last_swap = swap;
        vTaskDelay(pdMS_TO_TICKS(20));  // 50 Hz poll, 20 ms debounce
    }
}
```

### 5.7 `main.c`

```c
#include <stdio.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "wifi_manager.h"
#include "i2s_audio.h"
#include "ws_client.h"
#include "button.h"
#include "state_machine.h"
#include "led.h"

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_manager_init();
    i2s_audio_init();
    led_init();
    state_machine_init();   // creates queues and state_task
    button_init();          // creates button_task
    ws_client_init();       // starts ws client; audio_rx_task created on connect
}
```

---

## 6. Build Instructions

### 6.1 Host (Go + Python)

```bash
# 1. Python ML service (run this in a terminal, it stays up)
cd backend
source venv/bin/activate
pip install fastapi uvicorn websockets  # if not present
python pyworkers/ml_service.py          # listens on :9379

# 2. Existing web UI (optional, another terminal)
python backend/server.py                # listens on :3000

# 3. Go bridge (another terminal)
cd backend/bridge
go mod init translator/bridge 2>/dev/null || true
go get github.com/gorilla/websocket
go run .
# Listening on ws://0.0.0.0:8765
```

### 6.2 Device (ESP-IDF)

```bash
cd firmware
idf.py set-target esp32c3
idf.py menuconfig   # Set Wi-Fi SSID/Password under Example Connection Configuration
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

**Required `sdkconfig.defaults` snippets:**

```
CONFIG_ESP_WIFI_AUTH_WPA2_PSK=y
CONFIG_EXAMPLE_WIFI_SSID="YourSSID"
CONFIG_EXAMPLE_WIFI_PASSWORD="YourPass"
CONFIG_FREERTOS_UNICORE=y          # C3 is single-core
```

---

## 7. Implementation Roadmap

| Phase | Task | Validation |
|:---|:---|:---|
| **1** | **Host loopback test** — Run Go bridge + Python ML service. Connect with `websocat` and send a raw WAV file. Verify you get `stt` → `translation` → `tts_audio` binary back. | `websocat ws://localhost:8765` |
| **2** | **ESP32 I2S loopback** — Flash a test app that reads mic and immediately writes to speaker (no WiFi). Verify your voice loops back clearly. | Speak into mic, hear yourself |
| **3** | **ESP32 WiFi + WS** — Connect to Go bridge. Send `hello`, then `ping`. Verify `pong` and LED goes green. | Check serial monitor |
| **4** | **Audio upload** — Press PTT, speak, release. Verify the Go bridge logs the correct byte count and triggers STT. | Go stdout shows pipeline stages |
| **5** | **End-to-end** — Full button press → recorded speech → host STT → translation → TTS → playback on MAX98357A. | Hear translated speech within 2 s |
| **6** | **Polish** — Add NVS language persistence, mDNS host discovery (`translator.local`), battery voltage ADC, OLED idle screen. | Reboot retains language pair |

---

## 8. Troubleshooting

| Symptom | Cause | Fix |
|:---|:---|:---|
| **ESP32 won't boot** | GPIO 9 (or 2/8) held low at reset | Use GPIO 10 for PTT. Add external 10kΩ pull-up on strapping pins. |
| **Mic captures silence** | INMP441 `L/R` pin floating | Tie `L/R` to **GND** (left channel). |
| **Speaker crackles / no output** | MAX98357A `SD` pin floating | Pull `SD` high to 3.3 V. Verify I2S Philips mode (not MSB). |
| **STT returns empty** | Sample rate mismatch | Ensure mic is **16 kHz** exactly. Check with logic analyzer or `i2s_read` byte count: 3200 bytes/100 ms = correct. |
| **TTS plays too fast / slow** | Host sent 22050 Hz data | Verify `ml_service.py` resamples TTS output to 16000 Hz. |
| **WebSocket drops during long utterances** | No keep-alive | `sdkconfig`: enable `CONFIG_ESP_WS_CLIENT_KEEPALIVE`. Go `upgrader` has no timeout by default. |
| **Go bridge panics on concurrent clients** | Shared Python HTTP client | Add `sync.Mutex` around `runPipeline` if your Python ML service is single-threaded, or run multiple Python workers behind a Go `httputil.ReverseProxy` load balancer. |

---

## 9. Why This Design Wins

| Decision | Rationale |
|:---|:---|
| **Go for the bridge** | Handles 10k+ concurrent WebSockets with < 5 MB RAM. Native binary/JSON framing without base64 bloat. Single static binary deployment. |
| **Python only for ML** | You cannot run Moonshine or moonshine-voice in Go without rewriting PyTorch/ONNX runtimes. Isolating Python to a localhost microservice keeps the GIL away from your network I/O. |
| **ESP-IDF, not Arduino** | `idf.py` gives you the new I2S STD driver (correct full-duplex API), FreeRTOS task priorities, and `esp_websocket_client` with automatic reconnect. Arduino-ESP32 lags behind on C3 peripheral support. |
| **Single I2S @ 16 kHz** | Eliminates the impossible dual-clock problem on ESP32-C3. Host bears the cost of one linear resample (negligible on a modern CPU). |
| **Single WebSocket** | DeepSeek's original insight. Half the TCP overhead of Claude's dual-socket design, no magic `0xDEADBEEF` terminators, and natural back-pressure via TCP flow control. |