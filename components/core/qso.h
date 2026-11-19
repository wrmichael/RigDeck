// FT8/FT4 message parsing and the QSO sequencer (what to send next, when a contact is complete).
#pragma once
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <cstdint>
#include "ftx.h"

enum MsgKind { K_CQ, K_GRID, K_REPORT, K_RREPORT, K_RRR, K_RR73, K_73, K_OTHER };

struct ParsedMsg {
    std::string to, from, grid, cqTag;
    MsgKind kind = K_OTHER;
    int report = 0;
};
bool parseMsg(const std::string& text, ParsedMsg& p);
bool isGrid4(const std::string& s);

struct QsoLog {
    std::string call, grid;
    int rptSent = 0, rptRcvd = 0;
    int64_t startMs = 0, endMs = 0;
};

class Sequencer {
public:
    static const int UNSET = -99;
    // configuration (copied from Settings by the app)
    std::string myCall, myGrid;
    int opMode = 0, maxRetries = 5, cqParity = 0;
    bool useRR73 = true;
    // hooks
    std::function<void(const QsoLog&)> onLog;
    std::function<bool(const std::string&)> workedBefore;
    std::function<int64_t()> now;

    // state (read by the UI)
    bool txEnabled = false;
    int txMsg = 0;                 // 0 none, 1 grid, 2 report, 3 R+report, 4 RR73/RRR, 5 73, 6 CQ, 7 free text
    int txParity = 0;
    int retries = 0;
    std::string dxCall, dxGrid, freeText, lastWorked;
    int rptSent = UNSET, rptRcvd = UNSET;
    bool logged = false;
    int64_t qsoStart = 0, lastWorkedAt = 0;

    void onDecodes(const std::vector<Decode>& list, int parity);   // all decodes of one slot
    bool nextTx(int parity, std::string& text);                    // should we transmit in this slot?
    void txDone();                                                 // a transmission of txMsg finished
    std::string compose(int n) const;

    void callStation(const std::string& call, const std::string& grid, int snr, int parity);
    void respondTo(const ParsedMsg& p, int snr, int parity);       // a message addressed to us
    void startCq();
    void halt();                                                   // stop transmitting, keep QSO fields
    void clearQso();
    void setTxMsg(int n);
    void setOpMode(int m);

private:
    void setTx(int n);
    void logQso();
    void finish();
    std::map<std::string, int64_t> ignore_;                        // calls that did not answer: skip for a while
};
