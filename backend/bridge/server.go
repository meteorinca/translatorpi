package main

import (
	"encoding/json"
	"log"
	"net/http"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool { return true },
	ReadBufferSize:  16384,
	WriteBufferSize: 16384,
}

// ClientSession manages the state of a connected ESP32-C3 or Web client.
type ClientSession struct {
	conn       *websocket.Conn
	device     string
	src        string
	dst        string
	recording  bool
	audioBuf   []byte
	audioMu    sync.Mutex
	writeMu    sync.Mutex
	pipeline   *PipelineRunner
	lastActive time.Time
}

func handleWebSocket(w http.ResponseWriter, r *http.Request, pipeline *PipelineRunner) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("[server] WebSocket upgrade error: %v", err)
		return
	}
	defer conn.Close()
	conn.SetReadLimit(10 * 1024 * 1024)

	session := &ClientSession{
		conn:       conn,
		device:     "breadboard_c3",
		src:        "en",
		dst:        "es",
		audioBuf:   make([]byte, 0, 16000*2*10), // pre-allocate ~10 seconds @ 16kHz 16-bit
		pipeline:   pipeline,
		lastActive: time.Now(),
	}

	remoteAddr := conn.RemoteAddr().String()
	log.Printf("[server] Client connected from %s", remoteAddr)

	// Send initial greeting
	session.writeMu.Lock()
	sendJSON(conn, ServerMsg{
		Type:    "status",
		State:   "ready",
		Message: "Connected to Go Bridge",
		Src:     session.src,
		Dst:     session.dst,
	})
	session.writeMu.Unlock()

	for {
		messageType, message, err := conn.ReadMessage()
		if err != nil {
			if websocket.IsUnexpectedCloseError(err, websocket.CloseGoingAway, websocket.CloseAbnormalClosure) {
				log.Printf("[server] Client %s disconnected with error: %v", remoteAddr, err)
			} else {
				log.Printf("[server] Client %s disconnected", remoteAddr)
			}
			return
		}

		session.lastActive = time.Now()

		if messageType == websocket.TextMessage {
			var cm ClientMsg
			if err := json.Unmarshal(message, &cm); err != nil {
				log.Printf("[server] Invalid JSON from %s: %s", remoteAddr, string(message))
				continue
			}

			switch cm.Type {
			case "hello":
				if cm.Device != "" {
					session.device = cm.Device
				}
				if cm.Src != "" {
					session.src = cm.Src
				}
				if cm.Dst != "" {
					session.dst = cm.Dst
				}
				log.Printf("[server] Device '%s' hello: %s -> %s", session.device, session.src, session.dst)
				session.writeMu.Lock()
				sendJSON(conn, ServerMsg{
					Type:    "status",
					State:   "ready",
					Message: session.src + " -> " + session.dst,
					Src:     session.src,
					Dst:     session.dst,
				})
				session.writeMu.Unlock()

			case "set_lang":
				if cm.Src != "" {
					session.src = cm.Src
				}
				if cm.Dst != "" {
					session.dst = cm.Dst
				}
				log.Printf("[server] Language changed to %s -> %s", session.src, session.dst)
				session.writeMu.Lock()
				sendJSON(conn, ServerMsg{
					Type:    "status",
					State:   "ready",
					Message: session.src + " -> " + session.dst,
					Src:     session.src,
					Dst:     session.dst,
				})
				session.writeMu.Unlock()

			case "swap":
				targetLangs := []string{"es", "zh", "ja", "ar", "ko"}
				nextDst := "zh"
				for i, l := range targetLangs {
					if l == session.dst {
						nextDst = targetLangs[(i+1)%len(targetLangs)]
						break
					}
				}
				session.src = "en"
				session.dst = nextDst
				log.Printf("[server] Target language cycled: %s -> %s", session.src, session.dst)
				session.writeMu.Lock()
				sendJSON(conn, ServerMsg{
					Type:    "status",
					State:   "ready",
					Message: session.src + " -> " + session.dst,
					Src:     session.src,
					Dst:     session.dst,
				})
				session.writeMu.Unlock()

			case "record_start":
				session.audioMu.Lock()
				session.recording = true
				session.audioBuf = session.audioBuf[:0]
				session.audioMu.Unlock()
				log.Printf("[server] Recording started on %s", remoteAddr)
				session.writeMu.Lock()
				sendJSON(conn, ServerMsg{
					Type:    "status",
					State:   "recording",
					Message: "Recording...",
				})
				session.writeMu.Unlock()

			case "audio":
				// JSON header preceding binary frame (optional metadata)
				// Binary payload comes in websocket.BinaryMessage

			case "record_stop":
				session.audioMu.Lock()
				session.recording = false
				captured := make([]byte, len(session.audioBuf))
				copy(captured, session.audioBuf)
				session.audioMu.Unlock()

				log.Printf("[server] Recording stopped on %s: captured %d bytes", remoteAddr, len(captured))

				if len(captured) < 3200 { // Less than 100ms
					log.Printf("[server] Audio buffer too short (%d bytes), ignoring", len(captured))
					session.writeMu.Lock()
					sendJSON(conn, ServerMsg{
						Type:    "error",
						Message: "Audio too short (<100ms)",
						State:   "ready",
					})
					session.writeMu.Unlock()
					continue
				}

				// Run pipeline asynchronously so we don't block the WebSocket reading loop
				go session.pipeline.Run(captured, session.src, session.dst, session.conn, &session.writeMu)

			case "ping":
				session.writeMu.Lock()
				sendJSON(conn, ServerMsg{Type: "pong"})
				session.writeMu.Unlock()
			}

		} else if messageType == websocket.BinaryMessage {
			session.audioMu.Lock()
			if session.recording {
				session.audioBuf = append(session.audioBuf, message...)
			}
			session.audioMu.Unlock()
		}
	}
}
