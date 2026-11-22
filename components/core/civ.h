// Radio control. Three protocols share one Radio object so the rest of the firmware does not care
// which radio is plugged in:
//   RP_CIV    Icom CI-V (IC-7100, IC-705): binary frames
//   RP_YAESU  Yaesu "new CAT" as spoken by the FTX-1: ASCII commands ending in ';'
//   RP_QMX    QRP Labs QMX / QMX+: Kenwood TS-480 style ASCII commands ending in ';'
//   RP_NONE   no CAT at all (a SignaLink or other plain USB sound card): the frequency is what the
//             band buttons set, and PTT is the sound card's VOX or a serial port's RTS / DTR line
//   RP_KENWOOD Kenwood / Elecraft text CAT (TS-2000, TS-480, TS-590, K2, K3): like the QMX's, with
//             TX; / RX; for PTT and Kenwood's mode numbers
//   Yaesu radios other than the FTX-1 (FTDX10, FT-991A, FT-710, FTDX101, FT-891) speak RP_YAESU too.
// Whatever the radio, the public state uses Icom's numbering (mode codes, command-14 level and
// command-16 function sub-codes); the text protocols are translated in cat.cpp.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <deque>
#include <functional>
#include <mutex>
#include <atomic>
#include <string>

struct CivFrame {
    uint8_t to = 0, from = 0, cmd = 0;
    std::vector<uint8_t> data;           // everything between the command byte and 0xFD
};

std::vector<uint8_t> civBuild(uint8_t to, uint8_t from, uint8_t cmd, const std::vector<uint8_t>& data = {});
void civFreqToBcd(uint64_t hz, uint8_t out[5]);
uint64_t civBcdToFreq(const uint8_t* b, size_t n);
int civBcdToInt(const uint8_t* b, size_t n);
const char* civModelName(uint8_t civAddr);            // radio model from its factory CI-V address          // big-endian BCD as used by level/meter values

class CivParser {
public:
    bool feed(uint8_t byte, CivFrame& out);            // true when a complete frame was assembled
private:
    std::vector<uint8_t> buf_;
    int pre_ = 0;
    bool in_ = false;
};

enum RadioProto { RP_CIV = 0, RP_YAESU = 1, RP_QMX = 2, RP_NONE = 3, RP_KENWOOD = 4 };
enum PttMethod { PTT_CAT = 0, PTT_RTS = 1, PTT_DTR = 2, PTT_VOX = 3 };

class Radio {
public:
    std::atomic<int> proto{RP_CIV};
    void setProto(int p);                              // also forgets everything learned from the previous radio
    const char* modelName() const;
    bool isText() const { int p = proto.load(); return p != RP_CIV && p != RP_NONE; }
    bool kenwoodish() const { int p = proto.load(); return p == RP_QMX || p == RP_KENWOOD; }
    // PTT by a serial port's RTS or DTR line (Digirig and similar interfaces) or by the sound card's
    // VOX instead of a CAT command. pttLine is set by the USB code: it drives the line.
    std::atomic<int> pttMethod{PTT_CAT};
    std::function<void(bool)> pttLine;

    // The radio's own CW keyer sends text (the radio must be in CW mode). Used by the logger macros.
    void sendCw(const std::string& text);
    void stopCw();
    void setKeySpeed(int wpm);
    std::atomic<int> keySpeed{-1};                     // wpm as last reported, -1 = unknown
    static size_t cwMax(int proto);                    // longest text one sendCw() can carry

    static constexpr uint8_t CTRL = 0xE0;              // our (controller) address
    std::function<bool(const uint8_t*, size_t)> write; // transport; returns false when the port is down
    uint8_t addr = 0x88;

    void onRx(const uint8_t* d, size_t n);             // may be called from the USB thread
    void poll();                                       // call every ~20 ms from the control thread
    void setFreq(uint64_t hz);
    void setUsbData();                                 // USB + DATA mode (USB-D), filter 1
    void ptt(bool on);                                 // sent ahead of anything queued
    void setRfPower(int percent);
    void requestId();

    // Front-panel style control (the "radio" screen). Values are read back by polling while
    // extPoll is set; an entry stays -1 until the radio has reported it (or if it rejects it).
    void setMode(uint8_t mode, uint8_t filter);        // 00 LSB 01 USB 02 AM 03 CW 04 RTTY 05 FM 17 DV; filter 1..3
    void setData(bool on);
    void setLevel(uint8_t sub, int percent);           // command 14: 01 AF 02 RF 03 SQL 06 NR 0A power 0B mic
    void setFunc(uint8_t sub, uint8_t value);          // command 16: 02 preamp 12 AGC 22 NB 40 NR 41 auto notch 44 comp 46 VOX
    void setAtt(uint8_t bcdDb);                        // command 11: 00 off, 12 or 20 = dB
    void vfo(uint8_t sub);                             // command 07: 00 A, 01 B, B0 exchange
    std::atomic<bool> extPoll{false};
    std::atomic<int> filter{-1}, att{-1};
    std::atomic<int> level[16];                        // by command-14 sub-code (0..0F), percent
    std::atomic<int> func[0x50];                       // by command-16 sub-code
    Radio();

    bool connected() const;
    std::atomic<uint64_t> freq{0};
    std::atomic<int> mode{-1}, dataMode{-1}, smeter{0}, po{0}, swr{0}, alc{0};
    std::atomic<uint32_t> alcSeq{0};                   // +1 for every ALC reading received
    std::atomic<bool> pttState{false};                 // what the radio last reported
    std::atomic<bool> pttWanted{false};
    std::atomic<int64_t> lastRxMono{-100000};
    std::atomic<int> ng{0};                            // count of FA (rejected) replies

private:
    void send(uint8_t cmd, const std::vector<uint8_t>& data, bool front = false);
    void handle(const CivFrame& f);
    // text CAT (cat.cpp)
    void sendText(const std::string& s, bool front = false);
    void textRx(char c);
    void textAnswer(const std::string& a);
    void textPoll();
    void tSetFreq(uint64_t hz);
    void tSetUsbData();
    void tSetRfPower(int percent);
    void tSetMode(uint8_t mode);
    void tSetData(bool on);
    void tSetLevel(uint8_t sub, int percent);
    void tSetFunc(uint8_t sub, uint8_t value);
    void tSetAtt(bool on);
    void tVfo(uint8_t sub);
    void tSendCw(const std::string& text);
    std::string line_;
    int pcHead_ = 0;                                   // Yaesu FTX-1: 1 = field head (10 W), 2 = SPA-1 (100 W)
    int yMode_ = 0;                                    // last raw mode character from a text radio
    int textId_ = 0;                                   // the radio's answer to ID; (model number)
    std::mutex mtx_;
    CivParser parser_;
    std::deque<std::vector<uint8_t>> q_;
    int64_t lastSend_ = 0, lastPoll_ = 0;
    int pollStep_ = 0, extStep_ = 0;
};
