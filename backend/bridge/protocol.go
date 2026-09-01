package main

import (
	"encoding/json"
	"github.com/gorilla/websocket"
)

// ClientMsg represents incoming JSON frames from the ESP32-C3 client.
type ClientMsg struct {
	Type     string `json:"type"`               // "hello", "record_start", "audio", "record_stop", "swap", "ping", "set_lang"
	Device   string `json:"device,omitempty"`   // e.g. "breadboard_c3", "dogbot_c3"
	Src      string `json:"src,omitempty"`      // e.g. "en"
	Dst      string `json:"dst,omitempty"`      // e.g. "zh"
	Format   string `json:"format,omitempty"`   // "pcm16"
	Rate     int    `json:"rate,omitempty"`     // 16000
	Channels int    `json:"channels,omitempty"` // 1
	Len      int    `json:"len,omitempty"`      // audio chunk length in bytes
}

// ServerMsg represents outgoing JSON frames sent from the Go Bridge to the ESP32-C3 client.
type ServerMsg struct {
	Type        string `json:"type"`                  // "status", "stt", "translation", "tts_audio", "done", "error", "pong"
	State       string `json:"state,omitempty"`       // "ready", "recording", "transcribing", "translating", "synthesizing", "speaking", "idle", "error"
	Text        string `json:"text,omitempty"`        // Recognized STT text
	Translation string `json:"translation,omitempty"` // Translated target text
	Len         int    `json:"len,omitempty"`         // Next binary payload byte length
	Rate        int    `json:"rate,omitempty"`        // Audio sample rate (16000)
	Message     string `json:"message,omitempty"`     // Informational or error text
	Src         string `json:"src,omitempty"`
	Dst         string `json:"dst,omitempty"`
}

// sendJSON helper sends a ServerMsg as a WebSocket text message.
func sendJSON(conn *websocket.Conn, msg ServerMsg) error {
	b, err := json.Marshal(msg)
	if err != nil {
		return err
	}
	return conn.WriteMessage(websocket.TextMessage, b)
}
