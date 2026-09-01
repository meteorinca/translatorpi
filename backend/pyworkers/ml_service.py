"""
Python ML Inference Microservice for ESP32-C3 Portable Translator.
Provides fast endpoints for Moonshine STT, Gemma 4 Translation, and moonshine-voice TTS.
"""

import sys
import os
import io
import json
import time
import urllib.request
import urllib.parse
from typing import Optional
import httpx
import numpy as np
import uvicorn
from fastapi import FastAPI, Form, Request, Response
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse

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

# Add parent directory to sys.path so we can import models from backend.server
BACKEND_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if BACKEND_DIR not in sys.path:
    sys.path.insert(0, BACKEND_DIR)

from server import (
    get_stt_recognizer,
    get_tts_engine,
    SUPPORTED_STT_LANGS,
    TTS_LANG_MAP,
    LANGUAGE_LABELS,
)

app = FastAPI(title="Translator ML Microservice", version="1.0.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

ML_PORT = int(os.environ.get("ML_SERVICE_PORT", "9379"))

# Safe LLM endpoint resolution: never loop back to this service's own port
_raw_endpoint = os.environ.get("LLM_ENDPOINT", "").strip()
if _raw_endpoint:
    try:
        _parsed = urllib.parse.urlparse(_raw_endpoint)
        if _parsed.port == ML_PORT:
            print(f"[ML-Service] Warning: LLM_ENDPOINT points to ML service port ({ML_PORT}). Disabling LLM loopback.")
            LLM_ENDPOINT = None
        else:
            LLM_ENDPOINT = _raw_endpoint
    except Exception:
        LLM_ENDPOINT = _raw_endpoint
else:
    LLM_ENDPOINT = None

LLM_MODEL = os.environ.get("LLM_MODEL", "gemma4-e2b")


def resample_linear(audio: np.ndarray, orig_sr: int, target_sr: int) -> np.ndarray:
    """Fast numpy-based linear resampling without heavy external library dependencies."""
    if orig_sr == target_sr or len(audio) == 0:
        return audio
    duration = len(audio) / orig_sr
    num_target_samples = int(duration * target_sr)
    if num_target_samples == 0:
        return np.array([], dtype=audio.dtype)
    orig_indices = np.linspace(0, len(audio) - 1, num=len(audio))
    target_indices = np.linspace(0, len(audio) - 1, num=num_target_samples)
    return np.interp(target_indices, orig_indices, audio).astype(audio.dtype)


@app.get("/health")
async def health():
    return {
        "status": "ok",
        "service": "translator-ml-microservice",
        "supported_stt_languages": list(SUPPORTED_STT_LANGS),
        "supported_tts_languages": list(TTS_LANG_MAP.keys()),
        "time": time.time(),
    }


@app.post("/stt")
async def stt(request: Request, lang: str = "en"):
    """
    Accepts raw Float32 little-endian or Int16 audio buffer (16 kHz mono).
    Returns transcribed text JSON.
    """
    audio_bytes = await request.body()
    if not audio_bytes:
        return JSONResponse({"text": "", "error": "Empty audio payload"}, status_code=400)

    try:
        # Determine if payload is float32 (len % 4 == 0) or int16 (len % 2 == 0)
        # Default expectation from Go bridge is float32
        audio_np = np.frombuffer(audio_bytes, dtype=np.float32)
        if len(audio_np) == 0:
            return {"text": ""}

        recognizer = get_stt_recognizer(lang)
        transcript = recognizer.transcribe_without_streaming(audio_np, 16000)
        text = " ".join([line.text for line in transcript.lines]).strip()
        print(f"[ML-STT] ({lang}) Transcribed: '{text}'")
        return {"text": text}
    except Exception as e:
        print(f"[ML-STT Error] {e}")
        return JSONResponse({"text": "", "error": str(e)}, status_code=500)


GT_LANG_MAP = {
    "zh": "zh-CN",
    "en": "en",
    "es": "es",
    "ar": "ar",
    "ja": "ja",
    "ko": "ko",
}


async def translate_via_llm(text: str, src: str, dst: str) -> Optional[str]:
    """Attempts translation using local or remote LLM endpoint (Ollama / LiteRT / OpenAI format)."""
    if not LLM_ENDPOINT:
        return None

    src_name = LANGUAGE_LABELS.get(src, src)
    dst_name = LANGUAGE_LABELS.get(dst, dst)

    prompt = (
        f"You are a professional, accurate real-time speech translator. "
        f"Translate the following spoken text from {src_name} to {dst_name}.\n"
        f"Rules:\n"
        f"1. Output ONLY the translated text.\n"
        f"2. Do not include quotes, explanations, prefixes, or commentary.\n\n"
        f"Text:\n{text}\n\nTranslation:"
    )

    payload = {
        "model": LLM_MODEL,
        "messages": [{"role": "user", "content": prompt}],
        "temperature": 0.2,
        "max_tokens": 256,
    }

    try:
        async with httpx.AsyncClient(timeout=4.0) as client:
            resp = await client.post(LLM_ENDPOINT, json=payload)
            if resp.status_code == 200:
                res_json = resp.json()
                content = res_json["choices"][0]["message"]["content"].strip()
                if (content.startswith('"') and content.endswith('"')) or (content.startswith("'") and content.endswith("'")):
                    content = content[1:-1].strip()
                return content
            else:
                print(f"[ML-Translate LLM] Server returned HTTP {resp.status_code}")
    except Exception as e:
        print(f"[ML-Translate Warning] LLM endpoint unreachable ({e})")

    return None


async def translate_via_online(text: str, src: str, dst: str) -> Optional[str]:
    """Free, fast online translation fallback (MyMemory API with Google fallback)."""
    # 1. Try MyMemory API
    try:
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
        print(f"[ML-Translate Warning] MyMemory fallback error: {e}")

    # 2. Try Google GTX API
    try:
        sl = GT_LANG_MAP.get(src, src)
        tl = GT_LANG_MAP.get(dst, dst)
        q = urllib.parse.quote(text)
        url = f"https://translate.googleapis.com/translate_a/single?client=gtx&sl={sl}&tl={tl}&dt=t&q={q}"
        headers = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"}
        async with httpx.AsyncClient(timeout=3.0) as client:
            resp = await client.get(url, headers=headers)
            if resp.status_code == 200:
                data = resp.json()
                translated = "".join(segment[0] for segment in data[0] if segment and segment[0])
                if translated.strip():
                    return translated.strip()
    except Exception as e:
        print(f"[ML-Translate Warning] Google fallback error: {e}")

    return None


@app.post("/translate")
async def translate(
    text: str = Form(""),
    src: str = Form("en"),
    dst: str = Form("zh"),
):
    """
    Translates text from src language to dst language.
    1. Queries LLM endpoint (LiteRT-LM / Ollama) if available.
    2. Falls back to online translation service.
    3. Falls back to original text.
    """
    if not text.strip():
        return {"translation": ""}

    # 1. Try local LLM
    translated = await translate_via_llm(text, src, dst)
    if translated:
        print(f"[ML-Translate (LLM)] ({src}->{dst}) '{text}' => '{translated}'")
        return {"translation": translated}

    # 2. Try online translation
    translated = await translate_via_online(text, src, dst)
    if translated:
        print(f"[ML-Translate (Online)] ({src}->{dst}) '{text}' => '{translated}'")
        return {"translation": translated}

    # 3. Fallback to original text
    print(f"[ML-Translate (Fallback)] ({src}->{dst}) '{text}' => '{text}'")
    return {"translation": text}


@app.post("/tts")
async def tts(text: str = Form(""), lang: str = Form("zh")):
    """
    Synthesizes speech for the given text.
    Returns raw 16 kHz 16-bit mono PCM bytes.
    """
    if not text.strip():
        return Response(content=b"", media_type="application/octet-stream")

    try:
        engine = get_tts_engine(lang)
        audio, sample_rate = engine.synthesize(text)

        samples = np.asarray(audio, dtype=np.float32)

        # Resample to 16,000 Hz if engine output rate is different (e.g. 22,050 Hz)
        if sample_rate != 16000:
            samples = resample_linear(samples, int(sample_rate), 16000)

        # Clip and convert to 16-bit PCM
        samples = np.clip(samples, -1.0, 1.0)
        pcm16 = (samples * 32767.0).astype(np.int16).tobytes()

        print(f"[ML-TTS] ({lang}) Synthesized '{text[:30]}...' -> {len(pcm16)} bytes PCM16 @ 16kHz")
        return Response(content=pcm16, media_type="application/octet-stream")
    except Exception as e:
        print(f"[ML-TTS Error] {e}")
        return JSONResponse({"error": str(e)}, status_code=500)


if __name__ == "__main__":
    port = int(os.environ.get("ML_SERVICE_PORT", "9379"))
    host = os.environ.get("ML_SERVICE_HOST", "127.0.0.1")
    print(f"[ML Microservice] Starting on http://{host}:{port} ...")
    uvicorn.run(app, host=host, port=port)
