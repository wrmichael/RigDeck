#include "civ.h"
#include "clock.h"
#include <cstring>
#include <cstdio>
#include <cctype>

std::vector<uint8_t> civBuild(uint8_t to, uint8_t from, uint8_t cmd, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> f = {0xFE, 0xFE, to, from, cmd};
    f.insert(f.end(), data.begin(), data.end());
    f.push_back(0xFD);
    return f;
}

void civFreqToBcd(uint64_t hz, uint8_t out[5]) {
    for (int i = 0; i < 5; i++) {          // least significant pair first: 10Hz/1Hz, 1kHz/100Hz, ...
        unsigned v = (unsigned)(hz % 100);
        out[i] = (uint8_t)(((v / 10) << 4) | (v % 10));
        hz /= 100;
    }
}

uint64_t civBcdToFreq(const uint8_t* b, size_t n) {
    uint64_t f = 0, mult = 1;
    for (size_t i = 0; i < n; i++) {
        f += (uint64_t)((b[i] >> 4) * 10 + (b[i] & 0x0F)) * mult;
        mult *= 100;
    }
    return f;
}

const char* civModelName(uint8_t a) {
    switch (a) {
    case 0x88: return "IC-7100";
    case 0xA4: return "IC-705";
    case 0x94: return "IC-7300";
    case 0x98: return "IC-7610";
    case 0xA2: return "IC-9700";
    case 0xAC: return "IC-905";
    default: return "Icom";
    }
}

int civBcdToInt(const uint8_t* b, size_t n) {
    int v = 0;
    for (size_t i = 0; i < n; i++) v = v * 100 + (b[i] >> 4) * 10 + (b[i] & 0x0F);
    return v;
}

bool CivParser::feed(uint8_t byte, CivFrame& out) {
    if (!in_) {
        if (byte == 0xFE) { if (++pre_ >= 2) { in_ = true; buf_.clear(); pre_ = 0; } }
        else pre_ = 0;
        return false;
    }
    if (byte == 0xFE && buf_.empty()) return false;        // extra preamble bytes
    if (byte == 0xFC) { in_ = false; return false; }       // bus collision marker: drop the frame
    if (byte != 0xFD) {
        if (buf_.size() > 64) { in_ = false; return false; }
        buf_.push_back(byte);
        return false;
    }
    in_ = false;
    if (buf_.size() < 3) return false;
    out.to = buf_[0]; out.from = buf_[1]; out.cmd = buf_[2];
    out.data.assign(buf_.begin() + 3, buf_.end());
    return true;
}

void Radio::send(uint8_t cmd, const std::vector<uint8_t>& data, bool front) {
    if (proto == RP_NONE) return;
    std::lock_guard<std::mutex> lk(mtx_);
    auto f = civBuild(addr, CTRL, cmd, data);
    if (front) q_.push_front(f);
    else if (q_.size() < 24) q_.push_back(f);
}

void Radio::setFreq(uint64_t hz) {
    if (proto == RP_NONE) { freq = hz; return; }           // no CAT: the band buttons set the dial
    if (isText()) return tSetFreq(hz);
    uint8_t b[5];
    civFreqToBcd(hz, b);
    send(0x05, std::vector<uint8_t>(b, b + 5));
    send(0x03, {});
}

void Radio::setUsbData() {
    if (proto == RP_NONE) { mode = 0x01; dataMode = 1; return; }   // no CAT: what the operator sets by hand
    if (isText()) return tSetUsbData();
    send(0x06, {0x01, 0x01});              // USB, FIL1
    send(0x1A, {0x06, 0x01, 0x01});        // DATA mode on, FIL1
    send(0x04, {});
    send(0x1A, {0x06});
}

