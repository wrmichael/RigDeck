// The application core: ties radio control, audio, the FT8/FT4/PSK31 modems, the QSO sequencer,
// logging and the web API together. Everything here is portable C++; the ESP-IDF glue lives in main/.
#pragma once
#include <string>
#include <functional>
#include <map>
#include <deque>
#include <set>
#include <vector>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include "settings.h"
#include "civ.h"
#include "dsp.h"
#include "ftx.h"
#include "qso.h"
#include "psk31.h"
#include "adif.h"
#include "nmea.h"
#include "cwx.h"

enum DigiMode { DM_FT8 = 0, DM_FT4 = 1, DM_PSK31 = 2, DM_CW = 3 };   // DM_CW = "FT-CW": FT8-style contacts in Morse (cwx.h)

uint64_t parseDialFreq(const std::string& text);

class App {
public:
    App();
    void begin(const std::string& dataDir);        // loads settings and the log

    Settings cfg;
    Radio radio;
    Sequencer seq;

    // --- audio thread -------------------------------------------------------------------------
    void rxAudio48k(const int16_t* mono, size_t n);
    bool txActive() const { return txOn_.load(); }
    float txLevelNow() const;                       // TX audio level in use, % (automatic when cfg.autoAlc)
    void txAudio48k(int16_t* mono, size_t n);

    // --- worker threads (each call blocks forever) --------------------------------------------
    void decodeLoop();
    void netLoop();
    bool decodeOnce();                              // host tests: run one queued decode, if any
    bool netOnce();                                 // host tests: send one queued log record, if any

    // --- control thread, every 10-20 ms -------------------------------------------------------
    void tick();

    // --- web API ------------------------------------------------------------------------------
    std::string stateJson(uint32_t sinceDecode, uint32_t sincePsk, bool withCfg);
    std::string spectrumJson();
    std::string command(const std::string& c, const std::map<std::string, std::string>& args);
    std::string expandMacro(const std::string& macro, const std::string& call, const std::string& rst) const;
    std::string logPath() const { return dir_ + "/log.adi"; }          // current log: cleared by "clearlog"
    std::string archivePath() const { return dir_ + "/archive.adi"; }  // every QSO ever logged

    // --- on-device display --------------------------------------------------------------------
    // Reading state does not count as "an operator is present"; command() does (TX watchdog).
    struct UiState {
        int64_t utc = 0;
        bool clkOk = false, ser = false, aud = false, civ = false, ptt = false;
        bool txEn = false, txOn = false, tune = false, wd = false;
        int mode = 0, op = 0, rmode = -1, data = -1, wpm = -1, smeter = 0, po = 0, swr = 0, alc = 0;
        int slotLen = 15000, slotT = 0, txMsg = 0, txPar = 0, txFreq = 1500, tries = 0;
        int rptSent = -99, rptRcvd = -99, lvlDb = -99, decodeMs = 0, logN = 0, logAll = 0, netOk = 0, netFail = 0;
        uint64_t freq = 0;
        uint32_t decId = 0;
        std::string clkSrc, band, txText, nextText, dx, grid, status, net, call, myGrid, model;
        std::string staState, staIp, staReason;
        int staRssi = 0, txLvl = 0;
    };
    struct UiDecode { uint32_t id; int64_t slotMs; int snr; float dt; int freq; std::string text; bool cq, toMe; };
    void uiState(UiState& s);
    void uiDecodes(uint32_t since, std::vector<UiDecode>& out);
    void uiSpectrum(uint8_t out[Spectrum::BINS], uint32_t& seq) { spec_.snapshot(out, seq); }

    struct UiLogEntry { std::string when, call, grid, band, mode, sent, rcvd; };
    void uiLogTail(size_t max, std::vector<UiLogEntry>& out);
    bool uiWorked(const std::string& call);                        // already in the log on the band the radio is on      // newest first, from the current log file

    // --- GPS time ------------------------------------------------------------------------------
    // Bytes from the radio's second USB serial port (NMEA passed through from a GPS on DATA1).
    void gpsRx(const uint8_t* data, size_t n);
    std::atomic<int64_t> gpsLastMono{-1000000};      // when the clock was last set from GPS
    // A GPS module on the board's header: NMEA bytes, and the PPS edge (low 32 bits of monoMs, written
    // from an interrupt, so 32-bit to stay lock-free on the ESP32).
    void gpsModuleRx(const uint8_t* data, size_t n);
    std::atomic<uint32_t> ppsMono32{0};
    std::atomic<uint32_t> ppsCount{0};

