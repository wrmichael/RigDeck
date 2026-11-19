// BPSK31 modem: varicode, cosine-shaped modulator (48 kHz out) and a demodulator (12 kHz in).
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <deque>
#include <vector>

class Psk31Tx {
public:
    void start(float freq, int sampleRate);
    void queue(const std::string& text);
    void finish() { closing_ = true; }       // send what is queued, then the postamble, then stop
    bool done() const { return done_; }
    bool idle() const { return bits_.empty() && chars_.empty(); }
    float next();
private:
    bool nextBit();
    std::deque<char> chars_;
    std::deque<uint8_t> bits_;
    float freq_ = 1000, phase_ = 0;
    int rate_ = 48000, nsps_ = 1536, pos_ = 0, pre_ = 0, post_ = 0;
    float cur_ = 1, nxt_ = 1;                // symbol polarity at this centre and the next
    bool closing_ = false, done_ = true;
};

class Psk31Rx {
public:
    Psk31Rx();
    void setFreq(float f);
    float freq() const { return f0_ + afc_; }
    void process(const float* s, size_t n, std::string& out);   // appends decoded characters
    float quality() const { return q_; }                          // 0..1; ~0.2 on noise, ->1 on a clean signal
private:
    void symbol(float i, float q, std::string& out);
    float f0_ = 1000, afc_ = 0, ph_ = 0;
    std::vector<float> h1_, h2_;
    std::vector<float> z1i_, z1q_, z2i_, z2q_;
    int p1_ = 0, c1_ = 0, p2_ = 0, c2_ = 0;
    float env_[16];
    int tick_ = 0, cnt_ = 16;
    float pi_ = 0, pq_ = 0, q_ = 0, a2r_ = 0, a2i_ = 0;
    uint32_t sr_ = 0;
};

int varicodeEncode(unsigned char c);          // code bits, MSB first, value as integer
int varicodeDecode(uint32_t code);            // -1 if not a valid code
