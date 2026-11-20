#include "app.h"
#include "clock.h"
#include "netlog.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <strings.h>
#include <cmath>
#include <chrono>

static const float kPi = 3.14159265358979f;

// ---- small helpers ------------------------------------------------------------------------------
static std::string jstr(const std::string& in) {
    std::string o = "\"";
    for (unsigned char c : in) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c < 0x20 || c >= 0x7F) o += '?';
        else o += (char)c;
    }
    return o + "\"";
}
static std::string jnum(long long v) { char b[24]; snprintf(b, sizeof b, "%lld", v); return b; }
static const char* jbool(bool b) { return b ? "true" : "false"; }
static std::string arg(const std::map<std::string, std::string>& a, const char* k, const char* def = "") {
    auto it = a.find(k);
    return it == a.end() ? def : it->second;
}
static const char* modeName(int m) { return m == DM_FT4 ? "FT4" : (m == DM_PSK31 ? "PSK31" : (m == DM_CW ? "CW" : "FT8")); }
static std::string rpt(int v) {
    if (v == Sequencer::UNSET) return ""; char b[8]; snprintf(b, sizeof b, "%+03d", v); return b; }

// "Label|text" -> text with the placeholders filled in. {CALL} is left in place when there is no
// callsign yet so the caller can refuse to send it.
std::string App::expandMacro(const std::string& macro, const std::string& call, const std::string& rst) const {
    size_t bar = macro.find('|');
    std::string t = bar == std::string::npos ? macro : macro.substr(bar + 1);
    auto sub = [&t](const char* key, const std::string& v) {
        for (size_t p; (p = t.find(key)) != std::string::npos;) t.replace(p, strlen(key), v);
    };
    std::string r = rst;
    for (auto& ch : r) if (ch == '9') ch = 'N';                    // cut numbers, as sent by hand: 599 -> 5NN
    sub("{MYCALL}", cfg.call);
    sub("{RST}", r);
    sub("{GRID}", cfg.grid.substr(0, 4));
    sub("{REF}", cfg.mySigInfo);
    if (!call.empty()) sub("{CALL}", call);
    return t;
}

// "14.250", "14.250.000", "7074" (kHz), "14250000" (Hz) -> Hz. 0 = not a frequency from 100 kHz to 1.3 GHz.
uint64_t parseDialFreq(const std::string& text) {
    std::string t;
    for (char c : text) if (c != ' ' && c != ',') t += c;
    if (t.empty()) return 0;
    for (char c : t) if (!isdigit((unsigned char)c) && c != '.') return 0;
    double v;
    size_t d1 = t.find('.');
    if (d1 != std::string::npos && t.find('.', d1 + 1) != std::string::npos) {       // 14.250.000: MHz.kHz.Hz
        std::string s = t;
        s.erase(std::remove(s.begin() + d1 + 1, s.end(), '.'), s.end());
        v = atof(s.c_str()) * 1e6;
    } else if (d1 != std::string::npos) v = atof(t.c_str()) * 1e6;                 // a decimal point: MHz
    else {
        double n = atof(t.c_str());
        v = n < 2000 ? n * 1e6 : n < 2000000 ? n * 1e3 : n;                         // MHz, kHz or Hz by size
    }
    uint64_t hz = (uint64_t)(v + 0.5);
    return hz >= 100000ULL && hz <= 1300000000ULL ? hz : 0;
}

App::App() {}

void App::begin(const std::string& dataDir) {
    dir_ = dataDir;
    cfg.load(dir_ + "/settings.txt");
    alcLoad();
    radio.addr = (uint8_t)cfg.civAddr;
    seq.now = [] { return clk::utcMs(); };
    seq.workedBefore = [this](const std::string& call) {
        return worked_.count(call + "|" + bandName(radio.freq.load())) > 0;
    };
    seq.onLog = [this](const QsoLog& q) {           // called with mtx_ held
        QsoRecord r;
        r.call = q.call;
        r.grid = q.grid;
        if (mode_ == DM_FT4) { r.mode = "MFSK"; r.submode = "FT4"; }
        else if (mode_ == DM_CW) { r.mode = "CW"; r.comment = "FT-CW"; }
        else r.mode = "FT8";
        r.rstSent = rpt(q.rptSent);
        r.rstRcvd = rpt(q.rptRcvd);
        r.startMs = q.startMs;
        r.endMs = q.endMs;
        logQso(r);
    };
    syncSeq();
    // Worked-before comes from the archive (all QSOs) as well as the current log.
    logReload();
    archiveBehind_ = logCount_ > archiveCount_;
}

// Counts the records in an ADIF file and adds each one to the worked-before list.
int App::scanLog(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return 0;
    int count = 0, ch;
    std::string rec;
    while ((ch = fgetc(f)) != EOF) {
        rec += (char)ch;
        size_t n = rec.size();
        if (n >= 5 && (rec[n - 1] == '>') && !strncasecmp(rec.c_str() + n - 5, "<EOR>", 5)) {
            std::string call = adifField(rec, "CALL"), band = adifField(rec, "BAND");
            if (!call.empty()) { worked_.insert(call + "|" + band); count++; }
            rec.clear();
        } else if (n >= 5 && !strncasecmp(rec.c_str() + n - 5, "<EOH>", 5)) rec.clear();
        if (rec.size() > 2000) rec.clear();
    }
    fclose(f);
    return count;
}

// After the files changed (edit, delete): worked-before, counts and the recent list from scratch.
void App::logReload() {
    worked_.clear();
    archiveCount_ = scanLog(archivePath());
    logCount_ = scanLog(logPath());
    recent_.clear();
    std::vector<std::string> recs;
    adifReadRecords(logPath(), recs);
    for (size_t i = recs.size() > 30 ? recs.size() - 30 : 0; i < recs.size(); i++) recent_.push_back(adifToQso(recs[i]));
}

// Edit (fields) or delete (fields == nullptr) record idx of the current log or the archive. The
// same record in the other file (identical text) changes with it, so the two stay in step.
// expectCall guards against the list having changed since it was read.
std::string App::logChange(bool archive, int idx, const std::string& expectCall,
                           const std::map<std::string, std::string>* fields) {
    std::string path = archive ? archivePath() : logPath(), other = archive ? logPath() : archivePath();
    std::vector<std::string> recs;
    if (!adifReadRecords(path, recs) || idx < 0 || idx >= (int)recs.size()) return "That contact is no longer in the log; reload the list";
    if (!expectCall.empty() && strcasecmp(adifField(recs[idx], "CALL").c_str(), expectCall.c_str()))
        return "The log changed since the list was read; reload it";
    std::string orig = recs[idx], repl;
    if (fields) {
        AdifFields f = adifParse(orig);
        bool bandSet = false, freqSet = false;
        for (auto& kv : *fields) {
            std::string name = upperTrim(kv.first), v = kv.second;
            if (name.empty() || name.size() > 40 || name.find_first_of("<>: ") != std::string::npos) return "Bad field name: " + kv.first;
            if (v.find('\n') != std::string::npos || v.find("<EOR>") != std::string::npos || v.find("<eor>") != std::string::npos) return "Bad value for " + name;
            while (!v.empty() && v.back() == ' ') v.pop_back();
            while (!v.empty() && v.front() == ' ') v.erase(0, 1);
            if (name == "CALL" || name == "GRIDSQUARE" || name == "MODE" || name == "SUBMODE" || name == "BAND" ||
                name == "STATION_CALLSIGN" || name == "OPERATOR" || name == "MY_GRIDSQUARE") v = upperTrim(v);
            if (name == "BAND") { for (auto& c : v) c = (char)tolower((unsigned char)c); bandSet = true; }
            if (name == "CALL" && !validCall(v)) return "Not a valid callsign: " + v;
            if (name == "FREQ" && !v.empty()) {
                double mhz = atof(v.c_str());
                if (mhz < 0.1 || mhz > 1300) return "Frequency is in MHz, e.g. 14.074";
                char b[24]; snprintf(b, sizeof b, "%.6f", mhz); v = b;
                freqSet = true;
            }
            if ((name == "QSO_DATE" || name == "QSO_DATE_OFF") && !v.empty() && (v.size() != 8 || v.find_first_not_of("0123456789") != std::string::npos))
                return name + " is YYYYMMDD";
            if ((name == "TIME_ON" || name == "TIME_OFF") && !v.empty() && ((v.size() != 4 && v.size() != 6) || v.find_first_not_of("0123456789") != std::string::npos))
                return name + " is HHMM or HHMMSS (UTC)";
            adifSetField(f, name, v);
        }
        if (freqSet && !bandSet) {                    // keep BAND matching a changed frequency
            for (auto& kv : f) if (kv.first == "FREQ") adifSetField(f, "BAND", bandName((uint64_t)(atof(kv.second.c_str()) * 1e6 + 0.5)));
        }
        if (adifField(adifBuild(f), "CALL").empty()) return "A contact needs a callsign";
        repl = adifBuild(f);
        if (repl == orig) return "";
        recs[idx] = repl;
    } else {
        recs.erase(recs.begin() + idx);
    }
    if (!adifWriteRecords(path, recs)) return "Could not save the log (storage full?)";
    std::vector<std::string> o;
    if (adifReadRecords(other, o)) {
        for (size_t i = o.size(); i > 0; i--) {
            if (o[i - 1] != orig) continue;
            if (fields) o[i - 1] = repl;
            else o.erase(o.begin() + (i - 1));
            adifWriteRecords(other, o);
            break;
        }
    }
    logReload();
    return "";
}