    // --- platform status / requests -----------------------------------------------------------
    std::atomic<bool> usbSerial{false}, usbAudio{false};
    std::atomic<bool> rebootReq{false};
    std::atomic<bool> ntpSynced{false};
    std::string fwVersion = "dev";
    void (*logFn)(const char* line) = nullptr;      // platform log (serial monitor on the ESP32)                  // set by the platform (version.txt on the ESP32)
    bool (*haveRam)(size_t bytes) = nullptr;         // platform check before the big decoder allocations
    std::atomic<bool> memFail{false};
    void setNetInfo(const std::string& s);
    // Home-network (station) link, reported by the platform and controlled from Settings.
    struct StaStatus { std::string state = "off", ssid, ip, reason; int rssi = 0; };
    void setStaStatus(const StaStatus& s);
    StaStatus staStatus();
    std::function<void(bool connect)> wifiControl;    // set by the platform: (re)join with saved settings / leave
    int mode() const { return mode_.load(); }

private:
    int64_t slotLen() const { return mode_.load() == DM_FT4 ? 7500 : 15000; }
    void ftxRx(const float* s, size_t n);
    void syncSeq();
    void setMode(int m);
    void startTx(int gen);
    void stopTx(const char* why);
    void logQso(const QsoRecord& q);
    void status(const std::string& s);

    std::string dir_;
    std::mutex mtx_;                                // sequencer, decode list, log list, status text
    std::atomic<int> mode_{DM_FT8};

    // receive
    Decimator4 dec_;
    Spectrum spec_;
    FtxMonitor mon_[2];
    CwMonitor cwMon_[2];
    int cwWpmInUse_ = 0;
    bool jobCw_ = false;
    std::atomic<bool> reinit_{true};
    int64_t curSlot_ = -1;
    bool slotRx_ = false, triggered_ = false;
    std::atomic<int> rxPeak_{0};
    Psk31Rx pskRx_;
    std::string pskText_;
    uint32_t pskSeq_ = 0;
    std::atomic<int> pskFreq_{1000};
    std::atomic<bool> pskFreqDirty_{true};

    // decode hand-off
    std::mutex jobMtx_;
    std::condition_variable jobCv_;
    int64_t jobSlot_ = -1;
    bool jobPending_ = false;
    std::atomic<bool> decoding_{false};
    std::deque<Decode> decodes_;
    uint32_t nextId_ = 0;
    std::atomic<int> decodeMs_{0};

    // transmit
    enum { GEN_FTX, GEN_TUNE, GEN_PSK };
    enum { ST_IDLE, ST_TX };
    int txState_ = ST_IDLE, gen_ = GEN_FTX;
    std::atomic<bool> txOn_{false}, genStarted_{false}, genDone_{false}, haltReq_{false}, tuneReq_{false};
    std::atomic<int64_t> genDoneMono_{0}, txStartUtc_{0}, txSlot_{-1};
    int64_t pttOnMono_ = 0, pttOffRepeat_ = 0, maxTxMs_ = 0;
    FtxSynth synth_;
    CwSynth cwSynth_;
    bool txCw_ = false;                               // the transmission in progress is Morse
    Psk31Tx pskTx_;
    std::string pskPending_;
    float tunePhase_ = 0;
    std::string txText_, lastTx_;

    // log
    std::deque<QsoRecord> recent_;
    std::set<std::string> worked_;
    int logCount_ = 0, archiveCount_ = 0;
    bool archiveBehind_ = false;                     // a QSO is in the current log but not in the archive
    int scanLog(const std::string& path);
    std::mutex netMtx_;
    std::condition_variable netCv_;
    std::deque<std::string> netQ_;
    std::atomic<int> netOk_{0}, netFail_{0};

    NmeaTime nmea_, nmeaMod_;
    float gpsErr_ = 0;
    void gpsTime(int64_t utcNow, int64_t mono, const char* src);
    // automatic TX audio level (ALC)
    struct AlcLevel { float level = 0, ceiling = 0; };
    std::map<std::string, AlcLevel> alcLevel_;
    std::atomic<float> txGain_{50.0f};
    std::string alcBand_;
    uint32_t alcSeen_ = 0;
    int alcMax_ = 0, poMax_ = 0, alcReads_ = 0;
    bool alcCut_ = false;
    void alcStartTx();
    void alcTick(int64_t mono);
    void alcEndTx();
    uint32_t ppsUsed_ = 0;
    std::string status_, netInfo_;
    StaStatus sta_;
    int64_t lastUiMono_ = 0, bandReqHz_ = 0;
    bool watchdogTripped_ = false, wasConn_ = false;
};
