// FT-CW over a simulated radio path: the sender's audio clock off, slow fading (QSB), and the
// receiver's AGC pumping the noise between elements. On air the IC-705 could not copy an IC-7100
// with the first decoder (one decision level per slot, one fixed dot grid); these cases keep the
// fading-following level and the run-length reader working.
#include "cwx.h"
#include "dsp.h"
#include <cstdio>
#include <cmath>
#include <random>
#include <algorithm>
struct Imp { double clk; float fadeDb, fadeHz; bool agc; float snr; };
static bool one(const char* text, int wpm, Imp im, unsigned seed) {
    const int rate = 48000;
    std::vector<float> sig(15 * rate, 0.0f), mix(15 * rate);
    CwSynth syn;
    syn.start(text, wpm, 1200, (int)lround(rate * im.clk));      // sender's clock: its "48 kHz" is 48 kHz * clk
    std::mt19937 rng(seed);
    std::normal_distribution<float> nd(0, 0.05f);
    float n2500 = 0.05f * 0.05f * 2500.0f / (rate / 2);
    float amp = sqrtf(2 * n2500 * powf(10, im.snr / 10));
    size_t start = (size_t)(0.5 * rate);
    std::uniform_real_distribution<float> ph(0, 6.28f);
    float fph = ph(rng);
    for (int64_t i = 0; i < syn.totalSamples() && start + i < sig.size(); i++) {
        float t = (start + i) / (float)rate;
        float fade = im.fadeDb > 0 ? powf(10, -im.fadeDb / 20 * 0.5f * (1 + sinf(6.2832f * im.fadeHz * t + fph))) : 1;
        sig[start + i] = amp * fade * syn.next();
    }
    for (size_t i = 0; i < mix.size(); i++) mix[i] = sig[i] + nd(rng);
    if (im.agc) {                                               // fast attack, 0.3 s hang/decay, target 0.3
        float env = 0.01f;
        for (size_t i = 0; i < mix.size(); i++) {
            float a = fabsf(mix[i]);
            if (a > env) env += (a - env) * 0.02f; else env *= 0.99996f;
            if (env < 0.01f) env = 0.01f;
            mix[i] *= 0.3f / env;
        }
    }
    Decimator4 dec;
    CwMonitor mon;
    mon.init(wpm);
    std::vector<int16_t> pcm(960);
    std::vector<float> lo(300);
    for (size_t p = 0; p + 960 <= (size_t)(13.6 * rate); p += 960) {
        for (int i = 0; i < 960; i++) { float v = mix[p + i] * 32767 * 0.5f; pcm[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }
        mon.feed(lo.data(), dec.process(pcm.data(), 960, lo.data()));
    }
    std::vector<Decode> out;
    mon.decode(out);
    if (!text[0]) { for (auto& d : out) printf("FALSE DECODE: %s\n", d.text.c_str()); return !out.empty(); }
    for (auto& d : out) if (d.text == text) return true;
    return false;
}
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
int main() {
    const char* msgs[] = {"CQ K1ABC FN42", "K1ABC W9XYZ EN37", "W9XYZ K1ABC R-12", "K1ABC W9XYZ RR73"};
    struct { const char* name; Imp im; int need; } cases[] = {
        {"clean +5 dB", {1.0, 0, 0, false, 5}, 20},
        {"sender clock -0.5 %", {0.995, 0, 0, false, 5}, 16},
        {"sender clock +1 %", {1.01, 0, 0, false, 5}, 18},
        {"QSB 10 dB, 0.3 Hz", {1.0, 10, 0.3f, false, 8}, 18},
        {"receiver AGC, 0 dB", {1.0, 0, 0, true, 0}, 18},
        {"AGC + QSB + clock", {1.004, 10, 0.5f, true, 8}, 4},
    };
    for (auto& c : cases) {
        int ok = 0, n = 0;
        for (unsigned s = 1; s <= 5; s++) for (auto m : msgs) { ok += one(m, 40, c.im, s); n++; }
        printf("  %-22s decoded %2d of %d\n", c.name, ok, n);
        CHECK(ok >= c.need, "%s: only %d of %d", c.name, ok, n);
    }
    int bad = 0;
    for (unsigned s = 1; s <= 200; s++) bad += one("", 40, {1.0, 0, 0, (s & 1) != 0, -100}, s);
    CHECK(bad == 0, "%d false decodes from noise", bad);
    printf(fails ? "t_cwpath: %d FAILED\n" : "t_cwpath: all passed\n", fails);
    return fails ? 1 : 0;
}
