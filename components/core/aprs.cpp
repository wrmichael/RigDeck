#include "aprs.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>

namespace aprs {

static const double kPi = 3.14159265358979323846;

uint16_t fcs(const uint8_t* b, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= b[i];
        for (int k = 0; k < 8; k++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    return (uint16_t)~crc;
}

bool validCallSsid(const std::string& c) {
    size_t dash = c.find('-');
    std::string base = c.substr(0, dash);
    if (base.empty() || base.size() > 6) return false;
    for (char ch : base) if (!isupper((unsigned char)ch) && !isdigit((unsigned char)ch)) return false;
    if (dash == std::string::npos) return true;
    std::string s = c.substr(dash + 1);
    if (s.empty() || s.size() > 2) return false;
    for (char ch : s) if (!isdigit((unsigned char)ch)) return false;
    int n = atoi(s.c_str());
    return n >= 0 && n <= 15;
}

static void putAddr(std::vector<uint8_t>& o, std::string a, bool last, bool cmdOrH) {
    bool star = !a.empty() && a.back() == '*';
    if (star) a.pop_back();
    size_t dash = a.find('-');
    std::string base = a.substr(0, dash);
    int ssid = dash == std::string::npos ? 0 : atoi(a.c_str() + dash + 1);
    for (size_t i = 0; i < 6; i++) o.push_back((uint8_t)((i < base.size() ? toupper((unsigned char)base[i]) : ' ') << 1));
    o.push_back((uint8_t)(0x60 | ((ssid & 15) << 1) | (last ? 1 : 0) | ((cmdOrH || star) ? 0x80 : 0)));
}

std::vector<uint8_t> encodeFrame(const Frame& f) {
    std::vector<uint8_t> o;
    putAddr(o, f.dst, false, true);
    putAddr(o, f.src, f.path.empty(), false);
    for (size_t i = 0; i < f.path.size(); i++) putAddr(o, f.path[i], i + 1 == f.path.size(), false);
    o.push_back(0x03);
    o.push_back(0xF0);
    o.insert(o.end(), f.info.begin(), f.info.end());
    uint16_t c = fcs(o.data(), o.size());
    o.push_back((uint8_t)(c & 0xFF));
    o.push_back((uint8_t)(c >> 8));
    return o;
}

static std::string getAddr(const uint8_t* p, bool& h) {
    std::string s;
    for (int i = 0; i < 6; i++) {
        char ch = (char)(p[i] >> 1);
        if (ch != ' ') s += ch;
    }
    int ssid = (p[6] >> 1) & 15;
    if (ssid) s += "-" + std::to_string(ssid);
    h = (p[6] & 0x80) != 0;
    return s;
}

bool decodeFrame(const uint8_t* b, size_t n, Frame& f) {
    if (n < 18) return false;
    if (fcs(b, n - 2) != (uint16_t)(b[n - 2] | (b[n - 1] << 8))) return false;
    size_t p = 0;
    int na = 0;
    f = Frame();
    while (true) {
        if (p + 7 > n - 2) return false;
        bool h;
        std::string a = getAddr(b + p, h);
        for (char ch : a) if (!isalnum((unsigned char)ch) && ch != '-') return false;
        if (a.empty()) return false;
        if (na == 0) f.dst = a;
        else if (na == 1) f.src = a;
        else f.path.push_back(a + (h ? "*" : ""));
        bool last = b[p + 6] & 1;
        p += 7;
        na++;
        if (last) break;
        if (na >= 10) return false;
    }
    if (na < 2 || p + 2 > n - 2 || b[p] != 0x03 || b[p + 1] != 0xF0) return false;
    p += 2;
    f.info.assign((const char*)b + p, n - 2 - p);
    return true;
}

std::string tnc2(const Frame& f) {
    std::string s = f.src + ">" + f.dst;
    for (const auto& d : f.path) s += "," + d;
    return s + ":" + f.info;
}

// ---------------------------------------------------------------- modulator
void AfskMod::start(const std::vector<std::vector<uint8_t>>& frames, int rate, int txDelayMs) {
    rate_ = rate;
    bits_.clear();
    bit_ = 0;
    phase_ = 0;
    sub_ = 0;
    std::vector<uint8_t> raw;                                     // HDLC bits before NRZI
    auto flag = [&] { for (int i = 0; i < 8; i++) raw.push_back((0x7E >> i) & 1); };
    int pre = txDelayMs * 1200 / 1000 / 8;
    if (pre < 4) pre = 4;
    for (int i = 0; i < pre; i++) flag();
    for (const auto& fr : frames) {
        int ones = 0;
        for (uint8_t by : fr)
            for (int i = 0; i < 8; i++) {
                int b = (by >> i) & 1;
                raw.push_back((uint8_t)b);
                if (b) { if (++ones == 5) { raw.push_back(0); ones = 0; } }
                else ones = 0;
            }
        flag();
    }
    flag();
    flag();
    uint8_t tone = 1;                                             // NRZI: a 0 changes the tone, a 1 keeps it
    for (uint8_t b : raw) {
        if (!b) tone ^= 1;
        bits_.push_back(tone);
    }
}

float AfskMod::next() {
    if (bit_ >= bits_.size()) return 0;
    double f = bits_[bit_] ? 1200.0 : 2200.0;
    phase_ += 2 * kPi * f / rate_;
    if (phase_ > 2 * kPi) phase_ -= 2 * kPi;
    sub_ += 1200.0 / rate_;
    if (sub_ >= 1) { sub_ -= 1; bit_++; }
    return (float)sin(phase_);
}

// ---------------------------------------------------------------- demodulator
AfskDemod::AfskDemod() {
    sl_[0].balance = 1.0f;
    sl_[1].balance = 0.5f;                                        // space (2200 Hz) weaker than mark: de-emphasised audio
    sl_[2].balance = 2.0f;                                        // space stronger: pre-emphasised
}

void AfskDemod::feed(const float* s, size_t n, std::vector<std::vector<uint8_t>>& out) {
    const double wm = 2 * kPi * 1200 / 12000.0, ws = 2 * kPi * 2200 / 12000.0;
    for (size_t k = 0; k < n; k++) {
        // Band-pass 900-2500 Hz (two one-pole sections: high-pass then low-pass) to drop hum and hiss.
        float x = s[k];
        float hp = 0.82f * (bp_[1] + x - bp_[0]);
        bp_[0] = x;
        bp_[1] = hp;
        bp_[2] += 0.75f * (hp - bp_[2]);
        x = bp_[2];
        ph_ += 1;
        if (ph_ > 12000 * 100) ph_ -= 12000 * 100;                 // whole seconds keep both tones in phase
        float cm = (float)cos(wm * ph_), sm = (float)sin(wm * ph_), cs = (float)cos(ws * ph_), ss = (float)sin(ws * ph_);
        float a = x * cm, b = x * sm, c = x * cs, d = x * ss;
        smi_ += a - mi_[hi_]; smq_ += b - mq_[hi_]; ssi_ += c - si_[hi_]; ssq_ += d - sq_[hi_];
        mi_[hi_] = a; mq_[hi_] = b; si_[hi_] = c; sq_[hi_] = d;
        hi_ = (hi_ + 1) % W;
        float M = smi_ * smi_ + smq_ * smq_, S = ssi_ * ssi_ + ssq_ * ssq_;
        if (dcd_ > 0) dcd_--;
        for (auto& sl : sl_) {
            float dd = M - sl.balance * S;
            if ((dd > 0) != (sl.lastD > 0)) sl.phase -= (sl.phase - 0.5f) * 0.3f;   // tone change: pull to mid-bit
            sl.lastD = dd;
            sl.phase += 1.0f / W;
            if (sl.phase >= 1.0f) {
                sl.phase -= 1.0f;
                int tone = dd > 0 ? 1 : 0;
                int b = tone == sl.lastBit ? 1 : 0;              // NRZI
                sl.lastBit = tone;
                bit(sl, b, out);
            }
        }
        if (!recent_.empty() && dcd_ == 0) recent_.clear();
    }
}

void AfskDemod::bit(Slicer& s, int b, std::vector<std::vector<uint8_t>>& out) {
    auto acc = [&](int v) {
        s.cur = (uint8_t)((s.cur >> 1) | (v << 7));
        if (++s.nbits == 8) {
            s.nbits = 0;
            if (s.inFrame) {
                s.bytes.push_back(s.cur);
                if (s.bytes.size() > 340) { s.inFrame = false; s.bytes.clear(); }
                else if (s.bytes.size() > 2) dcd_ = 1200;
            }
        }
    };
    if (b) {
        if (++s.ones >= 7) { s.inFrame = false; s.bytes.clear(); s.ones = 7; return; }   // abort / idle
        acc(1);
        return;
    }
    if (s.ones == 6) {                                            // a flag: 0111 1110
        // The flag's first 0 and six 1s were taken as data: a whole frame leaves 7 extra bits.
        if (s.inFrame && s.nbits == 7 && s.bytes.size() >= 18) {
            uint16_t c = fcs(s.bytes.data(), s.bytes.size() - 2);
            if (c == (uint16_t)(s.bytes[s.bytes.size() - 2] | (s.bytes[s.bytes.size() - 1] << 8))) {
                bool dup = false;
                for (const auto& r : recent_) if (r == s.bytes) dup = true;
                if (!dup) {
                    out.push_back(s.bytes);
                    recent_.push_back(s.bytes);
                }
                dcd_ = 1200;
            }
        }
        s.inFrame = true;
        s.bytes.clear();
        s.nbits = 0;
        s.cur = 0;
        s.ones = 0;
        if (dcd_ < 600) dcd_ = 600;
        return;
    }
    if (s.ones == 5) { s.ones = 0; return; }                      // a stuffed 0
    s.ones = 0;
    acc(0);
}

// ---------------------------------------------------------------- APRS formats
static bool num(const std::string& s, size_t p, size_t n, double& v) {
    if (p + n > s.size()) return false;
    std::string t = s.substr(p, n);
    for (auto& ch : t) {
        if (ch == ' ') ch = '0';                                  // position ambiguity
        if (!isdigit((unsigned char)ch) && ch != '.') return false;
    }
    v = atof(t.c_str());
    return true;
}

static bool uncompressed(const std::string& s, size_t p, Position& out) {
    if (p + 19 > s.size()) return false;
    double la, lo;
    if (s[p + 4] != '.' || s[p + 14] != '.') return false;
    if (!num(s, p, 7, la) || !num(s, p + 9, 8, lo)) return false;
    char ns = s[p + 7], ew = s[p + 17];
    if ((ns != 'N' && ns != 'S') || (ew != 'E' && ew != 'W')) return false;
    double lat = (int)(la / 100) + fmod(la, 100.0) / 60.0, lon = (int)(lo / 100) + fmod(lo, 100.0) / 60.0;
    if (ns == 'S') lat = -lat;
    if (ew == 'W') lon = -lon;
    if (fabs(lat) > 90 || fabs(lon) > 180) return false;
    out.lat = lat;
    out.lon = lon;
    out.table = s[p + 8];
    out.symbol = s[p + 18];
    out.comment = s.substr(p + 19);
    return true;
}

static bool compressed(const std::string& s, size_t p, Position& out) {
    if (p + 13 > s.size()) return false;
    for (int i = 1; i <= 8; i++) if (s[p + i] < 33 || s[p + i] > 124) return false;
    double y = 0, x = 0;
    for (int i = 0; i < 4; i++) { y = y * 91 + (s[p + 1 + i] - 33); x = x * 91 + (s[p + 5 + i] - 33); }
    out.table = s[p];
    out.lat = 90 - y / 380926.0;
    out.lon = -180 + x / 190463.0;
    out.symbol = s[p + 9];
    out.comment = s.substr(p + 13);
    return fabs(out.lat) <= 90 && fabs(out.lon) <= 180;
}

static bool micE(const Frame& f, Position& out) {
    std::string d = f.dst.substr(0, f.dst.find('-'));
    const std::string& s = f.info;
    if (d.size() != 6 || s.size() < 9) return false;
    int dig[6];
    for (int i = 0; i < 6; i++) {
        char c = d[i];
        if (c >= '0' && c <= '9') dig[i] = c - '0';
        else if (c >= 'A' && c <= 'J') dig[i] = c - 'A';
        else if (c >= 'P' && c <= 'Y') dig[i] = c - 'P';
        else if (c == 'K' || c == 'L' || c == 'Z') dig[i] = 0;
        else return false;
    }
    double lat = dig[0] * 10 + dig[1] + (dig[2] * 10 + dig[3] + (dig[4] * 10 + dig[5]) / 100.0) / 60.0;
    bool north = d[3] >= 'P' && d[3] <= 'Z', off = d[4] >= 'P' && d[4] <= 'Z', west = d[5] >= 'P' && d[5] <= 'Z';
    int deg = (unsigned char)s[1] - 28 + (off ? 100 : 0);
    if (deg >= 180 && deg <= 189) deg -= 80;
    else if (deg >= 190 && deg <= 199) deg -= 190;
    int mn = (unsigned char)s[2] - 28;
    if (mn >= 60) mn -= 60;
    int hu = (unsigned char)s[3] - 28;
    if (deg < 0 || deg > 179 || mn < 0 || hu < 0 || hu > 99) return false;
    double lon = deg + (mn + hu / 100.0) / 60.0;
    out.lat = north ? lat : -lat;
    out.lon = west ? -lon : lon;
    out.symbol = s[7];
    out.table = s[8];
    out.comment = s.substr(9);
    out.mice = true;
    return lat <= 90;
}

bool parsePosition(const Frame& f, Position& p) {
    const std::string& s = f.info;
    if (s.empty()) return false;
    char t = s[0];
    if (t == '`' || t == '\'' || t == 0x1c || t == 0x1d) return micE(f, p);
    size_t at;
    if (t == '!' || t == '=') at = 1;
    else if (t == '/' || t == '@') at = 8;
    else return false;
    if (at >= s.size()) return false;
    if (isdigit((unsigned char)s[at]) || s[at] == ' ') return uncompressed(s, at, p);
    return compressed(s, at, p);
}

static std::string latStr(double v, char pos, char neg, int degDigits) {
    char h = v >= 0 ? pos : neg;
    v = fabs(v);
    int deg = (int)v;
    double mn = (v - deg) * 60.0;
    if (mn >= 59.995) { deg++; mn = 0; }
    char b[16];
    if (degDigits == 2) snprintf(b, sizeof b, "%02d%05.2f%c", deg, mn, h);
    else snprintf(b, sizeof b, "%03d%05.2f%c", deg, mn, h);
    return b;
}

std::string encodePosition(double lat, double lon, char table, char symbol, const std::string& comment, bool messaging) {
    return std::string(messaging ? "=" : "!") + latStr(lat, 'N', 'S', 2) + table + latStr(lon, 'E', 'W', 3) + symbol + comment;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool parseMessage(const std::string& s, Message& m) {
    if (s.size() < 11 || s[0] != ':' || s[10] != ':') return false;
    m = Message();
    m.to = trim(s.substr(1, 9));
    std::string t = s.substr(11);
    while (!t.empty() && (t.back() == '\r' || t.back() == '\n')) t.pop_back();
    if (t.compare(0, 3, "ack") == 0 && t.size() <= 9) { m.isAck = true; m.id = t.substr(3); return !m.id.empty(); }
    if (t.compare(0, 3, "rej") == 0 && t.size() <= 9) { m.isRej = true; m.id = t.substr(3); return !m.id.empty(); }
    size_t br = t.rfind('{');
    if (br != std::string::npos && t.size() - br <= 8) {
        m.id = t.substr(br + 1);
        size_t close = m.id.find('}');                             // reply-ack form {MM}AA
        if (close != std::string::npos) m.id = m.id.substr(0, close);
        t = t.substr(0, br);
    }
    m.text = t;
    return !m.to.empty();
}

static std::string pad9(const std::string& s) { std::string t = s.substr(0, 9); t.resize(9, ' '); return t; }
std::string encodeMessage(const std::string& to, const std::string& text, const std::string& id) {
    return ":" + pad9(to) + ":" + text.substr(0, 67) + (id.empty() ? "" : "{" + id);
}
std::string encodeAck(const std::string& to, const std::string& id) { return ":" + pad9(to) + ":ack" + id; }

double distanceKm(double la1, double lo1, double la2, double lo2) {
    const double r = kPi / 180;
    double d = sin(la1 * r) * sin(la2 * r) + cos(la1 * r) * cos(la2 * r) * cos((lo2 - lo1) * r);
    if (d > 1) d = 1;
    if (d < -1) d = -1;
    return acos(d) * 6371.0;
}

int bearingDeg(double la1, double lo1, double la2, double lo2) {
    const double r = kPi / 180;
    double y = sin((lo2 - lo1) * r) * cos(la2 * r);
    double x = cos(la1 * r) * sin(la2 * r) - sin(la1 * r) * cos(la2 * r) * cos((lo2 - lo1) * r);
    int b = (int)lround(atan2(y, x) / r);
    return (b + 360) % 360;
}

bool gridCentre(const std::string& g0, double& lat, double& lon) {
    std::string g = g0;
    for (auto& c : g) c = (char)toupper((unsigned char)c);
    if (g.size() < 4 || g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R' || !isdigit((unsigned char)g[2]) || !isdigit((unsigned char)g[3])) return false;
    lon = (g[0] - 'A') * 20 - 180 + (g[2] - '0') * 2 + 1.0;
    lat = (g[1] - 'A') * 10 - 90 + (g[3] - '0') + 0.5;
    if (g.size() >= 6 && g[4] >= 'A' && g[4] <= 'X' && g[5] >= 'A' && g[5] <= 'X') {
        lon += -1.0 + (g[4] - 'A') * (2.0 / 24) + 1.0 / 24;
        lat += -0.5 + (g[5] - 'A') * (1.0 / 24) + 0.5 / 24;
    }
    return true;
}

}  // namespace aprs
