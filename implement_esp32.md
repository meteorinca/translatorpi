# ESP32-C3 Portable Translator — Implementation Plan

This document describes how to turn this repo (a Raspberry Pi kiosk translator) into a
**handheld translator device** built on an **ESP32-C3**. The ESP32-C3 becomes the portable
piece (mic + speaker + 0.91" OLED + push-to-talk button) and offloads all the heavy work —
**STT**, **TTS**, and **translation** — to a computer (Linux / PC / Mac) over Wi-Fi.

The computer keeps the existing audio pipeline (`backend/server.py`) but replaces the LLM
translation step with **Google Offline Translation**, and gains a small WebSocket endpoint so
the ESP32-C3 can stream audio in and out over a single persistent connection.

---

## 1. Goal & architecture

```
                ┌──────────────────────────────┐
                │        ESP32-C3 device       │
                │  (portable, replaces the Pi) │
                │                              │
                │  INMP441 mic  ── I2S ──┐     │
                │  MAX98357A spk ── I2S ─┤     │
                │  SSD1306 0.91" ── I2C ─┤     │
                │  PTT / swap buttons ───┤     │
                └──────────────────┬─────┴─────┘
                                   │  Wi-Fi (2.4 GHz)
                                   │  WebSocket ws://<host-ip>:8765
                                   ▼
                ┌──────────────────────────────────────────────┐
                │      Computer (Linux / PC / Mac)             │
                │                                              │
                │  esp32_ws.py  ── WebSocket bridge            │
                │       │                                      │
                │       ├─ STT  (Moonshine, via server.py)     │
                │       ├─ TTS  (moonshine-voice, via server)  │
                │       └─ Translation (Google Offline Trans.) │
                └──────────────────────────────────────────────┘
```

**Division of responsibilities**

| Concern | Where it runs |
| :--- | :--- |
| Capture microphone audio | ESP32-C3 (I2S, 16 kHz mono) |
| Play back translated speech | ESP32-C3 (I2S speaker) |
| Show language pair / status | ESP32-C3 (0.91" SSD1306) |
| Push-to-talk & language swap | ESP32-C3 (GPIO buttons) |
| Speech-to-text | Computer (Moonshine — already in this repo) |
| Text-to-speech | Computer (moonshine-voice — already in this repo) |
| Translation | Computer (**Google Offline Translation**) |

The device only ever moves **audio bytes and short status strings**; it never runs an ML model.

---

## 2. What already exists vs. what changes

The current backend (`backend/server.py`, port 3000) already exposes exactly the audio
primitives we need:

| Endpoint | Purpose | Note |
| :--- | :--- | :--- |
| `POST /api/stt` | STT | Body `{"audio_base64", "language"}`; audio = base64 **Float32 16 kHz mono**; returns `{"text"}` |
| `GET /api/tts?text=…&lang=…` | TTS | Returns **16-bit PCM mono WAV** |
| `/proxy?url=…` | Translation | Proxies to LiteRT-LM (Gemma 4) `chat/completions` at `localhost:9379` — **this is the part we replace** |
| `/api/languages` | Language status | Lists `zh, en, ar, es, ja, ko` |

Supported languages (shared by STT and TTS): `zh, en, ar, es, ja, ko`.

**Changes to make on the computer:**

1. **Replace translation with Google Offline Translation** — new `backend/translate.py`
   exposing `translate_text(text, src, dst)`. The device never calls `/proxy` anymore.
2. **Add a WebSocket bridge** — new `backend/esp32_ws.py` on port 8765 that receives audio,
   runs STT → translate → TTS, and streams results back. It reuses the STT/TTS engines from
   `server.py` (`get_stt_recognizer`, `get_tts_engine`).
3. *(Optional)* Keep the existing React kiosk UI running for debugging/monitoring — it is not
   required for the device flow.

---

## 3. Hardware

### 3.1 Bill of materials

| Part | Role | Notes |
| :--- | :--- | :--- |
| ESP32-C3 dev board (e.g. SuperMini or DevKitM-1) | MCU + Wi-Fi | Single-core RISC-V, 2.4 GHz Wi-Fi, I2S + I2C |
| INMP441 (or ICS-43434) | I2S MEMS microphone | 3.3 V, mono, 16 kHz capable |
| MAX98357A + small speaker | I2S amplifier / speakerphone | 3.3–5 V VIN (use 5 V for louder output) |
| SSD1306 0.91" 128×32 OLED | Display | I2C |
| 2× tactile push buttons | PTT + language swap | Active-low (pull-up) |
| Breadboard / perfboard + jumpers | — | — |

> The ESP32-C3 has **no Bluetooth Classic** (BLE only) and no analog output — a plain
> speaker-on-GPIO will not work well. The MAX98357A I2S amp is the right choice.

### 3.2 Wiring (example pinout)

All software pins are configurable in `firmware/include/config.h`. Avoid the USB pins
(18/19) and strapping pins (2, 8, 9).

| Function | Signal | ESP32-C3 pin |
| :--- | :--- | :--- |
| OLED SDA | SDA | GPIO6 |
| OLED SCL | SCL | GPIO7 |
| OLED VCC / GND | — | 3.3 V / GND |
| Mic SCK | BCLK | GPIO0 |
| Mic WS | LRCL | GPIO1 |
| Mic SD | DIN (data **in** to ESP32) | GPIO3 |
| Mic L/R | — | GND (drives it to mono/left) |
| Mic VDD / GND | — | 3.3 V / GND |
| Amp BCLK | BCLK | GPIO4 |
| Amp LRC | LRCL | GPIO5 |
| Amp DIN | DIN (data **out** from ESP32) | GPIO10 |
| Amp VIN / GND | — | 5 V (or 3.3 V) / GND |
| Amp SD (shutdown) | — | 3.3 V (pull high = enabled) |
| Amp GAIN | — | leave floating or set per datasheet |
| PTT button | — | GPIO20 (to GND, pull-up) |
| Swap button | — | GPIO21 (to GND, pull-up) |

Notes:
- Two separate I2S peripherals are used: **I2S0 = mic (RX)**, **I2S1 = speaker (TX)**.
- GPIO20/21 are the UART0 TX/RX on the SuperMini; they are free to repurpose **only if**
  you log/program over USB-CDC (18/19). Otherwise pick other free pins.
- Verify the exact pin list for your specific board revision before soldering.

---

## 4. Communication protocol (WebSocket)

**Recommendation: a single persistent WebSocket** from the ESP32-C3 to the computer
(`ws://<host-ip>:8765`). One connection avoids per-request TCP setup, lets the host stream
TTS audio back in chunks, and gives natural back-pressure (the device queues only what it can
play).

The wire format is a mix of **JSON text frames** (control/metadata) and **binary frames**
(raw audio). The JSON frames always have a `type` field.

### 4.1 Device → Computer

| Frame | Meaning |
| :--- | :--- |
| `{"type":"hello","device":"esp32c3","src":"en","dst":"zh"}` | Register + declare the language pair |
| `{"type":"record_start"}` | PTT pressed |
| `{"type":"record_stop"}` | PTT released (audio follows) |
| `{"type":"audio","format":"pcm16","rate":16000,"channels":1,"len":<N>}` | Text header, then a **binary** frame of N bytes |
| `{"type":"swap"}` | Swap `src`/`dst` (each speaker taps it) |
| `{"type":"ping"}` | Keep-alive / reconnect |

Audio payload (binary frame): **16-bit little-endian PCM, 16 kHz, mono**. For the same reason
the browser path uses 16 kHz, the mic must be configured to 16 kHz mono before it leaves the
device. (If you prefer HTTP or base64, see §4.4.)

### 4.2 Computer → Device

| Frame | Meaning |
| :--- | :--- |
| `{"type":"status","state":"recording\|transcribing\|translating\|speaking\|idle\|error","message":"…"}` | Drives the OLED status line |
| `{"type":"stt","text":"…"}` | Recognized source text (shown on OLED) |
| `{"type":"translation","text":"…"}` | Translated text (shown on OLED) |
| `{"type":"tts_audio","len":<N>,"rate":16000}` | Text header, then a **binary** frame of N bytes PCM16 |
| `{"type":"done"}` | Entire round-trip finished |
| `{"type":"error","message":"…"}` | Failure to display/play |

TTS audio is sent as **raw 16-bit PCM mono** (no WAV header) because the sample rate/channels
are fixed by the protocol — this saves the 44-byte header and simplifies the device.

### 4.3 Why this is efficient

- 16 kHz × 16-bit mono = **32,000 bytes/s ≈ 256 kbit/s** — trivial for Wi-Fi.
- Binary frames avoid the ~33% base64 bloat and the 4-byte-per-sample `Float32` overhead that
  the browser path (`/api/stt`) pays.
- One socket → no reconnects between STT/TTS; results stream back as soon as each stage ends.

Optional (later) optimization: compress the mic uplink with **Opus** (~16–24 kbit/s) if you
ever need to cut bandwidth or relay through the internet. Skip for the first build.

### 4.4 Fallback: reuse the existing HTTP API (no WebSocket)

If you want zero new backend code, the device can call the existing endpoints directly:

1. `POST /api/stt` with `{"audio_base64": "<base64 Float32 16 kHz mono>", "language": src}`
2. new `POST /api/translate` with `{"text", "src", "dst"}` → `{"translation"}`
3. `GET /api/tts?text=…&lang=…` → WAV → strip header → play

This works but costs 3 separate requests and base64/Float32 overhead. Use it only as a
stop-gap or diagnostic; the WebSocket path is the target design.

---

## 5. Host-side changes (computer)

### 5.1 Translation adapter — `backend/translate.py`

```python
"""Translation adapter. Swap the body for whichever offline engine you use.

The device never calls this directly; esp32_ws.py calls translate_text().
"""

# --- Google Offline Translation seam ---
# Plug in your chosen engine here. The contract is simply:
#   translate_text(text: str, src: str, dst: str) -> str

def translate_text(text: str, src: str, dst: str) -> str:
    # Example using a hypothetical google_offline_translate module:
    #   from google_offline_translate import Translator
    #   return Translator(src, dst).translate(text)
    #
    # Argos Translate (fully offline, pip install argostranslate) is a drop-in
    # alternative if you don't have Google Offline Translation wired up yet:
    #   import argostranslate.translate as at
    #   return at.translate(text, src, dst)
    raise NotImplementedError("Wire translate_text() to your offline engine")
```

Keep this function the **only** place that knows about the translation engine, so you can
switch engines without touching the WebSocket bridge or the firmware.

> If your "Google Offline Translation" runs as a separate local service (its own port/CLI),
> wrap that call inside `translate_text()`. The rest of the system doesn't care.

### 5.2 WebSocket bridge — `backend/esp32_ws.py`

```python
"""WebSocket bridge: ESP32-C3 <-> STT/TTS/translation.

Run:  python backend/esp32_ws.py     (listens on ws://0.0.0.0:8765)

Reuses the STT/TTS engines from server.py and the translation adapter above.
"""
import asyncio
import json
import numpy as np
import websockets

# Reuse the existing engines (server.py must be importable from this dir).
from server import get_stt_recognizer, get_tts_engine, SUPPORTED_STT_LANGS, TTS_LANG_MAP
from translate import translate_text

SAMPLE_RATE = 16000

def pcm16_to_float32(pcm: bytes) -> np.ndarray:
    """ESP32 sends int16 PCM; Moonshine STT expects float32 in [-1, 1]."""
    return (np.frombuffer(pcm, dtype="<i2").astype(np.float32)) / 32768.0

def float32_to_pcm16(audio: np.ndarray) -> bytes:
    """moonshine-voice returns float32 in [-1, 1]; device expects int16 PCM."""
    samples = np.clip(np.asarray(audio, dtype=np.float32), -1.0, 1.0)
    return (samples * 32767.0).astype("<i2").tobytes()

async def send_json(ws, obj):
    await ws.send(json.dumps(obj))

async def handler(ws):
    src, dst = "en", "zh"
    audio_parts = []

    async for message in ws:
        # Binary frame = the audio payload that follows an "audio" header.
        if isinstance(message, bytes):
            audio_parts.append(message)
            continue

        msg = json.loads(message)
        t = msg.get("type")

        if t == "hello":
            src = msg.get("src", src)
            dst = msg.get("dst", dst)
            await send_json(ws, {"type": "status", "state": "idle"})

        elif t == "swap":
            src, dst = dst, src
            await send_json(ws, {"type": "status", "state": "idle",
                                 "message": f"{src} -> {dst}"})

        elif t == "record_start":
            audio_parts = []
            await send_json(ws, {"type": "status", "state": "recording"})

        elif t == "record_stop":
            if not audio_parts:
                await send_json(ws, {"type": "error", "message": "no audio"})
                continue
            pcm = b"".join(audio_parts)
            audio_parts = []

            # 1. STT
            await send_json(ws, {"type": "status", "state": "transcribing"})
            try:
                recognizer = get_stt_recognizer(src)
                transcript = recognizer.transcribe_without_streaming(
                    pcm16_to_float32(pcm), SAMPLE_RATE)
                text = " ".join(line.text for line in transcript.lines)
            except Exception as e:
                await send_json(ws, {"type": "error", "message": f"STT: {e}"})
                continue
            await send_json(ws, {"type": "stt", "text": text})
            if not text.strip():
                await send_json(ws, {"type": "done"})
                continue

            # 2. Translation
            await send_json(ws, {"type": "status", "state": "translating"})
            try:
                translation = translate_text(text, src, dst)
            except Exception as e:
                await send_json(ws, {"type": "error", "message": f"translate: {e}"})
                continue
            await send_json(ws, {"type": "translation", "text": translation})

            # 3. TTS (streamed back as raw PCM)
            await send_json(ws, {"type": "status", "state": "speaking"})
            try:
                engine = get_tts_engine(dst)
                audio, sr = engine.synthesize(translation)
                pcm_out = float32_to_pcm16(audio)
                await send_json(ws, {"type": "tts_audio", "len": len(pcm_out),
                                     "rate": sr})
                await ws.send(pcm_out)
            except Exception as e:
                await send_json(ws, {"type": "error", "message": f"TTS: {e}"})
                continue

            await send_json(ws, {"type": "done"})

        elif t == "ping":
            await send_json(ws, {"type": "pong"})

async def main():
    async with websockets.serve(handler, "0.0.0.0", 8765):
        print("ESP32 bridge listening on ws://0.0.0.0:8765")
        await asyncio.Future()

if __name__ == "__main__":
    asyncio.run(main())
```

Add `websockets` to `backend/requirements.txt`. If you prefer to keep everything in one file,
the `handler()` above can also be grafted into `server.py` (it already binds a
`ThreadingTCPServer`; you'd add a second thread running the asyncio WebSocket server).

---

## 6. ESP32-C3 firmware

### 6.1 Toolchain & libraries

Use **PlatformIO** with the Arduino framework (fastest path). `firmware/platformio.ini`:

```ini
[env:esp32-c3-devkitm-1]
platform = espressif32
board = esp32-c3-devkitm-1
framework = arduino
monitor_speed = 115200
lib_deps =
    links2004/WebSockets@^2.6.1      ; WebSocket client
    olikraus/U8g2@^2.34.22            ; SSD1306 OLED
    espressif/ESP_I2S@^1.1.0          ; I2S mic + speaker (ESP32-C3)
```

> I2S note: the current Arduino-ESP32 core uses the **ESP_I2S** library (`I2SClass`) for
> ESP32-C3 rather than the legacy `driver/i2s.h` from older cores. Pin the library version
> and adapt the API if your core version differs.

### 6.2 Firmware layout

```
firmware/
├── platformio.ini
├── include/config.h          # Wi-Fi creds, host IP, pins, language defaults
└── src/
    ├── main.cpp              # setup/loop, button handling, state machine
    ├── audio.cpp / audio.h   # I2S mic capture + speaker playback
    ├── display.cpp/display.h # OLED rendering
    └── ws_client.cpp/.h      # WebSocket connection + protocol framing
```

### 6.3 `config.h`

```cpp
#pragma once
// --- Wi-Fi / host --------------------------------------------------------
#define WIFI_SSID      "your-ssid"
#define WIFI_PASS      "your-password"
#define HOST_IP        "192.168.1.50"      // computer running esp32_ws.py
#define HOST_PORT      8765

// --- Pins -----------------------------------------------------------------
#define PIN_MIC_BCLK   0
#define PIN_MIC_LRC    1
#define PIN_MIC_DIN    3

#define PIN_SPK_BCLK   4
#define PIN_SPK_LRC    5
#define PIN_SPK_DOUT   10

#define PIN_OLED_SDA   6
#define PIN_OLED_SCL   7

#define PIN_BTN_PTT    20                  // push-to-talk
#define PIN_BTN_SWAP   21                  // swap language direction

// --- Audio -----------------------------------------------------------------
#define SAMPLE_RATE    16000
#define CHUNK_SAMPLES  1600                // 100 ms per I2S read/write
```

### 6.4 Main loop / state machine

The device is a small state machine:

```
IDLE → (PTT down) → RECORDING → (PTT up) → SENDING → WAITING → SPEAKING → IDLE
```

- **RECORDING**: read 100 ms I2S chunks into a ring buffer; count samples.
- **SENDING**: send `{"type":"record_stop"}`, then the `audio` header + binary PCM frame.
- **WAITING**: show `stt`/`translation` text as it arrives on the OLED.
- **SPEAKING**: receive `tts_audio` header + binary PCM and stream to the speaker I2S buffer.
- The **swap** button flips `src`/`dst` and sends `{"type":"swap"}`.

```cpp
// main.cpp (skeleton)
void setup() {
  initDisplay();          // U8g2 SSD1306 128x32
  initAudio();            // ESP_I2S mic (RX) + speaker (TX)
  initButtons();          // pull-ups + debounce
  connectWifi();
  ws.begin(HOST_IP, HOST_PORT, "/");
  ws.onEvent(onWsEvent);
}

void loop() {
  ws.loop();
  pollButtons();          // PTT -> record_start/stop ; SWAP -> swap
  if (recording) captureChunk();      // I2S read -> ring buffer
  if (speaking)  pumpSpeaker();       // feed PCM to I2S as it arrives
  renderDisplay();        // throttled OLED update (e.g. every 100 ms)
}
```

### 6.5 Audio capture / playback (I2S, ESP32-C3)

```cpp
// audio.cpp (abridged) — ESP_I2S API
#include <ESP_I2S.h>

ESP_I2S i2sMic;   // I2S0: RX (INMP441)
ESP_I2S i2sSpk;   // I2S1: TX (MAX98357A)

void initAudio() {
  i2sMic.setPins(PIN_MIC_BCLK, PIN_MIC_LRC, PIN_MIC_DIN);
  i2sMic.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
               I2S_SLOT_MODE_MONO);
  i2sSpk.setPins(PIN_SPK_BCLK, PIN_SPK_LRC, PIN_SPK_DOUT);
  i2sSpk.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
               I2S_SLOT_MODE_MONO);
}

// Pull one 100 ms chunk of int16 mono samples from the mic.
void captureChunk(std::vector<int16_t>& buf) {
  buf.resize(CHUNK_SAMPLES);
  i2sMic.readBytes((uint8_t*)buf.data(), CHUNK_SAMPLES * sizeof(int16_t));
}

// Queue PCM for playback; call repeatedly while 'speaking'.
void playPcm(const uint8_t* data, size_t len) {
  i2sSpk.write(data, len);
}
```

Key correctness points:
- Configure **16 kHz / 16-bit / mono** on both peripherals — this is what STT expects.
- The INMP441 must have its **L/R pin tied to GND** to emit on the left channel; otherwise the
  samples can be displaced (right-justified into a 32-bit slot).
- The MAX98357A needs its **SD pin high** and the I2S data must be left-justified (standard
  Philips I2S, `I2S_MODE_STD`).

### 6.6 OLED UI (128×32)

0.91" is small — reserve the top line for the **language pair** and the bottom for **status**:

```
EN -> ZH
[recording…]
```

Line 1 (fixed): `SRC -> DST` (from current `src`/`dst`). Line 2 (dynamic) shows the last
`status.state` plus a hint of the latest `stt`/`translation` text (scroll if too long):

| state | line 2 |
| :--- | :--- |
| idle | `ready` |
| recording | `● recording…` |
| transcribing | `listening…` |
| translating | `translating…` |
| speaking | `speaking…` |
| error | `error` (short message) |

---

## 7. Configuration & pairing

- **Language pair** defaults to `en`/`zh` and can be changed on the device (swap button) or
  from the host (`hello` frame). Persist the pair in ESP32 **NVS** (Preferences library) so it
  survives reboots.
- **Host discovery**: hard-code `HOST_IP` for the first build; later add mDNS
  (`esp32c3-translator.local`) or a settings page so the device finds the computer automatically.
- **Wi-Fi creds**: hard-code in `config.h` first; move to a captive portal (WiFiManager) later.

---

## 8. Latency & bandwidth budget

| Stage | Typical cost |
| :--- | :--- |
| Mic capture (PTT) | 0 (user-defined length) |
| Uplink 16 kHz PCM | 32 KB/s ≈ 256 kbit/s |
| STT (Moonshine, CPU/GPU host) | ~0.2–1× realtime |
| Translation (offline engine) | ~0.1–0.5 s |
| TTS (moonshine-voice) | ~0.2–1× realtime |
| Downlink PCM | 32 KB/s |

End-to-end after release is dominated by STT+TTS on the host, not by the radio. If you want to
shave the uplink, switch the mic to Opus (libopus) — but keep PCM for the first milestone.

---

## 9. Step-by-step plan

1. **Computer: wire the translation adapter.** Implement `backend/translate.py`
   (`translate_text`) with your Google Offline Translation engine; test it standalone with a
   fixed phrase.
2. **Computer: add the WebSocket bridge.** Add `backend/esp32_ws.py` + `websockets` dep. Test
   end-to-end from a small Python client (or `websocat`) by sending a WAV of speech and checking
   you get `stt` → `translation` → `tts_audio` frames back.
3. **Device: bring up the OLED.** Flash a minimal sketch; print `EN -> ZH` on the SSD1306.
4. **Device: bring up the mic.** Capture 2 s at 16 kHz, then dump the int16 samples over serial
   to confirm they're real speech (and re-check the INMP441 L/R grounding).
5. **Device: bring up the speaker.** Play a hard-coded PCM tone/ramp to confirm the MAX98357A.
6. **Device: Wi-Fi + WebSocket client.** Connect to the host and handshake `hello`.
7. **Device: full loop.** PTT → record → upload → show STT/translation → stream TTS to speaker.
8. **Polish.** Reconnect logic, swap button, NVS persistence, mDNS, optional Opus.

Acceptance test for the full loop: hold PTT, speak in language A, release; within ~2 s the OLED
shows the recognized + translated text and the speaker plays the translation in language B.

---

## 10. Troubleshooting

| Symptom | Likely cause |
| :--- | :--- |
| OLED blank | I2C address wrong (SSD1306 0.91" is usually `0x3C`), or SDA/SCL swapped |
| Mic captures silence/garbage | INMP441 L/R not grounded; WS/BCLK swapped; wrong channel slot |
| Speaker crackles/no output | MAX98357A SD pin floating; GAIN floating; wrong bit depth/slot mode |
| STT returns empty text | Device sending wrong sample rate, or int16↔float32 conversion missing |
| WebSocket drops on long clips | No keep-alive `ping`; reconnect logic missing |
| Host unreachable | Windows/macOS firewall blocking port 8765; device & host on different subnet/VLAN |

---

## 11. Files to create (summary)

```
backend/translate.py      # Google Offline Translation adapter (translate_text)
backend/esp32_ws.py       # WebSocket bridge (port 8765)
backend/requirements.txt  # + websockets
firmware/                 # PlatformIO project (ESP32-C3)
  platformio.ini
  include/config.h
  src/main.cpp, audio.*, display.*, ws_client.*
```