// One page of a log, newest first: {"total":N,"items":[{"i":index,"f":{"CALL":"..",..}}]}.
// filter (optional) matches the callsign (contains, any case).
std::string App::logReadJson(bool archive, int from, int n, const std::string& filter) {
    std::vector<std::string> recs;
    adifReadRecords(archive ? archivePath() : logPath(), recs);
    std::string flt = upperTrim(filter);
    std::vector<int> idx;
    for (int i = (int)recs.size() - 1; i >= 0; i--)
        if (flt.empty() || upperTrim(adifField(recs[i], "CALL")).find(flt) != std::string::npos) idx.push_back(i);
    std::string o = "{\"ok\":true,\"total\":" + jnum((long long)idx.size()) + ",\"items\":[";
    for (int k = std::max(0, from); k < (int)idx.size() && k < from + n; k++) {
        o += std::string(k > from ? "," : "") + "{\"i\":" + jnum(idx[k]) + ",\"f\":{";
        bool first = true;
        for (auto& kv : adifParse(recs[idx[k]])) { o += std::string(first ? "" : ",") + jstr(kv.first) + ":" + jstr(kv.second); first = false; }
        o += "}}";
    }
    return o + "]}";
}

void App::syncSeq() {
    seq.myCall = cfg.call;
    seq.myGrid = cfg.grid;
    seq.opMode = cfg.opMode;
    seq.maxRetries = cfg.maxRetries;
    seq.cqParity = cfg.cqParity;
    seq.useRR73 = cfg.useRR73 != 0;
}

void App::status(const std::string& s) { status_ = s; }

void App::setNetInfo(const std::string& s) {
    std::lock_guard<std::mutex> lk(mtx_);
    netInfo_ = s;
}
void App::setStaStatus(const StaStatus& s) {
    std::lock_guard<std::mutex> lk(mtx_);
    sta_ = s;
}
void App::setWifiChip(const std::string& ver, const std::string& upd) {
    std::lock_guard<std::mutex> lk(mtx_);
    wchipVer_ = ver;
    wchipUpd_ = upd;
}
void App::setWifiChipUpd(const std::string& upd) {
    std::lock_guard<std::mutex> lk(mtx_);
    wchipUpd_ = upd;
    if (!upd.empty()) status_ = upd;                 // also on the touch screen's status line
}
App::StaStatus App::staStatus() {
    std::lock_guard<std::mutex> lk(mtx_);
    return sta_;
}

// ---- receive ------------------------------------------------------------------------------------
void App::rxAudio48k(const int16_t* s, size_t n) {
    int peak = rxPeak_.load() * 9 / 10;
    for (size_t i = 0; i < n; i++) { int a = abs(s[i]); if (a > peak) peak = a; }
    rxPeak_ = peak;
    float buf[256];
    while (n > 0) {
        size_t chunk = n > 960 ? 960 : n;
        size_t n12 = dec_.process(s, chunk, buf);
        s += chunk;
        n -= chunk;
        spec_.feed(buf, n12);
        if (mode_ == DM_PSK31) {
            if (pskFreqDirty_.exchange(false)) pskRx_.setFreq((float)pskFreq_.load());
            std::string out;
            pskRx_.process(buf, n12, out);
            if (!out.empty()) {
                std::lock_guard<std::mutex> lk(mtx_);
                pskText_ += out;
                pskSeq_ += (uint32_t)out.size();
                if (pskText_.size() > 4000) pskText_.erase(0, pskText_.size() - 3000);
            }
        } else ftxRx(buf, n12);
    }
}

void App::ftxRx(const float* s, size_t n) {
    const bool cw = mode_ == DM_CW;
    if (cw && cwWpmInUse_ != cfg.cwWpm) reinit_ = true;             // speed changed in Settings
    if (reinit_) {
        if (mon_[0].busy || mon_[1].busy || cwMon_[0].busy || cwMon_[1].busy) return;
        bool ft4 = mode_ == DM_FT4;
        // Two slot buffers of ~240 kB each; ft8_lib does not check its allocations, so ask first.
        if (haveRam && !haveRam(600 * 1024)) { memFail = true; return; }
        if (cw) {
            if (!cwMon_[0].init(cfg.cwWpm) || !cwMon_[1].init(cfg.cwWpm)) { memFail = true; return; }
            cwWpmInUse_ = cfg.cwWpm;
        } else if (!mon_[0].init(ft4) || !mon_[1].init(ft4)) { memFail = true; return; }
        memFail = false;
        reinit_ = false;
        curSlot_ = -1;
    }
    if (!clk::valid()) return;
    const int64_t L = slotLen();
    int64_t now = clk::utcMs(), slot = now / L, tIn = now - slot * L;
    FtxMonitor& m = mon_[slot & 1];
    CwMonitor& cm = cwMon_[slot & 1];
    if (slot != curSlot_) {
        curSlot_ = slot;
        triggered_ = false;
        slotRx_ = !(cw ? cm.busy.load() : m.busy.load()) && slot != txSlot_.load();
        if (slotRx_) { if (cw) cm.reset(); else m.reset(); }
        size_t keep = (size_t)(tIn * 12);                           // samples that belong to the new slot
        if (keep < n) { s += n - keep; n = keep; }
    }
    if (slot == txSlot_.load()) slotRx_ = false;                    // we transmit in this slot
    if (!slotRx_ || triggered_) return;
    if (cw) cm.feed(s, n); else m.feed(s, n);
    // Start decoding as soon as a full-length transmission (plus some timing slack) is in, so the
    // reply is ready before the next slot begins.
    const int64_t trig = (mode_ == DM_FT4) ? 6000 : 13600;
    if (tIn >= trig) {
        triggered_ = true;
        if (cw) cm.busy = true; else m.busy = true;
        std::lock_guard<std::mutex> lk(jobMtx_);
        jobCw_ = cw;
        jobSlot_ = slot;
        jobPending_ = true;
        jobCv_.notify_one();
    }
}

