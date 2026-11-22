#include "app.h"
#include <ctime>
#include <thread>
#include "clock.h"
#include "netlog.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <strings.h>
#include <cmath>
#include <chrono>
#include <set>
#include <sys/stat.h>
#include <dirent.h>

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

void App::begin(const std::string& dataDir, const std::string& logDir) {
    dir_ = dataDir;
    logOnSd_ = !logDir.empty() && logDir != dataDir;
    logDir_ = logOnSd_ ? logDir : dataDir;
    if (logOnSd_) mkdir(logDir_.c_str(), 0777);
    cfg.load(dir_ + "/settings.txt");
    alcLoad();
    radio.addr = (uint8_t)cfg.civAddr;
    seq.now = [] { return clk::utcMs(); };
    seq.workedBefore = [this](const std::string& call) {
        return worked_.contains(call + "|" + bandName(radio.freq.load()));
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
    logOpen();
    storRefresh();
}

// ---- log files ----------------------------------------------------------------------------------
static bool fileExists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }
static long fileSize(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 ? (long)st.st_size : 0; }

// Moves the contents of src onto the end of dst (or renames it when dst does not exist yet and both
// are on the same store), then removes src. ADIF files: record lines only (dst gets a header if new).
static bool moveInto(const std::string& src, const std::string& dst, bool adif) {
    if (src == dst || !fileExists(src)) return true;
    if (!fileExists(dst) && rename(src.c_str(), dst.c_str()) == 0) return true;
    bool fresh = !fileExists(dst);
    FILE* out = fopen(dst.c_str(), "a");
    if (!out) return false;
    bool ok = true;
    if (adif) {
        if (fresh) ok = fputs(adifHeader().c_str(), out) >= 0;
        adifForEach(src, [&](std::string_view r, int, long) {
            ok = ok && fwrite(r.data(), 1, r.size(), out) == r.size() && fputc('\n', out) != EOF;
            return ok;
        });
    } else {
        FILE* in = fopen(src.c_str(), "rb");
        if (in) {
            char buf[1024];
            size_t n;
            while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, n, out) == n;
            fclose(in);
        }
    }
    ok = (fclose(out) == 0) && ok;
    if (ok) remove(src.c_str());
    return ok;
}

// The callsign as it appears in file names: "VE3/AC9HP/P" -> "VE3-AC9HP-P"; no callsign -> NOCALL.
static std::string fileCall(const std::string& call) {
    std::string o;
    for (char ch : upperTrim(call)) o += isalnum((unsigned char)ch) ? ch : '-';
    return o.empty() ? "NOCALL" : o.substr(0, 12);           // SPIFFS names: 31 characters at most
}

std::string App::logFile(bool archive) { std::lock_guard<std::mutex> lk(mtx_); return archive ? archivePath() : logPath(); }
std::string App::station() { std::lock_guard<std::mutex> lk(mtx_); return station_; }

static const char* const kLogKinds[] = {"log.adi", "archive.adi", "deleted.adi", "sent.txt"};

// Chooses the files of the station callsign now in Settings and loads them. On the way in:
//  - files from before per-station names (log.adi, archive.adi ...) become this station's;
//  - contacts logged while no callsign was set (NOCALL_*) become this station's;
//  - with the card in use, anything logged internally for this station (e.g. while the card was
//    out) is moved onto the card.
void App::logOpen() {
    station_ = fileCall(cfg.call);
    for (const char* k : kLogKinds) {
        bool adif = strcmp(k, "sent.txt") != 0;
        std::string dst = stationFile(k);
        moveInto(dir_ + "/" + k, dst, adif);
        if (station_ != "NOCALL") {
            moveInto(logDir_ + "/NOCALL_" + k, dst, adif);
            if (logOnSd_) moveInto(dir_ + "/NOCALL_" + k, dst, adif);
        }
        if (logOnSd_) moveInto(dir_ + "/" + station_ + "_" + k, dst, adif);
    }
    {
        std::lock_guard<std::mutex> nl(netMtx_);
        syncFile_ = -1;
        syncPending_ = false;
    }
    sentLoad();
    logReload();
    if (!logOnSd_) trashPurge();
    archiveBehind_ = logCount_ > archiveCount_;
    if (logFn) logFn(("log: " + logPath() + " (" + std::to_string(logCount_) + " contacts), archive " + std::to_string(archiveCount_)).c_str());
}

// After the files changed (edit, delete, switch): worked-before, counts, the recent list and the
// unsent count, streaming through both files once.
void App::logReload() {
    worked_.clear();
    archiveCount_ = logCount_ = 0;
    adifForEach(archivePath(), [&](std::string_view r, int, long) {
        worked_.bulkAdd(adifField(r, "CALL") + "|" + adifField(r, "BAND"));
        archiveCount_++;
        return true;
    });
    std::deque<PStr> last;
    int unsent = 0;
    {
        std::lock_guard<std::mutex> nl(netMtx_);
        adifForEach(logPath(), [&](std::string_view r, int, long) {
            worked_.bulkAdd(adifField(r, "CALL") + "|" + adifField(r, "BAND"));
            logCount_++;
            if (!sent_.contains(sentKey(r))) unsent++;
            last.push_back(pstr(r));
            if (last.size() > 30) last.pop_front();
            return true;
        });
    }
    worked_.finish();
    recent_.clear();
    for (auto& r : last) recent_.push_back(adifToQso(r));
    unsent_ = unsent;
}

