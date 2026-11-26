#include "peers.h"
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace peers {

static std::string clean(const std::string& s, size_t max, bool spaces) {
    std::string o;
    for (char c : s) {
        unsigned char u = (unsigned char)c;
        if (u < 32 || u > 126 || c == '"' || c == '\\' || c == '<' || c == '>' || (!spaces && c == ' ')) continue;
        o += c;
        if (o.size() >= max) break;
    }
    return o;
}

static bool ipOk(const std::string& ip) {
    int a, b, c, d;
    char x;
    return sscanf(ip.c_str(), "%d.%d.%d.%d%c", &a, &b, &c, &d, &x) == 4 && a >= 0 && a < 256 && b >= 0 && b < 256 &&
           c >= 0 && c < 256 && d >= 0 && d < 256;
}

std::string announce(const std::string& ip, const std::string& call, const std::string& ver, const std::string& name) {
    std::string c = clean(call, 12, false), v = clean(ver, 20, false);
    return "RIGDECK1 " + ip + " " + (c.empty() ? "-" : c) + " " + (v.empty() ? "-" : v) + " " + clean(name, 24, true);
}

bool isQuery(const std::string& line) { return line.rfind("RIGDECK1 ?", 0) == 0; }

bool parse(const std::string& line, Peer& p) {
    if (line.rfind("RIGDECK1 ", 0) != 0 || isQuery(line)) return false;
    std::string f[3];
    size_t pos = 9;
    for (int i = 0; i < 3; i++) {                     // ip, call, version; the name is the rest of the line
        size_t sp = line.find(' ', pos);
        if (sp == std::string::npos) { if (i < 2) return false; sp = line.size(); }
        f[i] = line.substr(pos, sp - pos);
        pos = sp + 1;
    }
    if (!ipOk(f[0])) return false;
    p.ip = f[0];
    p.call = f[1] == "-" ? "" : clean(f[1], 12, false);
    p.ver = f[2] == "-" ? "" : clean(f[2], 20, false);
    std::string n = pos < line.size() ? line.substr(pos) : "";
    while (!n.empty() && (n.back() == '\n' || n.back() == '\r' || n.back() == ' ')) n.pop_back();
    p.name = clean(n, 24, true);
    return true;
}

void heard(std::vector<Peer>& list, const Peer& p, int64_t now) {
    for (auto& q : list)
        if (q.ip == p.ip) { q = p; q.seenMono = now; return; }
    if (list.size() >= 32) return;
    Peer n = p;
    n.seenMono = now;
    list.push_back(n);
    std::sort(list.begin(), list.end(), [](const Peer& a, const Peer& b) { return a.ip < b.ip; });
}

void expire(std::vector<Peer>& list, int64_t now, int64_t maxAge) {
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Peer& p) { return now - p.seenMono > maxAge; }), list.end());
}

}  // namespace peers
