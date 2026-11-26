// Persistent settings, stored as key=value lines in a text file on the flash filesystem.
#pragma once
#include <string>
#include <cstdint>

enum OpMode { OP_MANUAL = 0, OP_AUTO_CQ = 1, OP_AUTO_HUNT = 2 };
enum LogProto { LOG_NONE = 0, LOG_TCP_ADIF = 1, LOG_UDP_ADIF = 2, LOG_UDP_WSJTX = 3 };

struct Settings {
    std::string rigName;                 // this unit's name ("Shack", "Mobile"): shown on the web page, used in download names
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
    long long dialHz = 14074000;         // no CAT: the dial frequency the band buttons (or typed) last set
    int dialMode = 1, dialData = 1;      // no CAT: mode (Icom numbering: 0 LSB 1 USB 3 CW ...) and DATA on/off
    int catBaud = 38400;                 // text CAT rate tried first (FTX-1 "CAT-1 RATE"; others are searched)
    int txFreq = 1500;                   // audio offset of our signal, Hz
    int txLevel = 50;                    // digital drive level, percent of full scale
    int opMode = OP_MANUAL;
    int cqParity = 0;                    // 0 = transmit in even slots (:00/:30), 1 = odd
    int useRR73 = 1;
    int maxRetries = 5;                  // unanswered repeats before a QSO attempt is abandoned
    int watchdogMin = 6;                 // stop auto TX after this long with no operator activity (0 = off)
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
    int tuneRadio = 0;                   // 1 = Tune starts the radio's tuner / ATAS (Yaesu AC002) instead of a carrier
    int usbFullSpeed = 1;                // 1 = radio USB port at full speed (reaches chips behind a high-speed hub, FTX-1)
    int gpsClock = 0;                    // 1 = set the clock from GPS sentences on the radio's second USB serial port
    int gpsBaud = 4800;                  // must match the radio's "DVdat/GPS Out Baud"
    int gpsDelayMs = 150;                // how long after the second the receiver starts sending its sentence
    int gpsModule = 0;                   // 1 = a GPS module wired to the board's header (UART, optional PPS)
    int gpsModBaud = 9600;               // its NMEA baud rate (most u-blox / ATGM modules: 9600)
    int gpsRxPin = 3;                    // board GPIO the module's TX goes to
    int gpsPpsPin = 4;                   // board GPIO the module's PPS goes to, -1 = not wired
    int bright = 100;                    // display backlight, % (5 - 100)
    int contrast = 100;                  // display colour contrast, % (50 - 150; 100 = as designed)
    int confirmQso = 0;
    std::string dnw;                     // do-not-work list: "W1ABC,K2XYZ,XX9*" (never worked automatically)
    int prioOn = 0;                      // 1 = choose between stations by the priority list below
    std::string prio = "pota,sota,dx,ses,-weak,strong";   // order of the rules; '-' = rule off
    int dxKm = 2500;                     // the "DX" rule: farther than this, from the grid in its message
    // APRS (mode APRS on the web page): 1200 baud packets on FM
    std::string aprsCall;                // call-SSID on the air, e.g. AC9HP-9 (empty = the station callsign)
    std::string pktCall;                 // packet terminal call-SSID (empty = the station callsign)
    long long pktFreq = 145010000;       // packet frequency the terminal tunes to (BBS, nodes)
    int pktAccept = 1;                   // take incoming connections (keyboard to keyboard)
    int kissPort = 8001;                 // KISS TNC on TCP for PC programs (0 = off)
    int bbsOn = 0;                       // packet BBS (mailbox) on the microSD card, answering on bbsCall
    std::string bbsCall;                 // BBS call-SSID (empty = the packet call with -1)
    long long aprsFreq = 144390000;      // North America; 144.800 in Europe
    int aprsBeaconMin = 10;              // position beacon interval while TX is enabled (0 = no beacons)
    std::string aprsSym = "/-";          // symbol table and code: /- house, /> car, /[ person, /k truck
    std::string aprsComment = "RigDeck";
    std::string aprsPath = "WIDE1-1,WIDE2-1";
    std::string aprsLat, aprsLon;        // fixed position, decimal degrees (empty = GPS, else the grid square's centre)
    int aprsLevel = 30;                  // TX audio level % for APRS (FM deviation)
    int deadman = 0;                     // 1 = no internet: finish the QSO in progress, then stop Auto CQ / hunt
    std::string txLevels;
    int levelFor(const std::string& radio) const;          // -1 = none remembered
    void rememberLevel(const std::string& radio, int lvl);                // manual TX level per radio, "IC-7100:8;No CAT:45" (kept by RigDeck)
    std::string deadmanHost = "1.1.1.1:53";   // what is checked for "internet" (host or IP, optional :port, TCP)                  // POTA mode: Auto CQ / hunt wait for "Next QSO" after each QSO
    int consoleLog = -1;                 // copy the console log to the microSD card (RigDeck/logs): 1 on, 0 off,
                                         // -1 = the build's default (on in Beta builds, off in Stable ones)
    int betaNotice = -1;                 // "update available" notice for new Beta releases too: 1 on, 0 off, -1 build default
    std::string updSeen;                 // release the touch screen's update notice was closed for

    bool set(const std::string& key, const std::string& value);   // false = unknown key / bad value
    std::string serialize(bool includeSecrets) const;             // key=value lines
    std::string json() const;                                     // for the web UI (no passwords)
    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

std::string upperTrim(const std::string& s);
bool validCall(const std::string& c);
bool validGrid(const std::string& g);