bool App::decodeOnce() {
    int64_t slot;
    bool cw;
    {
        std::lock_guard<std::mutex> lk(jobMtx_);
        if (!jobPending_) return false;
        slot = jobSlot_;
        cw = jobCw_;
        jobPending_ = false;
        decoding_ = true;
    }
    std::vector<Decode> out;
    int64_t t0 = clk::monoMs();
    if (cw) { cwMon_[slot & 1].decode(out); cwMon_[slot & 1].busy = false; }
    else { mon_[slot & 1].decode(out); mon_[slot & 1].busy = false; }
    decodeMs_ = (int)(clk::monoMs() - t0);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& d : out) {
            d.id = ++nextId_;
            d.slotMs = slot * slotLen();
            d.parity = (int)(slot & 1);
            decodes_.push_back(d);
        }
        while (decodes_.size() > 150) decodes_.pop_front();
        syncSeq();
        seq.onDecodes(out, (int)(slot & 1));
    }
    decoding_ = false;
    return true;
}

void App::decodeLoop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(jobMtx_);
            jobCv_.wait(lk, [this] { return jobPending_; });
        }
        decodeOnce();
    }
}

// ---- transmit -----------------------------------------------------------------------------------
void App::startTx(int gen) {
    gen_ = gen;
    genStarted_ = false;
    genDone_ = false;
    haltReq_ = false;
    pttOnMono_ = clk::monoMs();
    alcStartTx();
    radio.ptt(true);
    txState_ = ST_TX;
    txOn_ = true;                                   // audio thread switches to the transmit stream
}

// ---- automatic TX audio level from the radio's ALC meter ----------------------------------------
// Keep the radio's ALC inside its zone, out of the red: overdriving it compresses the audio, which
// widens the signal and spreads splatter. Icom (and FTX-1) ALC meters read 0..255 with 120 = the top
// of the ALC zone. While transmitting, every reading (about every 0.4 s) past the zone turns the
// audio down; a transmission that stays well inside the zone lets it rise again, never past the
// Settings maximum or just below the last level that went into the red. Remembered per band.
float App::txLevelNow() const {
    if (radio.proto.load() == RP_QMX) return 100.0f;              // see txAudio48k
    return cfg.autoAlc ? txGain_.load() : (float)cfg.txLevel;
}

void App::alcStartTx() {
    alcBand_ = std::string(radio.modelName()) + " " + bandName(radio.freq.load());   // each radio has its own levels
    auto it = alcLevel_.find(alcBand_);
    float g = it != alcLevel_.end() ? it->second.level : (float)cfg.txLevel;
    txGain_ = std::min(g, (float)cfg.txLevel);
    alcSeen_ = radio.alcSeq.load();
    alcMax_ = 0;
    poMax_ = 0;
    alcReads_ = 0;
    alcCut_ = false;
}

void App::alcTick(int64_t mono) {
    if (!txOn_ || mono - pttOnMono_ < 800) return;
    uint32_t seq = radio.alcSeq.load();
    if (seq == alcSeen_) return;
    alcSeen_ = seq;
    if (!genStarted_) return;                                      // no audio yet: ALC says nothing
    int a = radio.alc.load();
    alcMax_ = std::max(alcMax_, a);
    poMax_ = std::max(poMax_, radio.po.load());
    alcReads_++;
    if (!cfg.autoAlc || radio.proto.load() == RP_QMX) return;
    float g = txGain_.load(), f = a > 180 ? 0.8f : a > 130 ? 0.9f : a > 115 ? 0.97f : 1.0f;
    if (f < 1.0f) {
        auto& L = alcLevel_[alcBand_];
        L.ceiling = g * 0.97f;
        txGain_ = std::max(1.0f, g * f);
        alcCut_ = true;
    }
}

void App::alcEndTx() {
    if (logFn) {
        char b[160];
        snprintf(b, sizeof b, "TX end %s: audio %.1f %% (max %d %%, auto %s), ALC max %d/255 over %d readings, Po max %d/255",
                 alcBand_.c_str(), txLevelNow(), cfg.txLevel, cfg.autoAlc ? "on" : "off", alcMax_, alcReads_, poMax_);
        logFn(b);
    }
    if (!cfg.autoAlc || radio.proto.load() == RP_QMX || alcBand_.empty()) return;
    auto& L = alcLevel_[alcBand_];
    if (L.ceiling <= 0) L.ceiling = (float)cfg.txLevel;
    float g = txGain_.load();
    if (!alcCut_ && alcMax_ < 70 && genStarted_) g = std::min({g * 1.06f, L.ceiling, (float)cfg.txLevel});
    bool changed = fabsf(L.level - g) >= 0.5f || alcCut_;
    L.level = g;
    if (changed) alcSave();                                        // kept across restarts (only when it moved, to spare the flash)
    if (alcCut_) status("TX audio lowered to " + std::to_string((int)lroundf(g)) + " % to keep the radio's ALC out of the red");
}

// Learned levels: one line per radio and band, "<radio> <band>|<level>|<ceiling>".
std::string App::alcPath() const { return dir_ + "/alc.txt"; }
void App::alcLoad() {
    alcLevel_.clear();
    FILE* f = fopen(alcPath().c_str(), "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char* a = strchr(line, '|');
        if (!a) continue;
        *a = 0;
        AlcLevel L;
        if (sscanf(a + 1, "%f|%f", &L.level, &L.ceiling) == 2 && L.level >= 1 && L.level <= 100) alcLevel_[line] = L;
    }
    fclose(f);
}
void App::alcSave() {
    FILE* f = fopen(alcPath().c_str(), "w");
    if (!f) return;
    for (auto& kv : alcLevel_)
        if (kv.second.level > 0) fprintf(f, "%s|%.1f|%.1f\n", kv.first.c_str(), kv.second.level, kv.second.ceiling);
    fclose(f);
}

void App::stopTx(const char* why) {
    if (txOn_) alcEndTx();
    txOn_ = false;
    radio.ptt(false);
    pttOffRepeat_ = clk::monoMs() + 300;            // belt and braces: send "receive" a second time
    txState_ = ST_IDLE;
    tuneReq_ = false;
    if (why && *why) status(why);
}

void App::txAudio48k(int16_t* out, size_t n) {
    // 100 % = just under full scale. The QMX makes RF from the audio's frequency, not its level, and
    // only keys up when the audio is above ~80 % of full scale (its "Rise threshold"), so it always
    // gets full level; its power is set on the QMX itself.
    const float amp = txLevelNow() * 320.0f;
    if (gen_ == GEN_FTX) {
        if (!genStarted_.exchange(true)) {
            // Align to the clock: negative = wait for the nominal start (slot + 0.5 s),
            // positive = we are late, begin part-way into the message.
            int64_t pos = (clk::utcMs() - txStartUtc_.load()) * 48;
            if (txCw_) cwSynth_.seek(pos); else synth_.seek(pos);
        }
        if (txCw_) {
            for (size_t i = 0; i < n; i++) out[i] = (int16_t)(amp * cwSynth_.next());
            if (cwSynth_.pos() >= cwSynth_.totalSamples() && !genDone_) { genDoneMono_ = clk::monoMs(); genDone_ = true; }
        } else {
            for (size_t i = 0; i < n; i++) out[i] = (int16_t)(amp * synth_.next());
            if (synth_.pos() >= synth_.totalSamples() && !genDone_) { genDoneMono_ = clk::monoMs(); genDone_ = true; }
        }
    } else if (gen_ == GEN_TUNE) {
        float dphi = 2 * kPi * cfg.txFreq / 48000.0f;
        for (size_t i = 0; i < n; i++) {
            out[i] = (int16_t)(amp * sinf(tunePhase_));
            tunePhase_ += dphi;
            if (tunePhase_ > 2 * kPi) tunePhase_ -= 2 * kPi;
        }
    } else {
        for (size_t i = 0; i < n; i++) out[i] = (int16_t)(amp * pskTx_.next());
        if (pskTx_.done() && !genDone_) { genDoneMono_ = clk::monoMs(); genDone_ = true; }
    }
}

