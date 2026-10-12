package main

import (
	"encoding/binary"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"regexp"
	"strings"
	"time"

	"github.com/ggerganov/whisper.cpp/bindings/go/pkg/whisper"
)

var (
	model         whisper.Model
	modelPath     string
	modelLanguage string
)

func getenv(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

func main() {
	port := getenv("PORT", "8080")
	// Upstream model name, as downloaded by `mise run model`; relative to the repo root
	modelPath = "speech-models/ggml-" + getenv("MODEL", "medium.en") + ".bin"
	modelLanguage = getenv("MODEL_LANGUAGE", "en")

	initCapture()

	// Validate model file exists
	if _, err := os.Stat(modelPath); os.IsNotExist(err) {
		log.Fatalf("Model file not found: %s (download with `mise run model`)", modelPath)
	}

	// Get model file size for logging
	if info, err := os.Stat(modelPath); err == nil {
		log.Printf("Model file: %s (%.2f MB)", modelPath, float64(info.Size())/(1024*1024))
	}

	// Load whisper model
	log.Printf("Loading whisper model...")
	startLoad := time.Now()
	var err error
	model, err = whisper.New(modelPath)
	if err != nil {
		log.Fatalf("Failed to load model: %v", err)
	}
	defer model.Close()
	log.Printf("Model loaded successfully in %v", time.Since(startLoad))

	// Setup HTTP routes
	http.HandleFunc("/transcribe", transcribeHandler)
	http.HandleFunc("/health", healthHandler)

	// Start server
	addr := ":" + port
	log.Printf("Starting transcription service on %s", addr)
	log.Printf("POST /transcribe - Audio transcription endpoint")
	log.Printf("GET  /health     - Health check endpoint")

	if err := http.ListenAndServe(addr, nil); err != nil {
		log.Fatalf("Server failed: %v", err)
	}
}

// healthHandler returns service status
func healthHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	w.Header().Set("Content-Type", "text/plain")
	fmt.Fprintln(w, "OK")
}

// transcribeHandler processes audio and returns transcription
func transcribeHandler(w http.ResponseWriter, r *http.Request) {
	requestStart := time.Now()
	requestID := fmt.Sprintf("%d", time.Now().UnixNano())

	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	// Enforce Content-Type audio/wav
	if r.Header.Get("Content-Type") != "audio/wav" {
		log.Printf("[%s] Invalid Content-Type: %s", requestID, r.Header.Get("Content-Type"))
		http.Error(w, "Invalid Content-Type, expected audio/wav", http.StatusBadRequest)
		return
	}

	log.Printf("[%s] Starting transcription request", requestID)

	capture := newCapture(requestID, r)
	defer capture.finish()
	w = capture.responseWriter(w)

	// Create temporary file for audio
	tempFile, err := os.CreateTemp("", "audio-*.wav")
	if err != nil {
		log.Printf("[%s] Error creating temp file: %v", requestID, err)
		http.Error(w, "Internal server error", http.StatusInternalServerError)
		return
	}
	defer os.Remove(tempFile.Name())
	defer tempFile.Close()

	// Write request body to temp file
	ioStart := time.Now()
	bytesWritten, err := io.Copy(capture.audioWriter(tempFile), r.Body)
	if err != nil {
		log.Printf("[%s] Error writing audio data: %v", requestID, err)
		http.Error(w, "Error reading audio data", http.StatusBadRequest)
		return
	}
	tempFile.Close()
	uploadDuration := time.Since(ioStart)
	capture.timing("upload", uploadDuration)
	log.Printf("[%s] Audio upload complete: %d bytes in %v", requestID, bytesWritten, uploadDuration)

	// Load WAV file and convert to float32 samples
	loadStart := time.Now()
	samples, err := loadWAV(tempFile.Name())
	if err != nil {
		log.Printf("[%s] Error loading WAV file: %v", requestID, err)
		http.Error(w, "Error reading audio file", http.StatusBadRequest)
		return
	}
	audioDuration := float64(len(samples)) / 16000.0 // Assuming 16kHz sample rate
	decodeDuration := time.Since(loadStart)
	capture.timing("decode", decodeDuration)
	capture.decoded(samples, audioDuration)
	log.Printf("[%s] WAV loaded and decoded: %d samples (%.2fs audio) in %v", requestID, len(samples), audioDuration, decodeDuration)

	// Create processing context
	contextStart := time.Now()
	context, err := model.NewContext()
	if err != nil {
		log.Printf("[%s] Error creating context: %v", requestID, err)
		http.Error(w, "Internal server error", http.StatusInternalServerError)
		return
	}
	contextDuration := time.Since(contextStart)
	capture.timing("context", contextDuration)
	log.Printf("[%s] Context created in %v", requestID, contextDuration)

	// Set transcription parameters
	context.SetLanguage("en")
	context.SetTranslate(false)
	capture.context(context)

	// Process the audio samples
	log.Printf("[%s] Starting Whisper inference...", requestID)
	inferenceStart := time.Now()
	err = context.Process(samples, nil, nil, nil)
	if err != nil {
		log.Printf("[%s] Error processing audio: %v", requestID, err)
		http.Error(w, "Error transcribing audio", http.StatusInternalServerError)
		return
	}
	inferenceDuration := time.Since(inferenceStart)
	capture.timing("inference", inferenceDuration)
	realTimeFactor := inferenceDuration.Seconds() / audioDuration
	log.Printf("[%s] Whisper inference complete in %v (%.2fx realtime)", requestID, inferenceDuration, realTimeFactor)

	// Get transcription text
	segmentStart := time.Now()
	text := ""
	segmentCount := 0
	for {
		segment, err := context.NextSegment()
		if err == io.EOF {
			break
		}
		if err != nil {
			log.Printf("[%s] Error reading segment: %v", requestID, err)
			break
		}

		capture.segment(segment)

		// Add segment text
		segmentText := segment.Text

		// Add space between segments if needed
		if segmentCount > 0 && len(text) > 0 && len(segmentText) > 0 {
			// Check if we need a space (text doesn't end with space, segment doesn't start with space)
			lastChar := text[len(text)-1]
			firstChar := segmentText[0]
			if lastChar != ' ' && lastChar != '\n' && firstChar != ' ' {
				text += " "
			}
		}

		text += segmentText
		segmentCount++
	}
	extractDuration := time.Since(segmentStart)
	capture.timing("extract", extractDuration)
	log.Printf("[%s] Extracted %d segments (%d chars) in %v", requestID, segmentCount, len(text), extractDuration)
	capture.rawText(text)

	totalDuration := time.Since(requestStart)
	capture.timing("total", totalDuration)
	log.Printf("[%s] === SUMMARY === Total: %v | Upload: %v | Decode: %v | Context: %v | Inference: %v (%.2fx RT) | Extract: %v",
		requestID, totalDuration,
		uploadDuration,
		decodeDuration,
		contextDuration,
		inferenceDuration, realTimeFactor,
		extractDuration)

	// Return transcription as plain text
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	fmt.Fprint(w, cleanTranscription(text))
}

