// APRS: AX.25 framing, AFSK 1200 loopback through the 48 -> 12 kHz decimator with noise and tilt,
// and the position / Mic-E / message formats.
#include "aprs.h"
#include "dsp.h"
#include "app.h"
#include "clock.h"
#include <sys/stat.h>
#include <cstdio>
#include <cmath>
#include <random>

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
using namespace aprs;

// Modulate frames, optionally tilt (one-pole filter = de-emphasis) and add noise, decimate, demodulate.
static std::vector<Frame> loop(const std::vector<Frame>& in, float noise, float tilt, unsigned seed) {
    std::vector<std::vector<uint8_t>> raw;
    for (const auto& f : in) raw.push_back(encodeFrame(f));
    AfskMod m;
    m.start(raw, 48000, 300);
    std::vector<int16_t> pcm;
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0, 1);
    float lp = 0;
    for (int i = 0; i < 48000 / 4; i++) pcm.push_back((int16_t)(noise * 8000 * g(rng)));   // quiet lead-in
    while (!m.done()) {
        float v = m.next();
        lp += tilt * (v - lp);                                     // tilt < 1: high tones weaker
        float o = tilt < 1 ? lp * 1.6f : v;
        pcm.push_back((int16_t)(o * 8000 + noise * 8000 * g(rng)));
    }
    for (int i = 0; i < 48000 / 4; i++) pcm.push_back((int16_t)(noise * 8000 * g(rng)));
    Decimator4 dec;
    AfskDemod dm;
    std::vector<Frame> out;
    float buf[512];
    for (size_t p = 0; p < pcm.size(); p += 960) {
        size_t n = std::min<size_t>(960, pcm.size() - p);
        size_t k = dec.process(pcm.data() + p, n, buf);
        for (size_t j = 0; j < k; j++) buf[j] /= 8000.0f / 32768.0f * 4;   // around +-1
        std::vector<std::vector<uint8_t>> got;
        dm.feed(buf, k, got);
        for (auto& b : got) { Frame f; if (decodeFrame(b.data(), b.size(), f)) out.push_back(f); }
    }
    return out;
}