void App::tick() {
    radio.poll();
    int64_t mono = clk::monoMs();
    std::lock_guard<std::mutex> lk(mtx_);

    if (pttOffRepeat_ && mono >= pttOffRepeat_ && txState_ == ST_IDLE) { pttOffRepeat_ = 0; radio.ptt(false); }
    alcTick(mono);
    bool conn = radio.connected();
    if (conn && !wasConn_ && cfg.keyWpm > 0) radio.setKeySpeed(cfg.keyWpm);
    wasConn_ = conn;
    if (bandReqHz_ && radio.connected()) {
        radio.setFreq((uint64_t)bandReqHz_);
        radio.setUsbData();
        radio.setRfPower(cfg.rfPower);
        bandReqHz_ = 0;
    }
    // Nobody watching: stop automatic transmissions (same idea as the WSJT-X "Tx watchdog").
    if (seq.txEnabled && cfg.watchdogMin > 0 && mono - lastUiMono_ > (int64_t)cfg.watchdogMin * 60000) {
        seq.txEnabled = false;
        watchdogTripped_ = true;
        status("TX watchdog: no browser connected, transmit disabled");
    }

    if (txState_ == ST_TX) {
        if (haltReq_) { stopTx("Halted"); return; }
        if (!usbSerial || !usbAudio) { stopTx("USB link to the radio lost during transmit"); return; }
        if (gen_ == GEN_TUNE && !tuneReq_) { stopTx(""); return; }
        if (mono - pttOnMono_ > maxTxMs_) { stopTx(gen_ == GEN_TUNE ? "Tune ended" : "TX timeout: transmitter forced off"); return; }
        if (genDone_ && mono - genDoneMono_.load() > 200) {          // let the USB audio buffer drain
            int gen = gen_;
            stopTx("");
            if (gen == GEN_FTX) { lastTx_ = txText_; seq.txDone(); }
        }
        return;
    }

    haltReq_ = false;
    if (!usbSerial || !usbAudio) { tuneReq_ = false; return; }
    if (tuneReq_) { maxTxMs_ = 10000; tunePhase_ = 0; startTx(GEN_TUNE); return; }

    if (mode_ == DM_PSK31) {
        if (!pskPending_.empty()) {
            pskTx_.start((float)pskFreq_.load(), 48000);
            pskTx_.queue(pskPending_);
            pskTx_.finish();
            lastTx_ = pskPending_;
            pskPending_.clear();
            maxTxMs_ = 180000;
            startTx(GEN_PSK);
        }
        return;
    }

    if (!clk::valid() || !radio.connected() || !seq.txEnabled) return;
    const int64_t L = slotLen();
    int64_t now = clk::utcMs(), slot = now / L, tIn = now - slot * L;
    const int64_t lateLimit = (mode_ == DM_FT4) ? 1000 : 2000;      // latest moment we may still start
    if (tIn > lateLimit || slot == txSlot_.load()) return;
    {   // The previous slot's decodes decide what we send: wait for them.
        std::lock_guard<std::mutex> jl(jobMtx_);
        if (jobPending_ || decoding_) return;
    }
    syncSeq();
    std::string text;
    if (!seq.nextTx((int)(slot & 1), text)) return;
    int64_t startUtc = slot * L + 500;
    if (mode_ == DM_CW) {
        // Morse has no error correction to cover a missing beginning, so a late start sends the
        // whole message later instead; it must still end before the other side starts decoding.
        double secs = cwSeconds(text, cfg.cwWpm);
        if (secs < 0 || secs > 12.6) {
            status("\"" + text + "\" does not fit a time slot at this CW speed - transmit disabled");
            seq.txEnabled = false;
            return;
        }
        if (now + 80 > startUtc) startUtc = now + 80;
        if (startUtc + (int64_t)(secs * 1000) > slot * L + 13300) return;       // too late for this slot: try the next one
        cwSynth_.start(text, cfg.cwWpm, (float)cfg.txFreq, 48000);
        txCw_ = true;
    } else {
        std::vector<uint8_t> tones;
        if (!ftxEncode(text, mode_ == DM_FT4, tones)) {
            status("Cannot encode \"" + text + "\" - transmit disabled");
            seq.txEnabled = false;
            return;
        }
        synth_.start(tones, mode_ == DM_FT4, (float)cfg.txFreq, 48000);
        txCw_ = false;
    }
    txText_ = text;
    txStartUtc_ = startUtc;
    txSlot_ = slot;
    maxTxMs_ = L - 300;
    startTx(GEN_FTX);
}

// ---- logging ------------------------------------------------------------------------------------
static bool appendAdif(const std::string& path, const std::string& rec) {
    FILE* probe = fopen(path.c_str(), "r");
    bool fresh = !probe;
    if (probe) fclose(probe);
    FILE* f = fopen(path.c_str(), "a");
    if (!f) return false;
    bool ok = true;
    if (fresh) ok = fputs(adifHeader().c_str(), f) >= 0;
    ok = ok && fputs(rec.c_str(), f) >= 0 && fputc('\n', f) != EOF;
    return (fclose(f) == 0) && ok;
}

void App::logQso(const QsoRecord& in) {              // mtx_ held
    QsoRecord q = in;
    q.myCall = cfg.call;
    q.myGrid = cfg.grid;
    q.op = cfg.op;
    q.mySig = cfg.mySig;
    q.mySigInfo = cfg.mySigInfo;
    if (!q.freqHz) q.freqHz = radio.freq.load() + (uint64_t)(mode_ == DM_PSK31 ? pskFreq_.load() : cfg.txFreq);
    std::string rec = adifRecord(q);
    // Every QSO goes to two files: the current log, which the operator can clear, and the
    // archive, which keeps everything. Clearing the current log therefore never loses a contact.
    bool saved = true;
    if (appendAdif(archivePath(), rec)) archiveCount_++;
    else { archiveBehind_ = true; saved = false; }
    if (appendAdif(logPath(), rec)) logCount_++;
    else saved = false;
    worked_.insert(q.call + "|" + bandName(q.freqHz));
    recent_.push_back(q);
    while (recent_.size() > 30) recent_.pop_front();
    status(saved ? "Logged " + q.call : "QSO with " + q.call + " could not be saved to the device (storage full?)");
    if (cfg.logProto != LOG_NONE && !cfg.logHost.empty()) {
        std::lock_guard<std::mutex> nl(netMtx_);
        if (netQ_.size() < 50) netQ_.push_back({rec, q});
        netCv_.notify_one();
    }
}

bool App::netOnce() {
    std::string rec, host;
    QsoRecord q;
    int proto, port;
    {
        std::lock_guard<std::mutex> nl(netMtx_);
        if (netQ_.empty()) return false;
        rec = netQ_.front().first;
        q = netQ_.front().second;
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        proto = cfg.logProto; host = cfg.logHost; port = cfg.logPort;
    }
    bool ok = false;
    if (proto == LOG_TCP_ADIF) {
        std::string line = rec + "\r\n";
        ok = netSendTcp(host, port, (const uint8_t*)line.data(), line.size());
    } else if (proto == LOG_UDP_ADIF) {
        ok = netSendUdp(host, port, (const uint8_t*)rec.data(), rec.size());
    } else if (proto == LOG_UDP_WSJTX) {
        auto hb = wsjtxHeartbeat("RigDeck", fwVersion);
        auto p5 = wsjtxQsoLogged("RigDeck", q);
        auto p12 = wsjtxLoggedAdif("RigDeck", rec);
        netSendUdp(host, port, hb.data(), hb.size());
        ok = netSendUdp(host, port, p5.data(), p5.size());
        ok = netSendUdp(host, port, p12.data(), p12.size()) && ok;
    } else ok = true;                                // forwarding was switched off: drop it
    std::lock_guard<std::mutex> nl(netMtx_);
    if (ok) { netQ_.pop_front(); netOk_++; }
    else netFail_++;
    return ok;
}

void App::netLoop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> nl(netMtx_);
            netCv_.wait(nl, [this] { return !netQ_.empty(); });
        }
        if (!netOnce()) {                            // keep it queued and try again in a while
            std::unique_lock<std::mutex> nl(netMtx_);
            netCv_.wait_for(nl, std::chrono::seconds(15));
        }
    }
}