// Edit (fields) or delete (fields == nullptr) record idx of the current log or the archive. The
// same record in the other file (identical text) changes with it, so the two stay in step.
// expectCall guards against the list having changed since it was read.
std::string App::logChange(bool archive, int idx, const std::string& expectCall,
                           const std::map<std::string, std::string>* fields) {
    std::string path = archive ? archivePath() : logPath(), other = archive ? logPath() : archivePath();
    std::string orig;
    bool found = false;
    adifForEach(path, [&](std::string_view r, int i, long) {
        if (i != idx) return true;
        orig = sstr(r); found = true;
        return false;
    });
    if (!found || idx < 0) return "That contact is no longer in the log; reload the list";
    if (!expectCall.empty() && strcasecmp(adifField(orig, "CALL").c_str(), expectCall.c_str()))
        return "The log changed since the list was read; reload it";
    std::string repl;
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
    }
    auto change = [&](const std::string& file, int at) {
        return adifRewrite(file, [&](std::string_view, int i, std::string& out) {
            if (i != at) return true;
            out = repl;                               // empty = delete
            return false;
        });
    };
    if (change(path, idx) < 0) return "Could not save the log (storage full?)";
    int otherIdx = -1;
    adifForEach(other, [&](std::string_view r, int i, long) { if (r == orig) otherIdx = i; return true; });
    bool inOther = otherIdx >= 0;
    if (inOther) change(other, otherIdx);
    if (!fields) {                                    // into the deleted contacts (internal store: for a week)
        AdifFields f = adifParse(orig);
        int64_t now = clk::valid() ? clk::utcMs() : 0;
        char ts[24] = "0";
        if (now) { time_t t = (time_t)(now / 1000); struct tm tm; gmtime_r(&t, &tm); strftime(ts, sizeof ts, "%Y%m%d%H%M%S", &tm); }
        f.emplace_back("APP_RIGDECK_DELETED", ts);
        f.emplace_back("APP_RIGDECK_FROM", inOther ? "both" : (archive ? "all" : "cur"));
        bool fresh = !fileExists(trashPath());
        FILE* fp = fopen(trashPath().c_str(), "a");
        if (fp) {
            if (fresh) fputs(adifHeader().c_str(), fp);
            fputs(adifBuild(f).c_str(), fp); fputc('\n', fp); fclose(fp);
        }
        if (!logOnSd_) trashPurge();
    }
    {
        std::lock_guard<std::mutex> nl(netMtx_);
        syncOff_ = 0;                                 // the file changed under a "send unsent": start its scan again
    }
    logReload();
    return "";
}

// Deleted contacts carry the time of deletion and the files they came from. On the internal store
// they are dropped after 7 days (only when the clock is known; an entry deleted without a clock
// stays until the clock is set and its week starts then). On the card they are kept for good.
void App::trashPurge() {
    if (!clk::valid() || logOnSd_) return;
    time_t now = (time_t)(clk::utcMs() / 1000);
    char nowTs[24];
    { struct tm tm; gmtime_r(&now, &tm); strftime(nowTs, sizeof nowTs, "%Y%m%d%H%M%S", &tm); }
    auto verdict = [&](std::string_view r) {      // 0 keep, 1 drop, 2 restamp
        std::string d = adifField(r, "APP_RIGDECK_DELETED");
        if (d == "0" || d.size() != 14) return 2;
        QsoRecord q = adifToQso("<QSO_DATE:8>" + d.substr(0, 8) + " <TIME_ON:6>" + d.substr(8) + " <CALL:1>X <EOR>");
        return now * 1000LL - q.startMs > 7LL * 86400000LL ? 1 : 0;
    };
    bool changed = false;
    adifForEach(trashPath(), [&](std::string_view r, int, long) { changed = verdict(r) != 0; return !changed; });
    if (!changed) return;
    int left = adifRewrite(trashPath(), [&](std::string_view r, int, std::string& out) {
        int v = verdict(r);
        if (v == 0) return true;
        if (v == 2) { AdifFields f = adifParse(r); adifSetField(f, "APP_RIGDECK_DELETED", nowTs); out = adifBuild(f); }
        return false;
    });
    if (left == 0) remove(trashPath().c_str());
}

void App::uiTrashNewest(UiLogEntry& e, int& count) {
    std::lock_guard<std::mutex> lk(mtx_);
    trashPurge();
    std::string last;
    count = 0;
    adifForEach(trashPath(), [&](std::string_view r, int, long) { last = sstr(r); count++; return true; });
    e = UiLogEntry();
    if (!count) return;
    std::string_view r = last;
    e.idx = count - 1;
    e.call = adifField(r, "CALL");
    e.date = adifField(r, "QSO_DATE"); e.time = adifField(r, "TIME_ON");
    if (e.date.size() == 8 && e.time.size() >= 4) e.when = e.date.substr(0, 4) + "-" + e.date.substr(4, 2) + "-" + e.date.substr(6, 2) + " " + e.time.substr(0, 2) + ":" + e.time.substr(2, 2);
    e.band = adifField(r, "BAND");
}