int main() {
    // --- AX.25 ---
    Frame f;
    f.src = "AC9HP-9"; f.dst = "APZRDK"; f.path = {"WIDE1-1", "WIDE2-1"};
    f.info = encodePosition(39.7686, -86.1581, '/', '>', "RigDeck test", true);
    auto b = encodeFrame(f);
    Frame g;
    CHECK(decodeFrame(b.data(), b.size(), g) && g.src == "AC9HP-9" && g.dst == "APZRDK" && g.path.size() == 2 && g.path[1] == "WIDE2-1" && g.info == f.info);
    CHECK(tnc2(g) == "AC9HP-9>APZRDK,WIDE1-1,WIDE2-1:" + f.info);
    b[20] ^= 1;
    CHECK(!decodeFrame(b.data(), b.size(), g));                    // FCS catches it
    { Frame d = f; d.path = {"W9ABC-1*", "WIDE2-1"}; auto bb = encodeFrame(d); Frame e; CHECK(decodeFrame(bb.data(), bb.size(), e) && e.path[0] == "W9ABC-1*"); }
    CHECK(validCallSsid("AC9HP") && validCallSsid("AC9HP-15") && !validCallSsid("AC9HP-16") && !validCallSsid("ac9hp") && !validCallSsid("TOOLONG1"));
    { const uint8_t t[] = {'1','2','3','4','5','6','7','8','9'}; CHECK(fcs(t, 9) == 0x906E); }   // CRC-16/X-25 check value

    // --- position formats ---
    Position p;
    CHECK(f.info.compare(0, 19, "=3946.12N/08609.49W") == 0);
    CHECK(parsePosition(f, p) && fabs(p.lat - 39.7686) < 0.001 && fabs(p.lon + 86.1581) < 0.001 && p.symbol == '>' && p.table == '/' && p.comment == "RigDeck test");
    Frame t; t.dst = "APRS"; t.info = "@092345z4903.50N/07201.75W>Test";
    CHECK(parsePosition(t, p) && fabs(p.lat - 49.0583) < 0.001 && fabs(p.lon + 72.0292) < 0.001);
    t.info = "!/5L!!<*e7>7P[";                                         // compressed example from the APRS spec: 49.5 N, 72.75 W
    CHECK(parsePosition(t, p) && fabs(p.lat - 49.5) < 0.01 && fabs(p.lon + 72.75) < 0.01 && p.symbol == '>');
    // Mic-E example from the APRS 1.01 spec: destination S32U6T = 33 25.64 N; info gives 112 07.74 W.
    t.dst = "S32U6T"; t.info = std::string("`") + (char)(112 + 28) + (char)(7 + 28) + (char)(74 + 28) + "\x1c\x1c\x1c" + ">/" + "mobile";
    CHECK(parsePosition(t, p) && p.mice && fabs(p.lat - (33 + 25.64 / 60)) < 0.001 && fabs(p.lon + (112 + 7.74 / 60)) < 0.001 && p.symbol == '>' && p.table == '/');
    t.info = ">status text";
    CHECK(!parsePosition(t, p));

    // --- messages ---
    Message m;
    CHECK(encodeMessage("W9ABC", "Hello there", "12") == ":W9ABC    :Hello there{12");
    CHECK(parseMessage(":W9ABC    :Hello there{12", m) && m.to == "W9ABC" && m.text == "Hello there" && m.id == "12" && !m.isAck);
    CHECK(parseMessage(encodeAck("AC9HP-9", "12"), m) && m.isAck && m.id == "12" && m.to == "AC9HP-9");
    CHECK(parseMessage(":AC9HP    :Hi{AB}CD", m) && m.id == "AB" && m.text == "Hi");

    // --- geometry ---
    double la, lo;
    CHECK(gridCentre("EM69", la, lo) && fabs(la - 39.5) < 0.01 && fabs(lo + 87) < 0.01);
    CHECK(fabs(distanceKm(39.77, -86.16, 41.88, -87.63) - 266) < 5 && abs(bearingDeg(39.77, -86.16, 41.88, -87.63) - 333) < 3);

    // --- modem loopback ---
    std::vector<Frame> two = {f};
    Frame f2 = f; f2.src = "W9ABC-7"; f2.info = encodeMessage("AC9HP-9", "Test message", "1"); two.push_back(f2);
    auto clean = loop(two, 0.0f, 2.0f, 1);
    CHECK(clean.size() == 2 && clean[0].info == f.info && clean[1].src == "W9ABC-7");
    int ok = 0, total = 0;
    for (unsigned s = 1; s <= 20; s++) { auto r = loop({f}, 0.15f, 2.0f, s); total++; if (r.size() == 1 && r[0].info == f.info) ok++; }
    printf("t_aprs: noisy loopback %d of %d decoded\n", ok, total);
    CHECK(ok >= 18);
    int okT = 0;
    for (unsigned s = 1; s <= 10; s++) { auto r = loop({f}, 0.05f, 0.35f, s); if (r.size() == 1 && r[0].info == f.info) okT++; }   // de-emphasised audio
    printf("t_aprs: tilted loopback %d of 10 decoded\n", okT);
    CHECK(okT >= 9);
    auto none = loop({}, 0.3f, 2.0f, 7);
    CHECK(none.empty());                                            // noise alone gives nothing

    // --- through the App: receive a position and a message for us, see them in the state JSON ---
    {
        mkdir("test/out", 0755);
        mkdir("test/out/aprs", 0755);
        remove("test/out/aprs/settings.txt");
        App app;
        app.begin("test/out/aprs");
        app.usbSerial = true;
        app.usbAudio = true;
        app.command("set", {{"call", "AC9HP"}, {"grid", "EM69"}});
        app.command("mode", {{"m", "APRS"}});
        Frame a; a.src = "W9ABC-7"; a.dst = "APRS"; a.path = {"WIDE1-1"};
        a.info = encodePosition(39.9, -86.0, '/', '>', "mobile", false);
        Frame b2 = a; b2.info = encodeMessage("AC9HP", "Hello RigDeck", "7");
        AfskMod m;
        m.start({encodeFrame(a), encodeFrame(b2)}, 48000, 300);
        std::vector<int16_t> pcm;
        while (!m.done()) pcm.push_back((int16_t)(8000 * m.next()));
        for (int i = 0; i < 4800; i++) pcm.push_back(0);
        for (size_t p = 0; p < pcm.size(); p += 480) app.rxAudio48k(pcm.data() + p, std::min<size_t>(480, pcm.size() - p));
        app.tick();
        std::string js = app.stateJson(0, 0, false);
        CHECK(js.find("\"mode\":\"APRS\"") != std::string::npos);
        CHECK(js.find("\"c\":\"W9ABC-7\"") != std::string::npos && js.find("\"km\":") != std::string::npos);
        CHECK(js.find("Hello RigDeck") != std::string::npos);
        CHECK(app.command("aprsmsg", {{"to", "W9ABC-7"}, {"text", "Hi back"}}).find("\"ok\":true") != std::string::npos);
        CHECK(app.command("aprsmsg", {{"to", "bad call!"}, {"text", "x"}}).find("\"ok\":false") != std::string::npos);
        CHECK(app.command("set", {{"aprsPath", "WIDE1-1,WIDE2-1"}, {"aprsSym", "/>"}, {"aprsLat", "39.77"}, {"aprsLon", "-86.16"}}).find("\"ok\":true") != std::string::npos);
        CHECK(app.command("set", {{"aprsPath", "WIDE1-1,,"}}).find("\"ok\":false") != std::string::npos);
    }

    printf(fails ? "t_aprs: %d of %d FAILED\n" : "t_aprs: all %d checks passed\n", fails ? fails : checks, checks);
    return fails ? 1 : 0;
}
