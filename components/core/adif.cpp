#include "adif.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cctype>
#include <cstring>
#include <utility>


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
    return "RigDeck log\n<ADIF_VER:5>3.1.4 <PROGRAMID:7>RigDeck <EOH>\n";
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

// WSJT-X NetworkMessage (Qt QDataStream, big-endian): magic, schema, type, client id, then the
// fields of that type. Strings are a 32-bit byte count and UTF-8. WSJT-X sends a heartbeat now and
// then, and for each logged contact first type 5 "QSO Logged" and then type 12 "Logged ADIF";
// RigDeck does the same, because some programs read only one of the two.
namespace {
struct Wsjtx {
    std::vector<uint8_t> p;
    void u8(uint8_t v) { p.push_back(v); }
    void u32(uint32_t v) { p.push_back(v >> 24); p.push_back(v >> 16); p.push_back(v >> 8); p.push_back(v); }
    void u64(uint64_t v) { u32((uint32_t)(v >> 32)); u32((uint32_t)v); }
    void str(const std::string& s) { u32((uint32_t)s.size()); p.insert(p.end(), s.begin(), s.end()); }
    void time(int64_t utcMs) {                       // QDateTime: Julian day (qint64), ms of day, 1 = UTC
        int64_t day = utcMs >= 0 ? utcMs / 86400000 : (utcMs - 86399999) / 86400000;
        u64((uint64_t)(day + 2440588));
        u32((uint32_t)(utcMs - day * 86400000));
        u8(1);
    }
    Wsjtx(uint32_t type, const std::string& id) { u32(0xADBCCBDA); u32(3); u32(type); str(id); }
};
}

std::vector<uint8_t> wsjtxHeartbeat(const std::string& id, const std::string& version) {
    Wsjtx m(0, id);
    m.u32(3);                                        // highest schema we speak
    m.str(version);
    m.str("");                                       // revision
    return m.p;
}

std::vector<uint8_t> wsjtxQsoLogged(const std::string& id, const QsoRecord& q) {
    Wsjtx m(5, id);
    m.time(q.endMs);
    m.str(q.call);
    m.str(q.grid);
    m.u64(q.freqHz);
    m.str(q.submode.empty() ? q.mode : q.submode);   // WSJT-X says "FT4", not MFSK
    m.str(q.rstSent);
    m.str(q.rstRcvd);
    m.str("");                                       // TX power
    m.str(q.comment);
    m.str(q.name);
    m.time(q.startMs);
    m.str(q.op);
    m.str(q.myCall);
    m.str(q.myGrid);
    m.str("");                                       // exchange sent
    m.str("");                                       // exchange received
    m.str("");                                       // propagation mode
    return m.p;
}

std::vector<uint8_t> wsjtxLoggedAdif(const std::string& id, const std::string& record) {
    Wsjtx m(12, id);
    auto& p = m.p;
    auto str = [&](const std::string& s) { m.str(s); };
    str("\n<adif_ver:5>3.1.0\n<programid:6>WSJT-X\n<EOH>\n" + record);
    return p;
}

// ---- log management ---------------------------------------------------------------------------
AdifFields adifParse(const std::string& rec) {
    AdifFields out;
    size_t p = 0;
    while ((p = rec.find('<', p)) != std::string::npos) {
        size_t e = rec.find('>', p);
        if (e == std::string::npos) break;
        std::string tag = rec.substr(p + 1, e - p - 1);
        size_t c = tag.find(':');
        std::string name = tag.substr(0, c);
        for (auto& ch : name) ch = (char)toupper((unsigned char)ch);
        if (name == "EOR" || name == "EOH") break;
        if (c == std::string::npos) { p = e + 1; continue; }
        int len = atoi(tag.c_str() + c + 1);
        if (len < 0 || e + 1 + (size_t)len > rec.size()) break;
        out.emplace_back(name, rec.substr(e + 1, len));
        p = e + 1 + len;
    }
    return out;
}

std::string adifBuild(const AdifFields& f) {
    std::string s;
    for (auto& kv : f) {
        if (kv.second.empty()) continue;
        char h[48];
        snprintf(h, sizeof h, "<%s:%u>", kv.first.c_str(), (unsigned)kv.second.size());
        s += h;
        s += kv.second;
        s += ' ';
    }
    return s + "<EOR>";
}

