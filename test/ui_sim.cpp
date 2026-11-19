// Desktop preview of the on-device touch interface: runs the real application core with a
// simulated radio and band, renders the LVGL screen into memory and writes screenshots (PPM).
// It also taps the screen through a scripted pointer to check that the controls reach the app.
//   sh test/ui_sim.sh   (needs an LVGL 9.5 checkout; see the script)
#include "app.h"
#include "clock.h"
#include "../main/ui.h"
#include "lvgl.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <sys/stat.h>

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int64_t simMs = 0;
static int64_t mono() { return simMs; }
static const int64_t T0 = 1794328740000LL;

// ---- simulated IC-7100 on the CI-V port ----------------------------------------------------------
struct FakeRadio {
    App* app;
    CivParser parser;
    bool ptt = false;
    std::string cw;                                   // text sent with command 17 (keyer)
    uint64_t freq = 14074000;
    int dataMode = 1, mode = 1, filter = 1, att = 0;
    int level[16] = {0, 90, 255, 0, 0, 0, 40, 0, 0, 0, 255, 128, 0, 0, 0, 0};
    int func[0x50] = {0};
    void reply(uint8_t cmd, std::vector<uint8_t> d) { auto f = civBuild(Radio::CTRL, 0x88, cmd, d); app->radio.onRx(f.data(), f.size()); }
    void rx(const uint8_t* d, size_t n) {
        app->radio.onRx(d, n);
        for (size_t i = 0; i < n; i++) {
            CivFrame f;
            if (!parser.feed(d[i], f) || f.to != 0x88) continue;
            if (f.cmd == 0x03) { uint8_t b[5]; civFreqToBcd(freq, b); reply(0x03, std::vector<uint8_t>(b, b + 5)); }
            else if (f.cmd == 0x05 && f.data.size() == 5) { freq = civBcdToFreq(f.data.data(), 5); reply(0xFB, {}); }
            else if (f.cmd == 0x04) reply(0x04, {(uint8_t)mode, (uint8_t)filter});
            else if (f.cmd == 0x06 && f.data.size() >= 1) { mode = f.data[0]; if (f.data.size() >= 2) filter = f.data[1]; reply(0xFB, {}); }
            else if (f.cmd == 0x14 && f.data.size() == 1 && f.data[0] < 16) { int v = level[f.data[0]]; reply(0x14, {f.data[0], (uint8_t)(((v / 1000) << 4) | ((v / 100) % 10)), (uint8_t)((((v / 10) % 10) << 4) | (v % 10))}); }
            else if (f.cmd == 0x14 && f.data.size() == 3 && f.data[0] < 16) { level[f.data[0]] = civBcdToInt(f.data.data() + 1, 2); reply(0xFB, {}); }
            else if (f.cmd == 0x16 && f.data.size() == 1 && f.data[0] < 0x50) reply(0x16, {f.data[0], (uint8_t)func[f.data[0]]});
            else if (f.cmd == 0x16 && f.data.size() == 2 && f.data[0] < 0x50) { func[f.data[0]] = f.data[1]; reply(0xFB, {}); }
            else if (f.cmd == 0x11 && f.data.empty()) reply(0x11, {(uint8_t)att});
            else if (f.cmd == 0x11) { att = f.data[0]; reply(0xFB, {}); }
            else if (f.cmd == 0x1A && f.data.size() == 1) reply(0x1A, {0x06, (uint8_t)dataMode, 0x01});
            else if (f.cmd == 0x1A && f.data.size() == 3) { dataMode = f.data[1]; reply(0xFB, {}); }
            else if (f.cmd == 0x15 && f.data.size() == 1) reply(0x15, {f.data[0], (uint8_t)(f.data[0] == 0x02 ? 0x00 : 0x01), (uint8_t)(f.data[0] == 0x02 ? 0x95 : 0x20)});
            else if (f.cmd == 0x17) { cw += std::string(f.data.begin(), f.data.end()); reply(0xFB, {}); }
            else if (f.cmd == 0x1C && f.data.size() == 1) reply(0x1C, {0x00, (uint8_t)ptt});
            else if (f.cmd == 0x1C && f.data.size() == 2) { ptt = f.data[1] != 0; reply(0xFB, {}); }
            else reply(0xFB, {});
        }
    }
};

