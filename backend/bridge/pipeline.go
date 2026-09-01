package main

import (
	"log"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

// PipelineRunner coordinates the translation pipeline per client session.
type PipelineRunner struct {
	mlClient *MLClient
	mu       sync.Mutex
}

func NewPipelineRunner(mlClient *MLClient) *PipelineRunner {
	return &PipelineRunner{
		mlClient: mlClient,
	}
}

// Run executes the complete STT -> Translate -> TTS sequence and streams progress frames to the client.
func (p *PipelineRunner) Run(audioPCM []byte, src, dst string, conn *websocket.Conn, wsWriteMu *sync.Mutex) {
	p.mu.Lock()
	defer p.mu.Unlock()

	startTime := time.Now()
	log.Printf("[pipeline] starting processing: %d bytes PCM audio (%s -> %s)", len(audioPCM), src, dst)

	writeJSON := func(msg ServerMsg) {
		wsWriteMu.Lock()
		defer wsWriteMu.Unlock()
		if err := sendJSON(conn, msg); err != nil {
			log.Printf("[pipeline] error sending json msg: %v", err)
		}
	}

	writeBinary := func(data []byte) {
		wsWriteMu.Lock()
		defer wsWriteMu.Unlock()
		if err := conn.WriteMessage(websocket.BinaryMessage, data); err != nil {
			log.Printf("[pipeline] error sending binary audio chunk: %v", err)
		}
	}

	// Step 1: STT
	writeJSON(ServerMsg{Type: "status", State: "transcribing", Message: "Listening..."})
	floatAudio := pcm16ToFloat32(audioPCM)
	sttText, err := p.mlClient.CallSTT(floatAudio, src)
	if err != nil {
		log.Printf("[pipeline] STT error: %v", err)
		writeJSON(ServerMsg{Type: "error", Message: "STT error: " + err.Error()})
		writeJSON(ServerMsg{Type: "done"})
		writeJSON(ServerMsg{Type: "status", State: "ready", Message: src + " -> " + dst})
		return
	}

	if sttText == "" {
		log.Printf("[pipeline] STT returned empty transcript")
		writeJSON(ServerMsg{Type: "stt", Text: "(no speech detected)"})
		writeJSON(ServerMsg{Type: "done"})
		writeJSON(ServerMsg{Type: "status", State: "ready", Message: src + " -> " + dst})
		return
	}

	log.Printf("[pipeline] STT result: \"%s\"", sttText)
	writeJSON(ServerMsg{Type: "stt", Text: sttText})

	// Step 2: Translation
	writeJSON(ServerMsg{Type: "status", State: "translating", Message: "Translating..."})
	translatedText, err := p.mlClient.CallTranslate(sttText, src, dst)
	if err != nil {
		log.Printf("[pipeline] Translation error: %v", err)
		writeJSON(ServerMsg{Type: "error", Message: "Translate error: " + err.Error()})
		writeJSON(ServerMsg{Type: "done"})
		writeJSON(ServerMsg{Type: "status", State: "ready", Message: src + " -> " + dst})
		return
	}

	log.Printf("[pipeline] Translation result: \"%s\"", translatedText)
	writeJSON(ServerMsg{
		Type:        "translation",
		Text:        translatedText,
		Translation: translatedText,
	})

	// Step 3: TTS
	writeJSON(ServerMsg{Type: "status", State: "synthesizing", Message: "Generating voice..."})
	ttsPCM, err := p.mlClient.CallTTS(translatedText, dst)
	if err != nil {
		log.Printf("[pipeline] TTS error: %v", err)
		writeJSON(ServerMsg{Type: "error", Message: "TTS error: " + err.Error()})
		writeJSON(ServerMsg{Type: "done"})
		writeJSON(ServerMsg{Type: "status", State: "ready", Message: src + " -> " + dst})
		return
	}

	// Step 4: Stream audio back to device
	log.Printf("[pipeline] Streaming %d bytes TTS audio back to client (%.2f seconds @ 16kHz)", len(ttsPCM), float64(len(ttsPCM))/32000.0)
	writeJSON(ServerMsg{
		Type:  "tts_audio",
		Len:   len(ttsPCM),
		Rate:  16000,
		State: "speaking",
	})

	// Send binary in chunks of 1600 samples (3200 bytes) for smooth streaming if preferred, or whole payload
	chunkSize := 3200
	for offset := 0; offset < len(ttsPCM); offset += chunkSize {
		end := offset + chunkSize
		if end > len(ttsPCM) {
			end = len(ttsPCM)
		}
		writeBinary(ttsPCM[offset:end])
		// Pacing: 3200 bytes = 100ms audio @ 16kHz. 35ms sleep prevents overflowing ESP32 ringbuffer while buffering ahead smoothly.
		time.Sleep(35 * time.Millisecond)
	}

	writeJSON(ServerMsg{Type: "done"})
	writeJSON(ServerMsg{Type: "status", State: "ready", Message: src + " -> " + dst})
	log.Printf("[pipeline] Complete roundtrip took %v", time.Since(startTime))
}
