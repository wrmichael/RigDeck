// End-to-end simulation: two complete stations (two App instances) joined by an audio channel with
// noise, each with a simulated IC-7100 on its CI-V port, running on a simulated clock.
// Station A calls CQ automatically, station B hunts CQs automatically. They must find each other,
// complete a QSO, key their radios only in their own slots, log it, and forward the log over UDP.
#include "app.h"
#include "clock.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int64_t simMs = 0;
static int64_t mono() { return simMs; }
static const int64_t T0 = 1794328740000LL;         // 2026-11-10 16:39:00 UTC, a slot boundary

struct Station {
    App app;
    CivParser parser;
    bool ptt = false;
    uint64_t freq = 14074000;
    int dataMode = 0;
    struct Ev { int64_t utc; bool on; };
    std::vector<Ev> pttLog;
    int16_t txBuf[960];
    bool txNow = false;
    std::mt19937 rng;
    std::string name;

    void init(const char* dir, const char* call, const char* grid, int op, unsigned seed) {
        name = call;
        rng.seed(seed);
        mkdir(dir, 0755);
        remove((std::string(dir) + "/log.adi").c_str());
        remove((std::string(dir) + "/archive.adi").c_str());
        remove((std::string(dir) + "/settings.txt").c_str());
        app.radio.write = [this](const uint8_t* d, size_t n) { fakeRadio(d, n); return true; };
        app.begin(dir);
        app.usbSerial = true;
        app.usbAudio = true;
        char ops[4];
        snprintf(ops, sizeof ops, "%d", op);
        std::string r = app.command("set", {{"call", call}, {"grid", grid}, {"opMode", ops}, {"txLevel", "30"}});
        CHECK(r.find("\"ok\":true") != std::string::npos);
    }
    void reply(uint8_t cmd, std::vector<uint8_t> data) {
        auto f = civBuild(Radio::CTRL, 0x88, cmd, data);
        app.radio.onRx(f.data(), f.size());
    }
    void fakeRadio(const uint8_t* d, size_t n) {
        app.radio.onRx(d, n);                        // the USB CI-V port echoes what it is sent
        for (size_t i = 0; i < n; i++) {
            CivFrame f;
            if (!parser.feed(d[i], f) || f.to != 0x88) continue;
            if (f.cmd == 0x03) { uint8_t b[5]; civFreqToBcd(freq, b); reply(0x03, std::vector<uint8_t>(b, b + 5)); }
            else if (f.cmd == 0x05 && f.data.size() == 5) { freq = civBcdToFreq(f.data.data(), 5); reply(0xFB, {}); }
            else if (f.cmd == 0x04) reply(0x04, {0x01, 0x01});
            else if (f.cmd == 0x1A && f.data.size() == 1) reply(0x1A, {0x06, (uint8_t)dataMode, 0x01});
            else if (f.cmd == 0x1A && f.data.size() == 3) { dataMode = f.data[1]; reply(0xFB, {}); }
            else if (f.cmd == 0x15) reply(0x15, {f.data[0], 0x00, 0x50});
            else if (f.cmd == 0x1C && f.data.size() == 1) reply(0x1C, {0x00, (uint8_t)ptt});
            else if (f.cmd == 0x1C && f.data.size() == 2) {
                bool on = f.data[1] != 0;
                if (on != ptt) pttLog.push_back({clk::utcMs(), on});
                ptt = on;
                reply(0xFB, {});
            } else reply(0xFB, {});
        }
    }
};

