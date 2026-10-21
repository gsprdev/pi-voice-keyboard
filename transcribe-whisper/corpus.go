package main

// The corpus keeps transcription requests in one SQLite file, for evaluating and replaying them.
// A sample is the audio exactly as received, and never changes.
// Each transcription of it, live or replayed, is an appended run.
// A run belongs to a process (a server or a replay), which records the build, host, device and model,
// and how loading the model went.
// A human-verified reference transcript labels a sample, so every run against it can be scored.
//
// The server records into it when CORPUS_DB is set, and the subcommands in cli.go read and add to it.
// Several processes (builds, backends, models) may share one file, as long as it is on a local disk.

/*
#include <ggml-backend.h>
*/
import "C"

import (
	"bytes"
	"compress/zlib"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"runtime/debug"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	whisper "github.com/ggerganov/whisper.cpp/bindings/go"
	_ "github.com/mattn/go-sqlite3"
)

const (
	defaultCorpusMB = 1024 // about 8.7 hours of 16kHz audio
	schemaVersion   = 1
)

const schema = `
CREATE TABLE sample (
	id          INTEGER PRIMARY KEY,
	received_at TEXT NOT NULL,
	remote_addr TEXT,
	headers     TEXT,                   -- JSON
	audio_bytes INTEGER NOT NULL,       -- of its audio, for its duration without reading it
	reference   TEXT,                   -- human-verified transcript, NULL until labeled
	notes       TEXT,
	labeled_at  TEXT,
	keep        INTEGER NOT NULL DEFAULT 0  -- exempt from eviction and purge; labeling sets it
);

-- Recordings have a table of their own, since SQLite reads through a large value to reach the
-- columns stored after it: scanning samples by keep or reference read 15 times as much with them inline.
CREATE TABLE audio (
	sample_id INTEGER PRIMARY KEY REFERENCES sample ON DELETE CASCADE,
	wav       BLOB NOT NULL             -- exactly as received
);

CREATE TABLE process (
	id           INTEGER PRIMARY KEY,
	started_at   TEXT NOT NULL,
	kind         TEXT NOT NULL,         -- serve or replay
	build        TEXT NOT NULL,         -- git describe of this repo and whisper.cpp, Go binding and Go versions
	backend      TEXT NOT NULL,         -- what whisper.cpp was built for: cuda, vulkan, metal, cpu
	build_host   TEXT NOT NULL,         -- GGML_NATIVE tunes the build for this machine's CPU
	host         TEXT NOT NULL,         -- JSON hostInfo, with device memory before and after loading
	device       TEXT NOT NULL,         -- the device whisper runs on
	system_info  TEXT,                  -- whisper's
	model        TEXT NOT NULL,
	model_sha256 TEXT,
	load_ms      REAL,
	load_error   TEXT                   -- 'loading' until the model is loaded, NULL when it loaded
);

CREATE TABLE run (
	id          INTEGER PRIMARY KEY,
	sample_id   INTEGER NOT NULL REFERENCES sample ON DELETE CASCADE,
	process_id  INTEGER NOT NULL REFERENCES process,
	seq         INTEGER NOT NULL,       -- numbered within the process
	created_at  TEXT NOT NULL,
	params      TEXT NOT NULL,          -- JSON, the settings asked for; runs are compared by these
	effective   TEXT,                   -- JSON, what was actually used, which may vary by clip
	conditions  TEXT,                   -- JSON, the machine's state: power, profile, load, device memory
	error       TEXT,                   -- 'incomplete' until finished, so a crash leaves a trace; NULL on success
	raw_text    TEXT,                   -- Whisper's text before cleanTranscription
	text        TEXT,                   -- what was, or would have been, typed
	segments    TEXT,                   -- JSON, with token probabilities
	tokens      INTEGER,
	compression REAL,                   -- of text: well above 2 means repetition, such as a decoding loop
	timings     TEXT                    -- JSON, milliseconds
);
CREATE INDEX run_sample ON run (sample_id);
`

// Set by `mise run build` with -ldflags.
var (
	build     = "unknown" // git describe of this repo and whisper.cpp
	backend   = "unknown" // WHISPER_GPU, what whisper.cpp was built for
	buildHost = "unknown"
)

// Timestamps are stored in UTC with a fixed width, so they sort as text.
const timeLayout = "2006-01-02T15:04:05.000Z"

func stamp(t time.Time) string { return t.UTC().Format(timeLayout) }

func now() string { return stamp(time.Now()) }

