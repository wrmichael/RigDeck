// ADIF records, band lookup, and the WSJT-X UDP "Logged ADIF" datagram.
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <utility>
#include <string_view>
#include "psalloc.h"

struct QsoRecord {
    std::string call, grid, mode, submode, rstSent, rstRcvd, myCall, myGrid, comment, name;
    std::string op, mySig, mySigInfo;                      // OPERATOR, MY_SIG, MY_SIG_INFO (written when set)
    uint64_t freqHz = 0;
    int64_t startMs = 0, endMs = 0;
};

const char* bandName(uint64_t hz);                        // "20m", "" if outside the amateur bands
std::string adifRecord(const QsoRecord& q);               // one record ending in <EOR>
std::string adifHeader();
std::string adifField(std::string_view rec, const char* name);   // value of a field in a record ("" if absent)

// ---- log management (edit / delete) ----
using AdifFields = std::vector<std::pair<std::string, std::string>>;   // NAME (upper case), value, in order
AdifFields adifParse(std::string_view rec);              // fields of one record
std::string adifBuild(const AdifFields& f);               // record text ending in <EOR>
bool adifSetField(AdifFields& f, const std::string& name, const std::string& value);   // "" removes; true if changed
bool adifReadRecords(const std::string& path, RecVec& recs);   // in PSRAM   // one record per line, oldest first
bool adifWriteRecords(const std::string& path, const RecVec& recs);   // header + records (via a temp file)
QsoRecord adifToQso(std::string_view rec);              // the fields RigDeck shows (call, band, mode, reports, times)
void adifModeFromText(const std::string& text, std::string& mode, std::string& submode);   // "USB" -> SSB/USB, "FT4" -> MFSK/FT4
std::vector<uint8_t> wsjtxLoggedAdif(const std::string& id, const std::string& record);
std::vector<uint8_t> wsjtxQsoLogged(const std::string& id, const QsoRecord& q);   // type 5, what Log4OM's JT connection reads
std::vector<uint8_t> wsjtxHeartbeat(const std::string& id, const std::string& version);   // type 0
