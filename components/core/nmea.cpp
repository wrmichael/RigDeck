#include <cmath>
#include "nmea.h"
#include <cstring>
#include <cstdlib>
#include <initializer_list>

int64_t NmeaTime::daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

bool NmeaTime::feed(const uint8_t* data, size_t n, int64_t arrival) {
    bool got = false;
    for (size_t i = 0; i < n; i++) {
        char c = (char)data[i];
        if (c == '$') { in_ = true; len_ = 0; curStart_ = arrival; continue; }
        if (!in_) continue;
        if (c == '\r' || c == '\n') {
            buf_[len_] = 0;
            in_ = false;
            if (parse()) { start_ = curStart_; got = true; }
            continue;
        }
        if (len_ + 1 >= sizeof buf_) { in_ = false; continue; }
        buf_[len_++] = c;
    }
    return got;
}

static int two(const char* p) { return (p[0] - '0') * 10 + (p[1] - '0'); }
static bool digits(const char* p, int n) { for (int i = 0; i < n; i++) if (p[i] < '0' || p[i] > '9') return false; return true; }

// "3946.123","N","08609.512","W" -> degrees
void NmeaTime::takePos(const char* la, const char* ns, const char* lo, const char* ew) {
    if (strlen(la) < 4 || strlen(lo) < 5 || !ns[0] || !ew[0]) return;
    double a = atof(la), o = atof(lo);
    double lat = (int)(a / 100) + fmod(a, 100.0) / 60.0, lon = (int)(o / 100) + fmod(o, 100.0) / 60.0;
    if (ns[0] == 'S') lat = -lat;
    if (ew[0] == 'W') lon = -lon;
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) return;
    lat_ = lat; lon_ = lon; posOk_ = true;
}

bool NmeaTime::parse() {
    // buf_ holds e.g. "GPRMC,163900.00,A,3946.1,N,08609.5,W,0.0,0.0,101126,,,A*6B"
    char* star = strchr(buf_, '*');
    if (!star || strlen(star) < 3) return false;
    unsigned char sum = 0;
    for (char* p = buf_; p < star; p++) sum ^= (unsigned char)*p;
    if ((unsigned char)strtol(star + 1, nullptr, 16) != sum) return false;
    *star = 0;
    if (strlen(buf_) < 6) return false;
    const char* type = buf_ + 2;                       // skip the talker id (GP, GN, ...)
    const char* f[16];
    int nf = 0;
    f[nf++] = buf_;
    for (char* p = buf_; *p && nf < 16; p++) if (*p == ',') { *p = 0; f[nf++] = p + 1; }
    int hh, mm, ss, ms = 0, day, mon, year;
    if (!strncmp(type, "RMC", 3)) {
        if (nf < 10 || f[2][0] != 'A') return false;   // 'V' = receiver has no fix: time may be wrong
        if (strlen(f[1]) < 6 || !digits(f[1], 6) || strlen(f[9]) != 6 || !digits(f[9], 6)) return false;
        day = two(f[9]); mon = two(f[9] + 2); year = 2000 + two(f[9] + 4);
        takePos(f[3], f[4], f[5], f[6]);
    } else if (!strncmp(type, "ZDA", 3)) {
        if (nf < 5 || strlen(f[1]) < 6 || !digits(f[1], 6)) return false;
        day = atoi(f[2]); mon = atoi(f[3]); year = atoi(f[4]);
    } else if (!strncmp(type, "GGA", 3)) {
        // fix quality in field 6: 0 = no fix
        if (hint_ < 1700000000000LL || nf < 7 || strlen(f[1]) < 6 || !digits(f[1], 6) || f[6][0] < '1') return false;
        int th = two(f[1]), tm = two(f[1] + 2), ts = two(f[1] + 4), tms = 0;
        if (th > 23 || tm > 59 || ts > 60) return false;
        if (f[1][6] == '.') { int scale = 100; for (const char* p = f[1] + 7; *p >= '0' && *p <= '9' && scale > 0; p++, scale /= 10) tms += (*p - '0') * scale; }
        int64_t tod = ((th * 60 + tm) * 60 + ts) * 1000LL + tms;
        int64_t day0 = (hint_ / 86400000LL) * 86400000LL;
        int64_t best = day0 + tod;                      // the same time of day yesterday, today or tomorrow: nearest the hint
        for (int64_t c : {best - 86400000LL, best + 86400000LL})
            if (llabs(c - hint_) < llabs(best - hint_)) best = c;
        if (llabs(best - hint_) > 6 * 3600000LL) return false;
        if (nf >= 6) takePos(f[2], f[3], f[4], f[5]);
        utc_ = best;
        return true;
    } else return false;
    hh = two(f[1]); mm = two(f[1] + 2); ss = two(f[1] + 4);
    if (f[1][6] == '.') { int scale = 100; for (const char* p = f[1] + 7; *p >= '0' && *p <= '9' && scale > 0; p++, scale /= 10) ms += (*p - '0') * scale; }
    if (year < 2024 || year > 2099 || mon < 1 || mon > 12 || day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 60) return false;
    utc_ = (daysFromCivil(year, mon, day) * 86400 + hh * 3600 + mm * 60 + ss) * 1000 + ms;
    return true;
}
