package main

// Debug capture: when DEBUG_CAPTURE_DIR is set, every /transcribe request
// is kept on disk (audio as received, response text, and metadata) so that
// mistranscriptions can be replayed against other models and parameters.
// Only the newest DEBUG_CAPTURE_KEEP captures are retained.

import (
	"encoding/binary"
	"encoding/json"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/ggerganov/whisper.cpp/bindings/go/pkg/whisper"
)

const defaultCaptureKeep = 50

var (
	captureDir  string
	captureKeep int
	pruneMu     sync.Mutex
)

// initCapture reads the debug capture configuration from the environment.
func initCapture() {
	captureDir = os.Getenv("DEBUG_CAPTURE_DIR")
	if captureDir == "" {
		return
	}

	captureKeep = defaultCaptureKeep
	if v := os.Getenv("DEBUG_CAPTURE_KEEP"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 1 {
			log.Fatalf("DEBUG_CAPTURE_KEEP must be a positive integer, got %q", v)
		}
		captureKeep = n
	}

	if err := os.MkdirAll(captureDir, 0o700); err != nil {
		log.Fatalf("Cannot create DEBUG_CAPTURE_DIR %s: %v", captureDir, err)
	}
	log.Printf("DEBUG CAPTURE ENABLED: keeping the last %d requests (audio + transcript) in %s", captureKeep, captureDir)
}

// captureMeta is written as meta.json alongside each capture.
type captureMeta struct {
	ID         string              `json:"id"`
	ReceivedAt time.Time           `json:"received_at"`
	RemoteAddr string              `json:"remote_addr"`
	Headers    map[string][]string `json:"headers"`

	Status   int    `json:"status"`
	Response string `json:"response"`

	AudioBytes   int64     `json:"audio_bytes"`
	WAV          *wavInfo  `json:"wav,omitempty"`
	Samples      int       `json:"samples"`
	AudioSeconds float64   `json:"audio_seconds"`
	Model        modelInfo `json:"model"`
	Language     string    `json:"language,omitempty"`
	SystemInfo   string    `json:"system_info,omitempty"`

	TimingsMs map[string]float64 `json:"timings_ms"`
	Segments  []captureSegment   `json:"segments"`
}

type wavInfo struct {
	Channels      uint16 `json:"channels"`
	SampleRate    uint32 `json:"sample_rate"`
	BitsPerSample uint16 `json:"bits_per_sample"`
}

type modelInfo struct {
	Path     string    `json:"path"`
	Bytes    int64     `json:"bytes"`
	Modified time.Time `json:"modified"`
}

type captureSegment struct {
	StartMs int64          `json:"start_ms"`
	EndMs   int64          `json:"end_ms"`
	Text    string         `json:"text"`
	Tokens  []captureToken `json:"tokens"`
}

type captureToken struct {
	Text string  `json:"text"`
	P    float32 `json:"p"`
}

// capture holds one request's debug materials. A nil *capture is valid and
// does nothing, so the handler can call its methods unconditionally.
type capture struct {
	dir   string
	audio *os.File
	meta  captureMeta
}

// newCapture starts a capture for this request, or returns nil when debug
// capture is disabled or the capture directory cannot be created.
func newCapture(requestID string, r *http.Request) *capture {
	if captureDir == "" {
		return nil
	}
	now := time.Now()
	// The name sorts chronologically, which pruning relies on.
	dir := filepath.Join(captureDir, now.Format("20060102-150405.000")+"-"+requestID)
	if err := os.Mkdir(dir, 0o700); err != nil {
		log.Printf("[%s] Debug capture disabled for request: %v", requestID, err)
		return nil
	}
	audio, err := os.Create(filepath.Join(dir, "audio.wav"))
	if err != nil {
		log.Printf("[%s] Debug capture disabled for request: %v", requestID, err)
		os.RemoveAll(dir)
		return nil
	}

	c := &capture{
		dir:   dir,
		audio: audio,
		meta: captureMeta{
			ID:         requestID,
			ReceivedAt: now,
			RemoteAddr: r.RemoteAddr,
			Headers:    r.Header.Clone(),
			Model:      modelInfo{Path: modelPath},
			TimingsMs:  map[string]float64{},
		},
	}
	if info, err := os.Stat(modelPath); err == nil {
		c.meta.Model.Bytes = info.Size()
		c.meta.Model.Modified = info.ModTime()
	}
	return c
}

// audioWriter wraps w so that the uploaded audio is also saved to the capture.
func (c *capture) audioWriter(w io.Writer) io.Writer {
	if c == nil {
		return w
	}
	return io.MultiWriter(w, c.audio)
}

