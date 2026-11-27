// APRS: Bell 202 AFSK 1200 baud modem, AX.25 UI frames and the common APRS packet formats
// (positions incl. compressed and Mic-E, messages and acks).
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace aprs {

struct Frame {
    std::string src, dst;                       // "AC9HP-9", "APZRDK"
    std::vector<std::string> path;              // "WIDE1-1", "W9ABC-1*" (* = this digipeater has repeated it)
    std::string info;
};

// AX.25 bytes of a UI frame (addresses, control 0x03, PID 0xF0, info) followed by its 16-bit FCS.
std::vector<uint8_t> encodeFrame(const Frame& f);
// Checks the FCS and decodes; false for anything that is not a well-formed UI frame.
bool decodeFrame(const uint8_t* b, size_t n, Frame& f);
std::string tnc2(const Frame& f);               // "AC9HP-9>APZRDK,WIDE1-1:!3946.12N/08609.51W-..."
uint16_t fcs(const uint8_t* b, size_t n);
bool validCallSsid(const std::string& c);       // 1-6 letters/digits, optional -1..15

// Transmit: HDLC flags, bit stuffing, NRZI, continuous-phase 1200/2200 Hz tones.
class AfskMod {
public:
    void start(const std::vector<std::vector<uint8_t>>& frames, int rate, int txDelayMs);
    float next();                               // +-1.0
    bool done() const { return bit_ >= bits_.size(); }
    double seconds() const { return bits_.size() / 1200.0; }
private:
    std::vector<uint8_t> bits_;                 // one tone decision per bit (1 = mark 1200 Hz)
    size_t bit_ = 0;
    int rate_ = 48000;
    double phase_ = 0, sub_ = 0;
};

// Receive at 12 kHz: three slicers with different mark/space balance (FM radios tilt the audio),
// each with its own clock recovery and HDLC deframer. Good frames (FCS checked) come out once.
class AfskDemod {
public:
    AfskDemod();
    void feed(const float* s, size_t n, std::vector<std::vector<uint8_t>>& out);
    bool busy() const { return dcd_ > 0; }      // a packet seems to be on the channel
private:
    struct Slicer {
        float balance = 1;
        float lastD = 0, phase = 0;
        int lastBit = 0;
        uint8_t reg = 0;                        // last 8 raw bits (flag detection)
        int ones = 0;
        bool inFrame = false;
        std::vector<uint8_t> bytes;
        uint8_t cur = 0;
        int sinceFlag = 1000, flagRun = 0;      // bits since the last flag; flags in a row (a real preamble)
        bool pre = false;                       // this frame followed a real preamble (or a good frame)
        int nbits = 0;
        int good = 0;                           // flags or frames recently: carrier detect
    };
    static const int W = 10;                    // samples per bit at 12 kHz
    float hist_[W] = {};
    int hi_ = 0;
    float mi_[W] = {}, mq_[W] = {}, si_[W] = {}, sq_[W] = {};
    float smi_ = 0, smq_ = 0, ssi_ = 0, ssq_ = 0;
    double ph_ = 0;
    float bp_[4] = {};                          // band-pass state
    Slicer sl_[3];
    int dcd_ = 0;
    std::vector<std::vector<uint8_t>> recent_;  // dedupe between slicers
    void bit(Slicer& s, int b, std::vector<std::vector<uint8_t>>& out);
};

// --- APRS packets ---
struct Position {
    double lat = 0, lon = 0;
    char table = '/', symbol = '-';
    std::string comment;
    bool mice = false;
};
bool parsePosition(const Frame& f, Position& p);
// "!" (or "=" when the station takes messages) + uncompressed position + symbol + comment.
std::string encodePosition(double lat, double lon, char table, char symbol, const std::string& comment, bool messaging);

struct Message {
    std::string to, text, id;
    bool isAck = false, isRej = false;
};
bool parseMessage(const std::string& info, Message& m);
std::string encodeMessage(const std::string& to, const std::string& text, const std::string& id);
std::string encodeAck(const std::string& to, const std::string& id);

// Distance (km) and initial bearing (degrees) between two points.
double distanceKm(double lat1, double lon1, double lat2, double lon2);
int bearingDeg(double lat1, double lon1, double lat2, double lon2);
// Centre of a 4- or 6-character grid square.
bool gridCentre(const std::string& grid, double& lat, double& lon);

}  // namespace aprs