// ---- simulated band: a handful of stations transmitting standard messages -------------------------
struct BandSim {
    struct Sig { FtxSynth syn; float amp; };
    std::vector<Sig> sigs;
    int64_t slot = -1;
    std::mt19937 rng{5};
    void newSlot(int64_t s, const std::string& myCall, bool answerMe) {
        slot = s;
        sigs.clear();
        static const char* even[] = {"CQ K1ABC FN42", "W1AW N0CALL -12", "CQ DX VK2DEF QF56", "G4AAA EA3BB R-09", "CQ PA3XYZ JO22", "JA1ZZZ W6OAT RR73"};
        static const char* odd[] = {"K1ABC WB9XYZ EN52", "CQ N4DEF EM73", "EA3BB G4AAA RR73", "CQ POTA K7PRK DN17", "VE3ABC KD9QRS -03", "CQ SP5QRP KO02"};
        const char** list = (s & 1) ? odd : even;
        std::vector<std::string> msgs(list, list + 6);
        if (answerMe && (s & 1)) msgs.push_back(myCall + " K8TST EN82");
        float f = 420;
        for (auto& m : msgs) {
            std::vector<uint8_t> tones;
            if (!ftxEncode(m, false, tones)) continue;
            Sig g;
            g.syn.start(tones, false, f, 48000);
            g.amp = 600.0f * (1 + (rng() % 5));
            sigs.push_back(g);
            f += 330 + (rng() % 60);
        }
    }
    void fill(int16_t* out, int n, int64_t utc) {
        int64_t s = utc / 15000;
        if (s != slot) return;
        int64_t pos = (utc % 15000 - 500) * 48;       // sample position of this block within the message
        for (auto& g : sigs) {
            g.syn.seek(pos);
            for (int i = 0; i < n; i++) out[i] += (int16_t)(g.amp * g.syn.next());
        }
    }
};

