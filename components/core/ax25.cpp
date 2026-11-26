#include "ax25.h"
#include "aprs.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace ax25 {

// ---------------------------------------------------------------- frames
Kind Frame::kind() const {
    if (!(ctl & 1)) return K_I;
    if ((ctl & 3) == 1) {
        switch (ctl & 0x0F) {
        case 0x01: return K_RR;
        case 0x05: return K_RNR;
        case 0x09: return K_REJ;
        default: return K_OTHER;
        }
    }
    switch (ctl & ~0x10) {
    case 0x2F: return K_SABM;
    case 0x6F: return K_SABME;
    case 0x43: return K_DISC;
    case 0x63: return K_UA;
    case 0x0F: return K_DM;
    case 0x87: return K_FRMR;
    case 0x03: return K_UI;
    case 0xAF: return K_XID;
    case 0xE3: return K_TEST;
    default: return K_OTHER;
    }
}

uint8_t ctlI(int ns, int nr, bool p) { return (uint8_t)(((nr & 7) << 5) | (p ? 0x10 : 0) | ((ns & 7) << 1)); }
uint8_t ctlS(Kind k, int nr, bool pf) {
    uint8_t b = k == K_RNR ? 0x05 : k == K_REJ ? 0x09 : 0x01;
    return (uint8_t)(((nr & 7) << 5) | (pf ? 0x10 : 0) | b);
}
uint8_t ctlU(Kind k, bool pf) {
    uint8_t b = k == K_SABM ? 0x2F : k == K_SABME ? 0x6F : k == K_DISC ? 0x43 : k == K_UA ? 0x63 : k == K_DM ? 0x0F
              : k == K_FRMR ? 0x87 : k == K_XID ? 0xAF : k == K_TEST ? 0xE3 : 0x03;
    return (uint8_t)(b | (pf ? 0x10 : 0));
}

bool validCall(const std::string& c) { return aprs::validCallSsid(c); }

static void putAddr(std::vector<uint8_t>& o, std::string a, bool last, bool bit7) {
    bool star = !a.empty() && a.back() == '*';
    if (star) a.pop_back();
    size_t dash = a.find('-');
    std::string base = a.substr(0, dash);
    int ssid = dash == std::string::npos ? 0 : atoi(a.c_str() + dash + 1);
    for (size_t i = 0; i < 6; i++) o.push_back((uint8_t)((i < base.size() ? toupper((unsigned char)base[i]) : ' ') << 1));
    o.push_back((uint8_t)(0x60 | ((ssid & 15) << 1) | (last ? 1 : 0) | ((bit7 || star) ? 0x80 : 0)));
}

std::vector<uint8_t> encode(const Frame& f) {
    std::vector<uint8_t> o;
    putAddr(o, f.dst, false, f.command);
    putAddr(o, f.src, f.path.empty(), !f.command);
    for (size_t i = 0; i < f.path.size(); i++) putAddr(o, f.path[i], i + 1 == f.path.size(), false);
    o.push_back(f.ctl);
    Kind k = f.kind();
    if ((k == K_I || k == K_UI) && f.pid >= 0) o.push_back((uint8_t)f.pid);
    o.insert(o.end(), f.info.begin(), f.info.end());
    return o;
}

std::vector<uint8_t> encodeFcs(const Frame& f) {
    std::vector<uint8_t> o = encode(f);
    uint16_t c = aprs::fcs(o.data(), o.size());
    o.push_back((uint8_t)(c & 0xFF));
    o.push_back((uint8_t)(c >> 8));
    return o;
}

static std::string getAddr(const uint8_t* p, bool& bit7) {
    std::string s;
    for (int i = 0; i < 6; i++) {
        char ch = (char)(p[i] >> 1);
        if (ch != ' ') s += ch;
    }
    int ssid = (p[6] >> 1) & 15;
    if (ssid) s += "-" + std::to_string(ssid);
    bit7 = (p[6] & 0x80) != 0;
    return s;
}

bool decode(const uint8_t* b, size_t n, Frame& f) {
    f = Frame();
    size_t p = 0;
    int na = 0;
    bool cDst = false, cSrc = false;
    while (true) {
        if (p + 7 > n) return false;
        bool h;
        std::string a = getAddr(b + p, h);
        if (a.empty()) return false;
        for (char ch : a) if (!isalnum((unsigned char)ch) && ch != '-') return false;
        if (na == 0) { f.dst = a; cDst = h; }
        else if (na == 1) { f.src = a; cSrc = h; }
        else f.path.push_back(a + (h ? "*" : ""));
        bool last = b[p + 6] & 1;
        p += 7;
        na++;
        if (last) break;
        if (na >= 10) return false;
    }
    if (na < 2 || p >= n) return false;
    f.command = cDst || !cSrc;                                        // 2.0: 1/0 command, 0/1 response; 1.x: same bits
    f.ctl = b[p++];
    Kind k = f.kind();
    f.pid = -1;
    if (k == K_I || k == K_UI) {
        if (p >= n) return false;
        f.pid = b[p++];
    }
    f.info.assign((const char*)b + p, n - p);
    return true;
}

