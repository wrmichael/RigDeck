// FT-CW loopback: messages -> Morse audio at 48 kHz -> decimator -> noise -> multi-frequency decoder.
#include "cwx.h"
#include "dsp.h"
#include <cstdio>
#include <cmath>
#include <random>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
struct Sig { const char* text; float f0, snr, dt; };

static size_t run(int wpm, std::vector<Sig> sigs, float noiseRms, bool expectAll, unsigned seed = 3) {
    const int rate = 48000;
    std::vector<float> mix(15 * rate, 0.0f);
    std::mt19937 rng(seed);
    std::normal_distribution<float> nd(0, noiseRms);
    for (auto& v : mix) v = nd(rng);
    float n2500 = noiseRms * noiseRms * 2500.0f / (rate / 2);
    for (auto& s : sigs) {
        CwSynth syn;
        CHECK(syn.start(s.text, wpm, s.f0, rate), "cannot encode %s", s.text);
        float amp = sqrtf(2 * n2500 * powf(10, s.snr / 10));
        size_t start = (size_t)((0.5f + s.dt) * rate);
        for (int64_t i = 0; i < syn.totalSamples() && start + i < mix.size(); i++) mix[start + i] += amp * syn.next();
    }
    Decimator4 dec;
    CwMonitor mon;
    CHECK(mon.init(wpm), "init");
    std::vector<int16_t> pcm(960);
    std::vector<float> lo(300);
    for (size_t p = 0; p + 960 <= (size_t)(13.6 * rate); p += 960) {
        for (int i = 0; i < 960; i++) { float v = mix[p + i] * 32767; pcm[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }
        mon.feed(lo.data(), dec.process(pcm.data(), 960, lo.data()));
    }
    std::vector<Decode> out;
    mon.decode(out);
    size_t good = 0;
    for (auto& s : sigs) {
        bool found = false;
        for (auto& d : out) if (d.text == s.text && fabsf(d.freq - s.f0) < 30) {
            found = true; good++;
            if (expectAll) {
                printf("  %2d wpm %-22s f=%7.1f (sent %6.1f) dt=%+.2f (sent %+.2f) snr=%+3d (sent %+.0f)\n", wpm, d.text.c_str(), d.freq, s.f0, d.dt, s.dt, d.snr, s.snr);
                CHECK(fabsf(d.freq - s.f0) < 8, "freq off for %s", s.text);
                CHECK(fabsf(d.dt - s.dt) < 0.08f, "dt off for %s", s.text);
                CHECK(fabsf(d.snr - s.snr) <= 4, "snr off for %s", s.text);
            }
        }
        if (expectAll) CHECK(found, "not decoded at %d wpm: %s (snr %+.0f)", wpm, s.text, s.snr);
    }
    for (auto& d : out) {                                // nothing may be decoded that was not sent
        bool sent = false;
        for (auto& s : sigs) sent |= d.text == s.text;
        CHECK(sent, "false decode: %s", d.text.c_str());
    }
    return good;
}

int main() {
    std::string m;
    CHECK(cwUnframe(cwFrame("CQ K1ABC FN42"), m) && m == "CQ K1ABC FN42", "frame round trip");
    std::string bad = cwFrame("CQ K1ABC FN42"); bad[3] = 'J';
    CHECK(!cwUnframe(bad, m), "corrupted frame accepted");
    CHECK(cwUnits("E E") == 9 && cwUnits("PARIS") == 43 && cwUnits("a") == -1, "unit counts");
    for (auto t : {"CQ K1ABC FN42", "PJ4/K1ABC W9XYZ/P R-12", "W9XYZ K1ABC RR73"})
        printf("on-air time at 40 wpm: %-24s %.1f s   (30 wpm: %.1f s, 25 wpm: %.1f s)\n", t, cwSeconds(t, 40), cwSeconds(t, 30), cwSeconds(t, 25));
    CHECK(cwSeconds("PJ4/K1ABC W9XYZ/P R-12", 40) < 12.5, "longest standard message must fit a slot at 40 wpm");

    std::vector<Sig> band = {{"CQ K1ABC FN42", 600, 5, 0}, {"K1ABC W9XYZ EN37", 1000.5f, 0, 0.3f}, {"W9XYZ K1ABC -12", 1400, -6, -0.2f},
                             {"K1ABC W9XYZ R+07", 1810, 12, 0.6f}, {"W9XYZ K1ABC RR73", 2200, -3, 0.1f}, {"CQ DX VK2DEF QF56", 2650, 20, -0.4f}};
    run(40, band, 0.05f, true);
    run(30, {{"CQ K1ABC FN42", 700, -5, 0}, {"K1ABC W9XYZ EN37", 1500, 3, 0.2f}, {"W9XYZ K1ABC 73", 2300, -4, 0}}, 0.05f, true);
    run(25, {{"CQ K1ABC FN42", 900, -9, 0.1f}, {"W9XYZ K1ABC RR73", 2000, 0, 0}}, 0.05f, true);
    run(40, {{"CQ K1ABC FN42", 1500, 10, 0}}, 0.0005f, true);         // very quiet band (radio AGC off, no noise)

    // Noise only, many times over: the check group must keep garbage out of the log.
    for (unsigned seed = 1; seed <= 300; seed++) run(40, {}, 0.05f, false, seed);
    // Copy rate near the threshold at 40 wpm.
    for (float snr : {-12.f, -10.f, -8.f, -6.f}) {
        size_t ok = 0;
        for (unsigned seed = 1; seed <= 20; seed++) ok += run(40, {{"K1ABC W9XYZ EN37", 1500, snr, 0}}, 0.05f, false, seed);
        printf("40 wpm, %+.0f dB (2500 Hz): decoded %zu of 20\n", snr, ok);
    }
    for (float snr : {-10.f, -8.f}) {
        size_t ok = 0;
        for (unsigned seed = 1; seed <= 20; seed++) ok += run(30, {{"K1ABC W9XYZ EN37", 1500, snr, 0}}, 0.05f, false, seed);
        printf("30 wpm, %+.0f dB (2500 Hz): decoded %zu of 20\n", snr, ok);
    }
    printf(fails ? "t_cw: %d FAILED\n" : "t_cw: all passed\n", fails);
    return fails ? 1 : 0;
}
