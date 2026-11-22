// Text CAT for the Yaesu FTX-1 and the QRP Labs QMX / QMX+. Both use ASCII commands ending in ';'
// but agree on little else, so every function branches on the protocol. State is translated to
// the Icom numbering the rest of the firmware uses (see civ.h).
//
// FTX-1 (CAT Operation Reference Manual 2507): FA 9 digits, MD0x, TX0/1, SM0, RM meters, PC with a
//   head digit (1 field head, 2 SPA-1), KM/KY for keyer text.
// QMX (CAT manual 1_04): TS-480 subset. IF carries frequency, TX state and mode; TQ keys the
//   transmitter; MD6 is the digital mode; KY sends keyer text.
// Kenwood / Elecraft (TS-2000, TS-480, TS-590, K2, K3): the same IF layout and FA, TX; / RX; for
//   PTT, Kenwood's mode digits (1 LSB 2 USB 3 CW 4 FM 5 AM 6 FSK 7 CW-R 9 FSK-R); FT8 runs in USB.
// Other Yaesu radios (FTDX10, FT-991A, FT-710, FTDX101, FT-891): the FTX-1 commands, but PC is
//   three digits of watts without the FTX-1's head digit.
#include "civ.h"
#include "clock.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>

void Radio::sendText(const std::string& s, bool front) {
    if (proto == RP_NONE) return;
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<uint8_t> f(s.begin(), s.end());
    if (front) q_.push_front(f);
    else if (q_.size() < 24) q_.push_back(f);
}

