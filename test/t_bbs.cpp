// Packet BBS: a station connects over AX.25 (through a lossy channel), lists, sends, reads and kills messages;
// the messages survive a restart (load from the directory); privacy of personal mail; a full BBS refuses.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "ax25.h"
#include "bbs.h"

static int checks = 0, fails = 0;
#define CHECK(x) do { checks++; if (!(x)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

static uint32_t rng = 777;
static int rnd(int n) { rng = rng * 1103515245u + 12345u; return (int)((rng >> 16) % (uint32_t)n); }

struct Air { int64_t at; ax25::Frame f; bool toBbs; };

// Runs a user's link against the BBS; returns what the user received.
static std::string run(ax25::Link& u, bbs::Bbs& b, int64_t& now, int64_t until, int loss) {
    std::vector<Air> air;
    std::string got;
    auto put = [&](const ax25::Frame& f, bool toBbs) {
        std::vector<uint8_t> bytes = ax25::encodeFcs(f);
        ax25::Frame g;
        if (!ax25::decodeFcs(bytes.data(), bytes.size(), g)) { fails++; return; }
        if (rnd(100) < loss) return;
        air.push_back({now + 200 + rnd(300), g, toBbs});
    };
    while (now < until) {
        for (auto& f : u.takeOut()) put(f, true);
        for (auto& f : b.takeOut()) put(f, false);
        for (size_t k = 0; k < air.size();) {
            if (air[k].at <= now) {
                if (air[k].toBbs) b.onFrame(air[k].f, now); else u.onFrame(air[k].f, now);
                air.erase(air.begin() + (long)k);
            } else k++;
        }
        u.tick(now);
        b.tick(now);
        for (auto& e : u.takeEvents()) if (e.type == ax25::Link::Event::DATA) got += e.text;
        now += 50;
    }
    return got;
}

int main() {
    std::string dir = "test/out/bbs";
    if (system(("rm -rf " + dir).c_str()) != 0) return 1;
    bbs::Bbs b;
    b.call = "AC9HP-1"; b.sysop = "AC9HP"; b.dir = dir;
    b.utcMs = [] { return (int64_t)1795600000000LL; };
    b.heard = [] { return std::string("W9AIM 12:00\n"); };
    b.load();
    CHECK(b.msgs().empty());
    std::string err;
    CHECK(b.post("AC9HP", "ALL", "Welcome", "Welcome to the RigDeck BBS.", err));        // sysop bulletin
    CHECK(!b.post("AC9HP", "not a call", "x", "y", err) && !err.empty());

    ax25::Link u;
    u.me = "W9AIM";
    int64_t now = 1000;
    CHECK(u.connect("AC9HP-1", "", now, err));
    std::string got = run(u, b, now, now + 15000, 10);
    CHECK(u.state() == ax25::Link::CONNECTED);
    CHECK(got.find("RigDeck BBS AC9HP-1") != std::string::npos && got.find("Hello W9AIM") != std::string::npos);
    CHECK(b.users().size() == 1 && b.users()[0] == "W9AIM");

    u.send("L\r");                                                   // as a terminal sends it (CR)
    got = run(u, b, now, now + 20000, 10);
    CHECK(got.find("Welcome") != std::string::npos && got.find("AC9HP-1 BBS >") != std::string::npos);

    u.send("S KB9XYZ\nTest message\nHello there\nSecond line\n/EX\n");
    got = run(u, b, now, now + 40000, 10);
    CHECK(got.find("Message 2 saved for KB9XYZ") != std::string::npos);
    CHECK(b.msgs().size() == 2 && b.msgs().at(2).from == "W9AIM" && b.msgs().at(2).body == "Hello there\nSecond line\n" && b.msgs().at(2).subject == "Test message");
    CHECK(b.post("KB9XYZ", "W9AIM", "Reply", "Got it", err));       // one for W9AIM, and one he may not see
    CHECK(b.post("KB9XYZ", "N0CALL", "Private", "Not for W9AIM", err));

    u.send("RM\n");
    got = run(u, b, now, now + 30000, 10);
    CHECK(got.find("Got it") != std::string::npos && b.msgs().at(3).read);
    u.send("R 4\nL\n");
    got = run(u, b, now, now + 30000, 10);
    CHECK(got.find("Not for W9AIM") == std::string::npos && got.find("No message 4") != std::string::npos && got.find("Private") == std::string::npos);
    u.send("K 4\nK 2\nJ\n");
    got = run(u, b, now, now + 30000, 10);
    CHECK(got.find("Message 2 killed") != std::string::npos && b.msgs().count(4) && !b.msgs().count(2));
    CHECK(got.find("W9AIM 12:00") != std::string::npos);

    u.send("B\n");
    run(u, b, now, now + 30000, 10);
    CHECK(u.state() == ax25::Link::DISCONNECTED && b.users().empty());
    bool sawLog = false;
    for (auto& l : b.takeLog()) if (l.find("W9AIM left message 2") != std::string::npos) sawLog = true;
    CHECK(sawLog);

    // ---- restart: messages come back from the card, numbering continues
    bbs::Bbs b2;
    b2.call = "AC9HP-1"; b2.dir = dir; b2.utcMs = b.utcMs;
    b2.load();
    CHECK(b2.msgs().size() == 3 && b2.msgs().at(3).read && b2.msgs().at(3).body == "Got it\n" && b2.msgs().at(1).date == "2026-11-25 0946Z");
    CHECK(b2.post("W9AIM", "AC9HP", "After restart", "x", err) && b2.msgs().rbegin()->first == 5);

    // ---- full: the oldest read message or bulletin makes room; otherwise refused
    b2.maxMsgs = 4;
    CHECK(b2.post("W9AIM", "AC9HP", "Five", "x", err) && b2.msgs().size() == 4 && !b2.msgs().count(1));
    CHECK(b2.post("W9AIM", "AC9HP", "Six", "x", err) && !b2.msgs().count(3));   // 3 was read
    CHECK(!b2.post("W9AIM", "AC9HP", "Seven", "x", err) && err == "The BBS is full");

    // ---- busy: a fourth user is refused, others are not answered as the BBS
    bbs::Bbs b3;
    b3.call = "AC9HP-1"; b3.dir = dir; b3.maxUsers = 0;
    b3.load();
    ax25::Link v;
    v.me = "N0CALL";
    now += 1000;
    CHECK(v.connect("AC9HP-1", "", now, err));
    run(v, b3, now, now + 10000, 0);
    CHECK(v.state() == ax25::Link::DISCONNECTED);
    ax25::Link w;
    w.me = "N0CALL"; w.n2 = 2;
    CHECK(w.connect("AC9HP-2", "", now, err));                      // not the BBS's call: silence
    run(w, b, now, now + 30000, 0);
    CHECK(w.state() == ax25::Link::DISCONNECTED);

    printf(fails ? "t_bbs: %d of %d FAILED\n" : "t_bbs: all %d checks passed\n", fails ? fails : checks, checks);
    return fails ? 1 : 0;
}