static void runSim(Station& A, Station& B, int seconds, int64_t slotLen, bool slowDecoderB, float noise = 0.02f) {
    std::normal_distribution<float> nd(0, noise * 32767);
    int16_t rx[960];
    for (int step = 0; step < seconds * 50; step++) {
        simMs += 20;
        int64_t tIn = clk::utcMs() % slotLen;
        Station* st[2] = {&A, &B};
        for (auto s : st) {
            s->app.tick();
            s->txNow = s->app.txActive();
            if (s->txNow) {
                s->app.txAudio48k(s->txBuf, 960);
            } else memset(s->txBuf, 0, sizeof s->txBuf);
        }
        if (A.ptt && B.ptt) CHECK(!"both stations keyed at once");
        for (int k = 0; k < 2; k++) {
            Station* me = st[k];
            Station* peer = st[1 - k];
            if (me->txNow) continue;                 // half duplex
            for (int i = 0; i < 960; i++) {
                float v = (peer->ptt ? peer->txBuf[i] * 0.3f : 0) + nd(me->rng);   // signal only when the peer's radio is keyed
                rx[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            }
            me->app.rxAudio48k(rx, 960);
        }
        A.app.decodeOnce();
        // A slow decoder finishes 0.8 s into the next slot, forcing a late (mid-message) start.
        if (!slowDecoderB || (tIn >= 800 && tIn < slotLen - 2000)) B.app.decodeOnce();
        A.app.netOnce();
        B.app.netOnce();
    }
}

static std::string slurp(const std::string& p) {
    std::string s;
    FILE* f = fopen(p.c_str(), "r");
    if (!f) return s;
    int c;
    while ((c = fgetc(f)) != EOF) s += (char)c;
    fclose(f);
    return s;
}

static void checkPtt(Station& s, int parity, int64_t slotLen, int64_t latestOn) {
    CHECK(!s.pttLog.empty() && s.pttLog.size() % 2 == 0);
    for (size_t i = 0; i + 1 < s.pttLog.size(); i += 2) {
        auto& on = s.pttLog[i];
        auto& off = s.pttLog[i + 1];
        CHECK(on.on && !off.on);
        int64_t slot = on.utc / slotLen;
        CHECK((slot & 1) == parity);                             // keyed only in our own slots
        CHECK(on.utc % slotLen <= latestOn);                     // keyed at the start of the slot
        CHECK(off.utc / slotLen == slot);                        // unkeyed before the slot ends
        CHECK(off.utc - on.utc < slotLen - 300);
    }
}

static void scenario(const char* title, const char* mode, int64_t slotLen, bool slowB, const char* tag) {
    printf("--- %s ---\n", title);
    Station A, B;
    std::string da = std::string("test/out/") + tag + "A", db = std::string("test/out/") + tag + "B";
    A.init(da.c_str(), "W9XYZ", "EN37", OP_AUTO_CQ, 1);
    CHECK(A.app.command("set", {{"op", "k9op"}, {"mySig", "pota"}, {"mySigInfo", "us-1234"}}).find("\"ok\":true") != std::string::npos);
    B.init(db.c_str(), "K1ABC", "FN42", OP_AUTO_HUNT, 2);

    // UDP listener standing in for a logging program (WSJT-X protocol).
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    sa.sin_port = 0;
    CHECK(bind(sock, (struct sockaddr*)&sa, sizeof sa) == 0);
    socklen_t sl = sizeof sa;
    getsockname(sock, (struct sockaddr*)&sa, &sl);
    fcntl(sock, F_SETFL, O_NONBLOCK);
    char port[8];
    snprintf(port, sizeof port, "%d", ntohs(sa.sin_port));
    CHECK(A.app.command("set", {{"logProto", "3"}, {"logHost", "127.0.0.1"}, {"logPort", port}}).find("true") != std::string::npos);

    if (!clk::valid()) CHECK(A.app.command("txen", {{"v", "1"}}).find("Clock not set") != std::string::npos);   // refuses without a clock
    clk::setUtcMs(T0 + 3000, "test");
    A.app.command("mode", {{"m", mode}});
    B.app.command("mode", {{"m", mode}});
    A.app.command("band", {{"hz", "7074000"}});
    runSim(A, B, 2, slotLen, slowB);
    CHECK(A.freq == 7074000 && A.dataMode == 1);                 // band change reached the radio as USB-D
    CHECK(A.app.command("txen", {{"v", "1"}}).find("true") != std::string::npos);
    CHECK(B.app.command("txen", {{"v", "1"}}).find("true") != std::string::npos);
    CHECK(B.pttLog.empty());

    int secs = (int)(slotLen * 9 / 1000);
    for (int i = 0; i < secs; i += 5) {                          // a browser polling keeps the watchdog happy
        runSim(A, B, 5, slotLen, slowB);
        A.app.stateJson(0, 0, false);
        B.app.stateJson(0, 0, false);
    }
    std::string la = slurp(da + "/log.adi"), lb = slurp(db + "/log.adi");
    printf("A log: %s", la.c_str() + (la.find("<EOH>") == std::string::npos ? 0 : la.find("<EOH>") + 6));
    printf("B log: %s", lb.c_str() + (lb.find("<EOH>") == std::string::npos ? 0 : lb.find("<EOH>") + 6));
    auto count = [](const std::string& s) { int n = 0; for (size_t p = 0; (p = s.find("<EOR>", p)) != std::string::npos; p++) n++; return n; };
    CHECK(count(la) == 1 && count(lb) == 1);                     // one QSO each, no duplicates, no re-calling
    CHECK(adifField(la, "CALL") == "K1ABC" && adifField(la, "GRIDSQUARE") == "FN42" && adifField(la, "BAND") == "40m");
    CHECK(adifField(lb, "CALL") == "W9XYZ" && adifField(lb, "GRIDSQUARE") == "EN37");
    CHECK(adifField(la, "RST_SENT") == adifField(lb, "RST_RCVD") && adifField(lb, "RST_SENT") == adifField(la, "RST_RCVD"));
    CHECK(adifField(la, "QSO_DATE") == "20261110" && adifField(la, "STATION_CALLSIGN") == "W9XYZ");
    CHECK(adifField(la, "OPERATOR") == "K9OP" && adifField(la, "MY_SIG") == "POTA" && adifField(la, "MY_SIG_INFO") == "US-1234");
    CHECK(lb.find("OPERATOR") == std::string::npos && lb.find("MY_SIG") == std::string::npos);
    if (!strcmp(mode, "FT4")) CHECK(adifField(la, "MODE") == "MFSK" && adifField(la, "SUBMODE") == "FT4");
    else if (!strcmp(mode, "CW")) CHECK(adifField(la, "MODE") == "CW" && la.find("SUBMODE") == std::string::npos);
    else CHECK(adifField(la, "MODE") == "FT8");
    checkPtt(A, 0, slotLen, 300);
    checkPtt(B, 1, slotLen, slowB ? 1200 : 300);
    printf("A keyed %zu times, B keyed %zu times\n", A.pttLog.size() / 2, B.pttLog.size() / 2);
    CHECK(!A.ptt && !B.ptt);

    uint8_t pkt[1500];
    ssize_t n = recv(sock, pkt, sizeof pkt, 0);
    CHECK(n > 40 && pkt[0] == 0xAD && pkt[1] == 0xBC && pkt[11] == 12);
    if (n > 0) CHECK(std::string((char*)pkt, n).find("<CALL:5>K1ABC") != std::string::npos);
    close(sock);

    std::string js = A.app.stateJson(0, 0, true);
    FILE* f = fopen((std::string("test/out/state_") + tag + ".json").c_str(), "w");
    fputs(js.c_str(), f);
    fclose(f);
    CHECK(js.find("\"CQ W9XYZ EN37\"") != std::string::npos && js.find("apPass\"") == std::string::npos);

    // Log management: the archive got the same record; clearing the current log keeps the archive
    // and worked-before, and survives a restart.
    CHECK(count(slurp(db + "/archive.adi")) == 1 && adifField(slurp(db + "/archive.adi"), "CALL") == "W9XYZ");
    CHECK(B.app.command("clearlog", {}).find("\"ok\":false") != std::string::npos && count(slurp(db + "/log.adi")) == 1);
    CHECK(B.app.command("clearlog", {{"confirm", "yes"}}).find("true") != std::string::npos);
    CHECK(slurp(db + "/log.adi").empty() && count(slurp(db + "/archive.adi")) == 1);
    CHECK(B.app.stateJson(0, 0, false).find("\"log\":{\"n\":0,\"all\":1,\"behind\":false") != std::string::npos);
    {
        App again;
        again.begin(db);
        again.radio.freq = 14074000;                 // station B never changed band
        CHECK(again.seq.workedBefore("W9XYZ") && !again.seq.workedBefore("N0CALL"));
        CHECK(again.stateJson(0, 0, false).find("\"log\":{\"n\":0,\"all\":1") != std::string::npos);
        CHECK(again.command("eraselogs", {{"confirm", "yes"}}).find("true") != std::string::npos);
        CHECK(slurp(db + "/archive.adi").empty() && !again.seq.workedBefore("W9XYZ"));
        CHECK(again.stateJson(0, 0, false).find("\"log\":{\"n\":0,\"all\":0") != std::string::npos);
    }

    // Halt must unkey at once, mid-transmission.
    size_t before = A.pttLog.size();
    for (int i = 0; i < 4000 && !A.ptt; i++) runSim(A, B, 0, slotLen, slowB), simMs += 20, A.app.tick(), A.app.stateJson(0, 0, false);
    CHECK(A.ptt);
    runSim(A, B, 2, slotLen, slowB);
    CHECK(A.ptt);
    A.app.command("halt", {});
    simMs += 20; A.app.tick();
    simMs += 60; A.app.tick();
    CHECK(!A.ptt && A.pttLog.size() == before + 2);
    CHECK(!A.app.txActive());

    // Nobody watching for longer than the watchdog time: automatic transmit must switch itself off.
    CHECK(A.app.command("txen", {{"v", "1"}}).find("true") != std::string::npos);
    size_t keyed = A.pttLog.size();
    runSim(A, B, 5 * 60, slotLen, slowB);
    CHECK(A.pttLog.size() > keyed);                              // still calling CQ inside the window
    runSim(A, B, 90, slotLen, slowB);
    keyed = A.pttLog.size();
    runSim(A, B, 60, slotLen, slowB);
    CHECK(A.pttLog.size() == keyed && !A.ptt);                   // watchdog (6 min) has stopped it
    CHECK(A.app.stateJson(0, 0, false).find("\"wd\":true") != std::string::npos);
}

int main() {
    clk::init(mono);
    mkdir("test/out", 0755);
    scenario("FT8: auto CQ station meets auto hunt station", "FT8", 15000, false, "ft8");
    scenario("FT8: station B's decoder is slow (late starts)", "FT8", 15000, true, "slow");
    scenario("FT4", "FT4", 7500, false, "ft4");
    scenario("FT-CW: the same contact in 40 wpm Morse", "CW", 15000, false, "cw");
    scenario("FT-CW with a slow decoder (whole message sent late)", "CW", 15000, true, "cwslow");
    printf(fails ? "t_qso: %d of %d FAILED\n" : "t_qso: all %d checks passed\n", fails ? fails : checks, checks);
    return fails ? 1 : 0;
}