static std::string fmt(const char* f, long long a, long long b = 0) {
    char buf[40];
    snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

static int scale255(int percent) { return (percent * 255 + 50) / 100; }
static int pct255(int v) { return (v * 100 + 127) / 255; }
static int num(const std::string& s, size_t pos, size_t n = std::string::npos) {
    if (pos >= s.size()) return 0;
    return atoi(s.substr(pos, n).c_str());
}

// ---------------------------------------------------------------------------- commands
void Radio::tSetFreq(uint64_t hz) {
    sendText(proto == RP_YAESU ? fmt("FA%09lld;", (long long)hz) : fmt("FA%lld;", (long long)hz));
    sendText(proto == RP_YAESU ? "FA;" : "IF;");
}

void Radio::tSetUsbData() {
    if (proto == RP_YAESU) { sendText("MD0C;"); sendText("MD0;"); }      // DATA-U
    else if (proto == RP_KENWOOD) { sendText("MD2;"); sendText("IF;"); } // USB
    else { sendText("Q90;"); sendText("MD6;"); sendText("MD;"); }        // IQ mode off, DIGI
}

void Radio::tSetRfPower(int percent) {
    if (proto == RP_YAESU && pcHead_ == 0 && textId_ && textId_ != 800) {   // FTDX10 and friends: PC + watts
        int w = (percent * 100 + 50) / 100;
        if (w < 5) w = 5;
        sendText(fmt("PC%03lld;", w));
        sendText("PC;");
        return;
    }
    if (proto != RP_YAESU || pcHead_ < 1) return;                        // QMX / Kenwood: left as the radio has it
    int maxW = pcHead_ == 2 ? 100 : 10;
    int w = (percent * maxW + 50) / 100;
    if (w < (pcHead_ == 2 ? 5 : 1)) w = pcHead_ == 2 ? 5 : 1;
    sendText(fmt("PC%lld%03lld;", pcHead_, w));
    sendText("PC;");
}

// Icom mode code -> the radio's own. 0 = no such mode.
static char yaesuMode(uint8_t m) {
    switch (m) {
    case 0x00: return '1'; case 0x01: return '2'; case 0x02: return '5'; case 0x03: return '3';
    case 0x04: return '6'; case 0x05: return '4'; case 0x07: return '7'; case 0x08: return '9';
    case 0x17: return 'H';
    default: return 0;
    }
}
static char kenwoodMode(uint8_t m) {
    switch (m) {
    case 0x00: return '1'; case 0x01: return '2'; case 0x02: return '5'; case 0x03: return '3';
    case 0x04: return '6'; case 0x05: return '4'; case 0x07: return '7'; case 0x08: return '9';
    default: return 0;
    }
}
static char qmxMode(uint8_t m) {
    switch (m) {
    case 0x00: return '1'; case 0x01: return '2'; case 0x03: return '3'; case 0x07: return '7';
    default: return 0;
    }
}

void Radio::tSetMode(uint8_t m) {
    char c = proto == RP_YAESU ? yaesuMode(m) : proto == RP_KENWOOD ? kenwoodMode(m) : qmxMode(m);
    if (!c) { ng++; return; }
    if (proto == RP_YAESU) { sendText(std::string("MD0") + c + ";"); sendText("MD0;"); }
    else { sendText(std::string("MD") + c + ";"); sendText("MD;"); }
}

void Radio::tSetData(bool on) {
    if (proto == RP_KENWOOD) { ng++; return; }                          // no data mode: FT8 runs in USB
    int m = mode.load();
    char c;
    if (proto == RP_YAESU) {
        if (on) c = m == 0x00 ? '8' : m == 0x05 ? 'A' : 'C';
        else c = m == 0x00 ? '1' : m == 0x05 ? '4' : '2';
        sendText(std::string("MD0") + c + ";"); sendText("MD0;");
    } else {
        if (on) c = m == 0x00 ? '9' : '6';
        else c = m == 0x00 ? '1' : '2';
        sendText(std::string("MD") + c + ";"); sendText("MD;");
    }
}

void Radio::tSetLevel(uint8_t sub, int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (sub == 0x0C) return setKeySpeed(6 + (percent * 42 + 50) / 100);
    if (proto != RP_YAESU) { ng++; return; }                             // QMX: levels are front-panel only
    switch (sub) {
    case 0x01: sendText(fmt("AG0%03lld;", scale255(percent))); sendText("AG0;"); break;
    case 0x02: sendText(fmt("RG0%03lld;", scale255(percent))); sendText("RG0;"); break;
    case 0x03: sendText(fmt("SQ0%03lld;", scale255(percent))); sendText("SQ0;"); break;
    case 0x06: sendText(fmt("RL0%02lld;", (percent + 5) / 10)); sendText("RL0;"); break;   // DNR level 0 (off) .. 10
    case 0x0A: tSetRfPower(percent); break;
    case 0x0B: sendText(fmt("MG%03lld;", percent)); sendText("MG;"); break;
    default: ng++; break;
    }
}

static int yaesuPaBand(uint64_t hz) { return hz < 76000000ULL ? 0 : hz < 300000000ULL ? 1 : 2; }

void Radio::tSetFunc(uint8_t sub, uint8_t v) {
    if (proto != RP_YAESU) { ng++; return; }
    int pb = yaesuPaBand(freq.load());
    switch (sub) {
    case 0x02: sendText(fmt("PA%lld%lld;", pb, pb ? (v ? 1 : 0) : (v > 2 ? 2 : v))); sendText(fmt("PA%lld;", pb)); break;
    case 0x12: sendText(fmt("GT0%lld;", v >= 1 && v <= 3 ? v : 4)); sendText("GT0;"); break;
    case 0x22: sendText(v ? "NL0005;" : "NL0000;"); sendText("NL0;"); break;      // no on/off command: level 0 is off
    case 0x40: {
        int lvl = level[0x06].load() / 10;
        sendText(fmt("RL0%02lld;", v ? (lvl > 0 ? lvl : 5) : 0)); sendText("RL0;");
        break;
    }
    case 0x41: sendText(fmt("BC0%lld;", v ? 1 : 0)); sendText("BC0;"); break;
    case 0x44: sendText(v ? "PR02;" : "PR01;"); sendText("PR0;"); break;
    case 0x46: sendText(fmt("VX%lld;", v ? 1 : 0)); sendText("VX;"); break;
    case 0x47: sendText(fmt("BI%lld;", v ? 1 : 0)); sendText("BI;"); break;
    default: ng++; break;
    }
}

void Radio::tSetAtt(bool on) {
    if (proto != RP_YAESU) { ng++; return; }
    sendText(on ? "RA01;" : "RA00;");
    sendText("RA0;");
}

void Radio::tVfo(uint8_t) {
    // The frequency shown is always the operating VFO: the FTX-1 swaps MAIN and SUB, the QMX
    // switches A/B and reports the active one in IF.
    if (proto == RP_YAESU) { sendText("SV;"); sendText("FA;"); sendText("MD0;"); }
    else { sendText("FR;"); }                                            // answer handled in textAnswer: toggles
}

void Radio::tSendCw(const std::string& t) {
    if (proto == RP_YAESU) {
        if (func[0x47].load() == 0) sendText("BI1;");
        sendText("KM1" + t + "};");                                      // write text memory 1 ...
        sendText("KY01;");                                               // ... and play it
        return;
    }
    // QMX: 24 characters per command, space padded, is accepted with "KY compatibility" on or off.
    for (size_t i = 0; i < t.size(); i += 24) {
        std::string part = t.substr(i, 24);
        part.resize(24, ' ');
        sendText("KY " + part + ";");
    }
}

// ---------------------------------------------------------------------------- polling
void Radio::textPoll() {                                                 // mtx_ is held
    auto push = [this](const char* s) { q_.emplace_back(s, s + strlen(s)); };
    int step = pollStep_++;
    if (proto == RP_YAESU) {
        static const char* rx[] = {"FA;", "SM0;", "MD0;", "SM0;", "TX;", "BI;", "PC;", "KS;"};
        static const char* tx[] = {"RM4;", "RM5;", "RM4;", "RM6;", "RM4;", "TX;"};
        push(pttWanted ? tx[step % 6] : rx[step % 8]);
        if (extPoll && !pttWanted && (step & 1)) {
            static const char* ext[] = {"AG0;", "RG0;", "SQ0;", "MG;", "RL0;", "GT0;", "NL0;", "BC0;", "PR0;", "VX;", "RA0;", "PA"};
            const char* e = ext[extStep_++ % 12];
            if (!strcmp(e, "PA")) { std::string s = fmt("PA%lld;", yaesuPaBand(freq.load())); q_.emplace_back(s.begin(), s.end()); }
            else push(e);
        }
    } else if (proto == RP_KENWOOD) {
        static const char* rx[] = {"IF;", "IF;", "IF;", "KS;"};
        push(pttWanted ? "IF;" : rx[step % 4]);
    } else {
        static const char* rx[] = {"IF;", "SM;", "IF;", "KS;"};
        static const char* tx[] = {"IF;", "PC;", "SW;"};
        push(pttWanted ? tx[step % 3] : rx[step % 4]);
    }
}

// ---------------------------------------------------------------------------- answers
void Radio::textRx(char c) {
    std::string a;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (c != ';') {
            if ((unsigned char)c >= 0x20 && line_.size() < 80) line_ += c;
            return;
        }
        a.swap(line_);
    }
    textAnswer(a);
}