// Newest first, at most 100 (the card keeps every deleted contact; the list shows the latest).
std::string App::trashJson() {
    trashPurge();
    std::deque<std::pair<int, PStr>> last;
    int total = 0;
    adifForEach(trashPath(), [&](std::string_view r, int i, long) {
        last.emplace_back(i, pstr(r));
        if (last.size() > 100) last.pop_front();
        total++;
        return true;
    });
    std::string o = "{\"ok\":true,\"total\":" + jnum(total) + ",\"keep\":" + jbool(logOnSd_) + ",\"items\":[";
    for (size_t k = last.size(); k > 0; k--) {
        o += std::string(k == last.size() ? "" : ",") + "{\"i\":" + jnum(last[k - 1].first) + ",\"f\":{";
        bool first = true;
        for (auto& kv : adifParse(last[k - 1].second)) { o += std::string(first ? "" : ",") + jstr(kv.first) + ":" + jstr(kv.second); first = false; }
        o += "}}";
    }
    return o + "]}";
}

// Put rec into an ADIF file after the last contact that started no later than it.
static bool insertByTime(const std::string& path, const std::string& rec) {
    std::string key = adifStartKey(rec);
    int after = -1;
    adifForEach(path, [&](std::string_view r, int i, long) { if (adifStartKey(r) <= key) after = i; return true; });
    bool done = false;
    int n = adifRewrite(path, [&](std::string_view r, int i, std::string& out) {
        if (after == -1 && i == 0) { out = rec + "\n" + sstr(r); done = true; return false; }
        if (i == after) { out = sstr(r) + "\n" + rec; done = true; return false; }
        return true;
    }, "");
    if (n < 0) return false;
    if (!done) {                                       // empty or missing file
        bool fresh = !fileExists(path);
        FILE* f = fopen(path.c_str(), "a");
        if (!f) return false;
        if (fresh) fputs(adifHeader().c_str(), f);
        bool ok = fputs(rec.c_str(), f) >= 0 && fputc('\n', f) != EOF;
        return (fclose(f) == 0) && ok;
    }
    return true;
}

// Put a deleted contact back where it came from (in time order) and take it off the list.
std::string App::trashRestore(int idx) {
    std::string orig;
    adifForEach(trashPath(), [&](std::string_view r, int i, long) { if (i != idx) return true; orig = sstr(r); return false; });
    if (orig.empty() || idx < 0) return "That contact is no longer in the deleted list";
    AdifFields f = adifParse(orig);
    std::string from = "cur";
    for (auto& kv : f) if (kv.first == "APP_RIGDECK_FROM") from = kv.second;
    adifSetField(f, "APP_RIGDECK_DELETED", "");
    adifSetField(f, "APP_RIGDECK_FROM", "");
    std::string rec = adifBuild(f);
    bool ok = true;
    if (from == "cur" || from == "both") ok = insertByTime(logPath(), rec) && ok;
    if (from == "all" || from == "both") ok = insertByTime(archivePath(), rec) && ok;
    if (!ok) return "Could not save the log (storage full?)";
    int left = adifRewrite(trashPath(), [&](std::string_view, int i, std::string&) { return i != idx; });
    if (left == 0) remove(trashPath().c_str());
    {
        std::lock_guard<std::mutex> nl(netMtx_);
        syncOff_ = 0;
    }
    logReload();
    return "";
}

// One page of a log, newest first: {"total":N,"items":[{"i":index,"f":{"CALL":"..",..}}]}.
// filter (optional) matches the callsign (contains, any case). Two passes over the file: which
// records match, then the page itself.
std::string App::logReadJson(bool archive, int from, int n, const std::string& filter) {
    std::string path = archive ? archivePath() : logPath(), flt = upperTrim(filter);
    std::vector<int, PsAlloc<int>> match;            // file indices that match, oldest first
    int total = 0;
    adifForEach(path, [&](std::string_view r, int i, long) {
        if (flt.empty()) total++;
        else if (upperTrim(adifField(r, "CALL")).find(flt) != std::string::npos) match.push_back(i);
        return true;
    });
    if (!flt.empty()) total = (int)match.size();
    std::vector<int> want;                             // file index of each item on the page, in page order
    for (int k = std::max(0, from); k < total && k < from + n; k++)
        want.push_back(flt.empty() ? total - 1 - k : match[match.size() - 1 - k]);
    std::map<int, std::string> items;
    if (!want.empty()) {
        int lo = *std::min_element(want.begin(), want.end()), hi = *std::max_element(want.begin(), want.end());
        std::set<int> wanted(want.begin(), want.end());
        adifForEach(path, [&](std::string_view r, int i, long) {
            if (i > hi) return false;
            if (i < lo || !wanted.count(i)) return true;
            std::string o = "{\"i\":" + jnum(i) + ",\"f\":{";
            bool first = true;
            for (auto& kv : adifParse(r)) { o += std::string(first ? "" : ",") + jstr(kv.first) + ":" + jstr(kv.second); first = false; }
            items[i] = o + "}}";
            return true;
        });
    }
    std::string o = "{\"ok\":true,\"total\":" + jnum((long long)total) + ",\"items\":[";
    bool first = true;
    for (int i : want) {
        auto it = items.find(i);
        if (it == items.end()) continue;
        o += std::string(first ? "" : ",") + it->second;
        first = false;
    }
    return o + "]}";
}