type corpus struct {
	db       *sql.DB
	maxBytes int64
}

// openCorpus opens the corpus at path, creating it if needed.
// Its budget comes from CORPUS_MAX_MB.
func openCorpus(path string) (*corpus, error) {
	maxMB := defaultCorpusMB
	if v := os.Getenv("CORPUS_MAX_MB"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 1 {
			return nil, fmt.Errorf("CORPUS_MAX_MB must be a positive integer, got %q", v)
		}
		maxMB = n
	}

	// Created private up front: it holds recordings of whatever was said, and SQLite would follow the umask
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return nil, err
	}
	f, err := os.OpenFile(path, os.O_RDONLY|os.O_CREATE, 0o600)
	if err != nil {
		return nil, err
	}
	f.Close()

	db, err := sql.Open("sqlite3", path+"?_journal_mode=WAL&_busy_timeout=10000&_foreign_keys=1&_txlock=immediate")
	if err != nil {
		return nil, err
	}
	c := &corpus{db: db, maxBytes: int64(maxMB) << 20}
	if err := c.initSchema(); err != nil {
		db.Close()
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	return c, nil
}

// initSchema creates the schema in a new database, and refuses one from another schema version,
// since builds from different worktrees may share the file.
func (c *corpus) initSchema() error {
	tx, err := c.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var v int
	if err := tx.QueryRow("PRAGMA user_version").Scan(&v); err != nil {
		return err
	}
	switch v {
	case schemaVersion:
		return nil
	case 0:
		if _, err := tx.Exec(schema + fmt.Sprintf("PRAGMA user_version = %d;", schemaVersion)); err != nil {
			return err
		}
		return tx.Commit()
	default:
		return fmt.Errorf("corpus schema version %d, this build expects %d", v, schemaVersion)
	}
}

// process is a server or replay recording into the corpus. A nil *process records nothing.
type process struct {
	c       *corpus
	id      int64
	host    hostInfo
	seq     atomic.Int64
	model   string
	hashing sync.WaitGroup // the model's sha256, after it loads
}

// startProcess records a process that is about to load a model. It returns nil when c is nil.
func (c *corpus) startProcess(kind, modelName string) *process {
	if c == nil {
		return nil
	}
	p := &process{c: c, model: modelName}
	p.host, usedDevice = readHostInfo()
	host, _ := json.Marshal(p.host)
	res, err := c.db.Exec(`INSERT INTO process (started_at, kind, build, backend, build_host, host, device, system_info,
		model, load_error) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 'loading')`,
		now(), kind, buildString(), backend, buildHost, string(host), deviceName(usedDevice),
		whisper.Whisper_print_system_info(), modelName)
	if err != nil {
		log.Fatalf("Corpus: %v", err)
	}
	p.id, _ = res.LastInsertId()
	return p
}

// loaded records how loading the model went and the device memory it took,
// then hashes the model in the background, from the page cache and outside the timed load.
func (p *process) loaded(d time.Duration, loadErr error) {
	if p == nil {
		return
	}
	for i := range p.host.Devices {
		p.host.Devices[i].FreeAfterMB = deviceFreeMB(C.ggml_backend_dev_get(C.size_t(i)))
	}
	host, _ := json.Marshal(p.host)
	msg := ""
	if loadErr != nil {
		msg = loadErr.Error()
	}
	if _, err := p.c.db.Exec(`UPDATE process SET load_ms = ?, load_error = ?, host = ? WHERE id = ?`,
		ms(d), nullable(msg), string(host), p.id); err != nil {
		log.Printf("Corpus: %v", err)
	}
	p.hashing.Go(func() {
		start := time.Now()
		sha, err := fileSHA256(modelFile(p.model))
		if err != nil {
			return
		}
		log.Printf("Model sha256 %s, hashed in %v", sha[:12], time.Since(start))
		if _, err := p.c.db.Exec(`UPDATE process SET model_sha256 = ? WHERE id = ?`, sha, p.id); err != nil {
			log.Printf("Corpus: %v", err)
		}
	})
}

// newRun starts a run with the given params, numbered within the process.
func (p *process) newRun(prm params) *run {
	r := &run{Params: prm, Timings: map[string]float64{}}
	if p != nil {
		r.Seq = p.seq.Add(1)
	}
	return r
}