static void fromYaesuMode(char c, std::atomic<int>& mode, std::atomic<int>& data) {
    int m = -1, d = 0;
    switch (c) {
    case '1': m = 0x00; break; case '2': m = 0x01; break; case '3': m = 0x03; break; case '4': case 'B': m = 0x05; break;
    case '5': case 'D': m = 0x02; break; case '6': m = 0x04; break; case '7': m = 0x07; break; case '9': m = 0x08; break;
    case '8': m = 0x00; d = 1; break; case 'C': case 'E': m = 0x01; d = 1; break; case 'A': case 'F': m = 0x05; d = 1; break;
    case 'H': case 'I': m = 0x17; break;
    default: return;
    }
    mode = m; data = d;
}

static void fromKenwoodMode(char c, std::atomic<int>& mode, std::atomic<int>& data) {
    int m = -1;
    switch (c) {
    case '1': m = 0x00; break; case '2': m = 0x01; break; case '3': m = 0x03; break; case '4': m = 0x05; break;
    case '5': m = 0x02; break; case '6': m = 0x04; break; case '7': m = 0x07; break; case '9': m = 0x08; break;
    default: return;
    }
    mode = m; data = 0;
}

static void fromQmxMode(char c, std::atomic<int>& mode, std::atomic<int>& data) {
    int m = -1, d = 0;
    switch (c) {
    case '1': m = 0x00; break; case '2': m = 0x01; break; case '3': m = 0x03; break; case '5': m = 0x02; break;
    case '7': m = 0x07; break; case '6': m = 0x01; d = 1; break; case '9': m = 0x00; d = 1; break;
    default: return;
    }
    mode = m; data = d;
}

