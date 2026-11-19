// Yaesu FTX-1 and QRP Labs QMX control, CW macros, and manual log entries, against simulated radios.
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
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
    bool yaesu = true;
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
            if (yaesu) yaesuCmd(c); else qmxCmd(c);
        }
    }
    void yaesuCmd(const std::string& c) {
        char b[48];
        if (c == "ID") say("ID0800");
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
        for (const char* n : {"/log.adi", "/archive.adi", "/settings.txt"}) remove((std::string(dir) + n).c_str());
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
        FILE* fp = fopen((std::string(dir) + "/log.adi").c_str(), "r");
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
            FILE* fq = fopen((std::string(dir) + "/log.adi").c_str(), "r");
            std::string l2; char lb[512];
            while (fq && fgets(lb, sizeof lb, fq)) l2 += lb;
            if (fq) fclose(fq);
            std::string r1 = l2.substr(l2.find("N0SSB")), r2 = l2.substr(l2.find("N0USB"));
            CHECK(adifField(r1, "FREQ") == "3.985000" && adifField(r1, "BAND") == "80m" && adifField(r1, "MODE") == "SSB" && adifField(r1, "SUBMODE") == "LSB");
            CHECK(adifField(r1, "RST_SENT") == "59");
            CHECK(adifField(r2, "SUBMODE") == "USB" && adifField(r2, "BAND") == "60m");
        }
        Settings s2; s2.load(std::string(dir) + "/settings.txt");
        CHECK(s2.macro[1] == "Park|{CALL} {REF} {GRID}" && s2.keyWpm == 25);
    }
    printf("t_cat: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
