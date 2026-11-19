#include "dsp.h"
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <mutex>
extern "C" {
#include <fft/kiss_fftr.h>
}

static const float kPi = 3.14159265358979f;

Decimator4::Decimator4() {
    // Hamming-windowed sinc, cutoff 5 kHz at 48 kHz.
    const float fc = 5000.0f / 48000.0f;
    float sum = 0;
    for (int i = 0; i < TAPS; i++) {
        float m = i - (TAPS - 1) / 2.0f;
        float s = (fabsf(m) < 1e-6f) ? 2 * fc : sinf(2 * kPi * fc * m) / (kPi * m);
        float w = 0.54f - 0.46f * cosf(2 * kPi * i / (TAPS - 1));
        h_[i] = s * w;
        sum += h_[i];
    }
    for (int i = 0; i < TAPS; i++) h_[i] /= sum * 32768.0f;
    memset(z_, 0, sizeof z_);
}

size_t Decimator4::process(const int16_t* in, size_t n, float* out) {
    size_t no = 0;
    for (size_t i = 0; i < n; i++) {
        z_[zi_] = in[i];
        zi_ = (zi_ + 1) % TAPS;
        if (++phase_ == 4) {
            phase_ = 0;
            float acc = 0;
            int k = zi_;
            for (int t = 0; t < TAPS; t++) {
                acc += h_[t] * z_[k];
                if (++k == TAPS) k = 0;
            }
            out[no++] = acc;
        }
    }
    return no;
}

void FtxSynth::start(const std::vector<uint8_t>& tones, bool ft4, float f0, int sampleRate) {
    tones_ = tones;
    ft4_ = ft4;
    f0_ = f0;
    rate_ = sampleRate;
    nsps_ = (int)(sampleRate * (ft4 ? 0.048f : 0.160f) + 0.5f);
    phase_ = 0;
    pos_ = 0;
    // Gaussian-smoothed frequency pulse spanning 3 symbols (BT = 2 for FT8, 1 for FT4).
    const float bt = ft4 ? 1.0f : 2.0f;
    const float K = kPi * sqrtf(2.0f / logf(2.0f));
    pulse_.resize(3 * RES + 2);
    for (int i = 0; i < (int)pulse_.size(); i++) {
        float t = i / (float)RES - 1.5f;
        pulse_[i] = (erff(K * bt * (t + 0.5f)) - erff(K * bt * (t - 0.5f))) / 2;
    }
}

float FtxSynth::next() {
    int64_t total = totalSamples();
    if (pos_ < 0 || pos_ >= total) { pos_++; return 0; }
    int n = (int)tones_.size();
    int m = (int)(pos_ / nsps_);
    int o = (int)(pos_ - (int64_t)m * nsps_);
    float x = o / (float)nsps_;
    // Three overlapping pulses contribute: symbols m+1, m, m-1 (edge symbols are extended).
    auto sym = [&](int i) { return (float)tones_[i < 0 ? 0 : (i >= n ? n - 1 : i)]; };
    auto P = [&](float u) { float fi = u * RES; int i = (int)fi; float fr = fi - i; return pulse_[i] + (pulse_[i + 1] - pulse_[i]) * fr; };
    float dev = sym(m + 1) * P(x) + sym(m) * P(x + 1) + sym(m - 1) * P(x + 2);
    float dphi = 2 * kPi * f0_ / rate_ + 2 * kPi * dev / nsps_;
    float s = sinf(phase_);
    phase_ += dphi;
    if (phase_ > 2 * kPi) phase_ -= 2 * kPi;
    // Envelope: FT8 ramps over 1/8 symbol; FT4's first and last symbols are full-length ramps.
    int ramp = ft4_ ? nsps_ : nsps_ / 8;
    int64_t fromEnd = total - 1 - pos_;
    if (pos_ < ramp) s *= (1 - cosf(kPi * pos_ / ramp)) / 2;
    else if (fromEnd < ramp) s *= (1 - cosf(kPi * fromEnd / ramp)) / 2;
    pos_++;
    return s;
}

static std::mutex g_specMtx;

Spectrum::Spectrum() {
    win_ = (float*)malloc(N * sizeof(float));
    td_ = (float*)malloc(N * sizeof(float));
    fd_ = malloc((N / 2 + 1) * sizeof(kiss_fft_cpx));
    cfg_ = kiss_fftr_alloc(N, 0, nullptr, nullptr);
    for (int i = 0; i < N; i++) { float x = sinf(kPi * i / N); win_[i] = x * x * 2.0f / N; }
    memset(ring_, 0, sizeof ring_);
    memset(cur_, 0, sizeof cur_);
}

Spectrum::~Spectrum() { free(win_); free(td_); free(fd_); free(cfg_); }

void Spectrum::feed(const float* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        ring_[wi_] = s[i];
        wi_ = (wi_ + 1) % N;
        if (++since_ >= 3000) { since_ = 0; compute(); }      // 4 rows per second
    }
}

void Spectrum::compute() {
    for (int i = 0; i < N; i++) td_[i] = ring_[(wi_ + i) % N] * win_[i];
    kiss_fft_cpx* fd = (kiss_fft_cpx*)fd_;
    kiss_fftr((kiss_fftr_cfg)cfg_, td_, fd);
    std::lock_guard<std::mutex> lk(g_specMtx);
    for (int b = 0; b < BINS; b++) {
        float p = fd[b].r * fd[b].r + fd[b].i * fd[b].i;
        int v = (int)(2 * (10 * log10f(p + 1e-12f)) + 240);      // 0.5 dB steps, 240 = 0 dBFS
        cur_[b] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
    seq_++;
}

void Spectrum::snapshot(uint8_t out[BINS], uint32_t& seq) {
    std::lock_guard<std::mutex> lk(g_specMtx);
    memcpy(out, cur_, BINS);
    seq = seq_;
}
