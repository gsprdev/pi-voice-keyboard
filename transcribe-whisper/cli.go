package main

// Subcommands for working with the corpus (see corpus.go).

import (
	"bufio"
	"bytes"
	"cmp"
	"database/sql"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"math"
	"os"
	"os/exec"
	"slices"
	"strconv"
	"strings"
	"text/tabwriter"
	"time"
	"unicode"

	whisper "github.com/ggerganov/whisper.cpp/bindings/go"
)

const usage = `usage: transcribe-whisper <command>

  serve                            run the transcription service, configured by
                                   PORT, MODEL, MODEL_LANGUAGE and CORPUS_DB

Corpus commands, each taking -db (default $CORPUS_DB):
  list   [-n 20]                   newest samples
  show   <id>                      a sample and all its runs, scored when labeled
  play   [-raw] <id>               play it, normalized for listening (to stdout when redirected)
  label  [-note text] <id> [text]  set the correct transcript, which also keeps the sample;
                                   plays it and prompts when text is omitted, where /e edits
                                   the transcript in $VISUAL or $EDITOR
  keep   [-clear] <id>...          exempt from eviction and purge, or -clear that
  sweep  [-n N]                    play and label kept samples that have no label yet, oldest first
  purge  [-y]                      delete every sample that isn't kept, and compact the file
  replay [flags] [id...]           transcribe again with this build, model and params (-h for flags)
  report                           compare configurations across all runs

<id> may be "last" for the newest sample.
`

