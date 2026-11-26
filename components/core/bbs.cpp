#include "bbs.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>

namespace bbs {

static std::string upper(std::string s) { for (char& c : s) c = (char)toupper((unsigned char)c); return s; }
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static std::string base(const std::string& c) { return upper(c.substr(0, c.find('-'))); }
static bool mine(const Msg& m, const std::string& user) { return base(m.to) == base(user) || base(m.from) == base(user); }

std::string Bbs::path(int n) const { char b[16]; snprintf(b, sizeof b, "/%04d.msg", n); return dir + b; }

std::string Bbs::now() const {
    int64_t ms = utcMs ? utcMs() : 0;
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    char b[64];
    snprintf(b, sizeof b, "%04d-%02d-%02d %02d%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    return b;
}

void Bbs::load() {
    msgs_.clear();
    next_ = 1;
    if (dir.empty()) return;
    mkdir(dir.c_str(), 0777);
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        const char* nm = e->d_name;
        size_t len = strlen(nm);
        if (len != 8 || strcmp(nm + 4, ".msg") != 0) continue;
        int n = atoi(nm);
        if (n <= 0) continue;
        FILE* f = fopen((dir + "/" + nm).c_str(), "r");
        if (!f) continue;
        Msg m;
        m.n = n;
        char line[512];
        bool head = true;
        while (fgets(line, sizeof line, f)) {
            std::string l = line;
            if (head) {
                if (l == "\n" || l == "\r\n") { head = false; continue; }
                std::string v = trim(l.substr(l.find(':') == std::string::npos ? 0 : l.find(':') + 1));
                if (l.rfind("From:", 0) == 0) m.from = v;
                else if (l.rfind("To:", 0) == 0) m.to = v;
                else if (l.rfind("Date:", 0) == 0) m.date = v;
                else if (l.rfind("Subject:", 0) == 0) m.subject = v;
                else if (l.rfind("Read:", 0) == 0) m.read = v == "1";
            } else if (m.body.size() < maxBody + 600) m.body += l;
        }
        fclose(f);
        if (!m.from.empty() && !m.to.empty()) {
            msgs_[n] = m;
            next_ = std::max(next_, n + 1);
        }
    }
    closedir(d);
}

bool Bbs::save(const Msg& m) {
    if (dir.empty()) return false;
    std::string p = path(m.n), tmp = p + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return false;
    bool ok = fprintf(f, "From: %s\nTo: %s\nDate: %s\nSubject: %s\nRead: %d\n\n%s", m.from.c_str(), m.to.c_str(), m.date.c_str(),
                      m.subject.c_str(), m.read ? 1 : 0, m.body.c_str()) > 0;
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(tmp.c_str()); return false; }
    remove(p.c_str());
    return rename(tmp.c_str(), p.c_str()) == 0;
}

bool Bbs::post(const std::string& from, const std::string& to, const std::string& subject, const std::string& body, std::string& err) {
    std::string t = upper(trim(to));
    if (dir.empty()) { err = "The BBS is off"; return false; }
    if (t != "ALL" && t != "QST" && t != "CQ" && !ax25::validCall(t)) { err = "To: a callsign or ALL"; return false; }
    if (msgs_.size() >= maxMsgs) {                                     // full: the oldest read message goes
        for (auto it = msgs_.begin(); it != msgs_.end() && msgs_.size() >= maxMsgs;) {
            if (it->second.read || it->second.bulletin()) { remove(path(it->first).c_str()); it = msgs_.erase(it); }
            else ++it;
        }
        if (msgs_.size() >= maxMsgs) { err = "The BBS is full"; return false; }
    }
    Msg m;
    m.n = next_ > 9999 ? 1 : next_;
    while (msgs_.count(m.n)) m.n++;
    m.from = upper(trim(from));
    m.to = t;
    m.subject = trim(subject).substr(0, 60);
    m.date = now();
    m.body = body.substr(0, maxBody);
    if (!m.body.empty() && m.body.back() != '\n') m.body += "\n";
    if (!save(m)) { err = "Could not write to the microSD card"; return false; }
    msgs_[m.n] = m;
    next_ = m.n + 1;
    log_.push_back(m.from + " left message " + std::to_string(m.n) + " for " + m.to + ": " + m.subject);
    return true;
}

bool Bbs::kill(int n) {
    auto it = msgs_.find(n);
    if (it == msgs_.end()) return false;
    remove(path(n).c_str());
    msgs_.erase(it);
    return true;
}

int Bbs::countFor(const std::string& user, bool unreadOnly) const {
    int c = 0;
    for (const auto& kv : msgs_)
        if (!kv.second.bulletin() && base(kv.second.to) == base(user) && (!unreadOnly || !kv.second.read)) c++;
    return c;
}

