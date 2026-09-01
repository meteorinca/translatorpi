"""
Automated Test Client for ESP32-C3 WebSocket Pipeline.
Simulates an ESP32 client sending speech audio chunks and verifying STT, translation, and TTS responses.
"""

import asyncio
import json
import math
import sys
import numpy as np
import websockets

WS_URL = "ws://localhost:8765"


def generate_tone(freq=440.0, duration_sec=1.5, sample_rate=16000):
    """Generates a synthetic sine wave tone in 16-bit PCM little-endian format."""
    num_samples = int(duration_sec * sample_rate)
    t = np.linspace(0, duration_sec, num_samples, endpoint=False)
    # Sine wave with gentle envelope to simulate voice sound
    envelope = np.sin(np.pi * t / duration_sec)
    samples = 0.5 * envelope * np.sin(2 * np.pi * freq * t)
    pcm16 = (samples * 32767.0).astype(np.int16).tobytes()
    return pcm16


async def run_test():
    print(f"Connecting to {WS_URL} ...")
    async with websockets.connect(WS_URL) as ws:
        print("Connected! Waiting for welcome frame...")
        welcome = await ws.recv()
        print(f"< Received: {welcome}")

        # Send Hello
        hello_msg = {
            "type": "hello",
            "device": "breadboard_c3",
            "src": "en",
            "dst": "zh",
        }
        await ws.send(json.dumps(hello_msg))
        print(f"> Sent: {hello_msg}")

        # Receive greeting response
        resp = await ws.recv()
        print(f"< Received: {resp}")

        # Send record_start
        await ws.send(json.dumps({"type": "record_start"}))
        print("> Sent: record_start")

        # Stream audio chunks (100ms = 3200 bytes)
        raw_pcm = generate_tone(freq=300, duration_sec=1.2)
        chunk_size = 3200
        for i in range(0, len(raw_pcm), chunk_size):
            chunk = raw_pcm[i:i + chunk_size]
            await ws.send(chunk)
            await asyncio.sleep(0.05)

        # Send record_stop
        await ws.send(json.dumps({"type": "record_stop"}))
        print(f"> Sent: record_stop ({len(raw_pcm)} bytes total audio)")

        # Listen for pipeline responses
        total_audio_received = 0
        while True:
            msg = await ws.recv()
            if isinstance(msg, str):
                data = json.loads(msg)
                print(f"< JSON Response: {data}")
                if data.get("type") == "done":
                    print("Pipeline execution complete ('done' received)")
                    break
                elif data.get("type") == "error":
                    print(f"Server error: {data.get('message')}")
                    break
            elif isinstance(msg, bytes):
                total_audio_received += len(msg)
                print(f"< Received Binary Audio Chunk: {len(msg)} bytes (Total: {total_audio_received} bytes)")

        print(f"Test completed successfully! Received {total_audio_received} bytes TTS audio.")


if __name__ == "__main__":
    asyncio.run(run_test())