void Radio::ptt(bool on) {
    pttWanted = on;
    int m = pttMethod.load();
    if (m == PTT_RTS || m == PTT_DTR) {                    // a serial port's line keys the radio
        if (pttLine) pttLine(on);
        pttState = on;
        return;
    }
    if (m == PTT_VOX || proto == RP_NONE) { pttState = on; return; }   // the transmit audio keys it
    if (proto == RP_YAESU) return sendText(on ? "TX1;" : "TX0;", true);
    if (proto == RP_QMX) return sendText(on ? "TQ1;" : "TQ0;", true);
    // Kenwood: plain TX; transmits the microphone. TS-480 / TS-590 / TS-990 and radios that emulate them
    // (FX-4CR) take TX1; ("data send") to transmit the USB / rear audio. TS-2000 and Elecraft: TX;.
    if (proto == RP_KENWOOD) return sendText(on ? (textId_ == 17 || textId_ == 19 ? "TX;" : "TX1;") : "RX;", true);
    send(0x1C, {0x00, (uint8_t)(on ? 0x01 : 0x00)}, true);
}

void Radio::setRfPower(int percent) {
    if (percent < 0) return;
    if (percent > 100) percent = 100;
    if (isText()) return tSetRfPower(percent);
    int v = percent * 255 / 100;
    send(0x14, {0x0A, (uint8_t)(((v / 1000) << 4) | ((v / 100) % 10)), (uint8_t)((((v / 10) % 10) << 4) | (v % 10))});
}

void Radio::requestId() { if (isText()) sendText("ID;"); else send(0x19, {0x00}); }

Radio::Radio() {
    for (auto& l : level) l = -1;
    for (auto& f : func) f = -1;
}

static std::vector<uint8_t> bcd255(int percent) {     // 0..100 % -> 0000..0255 as two BCD bytes
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int v = (percent * 255 + 50) / 100;
    return {(uint8_t)(((v / 1000) << 4) | ((v / 100) % 10)), (uint8_t)((((v / 10) % 10) << 4) | (v % 10))};
}

void Radio::setMode(uint8_t m, uint8_t fil) {
    if (proto == RP_NONE) { mode = m; dataMode = 0; filter = fil < 1 || fil > 3 ? 1 : fil; return; }
    if (isText()) return tSetMode(m);
    send(0x06, {m, (uint8_t)(fil < 1 || fil > 3 ? 1 : fil)});
    send(0x04, {});
    send(0x1A, {0x06});
}

void Radio::setData(bool on) {
    if (proto == RP_NONE) { dataMode = on ? 1 : 0; return; }
    if (isText()) return tSetData(on);
    int fil = filter.load();
    send(0x1A, {0x06, (uint8_t)(on ? 1 : 0), (uint8_t)(on ? (fil >= 1 && fil <= 3 ? fil : 1) : 0)});
    send(0x1A, {0x06});
}

void Radio::setLevel(uint8_t sub, int percent) {
    if (isText()) return tSetLevel(sub, percent);
    auto v = bcd255(percent);
    send(0x14, {sub, v[0], v[1]});
    send(0x14, {sub});
}

void Radio::setFunc(uint8_t sub, uint8_t value) {
    if (isText()) return tSetFunc(sub, value);
    send(0x16, {sub, value});
    send(0x16, {sub});
}

bool Radio::startTuner() {
    if (proto.load() != RP_YAESU) return false;
    sendText("AC002;");                                 // tuning start: the radio transmits and tunes by itself
    sendText("AC;");
    return true;
}

void Radio::setAtt(uint8_t bcdDb) {
    if (isText()) return tSetAtt(bcdDb != 0);
    send(0x11, {bcdDb});
    send(0x11, {});
}

void Radio::vfo(uint8_t sub) {
    if (isText()) return tVfo(sub);
    send(0x07, {sub});
    send(0x03, {});
    send(0x04, {});
}

bool Radio::connected() const { return proto.load() == RP_NONE || clk::monoMs() - lastRxMono.load() < 3000; }

static bool isBcd(uint8_t b) { return (b >> 4) <= 9 && (b & 0x0F) <= 9; }
static int bcd2(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }

