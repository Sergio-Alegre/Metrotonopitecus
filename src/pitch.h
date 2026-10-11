// Pitch detection by normalized autocorrelation (NSDF, McLeod style),
// restricted to a window of +-span semitones around the expected note.
// No Arduino dependencies.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace pitch {

struct Result {
    bool valid = false;  // a peak was found inside the search range
    float freq = 0;      // detected fundamental, Hz
    float clarity = 0;   // peak height, 0..1 (1 = perfectly periodic)
    float rms = 0;       // signal level (DC removed), in sample units
};

// work must hold n floats. The expected frequency target must be such that
// the search range fits the frame (n >= 2 * longest lag + 2).
Result detect(const int16_t* samples, size_t n, float sampleRate, float targetHz, float spanSemitones, float* work);

}  // namespace pitch
