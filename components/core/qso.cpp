#include "qso.h"
#include <cstdio>
#include <cstdlib>
#include <cctype>

bool isGrid4(const std::string& s) {
    return s.size() == 4 && s != "RR73" && s[0] >= 'A' && s[0] <= 'R' && s[1] >= 'A' && s[1] <= 'R' &&
           isdigit((unsigned char)s[2]) && isdigit((unsigned char)s[3]);
}

static std::string bare(const std::string& c) {                  // "<K1ABC>" -> "K1ABC"
    if (c.size() > 2 && c.front() == '<' && c.back() == '>') return c.substr(1, c.size() - 2);
    return c;
}

static bool isReport(const std::string& s, int& v) {             // "-12", "+05"
    if (s.size() != 3 || (s[0] != '-' && s[0] != '+') || !isdigit((unsigned char)s[1]) || !isdigit((unsigned char)s[2])) return false;
    v = atoi(s.c_str());
    return true;
}

bool parseMsg(const std::string& text, ParsedMsg& p) {
    std::vector<std::string> t;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && text[i] == ' ') i++;
        size_t j = i;
        while (j < text.size() && text[j] != ' ') j++;
        if (j > i) t.push_back(text.substr(i, j - i));
        i = j;
    }
    p = ParsedMsg();
    if (t.size() < 2) return false;
    if (t[0] == "CQ") {
        p.kind = K_CQ;
        bool hasGrid = t.size() >= 3 && isGrid4(t.back());
        size_t callIdx = hasGrid ? t.size() - 2 : t.size() - 1;
        if (callIdx < 1) return false;
        p.from = bare(t[callIdx]);
        if (hasGrid) p.grid = t.back();
        if (callIdx == 2) p.cqTag = t[1];                        // "CQ DX K1ABC FN42", "CQ POTA ..."
        else if (callIdx > 2) return false;
        return true;
    }
    p.to = bare(t[0]);
    p.from = bare(t[1]);
    if (t.size() == 2) { p.kind = K_GRID; return true; }          // a bare call with no locator
    if (t.size() > 3) { p.kind = K_OTHER; return true; }          // contest exchanges etc.
    const std::string& x = t[2];
    int v;
    if (x == "RRR") p.kind = K_RRR;
    else if (x == "RR73") p.kind = K_RR73;
    else if (x == "73") p.kind = K_73;
    else if (isReport(x, v)) { p.kind = K_REPORT; p.report = v; }
    else if (x.size() == 4 && x[0] == 'R' && isReport(x.substr(1), v)) { p.kind = K_RREPORT; p.report = v; }
    else if (isGrid4(x)) { p.kind = K_GRID; p.grid = x; }
    else p.kind = K_OTHER;
    return true;
}

