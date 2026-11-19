// Loopback: text -> tones -> 48 kHz GFSK audio -> decimator -> waterfall -> decoder -> text.
#include "../components/core/ftx.h"
#include "../components/core/dsp.h"
#include <cstdio>
#include <cmath>
#include <random>
#include <cstdlib>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Sig { const char* text; float f0; float snr; float dt; };

static void run(bool ft4, std::vector<Sig> sigs, float noiseRms) {
    const int rate = 48000;
    float slot = ft4 ? 7.5f : 15.0f;
    std::vector<float> mix((size_t)(slot * rate), 0.0f);
    std::mt19937 rng(42);
    std::normal_distribution<float> nd(0, noiseRms);
    for (auto& v : mix) v = nd(rng);
    // noise power in 2500 Hz: noiseRms^2 * 2500 / (rate/2)
    float n2500 = noiseRms * noiseRms * 2500.0f / (rate / 2);
    for (auto& s : sigs) {
        std::vector<uint8_t> tones;
        CHECK(ftxEncode(s.text, ft4, tones), "encode %s", s.text);
        FtxSynth syn;
        syn.start(tones, ft4, s.f0, rate);
        float amp = sqrtf(2 * n2500 * powf(10, s.snr / 10));
        size_t start = (size_t)((0.5f + s.dt) * rate);
        for (int64_t i = 0; i < syn.totalSamples() && start + i < mix.size(); i++) mix[start + i] += amp * syn.next();
    }
    Decimator4 dec;
    FtxMonitor mon;
    CHECK(mon.init(ft4), "monitor init");
    mon.reset();
    std::vector<int16_t> pcm(960);
    std::vector<float> lo(300);
    for (size_t p = 0; p + 960 <= mix.size(); p += 960) {
        for (int i = 0; i < 960; i++) { float v = mix[p + i] * 32767; pcm[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }
        size_t n = dec.process(pcm.data(), 960, lo.data());
        mon.feed(lo.data(), n);
    }
    std::vector<Decode> out;
    mon.decode(out);
    printf("%s: %zu signals in, %zu decoded\n", ft4 ? "FT4" : "FT8", sigs.size(), out.size());
    for (auto& s : sigs) {
        bool found = false;
        for (auto& d : out) if (d.text == s.text) {
            found = true;
            printf("  %-22s f=%7.1f (sent %6.1f)  dt=%+.2f (sent %+.2f)  snr=%+3d (sent %+.0f)\n", d.text.c_str(), d.freq, s.f0, d.dt, s.dt, d.snr, s.snr);
            CHECK(fabsf(d.freq - s.f0) < 4, "freq off for %s", s.text);
            CHECK(fabsf(d.dt - s.dt) < 0.12f, "dt off for %s", s.text);
            CHECK(fabsf(d.snr - s.snr) <= 4, "snr estimate off by >4 dB for %s", s.text);
        }
        CHECK(found, "not decoded: %s", s.text);
    }
}

int main() {
    run(false, {{"CQ K1ABC FN42", 1000, 0, 0}, {"K1ABC W9XYZ EN37", 1500.5f, -10, 0.3f}, {"W9XYZ K1ABC -12", 2200, -16, -0.2f},
                {"K1ABC W9XYZ R-07", 600, 10, 0}, {"W9XYZ K1ABC RR73", 2700, -5, 0.1f}, {"CQ DX VK2ABC QF56", 350, -18, 0}}, 0.05f);
    run(true, {{"CQ K1ABC FN42", 1000, 0, 0}, {"K1ABC W9XYZ -05", 1800, -8, 0.2f}, {"W9XYZ K1ABC 73", 2500, 6, 0}}, 0.05f);
    // Late start: first 1.3 s of the transmission missing (decoder ran long) must still decode.
    {
        std::vector<uint8_t> tones;
        ftxEncode("K1ABC W9XYZ EN37", false, tones);
        FtxSynth syn; syn.start(tones, false, 1200, 48000);
        std::vector<float> mix(15 * 48000, 0.0f);
        std::mt19937 rng(7); std::normal_distribution<float> nd(0, 0.05f);
        for (auto& v : mix) v = nd(rng);
        int64_t skip = (int64_t)(1.3 * 48000);
        syn.seek(skip);
        for (int64_t i = skip; i < syn.totalSamples(); i++) mix[24000 + i] += 0.05f * syn.next();
        Decimator4 dec; FtxMonitor mon; mon.init(false); mon.reset();
        std::vector<int16_t> pcm(960); std::vector<float> lo(300);
        for (size_t p = 0; p + 960 <= mix.size(); p += 960) {
            for (int i = 0; i < 960; i++) pcm[i] = (int16_t)(mix[p + i] * 32767);
            mon.feed(lo.data(), dec.process(pcm.data(), 960, lo.data()));
        }
        std::vector<Decode> out; mon.decode(out);
        bool ok = false; for (auto& d : out) ok |= d.text == "K1ABC W9XYZ EN37";
        CHECK(ok, "late-start transmission not decoded");
        printf("late start (1.3 s missing): %s\n", ok ? "decoded" : "LOST");
    }
    printf(fails ? "t_codec: %d FAILED\n" : "t_codec: all passed\n", fails);
    return fails ? 1 : 0;
}