// ---- web API ------------------------------------------------------------------------------------
void App::setMode(int m) {
    if (m < 0 || m > DM_CW || m == mode_) return;
    if (txState_ == ST_TX) haltReq_ = true;
    seq.txEnabled = false;
    seq.clearQso();
    mode_ = m;
    reinit_ = true;
    decodes_.clear();
}

std::string App::command(const std::string& c, const std::map<std::string, std::string>& a) {
    std::lock_guard<std::mutex> lk(mtx_);
    lastUiMono_ = clk::monoMs();
    auto ok = [] { return std::string("{\"ok\":true}"); };
    auto err = [](const std::string& e) { return "{\"ok\":false,\"err\":" + jstr(e) + "}"; };
    auto needCall = [&]() { return !validCall(cfg.call) || !validGrid(cfg.grid); };

    if (c == "time") {
        long long ms = atoll(arg(a, "ms").c_str());
        if (ms < 1600000000000LL) return err("bad time");
        // NTP and GPS win over the browser's clock; never step mid-transmission.
        if (ntpSynced || txState_ == ST_TX || clk::monoMs() - gpsLastMono.load() < 60000) return ok();
        if (!clk::valid() || llabs(ms - clk::utcMs()) > 150) clk::setUtcMs(ms, "browser");
        return ok();
    }
    if (c == "wifi") {                                               // home network: do=connect (saved settings) | disconnect
        std::string d = arg(a, "do");
        if (d != "connect" && d != "disconnect") return err("bad wifi action");
        if (d == "connect" && cfg.staSsid.empty()) return err("Enter the home network name first");
        if (!wifiControl) return err("not available");
        wifiControl(d == "connect");
        return ok();
    }
    if (c == "ping") return ok();                                    // operator activity only (feeds the TX watchdog)
    if (c == "trim") { clk::trim(atoi(arg(a, "ms").c_str())); return ok(); }
    if (c == "mode") {
        std::string m = arg(a, "m");
        setMode(m == "FT4" ? DM_FT4 : (m == "PSK31" ? DM_PSK31 : (m == "CW" ? DM_CW : DM_FT8)));
        return ok();
    }
    if (c == "band" || c == "freq") {
        long long hz = atoll(arg(a, "hz").c_str());
        if (hz < 100000 || hz > 470000000LL) return err("frequency out of range");
        if (txState_ == ST_TX) return err("not while transmitting");
        bandReqHz_ = hz;
        seq.clearQso();
        return ok();
    }
    // ---- front-panel style radio control (the "radio" screen) ----
    if (c == "rext") { radio.extPoll = arg(a, "v") == "1"; return ok(); }
    if (c == "qsy" || c == "rstep") {                                // change frequency only; mode is left alone
        long long hz = c == "qsy" ? atoll(arg(a, "hz").c_str()) : (long long)radio.freq.load() + atoll(arg(a, "d").c_str());
        if (hz < 30000 || hz > 470000000LL) return err("frequency out of range");
        if (txState_ == ST_TX) return err("not while transmitting");
        radio.setFreq((uint64_t)hz);
        radio.freq = (uint64_t)hz;                                   // show it at once; the radio's reply confirms
        return ok();
    }
    if (c == "rmode" || c == "rdata" || c == "rvfo") {
        if (txState_ == ST_TX) return err("not while transmitting");
        seq.txEnabled = false;                                       // leaving the digital setup: no automatic transmissions
        seq.clearQso();
        if (c == "rmode") radio.setMode((uint8_t)strtol(arg(a, "m").c_str(), nullptr, 16), (uint8_t)atoi(arg(a, "fil", "1").c_str()));
        else if (c == "rdata") radio.setData(arg(a, "v") == "1");
        else radio.vfo((uint8_t)strtol(arg(a, "v").c_str(), nullptr, 16));
        return ok();
    }
    if (c == "rlevel") {
        int sub = (int)strtol(arg(a, "sub").c_str(), nullptr, 16), v = atoi(arg(a, "v").c_str());
        if (sub < 1 || sub > 15 || v < 0 || v > 100) return err("bad level");
        radio.setLevel((uint8_t)sub, v);
        radio.level[sub] = v;
        return ok();
    }
    if (c == "rfunc") {
        int sub = (int)strtol(arg(a, "sub").c_str(), nullptr, 16), v = atoi(arg(a, "v").c_str());
        if (sub < 0 || sub >= 0x50 || v < 0 || v > 9) return err("bad function");
        radio.setFunc((uint8_t)sub, (uint8_t)v);
        return ok();
    }
    if (c == "ratt") { radio.setAtt((uint8_t)strtol(arg(a, "v").c_str(), nullptr, 16)); return ok(); }
    if (c == "txen") {
        bool on = arg(a, "v") == "1";
        if (on && needCall()) return err("Set your callsign and grid in Settings first");
        if (on && !clk::valid()) return err("Clock not set yet");
        seq.txEnabled = on;
        watchdogTripped_ = false;
        syncSeq();
        if (on && cfg.opMode == OP_AUTO_CQ && seq.dxCall.empty()) seq.startCq();
        if (!on) tuneReq_ = false;
        return ok();
    }
    if (c == "halt") {
        seq.halt();
        tuneReq_ = false;
        pskPending_.clear();
        if (txState_ == ST_TX) haltReq_ = true;
        return ok();
    }
    if (c == "op") {
        cfg.set("opMode", arg(a, "v"));
        cfg.save(dir_ + "/settings.txt");
        syncSeq();
        seq.setOpMode(cfg.opMode);
        return ok();
    }
    if (c == "cq") {
        if (needCall()) return err("Set your callsign and grid in Settings first");
        syncSeq();
        seq.startCq();
        return ok();
    }
    if (c == "call") {                                               // work the sender of a decoded message
        if (needCall()) return err("Set your callsign and grid in Settings first");
        uint32_t id = (uint32_t)atoll(arg(a, "id").c_str());
        for (auto& d : decodes_) {
            if (d.id != id) continue;
            ParsedMsg p;
            if (!parseMsg(d.text, p) || p.from.empty() || p.from == cfg.call) return err("Nothing to call in that message");
            syncSeq();
            if (p.to == cfg.call && p.kind != K_OTHER && p.kind != K_CQ) seq.respondTo(p, d.snr, d.parity);
            else seq.callStation(p.from, (p.kind == K_CQ || p.kind == K_GRID) ? p.grid : "", d.snr, d.parity);
            if (clk::valid()) seq.txEnabled = true;
            watchdogTripped_ = false;
            return ok();
        }
        return err("That decode has scrolled away");
    }
    if (c == "dx") {
        std::string call = upperTrim(arg(a, "call")), grid = upperTrim(arg(a, "grid"));
        if (!validCall(call)) return err("bad callsign");
        seq.dxCall = call;
        seq.dxGrid = isGrid4(grid) ? grid : "";
        if (!arg(a, "rpt").empty()) seq.rptSent = atoi(arg(a, "rpt").c_str());
        return ok();
    }
    if (c == "txmsg") { syncSeq(); seq.setTxMsg(atoi(arg(a, "n").c_str())); return ok(); }
    if (c == "free") {
        std::string t = upperTrim(arg(a, "text"));
        if (t.empty() || t.size() > 13) return err("free text is 1-13 characters");
        seq.freeText = t;
        seq.setTxMsg(7);
        return ok();
    }
    if (c == "clear") { seq.clearQso(); return ok(); }
    if (c == "logdx") {                                              // log the contact now, even unfinished, then CQ
        if (needCall()) return err("Set your callsign and grid in Settings first");
        std::string call = seq.dxCall;
        int r = seq.forceLog();
        if (r == -1) return err("No QSO in progress to log");
        if (r == -2) return err("No signal report sent or received yet; nothing to log");
        status(r == 1 ? call + " was already logged; calling CQ" : "Logged " + call + "; calling CQ");
        syncSeq();
        seq.startCq();
        if (txState_ == ST_TX) haltReq_ = true;                     // stop the call to them; CQ in the next slot
        if (clk::valid()) seq.txEnabled = true;
        watchdogTripped_ = false;
        return ok();
    }
    if (c == "txfreq") {
        if (!cfg.set("txFreq", arg(a, "hz"))) return err("200-2900 Hz");
        return ok();
    }
    if (c == "parity") {
        if (!cfg.set("cqParity", arg(a, "v"))) return err("0 or 1");
        seq.cqParity = seq.txParity = cfg.cqParity;
        return ok();
    }
    if (c == "tune") {
        if (arg(a, "v") == "1" && (!usbSerial || !usbAudio)) return err("Radio not connected");
        tuneReq_ = arg(a, "v") == "1";
        return ok();
    }
    if (c == "pskfreq") {
        int f = atoi(arg(a, "hz").c_str());
        if (f < 200 || f > 2900) return err("200-2900 Hz");
        pskFreq_ = f;
        pskFreqDirty_ = true;
        return ok();
    }
    if (c == "psktx") {
        if (needCall()) return err("Set your callsign and grid in Settings first");
        if (mode_ != DM_PSK31) return err("Switch to PSK31 first");
        std::string t = arg(a, "text");
        if (t.empty() || t.size() > 500) return err("1-500 characters");
        if (pskPending_.size() + t.size() > 1000) return err("transmit queue full");
        pskPending_ += t;                                            // sent as the next over
        return ok();
    }
    if (c == "logqso") {                                             // manual entry: the logger, PSK31, or a fix-up
        QsoRecord q;
        q.call = upperTrim(arg(a, "call"));
        if (!validCall(q.call)) return err("bad callsign");
        q.grid = upperTrim(arg(a, "grid"));
        if (!q.grid.empty() && !validGrid(q.grid)) return err("bad grid");
        // The logger names the mode; without one the digital mode in use is logged.
        std::string m = upperTrim(arg(a, "mode"));
        if (m == "AUTO") {                                           // whatever the radio is doing
            int rm = radio.mode.load();
            m = rm == 3 || rm == 7 ? "CW" : rm == 2 ? "AM" : rm == 5 ? "FM" : rm == 4 || rm == 8 ? "RTTY" : rm == 0x17 ? "DIGITALVOICE" : "SSB";
        }
        bool manual = !m.empty();
        if (m == "SSB" || m == "CW" || m == "AM" || m == "FM" || m == "RTTY" || m == "DIGITALVOICE") {
            q.mode = m;
        } else if (manual) return err("bad mode");
        else if (mode_ == DM_PSK31) { q.mode = "PSK"; q.submode = "PSK31"; }
        else if (mode_ == DM_FT4) { q.mode = "MFSK"; q.submode = "FT4"; }
        else if (mode_ == DM_CW) q.mode = "CW";
        else q.mode = "FT8";
        const char* dflt = q.mode == "SSB" || q.mode == "AM" || q.mode == "FM" || q.mode == "DIGITALVOICE" ? "59" : "599";
        q.rstSent = arg(a, "rs", dflt).substr(0, 6);
        q.rstRcvd = arg(a, "rr", dflt).substr(0, 6);
        if (q.rstSent.empty()) q.rstSent = dflt;
        if (q.rstRcvd.empty()) q.rstRcvd = dflt;
        q.name = arg(a, "name").substr(0, 30);
        q.comment = arg(a, "comment").substr(0, 60);
        if (manual) {                                                // the dial frequency, not dial + audio offset
            std::string ft = arg(a, "freq");                           // typed by the operator: MHz, kHz or Hz
            long long hz = atoll(arg(a, "hz", "0").c_str());
            if (!ft.empty()) {
                hz = (long long)parseDialFreq(ft);
                if (!hz) return err("Frequency not understood: enter MHz, e.g. 14.250");
            }
            q.freqHz = hz > 0 ? (uint64_t)hz : radio.freq.load();
            if (!q.freqHz) return err("Frequency unknown: connect the radio or enter a frequency");
            if (q.mode == "SSB") {                                      // the radio's sideband if it is on this frequency, else the convention
                int rm = radio.mode.load();
                if (hz <= 0 && (rm == 0 || rm == 1)) q.submode = rm == 0 ? "LSB" : "USB";
                else q.submode = q.freqHz < 10000000ULL && !(q.freqHz >= 5000000ULL && q.freqHz < 5600000ULL) ? "LSB" : "USB";
            }
        }
        if (!clk::valid()) return err("Clock not set yet");
        bool dupe = worked_.count(q.call + "|" + bandName(q.freqHz ? q.freqHz : radio.freq.load())) > 0;
        q.startMs = q.endMs = clk::utcMs();
        logQso(q);
        if (dupe) status("Logged " + q.call + " (worked before on this band)");
        return ok();
    }
    // ---- CW through the radio's own keyer (logger macros) ----
    if (c == "cwsend" || c == "macro") {
        if (!radio.connected()) return err("Radio not connected");
        if (txState_ == ST_TX) return err("not while transmitting");
        int rm = radio.mode.load();
        if (rm != 3 && rm != 7) return err("Radio is not in CW mode");
        std::string text;
        if (c == "macro") {
            int n = atoi(arg(a, "n").c_str());
            if (n < 1 || n > 6) return err("bad macro");
            text = expandMacro(cfg.macro[n - 1], upperTrim(arg(a, "call")), arg(a, "rs", "599"));
            if (text.find("{CALL}") != std::string::npos) return err("Enter a callsign first");
        } else text = arg(a, "text");
        if (upperTrim(text).empty()) return err("nothing to send");
        if (text.size() > Radio::cwMax(radio.proto)) return err("Too long for this radio's keyer");
        seq.txEnabled = false;
        radio.sendCw(text);
        status("CW: " + upperTrim(text));
        return ok();
    }
    if (c == "cwstop") { radio.stopCw(); status("CW stopped"); return ok(); }
    if (c == "cwspeed") {
        int w = atoi(arg(a, "wpm").c_str());
        if (w < 6 || w > 48) return err("speed must be 6 to 48 wpm");
        radio.setKeySpeed(w);
        return ok();
    }
    if (c == "clearlog") {                                           // start a new current log; the archive keeps everything
        if (arg(a, "confirm") != "yes") return err("confirm=yes required");
        if (archiveBehind_ && arg(a, "force") != "yes")
            return err("The archive is missing contacts that are in the current log. Download the current log first.");
        remove(logPath().c_str());
        recent_.clear();
        logCount_ = 0;
        archiveBehind_ = false;
        status("Current log cleared");
        return ok();
    }
    if (c == "logread") {                                            // a page of the log for editing, newest first
        int from = atoi(arg(a, "from").c_str()), n = atoi(arg(a, "n", "50").c_str());
        return logReadJson(arg(a, "file") == "all", from, std::max(1, std::min(n, 200)), arg(a, "q"));
    }
    if (c == "logedit" || c == "logdel") {                           // file=cur|all, i=index, call=expected call, f.NAME=value
        bool archive = arg(a, "file") == "all";
        if (arg(a, "i").empty()) return err("i required");
        std::map<std::string, std::string> f;
        for (auto& kv : a) if (kv.first.size() > 2 && !kv.first.compare(0, 2, "f.")) f[kv.first.substr(2)] = kv.second;
        if (c == "logedit" && f.empty()) return err("nothing to change");
        std::string call = arg(a, "call");
        std::string e = logChange(archive, atoi(arg(a, "i").c_str()), call, c == "logedit" ? &f : nullptr);
        if (!e.empty()) return err(e);
        status(c == "logedit" ? "Contact updated" : "Contact " + call + " deleted");
        return ok();
    }
    if (c == "eraselogs") {                                          // current log and archive, and worked-before with them
        if (arg(a, "confirm") != "yes") return err("confirm=yes required");
        remove(logPath().c_str());
        remove(archivePath().c_str());
        recent_.clear();
        worked_.clear();
        logCount_ = archiveCount_ = 0;
        archiveBehind_ = false;
        status("All logs erased");
        return ok();
    }
    if (c == "set") {
        Settings n = cfg;
        std::string bad;
        bool wifi = false;
        for (auto& kv : a) {
            if (kv.first == "c") continue;
            if (!n.set(kv.first, kv.second)) bad += (bad.empty() ? "" : ", ") + kv.first;
            else if (kv.first.compare(0, 2, "ap") == 0 || kv.first.compare(0, 3, "sta") == 0) wifi = true;
        }
        if (!bad.empty()) return err("Invalid value for: " + bad);
        // The hotspot's own name/password need a restart; the home network is re-joined right away.
        bool wifiChanged = wifi && (n.apSsid != cfg.apSsid || n.apPass != cfg.apPass);
        bool staChanged = wifi && (n.staSsid != cfg.staSsid || n.staPass != cfg.staPass);
        bool alcReset = n.txLevel != cfg.txLevel || n.autoAlc != cfg.autoAlc;   // start again from the new level
        cfg = n;
        if (alcReset) { alcLevel_.clear(); alcSave(); }
        radio.addr = (uint8_t)cfg.civAddr;
        syncSeq();
        if (!cfg.save(dir_ + "/settings.txt")) return err("Could not save settings");
        if (staChanged && wifiControl) wifiControl(!cfg.staSsid.empty());
        return std::string("{\"ok\":true,\"restart\":") + jbool(wifiChanged) + "}";
    }
    if (c == "reboot") {
        if (txState_ == ST_TX) haltReq_ = true;
        seq.txEnabled = false;
        rebootReq = true;
        return ok();
    }
    return err("unknown command");
}

