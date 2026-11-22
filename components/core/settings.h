// Persistent settings, stored as key=value lines in a text file on the flash filesystem.
#pragma once
#include <string>
#include <cstdint>

enum OpMode { OP_MANUAL = 0, OP_AUTO_CQ = 1, OP_AUTO_HUNT = 2 };
enum LogProto { LOG_NONE = 0, LOG_TCP_ADIF = 1, LOG_UDP_ADIF = 2, LOG_UDP_WSJTX = 3 };

struct Settings {
    std::string call;                    // must be set before the firmware will transmit
    std::string grid;                    // 4 or 6 character locator
    std::string op;                      // ADIF OPERATOR: who is at the key, if different from the station callsign
    std::string mySig;                   // ADIF MY_SIG: the programme the reference below belongs to (optional, e.g. WWFF)
    std::string cqTag;                   // directed CQ: "" plain, or DX / POTA / SOTA / NA ... (1-4 letters) or 3 digits
    std::string mySigInfo;               // ADIF MY_SIG_INFO: e.g. a park reference such as US-1234 (empty = not logged)
    std::string apSsid = "RigDeck";
    std::string apPass = "ft8ft8ft8";    // WPA2 needs >= 8 chars; change it in the Settings tab
    std::string staSsid;                 // optional: also join an existing network
    std::string staPass;
    int civAddr = 0x88;                  // IC-7100 default CI-V address
    int civBaud = 19200;
    int radioType = 0;                   // 0 = find out, 1 = Icom, 2 = Yaesu, 3 = QRP Labs QMX / QMX+,
                                         // 4 = Kenwood / Elecraft, 5 = no CAT (SignaLink or other sound card)
    int pttMethod = 0;                   // 0 = CAT command, 1 = serial RTS line (Digirig), 2 = DTR, 3 = VOX
    long long dialHz = 14074000;         // no CAT: the dial frequency the band buttons last set
    int catBaud = 38400;                 // text CAT rate tried first (FTX-1 "CAT-1 RATE"; others are searched)
    int txFreq = 1500;                   // audio offset of our signal, Hz
    int txLevel = 50;                    // digital drive level, percent of full scale
    int opMode = OP_MANUAL;
    int cqParity = 0;                    // 0 = transmit in even slots (:00/:30), 1 = odd
    int useRR73 = 1;
    int maxRetries = 5;                  // unanswered repeats before a QSO attempt is abandoned
    int watchdogMin = 6;                 // stop auto TX after this long with no browser connected (0 = off)
    int rfPower = -1;                    // -1 = leave the radio's setting alone, else 0..100 %
    int autoAlc = 1;                     // 1 = lower the TX audio level automatically while the radio shows ALC
    int logProto = LOG_NONE;
    std::string logHost;
    int logPort = 2237;
    int cwWpm = 40;                      // FT-CW speed; both stations must use the same value
    int keyWpm = 0;                      // logger: keyer speed sent to the radio when it connects (0 = leave it alone)
    std::string macro[6] = {             // logger CW macros, "Label|text". {MYCALL} {CALL} {RST} {GRID} {REF} are filled in
        "CQ|CQ CQ DE {MYCALL} {MYCALL} K",
        "His call|{CALL}",
        "Report|{CALL} UR {RST} {RST} BK",
        "TU 73|TU 73 DE {MYCALL} E E",
        "My call|{MYCALL}",
        "AGN?|AGN?",
    };
    int gpsClock = 0;                    // 1 = set the clock from GPS sentences on the radio's second USB serial port
    int gpsBaud = 4800;                  // must match the radio's "DVdat/GPS Out Baud"
    int gpsDelayMs = 150;                // how long after the second the receiver starts sending its sentence
    int gpsModule = 0;                   // 1 = a GPS module wired to the board's header (UART, optional PPS)
    int gpsModBaud = 9600;               // its NMEA baud rate (most u-blox / ATGM modules: 9600)
    int gpsRxPin = 3;                    // board GPIO the module's TX goes to
    int gpsPpsPin = 4;                   // board GPIO the module's PPS goes to, -1 = not wired
    int bright = 100;                    // display backlight, % (5 - 100)
    int contrast = 100;                  // display colour contrast, % (50 - 150; 100 = as designed)
    int consoleLog = 1;                  // 1 = copy the console log to the microSD card (RigDeck/logs)

    bool set(const std::string& key, const std::string& value);   // false = unknown key / bad value
    std::string serialize(bool includeSecrets) const;             // key=value lines
    std::string json() const;                                     // for the web UI (no passwords)
    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

std::string upperTrim(const std::string& s);
bool validCall(const std::string& c);
bool validGrid(const std::string& g);
