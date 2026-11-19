// "FT-CW": FT8-style operation carried by Morse code.
// The same 15 s time slots, the same standard messages (CQ / grid / report / RR73 / 73) and the
// same sequencer as FT8, but each message is sent as machine-timed Morse on an audio tone, with a
// two-character check group at the end so the receiving side can reject garbled copies.
// Sender and receiver must use the same speed; the decoder is a matched filter for that dot
// length, run on every audio frequency at once, so several stations can share the passband.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <atomic>
#include "ftx.h"          // struct Decode

int cwDot12k(int wpm);                                       // dot length in samples at 12 kHz (multiple of 4)
std::string cwCheck(const std::string& text);                 // two-character check group for a message
std::string cwFrame(const std::string& text);                 // "CQ K1ABC FN42" -> "CQ K1ABC FN42 7Q"
bool cwUnframe(const std::string& framed, std::string& text); // verifies and strips the check group
int cwUnits(const std::string& framed);                       // duration in dot units; -1 if a character has no Morse code
double cwSeconds(const std::string& text, int wpm);           // on-air time of a message including its check group

class CwSynth {                                               // same interface as FtxSynth
public:
    bool start(const std::string& text, int wpm, float f0, int sampleRate);
    int64_t totalSamples() const { return (int64_t)cells_.size() * unit_; }
    void seek(int64_t pos) { pos_ = pos; }
    int64_t pos() const { return pos_; }
    float next();
private:
    std::vector<uint8_t> cells_;                              // key down / up per dot unit
    int unit_ = 1, ramp_ = 1, rate_ = 48000;
    float f0_ = 1500, phase_ = 0;
    int64_t pos_ = 0;
};

class CwMonitor {                                             // same interface as FtxMonitor
public:
    ~CwMonitor();
    bool init(int wpm);
    void reset();
    void feed(const float* s, size_t n);                      // 12 kHz samples from the start of the slot
    int decode(std::vector<Decode>& out);
    std::atomic<bool> busy{false};
    bool ready() const { return inited_; }
private:
    void frame();
    bool decodeBin(int b, int noise, Decode& d) const;
    bool inited_ = false;
    int dot_ = 360, hop_ = 90, nfft_ = 512, bin0_ = 0, nbins_ = 0, maxFrames_ = 0, frames_ = 0;
    std::vector<float> ring_, win_, td_;
    std::vector<uint8_t> mag_;                                // frames x bins, 0.5 dB steps, 240 = 0 dBFS
    int wi_ = 0, since_ = 0;
    long fed_ = 0;
    void* cfg_ = nullptr;
    void* fd_ = nullptr;
};
