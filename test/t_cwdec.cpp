// Plain CW decoder loopback: text -> Morse audio at 48 kHz -> decimator -> noise -> decoder -> text.
#include "cwdec.h"
#include "dsp.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <random>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const struct { char c; const char* code; } kMorse[] = {
    {'A', ".-"}, {'B', "-..."}, {'C', "-.-."}, {'D', "-.."}, {'E', "."}, {'F', "..-."}, {'G', "--."}, {'H', "...."},
    {'I', ".."}, {'J', ".---"}, {'K', "-.-"}, {'L', ".-.."}, {'M', "--"}, {'N', "-."}, {'O', "---"}, {'P', ".--."},
    {'Q', "--.-"}, {'R', ".-."}, {'S', "..."}, {'T', "-"}, {'U', "..-"}, {'V', "...-"}, {'W', ".--"}, {'X', "-..-"},
    {'Y', "-.--"}, {'Z', "--.."}, {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"}, {'4', "....-"},
    {'5', "....."}, {'6', "-...."}, {'7', "--..."}, {'8', "---.."}, {'9', "----."}, {'/', "-..-."}, {'?', "..--.."}};
static const char* morseOf(char c) { for (auto& m : kMorse) if (m.c == c) return m.code; return nullptr; }

// Machine-timed Morse at 48 kHz, with 5 ms raised-cosine edges like the FT-CW synth.
static std::vector<float> synth(const std::string& text, int wpm, float f0, float amp) {
    const int rate = 48000;
    int unit = (int)lroundf(1.2f * rate / wpm);
    std::vector<uint8_t> cells;
    bool firstInWord = true;
    for (char ch : text) {
        if (ch == ' ') { if (!cells.empty()) cells.insert(cells.end(), 7, 0); firstInWord = true; continue; }
        const char* code = morseOf(ch);
        if (!code) continue;
        if (!firstInWord) cells.insert(cells.end(), 3, 0);
        firstInWord = false;
        for (const char* p = code; *p; p++) {
            if (p != code) cells.push_back(0);
            cells.insert(cells.end(), *p == '-' ? 3 : 1, 1);
        }
    }
    std::vector<float> out(cells.size() * unit, 0);
    float ph = 0;
    int ramp = std::min(unit / 4, rate * 5 / 1000);
    for (size_t u = 0; u < cells.size(); u++) {
        for (int i = 0; i < unit; i++) {
            float env = 0;
            if (cells[u]) {
                env = 1;
                bool prev = u > 0 && cells[u - 1], nxt = u + 1 < cells.size() && cells[u + 1];
                if (!prev && i < ramp) env = (1 - cosf(3.14159265f * i / ramp)) / 2;
                else if (!nxt && unit - i <= ramp) env = (1 - cosf(3.14159265f * (unit - i) / ramp)) / 2;
            }
            out[u * unit + i] = amp * env * sinf(ph);
            ph += 2 * 3.14159265f * f0 / rate;
            if (ph > 2 * 3.14159265f) ph -= 2 * 3.14159265f;
        }
    }
    return out;
}