std::vector<std::string> Bbs::users() const {
    std::vector<std::string> u;
    for (const auto& s : ses_) if (s->link.state() == ax25::Link::CONNECTED) u.push_back(s->user);
    return u;
}

void Bbs::prompt(Session& s) { say(s, s.st == Session::CMD ? call + " BBS >\n" : ""); }

void Bbs::greet(Session& s) {
    s.greeted = true;
    int mineN = countFor(s.user, false), fresh = countFor(s.user, true);
    say(s, "RigDeck BBS " + call + (sysop.empty() ? "" : " (sysop " + sysop + ")") + "\nHello " + s.user + ". " + std::to_string(msgs_.size()) +
               " messages, " + std::to_string(mineN) + " for you" + (fresh ? " (" + std::to_string(fresh) + " new)" : "") + ".\nType ? for help.\n");
    prompt(s);
    log_.push_back(s.user + " connected");
}

static std::string listLine(const Msg& m, const std::string& user) {
    char b[160];
    bool forYou = !m.bulletin() && base(m.to) == base(user);
    snprintf(b, sizeof b, "%-5d %c%c %-9s %-9s %-11.11s %s\n", m.n, m.bulletin() ? 'B' : 'P', forYou && !m.read ? 'N' : ' ',
             m.to.c_str(), m.from.c_str(), m.date.c_str(), m.subject.c_str());
    return b;
}

void Bbs::command(Session& s, const std::string& raw) {
    std::string l = trim(raw), u = upper(l);
    if (u.empty()) { prompt(s); return; }
    std::string cmd = u.substr(0, u.find(' ')), arg = trim(u.size() > cmd.size() ? u.substr(cmd.size()) : "");
    auto list = [&](int which) {                                      // 0 all, 1 mine, 2 bulletins
        std::vector<const Msg*> v;
        for (auto it = msgs_.rbegin(); it != msgs_.rend() && v.size() < 20; ++it) {
            const Msg& m = it->second;
            if (which == 1 && !mine(m, s.user)) continue;
            if (which == 2 && !m.bulletin()) continue;
            if (which == 0 && !m.bulletin() && !mine(m, s.user)) continue;    // others' personal mail stays private
            v.push_back(&m);
        }
        if (v.empty()) { say(s, "No messages.\n"); return; }
        std::string t = "Msg#  St To        From      Date        Subject\n";
        for (const Msg* m : v) t += listLine(*m, s.user);
        say(s, t);
    };
    if (cmd == "?" || cmd == "H" || cmd == "HELP") {
        say(s, "Commands:\n L        list messages (yours and bulletins)\n LM       list your messages\n LB       list bulletins\n"
               " R n      read message n\n RM       read your new messages\n S call   send a message to call (SB ALL: bulletin)\n"
               " K n      kill message n (yours)\n I        about this BBS\n J        stations heard\n B        bye\n");
    } else if (cmd == "L") list(0);
    else if (cmd == "LM" || cmd == "LL") list(1);
    else if (cmd == "LB") list(2);
    else if (cmd == "R" || cmd == "RM") {
        std::vector<int> want;
        if (cmd == "RM") { for (auto& kv : msgs_) if (!kv.second.bulletin() && base(kv.second.to) == base(s.user) && !kv.second.read) want.push_back(kv.first); }
        else want.push_back(atoi(arg.c_str()));
        if (want.empty()) say(s, "No new messages for you.\n");
        for (int n : want) {
            auto it = msgs_.find(n);
            if (it == msgs_.end() || (!it->second.bulletin() && !mine(it->second, s.user))) { say(s, "No message " + std::to_string(n) + ".\n"); continue; }
            Msg& m = it->second;
            say(s, "Msg# " + std::to_string(m.n) + "  From: " + m.from + "  To: " + m.to + "  Date: " + m.date + "\nSubject: " + m.subject + "\n\n" + m.body + "[End of message " + std::to_string(m.n) + "]\n");
            if (!m.bulletin() && base(m.to) == base(s.user) && !m.read) { m.read = true; save(m); }
        }
    } else if (cmd == "S" || cmd == "SP" || cmd == "SB") {
        std::string to = arg.substr(0, arg.find(' '));
        if (cmd == "SB" && to.empty()) to = "ALL";
        if (to.empty()) { say(s, "S call  (or SB ALL)\n"); }
        else if (to != "ALL" && to != "QST" && to != "CQ" && !ax25::validCall(to)) say(s, "Not a callsign: " + to + "\n");
        else { s.to = to; s.subj.clear(); s.body.clear(); s.st = Session::SUBJ; say(s, "Subject:\n"); return; }
    } else if (cmd == "K" || cmd == "KM") {
        int n = atoi(arg.c_str());
        auto it = msgs_.find(n);
        if (it == msgs_.end() || !(mine(it->second, s.user) || base(s.user) == base(sysop))) say(s, "No message " + std::to_string(n) + " of yours.\n");
        else { kill(n); say(s, "Message " + std::to_string(n) + " killed.\n"); log_.push_back(s.user + " killed message " + std::to_string(n)); }
    } else if (cmd == "I") {
        say(s, "RigDeck BBS " + call + ", a mailbox on a RigDeck (github.com/wrmichael/RigDeck)." + (sysop.empty() ? "" : " Sysop " + sysop + ".") + "\n" +
               std::to_string(msgs_.size()) + " messages kept, up to " + std::to_string(maxMsgs) + ".\n");
    } else if (cmd == "J") {
        std::string h = heard ? heard() : "";
        say(s, h.empty() ? "Nobody heard yet.\n" : "Heard:\n" + h);
    } else if (cmd == "B" || cmd == "BYE" || cmd == "Q" || cmd == "QUIT") {
        say(s, "73 de " + call + "\n");
        s.link.disconnect(0);
        return;
    } else say(s, "Unknown command " + cmd + ". Type ? for help.\n");
    prompt(s);
}

