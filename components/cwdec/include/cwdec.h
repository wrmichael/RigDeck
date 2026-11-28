// Plain Morse (CW) decoder: a continuous text decoder for ordinary CW, alongside the slotted
// FT-CW modem in cwx.h. It listens on one audio pitch (the radio's CW pitch, or the FT-CW tone
// the operator tapped on the waterfall), tracks it a little either way, follows the sender's
// speed by itself, and prints the copy as it comes in - the way the PSK31 receiver does.
// Input is the same 12 kHz sample stream the other modems get.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

class CwDecRx {
public:
    CwDecRx();
    void setFreq(float f);                       // nominal pitch, Hz; clears the tracking offset
    float freq() const { return f0_ + afc_; }    // pitch actually being listened to, Hz
    void process(const float* s, size_t n, std::string& out);   // appends decoded characters
    int wpm() const;                             // sender's speed as estimated, 0 = not known yet
    float quality() const { return q_; }         // 0..1; low on noise, ->1 on a clean tone
    bool keyDown() const { return key_; }        // the tone is keyed right now (for a lamp)
private:
    void tick(float env, std::string& out);      // one envelope sample (4 ms)
    void commitElem();                           // finish the element being received
    void emitChar(std::string& out);
    float f0_ = 700, afc_ = 0, ph_ = 0;
    float i1_ = 0, q1_ = 0, i2_ = 0, q2_ = 0, i3_ = 0, q3_ = 0;   // quadrature low-pass, three poles each
    float pi_ = 0, pq_ = 0;                      // previous tick's I/Q, for the frequency tracker
    int dec_ = 0;                                // samples since the last envelope tick
    float peak_ = 0, noise_ = 0, q_ = 0;
    int ticks_ = 0;                              // envelope ticks since start (warm-up)
    bool key_ = false;
    int runTicks_ = 0;                           // length of the current key-down / key-up run
    float curElemMs_ = 0;                        // element in progress (bridges dropouts under 16 ms)
    bool elemPending_ = false;                   // an element has ended but may still be bridged
    bool charOut_ = false, spaceOut_ = false;    // this gap has already produced its character / space
    std::vector<int> elemMs_;                    // key-down durations of the character being received
    std::vector<float> elemPk_;                  // peak envelope of each of those elements
    float curElemPk_ = 0;                        // peak envelope of the element in progress
    std::vector<int> gapMs_;                     // gaps between those elements (one dot each)
    float dotMs_ = 60;                           // estimated dot length, ms (20 wpm to start)
    int elemHist_[16] = {};                      // recent element lengths, for speed rescue
    int elemHistN_ = 0, elemHistI_ = 0;
    int lastKeyTick_ = -100000;                  // last tick the tone was keyed (squelch memory)
    bool seenTone_ = false;                      // a solid tone has been copied (squelch memory)
    bool strong_ = false;                        // squelch open this tick
    float lastElemPk_ = 0;                       // peak envelope of the last committed element
    int keyRun_ = 0;                             // consecutive keyed ticks
};