bool civParseGpsPos(const uint8_t* p, size_t n, double& lat, double& lon) {
    if (n < 11) return false;
    bool allFF = true;
    for (size_t i = 0; i < 11; i++) if (p[i] != 0xFF) allFF = false;
    if (allFF) return false;                               // no fix (or the GPS is off)
    for (int i = 0; i < 3; i++) if (!isBcd(p[i])) return false;
    if ((p[3] & 0x0F) || (p[3] >> 4) > 9 || (p[4] & 0xF0) || (p[4] & 0x0F) > 1) return false;
    for (int i = 5; i < 10; i++) if (!isBcd(p[i])) return false;
    if ((p[5] & 0xF0) || (p[9] & 0x0F) || (p[10] & 0xF0) || (p[10] & 0x0F) > 1) return false;
    int latDeg = bcd2(p[0]);
    double latMin = bcd2(p[1]) + bcd2(p[2]) / 100.0 + (p[3] >> 4) / 1000.0;
    int lonDeg = (p[5] & 0x0F) * 100 + bcd2(p[6]);
    double lonMin = bcd2(p[7]) + bcd2(p[8]) / 100.0 + (p[9] >> 4) / 1000.0;
    if (latDeg > 90 || latMin >= 60 || lonDeg > 180 || lonMin >= 60) return false;
    lat = latDeg + latMin / 60.0;
    lon = lonDeg + lonMin / 60.0;
    if (!(p[4] & 0x0F)) lat = -lat;
    if (!(p[10] & 0x0F)) lon = -lon;
    if (lat == 0 && lon == 0) return false;               // 0/0: not a real position
    return true;
}

void Radio::poll() {
    if (proto == RP_NONE) return;
    int64_t now = clk::monoMs();
    std::vector<uint8_t> out;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (now - lastSend_ < 40) return;                  // give the radio time to answer
        if (q_.empty() && now - lastPoll_ >= 200 && isText()) {
            lastPoll_ = now;
            textPoll();
        } else if (q_.empty() && now - lastPoll_ >= 200) {
            lastPoll_ = now;
            static const uint8_t rxPoll[][2] = {{0x03, 0xFF}, {0x15, 0x02}, {0x04, 0xFF}, {0x15, 0x02}, {0x1A, 0x06}, {0x1C, 0x00}, {0x16, 0x47}};
            static const uint8_t txPoll[][2] = {{0x15, 0x13}, {0x15, 0x11}, {0x15, 0x13}, {0x15, 0x12}, {0x15, 0x13}, {0x1C, 0x00}};
            const uint8_t* p = pttWanted ? txPoll[pollStep_ % 6] : rxPoll[pollStep_ % 7];   // ALC every 0.4 s while transmitting
            pollStep_++;
            std::vector<uint8_t> d;
            if (p[1] != 0xFF) d.push_back(p[1]);
            q_.push_back(civBuild(addr, CTRL, p[0], d));
            if (!pttWanted && gpsState >= 0 && now - gpsAskMono_ >= 30000) {   // the radio's GPS position
                gpsAskMono_ = now;
                bool answering = now - lastRxMono.load() < 2000;  // only count asks while the radio answers others
                if (gpsState == 0 && answering && ++gpsUnanswered_ > 3) gpsState = -1;
                else q_.push_back(civBuild(addr, CTRL, 0x23, {0x00}));
            }
            if (extPoll && !pttWanted && (pollStep_ & 1)) {       // every other poll, one front-panel setting
                static const uint8_t ext[][2] = {{0x14, 0x01}, {0x14, 0x02}, {0x14, 0x03}, {0x14, 0x0A}, {0x14, 0x0B}, {0x14, 0x06},
                                                 {0x16, 0x02}, {0x16, 0x12}, {0x16, 0x22}, {0x16, 0x40}, {0x16, 0x41}, {0x16, 0x44},
                                                 {0x16, 0x46}, {0x11, 0xFF}, {0x14, 0x0C}, {0x16, 0x47}};
                const uint8_t* e = ext[extStep_++ % 16];
                std::vector<uint8_t> ed;
                if (e[1] != 0xFF) ed.push_back(e[1]);
                q_.push_back(civBuild(addr, CTRL, e[0], ed));
            }
        }
        if (q_.empty()) return;
        out = q_.front();
        q_.pop_front();
        lastSend_ = now;
    }
    if (write) write(out.data(), out.size());
}

