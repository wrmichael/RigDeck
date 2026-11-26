// Packet radio core: AX.25 frames of every kind, KISS framing, and two connected-mode links talking through a
// digipeater on a channel that loses frames.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "ax25.h"
#include "aprs.h"

static int checks = 0, fails = 0;
#define CHECK(x) do { checks++; if (!(x)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

static uint32_t rng = 12345;
static int rnd(int n) { rng = rng * 1103515245u + 12345u; return (int)((rng >> 16) % (uint32_t)n); }

struct Air { int64_t at; ax25::Frame f; int to; };

// Runs two links until both are idle or the time is up; loss = percent of frames lost, digi = via a digipeater.
static void run(ax25::Link& a, ax25::Link& b, int64_t& now, int64_t until, int loss, bool digi) {
    std::vector<Air> air;
    ax25::Link* L[2] = {&a, &b};
    while (now < until) {
        for (int i = 0; i < 2; i++) {
            for (auto& f : L[i]->takeOut()) {
                // through the encoder and decoder, as on the air
                std::vector<uint8_t> bytes = ax25::encodeFcs(f);
                ax25::Frame g;
                if (!ax25::decodeFcs(bytes.data(), bytes.size(), g)) { fails++; printf("FAIL frame did not decode\n"); continue; }
                if (digi) for (auto& p : g.path) if (p.back() != '*') p += "*";       // repeated by the digipeater
                if (rnd(100) < loss) continue;
                air.push_back({now + 300 + rnd(400) + (digi ? 600 : 0), g, 1 - i});
            }
        }
        for (size_t k = 0; k < air.size();) {
            if (air[k].at <= now) { L[air[k].to]->onFrame(air[k].f, now); air.erase(air.begin() + (long)k); }
            else k++;
        }
        a.tick(now);
        b.tick(now);
        now += 50;
    }
}

