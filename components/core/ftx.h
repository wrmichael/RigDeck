// FT8 / FT4 encode + decode on top of the vendored ft8_lib.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
extern "C" {
#include <common/monitor.h>
}

struct Decode {
    uint32_t id = 0;
    int64_t slotMs = 0;        // UTC ms of the start of the slot it was heard in
    int parity = 0;            // 0 = even slot, 1 = odd
    int snr = 0;               // dB in 2500 Hz (estimate)
    float dt = 0;              // seconds relative to nominal start
    float freq = 0;            // audio Hz
    std::string text;
};

// Message text -> channel tone sequence (79 tones for FT8, 105 for FT4). False if it cannot be packed.
bool ftxEncode(const std::string& text, bool ft4, std::vector<uint8_t>& tones);

// One receive "waterfall" for a single time slot.
class FtxMonitor {
public:
    ~FtxMonitor();
    bool init(bool ft4);                       // (re)allocates for the protocol
    void reset();
    void feed(const float* s, size_t n);       // 12 kHz samples, any chunk size
    int decode(std::vector<Decode>& out, int maxCandidates = 120, int ldpcIters = 25);
    std::atomic<bool> busy{false};             // set while the decoder owns it
    bool ready() const { return inited_; }
private:
    monitor_t mon_;
    bool inited_ = false, ft4_ = false;
    std::vector<float> blk_;
    size_t fill_ = 0;
};