void Radio::onRx(const uint8_t* d, size_t n) {
    if (isText()) { for (size_t i = 0; i < n; i++) textRx((char)d[i]); return; }
    for (size_t i = 0; i < n; i++) {
        CivFrame f;
        bool got;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            got = parser_.feed(d[i], f);
        }
        if (got) handle(f);
    }
}

void Radio::handle(const CivFrame& f) {
    if (f.from != addr) return;                            // includes the echo of our own commands
    if (f.to != CTRL && f.to != 0x00) return;              // 0x00 = "transceive" broadcast from the radio
    lastRxMono = clk::monoMs();
    const auto& d = f.data;
    switch (f.cmd) {
    case 0x00: case 0x03: if (d.size() >= 5) freq = civBcdToFreq(d.data(), 5); break;
    case 0x01: case 0x04:
        if (d.size() >= 1) mode = d[0];
        if (d.size() >= 2) filter = d[1];
        break;
    case 0x14:
        if (d.size() >= 3 && d[0] < 16) level[d[0]] = (civBcdToInt(d.data() + 1, 2) * 100 + 127) / 255;
        if (d.size() >= 3 && d[0] == 0x0C) keySpeed = 6 + (civBcdToInt(d.data() + 1, 2) * 42 + 127) / 255;
        break;
    case 0x16: if (d.size() >= 2 && d[0] < 0x50) func[d[0]] = d[1]; break;
    case 0x11: if (d.size() >= 1) att = (d[0] >> 4) * 10 + (d[0] & 0x0F); break;
    case 0x1A: if (d.size() >= 2 && d[0] == 0x06) dataMode = d[1]; break;
    case 0x1C: if (d.size() >= 2 && d[0] == 0x00) pttState = d[1] != 0; break;
    case 0x15:
        if (d.size() >= 3) {
            int v = civBcdToInt(d.data() + 1, 2);
            if (d[0] == 0x02) smeter = v;
            else if (d[0] == 0x11) po = v;
            else if (d[0] == 0x12) swr = v;
            else if (d[0] == 0x13) { alc = v; alcSeq++; }
        }
        break;
    case 0x23:
        if (d.size() >= 1 && d[0] == 0x00) {
            gpsState = 1;
            double la, lo;
            if (civParseGpsPos(d.data() + 1, d.size() - 1, la, lo)) {
                std::lock_guard<std::mutex> lk(mtx_);
                gpsLat_ = la; gpsLon_ = lo; gpsMono_ = clk::monoMs();
            }
        }
        break;
    case 0xFA: ng++; break;
    default: break;
    }
}

// ---------------------------------------------------------------------------- CW keyer, model
void Radio::setProto(int p) {
    std::lock_guard<std::mutex> lk(mtx_);
    proto = p;
    q_.clear();
    line_.clear();
    parser_ = CivParser();
    gpsState = 0; gpsUnanswered_ = 0; gpsAskMono_ = 0; gpsMono_ = 0;
    pcHead_ = 0; yMode_ = 0; textId_ = 0; ecModel_ = -1; omAsked_ = false; vnAsked_ = false; fw_.clear(); pollStep_ = 0; extStep_ = 0;
    freq = 0; mode = -1; dataMode = -1; filter = -1; att = -1; keySpeed = -1;
    smeter = 0; po = 0; swr = 0; alc = 0; pttState = false;
    for (auto& l : level) l = -1;
    for (auto& f : func) f = -1;
}

