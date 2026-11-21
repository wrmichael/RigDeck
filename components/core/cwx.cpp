#include "cwx.h"
#include "qso.h"
#include "settings.h"
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <algorithm>
extern "C" {
#include <fft/kiss_fftr.h>
}

static const float kPi = 3.14159265358979f;
static const float SNR_CAL = 0.5f;                 // dB, set from the loopback test

// ---- Morse code ---------------------------------------------------------------------------------
static const struct { char c; const char* code; } kMorse[] = {
    {'A', ".-"}, {'B', "-..."}, {'C', "-.-."}, {'D', "-.."}, {'E', "."}, {'F', "..-."}, {'G', "--."}, {'H', "...."},
    {'I', ".."}, {'J', ".---"}, {'K', "-.-"}, {'L', ".-.."}, {'M', "--"}, {'N', "-."}, {'O', "---"}, {'P', ".--."},
    {'Q', "--.-"}, {'R', ".-."}, {'S', "..."}, {'T', "-"}, {'U', "..-"}, {'V', "...-"}, {'W', ".--"}, {'X', "-..-"},
    {'Y', "-.--"}, {'Z', "--.."}, {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"}, {'4', "....-"},
    {'5', "....."}, {'6', "-...."}, {'7', "--..."}, {'8', "---.."}, {'9', "----."}, {'/', "-..-."}, {'+', ".-.-."},
    {'-', "-....-"}, {'?', "..--.."}, {'=', "-...-"}, {'.', ".-.-.-"}, {',', "--..--"}};

static const char* morseOf(char c) {
    for (auto& m : kMorse) if (m.c == c) return m.code;
    return nullptr;
}
static char charOf(const char* code) {
    for (auto& m : kMorse) if (!strcmp(m.code, code)) return m.c;
    return 0;
}

int cwDot12k(int wpm) {
    if (wpm < 15) wpm = 15;
    if (wpm > 60) wpm = 60;
    return 4 * (int)lroundf(3600.0f / wpm);          // 1.2 s / wpm, rounded so a dot is a whole number of analysis hops
}

std::string cwCheck(const std::string& text) {
    uint16_t crc = 0xFFFF;                            // CRC-16/CCITT over the message text
    for (unsigned char ch : text) {
        crc ^= (uint16_t)ch << 8;
        for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    static const char a[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    unsigned v = crc % 1296;
    return std::string() + a[v / 36] + a[v % 36];
}

std::string cwFrame(const std::string& text) { return text + " " + cwCheck(text); }

bool cwUnframe(const std::string& framed, std::string& text) {
    if (framed.size() < 5 || framed[framed.size() - 3] != ' ') return false;
    text = framed.substr(0, framed.size() - 3);
    return cwCheck(text) == framed.substr(framed.size() - 2);
}

// Key-down/key-up pattern in dot units: dit 1, dah 3, 1 between elements, 3 between characters,
// 7 between words.
static bool cwCells(const std::string& s, std::vector<uint8_t>& cells) {
    cells.clear();
    bool firstInWord = true;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == ' ') {
            if (!cells.empty()) cells.insert(cells.end(), 7, 0);
            firstInWord = true;
            continue;
        }
        const char* code = morseOf(s[i]);
        if (!code) return false;
        if (!firstInWord) cells.insert(cells.end(), 3, 0);
        firstInWord = false;
        for (const char* p = code; *p; p++) {
            if (p != code) cells.push_back(0);
            cells.insert(cells.end(), *p == '-' ? 3 : 1, 1);
        }
    }
    return !cells.empty();
}

int cwUnits(const std::string& framed) {
    std::vector<uint8_t> c;
    return cwCells(framed, c) ? (int)c.size() : -1;
}

double cwSeconds(const std::string& text, int wpm) {
    int u = cwUnits(cwFrame(text));
    return u < 0 ? -1 : u * (cwDot12k(wpm) / 12000.0);
}

// ---- transmit -----------------------------------------------------------------------------------
bool CwSynth::start(const std::string& text, int wpm, float f0, int sampleRate) {
    if (!cwCells(cwFrame(text), cells_)) { cells_.clear(); return false; }
    rate_ = sampleRate;
    unit_ = cwDot12k(wpm) * (sampleRate / 12000);
    ramp_ = std::min(unit_ / 4, sampleRate * 5 / 1000);        // 5 ms raised-cosine edges: no key clicks
    f0_ = f0;
    phase_ = 0;
    pos_ = 0;
    return true;
}

float CwSynth::next() {
    int64_t total = totalSamples();
    if (pos_ < 0 || pos_ >= total) { pos_++; return 0; }
    int u = (int)(pos_ / unit_), o = (int)(pos_ - (int64_t)u * unit_);
    float env = 0;
    if (cells_[u]) {
        env = 1;
        bool prev = u > 0 && cells_[u - 1], nxt = u + 1 < (int)cells_.size() && cells_[u + 1];
        if (!prev && o < ramp_) env = (1 - cosf(kPi * o / ramp_)) / 2;
        else if (!nxt && unit_ - o <= ramp_) env = (1 - cosf(kPi * (unit_ - o) / ramp_)) / 2;
    }
    float s = env * sinf(phase_);
    phase_ += 2 * kPi * f0_ / rate_;
    if (phase_ > 2 * kPi) phase_ -= 2 * kPi;
    pos_++;
    return s;
}

// ---- receive ------------------------------------------------------------------------------------
CwMonitor::~CwMonitor() { free(cfg_); free(fd_); }

bool CwMonitor::init(int wpm) {
    free(cfg_); free(fd_);
    cfg_ = fd_ = nullptr;
    inited_ = false;
    dot_ = cwDot12k(wpm);
    hop_ = dot_ / 4;
    nfft_ = 512;
    while (nfft_ < dot_) nfft_ *= 2;
    bin0_ = (int)(200.0f * nfft_ / 12000);
    nbins_ = (int)(3000.0f * nfft_ / 12000) - bin0_;
    maxFrames_ = (int)(13.8f * 12000 / hop_);
    cfg_ = kiss_fftr_alloc(nfft_, 0, nullptr, nullptr);
    fd_ = malloc((nfft_ / 2 + 1) * sizeof(kiss_fft_cpx));
    if (!cfg_ || !fd_) return false;
    ring_.assign(dot_, 0);
    td_.assign(nfft_, 0);
    win_.resize(dot_);
    // Half-sine window over one dot: within 1 dB of a true matched filter for a keyed dot, with
    // much lower leakage from strong neighbouring signals than a rectangular one.
    for (int i = 0; i < dot_; i++) win_[i] = sinf(kPi * (i + 0.5f) / dot_) * 1.5708f / dot_;
    mag_.assign((size_t)maxFrames_ * nbins_, 0);
    if (mag_.size() != (size_t)maxFrames_ * nbins_) return false;
    inited_ = true;
    reset();
    return true;
}

void CwMonitor::reset() {
    frames_ = 0;
    wi_ = 0;
    since_ = 0;
    fed_ = 0;
    std::fill(ring_.begin(), ring_.end(), 0.0f);
}

void CwMonitor::feed(const float* s, size_t n) {
    if (!inited_) return;
    for (size_t i = 0; i < n; i++) {
        ring_[wi_] = s[i];
        wi_ = (wi_ + 1) % dot_;
        fed_++;
        // Frame t covers samples [t*hop, t*hop + dot): a dot that starts at sample t*hop fills it exactly.
        if (fed_ >= dot_ && ++since_ >= hop_) { since_ = 0; frame(); }
        else if (fed_ == dot_) { since_ = 0; frame(); }
    }
}

void CwMonitor::frame() {
    if (frames_ >= maxFrames_) return;
    for (int i = 0; i < dot_; i++) td_[i] = ring_[(wi_ + i) % dot_] * win_[i];
    for (int i = dot_; i < nfft_; i++) td_[i] = 0;
    kiss_fft_cpx* fd = (kiss_fft_cpx*)fd_;
    kiss_fftr((kiss_fftr_cfg)cfg_, td_.data(), fd);
    uint8_t* row = &mag_[(size_t)frames_ * nbins_];
    for (int b = 0; b < nbins_; b++) {
        const kiss_fft_cpx& c = fd[bin0_ + b];
        int v = (int)(2 * 10 * log10f(c.r * c.r + c.i * c.i + 1e-12f) + 240.5f);
        row[b] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
    frames_++;
}

// Key-down/key-up cells -> text. Returns true when a stretch of keying forms a message whose check
// group matches and which has the shape of a standard message; startCell is where it began.
static bool cellsToMessage(const std::vector<uint8_t>& bits, std::string& msg, int& startCell) {
    std::string text, code;
    int run = 0, start = -1;
    bool on = false, any = false;
    auto endChar = [&]() {
        if (code.empty()) return;
        char ch = code.size() <= 6 ? charOf(code.c_str()) : 0;
        text += ch ? ch : '#';
        code.clear();
    };
    auto tryText = [&]() -> bool {
        endChar();
        while (!text.empty() && text.back() == ' ') text.pop_back();
        std::string m;
        bool ok = text.find('#') == std::string::npos && cwUnframe(text, m);
        if (ok) {                                                 // must also read as a standard message
            ParsedMsg p;
            ok = parseMsg(m, p) && validCall(p.from) && (p.kind == K_CQ || validCall(p.to)) && p.kind != K_OTHER;
        }
        if (ok) { msg = m; startCell = start; }
        text.clear();
        return ok;
    };
    const int n = (int)bits.size();
    for (int i = 0; i <= n; i++) {
        bool cell = i < n && bits[i];
        if (i < n && cell == on) { run++; continue; }
        if (on) code += run > 5 ? "#######" : (run >= 2 ? "-" : ".");   // a key-down run ended
        else if (any) {                                           // a gap ended
            if (run >= 12) { if (tryText()) return true; start = -1; }
            else if (run >= 5) { endChar(); text += ' '; }
            else if (run >= 2) endChar();
        }
        if (cell) { any = true; if (start < 0) start = i; }
        on = cell;
        run = 1;
    }
    return tryText();
}

// Same as cellsToMessage, but from key-down/up decisions every quarter dot, reading each run by its
// length the way an operator does. Follows a sender whose timing drifts or jumps (a glitch in a USB
// audio stream), which a fixed dot grid cannot.
static bool runsToMessage(const std::vector<uint8_t>& q, std::string& msg, int& startQ) {
    const int n = (int)q.size();
    std::vector<uint8_t> v(q);
    for (int i = 1; i + 1 < n; i++) if (v[i] != v[i - 1] && v[i] != v[i + 1]) v[i] = v[i - 1];   // 1-sample spikes
    std::string text, code;
    int start = -1;
    auto endChar = [&]() {
        if (code.empty()) return;
        char ch = code.size() <= 6 ? charOf(code.c_str()) : 0;
        text += ch ? ch : '#';
        code.clear();
    };
    auto tryText = [&]() -> bool {
        endChar();
        while (!text.empty() && text.back() == ' ') text.pop_back();
        std::string m;
        bool ok = text.find('#') == std::string::npos && cwUnframe(text, m);
        if (ok) {
            ParsedMsg p;
            ok = parseMsg(m, p) && validCall(p.from) && (p.kind == K_CQ || validCall(p.to)) && p.kind != K_OTHER;
        }
        if (ok) { msg = m; startQ = start; }
        text.clear();
        return ok;
    };
    bool any = false;
    int i = 0;
    while (i < n) {
        int j = i;
        while (j < n && v[j] == v[i]) j++;
        int len = j - i;                                          // quarter dots
        if (v[i]) {
            if (!any) start = i;
            any = true;
            if (len <= 7) code += '.';                            // 1 unit = 4
            else if (len <= 18) code += '-';                      // 3 units = 12
            else code += "#######";
        } else if (any) {
            if (j >= n || len >= 40) { if (tryText()) return true; start = -1; any = false; }
            else if (len >= 20) { endChar(); text += ' '; }      // word gap: 7 units = 28
            else if (len >= 8) endChar();                         // character gap: 3 units = 12
        }
        i = j;
    }
    return any && tryText();
}

// Decodes whatever is keyed on one frequency bin. Levels are in 0.5 dB steps.
bool CwMonitor::decodeBin(int b, int noise, Decode& d) const {
    int hist[256] = {0};
    for (int t = 0; t < frames_; t++) hist[mag_[(size_t)t * nbins_ + b]]++;
    auto pct = [&](float p) { int acc = 0, want = (int)(frames_ * p); for (int v = 0; v < 256; v++) { acc += hist[v]; if (acc > want) return v; } return 255; };
    int sig = pct(0.93f);
    if (sig < noise + 12) return false;                           // key-down level under 6 dB above the noise: not copyable
    int thr0 = std::max(sig - 11, noise + 5);                    // decide at about half amplitude

    // Second pass: the decision level follows the signal - half the key-down level seen within
    // +-0.35 s (a few characters) - so a fade does not drop whole characters below one fixed level.
    // The key-down level is the second highest 3-frame average in that window (one noise spike
    // cannot set it); never below noise + 2.5 dB.
    const int W = std::max(8, (int)(0.35f * 12000 / hop_));
    std::vector<int> sm(frames_), thrAt(frames_);
    for (int t = 0; t < frames_; t++) {
        int a = mag_[(size_t)std::max(0, t - 1) * nbins_ + b], c = mag_[(size_t)t * nbins_ + b], e = mag_[(size_t)std::min(frames_ - 1, t + 1) * nbins_ + b];
        sm[t] = (a + c + e) / 3;
    }
    for (int t = 0; t < frames_; t++) {
        int m1 = 0, m2 = 0;
        for (int k = std::max(0, t - W); k <= std::min(frames_ - 1, t + W); k++) {
            int s = sm[k];
            if (s > m1) { m2 = m1; m1 = s; } else if (s > m2) m2 = s;
        }
        thrAt[t] = (m2 < noise + 12) ? 255 : std::max(m2 - 11, noise + 5);   // nothing keyed nearby: all key-up
    }

    std::vector<int> thrFix(frames_, thr0);
    int startHop = -1, pUsed = 0, startCell = -1;
    const std::vector<int>* thrUsed = nullptr;
    bool found = false;
    // Pass 1: one level for the slot (best for weak, steady signals). Pass 2: the level that
    // follows fading. In each: the fixed dot grid at the two best timing phases (with repair of
    // marginal cells), then reading by run lengths (follows timing drift and audio glitches).
    for (int pass = 0; pass < 2 && !found; pass++) {
        const std::vector<int>& T = pass ? thrAt : thrFix;
        int order[4] = {0, 1, 2, 3};
        long score[4];
        for (int p = 0; p < 4; p++) {
            long sc = 0;
            for (int t = p; t < frames_; t += 4) sc += labs((long)mag_[(size_t)t * nbins_ + b] - std::min(T[t], 254));
            score[p] = sc;
        }
        std::sort(order, order + 4, [&](int a, int c) { return score[a] > score[c]; });
        for (int oi = 0; oi < 2 && !found; oi++) {
            int p = order[oi];
            int ncell = (frames_ - p + 3) / 4;
            std::vector<uint8_t> bits(ncell);
            std::vector<int> margin(ncell);
            for (int i = 0; i < ncell; i++) {
                int t = p + 4 * i, v = mag_[(size_t)t * nbins_ + b];
                bits[i] = v > T[t];
                margin[i] = abs(v - std::min(T[t], 255));
            }
            found = cellsToMessage(bits, d.text, startCell);
            if (!found) {
                // The check group lets us repair a marginal copy: flip the one or two cells that were
                // closest to the decision threshold and see whether a valid message appears.
                const int K = 14;
                std::vector<int> weak(ncell);
                for (int i = 0; i < ncell; i++) weak[i] = i;
                int k = std::min(K, ncell);
                std::partial_sort(weak.begin(), weak.begin() + k, weak.end(), [&](int a, int c) { return margin[a] < margin[c]; });
                for (int i = 0; i < k && !found; i++) {
                    bits[weak[i]] ^= 1;
                    found = cellsToMessage(bits, d.text, startCell);
                    for (int j = i + 1; j < k && !found; j++) {
                        bits[weak[j]] ^= 1;
                        found = cellsToMessage(bits, d.text, startCell);
                        bits[weak[j]] ^= 1;
                    }
                    bits[weak[i]] ^= 1;
                }
            }
            if (found) { pUsed = p; thrUsed = &T; }
        }
        if (!found) {
            std::vector<uint8_t> q(frames_);
            for (int t = 0; t < frames_; t++) q[t] = mag_[(size_t)t * nbins_ + b] > T[t];
            found = runsToMessage(q, d.text, startHop);
            if (found) thrUsed = &T;
        }
    }
    if (!found) return false;
    d.dt = (startHop >= 0 ? (float)startHop : (float)(pUsed + 4 * startCell)) * hop_ / 12000.0f - 0.5f;
    // Signal level: mean power of the key-down frames (signal + noise) minus the noise mean.
    double onSum = 0;
    int onCnt = 0;
    for (int t = 0; t < frames_; t++) {
        int v = mag_[(size_t)t * nbins_ + b];
        if (v > (*thrUsed)[t]) { onSum += pow(10.0, (v - 240) / 20.0); onCnt++; }
    }
    float nLin = powf(10, (noise - 240) / 20.0f), sLin = onCnt ? (float)(onSum / onCnt) : nLin;
    float enbw = 1.234f * 12000.0f / dot_;                      // noise bandwidth of the half-sine window
    float snr = 10 * log10f(std::max(sLin - nLin, nLin * 0.01f) / nLin) - 10 * log10f(2500.0f / enbw) + SNR_CAL;
    d.snr = (int)lroundf(snr < -24 ? -24 : (snr > 40 ? 40 : snr));
    return true;
}

int CwMonitor::decode(std::vector<Decode>& out) {
    if (!inited_ || frames_ < 200) return 0;
    // Where is something being keyed? Compare each bin's strong-signal level with its noise level.
    std::vector<int> peak(nbins_), cand, low(nbins_), noise(nbins_);
    for (int b = 0; b < nbins_; b++) {
        int hist[256] = {0};
        for (int t = 0; t < frames_; t += 2) hist[mag_[(size_t)t * nbins_ + b]]++;
        int acc = 0, total = (frames_ + 1) / 2, p30 = 0, p93 = 255;
        bool got30 = false;
        for (int v = 0; v < 256; v++) {
            acc += hist[v];
            if (!got30 && acc > total * 0.30f) { p30 = v; got30 = true; }
            if (acc > total * 0.93f) { p93 = v; break; }
        }
        peak[b] = p93;
        low[b] = p30;
    }
    // Noise level per bin. A bin that carries a signal is keyed much of the time, so its own low
    // percentile is not pure noise; take the median of the neighbourhood instead (most bins are
    // empty). Noise power is exponentially distributed: its mean is 4.5 dB above its 30th percentile.
    const int span = std::max(8, (int)(400.0f * nfft_ / 12000));
    std::vector<int> tmp;
    for (int b = 0; b < nbins_; b++) {
        tmp.assign(low.begin() + std::max(0, b - span), low.begin() + std::min(nbins_, b + span + 1));
        std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
        noise[b] = std::min(low[b], tmp[tmp.size() / 2]) + 9;
        peak[b] -= noise[b];
    }
    for (int b = 0; b < nbins_; b++) {
        if (peak[b] < 12) continue;
        bool isMax = true;
        for (int k = -3; k <= 3; k++) if (k && b + k >= 0 && b + k < nbins_ && peak[b + k] > peak[b]) isMax = false;
        if (isMax) cand.push_back(b);
    }
    std::sort(cand.begin(), cand.end(), [&](int a, int c) { return peak[a] > peak[c]; });
    if (cand.size() > 40) cand.resize(40);
    size_t first = out.size();
    for (int b : cand) {
        Decode d;
        if (!decodeBin(b, noise[b], d)) continue;
        bool dup = false;
        for (size_t i = first; i < out.size(); i++) if (out[i].text == d.text && fabsf(out[i].freq - (bin0_ + b) * 12000.0f / nfft_) < 120) dup = true;
        if (dup) continue;
        // Interpolate the tone frequency from the bin's neighbours.
        float y0 = b > 0 ? peak[b - 1] : peak[b], y1 = peak[b], y2 = b + 1 < nbins_ ? peak[b + 1] : peak[b];
        float den = y0 - 2 * y1 + y2, delta = fabsf(den) > 1e-3f ? 0.5f * (y0 - y2) / den : 0;
        if (delta > 0.5f) delta = 0.5f;
        if (delta < -0.5f) delta = -0.5f;
        d.freq = (bin0_ + b + delta) * 12000.0f / nfft_;
        out.push_back(d);
    }
    return (int)(out.size() - first);
}
