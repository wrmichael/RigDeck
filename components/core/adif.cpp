#include "adif.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cctype>


const char* bandName(uint64_t hz) {
    static const struct { uint32_t lo, hi; const char* n; } b[] = {
        {1800, 2000, "160m"}, {3500, 4000, "80m"}, {5060, 5450, "60m"}, {7000, 7300, "40m"},
        {10100, 10150, "30m"}, {14000, 14350, "20m"}, {18068, 18168, "17m"}, {21000, 21450, "15m"},
        {24890, 24990, "12m"}, {28000, 29700, "10m"}, {50000, 54000, "6m"}, {70000, 71000, "4m"},
        {144000, 148000, "2m"}, {420000, 450000, "70cm"}};
    uint32_t k = (uint32_t)(hz / 1000);
    for (auto& e : b) if (k >= e.lo && k <= e.hi) return e.n;
    return "";
}

static void field(std::string& s, const char* name, const std::string& v) {
    if (v.empty()) return;
    char h[40];
    snprintf(h, sizeof h, "<%s:%u>", name, (unsigned)v.size());
    s += h;
    s += v;
    s += ' ';
}

static void dateTime(int64_t ms, std::string& d, std::string& t) {
    time_t s = (time_t)(ms / 1000);
    struct tm tmv;
    gmtime_r(&s, &tmv);
    char b[16];
    strftime(b, sizeof b, "%Y%m%d", &tmv); d = b;
    strftime(b, sizeof b, "%H%M%S", &tmv); t = b;
}

std::string adifRecord(const QsoRecord& q) {
    std::string s, d, t;
    field(s, "CALL", q.call);
    field(s, "GRIDSQUARE", q.grid);
    field(s, "MODE", q.mode);
    field(s, "SUBMODE", q.submode);
    field(s, "RST_SENT", q.rstSent);
    field(s, "RST_RCVD", q.rstRcvd);
    dateTime(q.startMs, d, t);
    field(s, "QSO_DATE", d);
    field(s, "TIME_ON", t);
    dateTime(q.endMs ? q.endMs : q.startMs, d, t);
    field(s, "QSO_DATE_OFF", d);
    field(s, "TIME_OFF", t);
    field(s, "BAND", bandName(q.freqHz));
    char f[24];
    snprintf(f, sizeof f, "%.6f", q.freqHz / 1e6);
    field(s, "FREQ", f);
    field(s, "STATION_CALLSIGN", q.myCall);
    field(s, "MY_GRIDSQUARE", q.myGrid);
    field(s, "OPERATOR", q.op);
    field(s, "MY_SIG", q.mySig);                           // optional programme name (WWFF, POTA...)
    field(s, "MY_SIG_INFO", q.mySigInfo);
    field(s, "NAME", q.name);
    field(s, "COMMENT", q.comment);
    s += "<EOR>";
    return s;
}

std::string adifHeader() {
    return "IC-7100 FT8 controller log\n<ADIF_VER:5>3.1.4 <PROGRAMID:10>IC7100-FT8 <EOH>\n";
}

std::string adifField(const std::string& rec, const char* name) {
    std::string up;
    for (char c : rec) up += (char)toupper((unsigned char)c);
    std::string key = std::string("<") + name + ":";
    size_t p = up.find(key);
    if (p == std::string::npos) return "";
    size_t e = rec.find('>', p);
    if (e == std::string::npos) return "";
    int len = atoi(rec.c_str() + p + key.size());
    if (len <= 0 || e + 1 + (size_t)len > rec.size()) return "";
    return rec.substr(e + 1, len);
}

// WSJT-X NetworkMessage type 12 "Logged ADIF": big-endian magic, schema, type, then two
// length-prefixed UTF-8 strings (client id, ADIF text with a header).
std::vector<uint8_t> wsjtxLoggedAdif(const std::string& id, const std::string& record) {
    std::vector<uint8_t> p;
    auto u32 = [&](uint32_t v) { p.push_back(v >> 24); p.push_back(v >> 16); p.push_back(v >> 8); p.push_back(v); };
    auto str = [&](const std::string& s) { u32((uint32_t)s.size()); p.insert(p.end(), s.begin(), s.end()); };
    u32(0xADBCCBDA);
    u32(2);
    u32(12);
    str(id);
    str("\n<adif_ver:5>3.1.0\n<programid:6>WSJT-X\n<EOH>\n" + record);
    return p;
}
