#include "ftx.h"
#include <cstring>
#include <cmath>
#include <mutex>
extern "C" {
#include <ft8/encode.h>
#include <ft8/decode.h>
#include <ft8/message.h>
#include <ft8/constants.h>
}

// ---- callsign hash table (resolves <...> hashed callsigns) --------------------------------------
namespace {
const int HT = 512;
struct HEnt { char call[12]; uint32_t hash; };
HEnt g_ht[HT];
int g_htNext = 0;
std::mutex g_htMtx;

void htSave(const char* callsign, uint32_t n22) {
    std::lock_guard<std::mutex> lk(g_htMtx);
    for (int i = 0; i < HT; i++)
        if (g_ht[i].call[0] && g_ht[i].hash == n22 && !strcmp(g_ht[i].call, callsign)) return;
    HEnt& e = g_ht[g_htNext];                   // ring buffer: oldest entry is overwritten
    g_htNext = (g_htNext + 1) % HT;
    strncpy(e.call, callsign, 11);
    e.call[11] = 0;
    e.hash = n22;
}

bool htLookup(ftx_callsign_hash_type_t type, uint32_t hash, char* callsign) {
    int shift = (type == FTX_CALLSIGN_HASH_10_BITS) ? 12 : (type == FTX_CALLSIGN_HASH_12_BITS ? 10 : 0);
    std::lock_guard<std::mutex> lk(g_htMtx);
    for (int i = 0; i < HT; i++)
        if (g_ht[i].call[0] && (g_ht[i].hash >> shift) == hash) { strcpy(callsign, g_ht[i].call); return true; }
    callsign[0] = 0;
    return false;
}
ftx_callsign_hash_interface_t g_hashIf = {htLookup, htSave};
}

bool ftxEncode(const std::string& text, bool ft4, std::vector<uint8_t>& tones) {
    ftx_message_t msg;
    if (ftx_message_encode(&msg, &g_hashIf, text.c_str()) != FTX_MESSAGE_RC_OK) return false;
    tones.assign(ft4 ? FT4_NN : FT8_NN, 0);
    if (ft4) ft4_encode(msg.payload, tones.data());
    else ft8_encode(msg.payload, tones.data());
    return true;
}

FtxMonitor::~FtxMonitor() { if (inited_) monitor_free(&mon_); }

bool FtxMonitor::init(bool ft4) {
    if (inited_) monitor_free(&mon_);
    inited_ = false;
    monitor_config_t cfg;
    cfg.f_min = 200;
    cfg.f_max = 3000;
    cfg.sample_rate = 12000;
    cfg.time_osr = 2;
    cfg.freq_osr = 2;
    cfg.protocol = ft4 ? FTX_PROTOCOL_FT4 : FTX_PROTOCOL_FT8;
    monitor_init(&mon_, &cfg);
    if (!mon_.wf.mag || !mon_.window || !mon_.last_frame || !mon_.fft_work) return false;
    ft4_ = ft4;
    blk_.assign(mon_.block_size, 0);
    fill_ = 0;
    inited_ = true;
    return true;
}

void FtxMonitor::reset() {
    if (!inited_) return;
    monitor_reset(&mon_);
    memset(mon_.last_frame, 0, mon_.nfft * sizeof(float));
    fill_ = 0;
}

void FtxMonitor::feed(const float* s, size_t n) {
    if (!inited_) return;
    for (size_t i = 0; i < n; i++) {
        blk_[fill_++] = s[i];
        if (fill_ == blk_.size()) { monitor_process(&mon_, blk_.data()); fill_ = 0; }
    }
}

int FtxMonitor::decode(std::vector<Decode>& out, int maxCand, int iters) {
    if (!inited_) return 0;
    const ftx_waterfall_t* wf = &mon_.wf;
    if (wf->num_blocks < 20) return 0;
    std::vector<ftx_candidate_t> cands(maxCand);
    int nc = ftx_find_candidates(wf, maxCand, cands.data(), 10);

    // Noise floor: 25th percentile of all waterfall cells. For noise the power in a cell is
    // exponentially distributed, whose 25th percentile sits 5.4 dB below the mean.
    uint32_t hist[256] = {0};
    size_t cells = (size_t)wf->num_blocks * wf->block_stride, total = 0;
    for (size_t i = 0; i < cells; i += 3) { hist[wf->mag[i]]++; total++; }
    size_t acc = 0;
    int p25 = 0;
    for (; p25 < 255; p25++) { acc += hist[p25]; if (acc >= total / 4) break; }
    float noiseDb = p25 * 0.5f - 120.0f + 5.4f;

    std::vector<ftx_message_t> seen;
    int nn = ft4_ ? FT4_NN : FT8_NN;
    for (int i = 0; i < nc; i++) {
        const ftx_candidate_t* c = &cands[i];
        ftx_message_t msg;
        ftx_decode_status_t st;
        if (!ftx_decode_candidate(wf, c, iters, &msg, &st)) continue;
        bool dup = false;
        for (auto& s : seen) if (s.hash == msg.hash && !memcmp(s.payload, msg.payload, sizeof msg.payload)) { dup = true; break; }
        if (dup) continue;
        seen.push_back(msg);
        char text[FTX_MAX_MESSAGE_LENGTH + 4];
        ftx_message_offsets_t off;
        if (ftx_message_decode(&msg, &g_hashIf, text, &off) != FTX_MESSAGE_RC_OK) continue;

        // Signal level: mean dB in the cells the re-encoded tones occupy.
        uint8_t tones[FT4_NN];
        if (ft4_) ft4_encode(msg.payload, tones); else ft8_encode(msg.payload, tones);
        float sum = 0;
        int cnt = 0;
        for (int k = 0; k < nn; k++) {
            int block = c->time_offset + k;
            if (block < 0 || block >= wf->num_blocks) continue;
            int bin = c->freq_offset + tones[k];
            if (bin < 0 || bin >= wf->num_bins) continue;
            size_t idx = ((size_t)(block * wf->time_osr + c->time_sub) * wf->freq_osr + c->freq_sub) * wf->num_bins + bin;
            sum += wf->mag[idx] * 0.5f - 120.0f;
            cnt++;
        }
        // Reports are referenced to 2500 Hz; one cell is 1/symbol_period wide (6.25 Hz FT8, 20.8 Hz FT4).
        float bwDb = 10.0f * log10f(2500.0f * mon_.symbol_period);
        float snr = cnt ? (sum / cnt - noiseDb - bwDb) : -24;
        Decode d;
        d.snr = (int)lroundf(snr < -24 ? -24 : (snr > 30 ? 30 : snr));
        d.freq = (mon_.min_bin + c->freq_offset + (float)c->freq_sub / wf->freq_osr) / mon_.symbol_period;
        d.dt = (c->time_offset + (float)c->time_sub / wf->time_osr) * mon_.symbol_period - 0.5f
               - mon_.symbol_period;      // the 2-symbol analysis window reports one symbol late
        d.text = text;
        out.push_back(d);
    }
    return (int)out.size();
}
