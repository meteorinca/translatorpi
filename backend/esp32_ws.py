"""
Standalone Python WebSocket Bridge for ESP32-C3 Portable Translator.
Allows running STT, Translation, and TTS directly over WebSockets in a single Python script.
"""

import asyncio
import json
import os
import sys
import time
import numpy as np
import websockets

# Ensure UTF-8 output encoding so printing non-ASCII transcripts doesn't crash on Windows
if sys.stdout and hasattr(sys.stdout, "reconfigure"):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
if sys.stderr and hasattr(sys.stderr, "reconfigure"):
    try:
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

# Windows UCRT compatibility patch for moonshine-voice
if sys.platform == "win32":
    import ctypes
    try:
        import moonshine_voice.moonshine_api as _m_api
        _m_api._libc = ctypes.CDLL("ucrtbase")
        _m_api._libc.free.argtypes = [ctypes.c_void_p]
        _m_api._libc.free.restype = None
    except Exception:
        pass

BACKEND_DIR = os.path.dirname(os.path.abspath(__file__))
if BACKEND_DIR not in sys.path:
    sys.path.insert(0, BACKEND_DIR)

from server import (
    get_stt_recognizer,
    get_tts_engine,
    LANGUAGE_LABELS,
    SUPPORTED_STT_LANGS,
)

WS_PORT = int(os.environ.get("ESP32_WS_PORT", "8765"))
LLM_ENDPOINT = os.environ.get("LLM_ENDPOINT", "").strip()
LLM_MODEL = os.environ.get("LLM_MODEL", "gemma4-e2b")

GT_LANG_MAP = {
    "zh": "zh-CN",
    "en": "en",
    "es": "es",
    "ar": "ar",
    "ja": "ja",
    "ko": "ko",
}


def resample_linear(audio: np.ndarray, orig_sr: int, target_sr: int) -> np.ndarray:
    if orig_sr == target_sr or len(audio) == 0:
        return audio
    duration = len(audio) / orig_sr
    num_target_samples = int(duration * target_sr)
    if num_target_samples == 0:
        return np.array([], dtype=audio.dtype)
    orig_indices = np.linspace(0, len(audio) - 1, num=len(audio))
    target_indices = np.linspace(0, len(audio) - 1, num=num_target_samples)
    return np.interp(target_indices, orig_indices, audio).astype(audio.dtype)


async def translate_text(text: str, src: str, dst: str) -> str:
    if not text.strip():
        return ""

    src_name = LANGUAGE_LABELS.get(src, src)
    dst_name = LANGUAGE_LABELS.get(dst, dst)

    # 1. Try local LLM if configured
    if LLM_ENDPOINT:
        try:
            import httpx
            prompt = (
                f"You are a professional, accurate real-time speech translator. "
                f"Translate the following spoken text from {src_name} to {dst_name}.\n"
                f"Rules:\n1. Output ONLY the translated text.\n"
                f"2. Do not include quotes, explanations, prefixes, or commentary.\n\n"
                f"Text:\n{text}\n\nTranslation:"
            )
            payload = {
                "model": LLM_MODEL,
                "messages": [{"role": "user", "content": prompt}],
                "temperature": 0.2,
                "max_tokens": 256,
            }
            async with httpx.AsyncClient(timeout=4.0) as client:
                resp = await client.post(LLM_ENDPOINT, json=payload)
                if resp.status_code == 200:
                    res_json = resp.json()
                    content = res_json["choices"][0]["message"]["content"].strip()
                    if (content.startswith('"') and content.endswith('"')) or (content.startswith("'") and content.endswith("'")):
                        content = content[1:-1].strip()
                    return content
        except Exception as e:
            print(f"[WS Bridge] LLM translate warning: {e}")

    # 2. Try online translation fallback (MyMemory API)
    try:
        import urllib.parse
        import httpx
        q = urllib.parse.quote(text)
        pair = f"{src}|{dst}"
        url = f"https://api.mymemory.translated.net/get?q={q}&langpair={pair}"
        headers = {"User-Agent": "Mozilla/5.0"}
        async with httpx.AsyncClient(timeout=3.5) as client:
            resp = await client.get(url, headers=headers)
            if resp.status_code == 200:
                data = resp.json()
                trans = data.get("responseData", {}).get("translatedText", "").strip()
                if trans and trans.lower() != text.lower() and "MYMEMORY WARNING" not in trans:
                    return trans
    except Exception as e:
        print(f"[WS Bridge] Online translate warning: {e}")

    return text