void App::uiState(UiState& u) {
    std::lock_guard<std::mutex> lk(mtx_);
    int64_t L = slotLen();
    u.utc = clk::utcMs();
    u.clkOk = clk::valid();
    u.clkSrc = clk::source();
    u.ser = usbSerial; u.aud = usbAudio; u.civ = radio.connected(); u.ptt = radio.pttState;
    u.mode = mode_; u.op = cfg.opMode;
    u.freq = radio.freq.load(); u.band = bandName(u.freq);
    u.rmode = radio.mode; u.data = radio.dataMode;
    u.model = radio.connected() ? radio.modelName() : "";
    u.wpm = radio.keySpeed;
    u.txLvl = (int)lroundf(txLevelNow());
    u.staState = sta_.state; u.staIp = sta_.ip; u.staReason = sta_.reason; u.staRssi = sta_.rssi;
    u.smeter = radio.smeter; u.po = radio.po; u.swr = radio.swr; u.alc = radio.alc;
    u.slotLen = (int)L; u.slotT = (int)(u.utc % L);
    u.txEn = seq.txEnabled; u.txOn = txState_ == ST_TX; u.tune = tuneReq_; u.wd = watchdogTripped_;
    u.txMsg = seq.txMsg; u.txPar = seq.txParity; u.txFreq = cfg.txFreq; u.tries = seq.retries;
    u.txText = u.txOn ? (gen_ == GEN_TUNE ? "TUNE" : (gen_ == GEN_PSK ? "PSK31" : txText_)) : "";
    u.nextText = seq.compose(seq.txMsg);
    u.dx = seq.dxCall; u.grid = seq.dxGrid; u.rptSent = seq.rptSent; u.rptRcvd = seq.rptRcvd;
    int pk = rxPeak_.load();
    u.lvlDb = pk > 0 ? (int)lroundf(20 * log10f(pk / 32768.0f)) : -99;
    u.decId = nextId_; u.decodeMs = decodeMs_;
    u.logN = logCount_; u.logAll = archiveCount_; u.netOk = netOk_; u.netFail = netFail_;
    u.status = memFail ? "Not enough RAM for the decoder" : status_;
    u.net = netInfo_; u.call = cfg.call; u.myGrid = cfg.grid;
}