bool decodeFcs(const uint8_t* b, size_t n, Frame& f) {
    if (n < 17) return false;
    if (aprs::fcs(b, n - 2) != (uint16_t)(b[n - 2] | (b[n - 1] << 8))) return false;
    return decode(b, n - 2, f);
}

std::string monitor(const Frame& f) {
    std::string s = f.src + ">" + f.dst;
    for (const auto& d : f.path) s += "," + d;
    char t[48];
    Kind k = f.kind();
    const char* cr = f.command ? "C" : "R";
    switch (k) {
    case K_I: snprintf(t, sizeof t, "I %s S%d R%d%s", cr, f.ns(), f.nr(), f.pf() ? " P" : ""); break;
    case K_RR: case K_RNR: case K_REJ:
        snprintf(t, sizeof t, "%s %s R%d%s", k == K_RR ? "RR" : k == K_RNR ? "RNR" : "REJ", cr, f.nr(), f.pf() ? (f.command ? " P" : " F") : ""); break;
    default: {
        static const char* n[] = {"I", "RR", "RNR", "REJ", "SABM", "SABME", "DISC", "UA", "DM", "FRMR", "UI", "XID", "TEST", "?"};
        snprintf(t, sizeof t, "%s %s%s", n[k], cr, f.pf() ? (f.command ? " P" : " F") : "");
    }
    }
    s += std::string(" <") + t;
    if (f.pid >= 0 && f.pid != 0xF0) { char p[16]; snprintf(p, sizeof p, " pid %02X", f.pid & 0xFF); s += p; }
    s += ">";
    if (!f.info.empty()) {
        s += ": ";
        for (char c : f.info) {
            if (s.size() > 300) { s += "…"; break; }
            unsigned char u = (unsigned char)c;
            s += c == '\r' || c == '\n' ? ' ' : (u < 32 || u == 127 ? '.' : c);
        }
    }
    return s;
}

std::string reversePath(const std::vector<std::string>& path) {
    std::string s;
    for (size_t i = path.size(); i-- > 0;) {
        std::string a = path[i];
        if (!a.empty() && a.back() == '*') a.pop_back();
        s += (s.empty() ? "" : ",") + a;
    }
    return s;
}

// ---------------------------------------------------------------- KISS
std::vector<uint8_t> kissWrap(const std::vector<uint8_t>& frame, uint8_t cmd) {
    std::vector<uint8_t> o;
    o.reserve(frame.size() + 8);
    o.push_back(0xC0);
    o.push_back(cmd);
    for (uint8_t b : frame) {
        if (b == 0xC0) { o.push_back(0xDB); o.push_back(0xDC); }
        else if (b == 0xDB) { o.push_back(0xDB); o.push_back(0xDD); }
        else o.push_back(b);
    }
    o.push_back(0xC0);
    return o;
}

void KissDecoder::feed(const uint8_t* b, size_t n, std::vector<std::vector<uint8_t>>& out) {
    for (size_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c == 0xC0) {
            if (in_ && !cur_.empty()) out.push_back(cur_);
            cur_.clear();
            in_ = true;
            esc_ = false;
            continue;
        }
        if (!in_) continue;
        if (esc_) { cur_.push_back(c == 0xDC ? 0xC0 : c == 0xDD ? 0xDB : c); esc_ = false; }
        else if (c == 0xDB) esc_ = true;
        else cur_.push_back(c);
        if (cur_.size() > 1200) { cur_.clear(); in_ = false; }      // runaway: wait for the next FEND
    }
}

// ---------------------------------------------------------------- connected mode
static std::string upper(std::string s) { for (char& c : s) c = (char)toupper((unsigned char)c); return s; }

std::string Link::via() const {
    std::string s;
    for (const auto& p : path_) s += (s.empty() ? "" : ",") + p;
    return s;
}

void Link::reset() {
    vs_ = vr_ = va_ = 0;
    top_ = 0;
    retries_ = 0;
    rejSent_ = peerBusy_ = ackDue_ = recovering_ = false;
    t1_ = t3_ = ackAt_ = 0;
}

Frame Link::base(bool command) const {
    Frame f;
    f.dst = peer_;
    f.src = me;
    f.path = path_;
    f.command = command;
    f.pid = -1;
    return f;
}