func (c *capture) timing(name string, d time.Duration) {
	if c == nil {
		return
	}
	c.meta.TimingsMs[name] = float64(d.Microseconds()) / 1000
}

func (c *capture) decoded(samples []float32, audioSeconds float64) {
	if c == nil {
		return
	}
	c.meta.Samples = len(samples)
	c.meta.AudioSeconds = audioSeconds
}

func (c *capture) context(ctx whisper.Context) {
	if c == nil {
		return
	}
	c.meta.Language = ctx.Language()
	c.meta.SystemInfo = ctx.SystemInfo()
}

func (c *capture) segment(s whisper.Segment) {
	if c == nil {
		return
	}
	seg := captureSegment{
		StartMs: s.Start.Milliseconds(),
		EndMs:   s.End.Milliseconds(),
		Text:    s.Text,
		Tokens:  make([]captureToken, 0, len(s.Tokens)),
	}
	for _, t := range s.Tokens {
		seg.Tokens = append(seg.Tokens, captureToken{Text: t.Text, P: t.P})
	}
	c.meta.Segments = append(c.meta.Segments, seg)
}

// responseWriter wraps w to record the status and body sent to the client.
func (c *capture) responseWriter(w http.ResponseWriter) http.ResponseWriter {
	if c == nil {
		return w
	}
	return &recordingWriter{ResponseWriter: w, c: c}
}

type recordingWriter struct {
	http.ResponseWriter
	c    *capture
	body strings.Builder
}

func (rw *recordingWriter) WriteHeader(status int) {
	rw.c.meta.Status = status
	rw.ResponseWriter.WriteHeader(status)
}

func (rw *recordingWriter) Write(b []byte) (int, error) {
	if rw.c.meta.Status == 0 {
		rw.c.meta.Status = http.StatusOK
	}
	rw.body.Write(b)
	rw.c.meta.Response = rw.body.String()
	return rw.ResponseWriter.Write(b)
}

// finish writes transcript.txt and meta.json, then prunes old captures.
// Failures are logged and never affect the response.
func (c *capture) finish() {
	if c == nil {
		return
	}
	c.audio.Close()
	if info, err := os.Stat(c.audio.Name()); err == nil {
		c.meta.AudioBytes = info.Size()
	}
	c.meta.WAV = readWAVInfo(c.audio.Name())

	if c.meta.Status == http.StatusOK {
		if err := os.WriteFile(filepath.Join(c.dir, "transcript.txt"), []byte(c.meta.Response), 0o600); err != nil {
			log.Printf("[%s] Debug capture: %v", c.meta.ID, err)
		}
	}
	meta, err := json.MarshalIndent(c.meta, "", "  ")
	if err == nil {
		err = os.WriteFile(filepath.Join(c.dir, "meta.json"), append(meta, '\n'), 0o600)
	}
	if err != nil {
		log.Printf("[%s] Debug capture: %v", c.meta.ID, err)
	}
	log.Printf("[%s] Debug capture saved to %s", c.meta.ID, c.dir)

	pruneCaptures()
}

// readWAVInfo reads the format fields of a canonical 44-byte WAV header,
// or returns nil when the file is too short to have one.
func readWAVInfo(path string) *wavInfo {
	f, err := os.Open(path)
	if err != nil {
		return nil
	}
	defer f.Close()
	header := make([]byte, 44)
	if _, err := io.ReadFull(f, header); err != nil {
		return nil
	}
	return &wavInfo{
		Channels:      binary.LittleEndian.Uint16(header[22:24]),
		SampleRate:    binary.LittleEndian.Uint32(header[24:28]),
		BitsPerSample: binary.LittleEndian.Uint16(header[34:36]),
	}
}

// isCaptureName reports whether name looks like a directory made by
// newCapture, so pruning never touches anything else in captureDir.
func isCaptureName(name string) bool {
	const layout = "20060102-150405.000"
	if len(name) <= len(layout) || name[len(layout)] != '-' {
		return false
	}
	_, err := time.Parse(layout, name[:len(layout)])
	return err == nil
}

// pruneCaptures removes the oldest capture directories beyond captureKeep.
func pruneCaptures() {
	pruneMu.Lock()
	defer pruneMu.Unlock()

	entries, err := os.ReadDir(captureDir)
	if err != nil {
		log.Printf("Debug capture prune: %v", err)
		return
	}
	var dirs []string
	for _, e := range entries {
		if e.IsDir() && isCaptureName(e.Name()) {
			dirs = append(dirs, e.Name())
		}
	}
	sort.Strings(dirs)
	for len(dirs) > captureKeep {
		if err := os.RemoveAll(filepath.Join(captureDir, dirs[0])); err != nil {
			log.Printf("Debug capture prune: %v", err)
		}
		dirs = dirs[1:]
	}
}