bool App::uiWorked(const std::string& call) {
    std::lock_guard<std::mutex> lk(mtx_);
    return worked_.count(upperTrim(call) + "|" + bandName(radio.freq.load())) > 0;
}

void App::uiLogTail(size_t max, std::vector<UiLogEntry>& out) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::string> recs;
    adifReadRecords(logPath(), recs);
    for (size_t i = recs.size(); i > 0 && out.size() < max; i--) {
        const std::string& r = recs[i - 1];
        UiLogEntry e;
        e.idx = (int)i - 1;
        e.call = adifField(r, "CALL");
        e.date = adifField(r, "QSO_DATE"); e.time = adifField(r, "TIME_ON");
        if (e.date.size() == 8 && e.time.size() >= 4) e.when = e.date.substr(4, 2) + "-" + e.date.substr(6, 2) + " " + e.time.substr(0, 2) + ":" + e.time.substr(2, 2);
        e.grid = adifField(r, "GRIDSQUARE"); e.band = adifField(r, "BAND");
        e.mode = adifField(r, "SUBMODE");
        if (e.mode.empty()) e.mode = adifField(r, "MODE");
        e.sent = adifField(r, "RST_SENT"); e.rcvd = adifField(r, "RST_RCVD");
        e.freq = adifField(r, "FREQ"); e.name = adifField(r, "NAME"); e.comment = adifField(r, "COMMENT");
        out.push_back(e);
    }
}

// The time in an NMEA sentence is the second it describes; the receiver starts sending it a
// little later (cfg.gpsDelayMs). After the first fix the clock is only nudged when the averaged
// error passes 25 ms, and never while transmitting, so slot timing does not jitter.
void App::gpsRx(const uint8_t* data, size_t n) {
    int64_t mono = clk::monoMs();
    if (clk::valid()) nmea_.setDateHint(clk::utcMs());
    if (!nmea_.feed(data, n, mono)) return;
    if (!cfg.gpsClock || txOn_) return;
    if (cfg.gpsModule && mono - gpsLastMono.load() < 5000 && !strncmp(clk::source(), "gps-", 4)) return;   // the module's PPS is better
    gpsTime(nmea_.utcMs() + cfg.gpsDelayMs + (mono - nmea_.sentenceStartMono()), mono, "gps");
}

// GPS module on the header. With PPS wired, the pulse marks the start of the second the next
// sentence names (u-blox, ATGM and most others), which pins the clock to within a millisecond or two.
// Without PPS it is handled like the radio's GPS (sentence arrival + cfg.gpsDelayMs).
void App::gpsModuleRx(const uint8_t* data, size_t n) {
    int64_t mono = clk::monoMs();
    if (clk::valid()) nmeaMod_.setDateHint(clk::utcMs());
    if (!nmeaMod_.feed(data, n, mono)) return;
    if (!cfg.gpsModule || txOn_) return;
    uint32_t pc = ppsCount.load();
    int64_t sentence = nmeaMod_.sentenceStartMono();
    int32_t sincePps = (int32_t)((uint32_t)sentence - ppsMono32.load());
    if (cfg.gpsPpsPin >= 0 && pc != ppsUsed_ && sincePps >= 0 && sincePps < 950) {
        ppsUsed_ = pc;
        int64_t ppsMono = sentence - sincePps;
        int64_t utcNow = nmeaMod_.utcMs() + (mono - ppsMono);
        int64_t diff = utcNow - clk::utcMs();
        if (!clk::valid() || strcmp(clk::source(), "gps-pps") != 0 || llabs(diff) > 2) clk::setUtcMs(utcNow, "gps-pps");
        gpsLastMono = mono;
        return;
    }
    // Pulses seen in the last 3 s but none for this sentence: keep the PPS time rather than step
    // to the rougher sentence timing. Only when the pulses stop does the sentence timing take over.
    if (cfg.gpsPpsPin >= 0 && pc != 0 && (int32_t)((uint32_t)mono - ppsMono32.load()) < 3000 && !strcmp(clk::source(), "gps-pps")) {
        gpsLastMono = mono;
        return;
    }
    gpsTime(nmeaMod_.utcMs() + cfg.gpsDelayMs + (mono - sentence), mono, "gps-nmea");
}