// Every RigDeck log file of every station on the log store (and old ones left internally).
int App::eraseAllLogs() {
    auto isLog = [](const std::string& n) {
        auto ends = [&](const char* s) { size_t l = strlen(s); return n.size() >= l && n.compare(n.size() - l, l, s) == 0; };
        if (n == "log.adi" || n == "archive.adi" || n == "deleted.adi" || n == "sent.txt") return true;
        size_t u = n.find('_');
        if (u == std::string::npos || u == 0) return false;
        std::string rest = n.substr(u + 1);
        return rest == "log.adi" || rest == "archive.adi" || rest == "sent.txt" || rest.compare(0, 7, "deleted") == 0 ||
               ends(".adi.tmp");
    };
    int removed = 0;
    std::vector<std::string> dirs = {logDir_};
    if (logOnSd_) dirs.push_back(dir_);
    for (auto& d : dirs) {
        std::vector<std::string> names;
        if (DIR* dp = opendir(d.c_str())) {
            while (struct dirent* e = readdir(dp)) {
                std::string n = e->d_name;
                size_t sl = n.rfind('/');                 // SPIFFS can report names with a leading '/'
                if (sl != std::string::npos) n = n.substr(sl + 1);
                if (isLog(n)) names.push_back(n);
            }
            closedir(dp);
        }
        for (auto& n : names) if (remove((d + "/" + n).c_str()) == 0) removed++;
    }
    {
        std::lock_guard<std::mutex> nl(netMtx_);
        sent_.clear();
        netQ_.clear();
        syncFile_ = -1;
        syncPending_ = false;
    }
    recent_.clear();
    worked_.clear();
    logCount_ = archiveCount_ = 0;
    unsent_ = 0;
    archiveBehind_ = false;
    return removed;
}

// Free space on both stores (and a rough count of contacts that still fit where the log is). The
// card's first free-space query can take a moment, so this runs on the network thread.
void App::storRefresh() {
    storMono_ = clk::monoMs();
    if (!fsInfo) return;
    uint64_t t = 0, u = 0;
    if (fsInfo(false, t, u)) { intTotal_ = t; intUsed_ = u; }
    bool sd = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        sd = logOnSd_;
    }
    if (sd && fsInfo(true, t, u)) { sdTotal_ = t; sdUsed_ = u; }
    uint64_t total = sd ? sdTotal_.load() : intTotal_.load(), used = sd ? sdUsed_.load() : intUsed_.load();
    std::lock_guard<std::mutex> lk(mtx_);
    long per = 330;                                     // bytes per contact in one file, from the archive when it has a few
    if (archiveCount_ >= 20) per = std::max(150L, fileSize(archivePath()) / archiveCount_);
    // Each contact goes into two files; SPIFFS also needs some room to work in.
    long long room = (long long)(total > used ? total - used : 0) - (sd ? 1048576LL : (long long)(total / 20));
    long left = total ? (long)std::max(0LL, room / (2 * per)) : -1;
    left_ = left;
    bool low = left >= 0 && left < 200;
    if (low && !lowWarned_)
        status("Log storage almost full: room for about " + std::to_string(left) + " more contacts. Download the logs and clear them" +
               (sd ? std::string(", or use a bigger card.") : std::string(", or put a microSD card in (the log moves onto it at the next start).")));
    lowWarned_ = low;
}

void App::syncSeq() {
    radio.pttMethod = cfg.radioType == 5 && cfg.pttMethod == PTT_CAT ? PTT_VOX : cfg.pttMethod;   // no CAT: VOX unless a line is set
    seq.myCall = cfg.call;
    seq.myGrid = cfg.grid;
    seq.cqTag = cfg.cqTag;
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
void App::note(const std::string& msg) {
    std::lock_guard<std::mutex> lk(mtx_);
    status_ = msg;
    if (logFn) logFn(msg.c_str());
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
    alcHot_ = 0;
    alcCut_ = false;
    alcRd_.clear();
    if (!alcAdjusts()) return;                                     // FT-CW / PSK31: the band's FT8 level, unchanged
}

// Automatic TX audio level. The radio's ALC meter is read about every 0.4 s while transmitting.
// Decisions are made once per transmission from the whole set of readings (its upper quartile),
// not on single readings: one jumpy reading - or the spikes that Morse keying makes on every
// key-down - must not move the level. Only a reading deep in the red twice in a row cuts at once.
// FT-CW and PSK31 have no steady envelope, so they use the FT8 level learned for the band and do
// not change it. If lowering the audio does not lower the ALC (the radio's own settings hold it
// up, e.g. a low RF power setting), RigDeck stops lowering and says so instead of going to zero.
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
    if (alcRd_.size() < 128) alcRd_.push_back(a);
    if (!alcAdjusts()) return;
    alcHot_ = a > 180 ? alcHot_ + 1 : 0;
    if (alcHot_ >= 2 && !alcCut_) {                                // deep in the red: do not wait for the end
        auto& L = alcLevel_[alcBand_];
        if (!L.stuck) {
            float g = txGain_.load();
            txGain_ = std::max(alcFloor(), g * 0.8f);
            alcCut_ = true;
        }
    }
}

bool App::alcAdjusts() const {
    int m = mode_.load();
    return cfg.autoAlc && radio.proto.load() != RP_QMX && (m == DM_FT8 || m == DM_FT4);
}
float App::alcFloor() const { return std::max(2.0f, cfg.txLevel * 0.12f); }

