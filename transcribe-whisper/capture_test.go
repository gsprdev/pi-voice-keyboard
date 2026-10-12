package main

import (
	"encoding/json"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// useCaptureDir enables debug capture into a fresh directory for one test.
func useCaptureDir(t *testing.T, keep int) string {
	dir := t.TempDir()
	captureDir, captureKeep = dir, keep
	t.Cleanup(func() { captureDir, captureKeep = "", 0 })
	return dir
}

func readMeta(t *testing.T, dir string) captureMeta {
	b, err := os.ReadFile(filepath.Join(dir, "meta.json"))
	if err != nil {
		t.Fatal(err)
	}
	var meta captureMeta
	if err := json.Unmarshal(b, &meta); err != nil {
		t.Fatal(err)
	}
	return meta
}

func TestCaptureDisabled(t *testing.T) {
	if c := newCapture("x", httptest.NewRequest("POST", "/transcribe", nil)); c != nil {
		t.Fatalf("newCapture with no DEBUG_CAPTURE_DIR = %v, want nil", c)
	}
}

func TestCaptureSuccess(t *testing.T) {
	useCaptureDir(t, 5)
	req := httptest.NewRequest("POST", "/transcribe", nil)
	req.Header.Set("User-Agent", "voice-keyboard-pico")
	c := newCapture("ok", req)

	// 16kHz mono 16-bit header followed by two samples
	header := []byte("RIFF\x00\x00\x00\x00WAVEfmt \x10\x00\x00\x00\x01\x00\x01\x00\x80\x3e\x00\x00\x00\x7d\x00\x00\x02\x00\x10\x00data\x04\x00\x00\x00")
	c.audioWriter(&strings.Builder{}).Write(append(header, 1, 0, 2, 0))
	c.rawText(" Hello world. [Music]")
	w := c.responseWriter(httptest.NewRecorder())
	w.Write([]byte("Hello world."))
	c.finish()

	transcript, err := os.ReadFile(filepath.Join(c.dir, "transcript.txt"))
	if err != nil || string(transcript) != "Hello world." {
		t.Errorf("transcript.txt = %q, %v", transcript, err)
	}
	meta := readMeta(t, c.dir)
	if meta.Status != 200 || meta.RawText != " Hello world. [Music]" || meta.AudioBytes != 48 {
		t.Errorf("meta = status %d, raw %q, %d bytes", meta.Status, meta.RawText, meta.AudioBytes)
	}
	if meta.WAV == nil || *meta.WAV != (wavInfo{Channels: 1, SampleRate: 16000, BitsPerSample: 16}) {
		t.Errorf("meta.wav = %+v", meta.WAV)
	}
	if got := meta.Headers["User-Agent"]; len(got) != 1 || got[0] != "voice-keyboard-pico" {
		t.Errorf("meta.headers User-Agent = %v", got)
	}
}

func TestCaptureFailedRequest(t *testing.T) {
	dir := useCaptureDir(t, 5)
	req := httptest.NewRequest("POST", "/transcribe", strings.NewReader("RIFFxxxxNOTAWAV"))
	req.Header.Set("Content-Type", "audio/wav")
	rec := httptest.NewRecorder()
	transcribeHandler(rec, req)
	if rec.Code != 400 {
		t.Fatalf("status = %d, want 400", rec.Code)
	}

	entries, _ := os.ReadDir(dir)
	if len(entries) != 1 {
		t.Fatalf("got %d captures, want 1", len(entries))
	}
	capDir := filepath.Join(dir, entries[0].Name())
	if _, err := os.Stat(filepath.Join(capDir, "transcript.txt")); !os.IsNotExist(err) {
		t.Errorf("transcript.txt exists for a failed request")
	}
	if audio, _ := os.ReadFile(filepath.Join(capDir, "audio.wav")); string(audio) != "RIFFxxxxNOTAWAV" {
		t.Errorf("audio.wav = %q", audio)
	}
	if meta := readMeta(t, capDir); meta.Status != 400 || meta.WAV != nil {
		t.Errorf("meta = status %d, wav %+v", meta.Status, meta.WAV)
	}
}

func TestCapturePrune(t *testing.T) {
	dir := useCaptureDir(t, 2)
	names := []string{
		"20261010-045829.001-1",
		"20261010-045829.002-2",
		"20261010-045829.003-3",
		"keep-me",
	}
	for _, name := range names {
		os.Mkdir(filepath.Join(dir, name), 0o700)
	}
	pruneCaptures()

	var got []string
	entries, _ := os.ReadDir(dir)
	for _, e := range entries {
		got = append(got, e.Name())
	}
	want := "20261010-045829.002-2 20261010-045829.003-3 keep-me"
	if strings.Join(got, " ") != want {
		t.Errorf("after prune = %v, want %s", got, want)
	}
}
