// RigDecks find each other on the home network: each one broadcasts a short UDP line every 30 s (port 47731)
// and keeps the ones it hears, for the all-stations page (/multi, /api/peers).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace peers {

constexpr uint16_t kPort = 47731;

struct Peer {
    std::string ip, name, call, ver;
    int64_t seenMono = 0;
};

// "RIGDECK1 <ip> <call> <ver> <name>" (name last: it may hold spaces); "RIGDECK1 ?" asks everyone to announce.
std::string announce(const std::string& ip, const std::string& call, const std::string& ver, const std::string& name);
bool isQuery(const std::string& line);
bool parse(const std::string& line, Peer& p);

// The list of stations heard: add/refresh one, drop the ones not heard for maxAgeMs, at most 32.
void heard(std::vector<Peer>& list, const Peer& p, int64_t nowMono);
void expire(std::vector<Peer>& list, int64_t nowMono, int64_t maxAgeMs);

}  // namespace peers