void Link::put(Kind k, bool command, bool pf) {
    Frame f = base(command);
    f.ctl = (k == K_RR || k == K_RNR || k == K_REJ) ? ctlS(k, vr_, pf) : ctlU(k, pf);
    if (k == K_RR || k == K_RNR || k == K_REJ) ackDue_ = false;
    out_.push_back(f);
}

void Link::sendI(int ns, bool p) {
    Frame f = base(true);
    f.ctl = ctlI(ns, vr_, p);
    f.pid = 0xF0;
    f.info = sent_[ns & 7];
    out_.push_back(f);
    ackDue_ = false;                                                  // N(R) rides along
}

bool Link::connect(const std::string& call, const std::string& via, int64_t now, std::string& err) {
    std::string c = upper(call);
    if (!validCall(me)) { err = "Set a valid callsign first"; return false; }
    if (!validCall(c)) { err = "Not a callsign: " + call; return false; }
    if (st_ != DISCONNECTED) { err = "Already linked to " + peer_ + ": disconnect first"; return false; }
    std::vector<std::string> p;
    std::string cur;
    std::string v = upper(via);
    for (size_t i = 0; i <= v.size(); i++) {
        char ch = i < v.size() ? v[i] : ',';
        if (ch == ',' || ch == ' ') { if (!cur.empty()) p.push_back(cur); cur.clear(); }
        else cur += ch;
    }
    if (p.size() > 7) { err = "At most 7 digipeaters"; return false; }
    for (const auto& d : p) if (!validCall(d)) { err = "Not a digipeater call: " + d; return false; }
    reset();
    txq_.clear();
    peer_ = c;
    path_ = p;
    st_ = CONNECTING;
    put(K_SABM, true, true);
    t1_ = now + t1();
    info("Connecting to " + peer_ + (path_.empty() ? "" : " via " + this->via()) + " …");
    return true;
}

void Link::disconnect(int64_t now) {
    if (st_ == DISCONNECTED) return;
    if (st_ == DISCONNECTING) { down("Disconnected"); return; }
    st_ = DISCONNECTING;
    retries_ = 0;
    put(K_DISC, true, true);
    t1_ = now + t1();
    info("Disconnecting …");
}

void Link::send(const std::string& text) {
    std::string t;
    for (char c : text) t += c == '\n' ? '\r' : c;
    for (size_t i = 0; i < t.size(); i += (size_t)paclen) txq_.push_back(t.substr(i, (size_t)paclen));
}

void Link::down(const std::string& why) {
    st_ = DISCONNECTED;
    txq_.clear();
    t1_ = t3_ = 0;
    ackDue_ = false;
    info(why);
}

void Link::ackTo(int nr, int64_t now) {
    int outstanding = (top_ - va_ + 8) & 7, n = (nr - va_ + 8) & 7;
    if (n > outstanding) return;                                      // not a frame we sent: ignore
    va_ = nr & 7;
    if (((vs_ - va_ + 8) & 7) > outstanding) vs_ = va_;
    if (va_ == top_) {
        t1_ = 0;
        if (!recovering_) retries_ = 0;
        t3_ = now + t3Ms;
    } else if (n > 0) t1_ = now + t1();
}

void Link::pump(int64_t now) {
    if (st_ != CONNECTED || recovering_ || peerBusy_) return;
    while (vs_ != top_) { sendI(vs_, false); vs_ = (vs_ + 1) & 7; }          // go-back-N after a REJ
    while (((top_ - va_ + 8) & 7) < k && !txq_.empty()) {
        sent_[top_] = txq_.front();
        txq_.pop_front();
        sendI(top_, false);
        top_ = (top_ + 1) & 7;
        vs_ = top_;
    }
    if (va_ != top_ && !t1_) t1_ = now + t1();
}

