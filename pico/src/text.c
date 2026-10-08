#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>

#include "text.h"

// Same list and matching as pi/ptt.py: each keyword case-insensitively, with
// an optional opening ( or [ before it and closing ) or ] after it
static const char *const noise_keywords[] = {
    "BLANK_AUDIO", // Generally indicates an empty recording
    "silence",     // Generally indicates an empty recording
    "beep",        // Likely the buzzer marking the start of recording
    "inaudible",   // A bad recording, or pickup before or after speech
};

// Length of a noise match at s, or 0
static size_t noise_at(const char *s) {
    for (size_t k = 0; k < sizeof(noise_keywords) / sizeof(noise_keywords[0]); k++) {
        const char *kw = noise_keywords[k];
        size_t kw_len = strlen(kw);
        size_t open = (*s == '[' || *s == '(') ? 1 : 0;
        // Try with the opening bracket first, then without, like the regex
        for (int with_open = open; with_open >= 0; with_open--) {
            if (strncasecmp(s + with_open, kw, kw_len) == 0) {
                size_t len = with_open + kw_len;
                if (s[len] == ']' || s[len] == ')') {
                    len++;
                }
                return len;
            }
        }
    }
    return 0;
}

void text_clean(char *text) {
    // Remove noise
    char *dst = text;
    for (const char *src = text; *src;) {
        size_t skip = noise_at(src);
        if (skip) {
            src += skip;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';

    // Replace runs of two or more whitespace characters with one space
    dst = text;
    for (const char *src = text; *src;) {
        if (isspace((unsigned char)src[0]) && isspace((unsigned char)src[1])) {
            while (isspace((unsigned char)*src)) {
                src++;
            }
            *dst++ = ' ';
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';

    // Trim
    char *start = text;
    while (isspace((unsigned char)*start)) {
        start++;
    }
    size_t len = strlen(start);
    while (len > 0 && isspace((unsigned char)start[len - 1])) {
        len--;
    }
    memmove(text, start, len);
    text[len] = '\0';
}
