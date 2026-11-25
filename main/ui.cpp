// On-device touch interface (LVGL 9) for the 1024 x 600 display.
// Everything here runs in the LVGL task: ui_create() is called with the display lock held, and
// all later work happens in LVGL timers and event callbacks. It talks to the application only
// through App::uiState/uiDecodes/uiSpectrum (read) and App::command (the same commands the web
// page sends), so the screen and the web page can be used side by side.
#include "ui.h"
#include "app.h"
#include "lvgl.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <map>
#include <algorithm>

namespace {

App* g_app = nullptr;

const int W = 1024, H = 600;
const int HEAD_H = 58, SLOT_H = 6, WF_Y = HEAD_H + SLOT_H, WF_H = 90, LINE_Y = WF_Y + WF_H, LINE_H = 36;
const int MAIN_Y = LINE_Y + LINE_H, BOT_H = 64, MAIN_H = H - MAIN_Y - BOT_H, TABLE_W = 620, PANEL_X = 628;

// Every colour goes through C(); the Contrast setting (50-150 %) stretches each channel around a
// dark grey (the UI is mostly dark panels with light text), so higher contrast makes panels darker
// and text and dim labels much brighter; lower contrast greys everything towards the panels.
// Read once at start-up.
int gContrast = 100;
inline lv_color_t C(uint32_t rgb) {
    if (gContrast != 100) {
        auto f = [](int v) { v = 60 + (v - 60) * gContrast / 100; return v < 0 ? 0 : v > 255 ? 255 : v; };
        rgb = (uint32_t)(f((rgb >> 16) & 255) << 16 | f((rgb >> 8) & 255) << 8 | f(rgb & 255));
    }
    return lv_color_hex(rgb);
}
const uint32_t BG = 0x0d1317, PAN = 0x151e25, PAN2 = 0x1c2831, LINE = 0x2a3944, TXC = 0xff5b5b, RXC = 0x3fcf8e,
               AMB = 0xf6b93b, TXT = 0xdce5ea, DIM = 0x8498a5, CQC = 0x7fd0ff, MEC = 0xff9d9d;

struct BandDef { const char* name; uint32_t ft8, ft4, cw; };       // dial frequencies per mode (0 = none)
const BandDef kBands[] = {{"160", 1840000, 0, 1835000}, {"80", 3573000, 3575000, 3565000}, {"60", 5357000, 0, 0}, {"40", 7074000, 7047500, 7065000},
                          {"30", 10136000, 10140000, 10125000}, {"20", 14074000, 14080000, 14065000}, {"17", 18100000, 18104000, 18095000},
                          {"15", 21074000, 21140000, 21065000}, {"12", 24915000, 24919000, 24910000}, {"10", 28074000, 28180000, 28065000},
                          {"6", 50313000, 50318000, 50095000}, {"2", 144174000, 144170000, 144065000}};
uint32_t bandFreq(int i, int mode) { return mode == DM_FT4 ? kBands[i].ft4 : (mode == DM_CW ? kBands[i].cw : kBands[i].ft8); }
const int kNumBands = sizeof kBands / sizeof kBands[0];
const char* kBandMap[kNumBands + 1];
const char* kModeMap[] = {"FT8", "FT4", "CW", ""};        // CW = FT-CW: FT8-style contacts in Morse
int modeIndex(int mode) { return mode == DM_CW ? 2 : (mode == DM_FT8 || mode == DM_FT4 ? mode : -1); }
const char* kOpMap[] = {"Manual", "Auto CQ", "Auto hunt", ""};   // hunt = answer CQs automatically

// widgets
lv_obj_t *bCq, *lCq;
lv_obj_t *lFreq, *lSub, *barS, *lS, *barLvl, *lLvl, *lDot[4], *lClock, *lCall, *barSlot, *canvas, *marker, *lTx, *lStatus;
lv_obj_t *table, *bTxEn, *lTxEn, *bTune, *lTune, *mxOp, *mxMode, *mxBand, *lQso, *lMeters, *bPar, *lPar, *lLogBtn, *lPsk;
lv_obj_t *scrMain, *scrLog, *scrLogger, *logTable, *lLogCounts, *lLogInfo;        // second screen: logbook
void onOpenRadio(lv_event_t*);
void buildRadioScreen();
void refreshRadioScreen(const App::UiState& s);
void onOpenLogger(lv_event_t*);
void onOpenSettings(lv_event_t*);
void buildLoggerScreen();
void refreshLogger(const App::UiState& s);
int shownLogN = -1, shownLogAll = -1;
uint16_t* wfBuf = nullptr;
uint16_t lut[256];
uint32_t specSeq = 0xFFFFFFFF;

// band activity model
struct Row { uint32_t id; int kind; int par; std::string c[5]; };   // kind: 0 normal, 1 CQ, 2 to me, 3 our TX
std::vector<Row> rows;
uint32_t lastDecId = 0;
std::string lastTxText;
uint32_t lastTouchTick = 0;
int shownOp = -1, shownMode = -1, shownBand = -2;
std::string noteText;
uint32_t noteUntil = 0;

void setText(lv_obj_t* l, const std::string& s) {
    if (strcmp(lv_label_get_text(l), s.c_str()) != 0) lv_label_set_text(l, s.c_str());
}
void setTextColor(lv_obj_t* o, uint32_t rgb) {
    lv_color_t c = C(rgb);
    if (!lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), c)) lv_obj_set_style_text_color(o, c, LV_PART_MAIN);
}
void setBg(lv_obj_t* o, uint32_t rgb, lv_part_t part = LV_PART_MAIN) {
    lv_color_t c = C(rgb);
    if (!lv_color_eq(lv_obj_get_style_bg_color(o, part), c)) lv_obj_set_style_bg_color(o, c, part);
}

void note(const std::string& s) {                  // short-lived message on the status line
    noteText = s;
    noteUntil = lv_tick_get() + 6000;
}

// Send a command; show the error text if the application refuses it.
bool cmd(const char* c, std::map<std::string, std::string> args = {}) {
    if (!g_app) return false;
    std::string r = g_app->command(c, args);
    if (r.find("\"ok\":true") != std::string::npos) return true;
    size_t p = r.find("\"err\":\"");
    if (p != std::string::npos) {
        size_t e = r.find('"', p + 7);
        note(r.substr(p + 7, e == std::string::npos ? std::string::npos : e - p - 7));
    }
    return false;
}
std::string num(long long v) { char b[24]; snprintf(b, sizeof b, "%lld", v); return b; }

