#include "psk31.h"
#include <cmath>
#include <cstring>

static const float kPi = 3.14159265358979f;
static const uint16_t kVaricode[128] = {
#include "varicode.inc"
};

int varicodeEncode(unsigned char c) { return kVaricode[c & 0x7F]; }

int varicodeDecode(uint32_t code) {
    static int16_t rev[1024];
    static bool init = false;
    if (!init) {
        for (auto& r : rev) r = -1;
        for (int i = 0; i < 128; i++) rev[kVaricode[i]] = (int16_t)i;
        init = true;
    }
    return code < 1024 ? rev[code] : -1;
}

// ---- transmit -----------------------------------------------------------------------------------
void Psk31Tx::start(float freq, int sampleRate) {
    freq_ = freq;
    rate_ = sampleRate;
    nsps_ = (int)(sampleRate / 31.25f + 0.5f);
    phase_ = 0;
    pos_ = 0;
    pre_ = 64;                 // 2 s of idle reversals so the far end can acquire timing
    post_ = 32;                // steady carrier marks the end
    cur_ = 0;                  // ramp up from silence
    nxt_ = 1;
    closing_ = false;
    done_ = false;
    chars_.clear();
    bits_.clear();
}

void Psk31Tx::queue(const std::string& text) {
    for (char c : text) if ((unsigned char)c < 128) chars_.push_back(c);
}

bool Psk31Tx::nextBit() {      // returns the bit to send: 0 = phase reversal, 1 = steady
    if (pre_ > 0) { pre_--; return false; }
    if (bits_.empty() && !chars_.empty()) {
        int code = varicodeEncode((unsigned char)chars_.front());
        chars_.pop_front();
        int len = 0;
        while ((code >> len) != 0) len++;
        for (int i = len - 1; i >= 0; i--) bits_.push_back((code >> i) & 1);
        bits_.push_back(0);
        bits_.push_back(0);    // character separator
    }
    if (!bits_.empty()) { bool b = bits_.front(); bits_.pop_front(); return b; }
    if (closing_) { if (post_ > 0) post_--; return true; }
    return false;              // idle: keep reversing
}

float Psk31Tx::next() {
    if (done_) return 0;
    // Between symbol centres the envelope is constant when polarity repeats and a cosine when it flips.
    float x = pos_ / (float)nsps_;
    float amp = (cur_ == nxt_) ? cur_ : (cur_ + nxt_) / 2 + (cur_ - nxt_) / 2 * cosf(kPi * x);
    float s = amp * sinf(phase_);
    phase_ += 2 * kPi * freq_ / rate_;
    if (phase_ > 2 * kPi) phase_ -= 2 * kPi;
    if (++pos_ >= nsps_) {
        pos_ = 0;
        cur_ = nxt_;
        if (closing_ && idle() && post_ == 0) {
            if (cur_ == 0) done_ = true;
            nxt_ = 0;          // ramp down to silence over the last symbol
        } else {
            bool bit = nextBit();
            float base = (cur_ == 0) ? 1.0f : cur_;
            nxt_ = bit ? base : -base;
        }
    }
    return s;
}

// ---- receive ------------------------------------------------------------------------------------
static std::vector<float> lowpass(int taps, float fc) {      // fc as a fraction of the sample rate
    std::vector<float> h(taps);
    float sum = 0;
    for (int i = 0; i < taps; i++) {
        float m = i - (taps - 1) / 2.0f;
        float s = (fabsf(m) < 1e-6f) ? 2 * fc : sinf(2 * kPi * fc * m) / (kPi * m);
        h[i] = s * (0.54f - 0.46f * cosf(2 * kPi * i / (taps - 1)));
        sum += h[i];
    }
    for (auto& v : h) v /= sum;
    return h;
}