const char* Radio::modelName() const {
    switch (proto.load()) {
    case RP_YAESU:
        switch (textId_) {                              // ID answers of the Yaesu radios that share these commands
        case 761: return "FTDX10";
        case 670: return "FT-991A";
        case 681: return "FTDX101D";
        case 682: return "FTDX101MP";
        case 650: return "FT-891";
        case 800: case 0: return pcHead_ == 2 ? "FTX-1 optima" : "FTX-1";
        default: return pcHead_ == 2 ? "FTX-1 optima" : pcHead_ == 1 ? "FTX-1" : "Yaesu";
        }
    case RP_QMX: return "QMX";
    case RP_NONE: return "No CAT";
    case RP_KENWOOD:
        switch (textId_) {
        case 17: return ecModel_ == 2 ? "KX2" : ecModel_ == 1 ? "KX3" : ecModel_ == 0 ? "K3" : ecModel_ == 3 ? "Elecraft" : "Elecraft K2";
        case 19: return "TS-2000";
        case 20: return "TS-480";
        case 21: return "TS-590S";
        case 22: return "TS-990S";
        case 23: return "TS-590SG";
        default: return "Kenwood";
        }
    default: return civModelName(addr);
    }
}

size_t Radio::cwMax(int proto) { return proto == RP_YAESU ? 50 : proto == RP_QMX || proto == RP_KENWOOD ? 72 : proto == RP_NONE ? 0 : 90; }

void Radio::sendCw(const std::string& text) {
    std::string t;
    for (char ch : text) {                              // what a keyer can send, upper case
        ch = (char)toupper((unsigned char)ch);
        if (isalnum((unsigned char)ch) || strchr(" /?.,=+-@", ch)) t += ch;
    }
    if (t.size() > cwMax(proto)) t.resize(cwMax(proto));
    if (t.empty()) return;
    if (isText()) return tSendCw(t);
    if (func[0x47].load() == 0) send(0x16, {0x47, 0x01});          // semi break-in, or the radio only makes sidetone
    for (size_t i = 0; i < t.size(); i += 30) {                    // command 17 takes up to 30 characters
        std::string part = t.substr(i, 30);
        send(0x17, std::vector<uint8_t>(part.begin(), part.end()));
    }
}

void Radio::setBreakIn(int v) {
    if (proto == RP_YAESU) { sendText(v ? "BI1;" : "BI0;"); sendText("BI;"); return; }
    if (proto != RP_CIV) return;
    send(0x16, {0x47, (uint8_t)v});
    send(0x16, {0x47});
}

void Radio::sendCwPractice(const std::string& text) {
    if (func[0x47].load() != 0) return;                            // only with break-in reported off
    std::string t;
    for (char ch : text) {
        ch = (char)toupper((unsigned char)ch);
        if (isalnum((unsigned char)ch) || strchr(" /?.,=+-@", ch)) t += ch;
    }
    if (t.size() > cwMax(proto)) t.resize(cwMax(proto));
    if (t.empty()) return;
    if (proto == RP_YAESU) { sendText("KM1" + t + "};"); sendText("KY01;"); return; }
    if (proto != RP_CIV) return;
    for (size_t i = 0; i < t.size(); i += 30) {
        std::string part = t.substr(i, 30);
        send(0x17, std::vector<uint8_t>(part.begin(), part.end()));
    }
}

void Radio::stopCw() {
    if (proto == RP_YAESU) return sendText("KY00;", true);
    if (kenwoodish()) return sendText("KY                         ;", true);   // 24 spaces after the separator stop the keyer
    if (proto == RP_NONE) return;
    send(0x17, {0xFF}, true);
}

void Radio::setKeySpeed(int wpm) {
    if (wpm < 6) wpm = 6;
    if (wpm > 48) wpm = 48;
    char b[16];
    if (proto == RP_YAESU) { snprintf(b, sizeof b, "KS%03d;", wpm); sendText(b); sendText("KS;"); }
    else if (proto == RP_QMX) { snprintf(b, sizeof b, "KS%d;", wpm); sendText(b); sendText("KS;"); }
    else if (proto == RP_KENWOOD) { snprintf(b, sizeof b, "KS%03d;", wpm); sendText(b); sendText("KS;"); }
    else if (proto == RP_NONE) return;
    else {
        int v = ((wpm - 6) * 255 + 21) / 42;
        send(0x14, {0x0C, (uint8_t)(((v / 1000) << 4) | ((v / 100) % 10)), (uint8_t)((((v / 10) % 10) << 4) | (v % 10))});
        send(0x14, {0x0C});
    }
    keySpeed = wpm;
}
