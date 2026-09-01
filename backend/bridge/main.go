package main

import (
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
)

func main() {
	port := flag.Int("port", 8765, "WebSocket server port")
	mlURL := flag.String("ml-url", "http://127.0.0.1:9379", "Python ML service URL")
	flag.Parse()

	// Environment variable overrides
	if envPort := os.Getenv("BRIDGE_PORT"); envPort != "" {
		fmt.Sscanf(envPort, "%d", port)
	}
	if envML := os.Getenv("ML_SERVICE_URL"); envML != "" {
		*mlURL = envML
	}

	log.Printf("==================================================")
	log.Printf("  ESP32-C3 Translator — High-Performance Go Bridge")
	log.Printf("  WebSocket Port : ws://0.0.0.0:%d", *port)
	log.Printf("  ML Service URL : %s", *mlURL)
	log.Printf("==================================================")

	mlClient := NewMLClient(*mlURL)
	pipeline := NewPipelineRunner(mlClient)

	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		handleWebSocket(w, r, pipeline)
	})

	// Health check endpoint
	http.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusOK)
		w.Write([]byte(`{"status":"ok","service":"translator-go-bridge"}`))
	})

	addr := fmt.Sprintf(":%d", *port)
	log.Printf("[bridge] Listening on %s ...", addr)
	if err := http.ListenAndServe(addr, nil); err != nil {
		log.Fatalf("[bridge] Server error: %v", err)
	}
}