void Radio::textAnswer(const std::string& a) {
    if (a == "?") { ng++; return; }
    if (a.size() < 3 || !isupper((unsigned char)a[0]) || !isupper((unsigned char)a[1])) return;   // not an answer (or an echo of a read)
    std::string c = a.substr(0, 2);
    const bool y = proto == RP_YAESU;
    // Only full-length answers to the regular polls count as "the radio is there": the port probe
    // relies on this to tell the radios apart (ID is 4 digits on Yaesu, 3 on Kenwood-style radios)
    // and a CI-V port, which echoes whatever it is sent, must not look like a text radio.
    bool alive;
    if (y) alive = (c == "ID" && a.size() == 6) || (c == "FA" && a.size() == 11) || (c == "SM" && a.size() == 6) ||
                   (c == "TX" && a.size() == 3) || (c == "RM" && a.size() >= 9);
    else alive = (c == "ID" && a.size() == 5) || (c == "IF" && a.size() >= 30);
    if (alive) lastRxMono = clk::monoMs();
    if (c == "ID") { if (alive) textId_ = atoi(a.c_str() + 2); return; }
    if (!connected()) return;                                            // nothing is believed until the radio is identified
    if (c == "FA") { freq = strtoull(a.c_str() + 2, nullptr, 10); return; }
    if (c == "KS") { keySpeed = num(a, 2); return; }
    if (y) {
        if (c == "MD" && a.size() >= 4 && a[2] == '0') { yMode_ = a[3]; fromYaesuMode(a[3], mode, dataMode); }
        else if (c == "TX") pttState = a[2] != '0';
        else if (c == "SM" && a[2] == '0') smeter = num(a, 3, 3);
        else if (c == "RM" && a.size() >= 6) {
            int v = num(a, 3, 3);
            if (a[2] == '1') smeter = v; else if (a[2] == '5') po = v; else if (a[2] == '6') swr = v; else if (a[2] == '4') { alc = v; alcSeq++; }
        }
        else if (c == "AG" && a[2] == '0') level[0x01] = pct255(num(a, 3, 3));
        else if (c == "RG" && a[2] == '0') level[0x02] = pct255(num(a, 3, 3));
        else if (c == "SQ" && a[2] == '0') level[0x03] = pct255(num(a, 3, 3));
        else if (c == "MG") level[0x0B] = num(a, 2, 3);
        else if (c == "PC" && a.size() == 5) level[0x0A] = num(a, 2, 3);  // FTDX10 and friends: watts of 100
        else if (c == "PC" && a.size() >= 6) {
            pcHead_ = a[2] - '0';
            int w = (int)(atof(a.substr(3, 3).c_str()) + 0.5);            // the field head can report e.g. "7.5"
            level[0x0A] = w * 100 / (pcHead_ == 2 ? 100 : 10);
        }
        else if (c == "RL" && a[2] == '0') { int v = num(a, 3, 2); if (v > 0) level[0x06] = v * 10; func[0x40] = v > 0; }
        else if (c == "NL" && a[2] == '0') func[0x22] = num(a, 3, 3) > 0;
        else if (c == "PA" && a.size() >= 4) func[0x02] = a[3] - '0';
        else if (c == "GT" && a.size() >= 4) { int v = a[3] - '0'; func[0x12] = v > 3 ? v - 3 : v; }
        else if (c == "BC" && a.size() >= 4) func[0x41] = a[3] != '0';
        else if (c == "PR" && a.size() >= 4 && a[2] == '0') func[0x44] = a[3] == '2';
        else if (c == "VX") func[0x46] = a[2] != '0';
        else if (c == "BI") func[0x47] = a[2] != '0';
        else if (c == "RA" && a.size() >= 4) att = a[3] != '0' ? 12 : 0;
        return;
    }
    // QMX and Kenwood / Elecraft: the same IF layout (TS-480)
    const bool ken = proto == RP_KENWOOD;
    if (c == "IF" && a.size() >= 30) {
        freq = strtoull(a.substr(2, 11).c_str(), nullptr, 10);
        if (pttMethod.load() == PTT_CAT) pttState = a[28] == '1';
        if (ken) fromKenwoodMode(a[29], mode, dataMode); else fromQmxMode(a[29], mode, dataMode);
    }
    else if (c == "MD") { if (ken) fromKenwoodMode(a[2], mode, dataMode); else fromQmxMode(a[2], mode, dataMode); }
    else if (ken) return;                                                // the rest is the QMX's
    else if (c == "TQ") pttState = a[2] == '1';
    else if (c == "SM") { int v = num(a, 2) * 120 / 54; smeter = v < 0 ? 0 : v > 255 ? 255 : v; }   // dB -> Icom scale, S9 = 120
    else if (c == "PC") { int v = num(a, 2) * 213 / 50; po = v > 255 ? 255 : v; }                    // tenths of a watt; 5 W = full scale
    else if (c == "SW") {                                                                           // hundredths -> Icom's SWR scale
        int s = num(a, 2);
        swr = s <= 100 ? 0 : s < 150 ? (s - 100) * 48 / 50 : s < 200 ? 48 + (s - 150) * 32 / 50 : s < 300 ? 80 + (s - 200) * 40 / 100 : 255 < 120 + (s - 300) / 4 ? 255 : 120 + (s - 300) / 4;
    }
    else if (c == "FR" && a.size() >= 3) {                               // answer to the A/B button's query: switch to the other VFO
        sendText(a[2] == '0' ? "FR1;" : "FR0;");
        sendText(a[2] == '0' ? "FT1;" : "FT0;");
        sendText("IF;");
    }
}
