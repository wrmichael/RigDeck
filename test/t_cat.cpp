// Yaesu FTX-1 and QRP Labs QMX control, CW macros, and manual log entries, against simulated radios.
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <string>
#include <vector>
#include "app.h"
#include "clock.h"

static int fails = 0, checks = 0;
#define CHECK(x) do { checks++; if (!(x)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x); } } while (0)

static int64_t simMs = 0;
static int64_t mono() { return simMs; }

// A radio that answers the way the manuals describe.
struct FakeText {
    Radio* r = nullptr;
    bool yaesu = true, kenwood = false;
    std::string yid = "ID0800";
    uint64_t freq = 14074000;
    char mode = '2';
    bool tx = false, bk = false;
    int head = 1, watts = 10, ag = 128, ks = 20, nl = 0, vfo = 0;
    std::string line, km, keyed;
    std::vector<std::string> log;
    void say(const std::string& s) { std::string t = s + ";"; r->onRx((const uint8_t*)t.data(), t.size()); }
    void rx(const uint8_t* d, size_t n) {
        for (size_t i = 0; i < n; i++) {
            if (d[i] != ';') { line += (char)d[i]; continue; }
            std::string c; c.swap(line);
            log.push_back(c);
            if (kenwood) kenwoodCmd(c); else if (yaesu) yaesuCmd(c); else qmxCmd(c);
        }
    }
    void yaesuCmd(const std::string& c) {
        char b[48];
        if (c == "ID") say(yid);
        else if (c == "PC" && yid != "ID0800") { snprintf(b, sizeof b, "PC%03d", watts); say(b); }
        else if (c.compare(0, 2, "PC") == 0 && c.size() == 5 && yid != "ID0800") watts = atoi(c.c_str() + 2);
        else if (c == "FA") { snprintf(b, sizeof b, "FA%09llu", (unsigned long long)freq); say(b); }
        else if (c.compare(0, 2, "FA") == 0) { if (c.size() != 11) say("?"); else freq = strtoull(c.c_str() + 2, nullptr, 10); }
        else if (c == "MD0") say(std::string("MD0") + mode);
        else if (c.compare(0, 3, "MD0") == 0 && c.size() == 4) mode = c[3];
        else if (c == "TX") say(tx ? "TX1" : "TX0");
        else if (c == "TX1") tx = true;
        else if (c == "TX0") tx = false;
        else if (c == "SM0") say("SM0120");
        else if (c == "RM5") say("RM5150000");
        else if (c == "RM6") say("RM6010000");
        else if (c == "RM4") say("RM4040000");
        else if (c == "PC") { snprintf(b, sizeof b, "PC%d%03d", head, watts); say(b); }
        else if (c.compare(0, 2, "PC") == 0 && c.size() == 6) watts = atoi(c.c_str() + 3);
        else if (c == "AG0") { snprintf(b, sizeof b, "AG0%03d", ag); say(b); }
        else if (c.compare(0, 3, "AG0") == 0) ag = atoi(c.c_str() + 3);
        else if (c == "KS") { snprintf(b, sizeof b, "KS%03d", ks); say(b); }
        else if (c.compare(0, 2, "KS") == 0) ks = atoi(c.c_str() + 2);
        else if (c == "BI") say(bk ? "BI1" : "BI0");
        else if (c == "BI1") bk = true;
        else if (c == "NL0") { snprintf(b, sizeof b, "NL0%03d", nl); say(b); }
        else if (c.compare(0, 3, "NL0") == 0) nl = atoi(c.c_str() + 3);
        else if (c.compare(0, 3, "KM1") == 0) km = c.substr(3);
        else if (c == "KY01") keyed += km;
        else if (c == "KY00") keyed += "<STOP>";
        else if (c == "SV") vfo ^= 1;
        else say("?");
    }
    void kenwoodCmd(const std::string& c) {             // TS-2000 (no TQ, no MD6, TX; / RX;)
        char b[64];
        if (c == "ID") say("ID019");
        else if (c == "IF") {
            snprintf(b, sizeof b, "IF%011llu     +000000000%d%c%d00000 ", (unsigned long long)freq, tx ? 1 : 0, mode, vfo);
            say(b);
        }
        else if (c.compare(0, 2, "FA") == 0 && c.size() > 2) freq = strtoull(c.c_str() + 2, nullptr, 10);
        else if (c == "MD") say(std::string("MD") + mode);
        else if (c.compare(0, 2, "MD") == 0) mode = c[2];
        else if (c == "TX") tx = true;
        else if (c == "RX") tx = false;
        else if (c == "KS") { snprintf(b, sizeof b, "KS%03d", ks); say(b); }
        else if (c.compare(0, 2, "KS") == 0) ks = atoi(c.c_str() + 2);
        else say("?");
    }
    void qmxCmd(const std::string& c) {
        char b[64];
        if (c == "ID") say("ID020");
        else if (c == "IF") {
            snprintf(b, sizeof b, "IF%011llu     +000000000%d%c%d00000 ", (unsigned long long)freq, tx ? 1 : 0, mode, vfo);
            say(b);
        }
        else if (c.compare(0, 2, "FA") == 0 && c.size() > 2) freq = strtoull(c.c_str() + 2, nullptr, 10);
        else if (c == "MD") say(std::string("MD") + mode);
        else if (c.compare(0, 2, "MD") == 0) mode = c[2];
        else if (c == "TQ1") tx = true;
        else if (c == "TQ0") tx = false;
        else if (c == "SM") say("SM054");
        else if (c == "KS") { snprintf(b, sizeof b, "KS%d", ks); say(b); }
        else if (c.compare(0, 2, "KS") == 0) ks = atoi(c.c_str() + 2);
        else if (c == "PC") say("PC45");
        else if (c == "SW") say("SW121");
        else if (c.compare(0, 3, "KY ") == 0) { keyed += c.substr(3); say("KY0"); }
        else if (c == "Q90") {}
        else if (c == "FR") say(vfo ? "FR1" : "FR0");
        else if (c == "FR0" || c == "FR1") vfo = c[2] - '0';
        else if (c == "FT0" || c == "FT1") {}
        else say("?");
    }
};

