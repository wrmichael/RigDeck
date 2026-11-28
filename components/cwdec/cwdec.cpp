#include "cwdec.h"
#include <cmath>
#include <cstring>
#include <algorithm>

static const float kPi = 3.14159265358979f;
static const int kRate = 12000;
static const int kDec = 48;                       // envelope tick: 48 samples = 4 ms (250 Hz)
static const float kTickMs = 1000.0f * kDec / kRate;

// The same alphabet the FT-CW modem uses (cwx.cpp), plus the common prosigns a plain decoder
// meets on the air. Prosigns print in brackets so they are not mistaken for letters.
static const struct { const char* code; const char* text; } kMorse[] = {
    {".-", "A"}, {"-...", "B"}, {"-.-.", "C"}, {"-..", "D"}, {".", "E"}, {"..-.", "F"}, {"--.", "G"},
    {"....", "H"}, {"..", "I"}, {".---", "J"}, {"-.-", "K"}, {".-..", "L"}, {"--", "M"}, {"-.", "N"},
    {"---", "O"}, {".--.", "P"}, {"--.-", "Q"}, {".-.", "R"}, {"...", "S"}, {"-", "T"}, {"..-", "U"},
    {"...-", "V"}, {".--", "W"}, {"-..-", "X"}, {"-.--", "Y"}, {"--..", "Z"},
    {"-----", "0"}, {".----", "1"}, {"..---", "2"}, {"...--", "3"}, {"....-", "4"}, {".....", "5"},
    {"-....", "6"}, {"--...", "7"}, {"---..", "8"}, {"----.", "9"},
    {"-..-.", "/"}, {".-.-.", "+"}, {"-....-", "-"}, {"..--..", "?"}, {"-...-", "="}, {".-.-.-", "."},
    {"--..--", ","}, {"-.-.-.", ";"}, {"---...", ":"}, {".----.", "'"}, {"-.-.--", "!"}, {"-.--.", "("},
    {"-.--.-", ")"}, {".-...", "&"}, {".--.-.", "@"},
    {"...-.-", "<SK>"}, {"-...-", "<BT>"}, {".-.-.", "<AR>"}, {"-.--.", "<KN>"}, {"-.-.-", "<CT>"},
};
static const char* textOf(const char* code) {
    // Letters first: ".-.-." is both '+' and <AR>; the table order keeps the letter.
    for (auto& m : kMorse) if (!strcmp(m.code, code)) return m.text;
    return nullptr;
}

CwDecRx::CwDecRx() {}

void CwDecRx::setFreq(float f) {
    if (f < 200) f = 200;
    if (f > 2900) f = 2900;
    f0_ = f;
    afc_ = 0;
    i1_ = q1_ = i2_ = q2_ = i3_ = q3_ = pi_ = pq_ = 0;
}

int CwDecRx::wpm() const {
    if (elemMs_.empty() && !key_ && runTicks_ > 250) return 0;    // nothing heard for a while
    return (int)lroundf(1200.0f / dotMs_);
}

void CwDecRx::process(const float* s, size_t n, std::string& out) {
    // Quadrature mixer at the tracked pitch, three single-pole low-pass sections (~85 Hz), and
    // an envelope sample every 4 ms. The tracker watches the I/Q vector turn while the tone is
    // keyed: its turning rate is the pitch error, and the mixer follows it within +-200 Hz.
    const float alpha = 1.0f - expf(-2.0f * kPi * 85.0f / kRate);
    for (size_t k = 0; k < n; k++) {
        float inc = 2.0f * kPi * (f0_ + afc_) / kRate;
        ph_ += inc;
        if (ph_ > 2.0f * kPi) ph_ -= 2.0f * kPi;
        float i = s[k] * cosf(ph_), q = s[k] * sinf(ph_);
        i1_ += alpha * (i - i1_); q1_ += alpha * (q - q1_);
        i2_ += alpha * (i1_ - i2_); q2_ += alpha * (q1_ - q2_);
        i3_ += alpha * (i2_ - i3_); q3_ += alpha * (q2_ - q3_);
        if (++dec_ < kDec) continue;
        dec_ = 0;
        float env = 2.0f * sqrtf(i3_ * i3_ + q3_ * q3_);
        if (key_ && env > 0.6f * peak_) {                           // pitch tracking, on a solid tone only
            float cross = i3_ * pq_ - q3_ * pi_;
            float dot = i3_ * pi_ + q3_ * pq_;
            if (dot > 1e-9f || cross != 0) {
                float errHz = atan2f(cross, dot) / (2.0f * kPi * (kDec / (float)kRate));
                if (errHz > -300 && errHz < 300) {
                    afc_ += 0.03f * errHz;
                    if (afc_ > 200) afc_ = 200;
                    if (afc_ < -200) afc_ = -200;
                }
            }
        }
        pi_ = i3_; pq_ = q3_;
        tick(env, out);
    }
}

