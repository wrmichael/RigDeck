// Packet radio on top of the APRS modem: any AX.25 frame (not only APRS UI frames), the KISS framing that PC
// programs use to talk to a TNC, and a connected-mode AX.25 2.0 link (modulo 8) for a packet terminal.
#pragma once
#include <cstdint>
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace ax25 {

// Frame types by their control field.
enum Kind { K_I, K_RR, K_RNR, K_REJ, K_SABM, K_SABME, K_DISC, K_UA, K_DM, K_FRMR, K_UI, K_XID, K_TEST, K_OTHER };

struct Frame {
    std::string dst, src;
    std::vector<std::string> path;              // "W9ABC-1*": * = this digipeater has repeated it
    bool command = true;                        // C bits: dst set, src clear = command (AX.25 2.0)
    uint8_t ctl = 0x03;
    int pid = 0xF0;                             // -1: none (only I and UI frames carry one)
    std::string info;
    Kind kind() const;
    int ns() const { return (ctl >> 1) & 7; }   // I frames
    int nr() const { return (ctl >> 5) & 7; }   // I and S frames
    bool pf() const { return (ctl & 0x10) != 0; }
};

// Bytes of the frame without FCS (what KISS carries) and with it (what goes on the air).
std::vector<uint8_t> encode(const Frame& f);
std::vector<uint8_t> encodeFcs(const Frame& f);
bool decode(const uint8_t* b, size_t n, Frame& f);        // without FCS
bool decodeFcs(const uint8_t* b, size_t n, Frame& f);     // checks and drops the FCS
// Monitor line like a TNC's: "W9ABC-1>W9XYZ,WIDE1-1* <I C R1 S0 P>: text" (info made printable).
std::string monitor(const Frame& f);
bool validCall(const std::string& c);                     // CALL or CALL-SSID (0..15), letters and digits
std::string reversePath(const std::vector<std::string>& path);   // for answering: "B,A" -> "A,B", no *

// Control field builders.
uint8_t ctlI(int ns, int nr, bool p);
uint8_t ctlS(Kind k, int nr, bool pf);
uint8_t ctlU(Kind k, bool pf);

// ---- KISS (KA9Q): FEND 0xC0, FESC 0xDB, TFEND 0xDC, TFESC 0xDD; command byte: port << 4 | command.
std::vector<uint8_t> kissWrap(const std::vector<uint8_t>& frame, uint8_t cmd = 0);
class KissDecoder {
public:
    // Feeds bytes from the host; complete frames come out with their command byte first.
    void feed(const uint8_t* b, size_t n, std::vector<std::vector<uint8_t>>& out);
private:
    std::vector<uint8_t> cur_;
    bool esc_ = false, in_ = false;
};

// ---- Connected mode: one AX.25 2.0 link (modulo 8, window k, retries N2, timers T1 and T3).
class Link {
public:
    enum State { DISCONNECTED, CONNECTING, CONNECTED, DISCONNECTING };
    struct Event { enum Type { DATA, INFO } type; std::string text; };

    std::string me;                             // our call-SSID
    bool acceptIncoming = true;
    int k = 4, n2 = 10, paclen = 128;
    int t1Ms = 4000, t1PerDigiMs = 2500, t3Ms = 180000;

    // Starts a connection to call via digipeaters ("W9ABC-1" / "WIDE1-1,KB9XYZ-7").
    bool connect(const std::string& call, const std::string& via, int64_t now, std::string& err);
    void disconnect(int64_t now);
    // Text to send; CR ends lines on the air.
    void send(const std::string& text);
    // Every frame heard: the link takes what is addressed to it.
    void onFrame(const Frame& f, int64_t now);
    // Timers: call often.
    void tick(int64_t now);

    State state() const { return st_; }
    const std::string& peer() const { return peer_; }
    std::string via() const;
    size_t waiting() const { return txq_.size() + unacked(); }
    std::vector<Frame> takeOut() { std::vector<Frame> o; o.swap(out_); return o; }
    std::vector<Event> takeEvents() { std::vector<Event> e; e.swap(ev_); return e; }

private:
    State st_ = DISCONNECTED;
    std::string peer_;
    std::vector<std::string> path_;
    int vs_ = 0, vr_ = 0, va_ = 0, top_ = 0;    // send state, receive state, acknowledged, next new N(S)
    int retries_ = 0;
    bool rejSent_ = false, peerBusy_ = false, ackDue_ = false, recovering_ = false;
    int64_t t1_ = 0, t3_ = 0, ackAt_ = 0;
    std::deque<std::string> txq_;               // text waiting for the window
    std::string sent_[8];                       // I frame info by N(S), until acknowledged
    std::vector<Frame> out_;
    std::vector<Event> ev_;

    int unacked() const { return (vs_ - va_ + 8) & 7; }
    // Wait for an answer: the base time, more per digipeater, plus the air time of the frames still waiting for
    // their acknowledgement (at 1200 baud a 4-frame burst of help text takes about 5 s, longer than T1 alone, so
    // the link polled before the other station could even answer).
    int t1() const {
        size_t bytes = 0;
        for (int n = va_; n != top_; n = (n + 1) & 7) bytes += sent_[n].size() + 24;
        return t1Ms + t1PerDigiMs * (int)path_.size() + (int)(bytes * 8 * 1000 / 1200) * (1 + (int)path_.size());
    }
    Frame base(bool command) const;
    void put(Kind k, bool command, bool pf);
    void sendI(int ns, bool p);
    void pump(int64_t now);
    void ackTo(int nr, int64_t now);
    void info(const std::string& s) { ev_.push_back({Event::INFO, s}); }
    void down(const std::string& why);
    void reset();
};

}  // namespace ax25
