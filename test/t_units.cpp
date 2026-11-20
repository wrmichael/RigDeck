// Unit tests: CI-V framing, ADIF, WSJT-X datagram, settings, message parsing, sequencer logic.
#include "civ.h"
#include "adif.h"
#include "settings.h"
#include "qso.h"
#include "clock.h"
#include "nmea.h"
#include "app.h"
#include <cstdio>
#include <cstring>

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static int64_t fakeMono = 1000;
static int64_t mono() { return fakeMono; }

int main() {
    clk::init(mono);
    // --- CI-V ---
    uint8_t b[5];
    civFreqToBcd(14074000, b);
    CHECK(b[0] == 0x00 && b[1] == 0x40 && b[2] == 0x07 && b[3] == 0x14 && b[4] == 0x00);
    CHECK(civBcdToFreq(b, 5) == 14074000);
    civFreqToBcd(432174550, b);
    CHECK(civBcdToFreq(b, 5) == 432174550);
    uint8_t lv[2] = {0x01, 0x20};
    CHECK(civBcdToInt(lv, 2) == 120);
    auto f = civBuild(0x88, 0xE0, 0x1C, {0x00, 0x01});
    const uint8_t pttOn[] = {0xFE, 0xFE, 0x88, 0xE0, 0x1C, 0x00, 0x01, 0xFD};
    CHECK(f.size() == sizeof pttOn && !memcmp(f.data(), pttOn, sizeof pttOn));

    Radio r;
    std::vector<std::vector<uint8_t>> sent;
    r.write = [&](const uint8_t* d, size_t n) { sent.emplace_back(d, d + n); return true; };
    // echo of our own command + garbage + a frequency reply split across two reads + a transceive broadcast
    const uint8_t rx1[] = {0xFE, 0xFE, 0x88, 0xE0, 0x03, 0xFD, 0x55, 0xFE, 0xFE, 0xE0, 0x88, 0x03, 0x00, 0x40};
    const uint8_t rx2[] = {0x07, 0x14, 0x00, 0xFD, 0xFE, 0xFE, 0x00, 0x88, 0x01, 0x01, 0x01, 0xFD,
                           0xFE, 0xFE, 0xE0, 0x88, 0x15, 0x02, 0x01, 0x20, 0xFD, 0xFE, 0xFE, 0xE0, 0x88, 0x1A, 0x06, 0x01, 0x01, 0xFD};
    CHECK(!r.connected());
    r.onRx(rx1, sizeof rx1);
    CHECK(r.freq == 0);
    r.onRx(rx2, sizeof rx2);
    CHECK(r.freq == 14074000 && r.mode == 1 && r.smeter == 120 && r.dataMode == 1 && r.connected());
    r.setFreq(7074000);
    r.ptt(true);                                   // must jump the queue
    fakeMono += 100; r.poll();
    CHECK(sent.size() == 1 && sent[0].size() == 8 && sent[0][4] == 0x1C && sent[0][6] == 0x01);
    fakeMono += 10; r.poll();
    CHECK(sent.size() == 1);                       // paced: nothing within 40 ms
    fakeMono += 50; r.poll();
    CHECK(sent.size() == 2 && sent[1][4] == 0x05 && sent[1][5] == 0x00 && sent[1][6] == 0x40 && sent[1][7] == 0x07 && sent[1][8] == 0x07);
    r.setRfPower(100);
    for (int i = 0; i < 5; i++) { fakeMono += 50; r.poll(); }
    bool pw = false;
    for (auto& s : sent) if (s[4] == 0x14 && s[5] == 0x0A) pw = (s[6] == 0x02 && s[7] == 0x55);
    CHECK(pw);

    // --- NMEA time ---
    {
        NmeaTime n;
        auto feed = [&](const char* s, int64_t t) { return n.feed((const uint8_t*)s, strlen(s), t); };
        auto withSum = [](std::string body) { unsigned char c = 0; for (char ch : body) c ^= (unsigned char)ch; char b[8]; snprintf(b, sizeof b, "*%02X\r\n", c); return "$" + body + b; };
        CHECK(NmeaTime::daysFromCivil(1970, 1, 1) == 0 && NmeaTime::daysFromCivil(2026, 11, 10) == 20767);
        std::string rmc = withSum("GPRMC,163900.00,A,3946.10,N,08609.50,W,0.0,0.0,101126,,,A");
        CHECK(!feed(rmc.substr(0, 20).c_str(), 5000));                      // split across two reads
        CHECK(feed(rmc.substr(20).c_str(), 5030));
        CHECK(n.utcMs() == 1794328740000LL && n.sentenceStartMono() == 5000);
        CHECK(feed(withSum("GNZDA,163901.50,10,11,2026,00,00").c_str(), 6000) && n.utcMs() == 1794328741500LL);
        CHECK(!feed(withSum("GPRMC,163902.00,V,,,,,,,101126,,,N").c_str(), 7000));  // no fix
        CHECK(!feed("$GPRMC,163900.00,A,3946.10,N,08609.50,W,0.0,0.0,101126,,,A*00\r\n", 8000));  // bad checksum
        CHECK(!feed(withSum("GPGGA,163903.00,3946.10,N,08609.50,W,1,08,1.0,250.0,M,,M,,").c_str(), 9000));  // no date in GGA

        // App: GPS sets the clock, allows for the sentence delay, and then outranks the browser.
        App a;
        a.begin("test/out");
        a.cfg.gpsClock = 1; a.cfg.gpsDelayMs = 150;
        fakeMono = 100000;
        a.command("time", {{"ms", "1794328000000"}});
        CHECK(std::string(clk::source()) == "browser");
        a.gpsRx((const uint8_t*)rmc.data(), rmc.size());
        CHECK(std::string(clk::source()) == "gps" && clk::utcMs() == 1794328740000LL + 150);
        fakeMono += 1000;
        a.command("time", {{"ms", "1794328000000"}});                       // browser may not override GPS
        CHECK(std::string(clk::source()) == "gps" && clk::utcMs() == 1794328741150LL);
        for (int i = 1; i <= 10; i++) {                                     // receiver consistently 60 ms later: clock follows gently
            char body[80];
            snprintf(body, sizeof body, "GPRMC,1639%02d.00,A,3946.10,N,08609.50,W,0.0,0.0,101126,,,A", i + 1);
            std::string s2 = withSum(body);
            fakeMono = 101000 + i * 1000 - 60;
            a.gpsRx((const uint8_t*)s2.data(), s2.size());
        }
        fakeMono = 111000;
        long long off = clk::utcMs() - (1794328740000LL + 150 + 11000);
        CHECK(off > 20 && off < 75);                                      // within the 25 ms dead band of the true 60
        a.cfg.gpsClock = 0;
        fakeMono += 120000;
        a.command("time", {{"ms", "1794329000000"}});                       // GPS off and stale: browser time accepted again
        CHECK(std::string(clk::source()) == "browser");
    }

    // --- ADIF ---
    CHECK(!strcmp(bandName(14074000), "20m") && !strcmp(bandName(50313000), "6m") && !strcmp(bandName(144174000), "2m") && !strcmp(bandName(432174000), "70cm") && !strcmp(bandName(13000000), ""));
    QsoRecord q;
    q.call = "K1ABC"; q.grid = "FN42"; q.mode = "FT8"; q.rstSent = "-12"; q.rstRcvd = "+03";
    q.myCall = "W9XYZ"; q.myGrid = "EN37"; q.freqHz = 14075500;
    q.startMs = 1794328740000LL;                   // 2026-11-10 16:39:00 UTC
    q.endMs = q.startMs + 75000;
    std::string rec = adifRecord(q);
    printf("%s\n", rec.c_str());
    CHECK(adifField(rec, "CALL") == "K1ABC" && adifField(rec, "GRIDSQUARE") == "FN42" && adifField(rec, "BAND") == "20m");
    CHECK(adifField(rec, "QSO_DATE") == "20261110" && adifField(rec, "TIME_ON") == "163900" && adifField(rec, "TIME_OFF") == "164015");
    CHECK(adifField(rec, "FREQ") == "14.075500" && adifField(rec, "RST_SENT") == "-12" && adifField(rec, "RST_RCVD") == "+03");
    CHECK(rec.find("<EOR>") == rec.size() - 5 && rec.find("SUBMODE") == std::string::npos);
    CHECK(rec.find("OPERATOR") == std::string::npos && rec.find("MY_SIG") == std::string::npos);   // not written unless set
    q.op = "K9OP"; q.mySig = "POTA"; q.mySigInfo = "US-1234";
    std::string rec2 = adifRecord(q);
    CHECK(adifField(rec2, "OPERATOR") == "K9OP" && adifField(rec2, "MY_SIG") == "POTA" && adifField(rec2, "MY_SIG_INFO") == "US-1234");
    CHECK(rec2.find("<MY_SIG_INFO:7>US-1234") != std::string::npos && adifField(rec2, "STATION_CALLSIGN") == "W9XYZ");
    {
        Settings st;
        CHECK(st.set("op", "k9op") && st.op == "K9OP" && !st.set("op", "not a call") && st.set("op", "") && st.op.empty());
        CHECK(st.set("mySigInfo", " us-1234 ") && st.mySigInfo == "US-1234" && st.set("mySigInfo", "US-1234,US-5678") && !st.set("mySigInfo", "<EOR>"));
        CHECK(st.mySig.empty() && st.set("mySig", "wwff") && st.mySig == "WWFF" && st.set("mySig", "") && st.mySig.empty());
    }
    auto pkt = wsjtxLoggedAdif("ID", "X<EOR>");
    const uint8_t hdr[] = {0xAD, 0xBC, 0xCB, 0xDA, 0, 0, 0, 2, 0, 0, 0, 12, 0, 0, 0, 2, 'I', 'D'};
    CHECK(pkt.size() > sizeof hdr + 4 && !memcmp(pkt.data(), hdr, sizeof hdr));
    uint32_t alen = (pkt[18] << 24) | (pkt[19] << 16) | (pkt[20] << 8) | pkt[21];
    CHECK(alen == pkt.size() - 22 && !memcmp(pkt.data() + pkt.size() - 6, "X<EOR>", 6));

    // --- settings ---
    Settings s;
    CHECK(s.set("call", " w9xyz ") && s.call == "W9XYZ" && !s.set("call", "NOPE") && !s.set("call", "W9 XYZ"));
    CHECK(s.set("grid", "en37") && s.grid == "EN37" && !s.set("grid", "ZZ99") && s.set("grid", "EN37ab"));
    CHECK(!s.set("apPass", "short") && s.set("apPass", "longenough") && !s.set("txFreq", "5000") && s.set("txFreq", "1234"));
    CHECK(s.set("civAddr", "0x88") && s.civAddr == 0x88 && !s.set("bogus", "1"));
    s.logHost = "192.168.4.2";
    CHECK(s.save("test/out/settings.txt"));
    Settings s2;
    CHECK(s2.load("test/out/settings.txt") && s2.call == "W9XYZ" && s2.apPass == "longenough" && s2.txFreq == 1234 && s2.logHost == "192.168.4.2");
    CHECK(s.json().find("longenough") == std::string::npos && s.json().find("\"call\":\"W9XYZ\"") != std::string::npos);

    // --- message parsing ---
    ParsedMsg p;
    CHECK(parseMsg("CQ K1ABC FN42", p) && p.kind == K_CQ && p.from == "K1ABC" && p.grid == "FN42" && p.cqTag.empty());
    CHECK(parseMsg("CQ DX K1ABC FN42", p) && p.kind == K_CQ && p.from == "K1ABC" && p.cqTag == "DX");
    CHECK(parseMsg("CQ K1ABC", p) && p.kind == K_CQ && p.from == "K1ABC" && p.grid.empty());
    CHECK(parseMsg("CQ POTA K1ABC/P", p) && p.from == "K1ABC/P" && p.cqTag == "POTA");
    CHECK(parseMsg("W9XYZ K1ABC -07", p) && p.kind == K_REPORT && p.report == -7 && p.to == "W9XYZ" && p.from == "K1ABC");
    CHECK(parseMsg("W9XYZ K1ABC R+12", p) && p.kind == K_RREPORT && p.report == 12);
    CHECK(parseMsg("W9XYZ K1ABC RR73", p) && p.kind == K_RR73);
    CHECK(parseMsg("W9XYZ K1ABC RRR", p) && p.kind == K_RRR);
    CHECK(parseMsg("W9XYZ K1ABC 73", p) && p.kind == K_73);
    CHECK(parseMsg("W9XYZ K1ABC FN42", p) && p.kind == K_GRID && p.grid == "FN42");
    CHECK(parseMsg("<W9XYZ> PJ4/K1ABC RR73", p) && p.to == "W9XYZ" && p.from == "PJ4/K1ABC" && p.kind == K_RR73);
    CHECK(parseMsg("W9XYZ K1ABC R FN42", p) && p.kind == K_OTHER);
    CHECK(!parseMsg("TNX", p));

    // --- sequencer: answering a CQ, with a lost message and a repeat ---
    Sequencer q1;
    std::vector<QsoLog> logs;
    q1.myCall = "W9XYZ"; q1.myGrid = "EN37"; q1.opMode = 0; q1.txEnabled = true;
    q1.now = [] { return (int64_t)1794311940000LL + fakeMono; };
    q1.onLog = [&](const QsoLog& l) { logs.push_back(l); };
    auto D = [](const char* t, int snr) { Decode d; d.text = t; d.snr = snr; return std::vector<Decode>{d}; };
    std::string tx;
    q1.callStation("K1ABC", "FN42", -9, 0);
    CHECK(!q1.nextTx(0, tx) && q1.nextTx(1, tx) && tx == "K1ABC W9XYZ EN37");
    q1.txDone();
    q1.onDecodes({}, 0);                                           // no reply: repeat
    CHECK(q1.nextTx(1, tx) && tx == "K1ABC W9XYZ EN37" && q1.retries == 1);
    q1.txDone();
    q1.onDecodes(D("W9XYZ K1ABC -15", -8), 0);
    CHECK(q1.nextTx(1, tx) && tx == "K1ABC W9XYZ R-09" && q1.rptRcvd == -15 && q1.retries == 0);
    q1.txDone();
    q1.onDecodes(D("W9XYZ K1ABC -15", -8), 0);                     // they did not copy: same again
    CHECK(q1.nextTx(1, tx) && tx == "K1ABC W9XYZ R-09");
    q1.txDone();
    q1.onDecodes(D("W9XYZ K1ABC RR73", -8), 0);
    CHECK(logs.size() == 1 && logs[0].call == "K1ABC" && logs[0].grid == "FN42" && logs[0].rptSent == -9 && logs[0].rptRcvd == -15);
    CHECK(q1.nextTx(1, tx) && tx == "K1ABC W9XYZ 73");
    q1.txDone();
    CHECK(!q1.nextTx(1, tx) && !q1.txEnabled && q1.dxCall.empty());   // manual mode stands by after the QSO

    // --- sequencer: running CQ, two callers, the missed-RR73 repeat, and giving up on a no-show ---
    Sequencer q2;
    logs.clear();
    q2.myCall = "W9XYZ"; q2.myGrid = "EN37"; q2.opMode = 1; q2.txEnabled = true; q2.maxRetries = 3;
    q2.now = q1.now;
    q2.onLog = [&](const QsoLog& l) { logs.push_back(l); };
    q2.startCq();
    CHECK(q2.nextTx(0, tx) && tx == "CQ W9XYZ EN37");
    q2.txDone();
    std::vector<Decode> two = D("W9XYZ K1ABC FN42", -12);
    { Decode d; d.text = "W9XYZ N0CALL DM79"; d.snr = -3; two.push_back(d); Decode e; e.text = "CQ VK2DEF QF56"; e.snr = 5; two.push_back(e); }
    q2.onDecodes(two, 1);
    CHECK(q2.dxCall == "N0CALL" && q2.nextTx(0, tx) && tx == "N0CALL W9XYZ -03");   // strongest caller first
    q2.txDone();
    q2.onDecodes(D("W9XYZ N0CALL R-11", -4), 1);
    CHECK(q2.nextTx(0, tx) && tx == "N0CALL W9XYZ RR73");
    q2.txDone();
    CHECK(logs.size() == 1 && logs[0].call == "N0CALL" && logs[0].rptRcvd == -11 && logs[0].grid == "DM79");
    CHECK(q2.nextTx(0, tx) && tx == "CQ W9XYZ EN37");                // straight back to CQ
    q2.onDecodes(D("W9XYZ N0CALL R-11", -4), 1);                   // they missed the RR73
    CHECK(q2.nextTx(0, tx) && tx == "N0CALL W9XYZ RR73");
    q2.txDone();
    CHECK(logs.size() == 1);                                         // not logged twice
    q2.onDecodes(D("W9XYZ K1ABC FN42", -12), 1);
    for (int i = 0; i < 3; i++) { CHECK(q2.nextTx(0, tx) && tx == "K1ABC W9XYZ -12"); q2.txDone(); q2.onDecodes({}, 1); }
    CHECK(q2.nextTx(0, tx) && tx == "CQ W9XYZ EN37" && logs.size() == 1);   // gave up after maxRetries
    q2.onDecodes(D("W9XYZ K1ABC 73", -12), 1);                      // stray 73: no reply
    CHECK(q2.nextTx(0, tx) && tx == "CQ W9XYZ EN37");

    // --- sequencer: logging by hand when the other station fades out ---
    {
        Sequencer q;
        std::vector<QsoLog> fl;
        q.myCall = "W9XYZ"; q.myGrid = "EN37"; q.opMode = 1; q.txEnabled = true;
        q.now = q1.now;
        q.onLog = [&](const QsoLog& l) { fl.push_back(l); };
        CHECK(q.forceLog() == -1);                                   // nothing in progress
        q.callStation("K2AAA", "FN20", -7, 0);
        q.txDone();                                                  // only our grid went out
        CHECK(q.forceLog() == -2 && fl.empty());                     // no report either way: ignored
        q.onDecodes(D("W9XYZ K2AAA -14", -6), 0);                    // their report arrived, ours not sent yet
        CHECK(q.forceLog() == 0 && fl.size() == 1 && fl[0].partial && fl[0].call == "K2AAA" &&
              fl[0].rptRcvd == -14 && fl[0].rptSent == Sequencer::UNSET);
        CHECK(q.forceLog() == 1 && fl.size() == 1);                  // not twice
        q.startCq();
        q.txDone();
        q.onDecodes(D("W9XYZ N3BBB FM19", -10), 1);
        CHECK(q.nextTx(0, tx) && tx == "N3BBB W9XYZ -10");
        q.txDone();                                                  // our report went out, then they vanished
        q.onDecodes({}, 1);
        CHECK(q.forceLog() == 0 && fl.size() == 2 && fl[1].call == "N3BBB" && fl[1].rptSent == -10 &&
              fl[1].rptRcvd == Sequencer::UNSET && fl[1].grid == "FM19");
    }

    // --- sequencer: auto hunt skips directed CQs, worked stations and our own call ---
    Sequencer q3;
    q3.myCall = "W9XYZ"; q3.myGrid = "EN37"; q3.opMode = 2; q3.txEnabled = true;
    q3.now = q1.now;
    q3.workedBefore = [](const std::string& c) { return c == "K1ABC"; };
    std::vector<Decode> cqs;
    for (auto t : {"CQ K1ABC FN42", "CQ DX VK2DEF QF56", "CQ W9XYZ EN37", "CQ N0CALL DM79", "CQ G4AAA IO91"}) { Decode d; d.text = t; d.snr = (int)cqs.size() * 2 - 20; cqs.push_back(d); }
    cqs[0].snr = 20; cqs[1].snr = 19;
    q3.onDecodes(cqs, 1);
    CHECK(q3.dxCall == "G4AAA" && q3.dxGrid == "IO91" && q3.nextTx(0, tx) && tx == "G4AAA W9XYZ EN37");
    q3.txEnabled = false;
    Sequencer q4 = q3; q4.clearQso();
    q4.onDecodes(cqs, 1);
    CHECK(q4.dxCall.empty());                                        // TX disabled: do not start anything

    printf(fails ? "t_units: %d of %d FAILED\n" : "t_units: all %d checks passed\n", fails ? fails : checks, checks);
    return fails ? 1 : 0;
}