async def process_utterance(websocket, pcm_bytes: bytes, src: str, dst: str):
    start_time = time.time()
    print(f"[WS Bridge] Processing {len(pcm_bytes)} bytes audio ({src} -> {dst})")

    # 1. STT
    await websocket.send(json.dumps({"type": "status", "state": "transcribing", "message": "Listening..."}))
    
    int16_samples = np.frombuffer(pcm_bytes, dtype=np.int16)
    float32_samples = (int16_samples.astype(np.float32) / 32768.0)

    loop = asyncio.get_running_loop()
    def _run_stt():
        recognizer = get_stt_recognizer(src)
        transcript = recognizer.transcribe_without_streaming(float32_samples, 16000)
        return " ".join([l.text for l in transcript.lines]).strip()

    stt_text = await loop.run_in_executor(None, _run_stt)
    if not stt_text:
        print("[WS Bridge] STT returned empty transcript")
        await websocket.send(json.dumps({"type": "stt", "text": "(no speech)"}))
        await websocket.send(json.dumps({"type": "done"}))
        await websocket.send(json.dumps({"type": "status", "state": "ready", "message": f"{src} -> {dst}"}))
        return

    print(f"[WS Bridge] STT: '{stt_text}'")
    await websocket.send(json.dumps({"type": "stt", "text": stt_text}))

    # 2. Translate
    await websocket.send(json.dumps({"type": "status", "state": "translating", "message": "Translating..."}))
    translation = await translate_text(stt_text, src, dst)
    print(f"[WS Bridge] Translation: '{translation}'")
    await websocket.send(json.dumps({"type": "translation", "text": translation}))

    # 3. TTS
    await websocket.send(json.dumps({"type": "status", "state": "synthesizing", "message": "Synthesizing voice..."}))
    def _run_tts():
        engine = get_tts_engine(dst)
        audio, sample_rate = engine.synthesize(translation)
        samples = np.asarray(audio, dtype=np.float32)
        if sample_rate != 16000:
            samples = resample_linear(samples, int(sample_rate), 16000)
        samples = np.clip(samples, -1.0, 1.0)
        return (samples * 32767.0).astype(np.int16).tobytes()

    tts_pcm = await loop.run_in_executor(None, _run_tts)
    print(f"[WS Bridge] TTS synthesized: {len(tts_pcm)} bytes")

    # 4. Stream audio back
    await websocket.send(json.dumps({
        "type": "tts_audio",
        "len": len(tts_pcm),
        "rate": 16000,
        "state": "speaking"
    }))

    # Stream in 3200-byte chunks (100ms)
    chunk_size = 3200
    for i in range(0, len(tts_pcm), chunk_size):
        chunk = tts_pcm[i:i + chunk_size]
        await websocket.send(chunk)
        await asyncio.sleep(0.01)

    await websocket.send(json.dumps({"type": "done"}))
    await websocket.send(json.dumps({"type": "status", "state": "ready", "message": f"{src} -> {dst}"}))
    print(f"[WS Bridge] Pipeline finished in {time.time() - start_time:.2f}s")


async def handler(websocket):
    client_ip = websocket.remote_address[0]
    print(f"[WS Bridge] Client connected from {client_ip}")

    device = "breadboard_c3"
    src = "en"
    dst = "zh"
    recording = False
    audio_buffer = bytearray()

    await websocket.send(json.dumps({
        "type": "status",
        "state": "ready",
        "message": f"Connected ({src} -> {dst})",
        "src": src,
        "dst": dst,
    }))

    try:
        async for message in websocket:
            if isinstance(message, str):
                try:
                    data = json.loads(message)
                except Exception:
                    continue

                msg_type = data.get("type", "")

                if msg_type == "hello":
                    device = data.get("device", device)
                    src = data.get("src", src)
                    dst = data.get("dst", dst)
                    print(f"[WS Bridge] Device '{device}' hello ({src} -> {dst})")
                    await websocket.send(json.dumps({"type": "status", "state": "ready", "message": f"{src} -> {dst}", "src": src, "dst": dst}))

                elif msg_type == "swap":
                    src, dst = dst, src
                    print(f"[WS Bridge] Language swap: {src} -> {dst}")
                    await websocket.send(json.dumps({"type": "status", "state": "ready", "message": f"{src} -> {dst}", "src": src, "dst": dst}))

                elif msg_type == "set_lang":
                    src = data.get("src", src)
                    dst = data.get("dst", dst)
                    print(f"[WS Bridge] Language set: {src} -> {dst}")
                    await websocket.send(json.dumps({"type": "status", "state": "ready", "message": f"{src} -> {dst}", "src": src, "dst": dst}))

                elif msg_type == "record_start":
                    recording = True
                    audio_buffer.clear()
                    print("[WS Bridge] Recording started")
                    await websocket.send(json.dumps({"type": "status", "state": "recording", "message": "Recording..."}))

                elif msg_type == "record_stop":
                    recording = False
                    print(f"[WS Bridge] Recording stopped, captured {len(audio_buffer)} bytes")
                    if len(audio_buffer) < 3200:
                        await websocket.send(json.dumps({"type": "error", "message": "Audio too short (<100ms)", "state": "ready"}))
                    else:
                        asyncio.create_task(process_utterance(websocket, bytes(audio_buffer), src, dst))

                elif msg_type == "ping":
                    await websocket.send(json.dumps({"type": "pong"}))

            elif isinstance(message, bytes):
                if recording:
                    audio_buffer.extend(message)

    except websockets.exceptions.ConnectionClosed:
        print(f"[WS Bridge] Client {client_ip} disconnected")
    except Exception as e:
        print(f"[WS Bridge Error] {e}")


async def main():
    print(f"==================================================")
    print(f"  ESP32-C3 Translator — Standalone Python Bridge")
    print(f"  WebSocket Listening on ws://0.0.0.0:{WS_PORT}")
    print(f"==================================================")
    async with websockets.serve(handler, "0.0.0.0", WS_PORT, max_size=10 * 1024 * 1024):
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
