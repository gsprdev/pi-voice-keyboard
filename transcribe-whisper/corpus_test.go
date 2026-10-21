package main

import (
	"errors"
	"math"
	"net/http"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func testCorpus(t *testing.T) *corpus {
	c, err := openCorpus(filepath.Join(t.TempDir(), "corpus.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.db.Close() })
	return c
}

func sampleIDs(t *testing.T, c *corpus) []int64 {
	ids, err := c.ids("")
	if err != nil {
		t.Fatal(err)
	}
	return ids
}

func TestCorpusRoundTrip(t *testing.T) {
	c := testCorpus(t)
	proc := c.startProcess("serve", "no-such-model")
	var loadError string
	c.db.QueryRow(`SELECT load_error FROM process WHERE id = ?`, proc.id).Scan(&loadError)
	if loadError != "loading" {
		t.Errorf("load_error while loading = %q, want loading", loadError)
	}
	proc.loaded(5*time.Millisecond, errors.New("out of memory"))
	c.db.QueryRow(`SELECT load_error FROM process WHERE id = ?`, proc.id).Scan(&loadError)
	if loadError != "out of memory" {
		t.Errorf("load_error = %q", loadError)
	}

	wav := encodeWAV([]float32{0, 0.5, -0.5})
	r := proc.newRun(params{Language: "en"})
	r.RawText, r.Text, r.Tokens, r.Timings["inference"] = " Hello. [Music]", "Hello.", 7, 10
	r.Effective = effective{AudioCtx: 1500, Threads: 4}
	id, err := proc.addSample(time.Now(), "1.2.3.4:5", http.Header{"User-Agent": {"pico"}}, wav, r)
	if err != nil {
		t.Fatal(err)
	}
	// A replay that never finishes, as if it crashed
	if _, err := proc.startRun(c.db, id, proc.newRun(params{AudioCtx: 768})); err != nil {
		t.Fatal(err)
	}
	if err := c.label(id, "Hello there.", ""); err != nil {
		t.Fatal(err)
	}

	runs, err := c.runs(`WHERE r.sample_id = ?`, id)
	if err != nil || len(runs) != 2 {
		t.Fatalf("runs = %d, %v", len(runs), err)
	}
	first, crashed := runs[0], runs[1]
	if first.Text != "Hello." || first.RawText != " Hello. [Music]" || first.Params != `{"language":"en"}` ||
		first.Timings["inference"] != 10 || first.Tokens != 7 || first.Effective.Threads != 4 || first.Seq != 1 || first.Kind != "serve" {
		t.Errorf("first run = %+v", first)
	}
	if crashed.Error != "incomplete" || crashed.Seq != 2 || crashed.Params != `{"audio_ctx":768}` {
		t.Errorf("crashed run = %+v", crashed)
	}
	if !first.Reference.Valid || first.Reference.String != "Hello there." {
		t.Errorf("reference = %v", first.Reference)
	}
	if len(first.Host.Devices) == 0 || first.Device == "" {
		t.Errorf("host devices %v, device %q", first.Host.Devices, first.Device)
	}
	var keep bool
	c.db.QueryRow(`SELECT keep FROM sample WHERE id = ?`, id).Scan(&keep)
	if !keep {
		t.Error("labeling did not keep the sample")
	}

	got, err := c.audio(id, false)
	if err != nil || string(got) != string(wav) {
		t.Errorf("audio changed: %v", err)
	}
	if err := c.label(id+1, "x", ""); err == nil {
		t.Error("labeling a missing sample succeeded")
	}
}

func TestCorpusEviction(t *testing.T) {
	c := testCorpus(t)
	proc := c.startProcess("serve", "no-such-model")
	audio := make([]byte, 64<<10)
	start := time.Now()
	for i := range 4 {
		if _, err := proc.addSample(start.Add(time.Duration(i)*time.Second), "", nil, audio, proc.newRun(params{})); err != nil {
			t.Fatal(err)
		}
	}
	// The oldest is kept, so eviction goes oldest first among the rest
	c.keep(1, true)

	c.maxBytes = 200 << 10 // room for two samples and the schema
	c.evict()
	if ids := sampleIDs(t, c); len(ids) != 2 || ids[0] != 1 || ids[1] != 4 {
		t.Errorf("after eviction, samples = %v, want [1 4]", ids)
	}
	var runs int
	c.db.QueryRow(`SELECT count(*) FROM run`).Scan(&runs)
	if runs != 2 {
		t.Errorf("runs = %d, want 2 (evicted samples' runs deleted with them)", runs)
	}

	// Kept samples are never evicted, even over budget
	c.maxBytes = 1
	c.evict()
	if ids := sampleIDs(t, c); len(ids) != 1 || ids[0] != 1 {
		t.Errorf("over budget, samples = %v, want [1]", ids)
	}
}

func TestCorpusPurge(t *testing.T) {
	c := testCorpus(t)
	proc := c.startProcess("serve", "no-such-model")
	for range 4 {
		if _, err := proc.addSample(time.Now(), "", nil, make([]byte, 64<<10), proc.newRun(params{})); err != nil {
			t.Fatal(err)
		}
	}
	c.keep(2, true)
	c.label(3, "labeled", "") // which keeps it
	before, _ := c.usedBytes()

	n, err := c.purge()
	if err != nil || n != 2 {
		t.Fatalf("purge = %d, %v, want 2 deleted", n, err)
	}
	if ids := sampleIDs(t, c); len(ids) != 2 || ids[0] != 2 || ids[1] != 3 {
		t.Errorf("after purge, samples = %v, want [2 3]", ids)
	}
	var runs int
	c.db.QueryRow(`SELECT count(*) FROM run WHERE sample_id NOT IN (2, 3)`).Scan(&runs)
	if runs != 0 {
		t.Errorf("%d runs of purged samples remain", runs)
	}
	if after, _ := c.usedBytes(); after >= before {
		t.Errorf("corpus %d bytes after purge, not smaller than %d", after, before)
	}
}

func TestCompressionRatio(t *testing.T) {
	loop := compressionRatio(strings.Repeat("asked me to do something. ", 10))
	speech := compressionRatio("And so my fellow Americans, ask not what your country can do for you.")
	if loop < 2.4 || speech > 2 {
		t.Errorf("compression of a loop = %.2f, of speech = %.2f", loop, speech)
	}
}

func TestCorpusSchemaVersion(t *testing.T) {
	c := testCorpus(t)
	c.db.Exec(`PRAGMA user_version = 99`)
	if err := c.initSchema(); err == nil {
		t.Error("opened a corpus from another schema version")
	}
}

func TestScore(t *testing.T) {
	tests := []struct {
		ref, hyp    string
		errs, words int
	}{
		{"Hello, world.", "hello world", 0, 2},
		{"Don't stop", "don’t stop", 0, 2},
		{"the quick brown fox", "the quick fox", 1, 4},
		{"the quick fox", "the very quick fox", 1, 3},
		{"the quick fox", "a quick box", 2, 3},
		{"", "uh", 1, 0},
		{"", "", 0, 0},
	}
	for _, tt := range tests {
		if errs, words := score(tt.ref, tt.hyp); errs != tt.errs || words != tt.words {
			t.Errorf("score(%q, %q) = %d, %d, want %d, %d", tt.ref, tt.hyp, errs, words, tt.errs, tt.words)
		}
	}
}

func TestAudible(t *testing.T) {
	// A quiet tone on a DC offset, with one loud click
	samples := make([]float32, 16000)
	for i := range samples {
		samples[i] = 0.2 + 0.01*float32(math.Sin(float64(i)/5))
	}
	samples[100] = 1

	out := audible(samples)
	var mean, peak float64
	for i, s := range out {
		mean += float64(s)
		if i != 100 {
			peak = max(peak, math.Abs(float64(s)))
		}
	}
	if mean /= float64(len(out)); math.Abs(mean) > 0.01 {
		t.Errorf("DC offset %f remains", mean)
	}
	if peak < 0.8 || peak > 0.95 {
		t.Errorf("tone peak = %f, want it near 0.9 despite the click", peak)
	}
	if out[100] != 1 {
		t.Errorf("click = %f, want it clipped to 1", out[100])
	}
}

func TestWAVRoundTrip(t *testing.T) {
	in := []float32{0, 0.5, -0.5, 1, -1}
	out, err := decodeWAV(encodeWAV(in))
	if err != nil || len(out) != len(in) {
		t.Fatalf("decodeWAV = %v, %v", out, err)
	}
	for i := range in {
		if math.Abs(float64(out[i]-in[i])) > 1e-4 {
			t.Errorf("sample %d = %f, want %f", i, out[i], in[i])
		}
	}
}

func TestEdit(t *testing.T) {
	// An editor with arguments, which also joins lines like a careless edit might
	t.Setenv("VISUAL", `sed -i 's/teh/the/; $a\second line'`)
	got, err := edit("fix teh  typo")
	if err != nil {
		t.Fatal(err)
	}
	if want := "fix the typo second line"; got != want {
		t.Errorf("edit = %q, want %q", got, want)
	}
}