func command(args []string) {
	cmds := map[string]func([]string) error{
		"serve": cmdServe, "list": cmdList, "show": cmdShow, "play": cmdPlay, "label": cmdLabel,
		"keep": cmdKeep, "sweep": cmdSweep, "purge": cmdPurge, "replay": cmdReplay, "report": cmdReport,
	}
	if len(args) == 0 {
		fmt.Fprint(os.Stderr, usage)
		os.Exit(2)
	}
	switch args[0] {
	case "help", "-h", "-help", "--help":
		fmt.Print(usage)
		return
	}
	cmd, ok := cmds[args[0]]
	if !ok {
		fmt.Fprintf(os.Stderr, "unknown command %q\n\n%s", args[0], usage)
		os.Exit(2)
	}
	if err := cmd(args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

// corpusFlags starts a subcommand's flags with -db.
func corpusFlags(name string) *flag.FlagSet {
	fs := flag.NewFlagSet(name, flag.ExitOnError)
	fs.String("db", os.Getenv("CORPUS_DB"), "corpus database")
	return fs
}

// parseOpen parses a subcommand's arguments and opens the corpus that -db names.
// Every subcommand comes through here, and none creates a corpus.
func parseOpen(fs *flag.FlagSet, args []string) (*corpus, error) {
	fs.Parse(args)
	path := fs.Lookup("db").Value.String()
	if path == "" {
		return nil, errors.New("no corpus: set CORPUS_DB or pass -db")
	}
	if _, err := os.Stat(path); err != nil {
		return nil, err
	}
	return openCorpus(path)
}

func (c *corpus) sampleID(arg string) (int64, error) {
	if arg == "last" {
		var id sql.NullInt64
		if err := c.db.QueryRow(`SELECT max(id) FROM sample`).Scan(&id); err != nil || !id.Valid {
			return 0, fmt.Errorf("no samples")
		}
		return id.Int64, nil
	}
	return strconv.ParseInt(arg, 10, 64)
}

func cmdList(args []string) error {
	fs := corpusFlags("list")
	n := fs.Int("n", 20, "number of samples")
	c, err := parseOpen(fs, args)
	if err != nil {
		return err
	}
	rows, err := c.db.Query(`SELECT s.id, s.received_at, s.audio_bytes, s.keep, s.reference IS NOT NULL,
		(SELECT count(*) FROM run WHERE sample_id = s.id),
		coalesce((SELECT coalesce(error, text) FROM run WHERE sample_id = s.id ORDER BY id LIMIT 1), '')
		FROM sample s ORDER BY s.id DESC LIMIT ?`, *n)
	if err != nil {
		return err
	}
	defer rows.Close()
	tw := tabwriter.NewWriter(os.Stdout, 0, 0, 2, ' ', 0)
	fmt.Fprintln(tw, "ID\tRECEIVED\tSECS\tKEEP\tLABELED\tRUNS\tFIRST RUN")
	for rows.Next() {
		var id, size, runs int64
		var received, text string
		var keep, labeled bool
		if err := rows.Scan(&id, &received, &size, &keep, &labeled, &runs, &text); err != nil {
			return err
		}
		fmt.Fprintf(tw, "%d\t%s\t%.1f\t%s\t%s\t%d\t%s\n", id, localTime(received), audioSeconds(size),
			yes(keep), yes(labeled), runs, truncate(text, 70))
	}
	tw.Flush()
	return rows.Err()
}

func cmdShow(args []string) error {
	fs := corpusFlags("show")
	c, err := parseOpen(fs, args)
	if err != nil {
		return err
	}
	if fs.NArg() != 1 {
		return errors.New("usage: show <id>")
	}
	id, err := c.sampleID(fs.Arg(0))
	if err != nil {
		return err
	}

	var received, remote, headers string
	var size int64
	var keep bool
	var reference, notes sql.NullString
	err = c.db.QueryRow(`SELECT received_at, remote_addr, headers, audio_bytes, keep, reference, notes FROM sample WHERE id = ?`, id).
		Scan(&received, &remote, &headers, &size, &keep, &reference, &notes)
	if err != nil {
		return fmt.Errorf("sample %d: %w", id, err)
	}
	var h map[string][]string
	json.Unmarshal([]byte(headers), &h)
	fmt.Printf("Sample %d, received %s from %s (%s), %.1fs", id, localTime(received), remote, strings.Join(h["User-Agent"], " "), audioSeconds(size))
	if keep {
		fmt.Print(", kept")
	}
	fmt.Println()
	if reference.Valid {
		fmt.Printf("Reference: %s\n", quote(reference.String))
	} else {
		fmt.Println("Unlabeled")
	}
	if notes.Valid {
		fmt.Printf("Notes: %s\n", notes.String)
	}

	runs, err := c.runs(`WHERE r.sample_id = ?`, id)
	if err != nil {
		return err
	}
	for _, r := range runs {
		fmt.Printf("\nRun %d, %s %s, #%d of process %d: %s\n", r.ID, r.Kind, localTime(r.Created), r.Seq, r.ProcessID, r.config())
		if r.Error != "" {
			fmt.Printf("  error: %s\n", r.Error)
			continue
		}
		fmt.Printf("  inference %.0fms (%.2fx realtime)", r.Timings["inference"], r.Timings["inference"]/1000/r.Seconds)
		if r.Reference.Valid {
			fmt.Printf(", %s", werString(score(r.Reference.String, r.Text)))
		}
		fmt.Printf(", %d tokens, compression %.2f\n", r.Tokens, r.Compression)
		e, cond := r.Effective, r.Conditions
		fmt.Printf("  audio_ctx %d, %d threads; load %.2f", e.AudioCtx, e.Threads, cond.Load1)
		if cond.ACOnline != nil {
			fmt.Printf(", %s", map[bool]string{true: "on AC", false: "on battery"}[*cond.ACOnline])
		}
		for _, s := range []string{cond.Profile, cond.Governor} {
			if s != "" {
				fmt.Printf(", %s", s)
			}
		}
		if cond.DeviceFreeMB > 0 {
			fmt.Printf(", %d MB device memory free", cond.DeviceFreeMB)
		}
		fmt.Println()
		fmt.Printf("  text: %s\n", quote(r.Text))
		if raw := strings.TrimSpace(r.RawText); raw != r.Text {
			fmt.Printf("  raw:  %s\n", quote(raw))
		}
	}
	return nil
}

func cmdPlay(args []string) error {
	fs := corpusFlags("play")
	raw := fs.Bool("raw", false, "the audio exactly as received, not normalized")
	c, err := parseOpen(fs, args)
	if err != nil {
		return err
	}
	if fs.NArg() != 1 {
		return errors.New("usage: play [-raw] <id>")
	}
	id, err := c.sampleID(fs.Arg(0))
	if err != nil {
		return err
	}
	wav, err := c.audio(id, !*raw)
	if err != nil {
		return err
	}

	if fi, err := os.Stdout.Stat(); err == nil && fi.Mode()&os.ModeCharDevice == 0 {
		_, err := os.Stdout.Write(wav)
		return err
	}
	return aplay(wav)
}

// audio returns a sample's WAV, optionally normalized for listening.
func (c *corpus) audio(id int64, normalized bool) ([]byte, error) {
	var wav []byte
	if err := c.db.QueryRow(`SELECT wav FROM audio WHERE sample_id = ?`, id).Scan(&wav); err != nil {
		return nil, fmt.Errorf("sample %d: %w", id, err)
	}
	if !normalized {
		return wav, nil
	}
	samples, err := decodeWAV(wav)
	if err != nil {
		return nil, err
	}
	return encodeWAV(audible(samples)), nil
}

func aplay(wav []byte) error {
	cmd := exec.Command("aplay", "-q")
	cmd.Stdin = bytes.NewReader(wav)
	cmd.Stderr = os.Stderr
	return cmd.Run()
}

// audible returns samples made comfortable to listen to: the DC offset removed (the SPH0645 has one),
// then scaled so that the 99.9th percentile peak, not a stray click, lands just below full scale.
// It is only for people; Whisper always gets the audio as received.
func audible(samples []float32) []float32 {
	if len(samples) == 0 {
		return samples
	}
	var mean float64
	for _, s := range samples {
		mean += float64(s)
	}
	mean /= float64(len(samples))

	out := make([]float32, len(samples))
	peaks := make([]float64, len(samples))
	for i, s := range samples {
		out[i] = s - float32(mean)
		peaks[i] = math.Abs(float64(out[i]))
	}
	slices.Sort(peaks)
	peak := peaks[int(0.999*float64(len(peaks)-1))]
	if peak == 0 {
		return out
	}
	gain := float32(0.9 / peak)
	for i := range out {
		out[i] = max(-1, min(1, out[i]*gain))
	}
	return out
}

func cmdLabel(args []string) error {
	fs := corpusFlags("label")
	note := fs.String("note", "", "notes to keep with the sample")
	c, err := parseOpen(fs, args)
	if err != nil {
		return err
	}
	if fs.NArg() < 1 {
		return errors.New("usage: label [-note text] <id> [text]")
	}
	id, err := c.sampleID(fs.Arg(0))
	if err != nil {
		return err
	}
	var reference string
	if fs.NArg() > 1 {
		reference = strings.Join(fs.Args()[1:], " ")
	} else {
		var cmd string
		if reference, cmd, err = c.promptReference(id, "/q quits"); err != nil {
			return err
		} else if cmd != "" {
			return errors.New("not labeled")
		}
	}
	if err := c.label(id, reference, *note); err != nil {
		return err
	}
	fmt.Printf("Sample %d labeled and kept\n", id)
	return nil
}

// stdin is shared by the prompts, so input typed ahead isn't lost between them.
var stdin = bufio.NewReader(os.Stdin)

// promptReference plays a sample and asks for its correct transcript.
// commands are offered too, each like "/q quits"; typing one returns it instead of a transcript.
func (c *corpus) promptReference(id int64, commands ...string) (reference, command string, err error) {
	var shown string
	err = c.db.QueryRow(`SELECT coalesce((SELECT text FROM run WHERE sample_id = ? AND error IS NULL ORDER BY id LIMIT 1), '')`, id).Scan(&shown)
	if err != nil {
		return "", "", err
	}
	wav, err := c.audio(id, true)
	if err != nil {
		return "", "", err
	}
	fmt.Printf("Sample %d was transcribed as:\n  %s\n", id, quote(shown))
	hint := strings.Join(append([]string{"Enter accepts it", "/e edits it", "/r replays"}, commands...), ", ")
	for {
		if err := aplay(wav); err != nil {
			fmt.Fprintln(os.Stderr, err) // labeling can still go ahead, from the transcript alone
		}
		fmt.Printf("Correct transcript (%s): ", hint)
		line, err := stdin.ReadString('\n')
		if err != nil {
			return "", "", err
		}
		line = strings.TrimSpace(line)
		switch {
		case line == "/r":
			continue
		case line == "/e":
			edited, err := edit(shown)
			if err != nil {
				fmt.Fprintln(os.Stderr, err)
			} else if edited != "" {
				return edited, "", nil
			}
			continue
		case line == "":
			return shown, "", nil
		}
		for _, cmd := range commands {
			if strings.Fields(cmd)[0] == line {
				return "", line, nil
			}
		}
		return line, "", nil
	}
}

// edit opens text in $VISUAL or $EDITOR (default vi) and returns it as saved, on one line.
func edit(text string) (string, error) {
	f, err := os.CreateTemp("", "transcript-*.txt")
	if err != nil {
		return "", err
	}
	defer os.Remove(f.Name())
	_, err = f.WriteString(text + "\n")
	if err := cmp.Or(err, f.Close()); err != nil {
		return "", err
	}
	// Through the shell, since EDITOR may carry arguments, like "code -w"
	editor := cmp.Or(os.Getenv("VISUAL"), os.Getenv("EDITOR"), "vi")
	cmd := exec.Command("sh", "-c", editor+` "$1"`, "sh", f.Name())
	cmd.Stdin, cmd.Stdout, cmd.Stderr = os.Stdin, os.Stdout, os.Stderr
	if err := cmd.Run(); err != nil {
		return "", fmt.Errorf("%s: %w", editor, err)
	}
	b, err := os.ReadFile(f.Name())
	return strings.Join(strings.Fields(string(b)), " "), err
}

func cmdKeep(args []string) error {
	fs := corpusFlags("keep")
	clear := fs.Bool("clear", false, "allow eviction again")
	c, err := parseOpen(fs, args)
	if err != nil {
		return err
	}
	if fs.NArg() < 1 {
		return errors.New("usage: keep [-clear] <id>...")
	}
	for _, arg := range fs.Args() {
		id, err := c.sampleID(arg)
		if err == nil {
			err = c.keep(id, !*clear)
		}
		if err != nil {
			return fmt.Errorf("%s: %w", arg, err)
		}
	}
	return nil
}

func cmdSweep(args []string) error {
	fs := corpusFlags("sweep")
	n := fs.Int("n", 0, "stop after this many samples (0 for all)")
	c, err := parseOpen(fs, args)
	if err != nil {
		return err
	}
	type pending struct {
		id, size        int64
		received, notes string
	}
	// Read up front, so the query isn't open while labeling writes
	var todo []pending
	rows, err := c.db.Query(`SELECT id, audio_bytes, received_at, coalesce(notes, '')
		FROM sample WHERE keep AND reference IS NULL ORDER BY id`)
	if err != nil {
		return err
	}
	for rows.Next() {
		var p pending
		if err := rows.Scan(&p.id, &p.size, &p.received, &p.notes); err != nil {
			rows.Close()
			return err
		}
		todo = append(todo, p)
	}
	rows.Close()
	if *n > 0 && len(todo) > *n {
		todo = todo[:*n]
	}

	var labeled, skipped, unkept int
	defer func() {
		fmt.Printf("%d labeled, %d skipped, %d unkept, %d left\n", labeled, skipped, unkept, len(todo)-labeled-skipped-unkept)
	}()
	for i, p := range todo {
		fmt.Printf("\n[%d/%d] %s, %.1fs", i+1, len(todo), localTime(p.received), audioSeconds(p.size))
		if p.notes != "" {
			fmt.Printf(", notes: %s", p.notes)
		}
		fmt.Println()
		reference, cmd, err := c.promptReference(p.id, "/s skips", "/u unkeeps", "/q quits")
		if err != nil {
			return err
		}
		switch cmd {
		case "":
			err = c.label(p.id, reference, "")
			labeled++
		case "/s":
			skipped++
		case "/u":
			err = c.keep(p.id, false)
			unkept++
		case "/q":
			return nil
		}
		if err != nil {
			return err
		}
	}
	return nil
}

func cmdPurge(args []string) error {
	fs := corpusFlags("purge")
	assumeYes := fs.Bool("y", false, "don't ask first")
	c, err := parseOpen(fs, args)
	if err != nil {
		return err
	}
	var n, audioBytes int64
	if err := c.db.QueryRow(`SELECT count(*), coalesce(sum(audio_bytes), 0) FROM sample WHERE NOT keep`).Scan(&n, &audioBytes); err != nil {
		return err
	}
	if n == 0 {
		fmt.Println("Every sample is kept, nothing to purge")
		return nil
	}
	if !*assumeYes {
		fmt.Printf("Delete %d samples that aren't kept (%.1f MB of audio, %.0f s)? [y/N] ", n, float64(audioBytes)/(1<<20), audioSeconds(audioBytes))
		line, _ := stdin.ReadString('\n')
		if a := strings.ToLower(strings.TrimSpace(line)); a != "y" && a != "yes" {
			return errors.New("not purged")
		}
	}
	before, err := c.usedBytes()
	if err != nil {
		return err
	}
	deleted, err := c.purge()
	if err != nil {
		return err
	}
	after, err := c.usedBytes()
	if err != nil {
		return err
	}
	fmt.Printf("Deleted %d samples, corpus %.1f MB -> %.1f MB\n", deleted, float64(before)/(1<<20), float64(after)/(1<<20))
	return nil
}

func cmdReplay(args []string) error {
	fs := corpusFlags("replay")
	modelName := fs.String("model", getenv("MODEL", defaultModel), "upstream model name, as for MODEL")
	var p params
	fs.StringVar(&p.Language, "language", getenv("MODEL_LANGUAGE", "en"), "language, for multilingual models")
	fs.IntVar(&p.BeamSize, "beam", 0, "beam search size, up to 8 (0: Whisper's default greedy sampling)")
	temperature := fs.Float64("temperature", 0, "sampling temperature")
	fs.StringVar(&p.Prompt, "prompt", "", "initial prompt")
	fs.UintVar(&p.Threads, "threads", 0, "CPU threads (0: all)")
	fs.IntVar(&p.AudioCtx, "audio-ctx", 0, "encoder frames, 50 a second (0: the full 1500, 30 seconds)")
	labeled := fs.Bool("labeled", false, "only labeled samples")
	c, err := parseOpen(fs, args)
	if err != nil {
		return err
	}
	if p.BeamSize > 8 {
		return errors.New("-beam: whisper.cpp runs at most 8 decoders")
	}
	p.Temperature = float32(*temperature)

	var ids []int64
	for _, arg := range fs.Args() {
		id, err := c.sampleID(arg)
		if err != nil {
			return fmt.Errorf("%s: %w", arg, err)
		}
		ids = append(ids, id)
	}
	if ids == nil {
		where := ""
		if *labeled {
			where = "WHERE reference IS NOT NULL"
		}
		if ids, err = c.ids(where); err != nil {
			return err
		}
	}

	proc := c.startProcess("replay", *modelName)
	defer proc.hashing.Wait() // so the model's hash is recorded before exiting
	m, err := loadModel(modelFile(*modelName), proc)
	if err != nil {
		return err
	}
	defer m.Whisper_free()
	if p.Language, err = language(m, p.Language); err != nil {
		return fmt.Errorf("-language: %w", err)
	}

	var errs, words, scored int
	for _, id := range ids {
		var wav []byte
		var reference sql.NullString
		if err := c.db.QueryRow(`SELECT a.wav, s.reference FROM sample s JOIN audio a ON a.sample_id = s.id WHERE s.id = ?`, id).Scan(&wav, &reference); err != nil {
			return fmt.Errorf("sample %d: %w", id, err)
		}
		r := proc.newRun(p)
		runID, err := proc.startRun(c.db, id, r)
		if err != nil {
			return err
		}
		samples, err := decodeWAV(wav)
		if err == nil {
			err = r.transcribe(m, samples)
		}
		if err != nil {
			r.Error = err.Error()
		}
		if err := finishRun(c.db, runID, r); err != nil {
			return err
		}

		result := quote(r.Text)
		if r.Error != "" {
			result = "error: " + r.Error
		} else if reference.Valid {
			e, n := score(reference.String, r.Text)
			errs, words, scored = errs+e, words+n, scored+1
			result = werString(e, n) + "  " + result
		}
		fmt.Printf("%d  %s\n", id, result)
	}
	if scored > 0 {
		fmt.Printf("%d runs, %d labeled: %s\n", len(ids), scored, werString(errs, words))
	}
	c.evict()
	return nil
}

func cmdReport(args []string) error {
	fs := corpusFlags("report")
	first := fs.Bool("first", false, "include each process's first transcription in timings, despite its warm-up")
	c, err := parseOpen(fs, args)
	if err != nil {
		return err
	}
	runs, err := c.runs("")
	if err != nil {
		return err
	}

	// Whisper encodes a fixed window whatever the clip length, so speed is only comparable between
	// clips of similar length, and one average would rank configurations by the mix of clips they saw.
	type bucket struct {
		name string
		end  float64 // seconds
	}
	buckets := []bucket{{"under 2s", 2}, {"2-5s", 5}, {"5-10s", 10}, {"10-20s", 20}, {"20s and over", math.Inf(1)}}
	type total struct {
		runs, failed, scored, errs, words int
		inference                         map[int][]float64 // milliseconds, per bucket
	}
	var configs []string
	totals := map[string]*total{}
	warm := map[int64]bool{} // processes whose first transcription has been seen
	for _, r := range runs {
		key := r.config()
		t := totals[key]
		if t == nil {
			t = &total{inference: map[int][]float64{}}
			totals[key] = t
			configs = append(configs, key)
		}
		t.runs++
		if r.Error != "" {
			t.failed++
			continue
		}
		if r.Reference.Valid {
			e, n := score(r.Reference.String, r.Text)
			t.scored, t.errs, t.words = t.scored+1, t.errs+e, t.words+n
		}
		// A process's first transcription warms up, whatever runs failed before reaching Whisper
		if !*first && !warm[r.ProcessID] {
			warm[r.ProcessID] = true
			continue
		}
		b := slices.IndexFunc(buckets, func(b bucket) bool { return r.Seconds < b.end })
		t.inference[b] = append(t.inference[b], r.Timings["inference"])
	}
	for _, key := range configs {
		t := totals[key]
		fmt.Printf("%s\n  %d runs", key, t.runs)
		if t.failed > 0 {
			fmt.Printf(" (%d failed or incomplete)", t.failed)
		}
		if t.scored > 0 {
			fmt.Printf(", %d labeled: %s", t.scored, werString(t.errs, t.words))
		}
		fmt.Println()
		for i, b := range buckets {
			if ms := t.inference[i]; len(ms) > 0 {
				slices.Sort(ms)
				fmt.Printf("  %-12s %4d runs, median inference %.0fms\n", b.name, len(ms), ms[len(ms)/2])
			}
		}
		fmt.Println()
	}
	if !*first {
		fmt.Println("Timings leave out each process's first transcription, which includes warm-up; -first includes them.")
	}
	return nil
}

// runRow is a run as read back for show and report, with what it needs from its process and sample.
type runRow struct {
	run
	Params                                           string // as stored, which report groups by
	ID, ProcessID                                    int64
	Created, Kind, Build, Backend, BuildHost, Device string
	Model, ModelSHA                                  string
	Host                                             hostInfo
	Reference                                        sql.NullString
	Seconds                                          float64
}

// config describes everything that made a run the way it was, apart from the audio and the machine's state.
func (r runRow) config() string {
	return fmt.Sprintf("%s %s %s, %s build %s (built on %s), run on %s using %s",
		r.Model, truncate(r.ModelSHA, 8), r.Params, r.Backend, r.Build, r.BuildHost, r.Host.Hostname, r.Device)
}

func (c *corpus) runs(where string, args ...any) ([]runRow, error) {
	rows, err := c.db.Query(`SELECT r.id, r.process_id, r.seq, r.created_at,
		p.kind, p.build, p.backend, p.build_host, p.device, p.model, coalesce(p.model_sha256, ''), r.params,
		coalesce(r.error, ''), coalesce(r.raw_text, ''), coalesce(r.text, ''), p.host,
		coalesce(r.effective, '{}'), coalesce(r.conditions, '{}'), coalesce(r.tokens, 0), coalesce(r.compression, 0),
		coalesce(r.timings, '{}'), s.reference, s.audio_bytes
		FROM run r JOIN process p ON p.id = r.process_id JOIN sample s ON s.id = r.sample_id `+where+` ORDER BY r.id`, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var runs []runRow
	for rows.Next() {
		var r runRow
		var host, eff, cond, timings string
		var size int64
		if err := rows.Scan(&r.ID, &r.ProcessID, &r.Seq, &r.Created,
			&r.Kind, &r.Build, &r.Backend, &r.BuildHost, &r.Device, &r.Model, &r.ModelSHA, &r.Params,
			&r.Error, &r.RawText, &r.Text, &host, &eff, &cond, &r.Tokens, &r.Compression,
			&timings, &r.Reference, &size); err != nil {
			return nil, err
		}
		json.Unmarshal([]byte(host), &r.Host)
		json.Unmarshal([]byte(eff), &r.Effective)
		json.Unmarshal([]byte(cond), &r.Conditions)
		json.Unmarshal([]byte(timings), &r.Timings)
		r.Seconds = audioSeconds(size)
		runs = append(runs, r)
	}
	return runs, rows.Err()
}

// words normalizes text for scoring: lowercase, with punctuation other than apostrophes ignored.
func words(s string) []string {
	return strings.Fields(strings.Map(func(r rune) rune {
		switch {
		case r == '\'' || r == '’':
			return '\''
		case unicode.IsLetter(r) || unicode.IsDigit(r):
			return unicode.ToLower(r)
		}
		return ' '
	}, s))
}

// score returns the word errors (substitutions, insertions, deletions) of hyp against ref,
// and the number of words in ref.
func score(ref, hyp string) (errs, n int) {
	r, h := words(ref), words(hyp)
	// Edit distance, one row at a time
	prev := make([]int, len(h)+1)
	cur := make([]int, len(h)+1)
	for j := range prev {
		prev[j] = j
	}
	for i := 1; i <= len(r); i++ {
		cur[0] = i
		for j := 1; j <= len(h); j++ {
			sub := prev[j-1]
			if r[i-1] != h[j-1] {
				sub++
			}
			cur[j] = min(sub, prev[j]+1, cur[j-1]+1)
		}
		prev, cur = cur, prev
	}
	return prev[len(h)], len(r)
}

func werString(errs, n int) string {
	if n == 0 {
		return fmt.Sprintf("%d word errors", errs)
	}
	return fmt.Sprintf("WER %.1f%% (%d/%d)", 100*float64(errs)/float64(n), errs, n)
}

// audioSeconds is the duration of a 16kHz mono 16-bit WAV of the given size.
func audioSeconds(size int64) float64 {
	return float64(max(0, size-wavHeader)) / (whisper.SampleRate * 2)
}

func localTime(s string) string {
	t, err := time.Parse(timeLayout, s)
	if err != nil {
		return s
	}
	return t.Local().Format("2006-01-02 15:04:05")
}

func quote(s string) string {
	if s == "" {
		return "(nothing)"
	}
	return "“" + s + "”"
}

func truncate(s string, n int) string {
	if r := []rune(s); len(r) > n {
		return string(r[:n-1]) + "…"
	}
	return s
}

func yes(b bool) string {
	if b {
		return "yes"
	}
	return ""
}
