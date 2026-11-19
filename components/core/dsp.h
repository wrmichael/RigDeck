// Small DSP blocks: 48k->12k decimator, FT8/FT4 GFSK tone synthesiser, display spectrum.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

// 4:1 decimating low-pass FIR. The radio's SSB filter already limits audio to ~3 kHz, so this
// only has to keep 9 kHz+ from folding into the 0-3 kHz band we decode.
class Decimator4 {
public:
    Decimator4();
    size_t process(const int16_t* in, size_t n, float* out);   // out gets n/4 (+/-1) samples, scaled to +/-1.0
private:
    static const int TAPS = 32;
    float h_[TAPS];
    float z_[TAPS];
    int zi_ = 0, phase_ = 0;
};

// Generates the continuous-phase GFSK waveform for a tone sequence, one sample at a time and at
// any position, so a transmission can begin part-way through when the decoder finished late.
class FtxSynth {
public:
    void start(const std::vector<uint8_t>& tones, bool ft4, float f0, int sampleRate);
    int64_t totalSamples() const { return (int64_t)tones_.size() * nsps_; }
    void seek(int64_t pos) { pos_ = pos; }
    int64_t pos() const { return pos_; }
    float next();                                               // 0 outside [0, totalSamples)
private:
    std::vector<uint8_t> tones_;
    std::vector<float> pulse_;
    bool ft4_ = false;
    int nsps_ = 1, rate_ = 48000;
    float f0_ = 1500, phase_ = 0;
    int64_t pos_ = 0;
    static const int RES = 128;                                  // pulse table points per symbol
};

// Log-magnitude spectrum of the last 1024 samples at 12 kHz (11.7 Hz/bin), for the waterfall.
class Spectrum {
public:
    Spectrum();
    ~Spectrum();
    void feed(const float* s, size_t n);
    static const int BINS = 256;                                 // 0..3 kHz
    void snapshot(uint8_t out[BINS], uint32_t& seq);
private:
    void compute();
    static const int N = 1024;
    float ring_[N];
    int wi_ = 0, since_ = 0;
    float* win_;
    float* td_;
    void* fd_;
    void* cfg_;
    uint8_t cur_[BINS];
    uint32_t seq_ = 0;
};