// Sentinels Whisper emits when nothing was said. They only mean "nothing" when they
// are the entire transcription; mid-sentence they are left alone.
var sentinelPattern = regexp.MustCompile(`(?i)^(?:\s*[\[(](?:BLANK_AUDIO|silence|inaudible)[\])])+\s*$`)

// Background noise annotations, stripped wherever they appear.
// beep is the Pi's record-start buzzer, the rest is ambient pickup around the speaker.
var noisePattern = regexp.MustCompile(`(?i)[\[(](?:beep|clicks?|typing|music(?: playing)?)[\])]`)

// Pause filler: "..." and the unicode ellipsis.
var fillerPattern = regexp.MustCompile(`\.{2,}|…`)

// A space stranded before punctuation.
var strandedSpacePattern = regexp.MustCompile(` ([.,!?;:])`)

// cleanTranscription strips annotations and filler so the result can be typed verbatim.
// An empty result is intentional and means nothing should be typed.
// Each pass replaces with a space freely; the final pass tidies all whitespace.
func cleanTranscription(text string) string {
	text = noisePattern.ReplaceAllString(text, " ")
	if sentinelPattern.MatchString(text) {
		return ""
	}
	text = fillerPattern.ReplaceAllString(text, " ")

	text = strings.Join(strings.Fields(text), " ")
	return strandedSpacePattern.ReplaceAllString(text, "$1")
}

// loadWAV reads a WAV file and returns the audio samples as float32
func loadWAV(filename string) ([]float32, error) {
	file, err := os.Open(filename)
	if err != nil {
		return nil, err
	}
	defer file.Close()

	// Read WAV header (44 bytes for standard WAV)
	header := make([]byte, 44)
	_, err = io.ReadFull(file, header)
	if err != nil {
		return nil, fmt.Errorf("error reading WAV header: %v", err)
	}

	// Verify RIFF header
	if string(header[0:4]) != "RIFF" || string(header[8:12]) != "WAVE" {
		return nil, fmt.Errorf("not a valid WAV file")
	}

	// Read audio data
	data, err := io.ReadAll(file)
	if err != nil {
		return nil, fmt.Errorf("error reading audio data: %v", err)
	}

	// Convert 16-bit PCM to float32
	samples := make([]float32, len(data)/2)
	for i := 0; i < len(samples); i++ {
		sample := int16(binary.LittleEndian.Uint16(data[i*2 : i*2+2]))
		samples[i] = float32(sample) / 32768.0
	}

	return samples, nil
}
