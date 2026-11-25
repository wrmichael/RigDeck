#include "settings.h"
#include "qso.h"
#include <cstdio>
#include <cstdlib>
#include <cctype>

std::string upperTrim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    std::string r = s.substr(a, b - a);
    for (auto& ch : r) ch = (char)toupper((unsigned char)ch);
    return r;
}

bool validCall(const std::string& c) {
    if (c.size() < 3 || c.size() > 11) return false;
    bool digit = false, alpha = false;
    for (char ch : c) {
        if (isdigit((unsigned char)ch)) digit = true;
        else if (ch >= 'A' && ch <= 'Z') alpha = true;
        else if (ch != '/') return false;
    }
    return digit && alpha;
}

bool validGrid(const std::string& g) {
    if (g.size() != 4 && g.size() != 6) return false;
    if (g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R') return false;
    if (!isdigit((unsigned char)g[2]) || !isdigit((unsigned char)g[3])) return false;
    if (g.size() == 6 && (g[4] < 'A' || g[4] > 'X' || g[5] < 'A' || g[5] > 'X')) return false;
    return true;
}

static bool toInt(const std::string& v, int lo, int hi, int& out) {
    if (v.empty()) return false;
    char* end = nullptr;
    long n = strtol(v.c_str(), &end, 0);
    if (*end != 0 || n < lo || n > hi) return false;
    out = (int)n;
    return true;
}

bool Settings::set(const std::string& k, const std::string& v) {
    if (k == "call") { std::string c = upperTrim(v); if (!c.empty() && !validCall(c)) return false; call = c; return true; }
    if (k == "grid") { std::string g = upperTrim(v); if (!g.empty() && !validGrid(g)) return false; grid = g; return true; }
    if (k == "op") { std::string c = upperTrim(v); if (!c.empty() && !validCall(c)) return false; op = c; return true; }
    if (k == "mySig" || k == "mySigInfo") {
        std::string t = upperTrim(v);
        if (t.size() > (k == "mySig" ? 12u : 40u)) return false;
        for (char ch : t) if (!isalnum((unsigned char)ch) && ch != '-' && ch != ',' && ch != '/' && ch != ' ') return false;
        (k == "mySig" ? mySig : mySigInfo) = t;
        return true;
    }
    if (k == "cqTag") {                                   // what FT8 can carry after CQ: 1-4 letters or 3 digits
        std::string t = upperTrim(v);
        if (t.size() > 3 && t.compare(0, 3, "CQ ") == 0) t = upperTrim(t.substr(3));
        bool letters = !t.empty() && t.size() <= 4 && t.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") == std::string::npos;
        bool digits = t.size() == 3 && t.find_first_not_of("0123456789") == std::string::npos;
        if (!t.empty() && !letters && !digits) return false;
        cqTag = t;
        return true;
    }
    if (k == "rigName") {                               // printable, no quotes or slashes, at most 24 characters
        std::string t = v;
        while (!t.empty() && t.front() == ' ') t.erase(0, 1);
        while (!t.empty() && t.back() == ' ') t.pop_back();
        if (t.size() > 24) return false;
        for (unsigned char ch : t) if (ch < 32 || ch == '"' || ch == '/' || ch == '\\' || ch == '<' || ch == '>') return false;
        rigName = t;
        return true;
    }
    if (k == "apSsid") { if (v.empty() || v.size() > 32) return false; apSsid = v; return true; }
    if (k == "apPass") { if (!v.empty() && (v.size() < 8 || v.size() > 63)) return false; apPass = v; return true; }
    if (k == "staSsid") { if (v.size() > 32) return false; staSsid = v; return true; }
    if (k == "staPass") { if (v.size() > 63) return false; staPass = v; return true; }
    if (k == "logHost") { if (v.size() > 63) return false; logHost = v; return true; }
    if (k == "civAddr") return toInt(v, 1, 0xDF, civAddr);
    if (k == "civBaud") return toInt(v, 4800, 115200, civBaud);
    if (k == "radioType") return toInt(v, 0, 5, radioType);
    if (k == "pttMethod") return toInt(v, 0, 3, pttMethod);
    if (k == "dialMode") return toInt(v, 0, 0x17, dialMode);
    if (k == "dialData") return toInt(v, 0, 1, dialData);
    if (k == "dialHz") { long long x = atoll(v.c_str()); if (x < 100000 || x > 470000000LL) return false; dialHz = x; return true; }
    if (k == "catBaud") return toInt(v, 4800, 115200, catBaud);
    if (k == "keyWpm") { if (v == "0") { keyWpm = 0; return true; } return toInt(v, 6, 48, keyWpm); }
    if (k.size() == 6 && k.compare(0, 5, "macro") == 0 && k[5] >= '1' && k[5] <= '6') {
        if (v.size() > 90 || v.find('|') == std::string::npos) return false;
        for (unsigned char ch : v) if (ch < 0x20 || ch > 0x7E) return false;
        macro[k[5] - '1'] = v;
        return true;
    }
    if (k == "txFreq") return toInt(v, 200, 2900, txFreq);
    if (k == "autoAlc") return toInt(v, 0, 1, autoAlc);
    if (k == "txLevel") return toInt(v, 1, 100, txLevel);
    if (k == "opMode") return toInt(v, 0, 2, opMode);
    if (k == "cqParity") return toInt(v, 0, 1, cqParity);
    if (k == "useRR73") return toInt(v, 0, 1, useRR73);
    if (k == "maxRetries") return toInt(v, 1, 20, maxRetries);
    if (k == "watchdogMin") return toInt(v, 0, 120, watchdogMin);
    if (k == "rfPower") return toInt(v, -1, 100, rfPower);
    if (k == "logProto") return toInt(v, 0, 3, logProto);
    if (k == "logPort") return toInt(v, 1, 65535, logPort);
    if (k == "cwWpm") return toInt(v, 20, 50, cwWpm);
    if (k == "gpsClock") return toInt(v, 0, 1, gpsClock);
    if (k == "gpsBaud") return toInt(v, 4800, 115200, gpsBaud);
    if (k == "gpsDelayMs") return toInt(v, -500, 2000, gpsDelayMs);
    if (k == "gpsModule") return toInt(v, 0, 1, gpsModule);
    if (k == "gpsModBaud") return toInt(v, 4800, 115200, gpsModBaud);
    if (k == "gpsRxPin") return toInt(v, 0, 54, gpsRxPin);
    if (k == "gpsPpsPin") return toInt(v, -1, 54, gpsPpsPin);
    if (k == "bright") return toInt(v, 5, 100, bright);
    if (k == "contrast") return toInt(v, 50, 150, contrast);
    if (k == "consoleLog") return toInt(v, 0, 1, consoleLog);
    if (k == "confirmQso") return toInt(v, 0, 1, confirmQso);
    if (k == "deadman") return toInt(v, 0, 1, deadman);
    if (k == "dnw") {                                   // calls, or prefixes ending in *, at most 100 and 600 characters
        std::string out;
        auto list = parseCallList(v);
        if (list.size() > 100) return false;
        for (const auto& c : list) {
            if (c.size() > 13 || c == "*" || c.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/*") != std::string::npos) return false;
            if (c.find('*') != std::string::npos && c.find('*') != c.size() - 1) return false;
            if (!out.empty()) out += ',';
            out += c;
        }
        if (out.size() > 600) return false;
        dnw = out;
        return true;
    }
    if (k == "prioOn") return toInt(v, 0, 1, prioOn);
    if (k == "prio") { prio = prioText(parsePrio(v)); return true; }
    if (k == "dxKm") return toInt(v, 100, 20000, dxKm);
    if (k == "txLevels") {
        if (v.size() > 400 || v.find_first_of("\r\n=") != std::string::npos) return false;
        txLevels = v;
        return true;
    }
    if (k == "deadmanHost") {
        std::string h = v;
        while (!h.empty() && h.back() == ' ') h.pop_back();
        while (!h.empty() && h.front() == ' ') h.erase(0, 1);
        if (h.empty() || h.size() > 60 || h.find_first_of(" /\\") != std::string::npos) return false;
        deadmanHost = h;
        return true;
    }
    return false;
}

std::string Settings::serialize(bool secrets) const {
    char b[64];
    std::string s;
    auto S = [&](const char* k, const std::string& v) { s += k; s += '='; s += v; s += '\n'; };
    auto I = [&](const char* k, int v) { snprintf(b, sizeof b, "%d", v); S(k, b); };
    S("rigName", rigName); S("call", call); S("grid", grid); S("op", op); S("mySig", mySig); S("mySigInfo", mySigInfo); S("cqTag", cqTag); S("apSsid", apSsid); S("staSsid", staSsid); S("logHost", logHost);
    if (secrets) { S("apPass", apPass); S("staPass", staPass); }
    I("radioType", radioType); I("pttMethod", pttMethod); I("dialMode", dialMode); I("dialData", dialData); s += "dialHz=" + std::to_string(dialHz) + "\n"; I("catBaud", catBaud); I("keyWpm", keyWpm);
    for (int i = 0; i < 6; i++) { snprintf(b, sizeof b, "macro%d", i + 1); S(b, macro[i]); }
    I("civAddr", civAddr); I("civBaud", civBaud); I("txFreq", txFreq); I("autoAlc", autoAlc); I("txLevel", txLevel);
    I("opMode", opMode); I("cqParity", cqParity); I("useRR73", useRR73); I("maxRetries", maxRetries);
    I("watchdogMin", watchdogMin); I("rfPower", rfPower); I("logProto", logProto); I("logPort", logPort);
    I("cwWpm", cwWpm); I("gpsClock", gpsClock); I("gpsBaud", gpsBaud); I("gpsDelayMs", gpsDelayMs);
    I("gpsModule", gpsModule); I("gpsModBaud", gpsModBaud); I("gpsRxPin", gpsRxPin); I("gpsPpsPin", gpsPpsPin);
    I("bright", bright); I("contrast", contrast); I("consoleLog", consoleLog); I("confirmQso", confirmQso); I("deadman", deadman); S("dnw", dnw); I("prioOn", prioOn); S("prio", prio); I("dxKm", dxKm); S("deadmanHost", deadmanHost); S("txLevels", txLevels);
    return s;
}

static std::string jesc(const std::string& in) {
    std::string o;
    for (unsigned char c : in) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

std::string Settings::json() const {
    std::string text = serialize(false), o = "{";
    size_t pos = 0;
    bool first = true;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos), eq = text.find('=', pos);
        if (nl == std::string::npos) break;
        if (eq != std::string::npos && eq < nl) {
            if (!first) o += ',';
            first = false;
            o += '"' + text.substr(pos, eq - pos) + "\":\"" + jesc(text.substr(eq + 1, nl - eq - 1)) + '"';
        }
        pos = nl + 1;
    }
    o += std::string(",\"apPassSet\":") + (apPass.empty() ? "false" : "true");
    o += std::string(",\"staPassSet\":") + (staPass.empty() ? "false" : "true");
    return o + "}";
}

bool Settings::load(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    char line[200];
    while (fgets(line, sizeof line, f)) {
        std::string l(line);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        size_t eq = l.find('=');
        if (eq != std::string::npos) set(l.substr(0, eq), l.substr(eq + 1));
    }
    fclose(f);
    return true;
}

bool Settings::save(const std::string& path) const {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return false;
    std::string s = serialize(true);
    bool ok = fwrite(s.data(), 1, s.size(), f) == s.size();
    ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(tmp.c_str()); return false; }
    remove(path.c_str());                 // SPIFFS rename does not overwrite
    return rename(tmp.c_str(), path.c_str()) == 0;
}

int Settings::levelFor(const std::string& radio) const {
    size_t p = 0;
    while (p < txLevels.size()) {
        size_t e = txLevels.find(';', p);
        if (e == std::string::npos) e = txLevels.size();
        std::string item = txLevels.substr(p, e - p);
        size_t c = item.rfind(':');
        if (c != std::string::npos && item.substr(0, c) == radio) {
            int l = atoi(item.c_str() + c + 1);
            return l >= 1 && l <= 100 ? l : -1;
        }
        p = e + 1;
    }
    return -1;
}

void Settings::rememberLevel(const std::string& radio, int lvl) {
    if (radio.empty() || radio.find_first_of(";:=\r\n") != std::string::npos) return;
    std::string out;
    size_t p = 0;
    while (p < txLevels.size()) {                      // every other radio's entry stays as it was
        size_t e = txLevels.find(';', p);
        if (e == std::string::npos) e = txLevels.size();
        std::string item = txLevels.substr(p, e - p);
        size_t c = item.rfind(':');
        if (c != std::string::npos && item.substr(0, c) != radio && !item.empty()) out += (out.empty() ? "" : ";") + item;
        p = e + 1;
    }
    out += (out.empty() ? "" : ";") + radio + ":" + std::to_string(lvl);
    if (out.size() <= 400) txLevels = out;
}