void Bbs::line(Session& s, const std::string& l) {
    if (s.st == Session::CMD) { command(s, l); return; }
    if (s.st == Session::SUBJ) {
        s.subj = trim(l);
        s.st = Session::BODY;
        say(s, "Enter the message. End with /EX on a line by itself (/AB to abandon).\n");
        return;
    }
    std::string u = upper(trim(l));
    bool end = u == "/EX" || u == "NNNN" || l.find('\x1a') != std::string::npos;
    if (u == "/AB") { s.st = Session::CMD; say(s, "Message abandoned.\n"); prompt(s); return; }
    if (!end) {
        s.body += l + "\n";
        if (s.body.size() < maxBody) return;
        say(s, "Message too long: saved as it is.\n");
    }
    std::string err;
    if (post(s.user, s.to, s.subj, s.body, err)) say(s, "Message " + std::to_string(next_ - 1) + " saved for " + s.to + ".\n");
    else say(s, "Not saved: " + err + "\n");
    s.st = Session::CMD;
    prompt(s);
}

void Bbs::onFrame(const ax25::Frame& f, int64_t now) {
    if (dir.empty() || upper(f.dst) != upper(call) || f.kind() == ax25::K_UI) return;
    std::string from = upper(f.src);
    for (auto& s : ses_)
        if (s->link.peer() == from && s->link.state() != ax25::Link::DISCONNECTED) { s->link.onFrame(f, now); return; }
    if (f.kind() != ax25::K_SABM && f.kind() != ax25::K_SABME) {      // not connected: the link answers DM
        ax25::Link t;
        t.me = call;
        t.acceptIncoming = false;
        t.onFrame(f, now);
        for (auto& o : t.takeOut()) out_.push_back(o);
        return;
    }
    std::unique_ptr<Session> s(new Session());
    s->link.me = call;
    s->link.acceptIncoming = ses_.size() < maxUsers;
    s->user = from;
    s->link.onFrame(f, now);
    for (auto& o : s->link.takeOut()) out_.push_back(o);
    if (s->link.state() == ax25::Link::CONNECTED) ses_.push_back(std::move(s));
}

void Bbs::tick(int64_t now) {
    for (auto& s : ses_) {
        s->link.tick(now);
        for (auto& e : s->link.takeEvents()) {
            if (e.type == ax25::Link::Event::INFO) {
                if (!s->greeted && s->link.state() == ax25::Link::CONNECTED) greet(*s);
                continue;
            }
            s->in += e.text;
            size_t nl;
            while ((nl = s->in.find('\n')) != std::string::npos) {
                std::string l = s->in.substr(0, nl);
                s->in.erase(0, nl + 1);
                line(*s, l);
            }
            if (s->in.size() > 400) { line(*s, s->in); s->in.clear(); }
        }
        s->link.tick(now);                                            // send what the commands queued
        for (auto& o : s->link.takeOut()) out_.push_back(o);
    }
    for (size_t i = 0; i < ses_.size();) {
        if (ses_[i]->link.state() == ax25::Link::DISCONNECTED) { log_.push_back(ses_[i]->user + " disconnected"); ses_.erase(ses_.begin() + (long)i); }
        else i++;
    }
}

std::vector<ax25::Frame> Bbs::takeOut() { std::vector<ax25::Frame> o; o.swap(out_); return o; }
std::vector<std::string> Bbs::takeLog() { std::vector<std::string> o; o.swap(log_); return o; }

}  // namespace bbs