int main() {
    // ---- frames
    ax25::Frame f;
    f.dst = "W9AIM-1"; f.src = "AC9HP"; f.path = {"KB9XYZ-7", "WIDE2-1"}; f.command = true;
    f.ctl = ax25::ctlI(3, 5, true); f.pid = 0xF0; f.info = "Hello BBS\r";
    std::vector<uint8_t> b = ax25::encodeFcs(f);
    ax25::Frame g;
    CHECK(ax25::decodeFcs(b.data(), b.size(), g));
    CHECK(g.dst == "W9AIM-1" && g.src == "AC9HP" && g.path.size() == 2 && g.command && g.kind() == ax25::K_I && g.ns() == 3 && g.nr() == 5 && g.pf() && g.info == "Hello BBS\r");
    CHECK(ax25::monitor(g) == "AC9HP>W9AIM-1,KB9XYZ-7,WIDE2-1 <I C S3 R5 P>: Hello BBS ");
    b[20] ^= 1;
    CHECK(!ax25::decodeFcs(b.data(), b.size(), g));                                  // damaged
    ax25::Frame u; u.dst = "AC9HP"; u.src = "W9AIM-1"; u.command = false; u.ctl = ax25::ctlU(ax25::K_UA, true); u.pid = -1;
    b = ax25::encodeFcs(u);
    CHECK(ax25::decodeFcs(b.data(), b.size(), g) && g.kind() == ax25::K_UA && !g.command && g.pf() && g.info.empty());
    CHECK(ax25::monitor(g) == "W9AIM-1>AC9HP <UA R F>");
    // an APRS UI frame decodes both ways
    aprs::Frame af; af.src = "AC9HP-9"; af.dst = "APZRDK"; af.path = {"WIDE1-1"}; af.info = ">RigDeck";
    b = aprs::encodeFrame(af);
    CHECK(ax25::decodeFcs(b.data(), b.size(), g) && g.kind() == ax25::K_UI && g.pid == 0xF0 && g.info == ">RigDeck");
    CHECK(ax25::reversePath({"A-1*", "B*"}) == "B,A-1");

    // ---- KISS
    std::vector<uint8_t> raw = {0x01, 0xC0, 0x02, 0xDB, 0x03};
    std::vector<uint8_t> k = ax25::kissWrap(raw);
    CHECK(k.size() == 10 && k[0] == 0xC0 && k[1] == 0 && k[3] == 0xDB && k[4] == 0xDC && k[6] == 0xDB && k[7] == 0xDD && k.back() == 0xC0);
    ax25::KissDecoder kd;
    std::vector<std::vector<uint8_t>> out;
    std::vector<uint8_t> two = k;
    std::vector<uint8_t> k2 = ax25::kissWrap({0x41, 0x42}, 0x01);
    two.insert(two.end(), k2.begin(), k2.end());
    kd.feed(two.data(), 4, out);                                                     // in pieces
    kd.feed(two.data() + 4, two.size() - 4, out);
    CHECK(out.size() == 2 && out[0].size() == 6 && out[0][0] == 0 && std::vector<uint8_t>(out[0].begin() + 1, out[0].end()) == raw && out[1][0] == 0x01);

    // ---- connected mode through a digipeater, 15 % of frames lost
    ax25::Link a, z;
    a.me = "AC9HP"; z.me = "W9AIM-1";
    int64_t now = 1000;
    std::string err;
    CHECK(!a.connect("bad call!", "", now, err) && !err.empty());
    CHECK(a.connect("w9aim-1", "KB9XYZ-7", now, err));
    run(a, z, now, now + 20000, 15, true);
    CHECK(a.state() == ax25::Link::CONNECTED && z.state() == ax25::Link::CONNECTED && z.peer() == "AC9HP" && z.via() == "KB9XYZ-7");
    std::string msgA, msgZ;
    for (int i = 0; i < 40; i++) msgA += "Line " + std::to_string(i) + " from AC9HP: the quick brown fox jumps over the lazy dog\n";
    for (int i = 0; i < 15; i++) msgZ += "BBS line " + std::to_string(i) + "\n";
    a.send(msgA);
    z.send(msgZ);
    run(a, z, now, now + 600000, 15, true);
    std::string gotA, gotZ;
    int infoA = 0;
    for (auto& e : z.takeEvents()) if (e.type == ax25::Link::Event::DATA) gotZ += e.text;
    for (auto& e : a.takeEvents()) { if (e.type == ax25::Link::Event::DATA) gotA += e.text; else infoA++; }
    CHECK(gotZ == msgA);                                                             // in order, nothing lost or doubled
    CHECK(gotA == msgZ);
    CHECK(a.waiting() == 0 && z.waiting() == 0 && infoA >= 2);
    a.disconnect(now);
    run(a, z, now, now + 60000, 15, true);
    CHECK(a.state() == ax25::Link::DISCONNECTED && z.state() == ax25::Link::DISCONNECTED);

    // ---- nobody there: gives up after N2 tries
    ax25::Link c, nobody;
    c.me = "AC9HP"; c.n2 = 3; nobody.me = "N0NE";
    CHECK(c.connect("W9XYZ", "", now, err));
    run(c, nobody, now, now + 60000, 0, false);
    bool gaveUp = false;
    for (auto& e : c.takeEvents()) if (e.text.find("No answer") != std::string::npos) gaveUp = true;
    CHECK(c.state() == ax25::Link::DISCONNECTED && gaveUp);

    // ---- refusing: a station that does not take connections answers DM
    ax25::Link d, busy;
    d.me = "AC9HP"; busy.me = "W9AIM"; busy.acceptIncoming = false;
    CHECK(d.connect("W9AIM", "", now, err));
    run(d, busy, now, now + 10000, 0, false);
    CHECK(d.state() == ax25::Link::DISCONNECTED && busy.state() == ax25::Link::DISCONNECTED);

    printf(fails ? "t_ax25: %d of %d FAILED\n" : "t_ax25: all %d checks passed\n", fails ? fails : checks, checks);
    return fails ? 1 : 0;
}
