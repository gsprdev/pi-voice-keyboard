package main

import (
	"bytes"
	"crypto/rand"
	"embed"
	"encoding/base64"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"strings"
	"time"
)

//go:embed web/*
var webContent embed.FS

const (
	defaultPort             = "8081"
	defaultTranscriptionURL = "http://localhost:8080/transcribe"
	defaultSocketPath       = "/tmp/kb.sock"
	defaultConfigPath       = "web-server-config.txt"
)

// Config holds server configuration
type Config struct {
	Port             string
	APIKey           string
	TranscriptionURL string
	SocketPath       string
}

func main() {
	var (
		port             = flag.String("port", defaultPort, "Port to listen on")
		transcriptionURL = flag.String("transcription-url", defaultTranscriptionURL, "URL of transcription service")
		socketPath       = flag.String("socket", defaultSocketPath, "Path to type-ascii Unix socket")
		configPath       = flag.String("config", defaultConfigPath, "Path to config file")
		generateKey      = flag.Bool("generate-key", false, "Generate a new API key and exit")
	)
	flag.Parse()

	// Handle key generation
	if *generateKey {
		key, err := generateAPIKey()
		if err != nil {
			log.Fatalf("Failed to generate API key: %v", err)
		}
		fmt.Printf("Generated API key: %s\n", key)
		fmt.Printf("\nAdd this to your config file (%s):\n", *configPath)
		fmt.Printf("API_KEY=%s\n", key)
		os.Exit(0)
	}

	// Load or create config
	config := loadConfig(*configPath, *port, *transcriptionURL, *socketPath)

	log.Printf("Starting web server on port %s", config.Port)
	log.Printf("Transcription service: %s", config.TranscriptionURL)
	log.Printf("HID socket: %s", config.SocketPath)
	log.Printf("API key configured: %s", maskKey(config.APIKey))

	server := &Server{
		config: config,
	}

	http.HandleFunc("/", server.handleIndex)
	http.HandleFunc("/transcribe", server.handleTranscribe)

	addr := ":" + config.Port
	log.Printf("Serving at https://localhost:%s", config.Port)
	if err := http.ListenAndServeTLS(addr, "cert.pem", "key.pem", nil); err != nil {
		log.Fatalf("Server failed: %v", err)
	}
}

type Server struct {
	config *Config
}

// handleIndex serves the recording UI
func (s *Server) handleIndex(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}

	content, err := webContent.ReadFile("web/index.html")
	if err != nil {
		http.Error(w, "Failed to load page", http.StatusInternalServerError)
		log.Printf("Error reading index.html: %v", err)
		return
	}

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Write(content)
}

// handleTranscribe processes audio and coordinates transcription + typing
func (s *Server) handleTranscribe(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	// Validate API key
	apiKey := r.Header.Get("X-API-Key")
	if apiKey != s.config.APIKey {
		log.Printf("Authentication failed: invalid API key")
		http.Error(w, "Unauthorized", http.StatusUnauthorized)
		return
	}

	startTime := time.Now()
	log.Printf("[%d] Starting transcription request", startTime.UnixMilli())

	// Read audio data
	audioData, err := io.ReadAll(r.Body)
	if err != nil {
		log.Printf("Error reading request body: %v", err)
		http.Error(w, "Failed to read audio data", http.StatusBadRequest)
		return
	}
	log.Printf("[%d] Received audio: %d bytes", startTime.UnixMilli(), len(audioData))

	// Forward to transcription service
	transcription, err := s.callTranscriptionService(audioData)
	if err != nil {
		log.Printf("Transcription failed: %v", err)
		http.Error(w, "Transcription failed", http.StatusInternalServerError)
		return
	}
	log.Printf("[%d] Transcription complete: %d chars", startTime.UnixMilli(), len(transcription))

	// Type the transcription via HID
	if err := s.typeText(transcription); err != nil {
		log.Printf("Failed to type text: %v", err)
		http.Error(w, "Failed to output text", http.StatusInternalServerError)
		return
	}

	elapsed := time.Since(startTime)
	log.Printf("[%d] === SUMMARY === Total: %v | Transcription successful", startTime.UnixMilli(), elapsed)

	w.WriteHeader(http.StatusOK)
	w.Write([]byte("OK"))
}