// buildString identifies the build: this repo and whisper.cpp (the C library), the Go binding, and Go.
// The binding comes from the Go module cache rather than the submodule, so the two can drift apart.
func buildString() string {
	s := build
	if info, ok := debug.ReadBuildInfo(); ok {
		for _, d := range info.Deps {
			if d.Path == "github.com/ggerganov/whisper.cpp/bindings/go" {
				s += " bindings/go:" + d.Version
			}
		}
	}
	return s + " " + runtime.Version()
}

func fileSHA256(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// run is one transcription of a sample.
type run struct {
	Seq         int64
	Params      params
	Effective   effective
	Conditions  conditions
	Error       string
	RawText     string
	Text        string
	Segments    []segment
	Tokens      int
	Compression float64
	Timings     map[string]float64
}

// effective is what a run actually used, where that differs from asking for Whisper's default.
type effective struct {
	AudioCtx int  `json:"audio_ctx"` // encoder frames, 1500 being the full 30 second window
	Threads  uint `json:"threads"`
}

// conditions is the state of the machine during a run, which can sway its speed.
type conditions struct {
	ACOnline     *bool   `json:"ac_online,omitempty"` // absent without a mains power supply, as on most desktops
	Profile      string  `json:"platform_profile,omitempty"`
	Governor     string  `json:"governor,omitempty"`
	Load1        float64 `json:"load1"`
	DeviceFreeMB uint64  `json:"device_free_mb,omitempty"` // after the run
}

func readConditions() conditions {
	var c conditions
	if f := strings.Fields(readFile("/proc/loadavg")); len(f) > 0 {
		c.Load1, _ = strconv.ParseFloat(f[0], 64)
	}
	c.Profile = readFile("/sys/firmware/acpi/platform_profile")
	c.Governor = readFile("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
	supplies, _ := filepath.Glob("/sys/class/power_supply/*")
	for _, s := range supplies {
		if readFile(s+"/type") == "Mains" {
			online := readFile(s+"/online") == "1"
			c.ACOnline = &online
		}
	}
	return c
}

// compressionRatio is how much zlib shrinks text, as Whisper uses to spot repetition.
func compressionRatio(text string) float64 {
	if text == "" {
		return 0
	}
	var b bytes.Buffer
	w := zlib.NewWriter(&b)
	w.Write([]byte(text))
	w.Close()
	return float64(len(text)) / float64(b.Len())
}

// execer is a *sql.DB or *sql.Tx.
type execer interface {
	Exec(query string, args ...any) (sql.Result, error)
}

// startRun records a run as incomplete before it starts, so a crash during it leaves a trace.
func (p *process) startRun(db execer, sampleID int64, r *run) (int64, error) {
	prm, _ := json.Marshal(r.Params)
	res, err := db.Exec(`INSERT INTO run (sample_id, process_id, seq, created_at, params, error) VALUES (?, ?, ?, ?, ?, 'incomplete')`,
		sampleID, p.id, r.Seq, now(), string(prm))
	if err != nil {
		return 0, err
	}
	return res.LastInsertId()
}

func finishRun(db execer, id int64, r *run) error {
	eff, _ := json.Marshal(r.Effective)
	cond, _ := json.Marshal(r.Conditions)
	segments, _ := json.Marshal(r.Segments)
	timings, _ := json.Marshal(r.Timings)
	_, err := db.Exec(`UPDATE run SET effective = ?, conditions = ?, error = ?, raw_text = ?, text = ?,
		segments = ?, tokens = ?, compression = ?, timings = ? WHERE id = ?`,
		string(eff), string(cond), nullable(r.Error), r.RawText, r.Text,
		string(segments), r.Tokens, r.Compression, string(timings), id)
	return err
}

// addSample stores new audio along with its run, already finished.
func (p *process) addSample(received time.Time, remoteAddr string, headers http.Header, audio []byte, r *run) (int64, error) {
	h, _ := json.Marshal(headers)
	if audio == nil {
		audio = []byte{} // NOT NULL even when nothing arrived
	}
	tx, err := p.c.db.Begin()
	if err != nil {
		return 0, err
	}
	defer tx.Rollback()
	res, err := tx.Exec(`INSERT INTO sample (received_at, remote_addr, headers, audio_bytes) VALUES (?, ?, ?, ?)`,
		stamp(received), remoteAddr, string(h), len(audio))
	if err != nil {
		return 0, err
	}
	id, _ := res.LastInsertId()
	if _, err := tx.Exec(`INSERT INTO audio (sample_id, wav) VALUES (?, ?)`, id, audio); err != nil {
		return 0, err
	}
	runID, err := p.startRun(tx, id, r)
	if err == nil {
		err = finishRun(tx, runID, r)
	}
	if err != nil {
		return 0, err
	}
	return id, tx.Commit()
}

// ids returns the ids of samples, oldest first, filtered by a WHERE clause or "" for all.
func (c *corpus) ids(where string) ([]int64, error) {
	rows, err := c.db.Query(`SELECT id FROM sample ` + where + ` ORDER BY id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var ids []int64
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		ids = append(ids, id)
	}
	return ids, rows.Err()
}

// evict deletes the oldest samples, never kept ones, until the corpus fits its budget.
// The size is SQLite's pages in use, so everything stored counts, and freed pages are reused.
func (c *corpus) evict() {
	for {
		used, err := c.usedBytes()
		if err != nil {
			log.Printf("Corpus eviction: %v", err)
			return
		}
		if used <= c.maxBytes {
			return
		}
		res, err := c.db.Exec(`DELETE FROM sample WHERE id = (SELECT id FROM sample WHERE NOT keep ORDER BY id LIMIT 1)`)
		if err != nil {
			log.Printf("Corpus eviction: %v", err)
			return
		}
		if n, _ := res.RowsAffected(); n == 0 {
			log.Printf("Corpus: %d MB used is over the %d MB budget, and only kept samples are left", used>>20, c.maxBytes>>20)
			return
		}
	}
}

// usedBytes is the size of the pages in use, which after a VACUUM is the file's size.
func (c *corpus) usedBytes() (int64, error) {
	var used int64
	err := c.db.QueryRow(`SELECT (page_count - freelist_count) * page_size
		FROM pragma_page_count(), pragma_freelist_count(), pragma_page_size()`).Scan(&used)
	return used, err
}

// update runs a statement on one sample, failing if there is no such sample.
func (c *corpus) update(query string, args ...any) error {
	res, err := c.db.Exec(query, args...)
	if err != nil {
		return err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return fmt.Errorf("no such sample")
	}
	return nil
}

func (c *corpus) keep(id int64, keep bool) error {
	return c.update(`UPDATE sample SET keep = ? WHERE id = ?`, keep, id)
}

// label sets a sample's correct transcript and keeps it. An empty note leaves the notes alone.
func (c *corpus) label(id int64, reference, note string) error {
	return c.update(`UPDATE sample SET reference = ?, labeled_at = ?, notes = coalesce(?, notes), keep = 1 WHERE id = ?`,
		reference, now(), nullable(note), id)
}

// purge deletes every sample that isn't kept, with its runs, and compacts the file.
func (c *corpus) purge() (int64, error) {
	res, err := c.db.Exec(`DELETE FROM sample WHERE NOT keep`)
	if err != nil {
		return 0, err
	}
	n, _ := res.RowsAffected()
	// In WAL mode VACUUM writes through the log, so truncate that too for the space to come back
	_, err = c.db.Exec(`VACUUM; PRAGMA wal_checkpoint(TRUNCATE)`)
	return n, err
}

func nullable(s string) sql.NullString {
	return sql.NullString{String: s, Valid: s != ""}
}

// live is the server's own process, when CORPUS_DB is set.
var live *process

// openLiveCorpus opens CORPUS_DB for the server, or returns nil when it is unset.
func openLiveCorpus() *corpus {
	path := os.Getenv("CORPUS_DB")
	if path == "" {
		return nil
	}
	c, err := openCorpus(path)
	if err != nil {
		log.Fatalf("Cannot open CORPUS_DB: %v", err)
	}
	log.Printf("CORPUS ENABLED: recording every request (audio, transcript, metadata) in %s, up to %d MB", path, c.maxBytes>>20)
	return c
}

// record saves a request and its run in the background, so the response is never held up.
func (p *process) record(received time.Time, r *http.Request, audio []byte, rec *run) {
	if p == nil {
		return
	}
	remoteAddr, headers := r.RemoteAddr, r.Header.Clone()
	go func() {
		id, err := p.addSample(received, remoteAddr, headers, audio, rec)
		if err != nil {
			log.Printf("Corpus: %v", err)
			return
		}
		log.Printf("Corpus: recorded sample %d", id)
		p.c.evict()
	}()
}

// hostInfo describes the machine a process ran on.
type hostInfo struct {
	Hostname     string            `json:"hostname"`
	OS           string            `json:"os"`
	Kernel       string            `json:"kernel"`
	Arch         string            `json:"arch"`
	CPU          string            `json:"cpu"`
	CPUs         int               `json:"cpus"`
	MemoryMB     int               `json:"memory_mb"`
	Devices      []device          `json:"devices"`
	Env          map[string]string `json:"env,omitempty"` // device selection and other ggml settings
	NvidiaDriver string            `json:"nvidia_driver,omitempty"`
}

// device is one that ggml can run on.
type device struct {
	Name         string `json:"name"`
	Description  string `json:"description"`
	Type         string `json:"type"`
	MemoryMB     uint64 `json:"memory_mb"`
	FreeBeforeMB uint64 `json:"free_before_mb"` // before loading the model
	FreeAfterMB  uint64 `json:"free_after_mb"`
}

// usedDevice is the device whisper runs on, once a process has started.
var usedDevice C.ggml_backend_dev_t

// readHostInfo describes this machine, and returns the device whisper will run on.
func readHostInfo() (h hostInfo, used C.ggml_backend_dev_t) {
	h = hostInfo{
		Arch:         runtime.GOARCH,
		CPUs:         runtime.NumCPU(),
		OS:           procField("/etc/os-release", "PRETTY_NAME"),
		CPU:          procField("/proc/cpuinfo", "model name"),
		Kernel:       readFile("/proc/sys/kernel/osrelease"),
		NvidiaDriver: procField("/proc/driver/nvidia/version", "NVRM version"),
	}
	h.Hostname, _ = os.Hostname()
	if kb := strings.Fields(procField("/proc/meminfo", "MemTotal")); len(kb) > 0 {
		n, _ := strconv.Atoi(kb[0])
		h.MemoryMB = n / 1024
	}
	for _, kv := range os.Environ() {
		k, v, _ := strings.Cut(kv, "=")
		if strings.HasPrefix(k, "GGML_") || strings.HasSuffix(k, "_VISIBLE_DEVICES") {
			if h.Env == nil {
				h.Env = map[string]string{}
			}
			h.Env[k] = v
		}
	}

	types := map[C.enum_ggml_backend_dev_type]string{
		C.GGML_BACKEND_DEVICE_TYPE_CPU: "CPU", C.GGML_BACKEND_DEVICE_TYPE_GPU: "GPU",
		C.GGML_BACKEND_DEVICE_TYPE_IGPU: "IGPU", C.GGML_BACKEND_DEVICE_TYPE_ACCEL: "ACCEL",
	}
	var cpu C.ggml_backend_dev_t
	for i := range C.ggml_backend_dev_count() {
		d := C.ggml_backend_dev_get(i)
		var free, total C.size_t
		C.ggml_backend_dev_memory(d, &free, &total)
		t := C.ggml_backend_dev_type(d)
		h.Devices = append(h.Devices, device{
			Name:         C.GoString(C.ggml_backend_dev_name(d)),
			Description:  C.GoString(C.ggml_backend_dev_description(d)),
			Type:         types[t],
			MemoryMB:     uint64(total) >> 20,
			FreeBeforeMB: uint64(free) >> 20,
		})
		// whisper_backend_init_gpu's choice with the default gpu_device 0: the first GPU, else the CPU
		if used == nil && (t == C.GGML_BACKEND_DEVICE_TYPE_GPU || t == C.GGML_BACKEND_DEVICE_TYPE_IGPU) {
			used = d
		}
		if cpu == nil && t == C.GGML_BACKEND_DEVICE_TYPE_CPU {
			cpu = d
		}
	}
	if used == nil {
		used = cpu
	}
	return h, used
}

func deviceName(d C.ggml_backend_dev_t) string {
	if d == nil {
		return "unknown"
	}
	return C.GoString(C.ggml_backend_dev_name(d)) + ": " + C.GoString(C.ggml_backend_dev_description(d))
}

func deviceFreeMB(d C.ggml_backend_dev_t) uint64 {
	if d == nil {
		return 0
	}
	var free, total C.size_t
	C.ggml_backend_dev_memory(d, &free, &total)
	return uint64(free) >> 20
}

// readFile returns a small file's contents trimmed, or "" if it can't be read.
func readFile(path string) string {
	b, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(b))
}

// procField returns the value of the first "key: value" or key="value" line in a file, or "" if absent.
func procField(path, key string) string {
	for line := range strings.Lines(readFile(path)) {
		if rest, ok := strings.CutPrefix(line, key); ok {
			rest = strings.TrimLeft(rest, " \t")
			if rest != "" && (rest[0] == ':' || rest[0] == '=') {
				return strings.Trim(rest[1:], " \t\n\"")
			}
		}
	}
	return ""
}