std::string Sequencer::compose(int n) const {
    char b[48];
    if (myCall.empty()) return "";
    std::string g = myGrid.substr(0, 4);
    int r = rptSent == UNSET ? -15 : (rptSent < -30 ? -30 : (rptSent > 30 ? 30 : rptSent));
    switch (n) {
    case 1: if (dxCall.empty()) return ""; snprintf(b, sizeof b, "%s %s %s", dxCall.c_str(), myCall.c_str(), g.c_str()); break;
    case 2: if (dxCall.empty()) return ""; snprintf(b, sizeof b, "%s %s %+03d", dxCall.c_str(), myCall.c_str(), r); break;
    case 3: if (dxCall.empty()) return ""; snprintf(b, sizeof b, "%s %s R%+03d", dxCall.c_str(), myCall.c_str(), r); break;
    case 4: if (dxCall.empty()) return ""; snprintf(b, sizeof b, "%s %s %s", dxCall.c_str(), myCall.c_str(), useRR73 ? "RR73" : "RRR"); break;
    case 5: if (dxCall.empty()) return ""; snprintf(b, sizeof b, "%s %s 73", dxCall.c_str(), myCall.c_str()); break;
    case 6:
        if (cqTag.empty()) snprintf(b, sizeof b, "CQ %s %s", myCall.c_str(), g.c_str());
        else snprintf(b, sizeof b, "CQ %s %s %s", cqTag.c_str(), myCall.c_str(), g.c_str());
        break;
    case 7: return freeText;
    default: return "";
    }
    std::string s(b);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

void Sequencer::setTx(int n) {
    if (n != txMsg) retries = 0;
    txMsg = n;
}

void Sequencer::setTxMsg(int n) {
    if (n < 0 || n > 7) return;
    setTx(n);
    if (n == 6) txParity = cqParity;
}

void Sequencer::setOpMode(int m) {
    opMode = m;
    if (m == 1 && dxCall.empty() && txEnabled) startCq();
    if (m != 1 && txMsg == 6) txMsg = 0;
}

void Sequencer::startCq() {
    clearQso();
    waitNext = false;
    txMsg = 6;
    txParity = cqParity;
    retries = 0;
}

void Sequencer::halt() { txEnabled = false; }

void Sequencer::clearQso() {
    dxCall.clear();
    dxGrid.clear();
    rptSent = rptRcvd = UNSET;
    logged = false;
    sentReport = false;
    retries = 0;
    txMsg = 0;
}

void Sequencer::nextQso() {
    waitNext = false;
    if (opMode == 1) startCq();                                    // Auto CQ: call CQ again; Auto hunt: the next CQ heard
}

void Sequencer::callStation(const std::string& call, const std::string& grid, int snr, int parity) {
    waitNext = false;
    dxCall = call;
    dxGrid = grid;
    rptSent = snr;
    rptRcvd = UNSET;
    logged = false;
    sentReport = false;
    qsoStart = now ? now() : 0;
    txParity = 1 - parity;                                        // answer in the other slot
    txMsg = 1;
    retries = 0;
}

void Sequencer::respondTo(const ParsedMsg& p, int snr, int parity) {
    waitNext = false;
    bool same = (p.from == dxCall);
    if (!same) {
        dxCall = p.from;
        dxGrid.clear();
        rptSent = snr;
        rptRcvd = UNSET;
        sentReport = false;
        retries = 0;
        // They missed our RR73 and are repeating: answer again, but do not log twice.
        logged = (p.from == lastWorked && now && now() - lastWorkedAt < 180000);
        qsoStart = now ? now() : 0;
    }
    if (rptSent == UNSET) rptSent = snr;
    txParity = 1 - parity;
    switch (p.kind) {
    case K_GRID: if (!p.grid.empty()) dxGrid = p.grid; setTx(2); break;
    case K_REPORT: rptRcvd = p.report; setTx(3); break;
    case K_RREPORT: rptRcvd = p.report; setTx(4); break;
    case K_RRR: case K_RR73: logQso(); setTx(5); break;
    case K_73: logQso(); finish(); break;
    default: break;
    }
}

void Sequencer::logQso() {
    if (logged || dxCall.empty() || rptRcvd == UNSET) return;     // no report received = not a QSO
    logged = true;
    lastWorked = dxCall;
    lastWorkedAt = now ? now() : 0;
    if (onLog) {
        QsoLog q;
        q.call = dxCall; q.grid = dxGrid;
        q.rptSent = rptSent; q.rptRcvd = rptRcvd;
        q.endMs = now ? now() : qsoStart;
        q.startMs = qsoStart ? qsoStart : q.endMs;                // call typed in by hand: no start time recorded
        onLog(q);
    }
}

int Sequencer::forceLog() {
    if (dxCall.empty()) return -1;
    if (logged) return 1;
    if (!sentReport && rptRcvd == UNSET) return -2;
    logged = true;
    lastWorked = dxCall;
    lastWorkedAt = now ? now() : 0;
    if (onLog) {
        QsoLog q;
        q.call = dxCall; q.grid = dxGrid;
        q.rptSent = sentReport ? rptSent : UNSET;
        q.rptRcvd = rptRcvd;
        q.endMs = now ? now() : qsoStart;
        q.startMs = qsoStart ? qsoStart : q.endMs;                // call typed in by hand: no start time recorded
        q.partial = true;
        onLog(q);
    }
    return 0;
}

void Sequencer::finish() {
    clearQso();
    if (confirmEach && (opMode == 1 || opMode == 2) && txEnabled) { waitNext = true; return; }   // POTA mode
    if (opMode == 1 && txEnabled) { txMsg = 6; txParity = cqParity; }
    else if (opMode == 0) txEnabled = false;                      // manual: one QSO, then stand by
}

void Sequencer::txDone() {
    retries++;
    if (txMsg >= 2 && txMsg <= 4) sentReport = true;
    switch (txMsg) {
    case 4:
        logQso();
        if (useRR73 || retries >= maxRetries) finish();
        break;
    case 5: finish(); break;
    case 1: case 2: case 3:
        if (retries >= maxRetries) {
            if (now) ignore_[dxCall] = now() + 10 * 60 * 1000;
            finish();
        }
        break;
    case 7: txMsg = 0; if (opMode == 0) txEnabled = false; break;  // free text goes out once
    default: break;
    }
}

bool Sequencer::nextTx(int parity, std::string& text) {
    if (!txEnabled || waitNext || txMsg == 0 || parity != txParity) return false;
    text = compose(txMsg);
    return !text.empty();
}

void Sequencer::onDecodes(const std::vector<Decode>& list, int parity) {
    if (myCall.empty()) return;
    const Decode* forMe = nullptr;       // from the station we are working
    ParsedMsg forMeP;
    const Decode* caller = nullptr;      // someone else calling us
    ParsedMsg callerP;
    const Decode* cq = nullptr;
    ParsedMsg cqP;
    int64_t t = now ? now() : 0;
    for (const auto& d : list) {
        ParsedMsg p;
        if (!parseMsg(d.text, p) || p.from.empty() || p.from == myCall) continue;
        if (p.kind == K_CQ) {
            if (!p.cqTag.empty()) {
                // Directed CQ: left to the operator (CQ DX may not want us), except parks: a POTA or
                // WWFF activator can be worked again each UTC day on each band and mode.
                bool park = p.cqTag == "POTA" || p.cqTag == "WWFF";
                if (!park || !workedToday || workedToday(p.from)) continue;
            } else if (workedBefore && workedBefore(p.from)) continue;
            auto ig = ignore_.find(p.from);
            if (ig != ignore_.end() && ig->second > t) continue;
            if (!cq || d.snr > cq->snr) { cq = &d; cqP = p; }
        } else if (p.to == myCall && p.kind != K_OTHER) {
            if (!dxCall.empty() && p.from == dxCall) { forMe = &d; forMeP = p; }
            else if (!caller || d.snr > caller->snr) { caller = &d; callerP = p; }
        }
    }
    if (forMe) { respondTo(forMeP, forMe->snr, parity); return; }
    if (waitNext) {                                                // POTA mode: nothing new until the operator says so,
        if (caller && callerP.from == lastWorked && callerP.kind != K_73) respondTo(callerP, caller->snr, parity);   // except the last station repeating
        return;
    }
    bool freeToAnswer = dxCall.empty() || txMsg == 6 || txMsg == 0;
    if (caller && freeToAnswer && txEnabled) {
        // A 73 from someone we are not working needs no reply.
        if (callerP.kind != K_73) { respondTo(callerP, caller->snr, parity); return; }
    }
    if (opMode == 2 && txEnabled && dxCall.empty() && cq) callStation(cqP.from, cqP.grid, cq->snr, parity);
}
