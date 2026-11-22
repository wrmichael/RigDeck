// ADIF records, band lookup, and the WSJT-X UDP "Logged ADIF" datagram.
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <utility>
#include <string_view>
#include <functional>
#include <cstdio>
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

// Streaming access for logs of any size (one record per line, as RigDeck writes them). Only the
// record being looked at is in memory.
// fn(rec, index, endOffset) for each record in file order; return false to stop. start = a byte
// offset from an earlier endOffset (index then counts from there). false = no such file.
using AdifRecFn = std::function<bool(std::string_view rec, int idx, long endOff)>;
// File I/O through a small buffer on the stack (internal RAM). On the ESP32-P4 the SD card
// controller moves data by DMA straight to / from the caller's buffer; buffers in PSRAM went wrong
// there (reads came back empty until a restart), so file data never goes to or from PSRAM directly.
size_t fileRead(FILE* f, char* dst, size_t n);
bool fileWrite(FILE* f, const char* src, size_t n);
bool adifForEach(const std::string& path, const AdifRecFn& fn, long start = 0);
// Rewrites a file through a temp file: fn(rec, index, out) returns true to keep the record, or false
// to write out in its place (empty = drop it; several lines are fine). tail (record lines) goes at
// the end. A missing file is treated as empty. Returns how many records the new file holds, -1 = error.
using AdifEditFn = std::function<bool(std::string_view rec, int idx, std::string& out)>;
int adifRewrite(const std::string& path, const AdifEditFn& fn, const std::string& tail = "");
// The last max records (oldest first), reading only the end of the file. firstIdx = index of
// out[0] when the whole file was read, else -1 (count from the end then).
bool adifTail(const std::string& path, size_t max, RecVec& out, int& firstIdx);
// "YYYYMMDDHHMMSS" start time of a record, for ordering ("" if it has no date)
std::string adifStartKey(std::string_view rec);
QsoRecord adifToQso(std::string_view rec);              // the fields RigDeck shows (call, band, mode, reports, times)
void adifModeFromText(const std::string& text, std::string& mode, std::string& submode);   // "USB" -> SSB/USB, "FT4" -> MFSK/FT4
std::vector<uint8_t> wsjtxLoggedAdif(const std::string& id, const std::string& record);
std::vector<uint8_t> wsjtxQsoLogged(const std::string& id, const QsoRecord& q);   // type 5, what Log4OM's JT connection reads
std::vector<uint8_t> wsjtxHeartbeat(const std::string& id, const std::string& version);   // type 0