// ---- LVGL display + scripted pointer --------------------------------------------------------------
static const int W = 1024, H = 600;
static uint16_t fb[W * H];
static void flush(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
    int w = a->x2 - a->x1 + 1;
    for (int y = a->y1; y <= a->y2; y++) memcpy(&fb[y * W + a->x1], px + (size_t)(y - a->y1) * w * 2, (size_t)w * 2);
    lv_display_flush_ready(d);
}
static bool touchDown = false;
static int touchX = 0, touchY = 0;
static void touchRead(lv_indev_t*, lv_indev_data_t* data) {
    data->point.x = touchX;
    data->point.y = touchY;
    data->state = touchDown ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static uint32_t tickCb() { return (uint32_t)simMs; }
static void savePpm(const char* path) {
    FILE* f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint16_t p = fb[i];
        uint8_t rgb[3] = {(uint8_t)((p >> 11) * 255 / 31), (uint8_t)(((p >> 5) & 63) * 255 / 63), (uint8_t)((p & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("wrote %s\n", path);
}

#ifndef CONFIRM_X
#define CONFIRM_X 640
#define CONFIRM_Y 350
#endif
static App app;
static FakeRadio radio;
static BandSim band;
static bool answerMe = false;

static void step() {                                  // 20 ms of simulated time
    simMs += 20;
    int64_t utc = clk::utcMs();
    if (utc / 15000 != band.slot) band.newSlot(utc / 15000, "AC9HP", answerMe);
    app.tick();
    static int16_t buf[960];
    static std::mt19937 rng(9);
    static std::normal_distribution<float> nd(0, 500);
    if (app.txActive()) app.txAudio48k(buf, 960);
    else {
        for (int i = 0; i < 960; i++) buf[i] = (int16_t)nd(rng);
        band.fill(buf, 960, utc);
        app.rxAudio48k(buf, 960);
    }
    app.decodeOnce();
    lv_timer_handler();
}
static void run(double seconds) { for (int i = 0; i < seconds * 50; i++) step(); }
static void tap(int x, int y) {
    touchX = x; touchY = y; touchDown = true;
    run(0.12);
    touchDown = false;
    run(0.2);
}
static std::string state() { return app.stateJson(0, 0, false); }

int main() {
    clk::init(mono);
    mkdir("test/out", 0755);
    mkdir("test/out/ui", 0755);
    remove("test/out/ui/log.adi"); remove("test/out/ui/archive.adi"); remove("test/out/ui/settings.txt");
    radio.app = &app;
    app.radio.write = [](const uint8_t* d, size_t n) { radio.rx(d, n); return true; };
    app.begin("test/out/ui");
    app.usbSerial = true;
    app.usbAudio = true;
    app.setNetInfo("AP \"IC7100-FT8\" 192.168.4.1");
    app.command("set", {{"call", "AC9HP"}, {"grid", "EM69"}});
    clk::setUtcMs(T0 + 1000, "browser");

    lv_init();
    lv_tick_set_cb(tickCb);
    lv_display_t* disp = lv_display_create(W, H);
    static uint8_t drawBuf[W * 120 * 2];
    lv_display_set_buffers(disp, drawBuf, nullptr, sizeof drawBuf, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush);
    lv_indev_t* in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, touchRead);

    ui_bind(&app);
    ui_create();
    run(1);
    savePpm("test/out/ui/1_start.ppm");

    run(46);                                          // three slots of band activity
    savePpm("test/out/ui/2_receiving.ppm");
    CHECK(state().find("\"decId\":0,") == std::string::npos);       // something was decoded

    // Bottom bar: tap the 40 m band button -> radio retunes.
    tap(429, 568);
    run(1);
    CHECK(radio.freq == 7074000);
    tap(567, 568);                                    // back to 20 m
    run(1);
    CHECK(radio.freq == 14074000);

    // Waterfall tap sets the transmit audio frequency (x 512 -> 1500 Hz).
    tap(700, 100);
    CHECK(state().find("\"freq\":2050,") != std::string::npos);
    tap(512, 100);
    CHECK(state().find("\"freq\":1500,") != std::string::npos);

    // Operation buttons: Auto CQ, then Enable TX -> it calls CQ in the next even slot.
    tap(628 + 195, 190 + 82 + 26);
    CHECK(state().find("\"op\":1") != std::string::npos);
    tap(628 + 90, 190 + 8 + 30);
    CHECK(state().find("\"en\":true") != std::string::npos);
    answerMe = true;                                  // a station will answer our CQ
    bool keyed = false;
    for (int i = 0; i < 50 * 32 && !keyed; i++) { step(); keyed = radio.ptt; }
    CHECK(keyed);
    run(4);
    savePpm("test/out/ui/3_transmitting.ppm");
    run(60);
    savePpm("test/out/ui/4_qso.ppm");
    CHECK(state().find("K8TST") != std::string::npos);              // the caller was picked up

    // HALT must unkey and disable TX.
    for (int i = 0; i < 50 * 40 && !radio.ptt; i++) step();
    CHECK(radio.ptt);
    tap(628 + 290, 190 + 8 + 30);
    run(0.3);
    CHECK(!radio.ptt && state().find("\"en\":false") != std::string::npos);

    // Tapping a decoded CQ in the list starts a call to that station.
    app.command("op", {{"v", "0"}});
    app.command("clear", {});
    answerMe = false;
    run(32);
    savePpm("test/out/ui/5_list.ppm");
    std::string before = state();
    bool called = false;
    for (int y = 530; y > 270 && !called; y -= 27) {  // walk up the visible rows until one starts a call
        tap(300, y);
        std::string s = state();
        called = s.find("\"dx\":\"\"") == std::string::npos;
    }
    CHECK(called);
    run(1);
    savePpm("test/out/ui/6_called.ppm");
    app.command("halt", {});

    // Logbook screen: open it, clear the current log through the confirmation dialog.
    for (auto c : {"K1ABC", "VK2DEF", "G4AAA", "JA1ZZZ", "PA3XYZ"}) app.command("logqso", {{"call", c}, {"grid", "FN42"}, {"rs", "-12"}, {"rr", "+03"}});
    run(0.5);
    savePpm("test/out/ui/7_main_with_log.ppm");
    tap(628 + 98 + 46, 190 + 288 + 22);               // "Log: 5" button
    run(0.5);
    savePpm("test/out/ui/8_logbook.ppm");
    CHECK(state().find("\"log\":{\"n\":5,\"all\":5") != std::string::npos);
    tap(120, 562);                                    // Clear current log
    run(0.5);
    savePpm("test/out/ui/9_confirm.ppm");
    CHECK(state().find("\"log\":{\"n\":5") != std::string::npos);        // nothing cleared before confirming
    tap(CONFIRM_X, CONFIRM_Y);                        // the dialog's "Clear" button
    run(0.5);
    CHECK(state().find("\"log\":{\"n\":0,\"all\":5") != std::string::npos);
    savePpm("test/out/ui/10_cleared.ppm");
    tap(70, 32);                                      // Back
    run(0.5);
    savePpm("test/out/ui/11_back.ppm");

    // Radio screen (control head): open it and work the controls against the simulated radio.
    tap(628 + 294 + 46, 190 + 288 + 22);              // "Radio" button
    run(4);                                           // let the front-panel values be polled
    savePpm("test/out/ui/12_radio.ppm");
    tap(50, 243);                                     // LSB
    run(0.5);
    CHECK(radio.mode == 0);
    uint64_t f0 = radio.freq;
    tap(822, 112);                                    // + one step (1 kHz)
    run(0.5);
    CHECK(radio.freq == f0 + 1000);
    tap(70, 112);                                     // - one step
    tap(279, 179);                                    // 40 m
    run(0.5);
    CHECK(radio.freq == 7150000);
    tap(322, 307);                                    // NB on
    run(0.5);
    CHECK(radio.func[0x22] == 1);
    tap(70, 307);                                     // ATT on (12 dB on the simulated IC-7100)
    run(0.5);
    CHECK(radio.att == 0x12);
    tap(26 + 145, 489);                               // RF power slider to the middle
    run(0.5);
    CHECK(radio.level[0x0A] > 110 && radio.level[0x0A] < 145);
    run(4);
    savePpm("test/out/ui/13_radio_changed.ppm");
    tap(953, 112);                                    // Enter: keypad
    run(0.5);
    savePpm("test/out/ui/14_keypad.ppm");
    tap(293, 241); tap(293, 333); tap(511, 518); tap(405, 241);     // 1 4 . 2
    tap(680, 333);                                    // OK
    run(0.5);
    CHECK(radio.freq == 14200000);
    tap(70, 36);                                      // Back
    run(0.5);
    tap(567, 568);                                    // 20 m on the operating screen: back to USB-D on the FT8 frequency
    run(1.5);
    CHECK(radio.freq == 14074000 && radio.mode == 1 && radio.dataMode == 1);
    tap(10 + 78 * 2 + 39, 568);                       // CW mode button
    run(0.5);
    CHECK(state().find("\"mode\":\"CW\"") != std::string::npos);
    savePpm("test/out/ui/15_cw_mode.ppm");

    // Logger screen: hand-logged CW contact with a macro.
    tap(628 + 196 + 46, 190 + 288 + 22);              // "SSB/CW" button
    run(1);
    CHECK(!app.seq.txEnabled);
    tap(474, 32);                                     // CW
    run(1);
    CHECK(radio.mode == 3);
    savePpm("test/out/ui/16_logger.ppm");
    // keyboard rows (290 px high, bottom of the screen): type K1ABC
    auto key = [](int row, double units, double total) {
        static const int ys[5] = {339, 397, 455, 513, 571};
        tap((int)((units + 0.5) * W / total), ys[row]);
    };
    key(2, 7, 11); key(0, 0, 12); key(2, 0, 11); key(3, 4, 11); key(3, 2, 11);
    run(0.5);
    savePpm("test/out/ui/17_logger_call.ppm");
    tap(10 + 80, 164);                                // macro 1: CQ
    run(1);
    CHECK(radio.cw.find("CQ CQ DE AC9HP") == 0);
    tap(10 + 1 * 168 + 80, 164);                      // macro 2: his call
    run(1);
    CHECK(radio.cw.find("K1ABC") != std::string::npos);
    tap(952, 101);                                    // LOG
    run(1);
    savePpm("test/out/ui/18_logged.ppm");
    {
        FILE* fp = fopen("test/out/ui/log.adi", "r");
        std::string all; char buf[512];
        while (fp && fgets(buf, sizeof buf, fp)) all += buf;
        if (fp) fclose(fp);
        size_t p = all.find("<CALL:5>K1ABC");
        CHECK(p != std::string::npos);
        if (p != std::string::npos) {
            std::string rec = all.substr(p, all.find("<EOR>", p) - p);
            CHECK(rec.find("<MODE:2>CW") != std::string::npos && rec.find("<RST_SENT:3>599") != std::string::npos);
        }
    }
    // Typed frequency and chosen mode are logged instead of the radio's, then the box follows the radio again.
    auto key2 = [](int row, double units, double total) {
        static const int ys[5] = {339, 397, 455, 513, 571};
        tap((int)((units + 0.5) * W / total), ys[row]);
    };
    tap(222, 32);                                     // frequency box
    key2(3, 9, 11);                                   // Clear
    key2(0, 6, 12); key2(3, 8, 11); key2(0, 9, 12); key2(0, 2, 12); key2(0, 9, 12);   // 7.030
    radio.freq = 14025000;                            // the radio moves meanwhile: the typed value must stay
    run(1.5);
    tap(368, 32);                                     // SSB
    run(0.5);
    tap(135, 101);                                    // callsign box
    key2(2, 7, 11); key2(0, 1, 12); key2(2, 1, 11); key2(2, 1, 11); key2(0, 1, 12);  // K2SS2
    savePpm("test/out/ui/19_logger_typed.ppm");
    tap(952, 101);                                    // LOG
    run(1.5);
    savePpm("test/out/ui/20_logger_after.ppm");
    {
        FILE* fp = fopen("test/out/ui/log.adi", "r");
        std::string all; char buf[512];
        while (fp && fgets(buf, sizeof buf, fp)) all += buf;
        if (fp) fclose(fp);
        size_t p = all.find("<CALL:5>K2SS2");
        CHECK(p != std::string::npos);
        if (p != std::string::npos) {
            std::string rec = all.substr(p, all.find("<EOR>", p) - p);
            CHECK(rec.find("<FREQ:8>7.030000") != std::string::npos && rec.find("<MODE:3>SSB") != std::string::npos);
            CHECK(rec.find("<SUBMODE:3>LSB") != std::string::npos && rec.find("<RST_SENT:2>59") != std::string::npos);
        }
    }
    tap(60, 32);                                      // Back
    run(0.5);

    // Settings screen
    tap(871, 31);                                     // gear
    run(0.5);
    savePpm("test/out/ui/21_settings.ppm");
    tap(85, 64 + 45 + 2 * 89);                        // Radio tab
    run(0.5);
    savePpm("test/out/ui/22_settings_radio.ppm");
    tap(85, 64 + 45 + 3 * 89);                        // CW tab
    run(0.3);
    tap(170 + 16 + 270 + 200, 64 + 16 + 62 + 26);     // macro 1 text box: keyboard comes up
    run(0.5);
    savePpm("test/out/ui/23_settings_kb.ppm");
    std::string cfgBefore = app.cfg.serialize(true);
    tap(944, 32);                                     // Save, nothing changed
    run(0.5);
    CHECK(app.cfg.serialize(true) == cfgBefore);
    app.command("set", {{"mySigInfo", "K-1234"}});
    tap(85, 64 + 45);                                 // Station tab
    tap(60, 32);                                      // Back ...
    run(0.3);
    tap(871, 31);                                     // ... and in again: shows the new value
    run(0.5);
    savePpm("test/out/ui/24_settings_station.ppm");
    tap(60, 32);
    run(0.3);
    printf(fails ? "ui_sim: %d of %d FAILED\n" : "ui_sim: all %d checks passed\n", fails ? fails : checks, checks);
    return fails ? 1 : 0;
}