bool adifSetField(AdifFields& f, const std::string& name, const std::string& value) {
    for (size_t i = 0; i < f.size(); i++) {
        if (f[i].first != name) continue;
        if (f[i].second == value) return false;
        if (value.empty()) f.erase(f.begin() + i);
        else f[i].second = value;
        return true;
    }
    if (value.empty()) return false;
    f.emplace_back(name, value);
    return true;
}

bool adifReadRecords(const std::string& path, std::vector<std::string>& recs) {
    recs.clear();
    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) return false;
    std::string line;
    int ch;
    while ((ch = fgetc(fp)) != EOF) {
        if (ch != '\n') { line += (char)ch; continue; }
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t n = line.size();
        if (n >= 5 && !strncasecmp(line.c_str() + n - 5, "<EOR>", 5) && !adifField(line, "CALL").empty()) recs.push_back(line);
        line.clear();
    }
    size_t n = line.size();
    if (n >= 5 && !strncasecmp(line.c_str() + n - 5, "<EOR>", 5) && !adifField(line, "CALL").empty()) recs.push_back(line);
    fclose(fp);
    return true;
}

bool adifWriteRecords(const std::string& path, const std::vector<std::string>& recs) {
    std::string tmp = path + ".tmp";
    FILE* fp = fopen(tmp.c_str(), "w");
    if (!fp) return false;
    bool ok = fputs(adifHeader().c_str(), fp) >= 0;
    for (auto& r : recs) ok = ok && fputs(r.c_str(), fp) >= 0 && fputc('\n', fp) != EOF;
    ok = (fclose(fp) == 0) && ok;
    if (!ok) { remove(tmp.c_str()); return false; }
    remove(path.c_str());                                 // SPIFFS rename does not replace an existing file
    return rename(tmp.c_str(), path.c_str()) == 0;
}

static int64_t adifTimeMs(const std::string& d, const std::string& t) {
    if (d.size() != 8 || t.size() < 4) return 0;
    int y = atoi(d.substr(0, 4).c_str()), m = atoi(d.substr(4, 2).c_str()), dd = atoi(d.substr(6, 2).c_str());
    int hh = atoi(t.substr(0, 2).c_str()), mi = atoi(t.substr(2, 2).c_str()), ss = t.size() >= 6 ? atoi(t.substr(4, 2).c_str()) : 0;
    y -= m <= 2;                                          // days from civil (Howard Hinnant)
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + dd - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + (int64_t)doe - 719468;
    return ((days * 24 + hh) * 60 + mi) * 60000LL + ss * 1000LL;
}

QsoRecord adifToQso(const std::string& rec) {
    QsoRecord q;
    q.call = adifField(rec, "CALL");
    q.grid = adifField(rec, "GRIDSQUARE");
    q.mode = adifField(rec, "MODE");
    q.submode = adifField(rec, "SUBMODE");
    q.rstSent = adifField(rec, "RST_SENT");
    q.rstRcvd = adifField(rec, "RST_RCVD");
    q.name = adifField(rec, "NAME");
    q.comment = adifField(rec, "COMMENT");
    q.freqHz = (uint64_t)(atof(adifField(rec, "FREQ").c_str()) * 1e6 + 0.5);
    q.startMs = adifTimeMs(adifField(rec, "QSO_DATE"), adifField(rec, "TIME_ON"));
    q.endMs = adifTimeMs(adifField(rec, "QSO_DATE_OFF"), adifField(rec, "TIME_OFF"));
    if (!q.endMs) q.endMs = q.startMs;
    return q;
}

void adifModeFromText(const std::string& text, std::string& mode, std::string& submode) {
    std::string t;
    for (char c : text) if (!isspace((unsigned char)c)) t += (char)toupper((unsigned char)c);
    mode = t; submode.clear();
    if (t == "USB" || t == "LSB") { mode = "SSB"; submode = t; }
    else if (t == "FT4") { mode = "MFSK"; submode = "FT4"; }
    else if (t == "PSK31" || t == "BPSK31") { mode = "PSK"; submode = "PSK31"; }
    else if (t == "JS8") { mode = "MFSK"; submode = "JS8"; }
}