// ---- construction helpers -----------------------------------------------------------------------
lv_obj_t* box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t bg) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, C(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}
lv_obj_t* label(lv_obj_t* parent, int x, int y, const lv_font_t* font, uint32_t color, const char* text = "") {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, C(color), 0);
    lv_label_set_text(l, text);
    return l;
}
lv_obj_t* button(lv_obj_t* parent, int x, int y, int w, int h, const char* text, lv_event_cb_t cb, lv_obj_t** lbl = nullptr,
                 uint32_t bg = PAN2) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, C(bg), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, C(LINE), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, C(TXT), 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    if (lbl) *lbl = l;
    return b;
}
lv_obj_t* matrix(lv_obj_t* parent, int x, int y, int w, int h, const char* const map[], lv_event_cb_t cb) {
    lv_obj_t* m = lv_buttonmatrix_create(parent);
    lv_obj_set_pos(m, x, y);
    lv_obj_set_size(m, w, h);
    lv_buttonmatrix_set_map(m, map);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_set_style_pad_gap(m, 6, 0);
    lv_obj_set_style_text_font(m, &lv_font_montserrat_20, 0);
    lv_obj_set_style_bg_color(m, C(PAN2), LV_PART_ITEMS);
    lv_obj_set_style_text_color(m, C(TXT), LV_PART_ITEMS);
    lv_obj_set_style_radius(m, 8, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(m, 0, LV_PART_ITEMS);
    lv_obj_set_style_border_width(m, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(m, C(LINE), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, C(AMB), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(m, C(0x1a1300), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(m, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return m;
}
void matrixCheck(lv_obj_t* m, int count, int which) {     // show exactly one (or no) button as selected
    for (int i = 0; i < count; i++) {
        if (i == which) lv_buttonmatrix_set_button_ctrl(m, i, LV_BUTTONMATRIX_CTRL_CHECKED);
        else lv_buttonmatrix_clear_button_ctrl(m, i, LV_BUTTONMATRIX_CTRL_CHECKED);
    }
}
lv_obj_t* meter(lv_obj_t* parent, int x, int y, int w) {
    lv_obj_t* b = lv_bar_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, 10);
    lv_bar_set_range(b, 0, 100);
    lv_obj_set_style_bg_color(b, C(0x0b1014), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, C(RXC), LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 3, LV_PART_INDICATOR);
    return b;
}

// ---- events -------------------------------------------------------------------------------------
void touched() { lastTouchTick = lv_tick_get(); }

void onTxEn(lv_event_t*) {
    App::UiState s;
    g_app->uiState(s);
    cmd("txen", {{"v", s.txEn ? "0" : "1"}});
}
void onHalt(lv_event_t*) { cmd("halt"); }
void onTune(lv_event_t*) {
    App::UiState s;
    g_app->uiState(s);
    cmd("tune", {{"v", s.tune ? "0" : "1"}});
}
bool gWaitNext = false;
void onCq(lv_event_t*) { cmd(gWaitNext ? "nextqso" : "cq"); }   // POTA mode: the CQ button becomes Next QSO
void onClear(lv_event_t*) { cmd("clear"); }
void onLogDx(lv_event_t*) { cmd("logdx"); }
void onParity(lv_event_t*) {
    App::UiState s;
    g_app->uiState(s);
    cmd("parity", {{"v", s.txPar ? "0" : "1"}});
}
void onOp(lv_event_t* e) {
    uint32_t id = lv_buttonmatrix_get_selected_button((lv_obj_t*)lv_event_get_target(e));
    if (id < 3) cmd("op", {{"v", num(id)}});
    shownOp = -1;
}
void onMode(lv_event_t* e) {
    uint32_t id = lv_buttonmatrix_get_selected_button((lv_obj_t*)lv_event_get_target(e));
    if (id < 3 && cmd("mode", {{"m", kModeMap[id]}})) { rows.clear(); lv_table_set_row_count(table, 0); }
    shownMode = -1;
    shownBand = -2;
}
void onBand(lv_event_t* e) {
    uint32_t id = lv_buttonmatrix_get_selected_button((lv_obj_t*)lv_event_get_target(e));
    if (id < (uint32_t)kNumBands) {
        uint32_t hz = bandFreq((int)id, g_app->mode());
        if (hz) cmd("band", {{"hz", num(hz)}});
        else note("No frequency for this mode on that band");
    }
    shownBand = -2;
}
void onWaterfall(lv_event_t*) {
    lv_indev_t* in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    int hz = (int)((long)p.x * 3000 / W / 10) * 10;
    if (hz >= 200 && hz <= 2900) cmd("txfreq", {{"hz", num(hz)}});
}
void onTable(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_SCROLL) { touched(); return; }
    if (code != LV_EVENT_VALUE_CHANGED) return;
    uint32_t r, c;
    lv_table_get_selected_cell(table, &r, &c);
    if (r >= rows.size() || rows[r].id == 0) return;
    if (cmd("call", {{"id", num(rows[r].id)}})) note("Calling the sender of: " + rows[r].c[4]);
}
// Row colours: text by message type, background alternating by time slot.
void onTableDraw(lv_event_t* e) {
    lv_draw_task_t* t = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t* base = (lv_draw_dsc_base_t*)lv_draw_task_get_draw_dsc(t);
    if (!base || base->part != LV_PART_ITEMS || base->id1 >= rows.size()) return;
    const Row& r = rows[base->id1];
    lv_draw_label_dsc_t* ld = lv_draw_task_get_label_dsc(t);
    if (ld) {
        ld->color = C(r.kind == 1 ? CQC : (r.kind == 2 ? MEC : (r.kind == 3 ? AMB : TXT)));
        ld->align = base->id2 == 4 ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT;
    }
    lv_draw_fill_dsc_t* fd = lv_draw_task_get_fill_dsc(t);
    if (fd) {
        fd->color = C(r.par ? 0x0b1014 : 0x131c23);
        fd->opa = LV_OPA_COVER;
    }
}

// ---- logbook screen ---------------------------------------------------------------------------
std::vector<App::UiLogEntry> gLogRows;             // what the logbook table shows, newest first
void fillLogTable() {
    std::vector<App::UiLogEntry>& e = gLogRows;
    e.clear();
    g_app->uiLogTail(60, e);
    lv_table_set_row_count(logTable, (uint32_t)e.size() + 1);
    static const char* head[7] = {"UTC", "Call", "Grid", "Band", "Mode", "Sent", "Rcvd"};
    for (int c = 0; c < 7; c++) lv_table_set_cell_value(logTable, 0, c, head[c]);
    for (size_t r = 0; r < e.size(); r++) {
        const std::string* f[7] = {&e[r].when, &e[r].call, &e[r].grid, &e[r].band, &e[r].mode, &e[r].sent, &e[r].rcvd};
        for (int c = 0; c < 7; c++) lv_table_set_cell_value(logTable, (uint32_t)r + 1, c, f[c]->c_str());
    }
    lv_obj_scroll_to_y(logTable, 0, LV_ANIM_OFF);
}
void onLogDraw(lv_event_t* e) {
    lv_draw_task_t* t = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t* base = (lv_draw_dsc_base_t*)lv_draw_task_get_draw_dsc(t);
    if (!base || base->part != LV_PART_ITEMS) return;
    lv_draw_label_dsc_t* ld = lv_draw_task_get_label_dsc(t);
    if (ld) ld->color = C(base->id1 == 0 ? DIM : (base->id2 == 1 ? AMB : TXT));
    lv_draw_fill_dsc_t* fd = lv_draw_task_get_fill_dsc(t);
    if (fd) { fd->color = C(base->id1 == 0 ? PAN : (base->id1 % 2 ? 0x0b1014 : 0x131c23)); fd->opa = LV_OPA_COVER; }
}
void onOpenLog(lv_event_t*) {
    cmd("ping");                                    // counts as operator activity for the TX watchdog
    fillLogTable();
    shownLogN = -1;
    lv_screen_load(scrLog);
}
void onCloseLog(lv_event_t*) { lv_screen_load(scrMain); }

// Confirmation dialog: runs `command` (with its arguments and confirm=yes) when the operator presses the action button.
struct Confirm { lv_obj_t* box; const char* command; std::map<std::string, std::string> args; };
Confirm g_confirm = {nullptr, nullptr, {}};
lv_obj_t* scrLogEdit = nullptr;
void confirm(const char* title, const std::string& text, const char* action, const char* command,
             const std::map<std::string, std::string>& args = {});
void onConfirmNo(lv_event_t*) {
    if (g_confirm.box) lv_msgbox_close(g_confirm.box);
    g_confirm.box = nullptr;
}
void onConfirmYes(lv_event_t*) {
    const char* c = g_confirm.command;
    std::map<std::string, std::string> args = g_confirm.args;
    args["confirm"] = "yes";
    onConfirmNo(nullptr);
    if (c && !strcmp(c, "sdfmt1")) {
        confirm("Format microSD card - last chance", "Really erase the card?\nIt takes up to a minute; wait for it.", "Format", "sdformat");
        return;
    }
    if (c && !strcmp(c, "nuke1")) {                    // erasing every log asks twice
        confirm("Erase contact logs - last chance", "Really erase every contact of every station?\nThere is no undo.", "Erase all", "eraselogs");
        return;
    }
    if (c && cmd(c, args)) {
        fillLogTable();
        shownLogN = -1;
        if (lv_screen_active() == scrLogEdit) lv_screen_load(scrLog);   // the contact that was open is gone
    }
}
void confirm(const char* title, const std::string& text, const char* action, const char* command,
             const std::map<std::string, std::string>& args) {
    if (g_confirm.box) return;
    lv_obj_t* m = lv_msgbox_create(nullptr);
    g_confirm = {m, command, args};
    lv_obj_set_width(m, 560);
    lv_obj_set_style_text_font(m, &lv_font_montserrat_20, 0);
    lv_msgbox_add_title(m, title);
    lv_msgbox_add_text(m, text.c_str());
    lv_obj_t* no = lv_msgbox_add_footer_button(m, "Cancel");
    lv_obj_t* yes = lv_msgbox_add_footer_button(m, action);
    lv_obj_set_style_bg_color(yes, C(0x9a2a2a), 0);
    lv_obj_set_style_min_height(lv_msgbox_get_footer(m), 84, 0);
    lv_obj_set_size(no, 170, 56);                   // finger-sized
    lv_obj_set_size(yes, 170, 56);
    lv_obj_add_event_cb(no, onConfirmNo, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(yes, onConfirmYes, LV_EVENT_CLICKED, nullptr);
}
void onClearLog(lv_event_t*) {
    App::UiState s;
    g_app->uiState(s);
    if (s.logN == 0) { note("The current log is already empty"); return; }
    confirm("Clear current log", "Remove " + num(s.logN) + " contact(s) from the current log?\nThey stay in the archive.", "Clear", "clearlog");
}
lv_obj_t *bLogSync, *lLogSync;
int shownUnsent = -1;
void onLogSync(lv_event_t*) { cmd("logsync"); }
void onUndoDelete(lv_event_t*) {
    App::UiLogEntry e;
    int n = 0;
    g_app->uiTrashNewest(e, n);
    if (e.idx < 0) { note("Nothing was deleted in the last 7 days"); return; }
    confirm("Restore deleted contact", "Put back " + e.call + (e.when.empty() ? "" : " (" + e.when + " UTC, " + e.band + ")") +
            "?\n" + num(n) + " deleted contact(s) are kept for 7 days; the web page lists them all.", "Restore", "logrestore",
            {{"i", num(e.idx)}, {"call", e.call}});
}
void onEraseAll(lv_event_t*) {
    App::UiState s;
    g_app->uiState(s);
    confirm("Erase everything", "Erase the archive (" + num(s.logAll) + " contacts) AND the current log?\n"
            "Download them from the web page first. This cannot be undone.", "Erase all", "eraselogs");
}

extern const lv_buttonmatrix_ctrl_t kLogKeyCtrl[];   // logger keyboard layout (defined with the logger)
// ---- edit one contact (tap it in the logbook) ----------------------------------------------------
lv_obj_t *eField[10], *eKb, *eTitle, *eMsg;
int eFocus = 0;
App::UiLogEntry eRec;                                 // the contact as it was when opened
// Call, date, time, MHz, mode, sent, rcvd, grid, name, comment
const char* const kEditAdif[10] = {"CALL", "QSO_DATE", "TIME_ON", "FREQ", "", "RST_SENT", "RST_RCVD", "GRIDSQUARE", "NAME", "COMMENT"};
const char* kEditKeys[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
                           "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "/", "\n",
                           "A", "S", "D", "F", "G", "H", "J", "K", "L", "+", "-", "\n",
                           "Z", "X", "C", "V", "B", "N", "M", ",", ".", "Clear", "\n",
                           "Next", " ", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, "SAVE", ""};
std::string eOrig(int i) {
    const App::UiLogEntry& r = eRec;
    const std::string* v[10] = {&r.call, &r.date, &r.time, &r.freq, &r.mode, &r.sent, &r.rcvd, &r.grid, &r.name, &r.comment};
    return *v[i];
}
void editFocus(int i) {
    eFocus = i;
    lv_keyboard_set_textarea(eKb, eField[i]);
    for (int k = 0; k < 10; k++) lv_obj_set_style_border_color(eField[k], C(k == i ? AMB : LINE), 0);
}
void editSave() {
    std::map<std::string, std::string> a = {{"file", "cur"}, {"i", num(eRec.idx)}, {"call", eRec.call}};
    for (int i = 0; i < 10; i++) {
        std::string v = lv_textarea_get_text(eField[i]);
        if (v == eOrig(i)) continue;
        if (i == 4) {                                 // mode as shown (USB, FT8, FT4, CW...) -> MODE + SUBMODE
            std::string m, sm;
            adifModeFromText(v, m, sm);
            a["f.MODE"] = m;
            a["f.SUBMODE"] = sm;
        } else a[std::string("f.") + kEditAdif[i]] = v;
    }
    if (a.size() == 3) { lv_screen_load(scrLog); return; }   // nothing changed
    if (!cmd("logedit", a)) { setText(eMsg, "Not saved - see the message at the top"); return; }
    fillLogTable();
    shownLogN = -1;
    lv_screen_load(scrLog);
}
void onEditField(lv_event_t* e) { editFocus((int)(intptr_t)lv_event_get_user_data(e)); }
void onEditKey(lv_event_t* e) {
    lv_obj_t* kb = (lv_obj_t*)lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char* t = lv_buttonmatrix_get_button_text(kb, id);
    if (!t) return;
    if (!strcmp(t, "Next")) editFocus((eFocus + 1) % 10);
    else if (!strcmp(t, "SAVE")) editSave();
    else if (!strcmp(t, "Clear")) lv_textarea_set_text(eField[eFocus], "");
    else lv_keyboard_def_event_cb(e);
}
void onEditSave(lv_event_t*) { editSave(); }
void onEditBack(lv_event_t*) { lv_screen_load(scrLog); }
void onEditDelete(lv_event_t*) {
    confirm("Delete contact", "Delete the contact with " + eRec.call + (eRec.when.empty() ? "" : " (" + eRec.when + ")") +
            "\nfrom the current log and the archive? This cannot be undone.", "Delete", "logdel",
            {{"file", "cur"}, {"i", num(eRec.idx)}, {"call", eRec.call}});
}
void openLogEdit(const App::UiLogEntry& r) {
    cmd("ping");
    eRec = r;
    for (int i = 0; i < 10; i++) lv_textarea_set_text(eField[i], eOrig(i).c_str());
    setText(eTitle, "Edit contact: " + r.call);
    setText(eMsg, "");
    editFocus(0);
    lv_screen_load(scrLogEdit);
}
void onLogTable(lv_event_t* e) {
    uint32_t row = LV_TABLE_CELL_NONE, col = LV_TABLE_CELL_NONE;
    lv_table_get_selected_cell((lv_obj_t*)lv_event_get_target(e), &row, &col);
    if (row == LV_TABLE_CELL_NONE || row == 0 || row > gLogRows.size()) return;
    openLogEdit(gLogRows[row - 1]);
}

void buildLogEditScreen() {
    const lv_font_t *f16 = &lv_font_montserrat_16, *f20 = &lv_font_montserrat_20, *f28 = &lv_font_montserrat_28;
    scrLogEdit = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scrLogEdit, C(BG), 0);
    lv_obj_set_style_bg_opa(scrLogEdit, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scrLogEdit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* head = box(scrLogEdit, 0, 0, W, 64, PAN);
    button(head, 10, 8, 130, 48, LV_SYMBOL_LEFT "  Back", onEditBack);
    eTitle = label(head, 160, 16, f28, TXT, "Edit contact");
    static const struct { const char* hint; int x, y, w, max; const char* chars; } fd[10] = {
        {"Callsign", 10, 92, 230, 11, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/"},
        {"Date YYYYMMDD", 248, 92, 190, 8, "0123456789"},
        {"UTC HHMM", 446, 92, 140, 6, "0123456789"},
        {"MHz", 594, 92, 170, 11, "0123456789."},
        {"Mode", 772, 92, 242, 8, nullptr},
        {"Sent", 10, 176, 110, 6, nullptr}, {"Rcvd", 128, 176, 110, 6, nullptr},
        {"Grid", 246, 176, 130, 8, nullptr},
        {"Name", 384, 176, 230, 30, nullptr}, {"Comment", 622, 176, 392, 80, nullptr}};
    static const char* cap[10] = {"Call", "Date (UTC)", "Time", "Frequency", "Mode", "Sent", "Rcvd", "Grid", "Name", "Comment"};
    for (int i = 0; i < 10; i++) {
        label(scrLogEdit, fd[i].x + 2, fd[i].y - 22, f16, DIM, cap[i]);
        lv_obj_t* ta = lv_textarea_create(scrLogEdit);
        lv_obj_set_pos(ta, fd[i].x, fd[i].y);
        lv_obj_set_size(ta, fd[i].w, 54);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_max_length(ta, (uint32_t)fd[i].max);
        if (fd[i].chars) lv_textarea_set_accepted_chars(ta, fd[i].chars);
        lv_textarea_set_placeholder_text(ta, fd[i].hint);
        lv_obj_set_style_text_font(ta, f20, 0);
        lv_obj_set_style_pad_ver(ta, 12, 0);
        lv_obj_set_style_bg_color(ta, C(0x0b1014), 0);
        lv_obj_set_style_text_color(ta, C(TXT), 0);
        lv_obj_set_style_border_color(ta, C(LINE), 0);
        lv_obj_set_style_border_width(ta, 2, 0);
        lv_obj_set_style_radius(ta, 8, 0);
        lv_obj_add_event_cb(ta, onEditField, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        eField[i] = ta;
    }
    lv_obj_t* lSave;
    button(scrLogEdit, 10, 240, 200, 56, "Save", onEditSave, &lSave, 0x1f6b45);
    button(scrLogEdit, 220, 240, 200, 56, "Delete contact", onEditDelete, nullptr, 0x7a2020);
    eMsg = label(scrLogEdit, 436, 248, f16, DIM, "");
    lv_obj_set_width(eMsg, W - 446);
    label(scrLogEdit, 436, 272, f16, DIM, "Changes apply to the current log and the archive.");
    eKb = lv_keyboard_create(scrLogEdit);
    lv_keyboard_set_map(eKb, LV_KEYBOARD_MODE_USER_1, kEditKeys, kLogKeyCtrl);
    lv_keyboard_set_mode(eKb, LV_KEYBOARD_MODE_USER_1);
    lv_obj_set_size(eKb, W, 296);
    lv_obj_align(eKb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(eKb, f20, 0);
    lv_obj_set_style_bg_color(eKb, C(PAN), 0);
    lv_obj_set_style_bg_color(eKb, C(PAN2), LV_PART_ITEMS);
    lv_obj_set_style_text_color(eKb, C(TXT), LV_PART_ITEMS);
    lv_obj_remove_event_cb(eKb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(eKb, onEditKey, LV_EVENT_VALUE_CHANGED, nullptr);
}

void buildLogScreen() {
    const lv_font_t *f16 = &lv_font_montserrat_16, *f20 = &lv_font_montserrat_20, *f28 = &lv_font_montserrat_28;
    scrLog = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scrLog, C(BG), 0);
    lv_obj_set_style_bg_opa(scrLog, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scrLog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* head = box(scrLog, 0, 0, W, 64, PAN);
    button(head, 10, 8, 130, 48, LV_SYMBOL_LEFT "  Back", onCloseLog);
    label(head, 160, 16, f28, TXT, "Logbook");
    lLogCounts = label(head, 320, 22, f20, AMB);
    button(head, W - 250, 8, 240, 48, LV_SYMBOL_REFRESH "  Undo delete", onUndoDelete);

    const int TH = H - 64 - 76;
    logTable = lv_table_create(scrLog);
    lv_obj_set_pos(logTable, 0, 64);
    lv_obj_set_size(logTable, W, TH);
    lv_table_set_column_count(logTable, 7);
    static const int colW[7] = {170, 200, 120, 110, 120, 140, 152};
    for (int c = 0; c < 7; c++) lv_table_set_column_width(logTable, c, colW[c]);
    lv_obj_set_style_bg_color(logTable, C(0x0b1014), 0);
    lv_obj_set_style_border_width(logTable, 0, 0);
    lv_obj_set_style_radius(logTable, 0, 0);
    lv_obj_set_style_pad_all(logTable, 0, 0);
    lv_obj_set_style_text_font(logTable, f20, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(logTable, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_width(logTable, 0, LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(logTable, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(logTable, 10, LV_PART_ITEMS);
    lv_obj_add_event_cb(logTable, onLogDraw, LV_EVENT_DRAW_TASK_ADDED, nullptr);
    lv_obj_add_event_cb(logTable, onLogTable, LV_EVENT_VALUE_CHANGED, nullptr);   // tap a contact to edit it
    lv_obj_add_flag(logTable, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);

    lv_obj_t* bot = box(scrLog, 0, H - 76, W, 76, PAN);
    button(bot, 10, 10, 200, 56, "Clear current log", onClearLog);
    button(bot, 218, 10, 200, 56, "Erase archive + log", onEraseAll, nullptr, 0x7a2020);
    bLogSync = button(bot, 426, 10, 220, 56, "Send unsent", onLogSync, &lLogSync, 0x1f5a3a);
    lLogInfo = label(bot, 656, 6, f16, DIM);
    lv_obj_set_width(lLogInfo, W - 656 - 10);
}

// ---- radio screen: a control head for the transceiver --------------------------------------------
lv_obj_t *scrRadio, *rFreq, *rInfo, *rBarS, *rMeter, *mxStep, *mxRBand, *mxRMode, *mxFil, *bData, *lData, *kbBox, *kbText;
struct FuncBtn { const char* name; uint8_t sub; int states; lv_obj_t* btn; lv_obj_t* lbl; int shown; };
FuncBtn rFunc[] = {{"PRE", 0x02, 3, nullptr, nullptr, -2}, {"NB", 0x22, 2, nullptr, nullptr, -2}, {"NR", 0x40, 2, nullptr, nullptr, -2},
                   {"NOTCH", 0x41, 2, nullptr, nullptr, -2}, {"AGC", 0x12, 3, nullptr, nullptr, -2}, {"COMP", 0x44, 2, nullptr, nullptr, -2},
                   {"VOX", 0x46, 2, nullptr, nullptr, -2}};
lv_obj_t *bAtt, *lAtt;
int shownAtt = -2;
struct LevelCtl { const char* name; uint8_t sub; lv_obj_t* slider; lv_obj_t* lbl; int shown; };
LevelCtl rLevel[] = {{"AF gain", 0x01, nullptr, nullptr, -2}, {"RF gain", 0x02, nullptr, nullptr, -2}, {"Squelch", 0x03, nullptr, nullptr, -2},
                     {"RF power", 0x0A, nullptr, nullptr, -2}, {"Mic gain", 0x0B, nullptr, nullptr, -2}, {"NR level", 0x06, nullptr, nullptr, -2}};
const long kSteps[] = {10, 100, 1000, 10000, 100000, 1000000};
const char* kStepMap[] = {"10 Hz", "100 Hz", "1 kHz", "10 kHz", "100 kHz", "1 MHz", ""};
int stepIdx = 2;
struct RBand { const char* name; uint32_t lo, hi, home; };
const RBand kRBands[] = {{"160", 1800000, 2000000, 1900000}, {"80", 3500000, 4000000, 3750000}, {"60", 5250000, 5450000, 5357000},
                         {"40", 7000000, 7300000, 7150000}, {"30", 10100000, 10150000, 10120000}, {"20", 14000000, 14350000, 14200000},
                         {"17", 18068000, 18168000, 18130000}, {"15", 21000000, 21450000, 21250000}, {"12", 24890000, 24990000, 24950000},
                         {"10", 28000000, 29700000, 28400000}, {"6", 50000000, 54000000, 50125000}, {"2", 144000000, 148000000, 144200000},
                         {"70cm", 420000000, 450000000, 432100000}};
const int kNumRBands = sizeof kRBands / sizeof kRBands[0];
const char* kRBandMap[kNumRBands + 1];
struct RMode { const char* name; uint8_t code; };
const RMode kRModes[] = {{"LSB", 0x00}, {"USB", 0x01}, {"CW", 0x03}, {"AM", 0x02}, {"FM", 0x05}, {"RTTY", 0x04}, {"DV", 0x17}};
const int kNumRModes = sizeof kRModes / sizeof kRModes[0];
const char* kRModeMap[kNumRModes + 1];
const char* kFilMap[] = {"FIL1", "FIL2", "FIL3", ""};
int shownRBand = -2, shownRMode = -2, shownFil = -2, shownData = -2;

std::string hex(int v) { char b[8]; snprintf(b, sizeof b, "%02X", v); return b; }
lv_obj_t* radioReturn = nullptr;                      // the screen "Back" goes to
void onOpenRadio(lv_event_t*) {
    cmd("rext", {{"v", "1"}});
    radioReturn = scrMain;
    lv_screen_load(scrRadio);
}
void onCloseRadio(lv_event_t*) {
    if (radioReturn && radioReturn != scrMain) { lv_screen_load(radioReturn); return; }   // back to the logger, which also polls
    cmd("rext", {{"v", "0"}});
    lv_screen_load(scrMain);
}
void onStepSel(lv_event_t* e) {
    uint32_t id = lv_buttonmatrix_get_selected_button((lv_obj_t*)lv_event_get_target(e));
    if (id < 6) stepIdx = (int)id;
    matrixCheck(mxStep, 6, stepIdx);
}
void onTuneDown(lv_event_t*) { cmd("rstep", {{"d", num(-kSteps[stepIdx])}}); }
void onTuneUp(lv_event_t*) { cmd("rstep", {{"d", num(kSteps[stepIdx])}}); }
void onRBand(lv_event_t* e) {
    uint32_t id = lv_buttonmatrix_get_selected_button((lv_obj_t*)lv_event_get_target(e));
    if (id < (uint32_t)kNumRBands) cmd("qsy", {{"hz", num(kRBands[id].home)}});
    shownRBand = -2;
}
void onRMode(lv_event_t* e) {
    uint32_t id = lv_buttonmatrix_get_selected_button((lv_obj_t*)lv_event_get_target(e));
    int fil = g_app->radio.filter.load();
    if (id < (uint32_t)kNumRModes) cmd("rmode", {{"m", hex(kRModes[id].code)}, {"fil", num(fil >= 1 && fil <= 3 ? fil : 1)}});
    shownRMode = -2;
}
void onFil(lv_event_t* e) {
    uint32_t id = lv_buttonmatrix_get_selected_button((lv_obj_t*)lv_event_get_target(e));
    int m = g_app->radio.mode.load();
    if (id < 3 && m >= 0) cmd("rmode", {{"m", hex(m)}, {"fil", num(id + 1)}});
    shownFil = -2;
}
void onData(lv_event_t*) { cmd("rdata", {{"v", g_app->radio.dataMode.load() == 1 ? "0" : "1"}}); }
void onVfoSwap(lv_event_t*) { cmd("rvfo", {{"v", "B0"}}); }
void onFunc(lv_event_t* e) {
    FuncBtn* f = (FuncBtn*)lv_event_get_user_data(e);
    int cur = g_app->radio.func[f->sub].load();
    int next;
    if (f->sub == 0x12) next = cur == 1 ? 2 : (cur == 2 ? 3 : 1);          // AGC: fast -> mid -> slow
    else next = (cur < 0 ? 0 : cur + 1) % f->states;
    cmd("rfunc", {{"sub", hex(f->sub)}, {"v", num(next)}});
}
void onAtt(lv_event_t*) {
    // 12 dB on the IC-7100, 20 dB on the IC-705; the radio rejects the value it does not have.
    bool is705 = g_app->radio.addr == 0xA4;
    cmd("ratt", {{"v", g_app->radio.att.load() > 0 ? "00" : (is705 ? "20" : "12")}});
}
void onLevel(lv_event_t* e) {
    LevelCtl* l = (LevelCtl*)lv_event_get_user_data(e);
    cmd("rlevel", {{"sub", hex(l->sub)}, {"v", num(lv_slider_get_value(l->slider))}});
    l->shown = -2;
}
// Direct frequency entry: a numeric keypad; the value is in MHz (14.250) or, without a point, kHz (7150).
void closeKeypad() { lv_obj_add_flag(kbBox, LV_OBJ_FLAG_HIDDEN); }
void onKeypad(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CANCEL) { closeKeypad(); return; }
    if (code != LV_EVENT_READY) return;
    std::string t = lv_textarea_get_text(kbText);
    double v = atof(t.c_str());
    long long hz = t.find('.') != std::string::npos ? (long long)(v * 1e6 + 0.5) : (long long)(v * 1e3 + 0.5);
    if (cmd("qsy", {{"hz", num(hz)}})) closeKeypad();
}
void onOpenKeypad(lv_event_t*) {
    lv_textarea_set_text(kbText, "");
    lv_obj_remove_flag(kbBox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(kbBox);
}

void buildRadioScreen() {
    const lv_font_t *f16 = &lv_font_montserrat_16, *f20 = &lv_font_montserrat_20, *f36 = &lv_font_montserrat_36;
    scrRadio = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scrRadio, C(BG), 0);
    lv_obj_set_style_bg_opa(scrRadio, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scrRadio, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* head = box(scrRadio, 0, 0, W, 72, PAN);
    button(head, 10, 12, 120, 48, LV_SYMBOL_LEFT " Back", onCloseRadio);
    rFreq = label(head, 150, 14, f36, AMB, "--.---.---");
    lv_obj_add_flag(rFreq, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(rFreq, onOpenKeypad, LV_EVENT_CLICKED, nullptr);
    rInfo = label(head, 410, 12, f20, TXT);
    rBarS = meter(head, 650, 20, 230);
    rMeter = label(head, 650, 38, f16, DIM);
    button(head, W - 124, 12, 114, 48, "A / B", onVfoSwap);

    // tuning
    int y = 82;
    lv_obj_t* lb;
    lv_obj_t* dn = button(scrRadio, 10, y, 120, 60, LV_SYMBOL_MINUS, onTuneDown, &lb);
    lv_obj_add_event_cb(dn, onTuneDown, LV_EVENT_LONG_PRESSED_REPEAT, nullptr);
    mxStep = matrix(scrRadio, 140, y, 612, 60, kStepMap, onStepSel);
    matrixCheck(mxStep, 6, stepIdx);
    lv_obj_t* up = button(scrRadio, 762, y, 120, 60, LV_SYMBOL_PLUS, onTuneUp, &lb);
    lv_obj_add_event_cb(up, onTuneUp, LV_EVENT_LONG_PRESSED_REPEAT, nullptr);
    button(scrRadio, 892, y, 122, 60, "Enter", onOpenKeypad);

    // bands
    y += 70;
    for (int i = 0; i < kNumRBands; i++) kRBandMap[i] = kRBands[i].name;
    kRBandMap[kNumRBands] = "";
    mxRBand = matrix(scrRadio, 10, y, W - 20, 54, kRBandMap, onRBand);

    // mode, data, filter
    y += 64;
    for (int i = 0; i < kNumRModes; i++) kRModeMap[i] = kRModes[i].name;
    kRModeMap[kNumRModes] = "";
    mxRMode = matrix(scrRadio, 10, y, 590, 54, kRModeMap, onRMode);
    bData = button(scrRadio, 610, y, 110, 54, "DATA", onData, &lData);
    mxFil = matrix(scrRadio, 730, y, W - 740, 54, kFilMap, onFil);

    // functions
    y += 64;
    const int nf = sizeof rFunc / sizeof rFunc[0], fw = (W - 20 - 7 * 6) / 8;
    bAtt = button(scrRadio, 10, y, fw, 54, "ATT", onAtt, &lAtt);
    for (int i = 0; i < nf; i++) {
        rFunc[i].btn = lv_button_create(scrRadio);
        lv_obj_set_pos(rFunc[i].btn, 10 + (i + 1) * (fw + 6), y);
        lv_obj_set_size(rFunc[i].btn, fw, 54);
        lv_obj_set_style_bg_color(rFunc[i].btn, C(PAN2), 0);
        lv_obj_set_style_radius(rFunc[i].btn, 8, 0);
        lv_obj_set_style_shadow_width(rFunc[i].btn, 0, 0);
        lv_obj_set_style_border_width(rFunc[i].btn, 1, 0);
        lv_obj_set_style_border_color(rFunc[i].btn, C(LINE), 0);
        lv_obj_add_event_cb(rFunc[i].btn, onFunc, LV_EVENT_CLICKED, &rFunc[i]);
        rFunc[i].lbl = lv_label_create(rFunc[i].btn);
        lv_obj_set_style_text_font(rFunc[i].lbl, f20, 0);
        lv_obj_set_style_text_color(rFunc[i].lbl, C(TXT), 0);
        lv_label_set_text(rFunc[i].lbl, rFunc[i].name);
        lv_obj_center(rFunc[i].lbl);
    }

    // levels: two rows of three sliders
    y += 74;
    const int nl = sizeof rLevel / sizeof rLevel[0], cw = (W - 20) / 3;
    for (int i = 0; i < nl; i++) {
        int x = 10 + (i % 3) * cw, yy = y + (i / 3) * 86;
        rLevel[i].lbl = label(scrRadio, x + 6, yy, f20, DIM, rLevel[i].name);
        lv_obj_t* sl = lv_slider_create(scrRadio);
        rLevel[i].slider = sl;
        lv_obj_set_pos(sl, x + 16, yy + 40);
        lv_obj_set_size(sl, cw - 44, 18);
        lv_slider_set_range(sl, 0, 100);
        lv_obj_set_style_bg_color(sl, C(PAN2), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(sl, C(RXC), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sl, C(TXT), LV_PART_KNOB);
        lv_obj_set_style_pad_all(sl, 8, LV_PART_KNOB);
        lv_obj_set_ext_click_area(sl, 16);
        lv_obj_add_event_cb(sl, onLevel, LV_EVENT_RELEASED, &rLevel[i]);
    }

    // keypad overlay
    kbBox = box(scrRadio, 0, 0, W, H, 0x000000);
    lv_obj_set_style_bg_opa(kbBox, LV_OPA_80, 0);
    lv_obj_add_flag(kbBox, LV_OBJ_FLAG_CLICKABLE);
    label(kbBox, 232, 40, f20, TXT, "Frequency in MHz (14.250), or kHz without a point (7150)");
    kbText = lv_textarea_create(kbBox);
    lv_obj_set_pos(kbText, 312, 80);
    lv_obj_set_size(kbText, 400, 64);
    lv_textarea_set_one_line(kbText, true);
    lv_textarea_set_max_length(kbText, 10);
    lv_textarea_set_accepted_chars(kbText, "0123456789.");
    lv_obj_set_style_text_font(kbText, f36, 0);
    lv_obj_t* kb = lv_keyboard_create(kbBox);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(kb, kbText);
    lv_obj_set_size(kb, 560, 380);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_28, 0);
    lv_obj_add_event_cb(kb, onKeypad, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(kb, onKeypad, LV_EVENT_CANCEL, nullptr);
    lv_obj_add_flag(kbBox, LV_OBJ_FLAG_HIDDEN);
}

void refreshRadioScreen(const App::UiState& s) {
    if (lv_screen_active() != scrRadio) return;
    Radio& r = g_app->radio;
    char b[96];
    if (s.freq) {
        snprintf(b, sizeof b, "%u.%03u.%03u", (unsigned)(s.freq / 1000000), (unsigned)(s.freq / 1000 % 1000), (unsigned)(s.freq % 1000));
        setText(rFreq, b);
    } else setText(rFreq, "--.---.---");
    int mi = -1;
    for (int i = 0; i < kNumRModes; i++) if (kRModes[i].code == s.rmode) mi = i;
    snprintf(b, sizeof b, "%s%s  %s\n%s", mi >= 0 ? kRModes[mi].name : (s.rmode == 7 ? "CW-R" : (s.rmode == 8 ? "RTTY-R" : "--")),
             s.data == 1 ? "-D" : "", s.band.c_str(), s.civ ? s.model.c_str() : "radio not connected");
    setText(rInfo, b);
    bool tx = s.txOn || s.ptt;
    lv_bar_set_value(rBarS, tx ? std::min(100, s.po * 100 / 213) : std::min(100, s.smeter * 100 / 241), LV_ANIM_OFF);
    setBg(rBarS, tx ? TXC : RXC, LV_PART_INDICATOR);
    if (tx) snprintf(b, sizeof b, "TX  Po %d%%  ALC %d%%  audio %d%%", std::min(100, s.po * 100 / 213), std::min(100, s.alc * 100 / 120), s.txLvl);
    else if (s.smeter <= 120) snprintf(b, sizeof b, "S%d", (s.smeter * 9 + 60) / 120);
    else snprintf(b, sizeof b, "S9+%d dB", (s.smeter - 120) / 2);
    setText(rMeter, b);

    int band = -1;
    for (int i = 0; i < kNumRBands; i++) if (s.freq >= kRBands[i].lo && s.freq <= kRBands[i].hi) band = i;
    if (band != shownRBand) { shownRBand = band; matrixCheck(mxRBand, kNumRBands, band); }
    if (mi != shownRMode) { shownRMode = mi; matrixCheck(mxRMode, kNumRModes, mi); }
    int fil = r.filter.load();
    if (fil != shownFil) { shownFil = fil; matrixCheck(mxFil, 3, fil - 1); }
    if (s.data != shownData) { shownData = s.data; setBg(bData, s.data == 1 ? AMB : PAN2); setTextColor(lData, s.data == 1 ? 0x1a1300 : TXT); }

    for (auto& f : rFunc) {
        int v = r.func[f.sub].load();
        if (v == f.shown) continue;
        f.shown = v;
        std::string t = f.name;
        if (f.sub == 0x02 && v > 0) t = "PRE " + num(v);
        if (f.sub == 0x12) t = v == 1 ? "AGC F" : (v == 2 ? "AGC M" : (v == 3 ? "AGC S" : "AGC"));
        setText(f.lbl, t);
        bool on = f.sub == 0x12 ? false : v > 0;
        setBg(f.btn, on ? AMB : PAN2);
        setTextColor(f.lbl, on ? 0x1a1300 : (v < 0 ? DIM : TXT));
    }
    int att = r.att.load();
    if (att != shownAtt) {
        shownAtt = att;
        setText(lAtt, att > 0 ? "ATT " + num(att) : std::string("ATT"));
        setBg(bAtt, att > 0 ? AMB : PAN2);
        setTextColor(lAtt, att > 0 ? 0x1a1300 : (att < 0 ? DIM : TXT));
    }
    for (auto& l : rLevel) {
        int v = r.level[l.sub].load();
        if (v == l.shown || lv_obj_has_state(l.slider, LV_STATE_PRESSED)) continue;
        l.shown = v;
        if (v >= 0) lv_slider_set_value(l.slider, v, LV_ANIM_OFF);
        setText(l.lbl, v >= 0 ? std::string(l.name) + "  " + num(v) + "%" : std::string(l.name) + "  --");
    }
}

// ---- logger screen: hand-entered SSB / CW contacts and CW macros ---------------------------------
// The keyboard is a custom layout (callsign characters, no shift), the macros go out through the
// radio's own keyer, and the contact is written with whatever frequency and mode the radio reports.
lv_obj_t *gInfo, *gSsb, *gCw, *gWpm, *gStatus, *gRecent, *gDupe, *gKb, *gMacroLbl[6];
lv_obj_t *gField[6];                                  // call, sent, rcvd, name, note, frequency
// Frequency and mode follow the radio until the operator types a frequency or taps SSB / CW; what
// they chose is kept until the contact is logged.
bool gFreqManual = false, gSettingFreq = false;
char gLogMode = 0;                                    // 0 = follow the radio, 'S' = SSB, 'C' = CW
int gFocus = 0, gShownLogN = -1, gShownRm = -2, gShownMode = -3;
std::string gShownMacros;

const char* kLogKeys[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
                          "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "/", "\n",
                          "A", "S", "D", "F", "G", "H", "J", "K", "L", "?", "-", "\n",
                          "Z", "X", "C", "V", "B", "N", "M", ",", ".", "Clear", "\n",
                          "Next", " ", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, "LOG", ""};
const lv_buttonmatrix_ctrl_t kLogKeyCtrl[] = {
    (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)2,
    (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1,
    (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1,
    (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)2,
    (lv_buttonmatrix_ctrl_t)2, (lv_buttonmatrix_ctrl_t)5, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)3};

std::string fieldText(int i) { return lv_textarea_get_text(gField[i]); }
bool voiceMode(int rm) { return rm != 3 && rm != 7 && rm != 4 && rm != 8; }
void loggerFocus(int i) {
    gFocus = i;
    lv_keyboard_set_textarea(gKb, gField[i]);
    for (int k = 0; k < 6; k++) lv_obj_set_style_border_color(gField[k], C(k == i ? AMB : LINE), 0);
}
void loggerDupe() {
    std::string c = fieldText(0);
    bool dupe = c.size() >= 3 && g_app->uiWorked(c);
    setText(gDupe, dupe ? "WORKED BEFORE on this band" : "");
}
void loggerLog() {
    bool radioFreq = !gFreqManual && g_app->radio.connected();
    if (!cmd("logqso", {{"call", fieldText(0)}, {"mode", gLogMode == 'S' ? "SSB" : gLogMode == 'C' ? "CW" : "AUTO"},
                        {"rs", fieldText(1)}, {"rr", fieldText(2)}, {"name", fieldText(3)}, {"comment", fieldText(4)},
                        {"freq", radioFreq ? "" : fieldText(5)}})) return;
    for (int i = 0; i < 5; i++) lv_textarea_set_text(gField[i], "");
    gFreqManual = false;                              // back to following the radio
    gLogMode = 0;
    gShownMode = -3;
    loggerFocus(0);
    loggerDupe();
}
void onLoggerField(lv_event_t* e) { loggerFocus((int)(intptr_t)lv_event_get_user_data(e)); }
void onLoggerCall(lv_event_t*) { loggerDupe(); }
void onLoggerFreq(lv_event_t*) { if (!gSettingFreq) gFreqManual = true; }
void onLoggerKey(lv_event_t* e) {
    lv_obj_t* kb = (lv_obj_t*)lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char* t = lv_buttonmatrix_get_button_text(kb, id);
    if (!t) return;
    if (!strcmp(t, "Next")) loggerFocus(gFocus >= 4 ? 0 : gFocus + 1);
    else if (!strcmp(t, "LOG")) loggerLog();
    else if (!strcmp(t, "Clear")) { lv_textarea_set_text(gField[gFocus], ""); if (gFocus == 0) loggerDupe(); }
    else lv_keyboard_def_event_cb(e);                 // characters, backspace, cursor keys
}
void onLoggerLog(lv_event_t*) { loggerLog(); }
void onMacro(lv_event_t* e) {
    int n = (int)(intptr_t)lv_event_get_user_data(e);
    std::string rs = fieldText(1);
    cmd("macro", {{"n", num(n)}, {"call", fieldText(0)}, {"rs", rs.empty() ? "599" : rs}});
}
void onCwStop(lv_event_t*) { cmd("cwstop"); }
// SSB / CW choose the mode that is logged, whatever the radio says, and switch the radio too when it
// is connected (the CW macros need it in CW).
void onLoggerSsb(lv_event_t*) {
    gLogMode = 'S';
    gShownMode = -3;
    if (!g_app->radio.connected()) return;
    uint64_t f = gFreqManual ? parseDialFreq(fieldText(5)) : g_app->radio.freq.load();
    if (!f) f = g_app->radio.freq.load();
    bool lsb = f < 10000000ULL && !(f >= 5000000ULL && f < 5600000ULL);      // 60 m is upper sideband
    cmd("rmode", {{"m", lsb ? "00" : "01"}, {"fil", "1"}});
}
void onLoggerCw(lv_event_t*) {
    gLogMode = 'C';
    gShownMode = -3;
    if (g_app->radio.connected()) cmd("rmode", {{"m", "03"}, {"fil", "1"}});
}
void onWpm(lv_event_t* e) {
    int w = g_app->radio.keySpeed.load();
    if (w < 6) w = 20;
    cmd("cwspeed", {{"wpm", num(w + (int)(intptr_t)lv_event_get_user_data(e))}});
}
void onOpenLogger(lv_event_t*) {
    cmd("txen", {{"v", "0"}});                        // no automatic transmissions while logging by hand
    cmd("rext", {{"v", "1"}});
    gShownLogN = -1; gShownRm = -2;
    loggerFocus(0);
    lv_screen_load(scrLogger);
}
void onCloseLogger(lv_event_t*) {
    cmd("rext", {{"v", "0"}});
    lv_screen_load(scrMain);
}
void onLoggerRadio(lv_event_t*) { radioReturn = scrLogger; lv_screen_load(scrRadio); }

void buildLoggerScreen() {
    const lv_font_t *f16 = &lv_font_montserrat_16, *f20 = &lv_font_montserrat_20, *f28 = &lv_font_montserrat_28;
    scrLogger = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scrLogger, C(BG), 0);
    lv_obj_set_style_bg_opa(scrLogger, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scrLogger, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* head = box(scrLogger, 0, 0, W, 64, PAN);
    button(head, 10, 8, 104, 48, LV_SYMBOL_LEFT " Back", onCloseLogger);
    {
        lv_obj_t* ta = lv_textarea_create(head);           // frequency, MHz: follows the radio until typed in
        lv_obj_set_pos(ta, 122, 6);
        lv_obj_set_size(ta, 200, 52);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_max_length(ta, 12);
        lv_textarea_set_accepted_chars(ta, "0123456789.");
        lv_textarea_set_placeholder_text(ta, "MHz");
        lv_obj_set_style_text_font(ta, f28, 0);
        lv_obj_set_style_pad_ver(ta, 6, 0);
        lv_obj_set_style_bg_color(ta, C(0x0b1014), 0);
        lv_obj_set_style_text_color(ta, C(AMB), 0);
        lv_obj_set_style_border_color(ta, C(LINE), 0);
        lv_obj_set_style_border_width(ta, 2, 0);
        lv_obj_set_style_radius(ta, 8, 0);
        lv_obj_add_event_cb(ta, onLoggerField, LV_EVENT_CLICKED, (void*)(intptr_t)5);
        lv_obj_add_event_cb(ta, onLoggerFreq, LV_EVENT_VALUE_CHANGED, nullptr);
        gField[5] = ta;
    }
    gSsb = button(head, 330, 8, 92, 48, "SSB", onLoggerSsb);
    gCw = button(head, 428, 8, 92, 48, "CW", onLoggerCw);
    lv_obj_t* b = button(head, 540, 8, 56, 48, LV_SYMBOL_MINUS, onWpm);
    lv_obj_remove_event_cb(b, onWpm);
    lv_obj_add_event_cb(b, onWpm, LV_EVENT_CLICKED, (void*)(intptr_t)-1);
    gWpm = label(head, 604, 20, f20, TXT, "-- wpm");
    lv_obj_set_width(gWpm, 92);
    lv_obj_set_style_text_align(gWpm, LV_TEXT_ALIGN_CENTER, 0);
    b = button(head, 702, 8, 56, 48, LV_SYMBOL_PLUS, onWpm);
    lv_obj_remove_event_cb(b, onWpm);
    lv_obj_add_event_cb(b, onWpm, LV_EVENT_CLICKED, (void*)(intptr_t)1);
    button(head, 776, 8, 110, 48, "Stop CW", onCwStop, nullptr, 0x7a2020);
    button(head, 896, 8, 118, 48, "Radio " LV_SYMBOL_RIGHT, onLoggerRadio);

    // entry fields
    static const struct { const char* hint; int x, w, max; const char* chars; } fd[5] = {
        {"Callsign", 10, 250, 11, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/"},
        {"Sent", 268, 104, 6, nullptr}, {"Rcvd", 380, 104, 6, nullptr},
        {"Name", 492, 180, 30, nullptr}, {"Note", 680, 200, 60, nullptr}};
    for (int i = 0; i < 5; i++) {
        lv_obj_t* ta = lv_textarea_create(scrLogger);
        lv_obj_set_pos(ta, fd[i].x, 72);
        lv_obj_set_size(ta, fd[i].w, 58);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_max_length(ta, (uint32_t)fd[i].max);
        if (fd[i].chars) lv_textarea_set_accepted_chars(ta, fd[i].chars);
        lv_textarea_set_placeholder_text(ta, fd[i].hint);
        lv_obj_set_style_text_font(ta, i == 0 ? f28 : f20, 0);
        lv_obj_set_style_pad_ver(ta, i == 0 ? 8 : 14, 0);
        lv_obj_set_style_bg_color(ta, C(0x0b1014), 0);
        lv_obj_set_style_text_color(ta, C(TXT), 0);
        lv_obj_set_style_border_color(ta, C(LINE), 0);
        lv_obj_set_style_border_width(ta, 2, 0);
        lv_obj_set_style_radius(ta, 8, 0);
        lv_obj_add_event_cb(ta, onLoggerField, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        gField[i] = ta;
    }
    lv_obj_add_event_cb(gField[0], onLoggerCall, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_t* lLog;
    button(scrLogger, 890, 72, 124, 58, "LOG", onLoggerLog, &lLog, 0x1f6b45);
    lv_obj_set_style_text_font(lLog, f28, 0);

    // CW macros
    const int mw = (W - 20 - 5 * 6) / 6;
    for (int i = 0; i < 6; i++) {
        lv_obj_t* mb = button(scrLogger, 10 + i * (mw + 6), 138, mw, 52, "", onMacro, &gMacroLbl[i]);
        lv_obj_remove_event_cb(mb, onMacro);
        lv_obj_add_event_cb(mb, onMacro, LV_EVENT_CLICKED, (void*)(intptr_t)(i + 1));
        lv_label_set_long_mode(gMacroLbl[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(gMacroLbl[i], mw - 12);
        lv_obj_set_style_text_align(gMacroLbl[i], LV_TEXT_ALIGN_CENTER, 0);
    }

    // messages and the last contacts
    gInfo = label(scrLogger, 14, 200, f16, DIM);
    lv_obj_set_width(gInfo, 330);
    lv_label_set_long_mode(gInfo, LV_LABEL_LONG_MODE_DOTS);
    gDupe = label(scrLogger, 350, 198, f20, TXC);
    gStatus = label(scrLogger, 0, 198, f16, DIM);
    lv_obj_set_width(gStatus, 360);
    lv_label_set_long_mode(gStatus, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(gStatus, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(gStatus, W - 374, 200);
    gRecent = label(scrLogger, 14, 226, f20, TXT);
    lv_obj_set_style_text_line_space(gRecent, 4, 0);

    // keyboard
    gKb = lv_keyboard_create(scrLogger);
    lv_keyboard_set_map(gKb, LV_KEYBOARD_MODE_USER_1, kLogKeys, kLogKeyCtrl);
    lv_keyboard_set_mode(gKb, LV_KEYBOARD_MODE_USER_1);
    lv_obj_set_size(gKb, W, 290);
    lv_obj_align(gKb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(gKb, &lv_font_montserrat_20, 0);
    lv_obj_set_style_bg_color(gKb, C(PAN), 0);
    lv_obj_set_style_bg_color(gKb, C(PAN2), LV_PART_ITEMS);
    lv_obj_set_style_text_color(gKb, C(TXT), LV_PART_ITEMS);
    lv_obj_remove_event_cb(gKb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(gKb, onLoggerKey, LV_EVENT_VALUE_CHANGED, nullptr);
    loggerFocus(0);
}

void refreshLogger(const App::UiState& s) {
    if (lv_screen_active() != scrLogger) return;
    char b[96];
    if (!gFreqManual) {                               // follow the radio
        if (s.civ && s.freq) snprintf(b, sizeof b, "%u.%06u", (unsigned)(s.freq / 1000000), (unsigned)(s.freq % 1000000));
        else b[0] = 0;
        if (strcmp(lv_textarea_get_text(gField[5]), b)) {
            gSettingFreq = true;
            lv_textarea_set_text(gField[5], b);
            gSettingFreq = false;
        }
    }
    lv_obj_set_style_text_color(gField[5], C(gFreqManual ? TXT : AMB), 0);   // white = typed in, amber = from the radio
    static const struct { int code; const char* name; } mn[] = {{0, "LSB"}, {1, "USB"}, {2, "AM"}, {3, "CW"}, {4, "RTTY"}, {5, "FM"}, {7, "CW-R"}, {8, "RTTY-R"}, {0x17, "DV"}};
    const char* m = "--";
    for (auto& x : mn) if (x.code == s.rmode) m = x.name;
    snprintf(b, sizeof b, "Radio: %s%s  %s  %s", m, s.data == 1 && (s.rmode == 0 || s.rmode == 1 || s.rmode == 2 || s.rmode == 5) ? "-D" : "", s.band.c_str(), s.civ ? s.model.c_str() : "radio not connected");
    setText(gInfo, b);
    if (s.rmode != gShownRm || gShownMode != gLogMode) {
        gShownRm = s.rmode;
        gShownMode = gLogMode;
        bool cw = gLogMode ? gLogMode == 'C' : s.rmode == 3 || s.rmode == 7;
        bool ssb = gLogMode ? gLogMode == 'S' : (s.rmode == 0 || s.rmode == 1) && s.data != 1;
        setBg(gCw, cw ? AMB : PAN2);
        setBg(gSsb, ssb ? AMB : PAN2);
        setTextColor(lv_obj_get_child(gCw, 0), cw ? 0x1a1300 : TXT);
        setTextColor(lv_obj_get_child(gSsb, 0), ssb ? 0x1a1300 : TXT);
        const char* dflt = (gLogMode ? gLogMode == 'S' : voiceMode(s.rmode)) ? "59" : "599";      // what is logged when the field is left empty
        lv_textarea_set_placeholder_text(gField[1], dflt);
        lv_textarea_set_placeholder_text(gField[2], dflt);
    }
    if (s.wpm > 0) snprintf(b, sizeof b, "%d wpm", s.wpm); else snprintf(b, sizeof b, "-- wpm");
    setText(gWpm, b);
    std::string macros;
    for (int i = 0; i < 6; i++) macros += g_app->cfg.macro[i].substr(0, g_app->cfg.macro[i].find('|')) + "\n";
    if (macros != gShownMacros) {
        gShownMacros = macros;
        size_t p = 0;
        for (int i = 0; i < 6; i++) { size_t e = macros.find('\n', p); setText(gMacroLbl[i], macros.substr(p, e - p)); p = e + 1; }
    }
    bool noteOn = !noteText.empty() && (int32_t)(noteUntil - lv_tick_get()) > 0;
    setText(gStatus, noteOn ? noteText : s.status);
    setTextColor(gStatus, noteOn ? AMB : DIM);
    if (s.logN != gShownLogN) {
        gShownLogN = s.logN;
        std::vector<App::UiLogEntry> e;
        g_app->uiLogTail(2, e);
        std::string t;
        for (auto& q : e) t += q.when.substr(q.when.size() > 5 ? 6 : 0) + "  " + q.call + "  " + q.band + " " + q.mode + "  " + q.sent + " / " + q.rcvd + "\n";
        snprintf(b, sizeof b, "%d in the current log", s.logN);
        setText(gRecent, t + b);
    }
}

// ---- settings screen: everything on the web page's Settings tab ----------------------------------
// One tab per group down the left side; a full keyboard rises from the bottom while a box is being
// typed in. Values are read from the live settings when the screen opens and written with the same
// "set" command the web page uses, so the same checks apply.
enum SKind { SK_TEXT, SK_UPPER, SK_NUM, SK_HEX, SK_PASS, SK_DROP, SK_MACRO };
struct SField { const char* key; const char* label; int tab; SKind kind; int maxLen; const char* opts; const char* vals; const char* hint; };
const char* kSetTabs[] = {"Station", "Operating", "Radio", "CW", "Logging", "Wi-Fi", "Clock", "Device"};
constexpr int kSetTabN = sizeof kSetTabs / sizeof kSetTabs[0];
const SField kSet[] = {
    {"rigName", "RigDeck name", 0, SK_TEXT, 24, nullptr, nullptr, "optional, e.g. Shack or Mobile"},
    {"call", "Callsign", 0, SK_UPPER, 11, nullptr, nullptr, ""},
    {"grid", "Grid locator", 0, SK_UPPER, 6, nullptr, nullptr, ""},
    {"op", "Operator (ADIF OPERATOR)", 0, SK_UPPER, 11, nullptr, nullptr, "optional"},
    {"mySigInfo", "Reference (MY_SIG_INFO)", 0, SK_UPPER, 40, nullptr, nullptr, "e.g. US-1234, empty = none"},
    {"mySig", "Programme (MY_SIG)", 0, SK_UPPER, 12, nullptr, nullptr, "optional, e.g. WWFF"},
    {"cqTag", "Call CQ as", 0, SK_DROP, 0, "CQ\nCQ DX\nCQ POTA\nCQ SOTA\nCQ WWFF\nCQ NA\nCQ EU\nCQ TEST", "|DX|POTA|SOTA|WWFF|NA|EU|TEST", nullptr},
    {"txLevel", "TX audio level % (maximum)", 1, SK_NUM, 3, nullptr, nullptr, "1 - 100"},
    {"autoAlc", "Lower audio when ALC shows", 1, SK_DROP, 0, "On (recommended)\nOff", "1|0", nullptr},
    {"rfPower", "RF power %", 1, SK_NUM, 3, nullptr, nullptr, "-1 = leave the radio's"},
    {"useRR73", "Sign off with", 1, SK_DROP, 0, "RR73\nRRR, then 73", "1|0", nullptr},
    {"maxRetries", "Repeats before giving up", 1, SK_NUM, 2, nullptr, nullptr, ""},
    {"watchdogMin", "TX watchdog, minutes", 1, SK_NUM, 3, nullptr, nullptr, ""},
    {"cwWpm", "FT-CW speed, wpm", 1, SK_NUM, 2, nullptr, nullptr, "20 - 50, same at both stations"},
    {"confirmQso", "POTA mode: confirm each QSO", 1, SK_DROP, 0, "Off\nOn: press Next QSO after each", "0|1", nullptr},
    {"dnw", "Do-not-work list", 1, SK_TEXT, 600, nullptr, nullptr, "e.g. W1ABC, XX9* (more on the web page)"},
    {"prioOn", "Answer priority", 1, SK_DROP, 0, "Off: strongest first\nOn: order set on the web page", "0|1", nullptr},
    {"radioType", "Radio", 2, SK_DROP, 0, "Find automatically\nIcom (CI-V)\nYaesu (FTX-1, FTDX10, FT-991A...)\nQRP Labs QMX / QMX+\nKenwood / Elecraft (TS-2000, K2, KX2...)\nNo CAT (SignaLink / sound card)", "0|1|2|3|4|5", nullptr},
    {"pttMethod", "PTT", 2, SK_DROP, 0, "CAT command\nSerial RTS (Digirig)\nSerial DTR\nVOX (sound card keys it)", "0|1|2|3", nullptr},
    {"catBaud", "CAT rate tried first", 2, SK_DROP, 0, "4800\n9600\n19200\n38400\n115200", "4800|9600|19200|38400|115200", nullptr},
    {"civAddr", "CI-V address (hex)", 2, SK_HEX, 4, nullptr, nullptr, "88 7100 A4 705 94 7300 A2 9700"},
    {"civBaud", "CI-V baud", 2, SK_DROP, 0, "4800\n9600\n19200\n38400\n57600\n115200", "4800|9600|19200|38400|57600|115200", nullptr},
    {"keyWpm", "Keyer speed for logger, wpm", 3, SK_NUM, 2, nullptr, nullptr, "0 = leave the radio's"},
    {"macro1", "Macro 1", 3, SK_MACRO, 80, nullptr, nullptr, "text to send"},
    {"macro2", "Macro 2", 3, SK_MACRO, 80, nullptr, nullptr, "text to send"},
    {"macro3", "Macro 3", 3, SK_MACRO, 80, nullptr, nullptr, "text to send"},
    {"macro4", "Macro 4", 3, SK_MACRO, 80, nullptr, nullptr, "text to send"},
    {"macro5", "Macro 5", 3, SK_MACRO, 80, nullptr, nullptr, "text to send"},
    {"macro6", "Macro 6", 3, SK_MACRO, 80, nullptr, nullptr, "text to send"},
    {"logProto", "Send log to", 4, SK_DROP, 0, "Nowhere (file only)\nTCP, ADIF text\nUDP, ADIF text\nUDP, WSJT-X format", "0|1|2|3", nullptr},
    {"logHost", "Logger address", 4, SK_TEXT, 40, nullptr, nullptr, "192.168.4.2"},
    {"logPort", "Logger port", 4, SK_NUM, 5, nullptr, nullptr, "2237"},
    {"apSsid", "Hotspot name", 5, SK_TEXT, 32, nullptr, nullptr, ""},
    {"apPass", "Hotspot password", 5, SK_PASS, 63, nullptr, nullptr, "unchanged"},
    {"staSsid", "Home network name", 5, SK_TEXT, 32, nullptr, nullptr, "optional, 2.4 GHz"},
    {"staPass", "Home network password", 5, SK_PASS, 63, nullptr, nullptr, "unchanged"},
    {"deadman", "Dead man's switch", 5, SK_DROP, 0, "Off\nOn: no internet = stop auto", "0|1", nullptr},
    {"deadmanHost", "Internet check address", 5, SK_TEXT, 60, nullptr, nullptr, "1.1.1.1:53"},
    {"gpsClock", "Clock from the radio's GPS", 6, SK_DROP, 0, "Off\nOn (Icom: GPS Out to USB)", "0|1", nullptr},
    {"gpsBaud", "Radio's GPS Out baud", 6, SK_DROP, 0, "4800\n9600", "4800|9600", nullptr},
    {"gpsDelayMs", "GPS sentence delay, ms", 6, SK_NUM, 4, nullptr, nullptr, "used without PPS"},
    {"gpsModule", "GPS module on the header", 6, SK_DROP, 0, "Off\nOn", "0|1", nullptr},
    {"gpsModBaud", "Module baud", 6, SK_DROP, 0, "4800\n9600\n38400\n115200", "4800|9600|38400|115200", nullptr},
    {"gpsRxPin", "Module TX to board GPIO", 6, SK_NUM, 2, nullptr, nullptr, "3"},
    {"gpsPpsPin", "Module PPS to board GPIO", 6, SK_NUM, 2, nullptr, nullptr, "4, or -1 if not wired"},
};
constexpr int kSetN = sizeof kSet / sizeof kSet[0];
lv_obj_t *scrSettings, *sTabs, *sKb, *sStatus, *sWifi, *sWidget[kSetN], *sMacroLbl[kSetN];
lv_obj_t *sBright, *sBrightL, *sContrast, *sContrastL, *sStor, *sConsole;
std::map<std::string, std::string> sLoaded;
const int SET_HEAD = 64, SET_KB = 270;

std::string dropValue(const SField& f, uint32_t idx) {
    std::string v = f.vals;
    size_t p = 0;
    for (uint32_t i = 0; i < idx && p != std::string::npos; i++) { p = v.find('|', p); if (p != std::string::npos) p++; }
    if (p == std::string::npos) return "";
    return v.substr(p, v.find('|', p) - p);
}
void settingsKbHide() {
    lv_obj_add_flag(sKb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(sTabs, H - SET_HEAD);
    lv_obj_t* ta = lv_keyboard_get_textarea(sKb);
    if (ta) lv_obj_remove_state(ta, LV_STATE_FOCUSED);
    lv_keyboard_set_textarea(sKb, nullptr);
}
// Callsign / grid keyboard: digits and capitals on one page, with / and - (portable and suffix calls);
// "1#" still reaches every other character.
static const char* kCallKeys[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
                                  "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "/", "\n",
                                  "A", "S", "D", "F", "G", "H", "J", "K", "L", "-", "\n",
                                  "Z", "X", "C", "V", "B", "N", "M", "1#", LV_SYMBOL_OK, "\n",
                                  LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, ""};
#define KC_K(w) (lv_buttonmatrix_ctrl_t)(w)
#define KC_C(w) (lv_buttonmatrix_ctrl_t)((w) | LV_BUTTONMATRIX_CTRL_CHECKED)
static const lv_buttonmatrix_ctrl_t kCallCtrl[] = {
    KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_C(3),
    KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2),
    KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2),
    KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_K(2), KC_C(3), KC_C(4),
    KC_C(2), KC_C(2), KC_K(8), KC_C(2)};
void onSettingsTa(lv_event_t* e) {
    lv_obj_t* ta = (lv_obj_t*)lv_event_get_target(e);
    const SField& f = kSet[(int)(intptr_t)lv_event_get_user_data(e)];
    lv_keyboard_set_textarea(sKb, ta);
    lv_keyboard_set_mode(sKb, f.kind == SK_NUM ? LV_KEYBOARD_MODE_NUMBER
                              : f.kind == SK_UPPER ? LV_KEYBOARD_MODE_USER_2
                              : f.kind == SK_HEX || f.kind == SK_MACRO ? LV_KEYBOARD_MODE_TEXT_UPPER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_remove_flag(sKb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(sTabs, H - SET_HEAD - SET_KB);
    lv_obj_update_layout(sTabs);
    lv_obj_scroll_to_view_recursive(ta, LV_ANIM_OFF);
}
void onSettingsKbDone(lv_event_t*) { settingsKbHide(); }
void onSettingsTab(lv_event_t*) { settingsKbHide(); }

void settingsLoad() {
    sLoaded.clear();
    std::string t = g_app->cfg.serialize(false);
    for (size_t p = 0; p < t.size();) {
        size_t e = t.find('\n', p), q = t.find('=', p);
        if (e == std::string::npos) e = t.size();
        if (q != std::string::npos && q < e) sLoaded[t.substr(p, q - p)] = t.substr(q + 1, e - q - 1);
        p = e + 1;
    }
    for (int i = 0; i < kSetN; i++) {
        const SField& f = kSet[i];
        std::string v = sLoaded[f.key];
        if (f.kind == SK_DROP) {
            uint32_t idx = 0;
            for (uint32_t k = 0; k < 8; k++) if (dropValue(f, k) == v) { idx = k; break; }
            lv_dropdown_set_selected(sWidget[i], idx);
        } else if (f.kind == SK_PASS) lv_textarea_set_text(sWidget[i], "");
        else if (f.kind == SK_HEX) { char b[8]; snprintf(b, sizeof b, "%02X", atoi(v.c_str())); lv_textarea_set_text(sWidget[i], b); }
        else if (f.kind == SK_MACRO) {                                // "Label|text" in two boxes
            size_t bar = v.find('|');
            lv_textarea_set_text(sMacroLbl[i], bar == std::string::npos ? "" : v.substr(0, bar).c_str());
            lv_textarea_set_text(sWidget[i], bar == std::string::npos ? v.c_str() : v.substr(bar + 1).c_str());
        }
        else lv_textarea_set_text(sWidget[i], v.c_str());
    }
}
void onSettingsSave(lv_event_t*) {
    settingsKbHide();
    std::map<std::string, std::string> a;
    bool wifi = false;
    for (int i = 0; i < kSetN; i++) {
        const SField& f = kSet[i];
        std::string v;
        if (f.kind == SK_DROP) v = dropValue(f, lv_dropdown_get_selected(sWidget[i]));
        else v = lv_textarea_get_text(sWidget[i]);
        if (f.kind == SK_PASS && v.empty()) continue;                 // empty = keep the old password
        if (f.kind == SK_HEX && v.compare(0, 2, "0x") && v.compare(0, 2, "0X")) v = "0x" + v;
        if (f.kind == SK_MACRO) {
            std::string lb = lv_textarea_get_text(sMacroLbl[i]);
            if (lb.empty()) lb = std::string("M") + f.key[5];
            v = lb + "|" + v;
        }
        if (!strcmp(f.key, "apPass") || (!strcmp(f.key, "apSsid") && v != sLoaded[f.key])) wifi = true;   // hotspot changes need a restart
        a[f.key] = v;
    }
    if (cmd("set", a)) {
        note(wifi ? "Saved. Restart to apply the hotspot changes." : "Settings saved");
        settingsLoad();
    }
}
void onWifiConnect(lv_event_t* e) {
    onSettingsSave(e);                                // a changed name/password is saved (and joined) first
    cmd("wifi", {{"do", "connect"}});
}
void onWifiDisconnect(lv_event_t*) { cmd("wifi", {{"do", "disconnect"}}); }
std::string staText(const App::UiState& s) {
    if (s.staState == "off" || s.staState.empty()) return "Home network: not set up (enter its name above, then Connect)";
    if (s.staState == "connected") {
        const char* q = s.staRssi >= -60 ? "strong" : s.staRssi >= -70 ? "good" : s.staRssi >= -80 ? "fair" : "weak";
        char b[64];
        snprintf(b, sizeof b, ", signal %d dBm (%s)", s.staRssi, q);
        return "Home network: connected, address " + s.staIp + (s.staRssi ? b : "");
    }
    std::string t = "Home network: " + s.staState;
    if (!s.staReason.empty()) t += " - " + s.staReason;
    return t;
}
void onSettingsRestart(lv_event_t*) {
    confirm("Restart", "Restart the controller now?\nAny transmission is stopped first.", "Restart", "reboot");
}
// ---- Device tab: brightness, contrast, storage ----
std::string sizeText(uint64_t b) {
    char t[32];
    if (b >= 1000000000ULL) snprintf(t, sizeof t, "%.1f GB", b / 1e9);
    else if (b >= 1000000ULL) snprintf(t, sizeof t, "%.1f MB", b / 1e6);
    else snprintf(t, sizeof t, "%llu kB", (unsigned long long)(b / 1000));
    return t;
}
std::string storText(const App::UiState& s) {
    std::string t = "Log of station " + s.station + ": " + (s.logSd ? "on the microSD card" : "in the board's own memory") + "\n";
    if (s.intTotal) t += "Board memory: " + sizeText(s.intTotal > s.intUsed ? s.intTotal - s.intUsed : 0) + " free of " + sizeText(s.intTotal) + "\n";
    if (s.sdIn && s.sdTotal) t += "microSD card: " + sizeText(s.sdTotal > s.sdUsed ? s.sdTotal - s.sdUsed : 0) + " free of " + sizeText(s.sdTotal) + "\n";
    else t += "microSD card: " + (s.sdInfo.empty() ? std::string("none") : s.sdInfo) + "\n";
    if (s.left >= 0) t += "Room for about " + std::to_string(s.left) + " more contacts";
    return t;
}
void onBright(lv_event_t* e) {
    int v = lv_slider_get_value(sBright);
    setText(sBrightL, std::to_string(v) + " %");
    if (ui_brightness_fn) ui_brightness_fn(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) cmd("set", {{"bright", std::to_string(v)}});
}
void onContrast(lv_event_t* e) {
    int v = lv_slider_get_value(sContrast);
    setText(sContrastL, std::to_string(v) + " %");
    if (lv_event_get_code(e) == LV_EVENT_RELEASED && cmd("set", {{"contrast", std::to_string(v)}}) && v != gContrast)
        confirm("Contrast", "The new contrast applies after a restart.\nRestart now?", "Restart", "reboot");
}
void onConsoleLog(lv_event_t*) {
    bool on = lv_obj_has_state(sConsole, LV_STATE_CHECKED);
    cmd("set", {{"consoleLog", on ? "1" : "0"}});
}
void onSdFormat(lv_event_t*) {
    confirm("Format microSD card", "Erase EVERYTHING on the microSD card and format it (FAT32, whole card)? This station's "
            "log is kept: it waits in the board's memory and goes back on the card. Other stations' logs and kept "
            "deleted/cleared logs on the card are lost.", "Format", "sdfmt1");
}
void onNuke(lv_event_t*) {
    confirm("Erase all contact logs", "Erase EVERY contact log on this RigDeck: current logs, archives, deleted contacts and cleared logs of all "
            "station callsigns, on the card and in the board's memory? Download anything you want to keep first.", "Erase", "nuke1");
}

void onOpenSettings(lv_event_t*) {
    cmd("ping");
    settingsLoad();
    lv_slider_set_value(sBright, g_app->cfg.bright, LV_ANIM_OFF);
    setText(sBrightL, std::to_string(g_app->cfg.bright) + " %");
    lv_slider_set_value(sContrast, g_app->cfg.contrast, LV_ANIM_OFF);
    if (g_app->cfg.consoleLog) lv_obj_add_state(sConsole, LV_STATE_CHECKED); else lv_obj_remove_state(sConsole, LV_STATE_CHECKED);
    setText(sContrastL, std::to_string(g_app->cfg.contrast) + " %");
    settingsKbHide();
    lv_screen_load(scrSettings);
}
void onCloseSettings(lv_event_t*) {
    settingsKbHide();
    lv_screen_load(scrMain);
}

void buildSettingsScreen() {
    const lv_font_t *f16 = &lv_font_montserrat_16, *f20 = &lv_font_montserrat_20, *f28 = &lv_font_montserrat_28;
    scrSettings = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scrSettings, C(BG), 0);
    lv_obj_set_style_bg_opa(scrSettings, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scrSettings, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* head = box(scrSettings, 0, 0, W, SET_HEAD, PAN);
    button(head, 10, 8, 104, 48, LV_SYMBOL_LEFT " Back", onCloseSettings);
    label(head, 130, 4, f28, TXT, "Settings");
    label(head, 132, 38, &lv_font_montserrat_16, DIM, ("RigDeck " + g_app->fwVersion).c_str());
    sStatus = label(head, 270, 22, f16, DIM);
    lv_obj_set_width(sStatus, 440);
    lv_label_set_long_mode(sStatus, LV_LABEL_LONG_MODE_DOTS);
    button(head, 724, 8, 140, 48, "Restart", onSettingsRestart);
    lv_obj_t* lSave;
    button(head, 874, 8, 140, 48, LV_SYMBOL_SAVE " Save", onSettingsSave, &lSave, 0x1f6b45);

    sTabs = lv_tabview_create(scrSettings);
    lv_obj_set_pos(sTabs, 0, SET_HEAD);
    lv_obj_set_size(sTabs, W, H - SET_HEAD);
    lv_tabview_set_tab_bar_position(sTabs, LV_DIR_LEFT);
    lv_tabview_set_tab_bar_size(sTabs, 170);
    lv_obj_set_style_bg_color(sTabs, C(BG), 0);
    lv_obj_t* bar = lv_tabview_get_tab_bar(sTabs);
    lv_obj_set_style_bg_color(bar, C(PAN), 0);
    lv_obj_t* content = lv_tabview_get_content(sTabs);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);              // change tabs by tapping only
    lv_obj_t* pages[kSetTabN];
    for (int t = 0; t < kSetTabN; t++) {
        pages[t] = lv_tabview_add_tab(sTabs, kSetTabs[t]);
        lv_obj_set_style_bg_color(pages[t], C(BG), 0);
        lv_obj_set_style_bg_opa(pages[t], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(pages[t], 16, 0);
        lv_obj_t* tb = lv_obj_get_child(bar, t);
        lv_obj_set_style_text_font(tb, f20, 0);
        lv_obj_set_style_text_color(tb, C(TXT), 0);
        lv_obj_set_style_text_color(tb, C(0x1a1300), LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(tb, C(AMB), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_border_width(tb, 0, LV_STATE_CHECKED);
        lv_obj_add_event_cb(tb, onSettingsTab, LV_EVENT_CLICKED, nullptr);
    }
    int row[kSetTabN] = {};
    for (int i = 0; i < kSetN; i++) {
        const SField& f = kSet[i];
        lv_obj_t* pg = pages[f.tab];
        int y = row[f.tab]++ * 62;
        lv_obj_t* l = label(pg, 0, y + 14, f20, DIM, f.label);
        lv_obj_set_width(l, 320);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        if (f.kind == SK_DROP) {
            lv_obj_t* dd = lv_dropdown_create(pg);
            lv_obj_set_pos(dd, 330, y);
            lv_obj_set_size(dd, 470, 52);
            lv_dropdown_set_options(dd, f.opts);
            lv_obj_set_style_text_font(dd, f20, 0);
            lv_obj_set_style_bg_color(dd, C(0x0b1014), 0);
            lv_obj_set_style_text_color(dd, C(TXT), 0);
            lv_obj_set_style_border_color(dd, C(LINE), 0);
            lv_obj_t* list = lv_dropdown_get_list(dd);
            lv_obj_set_style_text_font(list, f20, 0);
            lv_obj_set_style_bg_color(list, C(PAN2), 0);
            lv_obj_set_style_text_color(list, C(TXT), 0);
            lv_obj_set_style_bg_color(list, C(AMB), LV_PART_SELECTED | LV_STATE_CHECKED);
            lv_obj_set_style_text_color(list, C(0x1a1300), LV_PART_SELECTED | LV_STATE_CHECKED);
            lv_obj_add_event_cb(dd, onSettingsTab, LV_EVENT_CLICKED, nullptr);
            sWidget[i] = dd;
        } else {
            int x = 330, w = 470;
            if (f.kind == SK_MACRO) {                                 // label box, then the text
                lv_obj_set_width(l, 100);
                for (int part = 0; part < 2; part++) {
                    lv_obj_t* ta = lv_textarea_create(pg);
                    lv_obj_set_pos(ta, part ? 270 : 110, y);
                    lv_obj_set_size(ta, part ? 530 : 150, 52);
                    lv_textarea_set_one_line(ta, true);
                    lv_textarea_set_max_length(ta, part ? 80 : 10);
                    lv_textarea_set_accepted_chars(ta, part ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 /?.,=+-@{}"
                                                            : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 /?.,=+-@");
                    lv_textarea_set_placeholder_text(ta, part ? "text to send" : "label");
                    lv_obj_set_style_text_font(ta, f20, 0);
                    lv_obj_set_style_pad_ver(ta, 12, 0);
                    lv_obj_set_style_bg_color(ta, C(0x0b1014), 0);
                    lv_obj_set_style_text_color(ta, C(TXT), 0);
                    lv_obj_set_style_border_color(ta, C(LINE), 0);
                    lv_obj_set_style_border_color(ta, C(AMB), LV_STATE_FOCUSED);
                    lv_obj_set_style_border_width(ta, 2, 0);
                    lv_obj_set_style_radius(ta, 8, 0);
                    lv_obj_add_event_cb(ta, onSettingsTa, LV_EVENT_CLICKED, (void*)(intptr_t)i);
                    if (part) sWidget[i] = ta; else sMacroLbl[i] = ta;
                }
                continue;
            }
            lv_obj_t* ta = lv_textarea_create(pg);
            lv_obj_set_pos(ta, x, y);
            lv_obj_set_size(ta, w, 52);
            lv_textarea_set_one_line(ta, true);
            lv_textarea_set_max_length(ta, (uint32_t)f.maxLen);
            if (f.kind == SK_NUM) lv_textarea_set_accepted_chars(ta, "-0123456789");
            if (f.kind == SK_HEX) lv_textarea_set_accepted_chars(ta, "0123456789ABCDEFabcdefxX");
            if (f.kind == SK_PASS) lv_textarea_set_password_mode(ta, true);
            lv_textarea_set_placeholder_text(ta, f.hint);
            lv_obj_set_style_text_font(ta, f20, 0);
            lv_obj_set_style_pad_ver(ta, 12, 0);
            lv_obj_set_style_bg_color(ta, C(0x0b1014), 0);
            lv_obj_set_style_text_color(ta, C(TXT), 0);
            lv_obj_set_style_border_color(ta, C(LINE), 0);
            lv_obj_set_style_border_color(ta, C(AMB), LV_STATE_FOCUSED);
            lv_obj_set_style_border_width(ta, 2, 0);
            lv_obj_set_style_radius(ta, 8, 0);
            lv_obj_add_event_cb(ta, onSettingsTa, LV_EVENT_CLICKED, (void*)(intptr_t)i);
            sWidget[i] = ta;
        }
    }
    label(pages[2], 0, row[2] * 62 + 6, f16, DIM, "The radio is found automatically on USB; pick one only if the search settles on the wrong one.\nBaud rate changes take effect after a restart.");
    label(pages[6], 0, row[6] * 62 + 6, f16, DIM, "The clock is kept through restarts. A GPS module (e.g. u-blox NEO-6M/M8N) with PPS sets it to\nwithin a few ms anywhere, no internet needed. GPS changes take effect after a restart.");
    label(pages[3], 0, row[3] * 62 + 6, f16, DIM, "Label = button name. In the text: {CALL} callsign box, {RST} Sent box (599 is sent as 5NN),\n{MYCALL}, {GRID}, {REF}. The { } keys are on the 1# keyboard.");
    sWifi = label(pages[5], 0, row[5] * 62 + 8, f20, TXT, "");
    lv_obj_set_width(sWifi, 800);
    lv_label_set_long_mode(sWifi, LV_LABEL_LONG_MODE_WRAP);
    button(pages[5], 0, row[5] * 62 + 64, 230, 52, LV_SYMBOL_WIFI " Save + connect", onWifiConnect, nullptr, 0x1f6b45);
    button(pages[5], 244, row[5] * 62 + 64, 180, 52, "Disconnect", onWifiDisconnect);
    label(pages[5], 0, row[5] * 62 + 130, f16, DIM, "Leave a password box empty to keep the current password. The home network must be 2.4 GHz.\nHotspot name/password changes take effect after a restart; the home network is joined right away.");

    {   // Device
        lv_obj_t* pg = pages[7];
        auto slider = [&](int y, const char* name, int lo, int hi, lv_event_cb_t cb, lv_obj_t** val) {
            label(pg, 0, y + 10, f20, DIM, name);
            lv_obj_t* sl = lv_slider_create(pg);
            lv_obj_set_pos(sl, 200, y + 14);
            lv_obj_set_size(sl, 480, 24);
            lv_slider_set_range(sl, lo, hi);
            lv_obj_set_style_bg_color(sl, C(LINE), LV_PART_MAIN);
            lv_obj_set_style_bg_color(sl, C(AMB), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(sl, C(TXT), LV_PART_KNOB);
            lv_obj_set_style_pad_all(sl, 8, LV_PART_KNOB);
            lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, nullptr);
            lv_obj_add_event_cb(sl, cb, LV_EVENT_RELEASED, nullptr);
            *val = label(pg, 710, y + 10, f20, TXT, "");
            return sl;
        };
        sBright = slider(0, "Brightness", 5, 100, onBright, &sBrightL);
        sContrast = slider(62, "Contrast", 50, 150, onContrast, &sContrastL);
        label(pg, 0, 116, f16, DIM, "Brightness changes at once. Contrast (100 % = normal) applies after a restart.");
        sStor = label(pg, 0, 160, f20, TXT, "");
        lv_obj_set_width(sStor, 800);
        lv_label_set_long_mode(sStor, LV_LABEL_LONG_MODE_WRAP);
        label(pg, 0, 300, f16, DIM, "Logs go on a microSD card (FAT32) when one is in at start-up; anything logged in the board's\n"
                                    "memory moves onto the card then. Files are named after the station callsign.");
        button(pg, 0, 356, 260, 52, LV_SYMBOL_TRASH " Erase contact logs", onNuke, nullptr, 0x9a2a2a);
        button(pg, 280, 356, 300, 52, LV_SYMBOL_SD_CARD " Format microSD card", onSdFormat, nullptr, 0x9a2a2a);
        sConsole = lv_switch_create(pg);
        lv_obj_set_pos(sConsole, 0, 428);
        lv_obj_set_size(sConsole, 70, 36);
        lv_obj_set_style_bg_color(sConsole, C(LINE), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sConsole, C(AMB), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_event_cb(sConsole, onConsoleLog, LV_EVENT_VALUE_CHANGED, nullptr);
        label(pg, 86, 434, f20, TXT, "Console log (troubleshooting) to the card, kept 7 days");
    }

    sKb = lv_keyboard_create(scrSettings);
    lv_keyboard_set_map(sKb, LV_KEYBOARD_MODE_USER_2, kCallKeys, kCallCtrl);
    lv_obj_set_size(sKb, W, SET_KB);
    lv_obj_align(sKb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(sKb, f20, 0);
    lv_obj_set_style_bg_color(sKb, C(PAN), 0);
    lv_obj_set_style_bg_color(sKb, C(PAN2), LV_PART_ITEMS);
    lv_obj_set_style_text_color(sKb, C(TXT), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(sKb, C(LINE), LV_PART_ITEMS | LV_STATE_CHECKED);     // control keys: no glaring white
    lv_obj_set_style_text_color(sKb, C(AMB), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sKb, onSettingsKbDone, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(sKb, onSettingsKbDone, LV_EVENT_CANCEL, nullptr);
    lv_obj_add_flag(sKb, LV_OBJ_FLAG_HIDDEN);
}

void refreshSettings(const App::UiState& s) {
    if (lv_screen_active() != scrSettings) return;
    bool noteOn = !noteText.empty() && (int32_t)(noteUntil - lv_tick_get()) > 0;
    setText(sStatus, noteOn ? noteText : s.status);
    setTextColor(sStatus, noteOn ? AMB : DIM);
    setText(sWifi, staText(s) + (s.wchipVer.empty() ? "" : "\nWi-Fi chip firmware: " + s.wchipVer));
    setTextColor(sWifi, s.staState == "connected" ? 0x3fcf8e : s.staState == "retrying" ? 0xff6b6b : TXT);
    setText(sStor, storText(s));
    setTextColor(sStor, s.left >= 0 && s.left < 200 ? 0xff6b6b : TXT);
}

// ---- periodic refresh ---------------------------------------------------------------------------
std::string hhmmss(int64_t ms) {
    long s = (long)((ms / 1000) % 86400);
    char b[12];
    snprintf(b, sizeof b, "%02ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    return b;
}

void rebuildTable() {
    const size_t maxRows = 80;
    if (rows.size() > maxRows) rows.erase(rows.begin(), rows.begin() + (rows.size() - maxRows));
    lv_table_set_row_count(table, (uint32_t)rows.size());
    for (size_t r = 0; r < rows.size(); r++)
        for (int c = 0; c < 5; c++) lv_table_set_cell_value(table, (uint32_t)r, c, rows[r].c[c].c_str());
    // Follow the newest line unless the operator touched the list in the last few seconds.
    if (lv_tick_elaps(lastTouchTick) > 8000) {
        lv_obj_update_layout(table);
        lv_obj_scroll_to_y(table, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

int shownBright = -1;
void refresh(lv_timer_t*) {
    if (g_app->cfg.bright != shownBright) {             // also when changed from the web page
        shownBright = g_app->cfg.bright;
        if (ui_brightness_fn) ui_brightness_fn(shownBright);
    }
    if (!g_app) return;
    {                                                   // any touch on the screen is operator activity (TX watchdog)
        static uint32_t lastAct = 0;
        if (lv_display_get_inactive_time(nullptr) < 400 && lv_tick_elaps(lastAct) > 2000) {
            lastAct = lv_tick_get();
            g_app->activity();
        }
    }
    App::UiState s;
    g_app->uiState(s);
    char b[96];

    if (s.waitNext != gWaitNext) {                       // POTA mode: CQ button <-> Next QSO
        gWaitNext = s.waitNext;
        setText(lCq, gWaitNext ? "Next QSO" : "CQ");
        lv_obj_set_style_bg_color(bCq, C(gWaitNext ? 0xb07800 : PAN2), 0);
    }
    // header
    if (s.freq) {
        snprintf(b, sizeof b, "%u.%03u.%03u", (unsigned)(s.freq / 1000000), (unsigned)(s.freq / 1000 % 1000), (unsigned)(s.freq % 1000));
        setText(lFreq, b);
    } else setText(lFreq, "--.---.---");
    static const char* modeNames[] = {"FT8", "FT4", "PSK31", "FT-CW", "APRS"};
    snprintf(b, sizeof b, "%s  %s  %s\n%s", s.band.empty() ? "--" : s.band.c_str(), s.rmode == 1 ? (s.data == 1 ? "USB-D" : "USB") : "",
             modeNames[s.mode < 0 || s.mode > 4 ? 0 : s.mode], s.model == "No CAT" ? "No CAT: tap the frequency" : s.model.c_str());
    setText(lSub, b);
    bool tx = s.txOn || s.ptt;
    if (tx) {
        int po = std::min(100, s.po * 100 / 213);
        lv_bar_set_value(barS, po, LV_ANIM_OFF);
        setBg(barS, TXC, LV_PART_INDICATOR);
        snprintf(b, sizeof b, "Po %d%%", po);
    } else {
        lv_bar_set_value(barS, std::min(100, s.smeter * 100 / 241), LV_ANIM_OFF);
        setBg(barS, RXC, LV_PART_INDICATOR);
        if (s.smeter <= 120) snprintf(b, sizeof b, "S%d", (s.smeter * 9 + 60) / 120);
        else snprintf(b, sizeof b, "S9+%d", (s.smeter - 120) / 2);
    }
    setText(lS, b);
    int lv = std::max(0, std::min(100, (s.lvlDb + 60) * 100 / 60));
    lv_bar_set_value(barLvl, lv, LV_ANIM_OFF);
    setBg(barLvl, s.lvlDb > -3 ? TXC : (s.lvlDb < -45 ? AMB : RXC), LV_PART_INDICATOR);
    if (s.lvlDb <= -99) setText(lLvl, "no audio");
    else { snprintf(b, sizeof b, "%d dB", s.lvlDb); setText(lLvl, b); }
    const bool ok[4] = {s.ser, s.civ, s.aud, s.clkOk};
    for (int i = 0; i < 4; i++) setTextColor(lDot[i], ok[i] ? RXC : TXC);
    setText(lClock, s.clkOk ? hhmmss(s.utc) : std::string("no clock"));
    setText(lCall, s.call.empty() ? std::string("Set callsign on the web page") : s.call + "  " + s.myGrid);

    // slot bar and transmit line
    lv_bar_set_value(barSlot, s.mode == DM_PSK31 ? (s.txOn ? 1000 : 0) : s.slotT * 1000 / std::max(1, s.slotLen), LV_ANIM_OFF);
    setBg(barSlot, s.txOn ? TXC : RXC, LV_PART_INDICATOR);
    if (s.txOn) { setText(lTx, "TX   " + s.txText); setTextColor(lTx, TXC); }
    else if (s.txEn && !s.nextText.empty()) { setText(lTx, "Next:  " + s.nextText + (s.txPar ? "   (odd slots)" : "   (even slots)")); setTextColor(lTx, AMB); }
    else { setText(lTx, "Receiving"); setTextColor(lTx, DIM); }
    if (lv_tick_get() < noteUntil && !noteText.empty()) { setText(lStatus, noteText); setTextColor(lStatus, AMB); }
    else if (s.wd) { setText(lStatus, "TX watchdog stopped auto TX - touch Enable TX"); setTextColor(lStatus, AMB); }
    else if (s.civ && s.rmode >= 0 && (s.rmode != 1 || s.data != 1)) { setText(lStatus, "Radio is not in USB-D: tap a band button"); setTextColor(lStatus, AMB); }
    else { setText(lStatus, s.status); setTextColor(lStatus, DIM); }

    // controls
    setText(lTxEn, s.txEn ? "TX enabled" : "Enable TX");
    setBg(bTxEn, s.txEn ? TXC : PAN2);
    setTextColor(lTxEn, s.txEn ? 0x2a0000 : TXT);
    setBg(bTune, s.tune ? TXC : PAN2);
    setTextColor(lTune, s.tune ? 0x2a0000 : TXT);
    setText(lPar, s.txPar ? "Slot: odd" : "Slot: even");
    if (s.op != shownOp) { shownOp = s.op; matrixCheck(mxOp, 3, s.op); }
    if (s.mode != shownMode) { shownMode = s.mode; matrixCheck(mxMode, 3, modeIndex(s.mode)); }
    int band = -1;
    for (int i = 0; i < kNumBands; i++) {
        long f = (long)bandFreq(i, s.mode);
        if (f && labs((long)s.freq - f) < 500) band = i;
    }
    if (band != shownBand) { shownBand = band; matrixCheck(mxBand, kNumBands, band); }

    // QSO panel
    if (s.dx.empty()) setText(lQso, "No QSO in progress");
    else {
        char rs[8] = "-", rr[8] = "-";
        if (s.rptSent > -99) snprintf(rs, sizeof rs, "%+03d", s.rptSent);
        if (s.rptRcvd > -99) snprintf(rr, sizeof rr, "%+03d", s.rptRcvd);
        snprintf(b, sizeof b, "%s %s\nsent %s   rcvd %s   try %d", s.dx.c_str(), s.grid.c_str(), rs, rr, s.tries);
        setText(lQso, b);
    }
    if (tx) {
        float swr = s.swr <= 48 ? 1 + s.swr / 96.0f : (s.swr <= 80 ? 1.5f + (s.swr - 48) / 64.0f : 2 + (s.swr - 80) / 40.0f);
        snprintf(b, sizeof b, "ALC %d%%   SWR %.1f   TX %d Hz", std::min(100, s.alc * 100 / 120), swr, s.txFreq);
    } else snprintf(b, sizeof b, "TX audio %d Hz   decode %d ms", s.txFreq, s.decodeMs);
    setText(lMeters, b);
    snprintf(b, sizeof b, "Log: %d " LV_SYMBOL_RIGHT, s.logN);
    setText(lLogBtn, b);
    refreshRadioScreen(s);
    refreshLogger(s);
    refreshSettings(s);
    if (lv_screen_active() == scrLog) {
        if (s.logN != shownLogN || s.logAll != shownLogAll) {       // a contact was logged while the logbook is open
            if (shownLogN >= 0) fillLogTable();
            shownLogN = s.logN;
            shownLogAll = s.logAll;
            snprintf(b, sizeof b, "Current log: %d      Archive: %d", s.logN, s.logAll);
            setText(lLogCounts, b);
        }
        if (s.unsent != shownUnsent) {
            shownUnsent = s.unsent;
            setText(lLogSync, s.unsent ? "Send unsent (" + num(s.unsent) + ")" : std::string("All sent"));
        }
        std::string info = "Tap a contact to edit or delete it. Logs download from the web page.  " + s.net;
        if (s.netOk || s.netFail) { snprintf(b, sizeof b, "\nSent to logger: %d, failed attempts: %d", s.netOk, s.netFail); info += b; }
        setText(lLogInfo, info);
    }

    // band activity
    bool changed = false;
    if (s.txOn && !s.txText.empty() && s.txText != lastTxText && s.txText != "TUNE" && s.txText != "PSK31") {
        Row r{0, 3, (int)((s.utc / std::max(1, s.slotLen)) & 1), {hhmmss(s.utc), "TX", "", num(s.txFreq), s.txText}};
        rows.push_back(r);
        changed = true;
    }
    lastTxText = s.txOn ? s.txText : "";
    if (s.decId < lastDecId) lastDecId = 0;                 // list was cleared (mode change)
    if (s.decId != lastDecId) {
        std::vector<App::UiDecode> d;
        g_app->uiDecodes(lastDecId, d);
        for (auto& x : d) {
            char dt[12];
            if (x.dt > -0.05f && x.dt < 0.05f) snprintf(dt, sizeof dt, "0.0");
            else snprintf(dt, sizeof dt, "%+.1f", x.dt);
            Row r{x.id, x.toMe ? 2 : (x.cq ? 1 : 0), (int)((x.slotMs / std::max(1, s.slotLen)) & 1),
                  {hhmmss(x.slotMs), num(x.snr), dt, num(x.freq), x.text}};
            rows.push_back(r);
        }
        lastDecId = s.decId;
        changed = changed || !d.empty();
    }
    if (changed) rebuildTable();
    bool psk = s.mode == DM_PSK31 || s.mode == DM_APRS;
    static int pskShownFor = -1;
    if (psk && pskShownFor != s.mode) {
        pskShownFor = s.mode;
        setText(lPsk, s.mode == DM_APRS ? "APRS is operated from the web page.\nSelect FT8 or FT4 below to use this screen."
                                        : "PSK31 is operated from the web page.\nSelect FT8 or FT4 below to use this screen.");
    }
    if (psk == lv_obj_has_flag(lPsk, LV_OBJ_FLAG_HIDDEN)) {
        if (psk) lv_obj_remove_flag(lPsk, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(lPsk, LV_OBJ_FLAG_HIDDEN);
    }

    // transmit-frequency marker on the waterfall
    int mx = s.txFreq * W / 3000, mw = (s.mode == DM_FT4 ? 83 : 50) * W / 3000;
    if (lv_obj_get_x(marker) != mx || lv_obj_get_width(marker) != mw) { lv_obj_set_x(marker, mx); lv_obj_set_width(marker, mw); }
}

void refreshWaterfall(lv_timer_t*) {
    if (!g_app || !wfBuf) return;
    uint8_t bins[Spectrum::BINS];
    uint32_t seq;
    g_app->uiSpectrum(bins, seq);
    if (seq == specSeq) return;
    specSeq = seq;
    // Auto level: the lower quartile of this row is taken as the noise floor.
    uint8_t sorted[Spectrum::BINS];
    memcpy(sorted, bins, sizeof sorted);
    std::nth_element(sorted, sorted + Spectrum::BINS / 4, sorted + Spectrum::BINS);
    int floor = sorted[Spectrum::BINS / 4];
    memmove(wfBuf + W, wfBuf, (size_t)W * (WF_H - 1) * sizeof(uint16_t));
    for (int x = 0; x < W; x++) {
        int v = (bins[x * Spectrum::BINS / W] - floor + 4) * 255 / 70;
        wfBuf[x] = lut[v < 0 ? 0 : (v > 255 ? 255 : v)];
    }
    lv_obj_invalidate(canvas);
}

void buildLut() {                                    // black, blue, cyan, yellow, white
    static const uint8_t stops[5][3] = {{0, 0, 0}, {10, 20, 110}, {20, 150, 190}, {240, 220, 60}, {255, 255, 255}};
    for (int i = 0; i < 256; i++) {
        float x = i / 255.0f * 3.999f;
        int k = (int)x;
        float f = x - k;
        int rgb[3];
        for (int c = 0; c < 3; c++) rgb[c] = (int)(stops[k][c] + (stops[k + 1][c] - stops[k][c]) * f);
        lut[i] = (uint16_t)(((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
    }
}

}  // namespace

void ui_bind(App* app) { g_app = app; }
void (*ui_brightness_fn)(int percent) = nullptr;

// Start-up screen: the station's callsign, large, in polished gold (Audiowide font), with
// "RigDeck by AC9HP" under it; over everything for a few seconds while the radio connects. The gold
// is computed here from the letter shapes: a soft bevel lit from the upper left with a highlight,
// metal bands across the letter height, a fine brushed texture, a dark rim and a warm shadow.
extern "C" const uint8_t audiowide_call_ttf[];
extern "C" const size_t audiowide_call_ttf_len;
static lv_obj_t* gSplash;
static lv_draw_buf_t* gLogoBuf;
static void splashGone(lv_anim_t*) {
    if (gSplash) { lv_obj_delete(gSplash); gSplash = nullptr; }
    if (gLogoBuf) { lv_draw_buf_destroy(gLogoBuf); gLogoBuf = nullptr; }
}
static void splashFade(lv_obj_t* o) {
    if (!o || o != gSplash) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, 600);
    lv_anim_set_exec_cb(&a, [](void* v, int32_t x) { lv_obj_set_style_opa((lv_obj_t*)v, (lv_opa_t)x, 0); });
    lv_anim_set_completed_cb(&a, splashGone);
    lv_anim_start(&a);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

static void boxBlur(const float* in, float* out, float* tmp, int w, int h, int r) {   // one separable pass
    for (int y = 0; y < h; y++) {
        const float* row = in + y * w;
        float acc = 0;
        for (int x = -r; x <= r; x++) acc += row[x < 0 ? 0 : x >= w ? w - 1 : x];
        for (int x = 0; x < w; x++) {
            tmp[y * w + x] = acc / (2 * r + 1);
            int xa = x + r + 1, xr = x - r;
            acc += row[xa >= w ? w - 1 : xa] - row[xr < 0 ? 0 : xr];
        }
    }
    for (int x = 0; x < w; x++) {
        float acc = 0;
        for (int y = -r; y <= r; y++) acc += tmp[(y < 0 ? 0 : y >= h ? h - 1 : y) * w + x];
        for (int y = 0; y < h; y++) {
            out[y * w + x] = acc / (2 * r + 1);
            int ya = y + r + 1, yr = y - r;
            acc += tmp[(ya >= h ? h - 1 : ya) * w + x] - tmp[(yr < 0 ? 0 : yr) * w + x];
        }
    }
}
static void gauss(const float* in, float* out, float* t1, float* t2, int w, int h, int r) {   // 3 box passes
    boxBlur(in, t1, t2, w, h, r);
    boxBlur(t1, out, t2, w, h, r);
    boxBlur(out, t1, t2, w, h, r);
    memcpy(out, t1, sizeof(float) * w * h);
}

// Gold text into an ARGB8888 draw buffer of w x h (black background). False if out of memory.
static bool goldText(lv_draw_buf_t* buf, const char* text) {
    const int w = buf->header.w, h = buf->header.h;
    // 1. letter shapes: draw the text in white with the largest Audiowide size that fits
    int size = h * 88 / 100;
    lv_font_t* f = nullptr;
    for (int tries = 0; tries < 4; tries++) {
        f = lv_tiny_ttf_create_data(audiowide_call_ttf, audiowide_call_ttf_len, size);
        if (!f) return false;
        lv_point_t sz;
        lv_text_get_size(&sz, text, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= w * 92 / 100 || tries == 3) break;
        int ns = size * (w * 92 / 100) / sz.x;
        lv_tiny_ttf_destroy(f);
        size = ns;
    }
    lv_obj_t* cv = lv_canvas_create(lv_layer_top());
    lv_canvas_set_draw_buf(cv, buf);
    lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_TRANSP);
    lv_layer_t layer;
    lv_canvas_init_layer(cv, &layer);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = f;
    d.color = lv_color_white();
    d.align = LV_TEXT_ALIGN_CENTER;
    d.text = text;
    int32_t lh = lv_font_get_line_height(f);
    lv_area_t a = {0, (h - lh) / 2, w - 1, (h - lh) / 2 + lh - 1};
    lv_draw_label(&layer, &d, &a);
    lv_canvas_finish_layer(cv, &layer);
    lv_obj_delete(cv);
    lv_tiny_ttf_destroy(f);

    // 2. coverage mask and its extent
    const int n = w * h;
    float* M = (float*)malloc(sizeof(float) * n * 5);
    if (!M) return false;
    float *hg = M + n, *t1 = M + 2 * n, *t2 = M + 3 * n, *sh = M + 4 * n;
    uint8_t* px = (uint8_t*)buf->data;
    const uint32_t stride = buf->header.stride;
    int top = h, bot = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float v = px[y * stride + x * 4 + 3] / 255.0f;
            M[y * w + x] = v;
            if (v > 0.5f) { if (y < top) top = y; if (y > bot) bot = y; }
        }
    if (bot <= top) { free(M); return false; }

    // 3. bevel height field and the shadow
    gauss(M, hg, t1, t2, w, h, 3);
    gauss(M, sh, t1, t2, w, h, 4);
    const float Lx = -0.55f, Ly = -0.75f, Lz = 0.36f, Ln = sqrtf(Lx * Lx + Ly * Ly + Lz * Lz);
    const float lx = Lx / Ln, ly = Ly / Ln, lz = Lz / Ln;
    float hx = lx, hy = ly, hz = lz + 1, hn = sqrtf(hx * hx + hy * hy + hz * hz);
    hx /= hn; hy /= hn; hz /= hn;
    struct Stop { float t, r, g, b; };
    static const Stop st[] = {{0.00f, 255, 236, 160}, {0.18f, 250, 210, 92}, {0.42f, 184, 128, 26}, {0.50f, 122, 82, 10},
                              {0.58f, 196, 142, 34},  {0.78f, 255, 214, 100}, {1.00f, 150, 100, 18}};
    uint32_t seed = 12345;
    for (int y = 0; y < h; y++) {
        float t = (float)(y - top) / (float)(bot - top);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        int k = 0;
        while (k < 5 && t > st[k + 1].t) k++;
        float u = (t - st[k].t) / (st[k + 1].t - st[k].t);
        float br = st[k].r + (st[k + 1].r - st[k].r) * u, bg = st[k].g + (st[k + 1].g - st[k].g) * u,
              bb = st[k].b + (st[k + 1].b - st[k].b) * u;
        for (int x = 0; x < w; x++) {
            int i = y * w + x;
            float m = M[i];
            float gx = (hg[y * w + (x < w - 1 ? x + 1 : x)] - hg[y * w + (x > 0 ? x - 1 : x)]) * 0.5f;
            float gy = (hg[(y < h - 1 ? y + 1 : y) * w + x] - hg[(y > 0 ? y - 1 : y) * w + x]) * 0.5f;
            float nx = -gx * 22, ny = -gy * 22, nz = 1, nn = sqrtf(nx * nx + ny * ny + nz * nz);
            nx /= nn; ny /= nn; nz /= nn;
            float dif = nx * lx + ny * ly + nz * lz; dif = dif < 0 ? 0 : dif;
            float sp = nx * hx + ny * hy + nz * hz; sp = sp < 0 ? 0 : sp;
            sp = powf(sp, 40);
            seed = seed * 1664525u + 1013904223u;
            float tex = 0.96f + 0.08f * ((seed >> 8) & 0xFFFF) / 65535.0f;
            float shade = (0.55f + 0.65f * dif) * tex;
            float r = br * shade + sp * 230, g = bg * shade + sp * 225, b = bb * shade + sp * 200;
            // outside the letters: warm shadow (offset down-right) and a thin dark rim
            int sx = x - 5, sy = y - 6;
            float s = (sx >= 0 && sy >= 0) ? sh[sy * w + sx] : 0;
            float rim = hg[i] > m ? (hg[i] - m) * 2.2f : 0;
            rim = rim > 1 ? 1 : rim;
            float or_ = 48 * s + 70 * rim, og = 32 * s + 48 * rim, ob = 5 * s + 8 * rim;
            r = r * m + or_ * (1 - m); g = g * m + og * (1 - m); b = b * m + ob * (1 - m);
            uint8_t* p = px + y * stride + x * 4;
            p[0] = (uint8_t)(b > 255 ? 255 : b); p[1] = (uint8_t)(g > 255 ? 255 : g);
            p[2] = (uint8_t)(r > 255 ? 255 : r); p[3] = 255;
        }
    }
    free(M);
    return true;
}

static void showSplash() {
    std::string call = g_app->cfg.call;
    for (char& c : call) c = (char)toupper((unsigned char)c);
    if (call.empty() || call == "N0CALL") call = "RigDeck";      // mixed case: the font subset has a-z too
    gSplash = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(gSplash);
    lv_obj_set_size(gSplash, W, H);
    lv_obj_set_style_bg_color(gSplash, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(gSplash, LV_OPA_COVER, 0);
    lv_obj_add_flag(gSplash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(gSplash, [](lv_event_t* e) { splashFade((lv_obj_t*)lv_event_get_target(e)); }, LV_EVENT_CLICKED, nullptr);
    gLogoBuf = lv_draw_buf_create(900, 200, LV_COLOR_FORMAT_ARGB8888, 0);
    if (gLogoBuf && goldText(gLogoBuf, call.c_str())) {
        lv_obj_t* im = lv_image_create(gSplash);
        lv_image_set_src(im, gLogoBuf);
        lv_obj_align(im, LV_ALIGN_CENTER, 0, -50);
    }
    lv_obj_t* t = lv_label_create(gSplash);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xD4AF5A), 0);
    lv_label_set_text(t, "RigDeck by AC9HP");
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 85);
    lv_obj_t* v = lv_label_create(gSplash);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(0x8A7A55), 0);
    lv_label_set_text(v, g_app->fwVersion.c_str());
    lv_obj_align(v, LV_ALIGN_CENTER, 0, 125);
    lv_timer_create([](lv_timer_t* tt) { splashFade(gSplash); lv_timer_delete(tt); }, 3500, nullptr);
}

extern "C" void ui_create(void) {
    gContrast = g_app->cfg.contrast;
    lv_obj_t* scr = lv_screen_active();
    scrMain = scr;
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, C(BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    const lv_font_t *f16 = &lv_font_montserrat_16, *f20 = &lv_font_montserrat_20, *f28 = &lv_font_montserrat_28, *f36 = &lv_font_montserrat_36;

    // header
    lv_obj_t* head = box(scr, 0, 0, W, HEAD_H, PAN);
    lFreq = label(head, 14, 8, f36, AMB, "--.---.---");
    lv_obj_add_flag(lFreq, LV_OBJ_FLAG_CLICKABLE);                   // the radio screen: bands, modes, typed frequency
    lv_obj_add_event_cb(lFreq, onOpenRadio, LV_EVENT_CLICKED, nullptr);
    lSub = label(head, 262, 9, f16, DIM);
    label(head, 440, 8, f16, DIM, "Sig");
    barS = meter(head, 484, 13, 150);
    lS = label(head, 644, 8, f16, TXT);
    label(head, 440, 32, f16, DIM, "Aud");
    barLvl = meter(head, 484, 37, 150);
    lLvl = label(head, 644, 32, f16, TXT);
    static const char* dotNames[4] = {"USB", "CI-V", "Audio", "Clock"};
    for (int i = 0; i < 4; i++) lDot[i] = label(head, 728 + (i % 2) * 62, i < 2 ? 8 : 32, f16, TXC, dotNames[i]);
    lClock = label(head, 0, 4, f28, TXT, "--:--:--");
    lv_obj_align(lClock, LV_ALIGN_TOP_RIGHT, -14, 3);
    lCall = label(head, 0, 0, f16, DIM);
    lv_obj_align(lCall, LV_ALIGN_BOTTOM_RIGHT, -14, -4);
    lv_obj_t* lGear;
    button(head, 848, 8, 46, 46, LV_SYMBOL_SETTINGS, onOpenSettings, &lGear);
    lv_obj_center(lGear);

    // slot progress
    barSlot = lv_bar_create(scr);
    lv_obj_set_pos(barSlot, 0, HEAD_H);
    lv_obj_set_size(barSlot, W, SLOT_H);
    lv_bar_set_range(barSlot, 0, 1000);
    lv_obj_set_style_radius(barSlot, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(barSlot, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(barSlot, C(0x0b1014), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(barSlot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(barSlot, C(RXC), LV_PART_INDICATOR);

    // waterfall (tap to set the transmit audio frequency)
    buildLut();
    wfBuf = (uint16_t*)malloc((size_t)W * WF_H * sizeof(uint16_t));
    canvas = lv_canvas_create(scr);
    lv_obj_set_pos(canvas, 0, WF_Y);
    if (wfBuf) {
        memset(wfBuf, 0, (size_t)W * WF_H * sizeof(uint16_t));
        lv_canvas_set_buffer(canvas, wfBuf, W, WF_H, LV_COLOR_FORMAT_RGB565);
    }
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(canvas, onWaterfall, LV_EVENT_CLICKED, nullptr);
    marker = box(scr, 0, WF_Y, 16, 4, TXC);

    // transmit / status line
    lv_obj_t* line = box(scr, 0, LINE_Y, W, LINE_H, PAN);
    lTx = label(line, 14, 6, f20, DIM, "Receiving");
    lStatus = label(line, 0, 0, f16, DIM);
    lv_obj_set_width(lStatus, 470);
    lv_label_set_long_mode(lStatus, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(lStatus, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(lStatus, LV_ALIGN_RIGHT_MID, -14, 0);

    // band activity
    table = lv_table_create(scr);
    lv_obj_set_pos(table, 0, MAIN_Y);
    lv_obj_set_size(table, TABLE_W, MAIN_H);
    lv_table_set_column_count(table, 5);
    static const int colW[5] = {96, 54, 62, 70, TABLE_W - 96 - 54 - 62 - 70 - 12};
    for (int c = 0; c < 5; c++) lv_table_set_column_width(table, c, colW[c]);
    lv_table_set_row_count(table, 0);
    lv_obj_set_style_bg_color(table, C(0x0b1014), 0);
    lv_obj_set_style_border_width(table, 0, 0);
    lv_obj_set_style_radius(table, 0, 0);
    lv_obj_set_style_pad_all(table, 0, 0);
    lv_obj_set_style_text_font(table, f20, LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, C(TXT), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(table, C(0x0b1014), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(table, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(table, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(table, 6, LV_PART_ITEMS);
    lv_obj_add_event_cb(table, onTable, LV_EVENT_ALL, nullptr);
    lv_obj_add_event_cb(table, onTableDraw, LV_EVENT_DRAW_TASK_ADDED, nullptr);
    lv_obj_add_flag(table, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lPsk = label(scr, 40, MAIN_Y + 60, f20, DIM, "PSK31 is operated from the web page.\nSelect FT8 or FT4 below to use this screen.");
    lv_obj_add_flag(lPsk, LV_OBJ_FLAG_HIDDEN);

    // control panel
    const int PW = W - PANEL_X - 10;
    lv_obj_t* pan = box(scr, PANEL_X, MAIN_Y, W - PANEL_X, MAIN_H, BG);
    bTxEn = button(pan, 0, 8, PW / 2 - 4, 64, "Enable TX", onTxEn, &lTxEn);
    lv_obj_t* lHalt;
    button(pan, PW / 2 + 4, 8, PW / 2 - 4, 64, "HALT TX", onHalt, &lHalt, 0x7a2020);
    mxOp = matrix(pan, 0, 82, PW, 52, kOpMap, onOp);
    int bw = (PW - 18) / 4;
    bCq = button(pan, 0, 144, bw, 52, "CQ", onCq, &lCq);
    bTune = button(pan, bw + 6, 144, bw, 52, "Tune", onTune, &lTune);
    button(pan, 2 * (bw + 6), 144, bw, 52, "Log QSO", onLogDx, nullptr, 0x1f5a3a);   // log now, even unfinished, then CQ
    button(pan, 3 * (bw + 6), 144, PW - 3 * (bw + 6), 52, "Clear", onClear);
    lQso = label(pan, 4, 204, f20, TXT, "No QSO in progress");
    lMeters = label(pan, 4, 258, f16, DIM);
    const int qw = (PW - 18) / 4;
    bPar = button(pan, 0, 288, qw, 44, "Slot: even", onParity, &lPar);
    lv_obj_set_style_text_font(lPar, f16, 0);
    button(pan, qw + 6, 288, qw, 44, "Log: 0", onOpenLog, &lLogBtn);
    lv_obj_set_style_text_font(lLogBtn, f16, 0);
    lv_obj_t *lRadioBtn, *lLoggerBtn;
    button(pan, 2 * (qw + 6), 288, qw, 44, "SSB/CW", onOpenLogger, &lLoggerBtn);
    lv_obj_set_style_text_font(lLoggerBtn, f16, 0);
    button(pan, 3 * (qw + 6), 288, PW - 3 * (qw + 6), 44, "Radio " LV_SYMBOL_RIGHT, onOpenRadio, &lRadioBtn);
    lv_obj_set_style_text_font(lRadioBtn, f16, 0);

    // bottom bar: mode and band
    lv_obj_t* bot = box(scr, 0, H - BOT_H, W, BOT_H, PAN);
    mxMode = matrix(bot, 10, 8, 236, BOT_H - 16, kModeMap, onMode);
    for (int i = 0; i < kNumBands; i++) kBandMap[i] = kBands[i].name;
    kBandMap[kNumBands] = "";
    mxBand = matrix(bot, 258, 8, W - 258 - 10, BOT_H - 16, kBandMap, onBand);

    buildLogScreen();
    buildLogEditScreen();
    buildRadioScreen();
    buildLoggerScreen();
    buildSettingsScreen();
    lv_timer_create(refresh, 200, nullptr);
    lv_timer_create(refreshWaterfall, 125, nullptr);
    refresh(nullptr);
    showSplash();
}
