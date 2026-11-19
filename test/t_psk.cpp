// PSK31 loopback: text -> varicode -> BPSK at 48 kHz -> decimate -> noise + frequency error -> demod -> text.
#include "psk31.h"
#include "dsp.h"
#include <cstdio>
#include <random>
#include <cmath>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::string loop(const std::string& text, float txF, float rxF, float noiseRms, float amp, float* imdOut = nullptr) {
    Psk31Tx tx;
    tx.start(txF, 48000);
    tx.queue(text);
    tx.finish();
    Decimator4 dec;
    Psk31Rx rx;
    rx.setFreq(rxF);
    std::mt19937 rng(1);
    std::normal_distribution<float> nd(0, noiseRms);
    std::string out;
    int16_t pcm[960];
    float lo[300];
    float peak = 0;
    int guard = 0;
    for (int blk = 0; blk < 5000 && guard < 30; blk++) {
        for (int i = 0; i < 960; i++) {
            float s = amp * tx.next();
            if (fabsf(s) > peak) peak = fabsf(s);
            pcm[i] = (int16_t)((s + nd(rng)) * 32767);
        }
        if (tx.done()) guard++;                   // run on a little so the filters flush
        rx.process(lo, dec.process(pcm, 960, lo), out);
    }
    if (imdOut) *imdOut = peak / amp;
    return out;
}

int main() {
    CHECK(varicodeEncode(' ') == 1 && varicodeEncode('e') == 3 && varicodeEncode('t') == 5 && varicodeEncode('\n') == 0x1D);
    for (int c = 0; c < 128; c++) CHECK(varicodeDecode(varicodeEncode((unsigned char)c)) == c);
    const std::string msg = "CQ CQ CQ de W9XYZ W9XYZ pse k\nThe quick brown fox 0123456789 ?/=-.,";
    float peak;
    std::string a = loop(msg, 1000, 1000, 0.0f, 0.5f, &peak);
    printf("clean        : [%s]\n", a.c_str());
    CHECK(a.find(msg) != std::string::npos);
    CHECK(peak <= 1.001f);                         // envelope never exceeds the carrier level
    std::string b = loop(msg, 1500, 1504, 0.2f, 0.2f);    // 4 Hz mistuned, about 25 dB SNR in 31 Hz: AFC must pull in
    printf("4 Hz off+noise: [%s]\n", b.c_str());
    CHECK(b.find("de W9XYZ W9XYZ pse k") != std::string::npos && b.find("quick brown fox 0123456789") != std::string::npos);
    std::string n = loop(std::string(300, 'e'), 1000, 2000, 0.1f, 0.0f);       // noise only, nothing on frequency: squelch stays shut
    printf("noise only   : %zu chars printed\n", n.size());
    CHECK(n.size() <= 2);
    printf(fails ? "t_psk: %d FAILED\n" : "t_psk: all passed\n", fails);
    return fails ? 1 : 0;
}