void App::alcEndTx() {
    std::vector<int> r = alcRd_;
    alcRd_.clear();
    int q75 = 0;
    if (!r.empty()) { std::sort(r.begin(), r.end()); q75 = r[(r.size() * 3) / 4]; }
    if (logFn) {
        char b[200];
        snprintf(b, sizeof b, "TX end %s: audio %.1f %% (max %d %%, auto %s), ALC max %d, upper quartile %d /255 over %d readings, Po max %d/255",
                 alcBand_.c_str(), txLevelNow(), cfg.txLevel, cfg.autoAlc ? "on" : "off", alcMax_, q75, alcReads_, poMax_);
        logFn(b);
    }
    if (!alcAdjusts() || alcBand_.empty() || !genStarted_ || r.size() < 5) return;
    auto& L = alcLevel_[alcBand_];
    float mx = (float)cfg.txLevel;
    if (L.ceiling <= 0) L.ceiling = mx;
    float g = txGain_.load(), before = L.level > 0 ? L.level : g;
    std::string note;
    if (q75 > 115 && !L.stuck) {
        // Did the last cut do anything? If the audio went down 10 % or more and the ALC stayed
        // about where it was, the ALC is not following the audio: stop lowering.
        if (L.cutFrom > 0 && g <= L.cutFrom * 0.9f && q75 >= L.cutAlc - 8) {
            L.stuck = true;
            note = "The radio's ALC stays high whatever the TX audio level (check its RF power, compressor and USB MOD level); audio kept at " +
                   std::to_string((int)lroundf(g)) + " %";
        } else {
            float f = q75 > 180 ? 0.8f : q75 > 140 ? 0.88f : 0.94f;
            if (L.cutFrom <= 0) { L.cutFrom = before; L.cutAlc = q75; }   // where this run of cuts started
            if (!alcCut_) g = std::max(alcFloor(), g * f);         // (otherwise a mid-transmission cut already happened)
            L.ceiling = std::min(L.ceiling, before);
            note = "TX audio lowered to " + std::to_string((int)lroundf(g)) + " % to keep the radio's ALC out of the red";
        }
    } else if (q75 > 115) {
        g = before;                                                // stuck: hold the level
    } else {
        if (q75 < 70 && !alcCut_) g = std::min({g * 1.06f, L.ceiling, mx});
        L.cutFrom = 0;
        L.stuck = false;
        L.ceiling = std::min(mx, L.ceiling * 1.02f + 0.1f);       // a ceiling can recover
    }
    txGain_ = g;
    bool changed = fabsf(L.level - g) >= 0.5f || !note.empty();
    L.level = g;
    if (changed) alcSave();                                        // kept across restarts (only when it moved, to spare the flash)
    if (!note.empty()) { status(note); if (logFn) logFn(note.c_str()); }
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
    if (radio.proto == RP_NONE && radio.mode >= 0 && (radio.mode != cfg.dialMode || radio.dataMode != cfg.dialData)) {
        cfg.dialMode = radio.mode;                      // no CAT: the mode set on the radio screen is remembered too
        cfg.dialData = radio.dataMode > 0 ? 1 : 0;
        cfg.save(dir_ + "/settings.txt");
    }
    alcTick(mono);
    bool conn = radio.connected();
    if (conn && !wasConn_ && cfg.keyWpm > 0) radio.setKeySpeed(cfg.keyWpm);
    wasConn_ = conn;
    if (bandReqHz_ && radio.connected()) {
        radio.setFreq((uint64_t)bandReqHz_);
        radio.setUsbData();
        radio.setRfPower(cfg.rfPower);
        if (radio.proto == RP_NONE && cfg.dialHz != bandReqHz_) {   // no CAT: remember the dial for the next start
            cfg.dialHz = bandReqHz_;
            cfg.save(dir_ + "/settings.txt");
        }
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
    // (header first when a file is new, so programs that expect one read it)
    for (const std::string& p : {archivePath(), logPath()})
        if (!fileExists(p)) { if (FILE* f = fopen(p.c_str(), "w")) { fputs(adifHeader().c_str(), f); fclose(f); } }
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
        if (netQ_.size() < 50) netQ_.push_back({pstr(rec), false});
        netCv_.notify_one();
    }
    unsent_++;
}