void CwDecRx::tick(float env, std::string& out) {
    // Signal and noise levels: the peak decays slowly; the noise level is the average envelope
    // while the tone is off (the minimum would sit near zero and let noise key the decoder).
    // The keying threshold sits between them, with hysteresis, and the squelch stays shut until
    // the tone stands about 7 dB over the noise in the filter's bandwidth.
    ticks_++;
    if (env > peak_) peak_ = env; else peak_ *= 0.9993f;
    float onThr = noise_ + 0.45f * (peak_ - noise_);
    float offThr = noise_ + 0.25f * (peak_ - noise_);
    // Squelch with memory: opening from silence takes a clear tone (9 dB over the noise),
    // but once copy is under way it stays open on far less - a radio AGC pumps the noise up
    // during the long pauses of slow CW, and a ratio-only squelch would shut on exactly the
    // slow, weak senders. The thresholds themselves are peak-relative, so pumped noise
    // (which stays under the tone's own level) does not key the decoder either way.
    float sqRatio = (seenTone_ && ticks_ - lastKeyTick_ < 750) ? 1.6f : 3.0f;
    bool strong = peak_ > sqRatio * noise_ && peak_ > 0.002f;
    strong_ = strong;
    // Average the envelope while the tone is off; while it is keyed the level is held, so a
    // long dah cannot pull the noise level up to itself.
    if (!key_) noise_ += (env - noise_) * 0.04f;
    else if (key_) noise_ += (env - noise_) * 0.0002f;
    if (noise_ < 1e-6f) noise_ = 1e-6f;
    float snrDb = 20.0f * log10f(peak_ / noise_);
    q_ = snrDb <= 0 ? 0 : (snrDb >= 30 ? 1.0f : snrDb / 30.0f);
    bool key = key_ ? (env > offThr) : (env > onThr);
    if (!strong || ticks_ < 75) key = false;                        // 0.3 s of warm-up while levels settle
    if (key) {
        lastKeyTick_ = ticks_;
        if (env > curElemPk_) curElemPk_ = env;
        if (++keyRun_ >= 12 && env > 0.6f * peak_) seenTone_ = true;  // a real element, not noise chatter
    } else keyRun_ = 0;

    runTicks_++;
    float runMs = runTicks_ * kTickMs;
    // A dropout (fading dip, AGC sag) inside an element is bridged if it is short next to a
    // dot; at slow speeds that allows up to 32 ms, at fast speeds it shrinks with the dot so
    // real gaps between elements are never bridged.
    float bridgeMs = std::min(32.0f, 0.4f * dotMs_);
    if (key && !key_) {                                             // key down: a gap just ended
        if (elemPending_ && runMs < bridgeMs) {
            curElemMs_ += runMs;                                    // a dropout, not a gap: the element goes on
        } else {
            if (elemPending_) commitElem();
            if (!elemMs_.empty() && !charOut_) {
                if (runMs >= 2.3f * dotMs_) {                       // a character (or word) gap
                    emitChar(out);
                    charOut_ = true;
                    if (runMs >= 5.5f * dotMs_) { out += ' '; spaceOut_ = true; }
                } else {                                            // a gap inside a character: one dot
                    if (lastElemPk_ >= 0.3f * peak_) gapMs_.push_back((int)lroundf(runMs));
                    if (lastElemPk_ >= 0.3f * peak_)
                        dotMs_ = std::min(300.0f, std::max(20.0f, 0.6f * dotMs_ + 0.4f * runMs));
                }
            }
            charOut_ = spaceOut_ = false;
        }
        runTicks_ = 0;
    } else if (!key && key_) {                                      // key up: an element may have ended
        curElemMs_ += runMs;
        elemPending_ = true;
        runTicks_ = 0;
    } else if (!key) {                                              // still in a gap: print as soon as it is long enough
        if (elemPending_ && runMs >= bridgeMs) commitElem();
        if (!elemMs_.empty() && !charOut_ && runMs >= 2.3f * dotMs_) { emitChar(out); charOut_ = true; }
        if (charOut_ && !spaceOut_ && runMs >= 5.5f * dotMs_) { out += ' '; spaceOut_ = true; }
    }
    key_ = key;
}

