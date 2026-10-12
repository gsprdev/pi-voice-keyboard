package main

import "testing"

func TestCleanTranscription(t *testing.T) {
	cases := map[string]string{
		// Sentinels alone mean nothing was said
		" [BLANK_AUDIO]":          "",
		" [silence]":              "",
		"(inaudible)":             "",
		"[BLANK_AUDIO] (Silence)": "",
		"(beep) [silence]":        "",
		// Sentinels and bare keywords mid-sentence are kept
		"The silence was loud.":           "The silence was loud.",
		"I heard (inaudible) yesterday.":  "I heard (inaudible) yesterday.",
		"The beep and the music stopped.": "The beep and the music stopped.",
		// Noise annotations are stripped anywhere
		"(beep) Hello world. [Music]":        "Hello world.",
		"Stop (music playing) now (clicks).": "Stop now.",
		"Send it (typing) (click) please":    "Send it please",
		// Other parentheticals are dictation
		"Call me (maybe) later.": "Call me (maybe) later.",
		// Filler
		"So... I think yes…": "So I think yes",
	}
	for in, want := range cases {
		if got := cleanTranscription(in); got != want {
			t.Errorf("cleanTranscription(%q) = %q, want %q", in, got, want)
		}
	}
}