void App::gpsTime(int64_t utcNow, int64_t mono, const char* src) {
    int64_t diff = utcNow - clk::utcMs();
    if (!clk::valid() || llabs(diff) > 1000 || strcmp(clk::source(), src) != 0) {
        clk::setUtcMs(utcNow, src);
        gpsErr_ = 0;
    } else {
        gpsErr_ = 0.8f * gpsErr_ + 0.2f * (float)diff;         // average out arrival-time jitter
        if (gpsErr_ > 25 || gpsErr_ < -25) { clk::trim((int)gpsErr_); gpsErr_ = 0; }
    }
    gpsLastMono = mono;
}

void App::uiDecodes(uint32_t since, std::vector<UiDecode>& out) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& d : decodes_) {
        if (d.id <= since) continue;
        UiDecode r{d.id, d.slotMs, d.snr, d.dt, (int)lroundf(d.freq), d.text, false, false};
        r.cq = d.text.compare(0, 3, "CQ ") == 0;
        r.toMe = !cfg.call.empty() && d.text.compare(0, cfg.call.size() + 1, cfg.call + " ") == 0;
        out.push_back(r);
    }
}

std::string App::spectrumJson() {
    uint8_t b[Spectrum::BINS];
    uint32_t seq;
    spec_.snapshot(b, seq);
    static const char hex[] = "0123456789abcdef";
    std::string o = "{\"seq\":" + jnum(seq) + ",\"hz\":11.71875,\"d\":\"";
    for (int i = 0; i < Spectrum::BINS; i++) { o += hex[b[i] >> 4]; o += hex[b[i] & 15]; }
    return o + "\"}";
}

std::string App::stateJson(uint32_t sinceDecode, uint32_t sincePsk, bool withCfg) {
    std::lock_guard<std::mutex> lk(mtx_);
    lastUiMono_ = clk::monoMs();
    int64_t now = clk::utcMs(), L = slotLen();
    uint64_t f = radio.freq.load();
    std::string o = "{";
    o += "\"utc\":" + jnum(now);
    o += std::string(",\"clk\":{\"ok\":") + jbool(clk::valid()) + ",\"src\":" + jstr(clk::source()) + ",\"age\":" + jnum(clk::ageMs() / 1000) + "}";
    o += std::string(",\"mode\":\"") + modeName(mode_) + "\",\"op\":" + jnum(cfg.opMode);
    o += std::string(",\"radio\":{\"ser\":") + jbool(usbSerial) + ",\"aud\":" + jbool(usbAudio) + ",\"civ\":" + jbool(radio.connected());
    o += ",\"model\":" + jstr(radio.connected() ? radio.modelName() : "") + ",\"freq\":" + jnum((long long)f) + ",\"band\":" + jstr(bandName(f)) + ",\"rmode\":" + jnum(radio.mode.load()) + ",\"data\":" + jnum(radio.dataMode.load());
    o += std::string(",\"ptt\":") + jbool(radio.pttState) + ",\"s\":" + jnum(radio.smeter.load()) + ",\"po\":" + jnum(radio.po.load());
    o += ",\"swr\":" + jnum(radio.swr.load()) + ",\"alc\":" + jnum(radio.alc.load()) + ",\"ng\":" + jnum(radio.ng.load());
    o += ",\"proto\":" + jnum(radio.proto.load()) + ",\"wpm\":" + jnum(radio.keySpeed.load()) + "}";
    o += ",\"slot\":{\"len\":" + jnum(L) + ",\"t\":" + jnum(now % L) + ",\"par\":" + jnum((now / L) & 1) + "}";
    o += std::string(",\"tx\":{\"en\":") + jbool(seq.txEnabled) + ",\"on\":" + jbool(txState_ == ST_TX) + ",\"msg\":" + jnum(seq.txMsg) + ",\"par\":" + jnum(seq.txParity);
    o += ",\"text\":" + jstr(txState_ == ST_TX ? (gen_ == GEN_TUNE ? "TUNE" : (gen_ == GEN_PSK ? "PSK31" : txText_)) : "") + ",\"last\":" + jstr(lastTx_);
    o += ",\"freq\":" + jnum(cfg.txFreq) + ",\"lvl\":" + jnum(cfg.txLevel) + ",\"txLvl\":" + jnum((int)lroundf(txLevelNow())) + std::string(",\"tune\":") + jbool(tuneReq_) + std::string(",\"wd\":") + jbool(watchdogTripped_) + "}";
    o += ",\"qso\":{\"dx\":" + jstr(seq.dxCall) + ",\"grid\":" + jstr(seq.dxGrid) + ",\"rs\":" + jnum(seq.rptSent) + ",\"rr\":" + jnum(seq.rptRcvd) + ",\"try\":" + jnum(seq.retries) + ",\"m\":[";
    for (int i = 1; i <= 7; i++) o += (i > 1 ? "," : "") + jstr(seq.compose(i));
    o += "]}";
    int pk = rxPeak_.load();
    o += ",\"lvl\":" + jnum(pk > 0 ? (long long)lroundf(20 * log10f(pk / 32768.0f)) : -99);
    o += ",\"dec\":[";
    bool first = true;
    for (auto& d : decodes_) {
        if (d.id <= sinceDecode) continue;
        char b[96];
        snprintf(b, sizeof b, "%s{\"id\":%u,\"t\":%lld,\"p\":%d,\"snr\":%d,\"dt\":%.1f,\"f\":%d,\"m\":", first ? "" : ",", (unsigned)d.id, (long long)d.slotMs, d.parity, d.snr, d.dt, (int)lroundf(d.freq));
        o += b + jstr(d.text) + "}";
        first = false;
    }
    o += "],\"decId\":" + jnum(nextId_) + ",\"decMs\":" + jnum(decodeMs_.load());
    o += ",\"psk\":{\"seq\":" + jnum(pskSeq_) + ",\"f\":" + jnum(pskFreq_.load()) + ",\"afc\":" + jnum((long long)lroundf(pskRx_.freq())) + ",\"q\":" + jnum((long long)(pskRx_.quality() * 100)) + ",\"text\":";
    uint32_t fresh = pskSeq_ - sincePsk;
    if (sincePsk > pskSeq_ || fresh > pskText_.size()) fresh = (uint32_t)pskText_.size();
    o += jstr(pskText_.substr(pskText_.size() - fresh)) + "}";
    o += ",\"log\":{\"n\":" + jnum(logCount_) + ",\"all\":" + jnum(archiveCount_) + std::string(",\"behind\":") + jbool(archiveBehind_) + ",\"ok\":" + jnum(netOk_.load()) + ",\"fail\":" + jnum(netFail_.load()) + ",\"last\":[";
    first = true;
    for (auto& q : recent_) {
        o += std::string(first ? "" : ",") + "{\"call\":" + jstr(q.call) + ",\"grid\":" + jstr(q.grid) + ",\"t\":" + jnum(q.endMs) + ",\"band\":" + jstr(bandName(q.freqHz));
        o += ",\"mode\":" + jstr(q.submode.empty() ? q.mode : q.submode) + ",\"rs\":" + jstr(q.rstSent) + ",\"rr\":" + jstr(q.rstRcvd) + "}";
        first = false;
    }
    o += "]}";
    o += ",\"msg\":" + jstr(memFail ? "Not enough RAM for the decoder - this firmware needs a board with PSRAM" : status_) + ",\"net\":" + jstr(netInfo_) + ",\"ver\":" + jstr(fwVersion) +
         ",\"sta\":{\"state\":" + jstr(sta_.state) + ",\"ssid\":" + jstr(sta_.ssid) + ",\"ip\":" + jstr(sta_.ip) +
         ",\"rssi\":" + jnum(sta_.rssi) + ",\"reason\":" + jstr(sta_.reason) + "}" +
         ",\"wchip\":{\"ver\":" + jstr(wchipVer_) + ",\"upd\":" + jstr(wchipUpd_) + "}";
    if (withCfg) o += ",\"cfg\":" + cfg.json();
    return o + "}";
}
