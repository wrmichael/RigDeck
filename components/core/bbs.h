// A small packet BBS (mailbox) on the microSD card: stations connect to its own call-SSID (e.g. AC9HP-1) over
// AX.25 connected mode and list, read, send and kill messages; personal mail and bulletins to ALL. Commands in the
// style of the classic BBSs: L, LM, LB, R n, S call, SB ALL, K n, I, J, H/?, B. Up to 3 users at once.
// Messages are one file each in dir (NNNN.msg) so they survive restarts.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "ax25.h"

namespace bbs {

struct Msg {
    int n = 0;
    std::string from, to, subject, date, body;
    bool read = false;                          // read by the addressee
    bool bulletin() const { return to == "ALL" || to == "QST" || to == "CQ"; }
};

class Bbs {
public:
    std::string call;                           // the BBS's own call-SSID
    std::string sysop;                          // shown in I (info)
    std::string dir;                            // where messages live; empty = BBS off
    std::function<int64_t()> utcMs;             // for message dates
    std::function<std::string()> heard;         // J: stations heard recently, one per line
    size_t maxUsers = 3, maxMsgs = 500, maxBody = 8000;

    void load();                                // reads the messages in dir
    void onFrame(const ax25::Frame& f, int64_t now);
    void tick(int64_t now);
    std::vector<ax25::Frame> takeOut();
    std::vector<std::string> takeLog();         // "W9ABC connected", "W9ABC sent msg 12 to ALL" ...

    // Sysop (web page)
    const std::map<int, Msg>& msgs() const { return msgs_; }
    bool post(const std::string& from, const std::string& to, const std::string& subject, const std::string& body, std::string& err);
    bool kill(int n);
    std::vector<std::string> users() const;     // callsigns connected now

private:
    struct Session {
        ax25::Link link;
        std::string user, in;
        enum { CMD, SUBJ, BODY } st = CMD;
        std::string to, subj, body;
        bool greeted = false;
    };
    std::map<int, Msg> msgs_;
    int next_ = 1;
    std::vector<std::unique_ptr<Session>> ses_;
    std::vector<ax25::Frame> out_;
    std::vector<std::string> log_;

    void greet(Session& s);
    void line(Session& s, const std::string& l);
    void command(Session& s, const std::string& l);
    void prompt(Session& s);
    void say(Session& s, const std::string& t) { s.link.send(t); }
    std::string path(int n) const;
    bool save(const Msg& m);
    std::string now() const;
    int countFor(const std::string& user, bool unreadOnly) const;
};

}  // namespace bbs