static std::string run(const std::string& text, int wpm, float txF, float rxF, float noiseRms, float amp, int* wpmOut = nullptr, float* fOut = nullptr) {
    std::vector<float> sig = synth(text, wpm, txF, amp);
    sig.insert(sig.begin(), 48000 / 2, 0.0f);                        // half a second of quiet first
    sig.insert(sig.end(), 48000, 0.0f);                              // and a second after, to flush the last word
    Decimator4 dec;
    CwDecRx rx;
    rx.setFreq(rxF);
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0, noiseRms);
    std::string out;
    int16_t pcm[960];
    float lo[300];
    for (size_t p = 0; p + 960 <= sig.size(); p += 960) {
        for (int i = 0; i < 960; i++) {
            float v = (sig[p + i] + nd(rng)) * 32767;
            pcm[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
        rx.process(lo, dec.process(pcm, 960, lo), out);
    }
    if (wpmOut) *wpmOut = rx.wpm();
    if (fOut) *fOut = rx.freq();
    return out;
}

// Like run(), but the signal+noise first goes through a radio-style AGC: fast attack,
// ~400 ms release toward high gain, so the noise pumps up in the gaps between elements.
// Optional slow fading on the signal. This is what real HF CW through a rig sounds like.
static std::string runAgc(const std::string& text, int wpm, float txF, float rxF, float noiseRms, float amp,
                          bool fade, int* wpmOut = nullptr) {
    std::vector<float> sig = synth(text, wpm, txF, amp);
    sig.insert(sig.begin(), 48000 / 2, 0.0f);
    sig.insert(sig.end(), 48000, 0.0f);
    Decimator4 dec;
    CwDecRx rx;
    rx.setFreq(rxF);
    std::mt19937 rng(11);
    std::normal_distribution<float> nd(0, noiseRms);
    std::string out;
    int16_t pcm[960];
    float lo[300];
    float gain = 1.0f, lvl = 0;
    const float atk = expf(-1.0f / (0.005f * 48000));     // 5 ms attack
    const float rel = expf(-1.0f / (0.400f * 48000));     // 400 ms release
    for (size_t p = 0; p + 960 <= sig.size(); p += 960) {
        for (int i = 0; i < 960; i++) {
            float t = (p + i) / 48000.0f;
            float s = sig[p + i];
            if (fade) s *= 0.65f + 0.35f * sinf(2 * 3.14159265f * 0.3f * t);
            float x = s + nd(rng);
            lvl = fmaxf(fabsf(x), lvl * 0.999f);
            if (lvl * gain > 0.4f) gain *= atk; else gain = fminf(30.0f, gain / rel);
            float v = x * gain * 32767;
            pcm[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
        rx.process(lo, dec.process(pcm, 960, lo), out);
    }
    if (wpmOut) *wpmOut = rx.wpm();
    return out;
}

int main() {
    const std::string msg = "CQ CQ DE W9XYZ W9XYZ K";
    for (int wpm : {13, 20, 30, 40}) {
        int est = 0;
        std::string a = run(msg, wpm, 700, 700, 0.0f, 0.3f, &est);
        printf("%2d wpm clean      : [%s] (est %d wpm)\n", wpm, a.c_str(), est);
        CHECK(a.find(msg) != std::string::npos, "clean %d wpm: got [%s]", wpm, a.c_str());
        CHECK(est >= wpm - 3 && est <= wpm + 3, "wpm estimate %d at %d wpm", est, wpm);
    }
    std::string b = run("W9XYZ DE K1ABC UR 599 599 BK", 25, 650, 700, 0.05f, 0.2f);
    printf("25 wpm 50 Hz off+noise: [%s]\n", b.c_str());
    CHECK(b.find("W9XYZ DE K1ABC UR 599") != std::string::npos, "offset+noise: got [%s]", b.c_str());
    float f = 0;
    std::string c = run(msg, 20, 800, 700, 0.0f, 0.3f, nullptr, &f);
    printf("20 wpm 100 Hz off   : [%s] (tracking %.0f Hz)\n", c.c_str(), f);
    CHECK(c.find(msg) != std::string::npos, "100 Hz off: got [%s]", c.c_str());
    CHECK(f > 770 && f < 830, "AFC tracked to %.0f Hz, want ~800", f);
    std::string d = run("PARIS PARIS 0123456789", 18, 600, 600, 0.15f, 0.15f);
    printf("18 wpm noisy        : [%s]\n", d.c_str());
    CHECK(d.find("PARIS PARIS 0123456789") != std::string::npos, "noisy: got [%s]", d.c_str());
    std::string n = run("", 20, 700, 700, 0.1f, 0.0f);
    printf("noise only          : %zu chars printed\n", n.size());
    CHECK(n.size() <= 2, "noise only printed [%s]", n.c_str());
    std::string o = run("CQ DE W9XYZ", 20, 1500, 700, 0.0f, 0.3f);   // a strong signal 800 Hz away is not copied
    printf("off-pitch signal    : [%s]\n", o.c_str());
    CHECK(o.find("W9XYZ") == std::string::npos, "copied a signal 800 Hz away: [%s]", o.c_str());
    // Slow CW through a radio AGC (noise pumps up in the gaps), with and without fading.
    for (int wpm : {8, 10, 13, 15, 18, 25, 35}) {
        int est = 0;
        std::string a = runAgc(msg, wpm, 700, 700, 0.03f, 0.25f, false, &est);
        printf("%2d wpm AGC        : [%s] (est %d wpm)\n", wpm, a.c_str(), est);
        CHECK(a.find("CQ") != std::string::npos && a.find("W9XYZ") != std::string::npos,
              "AGC %d wpm: got [%s]", wpm, a.c_str());
    }
    for (int wpm : {10, 15, 20}) {
        std::string a = runAgc(msg, wpm, 700, 700, 0.03f, 0.25f, true);
        printf("%2d wpm AGC+fade   : [%s]\n", wpm, a.c_str());
        CHECK(a.find("W9XYZ") != std::string::npos, "AGC+fade %d wpm: got [%s]", wpm, a.c_str());
    }
    for (int wpm : {12, 18}) {   // weaker signal through the AGC
        std::string a = runAgc(msg, wpm, 700, 700, 0.05f, 0.12f, false);
        printf("%2d wpm AGC weak   : [%s]\n", wpm, a.c_str());
        CHECK(a.find("W9XYZ") != std::string::npos, "AGC weak %d wpm: got [%s]", wpm, a.c_str());
    }
    printf(fails ? "t_cwdec: %d FAILED\n" : "t_cwdec: all passed\n", fails);
    return fails ? 1 : 0;
}