// A contact's identity for "already sent": call, start and band/mode, so an edited comment or
// report does not make it go again, but a changed call or time does.
std::string App::sentKey(std::string_view rec) {
    return upperTrim(adifField(rec, "CALL")) + "|" + adifField(rec, "QSO_DATE") + "|" + adifField(rec, "TIME_ON").substr(0, 4) + "|" +
           adifField(rec, "BAND") + "|" + adifField(rec, "MODE") + "|" + adifField(rec, "SUBMODE");
}
void App::sentLoad() {
    std::lock_guard<std::mutex> nl(netMtx_);
    sent_.clear();
    FILE* f = fopen(sentPath().c_str(), "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n) sent_.bulkAdd(line);
    }
    fclose(f);
    sent_.finish();
}
int App::countUnsent() {                               // mtx_ held
    std::lock_guard<std::mutex> nl(netMtx_);
    int n = 0;
    adifForEach(logPath(), [&](std::string_view r, int, long) { if (!sent_.contains(sentKey(r))) n++; return true; });
    return n;
}
// "Send unsent": counts what the logging program does not have, then the network thread queues it
// in batches (syncRefill), so a whole archive never sits in memory.
int App::queueUnsent(bool archive) {                   // mtx_ held
    int n = 0;
    {
        std::lock_guard<std::mutex> nl(netMtx_);
        PSet queued;                                   // already on their way: not counted twice
        for (auto& it : netQ_) queued.insert(pstr(sentKey(it.rec)));
        adifForEach(archive ? archivePath() : logPath(), [&](std::string_view r, int, long) {
            std::string k = sentKey(r);
            if (!sent_.contains(k) && !queued.count(k)) n++;
            return true;
        });
        if (!n) return 0;
        syncFile_ = archive ? 1 : 0;
        syncOff_ = 0;
        syncPending_ = true;
    }
    syncRefill();
    return n;
}
void App::syncRefill() {                               // mtx_ held
    std::lock_guard<std::mutex> nl(netMtx_);
    if (syncFile_ < 0) { syncPending_ = false; return; }
    std::string path = syncFile_ ? archivePath() : logPath();
    PSet queued;
    for (auto& it : netQ_) queued.insert(pstr(sentKey(it.rec)));
    int scanned = 0;
    bool more = false;
    long off = syncOff_;
    adifForEach(path, [&](std::string_view r, int, long end) {
        if (++scanned > 2000 || netQ_.size() >= 100) { more = true; return false; }   // the rest next time
        std::string k = sentKey(r);
        if (!sent_.contains(k) && !queued.count(k)) { queued.insert(pstr(k)); netQ_.push_back({pstr(r), false}); }
        off = end;
        return true;
    }, syncOff_);
    syncOff_ = off;
    if (!more) { syncFile_ = -1; syncPending_ = false; }
    netCv_.notify_one();
}

bool App::netOnce() {
    std::string rec, host;
    QsoRecord q;
    int proto, port;
    bool isTest = false;
    bool refill;
    { std::lock_guard<std::mutex> nl(netMtx_); refill = netQ_.empty() && syncPending_; }
    if (refill) { std::lock_guard<std::mutex> lk(mtx_); syncRefill(); }
    {
        std::lock_guard<std::mutex> nl(netMtx_);
        if (netQ_.empty()) return false;
        rec = sstr(netQ_.front().rec);
        isTest = netQ_.front().test;
    }
    q = adifToQso(rec);                              // the record is the one source (also for a resend)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        proto = cfg.logProto; host = cfg.logHost; port = cfg.logPort;
    }
    bool ok = false;
    static std::string primed;                       // a first packet to a host can be lost while its address is looked up
    if (proto != LOG_TCP_ADIF && proto != LOG_NONE && primed != host) {
        static const uint8_t z = 0;
        netSendUdp(host, 9, &z, 1);                  // discard port; only makes the network learn the host
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        primed = host;
    }
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
    static const char* const pn[] = {"off", "TCP ADIF", "UDP ADIF", "UDP WSJT-X"};
    char m[160];
    snprintf(m, sizeof m, "%s %s to %s:%d (%s)", ok ? "sent" : "could not send", q.call.c_str(), host.c_str(), port,
             proto >= 0 && proto <= 3 ? pn[proto] : "?");
    if (logFn) logFn(m);
    std::lock_guard<std::mutex> nl(netMtx_);
    netLast_ = m;
    netLastMs_ = clk::valid() ? clk::utcMs() : 0;
    if (ok) {
        netQ_.pop_front();
        netOk_++;
        if (!isTest && proto != LOG_NONE) {          // remember it, so "Send unsent" skips it
            std::string k = sentKey(rec);
            if (sent_.insert(k)) {
                FILE* f = fopen(sentPath().c_str(), "a");
                if (f) { fputs(k.c_str(), f); fputc('\n', f); fclose(f); }
                if (unsent_ > 0) unsent_--;
            }
        }
    } else netFail_++;
    return ok;
}