Psk31Rx::Psk31Rx() {
    h1_ = lowpass(48, 300.0f / 12000.0f);     // 12 kHz -> 1.5 kHz
    h2_ = lowpass(64, 24.0f / 1500.0f);       // 1.5 kHz -> 500 Hz (16 samples per symbol)
    z1i_.assign(48, 0); z1q_.assign(48, 0);
    z2i_.assign(64, 0); z2q_.assign(64, 0);
    memset(env_, 0, sizeof env_);
}

void Psk31Rx::setFreq(float f) { f0_ = f; afc_ = 0; sr_ = 0; }

void Psk31Rx::process(const float* s, size_t n, std::string& out) {
    for (size_t k = 0; k < n; k++) {
        ph_ += 2 * kPi * (f0_ + afc_) / 12000.0f;
        if (ph_ > 2 * kPi) ph_ -= 2 * kPi;
        z1i_[p1_] = s[k] * cosf(ph_);
        z1q_[p1_] = -s[k] * sinf(ph_);
        p1_ = (p1_ + 1) % 48;
        if (++c1_ < 8) continue;
        c1_ = 0;
        float ai = 0, aq = 0;
        for (int t = 0, j = p1_; t < 48; t++) { ai += h1_[t] * z1i_[j]; aq += h1_[t] * z1q_[j]; if (++j == 48) j = 0; }
        z2i_[p2_] = ai;
        z2q_[p2_] = aq;
        p2_ = (p2_ + 1) % 64;
        if (++c2_ < 3) continue;
        c2_ = 0;
        float bi = 0, bq = 0;
        for (int t = 0, j = p2_; t < 64; t++) { bi += h2_[t] * z2i_[j]; bq += h2_[t] * z2q_[j]; if (++j == 64) j = 0; }
        // Symbol timing: the envelope dips at phase reversals, so the right sampling instant is
        // the one of the 16 phases with the largest average amplitude. The sampling point walks
        // towards it one step per symbol, so a symbol is never sampled twice or skipped.
        float mag = sqrtf(bi * bi + bq * bq);
        env_[tick_] += 0.1f * (mag - env_[tick_]);
        if (--cnt_ <= 0) {
            symbol(bi, bq, out);
            int best = 0;
            for (int i = 1; i < 16; i++) if (env_[i] > env_[best]) best = i;
            int diff = (best - tick_) & 15;
            cnt_ = (diff == 0) ? 16 : (diff <= 8 ? 17 : 15);
        }
        tick_ = (tick_ + 1) & 15;
    }
}

void Psk31Rx::symbol(float i, float q, std::string& out) {
    float dr = i * pi_ + q * pq_;             // z * conj(previous)
    float di = q * pi_ - i * pq_;
    pi_ = i;
    pq_ = q;
    float mag = sqrtf(dr * dr + di * di);
    if (mag < 1e-12f) return;
    float c = dr / mag, sn = di / mag;
    // Squaring removes the BPSK modulation: for a real signal the doubled angle is steady (zero
    // when tuned exactly, offset by a frequency error); for noise it is random and averages out.
    a2r_ += 0.06f * ((c * c - sn * sn) - a2r_);
    a2i_ += 0.06f * ((2 * c * sn) - a2i_);
    q_ = sqrtf(a2r_ * a2r_ + a2i_ * a2i_);
    bool open = q_ > 0.6f;                    // squelch: noise alone gives ~0.2
    if (q_ > 0.4f) {
        float err = 0.5f * atan2f(a2i_, a2r_);            // radians of rotation per symbol
        afc_ += 0.05f * err * 31.25f / (2 * kPi);
        if (afc_ > 15) afc_ = 15;
        if (afc_ < -15) afc_ = -15;
    }
    int bit = c > 0 ? 1 : 0;
    sr_ = (sr_ << 1) | (uint32_t)bit;
    if ((sr_ & 3) == 0) {                     // "00" ends a character
        int ch = varicodeDecode(sr_ >> 2);
        if (open && ch > 0 && (ch >= 32 || ch == '\n' || ch == '\r' || ch == 8)) out += (char)ch;
        sr_ = 0;
    } else if (sr_ > 0xFFFFF) sr_ = 0;
}