void Link::onFrame(const Frame& f, int64_t now) {
    if (upper(f.dst) != upper(me) || f.kind() == K_UI) return;
    if (!f.path.empty() && f.path.back().back() != '*') return;      // still on its way through the digipeaters
    Kind k = f.kind();
    std::string from = upper(f.src);
    if (st_ == DISCONNECTED) {
        if (k == K_SABM && acceptIncoming) {
            reset();
            txq_.clear();
            peer_ = from;
            path_.clear();
            std::string r = reversePath(f.path), cur;
            for (size_t i = 0; i <= r.size(); i++) { if (i == r.size() || r[i] == ',') { if (!cur.empty()) path_.push_back(cur); cur.clear(); } else cur += r[i]; }
            st_ = CONNECTED;
            put(K_UA, false, f.pf());
            t3_ = now + t3Ms;
            info("*** Connected to " + peer_ + " (they called you)");
        } else if (f.command && (k == K_SABM || k == K_SABME || k == K_DISC || f.pf())) {
            std::string keep = peer_;
            std::vector<std::string> kp = path_;
            peer_ = from;
            std::string r = reversePath(f.path), cur;
            path_.clear();
            for (size_t i = 0; i <= r.size(); i++) { if (i == r.size() || r[i] == ',') { if (!cur.empty()) path_.push_back(cur); cur.clear(); } else cur += r[i]; }
            put(K_DM, false, f.pf());
            peer_ = keep;
            path_ = kp;
        }
        return;
    }
    if (from != peer_) {                                              // someone else while we are busy
        if (k == K_SABM || k == K_SABME) {
            Frame dm;
            dm.dst = from; dm.src = me; dm.command = false; dm.ctl = ctlU(K_DM, f.pf()); dm.pid = -1;
            std::string r = reversePath(f.path), cur;
            for (size_t i = 0; i <= r.size(); i++) { if (i == r.size() || r[i] == ',') { if (!cur.empty()) dm.path.push_back(cur); cur.clear(); } else cur += r[i]; }
            out_.push_back(dm);
        }
        return;
    }
    if (st_ == CONNECTING) {
        if (k == K_UA) { reset(); st_ = CONNECTED; t3_ = now + t3Ms; info("*** Connected to " + peer_); }
        else if (k == K_DM) down("*** " + peer_ + " is busy or does not take connections");
        else if (k == K_SABM) { reset(); st_ = CONNECTED; put(K_UA, false, f.pf()); t3_ = now + t3Ms; info("*** Connected to " + peer_); }
        return;
    }
    if (st_ == DISCONNECTING) {
        if (k == K_UA || k == K_DM) down("*** Disconnected from " + peer_);
        else if (k == K_DISC) { put(K_UA, false, f.pf()); down("*** Disconnected from " + peer_); }
        return;
    }
    // connected
    switch (k) {
    case K_SABM: case K_SABME:
        reset();
        txq_.clear();
        put(K_UA, false, f.pf());
        t3_ = now + t3Ms;
        info("*** Link reset by " + peer_);
        return;
    case K_DISC:
        put(K_UA, false, f.pf());
        down("*** " + peer_ + " disconnected");
        return;
    case K_DM:
        down("*** " + peer_ + " ended the link");
        return;
    case K_FRMR:
        down("*** " + peer_ + " rejected a frame (FRMR): link ended");
        return;
    case K_I: {
        if (f.ns() == vr_) {
            vr_ = (vr_ + 1) & 7;
            rejSent_ = false;
            std::string t;
            for (char c : f.info) t += c == '\r' ? '\n' : c;
            if (!t.empty()) ev_.push_back({Event::DATA, t});
            if (f.pf()) put(K_RR, false, true);
            else { ackDue_ = true; ackAt_ = now + 400; }
        } else if (!rejSent_) {
            rejSent_ = true;
            put(K_REJ, false, f.pf());
        } else if (f.pf()) put(K_RR, false, true);
        ackTo(f.nr(), now);
        break;
    }
    case K_RR: case K_RNR: case K_REJ:
        peerBusy_ = k == K_RNR;
        ackTo(f.nr(), now);
        if (k == K_REJ) vs_ = va_;
        if (f.command && f.pf()) put(K_RR, false, true);                 // they ask where we are
        if (!f.command && f.pf() && recovering_) { recovering_ = false; retries_ = 0; vs_ = va_; if (va_ == top_) t1_ = 0; }
        break;
    default:
        break;
    }
    pump(now);
}

void Link::tick(int64_t now) {
    if (st_ == DISCONNECTED) return;
    if (ackDue_ && now >= ackAt_ && st_ == CONNECTED) {
        pump(now);                                                    // an I frame carries the ack if one is waiting
        if (ackDue_) put(K_RR, false, false);
    }
    if (t1_ && now >= t1_) {
        if (++retries_ > n2) {
            if (st_ == CONNECTING) down("*** No answer from " + peer_);
            else if (st_ == DISCONNECTING) down("*** Disconnected (no answer)");
            else { put(K_DM, false, false); down("*** Link to " + peer_ + " failed: no answer after " + std::to_string(n2) + " tries"); }
            return;
        }
        if (st_ == CONNECTING) put(K_SABM, true, true);
        else if (st_ == DISCONNECTING) put(K_DISC, true, true);
        else { recovering_ = true; put(K_RR, true, true); }               // "where are you?"
        t1_ = now + t1();
    }
    if (st_ == CONNECTED && t3_ && now >= t3_ && va_ == top_ && !t1_) {   // idle a long time: check the link
        recovering_ = true;
        put(K_RR, true, true);
        t1_ = now + t1();
        t3_ = 0;
    }
    pump(now);
}

}  // namespace ax25
