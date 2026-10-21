package main

import (
	"cmp"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"regexp"
	"runtime"
	"strings"
	"sync"
	"time"

	// The low-level binding, since pkg/whisper can't select beam search or read back its settings
	whisper "github.com/ggerganov/whisper.cpp/bindings/go"
)

var (
	model *whisper.Context
	// liveParams are what the server transcribes with
	liveParams params
)

// defaultModel is used when MODEL is unset, by the service and replay alike.
const defaultModel = "medium.en"

// maxUpload bounds the request body, which is held in memory: about 30 minutes of 16kHz audio.
const maxUpload = 64 << 20

func getenv(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

// modelFile is where `mise run model` puts the named upstream model, relative to the repo root.
func modelFile(name string) string {
	return "speech-models/ggml-" + name + ".bin"
}

func main() {
	command(os.Args[1:])
}

// cmdServe runs the transcription service, configured by environment variables.
func cmdServe(args []string) error {
	if len(args) > 0 {
		return errors.New("serve takes no arguments; configure it with PORT, MODEL, MODEL_LANGUAGE and CORPUS_DB")
	}

	port := getenv("PORT", "8080")
	modelName := getenv("MODEL", defaultModel)

	live = openLiveCorpus().startProcess("serve", modelName)
	var err error
	if model, err = loadModel(modelFile(modelName), live); err != nil {
		return err
	}
	defer model.Whisper_free()
	if liveParams.Language, err = language(model, getenv("MODEL_LANGUAGE", "en")); err != nil {
		return fmt.Errorf("MODEL_LANGUAGE: %w", err)
	}

	// Setup HTTP routes
	http.HandleFunc("/transcribe", transcribeHandler)
	http.HandleFunc("/health", healthHandler)

	// Start server
	addr := ":" + port
	log.Printf("Starting transcription service on %s", addr)
	log.Printf("POST /transcribe - Audio transcription endpoint")
	log.Printf("GET  /health     - Health check endpoint")

	return fmt.Errorf("server failed: %w", http.ListenAndServe(addr, nil))
}

// language is the language setting for model m: English-only models have none.
// It is checked up front, so a bad one stops the service rather than failing every request.
func language(m *whisper.Context, lang string) (string, error) {
	if m.Whisper_is_multilingual() == 0 {
		return "", nil
	}
	if lang != "auto" && m.Whisper_lang_id(lang) < 0 {
		return "", fmt.Errorf("unsupported language %q", lang)
	}
	return lang, nil
}

// loadModel loads a Whisper model, recording how that went in the process.
func loadModel(path string, proc *process) (*whisper.Context, error) {
	info, err := os.Stat(path)
	if err != nil {
		err = fmt.Errorf("model file not found: %s (download with `mise run model`)", path)
		proc.loaded(0, err)
		return nil, err
	}
	log.Printf("Model file: %s (%.2f MB)", path, float64(info.Size())/(1024*1024))

	log.Printf("Loading whisper model...")
	startLoad := time.Now()
	m := whisper.Whisper_init(path)
	loadDuration := time.Since(startLoad)
	if m == nil {
		err = fmt.Errorf("failed to load model %s", path)
	}
	proc.loaded(loadDuration, err)
	if err != nil {
		return nil, err
	}
	log.Printf("Model loaded successfully in %v", loadDuration)
	return m, nil
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
	requestID := fmt.Sprintf("%d", requestStart.UnixNano())

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

	rec := live.newRun(liveParams)
	fail := func(status int, msg string, err error) {
		log.Printf("[%s] %s: %v", requestID, msg, err)
		rec.Error = fmt.Sprintf("%s: %v", msg, err)
		http.Error(w, msg, status)
	}

	ioStart := time.Now()
	audio, err := io.ReadAll(http.MaxBytesReader(w, r.Body, maxUpload))
	uploadDuration := time.Since(ioStart)
	rec.Timings["upload"] = ms(uploadDuration)
	tooLarge := errors.As(err, new(*http.MaxBytesError))
	if tooLarge {
		audio = nil // only its start arrived, which isn't worth its space in the corpus
	}
	// Recorded whatever the outcome, so failed requests keep their audio too
	defer live.record(requestStart, r, audio, rec)
	if tooLarge {
		fail(http.StatusRequestEntityTooLarge, "Audio too large", err)
		return
	}
	if err != nil {
		fail(http.StatusBadRequest, "Error reading audio data", err)
		return
	}
	log.Printf("[%s] Audio upload complete: %d bytes in %v", requestID, len(audio), uploadDuration)

	// Convert 16-bit PCM to float32 samples
	loadStart := time.Now()
	samples, err := decodeWAV(audio)
	if err != nil {
		fail(http.StatusBadRequest, "Error reading audio file", err)
		return
	}
	audioDuration := float64(len(samples)) / whisper.SampleRate
	decodeDuration := time.Since(loadStart)
	rec.Timings["decode"] = ms(decodeDuration)
	log.Printf("[%s] WAV loaded and decoded: %d samples (%.2fs audio) in %v", requestID, len(samples), audioDuration, decodeDuration)

	log.Printf("[%s] Starting Whisper inference...", requestID)
	if err := rec.transcribe(model, samples); err != nil {
		fail(http.StatusInternalServerError, "Error transcribing audio", err)
		return
	}

	t := rec.Timings
	t["total"] = ms(time.Since(requestStart))
	log.Printf("[%s] === SUMMARY === Total: %.0fms | Upload: %.0fms | Decode: %.0fms | Inference: %.0fms (%.2fx RT) | Extract: %.0fms | %d segments, %d chars",
		requestID, t["total"], t["upload"], t["decode"],
		t["inference"], t["inference"]/1000/audioDuration, t["extract"],
		len(rec.Segments), len(rec.RawText))

	// Return transcription as plain text
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	fmt.Fprint(w, rec.Text)
}

func ms(d time.Duration) float64 {
	return float64(d.Microseconds()) / 1000
}

// params are the Whisper settings a run used. Zero values leave Whisper's defaults.
type params struct {
	Language    string  `json:"language,omitempty"`
	BeamSize    int     `json:"beam_size,omitempty"`
	Temperature float32 `json:"temperature,omitempty"`
	Prompt      string  `json:"prompt,omitempty"`
	Threads     uint    `json:"threads,omitempty"`
	AudioCtx    int     `json:"audio_ctx,omitempty"` // encoder frames: 50 a second, up to 1500
}

type segment struct {
	StartMs int64   `json:"start_ms"`
	EndMs   int64   `json:"end_ms"`
	Text    string  `json:"text"`
	Tokens  []token `json:"tokens"`
}

type token struct {
	Text string  `json:"text"`
	P    float32 `json:"p"`
}

// whisperMu serializes transcriptions, which concurrent requests would otherwise interleave.
var whisperMu sync.Mutex

// transcribe runs Whisper on samples with r.Params, filling in r's results, timings,
// and what it actually used and ran under.
// The server and replay both come through here, so their runs are comparable.
func (r *run) transcribe(m *whisper.Context, samples []float32) error {
	// The context holds one transcription's state at a time, read back below
	whisperMu.Lock()
	defer whisperMu.Unlock()
	if len(samples) == 0 {
		return errors.New("no audio")
	}
	p := r.Params
	strategy := whisper.SAMPLING_GREEDY // which ignores beam_size
	if p.BeamSize > 0 {
		strategy = whisper.SAMPLING_BEAM_SEARCH
	}
	wp := m.Whisper_full_default_params(strategy)
	// The settings the service has always run with, from the binding's pkg/whisper
	wp.SetTranslate(false)
	wp.SetPrintSpecial(false)
	wp.SetPrintProgress(false)
	wp.SetPrintRealtime(false)
	wp.SetPrintTimestamps(false)
	wp.SetNoContext(true)
	wp.SetThreads(cmp.Or(int(p.Threads), runtime.NumCPU()))
	if p.Language != "" {
		id := -1 // auto-detect
		if p.Language != "auto" {
			if id = m.Whisper_lang_id(p.Language); id < 0 {
				return fmt.Errorf("unsupported language %q", p.Language)
			}
		}
		wp.SetLanguage(id)
	}
	if p.BeamSize > 0 {
		wp.SetBeamSize(p.BeamSize)
	}
	if p.Temperature > 0 {
		wp.SetTemperature(p.Temperature)
	}
	if p.Prompt != "" {
		wp.SetInitialPrompt(p.Prompt)
	}
	if p.AudioCtx > 0 {
		wp.SetAudioCtx(p.AudioCtx)
	}
	r.Effective = effective{AudioCtx: cmp.Or(p.AudioCtx, m.Whisper_n_audio_ctx()), Threads: uint(wp.Threads())}
	r.Conditions = readConditions()

	start := time.Now()
	if err := m.Whisper_full(wp, samples, nil, nil, nil); err != nil {
		return err
	}
	r.Timings["inference"] = ms(time.Since(start))
	r.Conditions.DeviceFreeMB = deviceFreeMB(usedDevice)

	start = time.Now()
	text := ""
	for i := range m.Whisper_full_n_segments() {
		seg := segment{
			StartMs: m.Whisper_full_get_segment_t0(i) * 10, // in 10ms units
			EndMs:   m.Whisper_full_get_segment_t1(i) * 10,
			Text:    strings.TrimSpace(m.Whisper_full_get_segment_text(i)),
		}
		for j := range m.Whisper_full_n_tokens(i) {
			seg.Tokens = append(seg.Tokens, token{Text: m.Whisper_full_get_token_text(i, j), P: m.Whisper_full_get_token_p(i, j)})
		}
		r.Tokens += len(seg.Tokens)
		r.Segments = append(r.Segments, seg)

		// Add a space between segments if neither side has one
		if len(text) > 0 && len(seg.Text) > 0 {
			lastChar := text[len(text)-1]
			if lastChar != ' ' && lastChar != '\n' && seg.Text[0] != ' ' {
				text += " "
			}
		}
		text += seg.Text
	}
	r.Timings["extract"] = ms(time.Since(start))
	r.RawText = text
	r.Text = cleanTranscription(text)
	r.Compression = compressionRatio(r.Text)
	return nil
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

// wavHeader is the size of a standard WAV header, as Whisper's input comes with.
const wavHeader = 44

// decodeWAV returns the audio samples of a 16-bit PCM WAV as float32
func decodeWAV(data []byte) ([]float32, error) {
	if len(data) < wavHeader {
		return nil, fmt.Errorf("error reading WAV header: %d bytes", len(data))
	}
	if string(data[0:4]) != "RIFF" || string(data[8:12]) != "WAVE" {
		return nil, fmt.Errorf("not a valid WAV file")
	}
	data = data[wavHeader:]

	samples := make([]float32, len(data)/2)
	for i := range samples {
		sample := int16(binary.LittleEndian.Uint16(data[i*2 : i*2+2]))
		samples[i] = float32(sample) / 32768.0
	}
	return samples, nil
}

// encodeWAV is the inverse of decodeWAV: 16kHz mono 16-bit PCM.
func encodeWAV(samples []float32) []byte {
	const rate = whisper.SampleRate
	n := uint32(len(samples) * 2)
	b := make([]byte, 0, wavHeader+n)
	le := binary.LittleEndian
	b = append(b, "RIFF"...)
	b = le.AppendUint32(b, 36+n)
	b = append(b, "WAVEfmt "...)
	b = le.AppendUint32(b, 16)     // fmt chunk size
	b = le.AppendUint16(b, 1)      // PCM
	b = le.AppendUint16(b, 1)      // mono
	b = le.AppendUint32(b, rate)   // sample rate
	b = le.AppendUint32(b, rate*2) // byte rate
	b = le.AppendUint16(b, 2)      // block align
	b = le.AppendUint16(b, 16)     // bits per sample
	b = append(b, "data"...)
	b = le.AppendUint32(b, n)
	for _, s := range samples {
		b = le.AppendUint16(b, uint16(int16(max(-1, min(1, s))*32767)))
	}
	return b
}
