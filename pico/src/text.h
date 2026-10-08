#ifndef TEXT_H
#define TEXT_H

// Clean a transcription in place: remove Whisper's non-speech markers and
// noise words, collapse whitespace, trim. May leave an empty string, which is
// intentional: nothing should be typed. Port of clean_transcription in pi/ptt.py.
void text_clean(char *text);

#endif