void CwDecRx::commitElem() {
    if (curElemMs_ >= 12) {                                         // shorter is a glitch, not a dit
        elemMs_.push_back((int)lroundf(curElemMs_));
        elemPk_.push_back(curElemPk_);
        lastElemPk_ = curElemPk_;
        bool real = curElemPk_ >= 0.3f * peak_;    // a real element of the tone, not a noise burst
        if (curElemMs_ < 1.5f * dotMs_ && real)    // clearly a dit: it measures the dot
            dotMs_ = std::min(300.0f, std::max(20.0f, 0.8f * dotMs_ + 0.2f * curElemMs_));
        // Speed rescue: if every recent element is far longer than our dot, the sender is
        // slower than we think (the gaps alone may never say so - at 10 wpm the first gaps
        // look like character gaps to a decoder still set for 20). Pull the dot toward the
        // shortest recent element; symmetrically if all recent elements are short.
        if (real) {
            elemHist_[elemHistI_] = (int)lroundf(curElemMs_);
            elemHistI_ = (elemHistI_ + 1) % 16;
            if (elemHistN_ < 16) elemHistN_++;
        }
        if (elemHistN_ >= 4 && real) {
            // Second extremes, not the extremes: one stray short or long element must not
            // veto the rescue, but dah-heavy text (digits) still has to find its dits.
            int h[16];
            for (int i = 0; i < elemHistN_; i++) h[i] = elemHist_[i];
            std::sort(h, h + elemHistN_);
            int lo = h[1], hi = h[elemHistN_ - 2];
            // Pull up only toward a dit-like length: if the recent elements are bimodal
            // (dits and dahs both present) but the second-shortest is already dah-like,
            // the sender is not slow - the sample is just dah-heavy (a callsign's 9).
            bool bimodal = h[0] < 0.6f * h[elemHistN_ - 1];
            if (lo > 1.6f * dotMs_ && (!bimodal || lo < 0.6f * hi)) dotMs_ = 0.7f * dotMs_ + 0.3f * lo;
            else if (hi < 1.6f * dotMs_) dotMs_ = 0.7f * dotMs_ + 0.3f * hi;
            dotMs_ = std::min(300.0f, std::max(20.0f, dotMs_));
        }
    }
    curElemMs_ = 0;
    curElemPk_ = 0;
    elemPending_ = false;
}

void CwDecRx::emitChar(std::string& out) {
    if (elemMs_.empty()) return;
    // Drop elements that were far weaker than the tone's peak: a noise burst just before a
    // transmission starts can key the decoder for a few tens of ms, and without this it would
    // be glued onto the first real character. One character's elements share one signal
    // level (fading within a character rarely reaches 10 dB), so the cut is safe.
    {
        std::vector<int> kept;
        for (size_t i = 0; i < elemMs_.size(); i++)
            if (i >= elemPk_.size() || elemPk_[i] >= 0.3f * peak_) kept.push_back(elemMs_[i]);
        if (kept.empty()) { elemMs_.clear(); elemPk_.clear(); gapMs_.clear(); return; }
        elemMs_ = kept;
    }
    // Decide dit from dah for this character. The gaps inside a character are one dot each, so
    // they measure the dot directly; failing that, a character holding both dits and dahs shows
    // them as two clusters. The running estimate covers the rest (T, M, O and friends) and is
    // pulled toward whatever this character measured.
    int mn = *std::min_element(elemMs_.begin(), elemMs_.end());
    int mx = *std::max_element(elemMs_.begin(), elemMs_.end());
    float dotSample = 0;
    if (!gapMs_.empty()) {
        std::vector<int> g = gapMs_;
        std::nth_element(g.begin(), g.begin() + g.size() / 2, g.end());
        dotSample = (float)g[g.size() / 2];
    } else if (mx > 1.8f * mn) {
        dotSample = (float)mn;
    }
    float thr;
    if (mx > 1.8f * mn) thr = 1.8f * mn;                            // both kinds present: split them here
    else thr = 2.0f * dotMs_;                                       // one kind only: trust the running speed
    std::string code;
    for (int d : elemMs_) code += (d < thr ? '.' : '-');
    if (dotSample > 0) dotMs_ = 0.5f * dotMs_ + 0.5f * dotSample;
    else if (code.find('.') != std::string::npos) dotMs_ = 0.8f * dotMs_ + 0.2f * mn;
    if (dotMs_ < 20) dotMs_ = 20;                                   // 60 wpm
    if (dotMs_ > 300) dotMs_ = 300;                                 // 4 wpm
    const char* t = code.size() <= 7 ? textOf(code.c_str()) : nullptr;
    out += t ? t : "#";
    elemMs_.clear();
    elemPk_.clear();
    gapMs_.clear();
}
