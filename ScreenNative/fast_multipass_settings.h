#pragma once
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>

struct FastMultiPassSettings {
    bool enabled = false;
    unsigned target = 2;
    float stability = 0.9f;
};

// Strict flat JSON schema written by the Manager. Parse into a temporary so
// incomplete/invalid settings never change a running frame's configuration.
static bool ParseFastMultiPassSettings(const char *text, FastMultiPassSettings &out)
{
    const char *p = text;
    auto space = [&] { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p; };
    space();
    if (*p++ != '{') return false;
    FastMultiPassSettings next;
    bool seen[10] = {};
    for (;;) {
        space();
        unsigned key;
        if (strncmp(p, "\"enabled\"", 9) == 0) { key = 0; p += 9; }
        else if (strncmp(p, "\"strength\"", 10) == 0) { key = 1; p += 10; }
        else if (strncmp(p, "\"texture\"", 9) == 0) { key = 2; p += 9; }
        else if (strncmp(p, "\"temporal\"", 10) == 0) { key = 3; p += 10; }
        else if (strncmp(p, "\"tone\"", 6) == 0) { key = 4; p += 6; }
        else if (strncmp(p, "\"stability\"", 11) == 0) { key = 5; p += 11; }
        else if (strncmp(p, "\"reference\"", 11) == 0) { key = 6; p += 11; }
        else if (strncmp(p, "\"target\"", 8) == 0) { key = 7; p += 8; }
        else if (strncmp(p, "\"full_stability\"", 16) == 0) { key = 8; p += 16; }
        // Retired placement option: validate old saved files, but always use raw NR.
        else if (strncmp(p, "\"final_output\"", 14) == 0) { key = 9; p += 14; }
        else return false;
        if (seen[key]) return false;
        seen[key] = true;
        space(); if (*p++ != ':') return false; space();
        if (key == 0 || key == 2 || key == 3 || key == 6 || key == 8 || key == 9) {
            bool value;
            if (strncmp(p, "true", 4) == 0) { value = true; p += 4; }
            else if (strncmp(p, "false", 5) == 0) { value = false; p += 5; }
            else return false;
            if (key == 0) next.enabled = value;
            // Retired booleans are type-checked for saved-file compatibility.
        } else {
            const char *start = p;
            if (*p == '-') ++p;
            if (*p == '0') ++p;
            else if (*p >= '1' && *p <= '9') { do { ++p; } while (*p >= '0' && *p <= '9'); }
            else return false;
            if (*p == '.') {
                ++p; if (*p < '0' || *p > '9') return false;
                do { ++p; } while (*p >= '0' && *p <= '9');
            }
            if (*p == 'e' || *p == 'E') {
                ++p; if (*p == '+' || *p == '-') ++p;
                if (*p < '0' || *p > '9') return false;
                do { ++p; } while (*p >= '0' && *p <= '9');
            }
            char *end = nullptr;
            const float value = std::strtof(start, &end);
            if (end != p || !std::isfinite(value)) return false;
            if (key == 5) next.stability = std::clamp(value, 0.0f, 0.9f);
            else if (key == 7) {
                if (value != std::floor(value)) return false;
                next.target = static_cast<unsigned>(std::clamp(value, 1.0f, 6.0f));
            }
            // Retired strength/tone values are validated but never applied.
        }
        space();
        if (*p == ',') { ++p; continue; }
        if (*p++ != '}') return false;
        space(); if (*p != '\0' || !seen[0]) return false;
        out = next; return true;
    }
}