// callTranscriptionService forwards audio to the GPU transcription service
func (s *Server) callTranscriptionService(audioData []byte) (string, error) {
	req, err := http.NewRequest(http.MethodPost, s.config.TranscriptionURL, bytes.NewReader(audioData))
	if err != nil {
		return "", fmt.Errorf("failed to create request: %w", err)
	}

	req.Header.Set("Content-Type", "audio/wav")

	client := &http.Client{Timeout: 30 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return "", fmt.Errorf("request failed: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return "", fmt.Errorf("transcription service returned %d: %s", resp.StatusCode, string(body))
	}

	text, err := io.ReadAll(resp.Body)
	if err != nil {
		return "", fmt.Errorf("failed to read response: %w", err)
	}

	return string(text), nil
}

// typeText sends transcribed text to the HID socket
func (s *Server) typeText(text string) error {
	conn, err := net.Dial("unix", s.config.SocketPath)
	if err != nil {
		return fmt.Errorf("failed to connect to socket %s: %w (is kb-serve.sh running?)", s.config.SocketPath, err)
	}
	defer conn.Close()

	_, err = conn.Write([]byte(text))
	if err != nil {
		return fmt.Errorf("failed to write to socket: %w", err)
	}

	return nil
}

// loadConfig loads configuration from file or uses defaults
func loadConfig(configPath, port, transcriptionURL, socketPath string) *Config {
	config := &Config{
		Port:             port,
		TranscriptionURL: transcriptionURL,
		SocketPath:       socketPath,
	}

	// Try to load from config file
	data, err := os.ReadFile(configPath)
	if err == nil {
		parseConfig(string(data), config)
		log.Printf("Loaded configuration from %s", configPath)
	} else {
		log.Printf("No config file found at %s, using defaults", configPath)
	}

	// Generate initial API key if not set
	if config.APIKey == "" {
		key, err := generateAPIKey()
		if err != nil {
			log.Fatalf("Failed to generate initial API key: %v", err)
		}
		config.APIKey = key

		// Save to config file
		configContent := fmt.Sprintf("# Web Server Configuration\n# Generated: %s\n\nAPI_KEY=%s\nPORT=%s\nTRANSCRIPTION_URL=%s\nSOCKET_PATH=%s\n",
			time.Now().Format(time.RFC3339), key, config.Port, config.TranscriptionURL, config.SocketPath)

		if err := os.WriteFile(configPath, []byte(configContent), 0600); err != nil {
			log.Printf("Warning: Failed to save config file: %v", err)
		} else {
			log.Printf("Generated new API key and saved to %s", configPath)
			log.Printf("Your API key: %s", key)
		}
	}

	return config
}

// parseConfig parses simple KEY=VALUE config format
func parseConfig(content string, config *Config) {
	for _, line := range strings.Split(content, "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}

		parts := strings.SplitN(line, "=", 2)
		if len(parts) != 2 {
			continue
		}

		key := strings.TrimSpace(parts[0])
		value := strings.TrimSpace(parts[1])

		switch key {
		case "API_KEY":
			config.APIKey = value
		case "PORT":
			config.Port = value
		case "TRANSCRIPTION_URL":
			config.TranscriptionURL = value
		case "SOCKET_PATH":
			config.SocketPath = value
		}
	}
}

// generateAPIKey creates a human-friendly API key using word list
func generateAPIKey() (string, error) {
	// Simple word list for memorable keys
	words := []string{
		"alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel",
		"india", "juliet", "kilo", "lima", "mike", "november", "oscar", "papa",
		"quebec", "romeo", "sierra", "tango", "uniform", "victor", "whiskey", "xray",
		"yankee", "zulu", "apple", "banana", "cherry", "dragon", "eagle", "falcon",
		"guitar", "hammer", "island", "jungle", "kitten", "lemon", "mango", "ninja",
		"ocean", "piano", "quartz", "rocket", "sunset", "tiger", "ultra", "violet",
		"wizard", "xenon", "yellow", "zebra", "anchor", "bridge", "castle", "desert",
	}

	// Generate 4 random words + 2 digit number
	var parts []string
	for i := 0; i < 4; i++ {
		idx, err := randomInt(len(words))
		if err != nil {
			return "", err
		}
		parts = append(parts, words[idx])
	}

	// Add 2-digit number
	num, err := randomInt(100)
	if err != nil {
		return "", err
	}

	return fmt.Sprintf("%s-%d", strings.Join(parts, "-"), num), nil
}

// randomInt returns a random integer in [0, max)
func randomInt(max int) (int, error) {
	// Generate enough random bytes
	nBig := max
	var b [8]byte
	_, err := rand.Read(b[:])
	if err != nil {
		return 0, err
	}

	// Convert to int and mod
	n := int(base64.StdEncoding.EncodeToString(b[:])[0]) % nBig
	return n, nil
}

// maskKey shows only first/last chars of API key for logging
func maskKey(key string) string {
	if len(key) <= 8 {
		return "***"
	}
	return key[:4] + "..." + key[len(key)-4:]
}
