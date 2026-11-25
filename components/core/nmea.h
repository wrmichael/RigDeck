// Minimal NMEA 0183 reader: extracts UTC date and time from RMC and ZDA sentences.
#pragma once
#include <cstdint>
#include <cstddef>

class NmeaTime {
public:
    // Feed received bytes; arrivalMonoMs is the monotonic time at which this chunk arrived.
    // Returns true when a valid time sentence was completed; then utcMs() is the time the
    // sentence states and sentenceStartMono() is when its first character arrived.
    bool feed(const uint8_t* data, size_t n, int64_t arrivalMonoMs);
    int64_t utcMs() const { return utc_; }
    int64_t sentenceStartMono() const { return start_; }
    static int64_t daysFromCivil(int y, int m, int d);     // days since 1970-01-01
    // GGA carries the time of day but no date (the IC-705 sends only GGA by default). With a hint
    // (the current clock, to within a few hours) GGA is accepted and given the nearest date.
    void setDateHint(int64_t utcMs) { hint_ = utcMs; }
    // Position from the last RMC or GGA sentence with a fix (degrees, north and east positive).
    bool hasPos() const { return posOk_; }
    double lat() const { return lat_; }
    double lon() const { return lon_; }
private:
    bool parse();
    char buf_[96];
    size_t len_ = 0;
    bool in_ = false;
    int64_t curStart_ = 0, start_ = 0, utc_ = 0, hint_ = 0;
    bool posOk_ = false;
    double lat_ = 0, lon_ = 0;
    void takePos(const char* la, const char* ns, const char* lo, const char* ew);
};