static void run(Radio& r, int ms) { for (int i = 0; i < ms; i += 10) { simMs += 10; r.poll(); } }
static bool sent(const FakeText& f, const std::string& c) { for (auto& l : f.log) if (l == c) return true; return false; }
static void cleanDir(const char* d) {                  // remove every file in a test directory
    if (DIR* dp = opendir(d)) {
        std::vector<std::string> names;
        while (struct dirent* e = readdir(dp)) if (e->d_name[0] != '.') names.push_back(e->d_name);
        closedir(dp);
        for (auto& n : names) remove((std::string(d) + "/" + n).c_str());
    }
}
static std::string slurpP(const std::string& p) { std::string s; FILE* f = fopen(p.c_str(), "r"); char b[1024]; while (f && fgets(b, sizeof b, f)) s += b; if (f) fclose(f); return s; }
static int nrec(const std::string& s) { int n = 0; for (size_t p = 0; (p = s.find("<EOR>", p)) != std::string::npos; p++) n++; return n; }
static bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }
static std::string trimRight(std::string s) { while (!s.empty() && s.back() == ' ') s.pop_back(); return s; }

int main() {
    clk::init(mono);
    simMs = 1000000;

    {   // ---------------- Yaesu FTX-1
        Radio r; FakeText f; f.r = &r; f.yaesu = true;
        r.write = [&f](const uint8_t* d, size_t n) { f.rx(d, n); return true; };
        r.setProto(RP_YAESU);
        CHECK(!r.connected());
        run(r, 3000);
        CHECK(r.connected() && r.freq == 14074000 && r.mode == 1 && r.dataMode == 0);
        CHECK(r.smeter == 120 && !strcmp(r.modelName(), "FTX-1"));
        r.setFreq(7074000); r.setUsbData(); run(r, 1500);
        CHECK(f.freq == 7074000 && sent(f, "FA007074000") && f.mode == 'C');
        CHECK(r.freq == 7074000 && r.mode == 1 && r.dataMode == 1);
        r.ptt(true); run(r, 1500);
        CHECK(f.tx && r.pttState && r.po == 150 && r.swr == 10 && r.alc == 40);
        r.ptt(false); run(r, 1500);
        CHECK(!f.tx && !r.pttState);
        r.setRfPower(50); run(r, 3000);                 // field head: 50 % of 10 W
        CHECK(f.watts == 5 && r.level[0x0A] == 50);
        f.head = 2; f.watts = 100; run(r, 3000);        // on the SPA-1 amplifier
        r.setRfPower(20); run(r, 3000);
        CHECK(sent(f, "PC2020") && f.watts == 20 && !strcmp(r.modelName(), "FTX-1 optima"));
        r.setMode(0x03, 1); run(r, 1000);
        CHECK(f.mode == '3' && r.mode == 3 && r.dataMode == 0);
        r.setMode(0x00, 1); run(r, 600); r.setData(true); run(r, 1000);
        CHECK(f.mode == '8' && r.mode == 0 && r.dataMode == 1);
        r.setLevel(0x01, 50); run(r, 1000);
        CHECK(f.ag == 128 && r.level[0x01] == 50);
        r.setFunc(0x22, 1); run(r, 1000);
        CHECK(f.nl == 5 && r.func[0x22] == 1);
        r.setKeySpeed(28); run(r, 1000);
        CHECK(f.ks == 28 && r.keySpeed == 28);
        r.sendCw("cq de w9xyz k"); run(r, 1500);
        CHECK(f.bk && f.keyed == "CQ DE W9XYZ K}");     // break-in was off: turned on first
        r.stopCw(); run(r, 300);
        CHECK(f.keyed.find("<STOP>") != std::string::npos);
        int ng0 = r.ng; r.setMode(0x55, 1); CHECK(r.ng == ng0 + 1);
    }
    {   // ---------------- QMX
        Radio r; FakeText f; f.r = &r; f.yaesu = false; f.mode = '3'; f.freq = 7030000;
        r.write = [&f](const uint8_t* d, size_t n) { f.rx(d, n); return true; };
        r.setProto(RP_QMX);
        run(r, 3000);
        CHECK(r.connected() && r.freq == 7030000 && r.mode == 3 && r.keySpeed == 20);
        CHECK(r.smeter == 120 && !strcmp(r.modelName(), "QMX"));
        r.setFreq(14074000); r.setUsbData(); run(r, 2000);
        CHECK(f.freq == 14074000 && f.mode == '6' && r.mode == 1 && r.dataMode == 1 && sent(f, "Q90"));
        r.ptt(true); run(r, 2000);
        CHECK(f.tx && r.pttState && r.po > 180 && r.swr > 15 && r.swr < 25);
        r.ptt(false); run(r, 1000);
        CHECK(!f.tx && !r.pttState);
        r.setMode(0x03, 1); run(r, 1000);
        r.sendCw("AC9HP DE W9XYZ UR 5NN 5NN IN EM69 BK"); run(r, 2000);
        CHECK(trimRight(f.keyed) == "AC9HP DE W9XYZ UR 5NN 5NN IN EM69 BK");
        CHECK(sent(f, "KY AC9HP DE W9XYZ UR 5NN 5N"));  // 24 characters per command
        r.vfo(0xB0); run(r, 1500);
        CHECK(f.vfo == 1);
    }
    {   // ---------------- Kenwood TS-2000
        Radio r; FakeText f; f.r = &r; f.kenwood = true; f.mode = '2'; f.freq = 7074000;
        r.write = [&f](const uint8_t* d, size_t n) { f.rx(d, n); return true; };
        r.setProto(RP_KENWOOD);
        r.requestId();
        run(r, 3000);
        CHECK(r.connected() && r.freq == 7074000 && r.mode == 1 && !strcmp(r.modelName(), "TS-2000"));
        r.setFreq(14074000); r.setUsbData(); run(r, 1500);
        CHECK(f.freq == 14074000 && f.mode == '2' && sent(f, "FA14074000") && !sent(f, "MD6") && !sent(f, "Q90"));
        r.ptt(true); run(r, 1000);
        CHECK(f.tx && r.pttState && sent(f, "TX") && !sent(f, "TQ1"));
        r.ptt(false); run(r, 1000);
        CHECK(!f.tx && !r.pttState && sent(f, "RX"));
        r.setMode(0x05, 1); run(r, 1000);
        CHECK(f.mode == '4' && r.mode == 5);
        r.setKeySpeed(25); run(r, 1000);
        CHECK(f.ks == 25 && sent(f, "KS025"));
        // PTT on the serial port's RTS line (Digirig) with CAT for the frequency
        std::vector<bool> line;
        r.pttLine = [&line](bool on) { line.push_back(on); };
        r.pttMethod = PTT_RTS;
        r.ptt(true); run(r, 600);
        CHECK(line.size() == 1 && line[0] && !f.tx && r.pttState);
        r.ptt(false); run(r, 600);
        CHECK(line.size() == 2 && !line[1] && !r.pttState);
        // a Kenwood must not pass for a Yaesu
        r.pttMethod = PTT_CAT;
        r.setProto(RP_YAESU); run(r, 4000);
        CHECK(!r.connected());
    }
    {   // ---------------- Yaesu FTDX10: FTX-1 commands, PC in plain watts
        Radio r; FakeText f; f.r = &r; f.yid = "ID0761"; f.watts = 50;
        r.write = [&f](const uint8_t* d, size_t n) { f.rx(d, n); return true; };
        r.setProto(RP_YAESU);
        r.requestId();
        run(r, 3000);
        CHECK(r.connected() && !strcmp(r.modelName(), "FTDX10") && r.level[0x0A] == 50);
        r.setRfPower(25); run(r, 2000);
        CHECK(f.watts == 25 && sent(f, "PC025") && r.level[0x0A] == 25);
    }
    {   // ---------------- no CAT (SignaLink): the band buttons set the dial, VOX keys the radio
        Radio r;
        int writes = 0;
        r.write = [&writes](const uint8_t*, size_t) { writes++; return true; };
        r.setProto(RP_NONE);
        CHECK(r.connected() && !strcmp(r.modelName(), "No CAT"));
        r.setFreq(10136000); r.setUsbData(); r.setRfPower(50); r.ptt(true); run(r, 2000);
        CHECK(r.freq == 10136000 && r.pttState && writes == 0);
        r.ptt(false);
        CHECK(!r.pttState);
    }
    {   // ---------------- no CAT through the app: the band button's dial is remembered
        const char* dir = "test/out/nocat";
        mkdir(dir, 0755);
        cleanDir(dir);
        {
            App app;
            app.begin(dir);
            app.command("set", {{"radioType", "5"}});
            CHECK(app.radio.pttMethod == PTT_VOX);              // no CAT and PTT "CAT": the sound card's VOX
            app.radio.setProto(RP_NONE);
            app.usbSerial = true;
            app.command("band", {{"hz", "7074000"}});
            for (int i = 0; i < 20; i++) { simMs += 10; app.tick(); }
            CHECK(app.radio.freq == 7074000 && app.cfg.dialHz == 7074000);
            CHECK(app.radio.mode == 1 && app.radio.dataMode == 1);   // the band button: USB-D
            app.command("qsy", {{"hz", "7030000"}});
            app.command("rmode", {{"m", "03"}, {"fil", "1"}});
            for (int i = 0; i < 5; i++) { simMs += 10; app.tick(); }
            CHECK(app.radio.freq == 7030000 && app.cfg.dialHz == 7030000 && app.cfg.dialMode == 3 && app.cfg.dialData == 0);
            app.command("set", {{"pttMethod", "1"}});
            CHECK(app.radio.pttMethod == PTT_RTS);
        }
        App app2;
        app2.begin(dir);
        CHECK(app2.cfg.dialHz == 7030000 && app2.cfg.dialMode == 3 && app2.cfg.radioType == 5 && app2.cfg.pttMethod == 1);
    }
    {   // ---------------- a CI-V port echoes: it must not pass for a text radio
        Radio r;
        r.write = [&r](const uint8_t* d, size_t n) { r.onRx(d, n); return true; };
        r.setProto(RP_YAESU);
        run(r, 4000);
        CHECK(!r.connected());
        r.setProto(RP_QMX);
        run(r, 4000);
        CHECK(!r.connected());
    }
    {   // ---------------- a Yaesu must not pass for a QMX and the other way round
        Radio r; FakeText f; f.r = &r; f.yaesu = true;
        r.write = [&f](const uint8_t* d, size_t n) { f.rx(d, n); return true; };
        r.setProto(RP_QMX);
        run(r, 4000);
        CHECK(!r.connected());
        f.yaesu = false; r.setProto(RP_YAESU);
        run(r, 4000);
        CHECK(!r.connected());
    }
    {   // ---------------- logger: macros and manual entries through the app
        const char* dir = "test/out/cat";
        mkdir(dir, 0755);
        cleanDir(dir);
        App app; FakeText f; f.r = &app.radio; f.yaesu = false; f.mode = '2'; f.freq = 14250000;
        app.radio.write = [&f](const uint8_t* d, size_t n) { f.rx(d, n); return true; };
        app.begin(dir);
        app.radio.setProto(RP_QMX);
        app.usbSerial = true;
        auto okr = [](const std::string& s) { return s.find("\"ok\":true") != std::string::npos; };
        CHECK(okr(app.command("set", {{"call", "AC9HP"}, {"grid", "EM69"}, {"mySigInfo", "US-1234"}, {"keyWpm", "25"}})));
        clk::setUtcMs(1794328740000LL, "test");
        for (int i = 0; i < 300; i++) { simMs += 10; app.tick(); }
        CHECK(app.radio.connected() && f.ks == 25);       // keyer speed applied when the radio appeared
        // SSB contact
        CHECK(!okr(app.command("logqso", {{"call", "??"}, {"mode", "SSB"}})));
        CHECK(okr(app.command("logqso", {{"call", "w9xyz"}, {"mode", "AUTO"}, {"name", "Bob"}, {"comment", "P2P US-0001"}})));
        // CW: macros refuse outside CW mode, then work
        CHECK(!okr(app.command("macro", {{"n", "1"}})));
        app.radio.setMode(0x03, 1);
        for (int i = 0; i < 200; i++) { simMs += 10; app.tick(); }
        CHECK(okr(app.command("macro", {{"n", "1"}})));
        CHECK(!okr(app.command("macro", {{"n", "3"}})));  // needs a callsign
        CHECK(okr(app.command("macro", {{"n", "3"}, {"call", "k1abc"}, {"rs", "599"}})));
        CHECK(okr(app.command("cwsend", {{"text", "tu"}})));
        CHECK(okr(app.command("cwspeed", {{"wpm", "30"}})));
        for (int i = 0; i < 400; i++) { simMs += 10; app.tick(); }
        std::string k;
        for (size_t i = 0; i < f.keyed.size(); i += 24) k += trimRight(f.keyed.substr(i, 24)) + "|";
        CHECK(k == "CQ CQ DE AC9HP AC9HP K|K1ABC UR 5NN 5NN BK|TU|");
        CHECK(f.ks == 30);
        CHECK(okr(app.command("logqso", {{"call", "K1ABC"}, {"mode", "CW"}, {"rs", "579"}})));
        CHECK(okr(app.command("set", {{"macro2", "Park|{CALL} {REF} {GRID}"}})) && !okr(app.command("set", {{"macro2", "no label"}})));
        CHECK(app.expandMacro(app.cfg.macro[1], "N0P", "599") == "N0P US-1234 EM69");
        FILE* fp = fopen(app.logPath().c_str(), "r");
        std::string all; char buf[512];
        while (fp && fgets(buf, sizeof buf, fp)) all += buf;
        if (fp) fclose(fp);
        size_t second = all.find("K1ABC");
        CHECK(adifField(all, "CALL") == "W9XYZ" && adifField(all, "MODE") == "SSB" && adifField(all, "SUBMODE") == "USB");
        CHECK(adifField(all, "RST_SENT") == "59" && adifField(all, "NAME") == "Bob" && adifField(all, "FREQ") == "14.250000");
        CHECK(adifField(all, "MY_SIG_INFO") == "US-1234");
        CHECK(second != std::string::npos);
        if (second != std::string::npos) {
            std::string rec = all.substr(all.rfind('\n', second) + 1);
            CHECK(adifField(rec, "MODE") == "CW" && adifField(rec, "RST_SENT") == "579" && adifField(rec, "RST_RCVD") == "599");
        }
        // typed frequency and chosen mode win over the radio
        CHECK(parseDialFreq("14.250") == 14250000 && parseDialFreq("7074") == 7074000 && parseDialFreq("3.985.500") == 3985500);
        CHECK(parseDialFreq("14250000") == 14250000 && parseDialFreq("1.8") == 1800000 && parseDialFreq("abc") == 0 && parseDialFreq("") == 0);
        CHECK(okr(app.command("logqso", {{"call", "N0SSB"}, {"mode", "SSB"}, {"freq", "3.985"}})));
        CHECK(okr(app.command("logqso", {{"call", "N0USB"}, {"mode", "SSB"}, {"freq", "5.357"}})));
        CHECK(!okr(app.command("logqso", {{"call", "N0BAD"}, {"mode", "SSB"}, {"freq", "fourteen"}})));
        {
            FILE* fq = fopen(app.logPath().c_str(), "r");
            std::string l2; char lb[512];
            while (fq && fgets(lb, sizeof lb, fq)) l2 += lb;
            if (fq) fclose(fq);
            std::string r1 = l2.substr(l2.find("N0SSB")), r2 = l2.substr(l2.find("N0USB"));
            CHECK(adifField(r1, "FREQ") == "3.985000" && adifField(r1, "BAND") == "80m" && adifField(r1, "MODE") == "SSB" && adifField(r1, "SUBMODE") == "LSB");
            CHECK(adifField(r1, "RST_SENT") == "59");
            CHECK(adifField(r2, "SUBMODE") == "USB" && adifField(r2, "BAND") == "60m");
        }
        // FT8 contact cut short: Log QSO logs what we have and goes back to CQ
        CHECK(!okr(app.command("logdx", {})));                          // nothing in progress
        CHECK(okr(app.command("dx", {{"call", "K4FADE"}, {"grid", "EM73"}})));
        CHECK(!okr(app.command("logdx", {})));                          // no report either way
        app.seq.rptRcvd = -17;
        CHECK(okr(app.command("logdx", {})));
        CHECK(app.seq.txMsg == 6 && app.seq.txEnabled && app.seq.dxCall.empty());
        {
            FILE* fq = fopen(app.logPath().c_str(), "r");
            std::string l3; char lb[512];
            while (fq && fgets(lb, sizeof lb, fq)) l3 += lb;
            if (fq) fclose(fq);
            std::string r3 = l3.substr(l3.find("K4FADE"));
            CHECK(adifField(r3, "RST_RCVD") == "-17" && adifField(r3, "GRIDSQUARE") == "EM73" && adifField(r3, "COMMENT").empty());
            CHECK(adifField(r3, "QSO_DATE") != "19700101");
            CHECK(r3.substr(0, r3.find("<EOR>")).find("RST_SENT") == std::string::npos);
        }
        // log management: list, edit (both files), delete
        {
            auto slurpF = [&](const char* n) { std::string s; FILE* fq = fopen((!strcmp(n, "/log.adi") ? app.logPath() : app.archivePath()).c_str(), "r"); char lb[512];
                                               while (fq && fgets(lb, sizeof lb, fq)) s += lb; if (fq) fclose(fq); return s; };
            App::UiState u0; app.uiState(u0);
            std::string js = app.command("logread", {{"n", "2"}});
            CHECK(js.find("\"total\":" + std::to_string(u0.logN)) != std::string::npos && js.find("K4FADE") != std::string::npos);
            size_t ip = js.find("\"i\":");
            int idx = atoi(js.c_str() + ip + 4);                         // newest = K4FADE
            CHECK(!okr(app.command("logedit", {{"i", std::to_string(idx)}, {"call", "W1XYZ"}, {"f.RST_SENT", "-05"}})));   // guard
            CHECK(!okr(app.command("logedit", {{"i", std::to_string(idx)}, {"call", "K4FADE"}, {"f.CALL", "not a call"}})));
            CHECK(okr(app.command("logedit", {{"i", std::to_string(idx)}, {"call", "K4FADE"}, {"f.RST_SENT", "-05"},
                                               {"f.FREQ", "7.074"}, {"f.COMMENT", "faded"}, {"f.GRIDSQUARE", ""}})));
            std::string cur = slurpF("/log.adi"), all = slurpF("/archive.adi");
            std::string rc = cur.substr(cur.find("K4FADE")), ra = all.substr(all.find("K4FADE"));
            CHECK(adifField(rc, "RST_SENT") == "-05" && adifField(rc, "BAND") == "40m" && adifField(rc, "FREQ") == "7.074000" &&
                  adifField(rc, "COMMENT") == "faded" && adifField(rc, "GRIDSQUARE").empty());
            CHECK(rc.substr(0, rc.find('\n')) == ra.substr(0, ra.find('\n')));   // archive changed the same way
            CHECK(cur.find("<EOH>") != std::string::npos && cur.find("N0USB") != std::string::npos);
            App::UiState u1; app.uiState(u1);
            CHECK(u1.logN == u0.logN && u1.logAll == u0.logAll);
            CHECK(okr(app.command("logdel", {{"i", std::to_string(idx)}, {"call", "K4FADE"}})));
            cur = slurpF("/log.adi"); all = slurpF("/archive.adi");
            CHECK(cur.find("K4FADE") == std::string::npos && all.find("K4FADE") == std::string::npos);
            App::UiState u2; app.uiState(u2);
            CHECK(u2.logN == u0.logN - 1 && u2.logAll == u0.logAll - 1);
            CHECK(app.command("logread", {{"file", "all"}, {"q", "usb"}}).find("\"total\":1") != std::string::npos);
            // recently deleted: listed, restored to both files in time order
            std::string tr = app.command("logtrash", {});
            CHECK(tr.find("K4FADE") != std::string::npos && tr.find("\"APP_RIGDECK_FROM\":\"both\"") != std::string::npos);
            CHECK(okr(app.command("logrestore", {{"i", "0"}, {"call", "K4FADE"}})));
            cur = slurpF("/log.adi"); all = slurpF("/archive.adi");
            CHECK(cur.find("K4FADE") != std::string::npos && all.find("K4FADE") != std::string::npos);
            CHECK(cur.find("APP_RIGDECK") == std::string::npos);
            App::UiState u3; app.uiState(u3);
            CHECK(u3.logN == u0.logN && u3.logAll == u0.logAll);
            CHECK(app.command("logtrash", {}).find("K4FADE") == std::string::npos);
            CHECK(!okr(app.command("logrestore", {{"i", "0"}})));
            CHECK(!okr(app.command("logdel", {{"i", "999"}})));
        }
        {   // a logger address off RigDeck's networks is flagged
            App::StaStatus st; st.state = "connected"; st.ip = "192.168.68.94";
            app.setStaStatus(st);
            std::string r1 = app.command("set", {{"logProto", "2"}, {"logHost", "196.168.68.137"}, {"logPort", "2333"}});
            std::string r2 = app.command("set", {{"logHost", "192.168.68.137"}});
            CHECK(okr(r1) && r1.find("\"warn\"") != std::string::npos && okr(r2) && r2.find("\"warn\"") == std::string::npos);
            app.command("set", {{"logProto", "0"}});
        }
        Settings s2; s2.load(std::string(dir) + "/settings.txt");
        CHECK(s2.macro[1] == "Park|{CALL} {REF} {GRID}" && s2.keyWpm == 25);
    }
    {   // ---------------- log storage: per-station files, older files moved in, the card, big logs
        std::string in = "test/out/stor", sd = "test/out/stor/sd";
        mkdir(in.c_str(), 0755);
        cleanDir(sd.c_str()); rmdir(sd.c_str()); cleanDir(in.c_str());
        auto okr = [](const std::string& s) { return s.find("\"ok\":true") != std::string::npos; };
        auto put = [](const std::string& p, const std::string& body) { FILE* f = fopen(p.c_str(), "w"); fputs(body.c_str(), f); fclose(f); };
        std::string rec1 = "<CALL:5>K1OLD <QSO_DATE:8>20260101 <TIME_ON:4>1200 <BAND:3>20m <MODE:3>FT8 <EOR>\n";
        put(in + "/settings.txt", "call=AC9HP\ngrid=EM69\n");
        put(in + "/log.adi", "<ADIF_VER:5>3.1.4 <EOH>\n" + rec1);
        put(in + "/archive.adi", "<EOH>\n" + rec1);
        put(in + "/sent.txt", "K1OLD|20260101|1200|20m|FT8|\n");
        clk::setUtcMs(1794328740000LL, "test");
        {
            App app;
            app.begin(in);
            CHECK(app.logPath() == in + "/AC9HP_log.adi" && app.archivePath() == in + "/AC9HP_archive.adi");
            CHECK(!exists(in + "/log.adi") && !exists(in + "/sent.txt") && nrec(slurpP(app.logPath())) == 1 && nrec(slurpP(app.archivePath())) == 1);
            app.radio.freq = 14074000;
            CHECK(app.seq.workedBefore("K1OLD"));
            CHECK(app.stateJson(0, 0, false).find("\"unsent\":0") != std::string::npos);   // sent.txt came along
            // another callsign has its own log; switching back finds the first one again
            CHECK(okr(app.command("set", {{"call", "W1AW/P"}})));
            CHECK(app.logPath() == in + "/W1AW-P_log.adi" && !app.seq.workedBefore("K1OLD"));
            CHECK(okr(app.command("logqso", {{"call", "N0NEW"}, {"mode", "SSB"}, {"freq", "14.250"}})));
            CHECK(okr(app.command("set", {{"call", "AC9HP"}})));
            CHECK(app.seq.workedBefore("K1OLD") && !app.seq.workedBefore("N0NEW"));
            CHECK(app.stateJson(0, 0, false).find("\"station\":\"AC9HP\"") != std::string::npos);
            // a contact only in the current log (not in the archive) is copied there before clearing
            {
                FILE* fa = fopen(app.logPath().c_str(), "a");
                fputs("<CALL:5>K1ONE <QSO_DATE:8>20260102 <TIME_ON:4>1300 <BAND:3>20m <MODE:3>FT8 <EOR>\n", fa);
                fclose(fa);
                app.command("set", {{"call", "W1AW"}}); app.command("set", {{"call", "AC9HP"}});   // reload the files
            }
            CHECK(app.stateJson(0, 0, false).find("\"behind\":true") != std::string::npos);
            CHECK(okr(app.command("clearlog", {{"confirm", "yes"}})) && slurpP(app.archivePath()).find("K1ONE") != std::string::npos);
            CHECK(app.stateJson(0, 0, false).find("\"behind\":false") != std::string::npos);
            // internal store: a cleared log is gone (the archive keeps the contacts)
            CHECK(okr(app.command("clearlog", {{"confirm", "yes"}})) && !exists(app.logPath()));
        }
        {   // with a card: what was logged internally moves onto it
            App app;
            app.sdInfo = "test card";
            uint64_t fakeT = 4000000000ULL;
            app.fsInfo = [](bool sdc, uint64_t& t, uint64_t& u) { t = sdc ? 4000000000ULL : 2000000; u = sdc ? 1000 : 1990000; return true; };
            (void)fakeT;
            app.begin(in, sd);
            CHECK(app.logPath() == sd + "/AC9HP_log.adi" && !exists(in + "/AC9HP_archive.adi") && nrec(slurpP(app.archivePath())) == 2);
            CHECK(exists(in + "/W1AW-P_archive.adi"));                    // other stations stay where they are until used
            std::string js = app.stateJson(0, 0, false);
            CHECK(js.find("\"stor\":{\"sd\":true") != std::string::npos && js.find("\"sdT\":4000000000") != std::string::npos);
            // a big log: 3000 contacts, streamed rather than read whole
            char call[12];
            for (int i = 0; i < 3000; i++) {
                snprintf(call, sizeof call, "W%d%c%c%c", i % 10, 'A' + (i / 10) % 26, 'A' + (i / 260) % 26, 'A' + i % 7);
                app.command("logqso", {{"call", call}, {"mode", "SSB"}, {"freq", "14.250"}, {"rs", std::to_string(i)}});
            }
            App::UiState u; app.uiState(u);
            CHECK(u.logN == 3000 && u.logAll == 3002 && u.logSd && u.station == "AC9HP");
            std::string pg = app.command("logread", {{"n", "3"}, {"from", "0"}});
            CHECK(pg.find("\"total\":3000") != std::string::npos && pg.find("{\"i\":2999,") != std::string::npos && pg.find("\"RST_SENT\":\"2999\"") != std::string::npos);
            pg = app.command("logread", {{"file", "all"}, {"n", "2"}, {"from", "3000"}});   // the two oldest: K1ONE, K1OLD
            CHECK(pg.find("{\"i\":0,") != std::string::npos && pg.find("K1OLD") != std::string::npos);
            std::vector<App::UiLogEntry> tail;
            app.uiLogTail(2, tail);
            CHECK(tail.size() == 2 && tail[0].idx == 2999 && tail[1].idx == 2998 && tail[0].sent == "2999");
            // delete on the card: kept in AC9HP_deleted.adi for good; restore still works
            CHECK(okr(app.command("logdel", {{"i", "1500"}})));
            CHECK(nrec(slurpP(sd + "/AC9HP_deleted.adi")) == 1 && app.command("logtrash", {}).find("\"keep\":true") != std::string::npos);
            app.uiState(u);
            CHECK(u.logN == 2999 && u.logAll == 3001);
            CHECK(okr(app.command("logrestore", {{"i", "0"}})));
            app.uiState(u);
            CHECK(u.logN == 3000 && u.logAll == 3002 && slurpP(app.logPath()).find("<RST_SENT:4>1500") != std::string::npos);
            // send unsent: the whole archive goes out in batches (K1OLD was sent before, K1ONE was not)
            CHECK(okr(app.command("set", {{"logProto", "2"}, {"logHost", "127.0.0.1"}, {"logPort", "9"}})));
            std::string r = app.command("logsync", {{"file", "all"}});
            CHECK(r.find("\"queued\":3001") != std::string::npos);
            int sentN = 0;
            while (app.netOnce()) sentN++;
            CHECK(sentN == 3001);
            CHECK(app.command("logsync", {{"file", "all"}}).find("\"queued\":0") != std::string::npos);
            app.command("set", {{"logProto", "0"}});
            // a cleared log on the card is renamed and kept
            CHECK(okr(app.command("clearlog", {{"confirm", "yes"}})) && !exists(app.logPath()));
            std::string kept;
            if (DIR* dp = opendir(sd.c_str())) { while (struct dirent* e = readdir(dp)) if (!strncmp(e->d_name, "AC9HP_deleted_", 14)) kept = e->d_name; closedir(dp); }
            CHECK(kept.size() == 33 && !kept.compare(0, 18, "AC9HP_deleted_2026") && !kept.compare(29, 4, ".adi") && nrec(slurpP(sd + "/" + kept)) == 3000);
            // NUKE: every log file of every station, on the card and internally
            r = app.command("eraselogs", {{"confirm", "yes"}});
            CHECK(okr(r) && !exists(sd + "/" + kept) && !exists(app.archivePath()) && !exists(in + "/W1AW-P_archive.adi"));
            CHECK(exists(in + "/settings.txt"));
            app.uiState(u);
            CHECK(u.logN == 0 && u.logAll == 0 && !app.seq.workedBefore("K1OLD"));
        }
        {   // formatting the card keeps this station's log (parked in the board's memory meanwhile)
            App app;
            app.fsInfo = [](bool sdc, uint64_t& t, uint64_t& u) { t = sdc ? 4000000000ULL : 2000000; u = 100000; return true; };
            app.sdFormat = [](std::string& msg) { cleanDir("test/out/stor/sd"); msg = "TEST, card 4.0 GB, files 4.0 GB"; return 0; };
            app.begin(in, sd);
            CHECK(okr(app.command("logqso", {{"call", "K9FMT"}, {"mode", "SSB"}, {"freq", "14.250"}})));
            put(sd + "/OTHER_log.adi", "<EOH>\n");
            std::string r = app.command("sdformat", {{"confirm", "yes"}});
            CHECK(okr(r) && !exists(sd + "/OTHER_log.adi") && !exists(in + "/AC9HP_log.adi"));
            CHECK(slurpP(app.logPath()).find("K9FMT") != std::string::npos && slurpP(app.archivePath()).find("K9FMT") != std::string::npos);
            CHECK(app.stateJson(0, 0, false).find("TEST, card 4.0 GB") != std::string::npos && !app.rebootReq);
            app.sdFormat = [](std::string& msg) { msg = "Formatting the card failed (ESP_FAIL)"; return -1; };
            CHECK(!okr(app.command("sdformat", {{"confirm", "yes"}})));
            CHECK(app.logPath() == in + "/AC9HP_log.adi" && slurpP(app.logPath()).find("K9FMT") != std::string::npos);
            app.command("eraselogs", {{"confirm", "yes"}});
        }
        {   // almost full internal store: a warning with a rough count of contacts left
            App app;
            app.fsInfo = [](bool, uint64_t& t, uint64_t& u) { t = 2000000; u = 1880000; return true; };
            app.begin(in);
            std::string js = app.stateJson(0, 0, false);
            CHECK(js.find("almost full") != std::string::npos && js.find("\"left\":30") != std::string::npos);
        }
    }
    {   // ---------------- GPS module: PPS pins the second, NMEA names it
        const char* dir = "test/out/gps";
        mkdir(dir, 0755);
        remove("test/out/gps/settings.txt");
        App app;
        app.begin(dir);
        app.command("set", {{"gpsModule", "1"}, {"gpsPpsPin", "4"}, {"gpsDelayMs", "150"}});
        auto rmc = [](int hh, int mm, int ss) {
            char body[96], s[110];
            snprintf(body, sizeof body, "GPRMC,%02d%02d%02d.00,A,4000.000,N,08600.000,W,0.0,0.0,191126,,,A", hh, mm, ss);
            unsigned ck = 0;
            for (const char* p = body; *p; p++) ck ^= (unsigned char)*p;
            snprintf(s, sizeof s, "$%s*%02X\r\n", body, ck);
            return std::string(s);
        };
        const int64_t t0 = 1795046400000LL;            // 2026-11-19 00:00:00 UTC
        auto utcOf = [&](int hh, int mm, int ss) { return t0 + ((hh * 60 + mm) * 60 + ss) * 1000LL; };
        // PPS at mono M, sentence starts 380 ms later (this module is slow to talk)
        simMs = 5000000;
        app.ppsMono32 = (uint32_t)simMs; app.ppsCount++;
        simMs += 380;
        std::string s = rmc(18, 30, 5);
        app.gpsModuleRx((const uint8_t*)s.data(), s.size());
        CHECK(clk::valid() && !strcmp(clk::source(), "gps-pps"));
        CHECK(llabs(clk::utcMs() - (utcOf(18, 30, 5) + 380)) <= 1);   // 380 ms after the second, not 150
        // a second sentence without a new pulse is not trusted for PPS timing
        simMs += 1000;
        s = rmc(18, 30, 6);
        app.gpsModuleRx((const uint8_t*)s.data(), s.size());
        CHECK(llabs(clk::utcMs() - (utcOf(18, 30, 6) + 380)) <= 1);
        // no PPS wired: the sentence delay setting is used
        app.command("set", {{"gpsPpsPin", "-1"}});
        simMs += 1000;
        s = rmc(18, 30, 7);
        app.gpsModuleRx((const uint8_t*)s.data(), s.size());
        CHECK(!strcmp(clk::source(), "gps-nmea"));
        CHECK(llabs(clk::utcMs() - (utcOf(18, 30, 7) + 150)) <= 1);
        // GGA (time of day only, the IC-705's default) is accepted once the date is known, nearest date wins
        {
            NmeaTime nt;
            auto gga = [](const char* hms, char fix) {
                char body[96], s2[110];
                snprintf(body, sizeof body, "GPGGA,%s,4000.000,N,08600.000,W,%c,08,1.0,250.0,M,-33.0,M,,", hms, fix);
                unsigned ck = 0;
                for (const char* p = body; *p; p++) ck ^= (unsigned char)*p;
                snprintf(s2, sizeof s2, "$%s*%02X\r\n", body, ck);
                return std::string(s2);
            };
            std::string g = gga("183005.00", '1');
            CHECK(!nt.feed((const uint8_t*)g.data(), g.size(), 0));          // no date known yet
            nt.setDateHint(utcOf(18, 29, 0));
            CHECK(nt.feed((const uint8_t*)g.data(), g.size(), 0) && nt.utcMs() == utcOf(18, 30, 5));
            g = gga("000002.00", '1');                                       // just after midnight, hint just before
            nt.setDateHint(t0 + 86400000LL - 3000);
            CHECK(nt.feed((const uint8_t*)g.data(), g.size(), 0) && nt.utcMs() == t0 + 86400000LL + 2000);
            g = gga("183005.00", '0');                                       // no fix
            nt.setDateHint(utcOf(18, 29, 0));
            CHECK(!nt.feed((const uint8_t*)g.data(), g.size(), 0));
        }
        // the clock sink sees every change (the ESP32 copies it into its own clock)
        static int64_t sunk = 0;
        clk::setSink([](int64_t u) { sunk = u; });
        clk::setUtcMs(utcOf(1, 2, 3), "test");
        CHECK(sunk == utcOf(1, 2, 3));
        clk::trim(40);
        CHECK(llabs(sunk - (utcOf(1, 2, 3) + 40)) <= 1);
        clk::setSink(nullptr);
    }
    printf("t_cat: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
