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
    // Loads settings and the log. Settings live in dataDir; the log files in logDir (the microSD
    // card when one is in) or, if that is empty, in dataDir as well.
    void begin(const std::string& dataDir, const std::string& logDir = "");

    Settings cfg;
    Radio radio;
    Sequencer seq;

    // --- audio thread -------------------------------------------------------------------------
    void rxAudio48k(const int16_t* mono, size_t n);
    bool txActive() const { return txOn_.load(); }
    float txLevelNow() const;
    float alcLearned(const std::string& key) const { auto it = alcLevel_.find(key); return it == alcLevel_.end() ? 0 : it->second.level; }                       // TX audio level in use, % (automatic when cfg.autoAlc)
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
    void activity();                       // operator touched the screen: restarts the TX watchdog
    int wdLeftSec() const;                 // caller holds the lock; -1 = watchdog not counting
    std::string spectrumJson();
    std::string command(const std::string& c, const std::map<std::string, std::string>& args);
    std::string expandMacro(const std::string& macro, const std::string& call, const std::string& rst) const;
    // Log files are per station callsign ("AC9HP_log.adi"): changing the callsign switches logs.
    std::string logPath() const { return stationFile("log.adi"); }          // current log: cleared by "clearlog"
    std::string archivePath() const { return stationFile("archive.adi"); }  // every QSO ever logged
    std::string trashPath() const { return stationFile("deleted.adi"); }    // deleted contacts (Restore)
    std::string logFile(bool archive);                 // the same, safe from any thread (web download)
    std::string station();                             // file-name form of the station callsign

    // --- on-device display --------------------------------------------------------------------
    // Reading state does not count as "an operator is present"; command() does (TX watchdog).
    struct UiState {
        int64_t utc = 0;
        bool clkOk = false, ser = false, aud = false, civ = false, ptt = false;
        bool txEn = false, txOn = false, tune = false, wd = false;
        int mode = 0, op = 0, rmode = -1, data = -1, wpm = -1, smeter = 0, po = 0, swr = 0, alc = 0;
        int slotLen = 15000, slotT = 0, txMsg = 0, txPar = 0, txFreq = 1500, tries = 0;
        int rptSent = -99, rptRcvd = -99, lvlDb = -99, decodeMs = 0, logN = 0, logAll = 0, netOk = 0, netFail = 0, unsent = 0;
        uint64_t freq = 0;
        uint32_t decId = 0;
        std::string clkSrc, band, txText, nextText, dx, grid, status, net, call, myGrid, model;
        std::string staState, staIp, staReason, wchipVer;
        // log storage: where the log is, free space on each store, rough contacts left
        bool logSd = false, sdIn = false;
        uint64_t intTotal = 0, intUsed = 0, sdTotal = 0, sdUsed = 0;
        long left = -1;
        std::string station, sdInfo;
        int bright = 100, contrast = 100;
        bool waitNext = false;                         // POTA mode: a QSO ended, Next QSO starts the next
        int staRssi = 0, txLvl = 0;
    };
    struct UiDecode { uint32_t id; int64_t slotMs; int snr; float dt; int freq; std::string text; bool cq, toMe; };
    void uiState(UiState& s);
    void uiDecodes(uint32_t since, std::vector<UiDecode>& out);
    void uiSpectrum(uint8_t out[Spectrum::BINS], uint32_t& seq) { spec_.snapshot(out, seq); }

    struct UiLogEntry { std::string when, call, grid, band, mode, sent, rcvd, date, time, freq, name, comment; int idx = -1; };
    void uiLogTail(size_t max, std::vector<UiLogEntry>& out);
    // newest entry of "recently deleted" (idx -1 if none) and how many there are
    void uiTrashNewest(UiLogEntry& e, int& count);
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
    std::string otaUpd_;
    void (*logFn)(const char* line) = nullptr;      // platform log (serial monitor on the ESP32)                  // set by the platform (version.txt on the ESP32)
    bool (*haveRam)(size_t bytes) = nullptr;         // platform check before the big decoder allocations
    std::atomic<bool> memFail{false};
    // Storage: the platform reports size and use of the internal store (sd=false) or the card.
    bool (*fsInfo)(bool sd, uint64_t& total, uint64_t& used) = nullptr;
    std::string sdInfo;                                // set before begin(): "", or what the card is / why it is not used
    int (*sdFormat)(std::string& msg) = nullptr;       // format the card (0 ok; msg = what it is now / why not)
    void setNetInfo(const std::string& s);
    // Home-network (station) link, reported by the platform and controlled from Settings.
    struct StaStatus { std::string state = "off", ssid, ip, reason; int rssi = 0; };
    void setStaStatus(const StaStatus& s);
    // Wi-Fi chip (co-processor): firmware version ("" = unknown) and the result of the last update
    void setWifiChip(const std::string& ver, const std::string& upd);
    void setWifiChipUpd(const std::string& upd);
    void setOtaStatus(const std::string& msg);         // RigDeck firmware update progress / result
    std::string otaInfo;                               // set before begin(): which flash slot is running
    void note(const std::string& msg);                 // status line message from platform code
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
    // The last 150 decodes, in PSRAM: kept in internal RAM they took ~20 kB of it once the list was
    // full, and the microSD card's reads then failed for want of a DMA buffer.
    struct DecodeKept { uint32_t id; int64_t slotMs; int parity, snr; float dt, freq; PStr text; };
    std::deque<DecodeKept, PsAlloc<DecodeKept>> decodes_;
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
    std::string logDir_, station_;
    bool logOnSd_ = false;
    std::string stationFile(const char* what) const { return logDir_ + "/" + station_ + "_" + what; }
    void logOpen();                                    // pick the station's files (moving older ones in) and load them
    int eraseAllLogs();                                // every log file of every station; returns files removed
    std::deque<QsoRecord, PsAlloc<QsoRecord>> recent_;
    HashIndex worked_;                                 // "CALL|band"
    HashIndex workedDay_;                              // "CALL|band|mode|YYYYMMDD" (parks: once a day)
    static std::string dayKey(std::string_view rec);
    int logCount_ = 0, archiveCount_ = 0;
    bool archiveBehind_ = false;                     // a QSO is in the current log but not in the archive
    // storage figures, refreshed every 30 s by the network thread
    std::atomic<uint64_t> intTotal_{0}, intUsed_{0}, sdTotal_{0}, sdUsed_{0};
    std::atomic<long> left_{-1};
    int64_t storMono_ = -100000;
    bool lowWarned_ = false;
    void storRefresh();
    void logReload();                                  // worked-before, counts and recent list from the files
    std::string logChange(bool archive, int idx, const std::string& expectCall,
                          const std::map<std::string, std::string>* fields);   // nullptr = delete; returns error or ""
    std::string logReadJson(bool archive, int from, int n, const std::string& filter);
    void trashPurge();                                 // drop deleted contacts older than 7 days
    std::string trashJson();
    std::string trashRestore(int idx);                 // returns error or ""
    std::string netLast_;                              // last forwarding attempt, for the Log tab
    int64_t netLastMs_ = 0;
    std::mutex netMtx_;
    std::condition_variable netCv_;
    struct NetItem { PStr rec; bool test = false; };
    std::deque<NetItem, PsAlloc<NetItem>> netQ_;       // contacts waiting to go to the logging program (PSRAM)
    HashIndex sent_;                                   // contacts the logging program has (key: sentKey)
    std::atomic<int> unsent_{0};                       // contacts in the current log not sent yet
    std::string sentPath() const { return stationFile("sent.txt"); }
    static std::string sentKey(std::string_view rec);
    void sentLoad();
    int countUnsent();                                 // reads the current log
    int queueUnsent(bool archive);                     // starts sending; returns how many will go
    int syncFile_ = -1;                                // "send unsent" in progress: 0 current log, 1 archive (mtx_)
    long syncOff_ = 0;                                 // how far through that file it has queued
    std::atomic<bool> syncPending_{false};
    void syncRefill();                                 // mtx_ held: queue the next batch
    std::atomic<int> netOk_{0}, netFail_{0};

    NmeaTime nmea_, nmeaMod_;
    float gpsErr_ = 0;
    void gpsTime(int64_t utcNow, int64_t mono, const char* src);
    // automatic TX audio level (ALC)
    struct AlcLevel { float level = 0, ceiling = 0; float cutFrom = 0; int cutAlc = 0; bool stuck = false; };
    std::map<std::string, AlcLevel> alcLevel_;
    std::atomic<float> txGain_{50.0f};
    std::string alcBand_;
    uint32_t alcSeen_ = 0;
    int alcMax_ = 0, poMax_ = 0, alcReads_ = 0, alcHot_ = 0;
    bool alcCut_ = false;
    std::vector<int> alcRd_;                           // ALC readings of this transmission
    bool alcAdjusts() const;                           // FT8/FT4 with auto level on (not QMX)
    float alcFloor() const;
    void alcStartTx();
    void alcTick(int64_t mono);
    void alcEndTx();
    std::string alcPath() const;
    void alcLoad();
    void alcSave();
    uint32_t ppsUsed_ = 0;
    std::string status_, netInfo_, wchipVer_, wchipUpd_;
    StaStatus sta_;
    int64_t lastUiMono_ = 0, bandReqHz_ = 0;
    bool watchdogTripped_ = false, wasConn_ = false;
    std::string levelModel_, levelCand_;               // radio whose manual TX level is in use / being detected
    int64_t levelCandMono_ = 0;
    void levelTrack(int64_t mono);
    bool waitShown_ = false;                           // the "press Next QSO" note was shown
    // Dead man's switch: internet reachability (-1 not checked, 0 lost, 1 there), checked every
    // 20 s by the network thread; lost after 3 failed checks in a row.
    std::atomic<int> inet_{-1};
    int inetMiss_ = 0;
    int64_t inetCheckMono_ = -100000;
    void inetCheck();
};