void App::netLoop() {
    for (;;) {
        bool work;
        {
            std::unique_lock<std::mutex> nl(netMtx_);
            netCv_.wait_for(nl, std::chrono::seconds(30), [this] { return !netQ_.empty() || syncPending_.load(); });
            work = !netQ_.empty();
        }
        if (clk::monoMs() - storMono_ >= 30000) storRefresh();
        if (!work) {
            if (syncPending_) { std::lock_guard<std::mutex> lk(mtx_); syncRefill(); }
            continue;
        }
        if (netOnce()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));   // a whole log: do not flood the receiver
            bool empty;
            { std::lock_guard<std::mutex> nl(netMtx_); empty = netQ_.empty(); }
            if (empty) {
                std::lock_guard<std::mutex> lk(mtx_);
                if (syncPending_) syncRefill();
                else unsent_ = countUnsent();
            }
        } else {                                     // keep it queued and try again in a while
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
        if (radio.proto == RP_NONE) { cfg.dialHz = hz; cfg.save(dir_ + "/settings.txt"); }
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
        bool dupe = worked_.contains(q.call + "|" + bandName(q.freqHz ? q.freqHz : radio.freq.load()));
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
        if (txState_ == ST_TX) return err("not while transmitting");
        std::string kept;
        if (logOnSd_ && fileExists(logPath())) {            // on the card the cleared log is kept, renamed
            char ts[32] = "nodate";
            if (clk::valid()) { time_t t = (time_t)(clk::utcMs() / 1000); struct tm tm; gmtime_r(&t, &tm); strftime(ts, sizeof ts, "%Y%m%d_%H%M%S", &tm); }
            kept = stationFile((std::string("deleted_") + ts + ".adi").c_str());
            remove(kept.c_str());
            if (rename(logPath().c_str(), kept.c_str()) != 0) return err("Could not rename the log on the card");
        } else remove(logPath().c_str());
        {
            std::lock_guard<std::mutex> nl(netMtx_);
            if (syncFile_ == 0) { syncFile_ = -1; syncPending_ = false; }
        }
        recent_.clear();
        logCount_ = 0;
        unsent_ = 0;
        archiveBehind_ = false;
        status(kept.empty() ? std::string("Current log cleared") : "Current log cleared (kept on the card as " + kept.substr(kept.rfind('/') + 1) + ")");
        return ok();
    }
    if (c == "logtest") {                                            // send a test contact to the logging program only
        if (cfg.logProto == LOG_NONE || cfg.logHost.empty()) return err("Choose where to send the log first (Settings)");
        QsoRecord q;
        q.call = "T1EST";
        q.mode = "SSB"; q.submode = "USB";
        q.rstSent = "59"; q.rstRcvd = "59";
        q.freqHz = radio.freq.load() ? radio.freq.load() : 14250000;
        q.startMs = q.endMs = clk::valid() ? clk::utcMs() : 0;
        q.myCall = cfg.call; q.myGrid = cfg.grid;
        q.comment = "RigDeck test - delete me";
        std::lock_guard<std::mutex> nl(netMtx_);
        netQ_.push_back({pstr(adifRecord(q)), true});
        netCv_.notify_one();
        status("Test contact T1EST queued for " + cfg.logHost + ":" + std::to_string(cfg.logPort));
        return ok();
    }
    if (c == "logsync") {                                            // send every contact the logging program does not have yet
        if (cfg.logProto == LOG_NONE || cfg.logHost.empty()) return err("Choose where to send the log first (Settings)");
        int n = queueUnsent(arg(a, "file") == "all");
        status(n ? "Sending " + std::to_string(n) + " contact(s) to " + cfg.logHost : std::string("Nothing to send: the logging program has every contact"));
        return "{\"ok\":true,\"queued\":" + jnum(n) + "}";
    }
    if (c == "logmarksent") {                                        // the logging program already has these: do not send them
        if (arg(a, "confirm") != "yes") return err("confirm=yes required");
        {
            std::lock_guard<std::mutex> nl(netMtx_);
            FILE* f = fopen(sentPath().c_str(), "a");
            adifForEach(arg(a, "file") == "all" ? archivePath() : logPath(), [&](std::string_view r, int, long) {
                std::string k = sentKey(r);
                if (!sent_.contains(k)) { if (f) { fputs(k.c_str(), f); fputc('\n', f); } sent_.bulkAdd(k); }
                return true;
            });
            if (f) fclose(f);
            sent_.finish();
            syncFile_ = -1;
            syncPending_ = false;
        }
        unsent_ = countUnsent();
        status("Marked as already in the logging program");
        return ok();
    }
    if (c == "logtrash") return trashJson();                          // recently deleted contacts
    if ((c == "logrestore" || c == "logedit" || c == "logdel") && txState_ == ST_TX) return err("not while transmitting");
    if (c == "logrestore") {
        std::string call = arg(a, "call");
        std::string e = trashRestore(atoi(arg(a, "i", "-1").c_str()));
        if (!e.empty()) return err(e);
        status("Contact " + call + " restored");
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
    if (c == "sdformat") {
        if (arg(a, "confirm") != "yes") return err("confirm=yes required");
        if (!sdFormat) return err("This board has no microSD card slot");
        if (txState_ == ST_TX) return err("not while transmitting");
        bool wasSd = logOnSd_;
        if (wasSd) {                                    // keep this station's log in the board's memory meanwhile
            long need = 0;
            for (const char* k : kLogKinds) need += fileSize(stationFile(k));
            long long room = (long long)intTotal_.load() - (long long)intUsed_.load() - (long long)intTotal_.load() / 10;
            if (need > room)
                return err("This station's log (" + std::to_string(need / 1000) + " kB) does not fit in the board's memory while the card is formatted. "
                           "Download the logs, Erase all logs, then format.");
            for (const char* k : kLogKinds) moveInto(stationFile(k), dir_ + "/" + station_ + "_" + k, strcmp(k, "sent.txt") != 0);
        }
        std::string msg;
        int r = sdFormat(msg);
        if (r != 0) {
            if (wasSd) { logOnSd_ = false; logDir_ = dir_; logOpen(); }
            status(msg + (wasSd ? ". The log stays in the board's memory." : ""));
            return err(msg);
        }
        sdInfo = msg;
        storMono_ = -100000;                            // new free-space figures on the next network-thread pass
        if (wasSd) {
            logOpen();                                  // moves this station's log back onto the card
            status("microSD card formatted (" + msg + "); this station's log is back on it");
        } else {
            status("microSD card formatted (" + msg + "); restarting to use it");
            rebootReq = true;
        }
        return "{\"ok\":true,\"info\":" + jstr(msg) + "}";
    }
    if (c == "eraselogs") {                                          // current log and archive, and worked-before with them
        if (arg(a, "confirm") != "yes") return err("confirm=yes required");
        if (txState_ == ST_TX) return err("not while transmitting");
        int n = eraseAllLogs();
        status("All logs erased (" + std::to_string(n) + " files)");
        return "{\"ok\":true,\"files\":" + jnum(n) + "}";
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
        bool newStation = fileCall(n.call) != station_;
        if (newStation && txState_ == ST_TX) return err("Change the callsign when not transmitting");
        cfg = n;
        if (newStation) {                                            // each station callsign has its own log files
            logOpen();
            status("Station " + station_ + ": " + std::to_string(logCount_) + " contacts in the current log, " + std::to_string(archiveCount_) + " in the archive");
        }
        if (alcReset) { alcLevel_.clear(); alcSave(); }
        radio.addr = (uint8_t)cfg.civAddr;
        syncSeq();
        if (!cfg.save(dir_ + "/settings.txt")) return err("Could not save settings");
        if (staChanged && wifiControl) wifiControl(!cfg.staSsid.empty());
        // A logger address on neither of RigDeck's networks is most likely a typo (e.g. 196. for 192.).
        std::string warn;
        if (cfg.logProto != LOG_NONE && !cfg.logHost.empty() && isdigit((unsigned char)cfg.logHost[0])) {
            auto net16 = [](const std::string& ip) { size_t d = ip.find('.'); d = d == std::string::npos ? d : ip.find('.', d + 1); return ip.substr(0, d); };
            std::string h = net16(cfg.logHost);
            bool onSta = !sta_.ip.empty() && h == net16(sta_.ip);
            bool onAp = h == "192.168" && cfg.logHost.compare(0, 10, "192.168.4.") == 0;
            if (!onSta && !onAp)
                warn = "Saved, but the logger address " + cfg.logHost + " is not on RigDeck's networks (home network " +
                       (sta_.ip.empty() ? std::string("not joined") : sta_.ip) + ", hotspot 192.168.4.x). Check for a typo.";
        }
        if (!warn.empty()) status(warn);
        return std::string("{\"ok\":true,\"restart\":") + jbool(wifiChanged) + (warn.empty() ? "" : ",\"warn\":" + jstr(warn)) + "}";
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
    u.staState = sta_.state; u.staIp = sta_.ip; u.staReason = sta_.reason; u.staRssi = sta_.rssi; u.wchipVer = wchipVer_;
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
    u.logN = logCount_; u.logAll = archiveCount_; u.netOk = netOk_; u.netFail = netFail_; u.unsent = unsent_;
    u.status = memFail ? "Not enough RAM for the decoder" : status_;
    u.net = netInfo_; u.call = cfg.call; u.myGrid = cfg.grid;
    u.logSd = logOnSd_; u.sdIn = logOnSd_; u.station = station_; u.sdInfo = sdInfo;
    u.intTotal = intTotal_; u.intUsed = intUsed_; u.sdTotal = sdTotal_; u.sdUsed = sdUsed_; u.left = left_;
    u.bright = cfg.bright; u.contrast = cfg.contrast;
}

bool App::uiWorked(const std::string& call) {
    std::lock_guard<std::mutex> lk(mtx_);
    return worked_.contains(upperTrim(call) + "|" + bandName(radio.freq.load()));
}

void App::uiLogTail(size_t max, std::vector<UiLogEntry>& out) {
    std::lock_guard<std::mutex> lk(mtx_);
    RecVec recs;
    int first = -1;
    adifTail(logPath(), max, recs, first);
    if (first < 0) first = logCount_ - (int)recs.size();
    for (size_t i = recs.size(); i > 0 && out.size() < max; i--) {
        std::string_view r = recs[i - 1];
        UiLogEntry e;
        e.idx = first + (int)i - 1;
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
    o += ",\"log\":{\"n\":" + jnum(logCount_) + ",\"all\":" + jnum(archiveCount_) + std::string(",\"behind\":") + jbool(archiveBehind_) + ",\"ok\":" + jnum(netOk_.load()) + ",\"fail\":" + jnum(netFail_.load()) + ",\"unsent\":" + jnum(unsent_.load()) + ",\"fwd\":" + [this] { std::lock_guard<std::mutex> nl(netMtx_); return jstr(netLast_) + ",\"fwdT\":" + jnum((long long)netLastMs_); }() + ",\"last\":[";
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
         ",\"wchip\":{\"ver\":" + jstr(wchipVer_) + ",\"upd\":" + jstr(wchipUpd_) + "}" +
         ",\"stor\":{\"sd\":" + jbool(logOnSd_) + ",\"station\":" + jstr(station_) + ",\"sdInfo\":" + jstr(sdInfo) +
         ",\"intT\":" + jnum((long long)intTotal_.load()) + ",\"intU\":" + jnum((long long)intUsed_.load()) +
         ",\"sdT\":" + jnum((long long)sdTotal_.load()) + ",\"sdU\":" + jnum((long long)sdUsed_.load()) + ",\"left\":" + jnum(left_.load()) + "}";
    if (withCfg) o += ",\"cfg\":" + cfg.json();
    return o + "}";
}
