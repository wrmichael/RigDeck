// ADIF records, band lookup, and the WSJT-X UDP "Logged ADIF" datagram.
#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct QsoRecord {
    std::string call, grid, mode, submode, rstSent, rstRcvd, myCall, myGrid, comment, name;
    std::string op, mySig, mySigInfo;                      // OPERATOR, MY_SIG, MY_SIG_INFO (written when set)
    uint64_t freqHz = 0;
    int64_t startMs = 0, endMs = 0;
};

const char* bandName(uint64_t hz);                        // "20m", "" if outside the amateur bands
std::string adifRecord(const QsoRecord& q);               // one record ending in <EOR>
std::string adifHeader();
std::string adifField(const std::string& rec, const char* name);   // value of a field in a record ("" if absent)
std::vector<uint8_t> wsjtxLoggedAdif(const std::string& id, const std::string& record);
std::vector<uint8_t> wsjtxQsoLogged(const std::string& id, const QsoRecord& q);   // type 5, what Log4OM's JT connection reads
std::vector<uint8_t> wsjtxHeartbeat(const std::string& id, const std::string& version);   // type 0
