// USB host side of the radio link. One USB cable to the radio carries everything.
//  IC-7100: the USB-B port is a hub with two CP210x serial bridges (one is CI-V) and a USB Audio
//           Class 1 codec.
//  IC-705:  the micro-USB port is one composite device: two CDC-ACM serial ports and USB audio.
//  FTX-1:   like the IC-7100: a hub, a CP2105 dual serial bridge (first port = CAT-1) and an audio codec.
//  QMX/QMX+: one composite device: CDC-ACM serial port(s) and 24-bit USB audio.
//  IC-7300 / IC-7610 / IC-9700, FTDX10 / FT-991A / FT-710: like the IC-7100 / FTX-1 (CP210x + codec).
//  Digirig: a hub with a CP2102 (CAT, PTT on RTS) and a CM108 sound card.
//  SignaLink USB: only a sound card; it keys the radio itself (VOX) - Settings > Radio "No CAT".
//
// Endpoint budget: an ESP32-S3 has only 8 host channels. Hub (2) + three control pipes (3) + CI-V
// bulk in/out (2) leaves exactly one for audio, so the receive and transmit audio streams are never
// open at the same time. FT8 is half duplex anyway. ESP32-P4 (16 channels) has room to spare.
#include "platform.h"
#include "clock.h"
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <atomic>
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "usb/uac_host.h"

static const char* TAG = "usb";
static App* s_app;

// ---------------------------------------------------------------------------- CI-V serial port
// Two kinds of radio-side serial port are supported:
//  - IC-7100: Silicon Labs CP210x bridges behind the radio's hub (vendor-specific setup requests).
//  - IC-705:  one composite USB device with two standard CDC-ACM ports ("A" = CI-V, "B") and audio.
#define SILABS_VID 0x10C4
#define ICOM_VID 0x0C26
#define STM_VID 0x0483                 // QRP Labs QMX / QMX+ (0483:A34C)
#define WCH_VID 0x1A86                 // WCH CH340 / CH341 serial chips: FX-4CR (1A86:7523), many CAT and CI-V cables
#define CH34X_REQ_OUT 0x40             // vendor, host-to-device, recipient device
#define CH34X_REQ_WRITE_REG 0x9A
#define CH34X_REQ_SERIAL_INIT 0xA1
#define CH34X_REQ_MODEM_CTRL 0xA4      // value = inverted bits: 0x20 DTR, 0x40 RTS (lines are active low)
#define CP210X_REQ_OUT 0x41            // vendor, host-to-device, recipient interface
#define CP210X_IFC_ENABLE 0x00
#define CP210X_SET_LINE_CTL 0x03
#define CP210X_SET_MHS 0x07
#define CP210X_SET_BAUDRATE 0x1E

enum PortKind { PK_CP210X, PK_CDC, PK_CH34X };
// Which radios can be behind each kind of port (bit per RadioProto).
static const struct { uint16_t vid; PortKind kind; uint8_t ifaces; uint8_t protos; } kPortTypes[] = {
    {SILABS_VID, PK_CP210X, 2, (1 << RP_CIV) | (1 << RP_YAESU) | (1 << RP_KENWOOD)},
    {ICOM_VID, PK_CDC, 8, 1 << RP_CIV},
    {STM_VID, PK_CDC, 8, 1 << RP_QMX},
    {WCH_VID, PK_CH34X, 1, (1 << RP_CIV) | (1 << RP_YAESU) | (1 << RP_KENWOOD)},
};

static cdc_acm_dev_hdl_t s_cdc = nullptr;
static cdc_acm_dev_hdl_t s_gps = nullptr;         // the radio's second serial port, used for GPS sentences
static uint8_t s_civDev = 0, s_civIface = 0;      // where the CI-V port was found
static PortKind s_civKind = PK_CP210X;
static uint16_t s_civVid = 0;
static SemaphoreHandle_t s_cdcMtx;

static bool cdcRx(const uint8_t* data, size_t len, void*) {
    s_app->radio.onRx(data, len);
    return true;
}

static bool ignoreRx(const uint8_t*, size_t, void*) { return true; }   // a port used only for its PTT line

static bool gpsRx(const uint8_t* data, size_t len, void*) {
    s_app->gpsRx(data, len);
    return true;
}

static void cdcEvent(const cdc_acm_host_dev_event_data_t* ev, void*) {
    if (ev->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        ESP_LOGW(TAG, "serial port disconnected");
        xSemaphoreTake(s_cdcMtx, portMAX_DELAY);
        if (s_gps == ev->data.cdc_hdl) s_gps = nullptr;
        if (s_cdc == ev->data.cdc_hdl) { s_cdc = nullptr; s_app->usbSerial = false; }
        xSemaphoreGive(s_cdcMtx);
        cdc_acm_host_close(ev->data.cdc_hdl);
    }
}

static bool civWrite(const uint8_t* d, size_t n) {
    bool ok = false;
    xSemaphoreTake(s_cdcMtx, portMAX_DELAY);
    if (s_cdc) ok = cdc_acm_host_data_tx_blocking(s_cdc, d, n, 100) == ESP_OK;
    xSemaphoreGive(s_cdcMtx);
    return ok;
}

static bool cp210xSetup(cdc_acm_dev_hdl_t h, uint8_t iface, uint32_t baud) {
    if (cdc_acm_host_send_custom_request(h, CP210X_REQ_OUT, CP210X_IFC_ENABLE, 1, iface, 0, nullptr) != ESP_OK) return false;
    if (cdc_acm_host_send_custom_request(h, CP210X_REQ_OUT, CP210X_SET_BAUDRATE, 0, iface, 4, (uint8_t*)&baud) != ESP_OK) return false;
    if (cdc_acm_host_send_custom_request(h, CP210X_REQ_OUT, CP210X_SET_LINE_CTL, 0x0800, iface, 0, nullptr) != ESP_OK) return false;   // 8N1
    // DTR and RTS held low: on Icom radios these lines can be mapped to PTT / CW keying.
    cdc_acm_host_send_custom_request(h, CP210X_REQ_OUT, CP210X_SET_MHS, 0x0300, iface, 0, nullptr);
    return true;
}

// CH340 / CH341: serial init, then the baud divisor (registers 0x12/0x13) and 8N1 (registers 0x18/0x25), as the
// Linux ch341 driver does; DTR and RTS low.
static bool ch34xSetup(cdc_acm_dev_hdl_t h, uint32_t baud) {
    if (baud < 300) baud = 300;
    uint32_t factor = 1532620800u / baud;
    uint16_t divisor = 3;
    while (factor > 0xFFF0 && divisor) { factor >>= 3; divisor--; }
    if (factor > 0xFFF0) return false;
    factor = 0x10000 - factor;
    uint16_t a = (uint16_t)((factor & 0xFF00) | divisor | 0x80);
    if (cdc_acm_host_send_custom_request(h, CH34X_REQ_OUT, CH34X_REQ_SERIAL_INIT, 0, 0, 0, nullptr) != ESP_OK) return false;
    if (cdc_acm_host_send_custom_request(h, CH34X_REQ_OUT, CH34X_REQ_WRITE_REG, 0x1312, a, 0, nullptr) != ESP_OK) return false;
    if (cdc_acm_host_send_custom_request(h, CH34X_REQ_OUT, CH34X_REQ_WRITE_REG, 0x2518, 0x00C3, 0, nullptr) != ESP_OK) return false;
    cdc_acm_host_send_custom_request(h, CH34X_REQ_OUT, CH34X_REQ_MODEM_CTRL, 0xFF, 0, 0, nullptr);
    return true;
}

// PTT on a serial port's RTS or DTR line (Settings > PTT), e.g. a Digirig: RTS keys the radio.
static void setPttLine(bool on) {
    const int m = s_app->radio.pttMethod.load();
    const bool rts = m == PTT_RTS && on, dtr = m == PTT_DTR && on;
    xSemaphoreTake(s_cdcMtx, portMAX_DELAY);
    if (s_cdc) {
        esp_err_t e;
        if (s_civKind == PK_CP210X)       // SET_MHS: bit 0 DTR, bit 1 RTS, bits 8/9 = write those two
            e = cdc_acm_host_send_custom_request(s_cdc, CP210X_REQ_OUT, CP210X_SET_MHS, (uint16_t)(0x0300 | (dtr ? 1 : 0) | (rts ? 2 : 0)), s_civIface, 0, nullptr);
        else if (s_civKind == PK_CH34X)
            e = cdc_acm_host_send_custom_request(s_cdc, CH34X_REQ_OUT, CH34X_REQ_MODEM_CTRL, (uint16_t)(~((dtr ? 0x20 : 0) | (rts ? 0x40 : 0)) & 0xFF), 0, 0, nullptr);
        else
            e = cdc_acm_host_set_control_line_state(s_cdc, dtr, rts);
        if (e != ESP_OK) ESP_LOGW(TAG, "PTT line could not be set (%s)", esp_err_to_name(e));
    } else if (on) ESP_LOGW(TAG, "PTT by %s, but no serial port is connected", m == PTT_RTS ? "RTS" : "DTR");
    xSemaphoreGive(s_cdcMtx);
}

// Opens one serial port and sets it to `baud`, 8N1, DTR and RTS low. Returns nullptr if the
// device at that address is not of this kind or the interface is not a serial port.
static cdc_acm_dev_hdl_t openPort(uint16_t vid, PortKind kind, uint8_t dev, uint8_t iface, uint32_t baud, cdc_acm_data_callback_t cb) {
    cdc_acm_host_open_config_t oc = {};
    oc.vid = vid;
    oc.pid = CDC_HOST_ANY_PID;
    oc.interface_idx = iface;
    oc.dev_addr = dev;
    oc.connection_timeout_ms = 100;
    oc.out_buffer_size = 64;
    oc.in_buffer_size = 64;
    oc.event_cb = cdcEvent;
    oc.data_cb = cb;
    cdc_acm_dev_hdl_t h = nullptr;
    if (cdc_acm_host_open(&oc, &h) != ESP_OK) return nullptr;
    if (kind == PK_CP210X) {
        if (!cp210xSetup(h, iface, baud)) { cdc_acm_host_close(h); return nullptr; }
    } else if (kind == PK_CH34X) {
        if (!ch34xSetup(h, baud)) { cdc_acm_host_close(h); return nullptr; }
    } else {
        // A USB-native port has no real baud rate, but set the line state anyway. Errors are not
        // fatal: some devices do not implement these requests.
        cdc_acm_line_coding_t lc = {};
        lc.dwDTERate = baud;
        lc.bCharFormat = 0;        // 1 stop bit
        lc.bParityType = 0;        // none
        lc.bDataBits = 8;
        cdc_acm_host_line_coding_set(h, &lc);
        cdc_acm_host_set_control_line_state(h, false, false);
    }
    return h;
}

// GPS sentences arrive on the radio's other serial port:
//  IC-7100: the second CP210x bridge (a different USB address), from a receiver on DATA1.
//  IC-705:  port "B" of the same device, from the radio's built-in GPS.
static void openGpsPort(const uint8_t* addrs, int n) {
    if (s_app->radio.proto != RP_CIV) return;                    // only the Icoms pass GPS data to USB
    if (s_civKind == PK_CH34X) return;                           // a CI-V cable: no GPS port
    if (s_civKind == PK_CP210X) {
        for (int i = 0; i < n && !s_gps; i++) {
            if (addrs[i] == s_civDev) continue;
            s_gps = openPort(SILABS_VID, PK_CP210X, addrs[i], 0, (uint32_t)s_app->cfg.gpsBaud, gpsRx);
            if (s_gps) ESP_LOGI(TAG, "GPS port opened: USB address %d, %d baud", addrs[i], s_app->cfg.gpsBaud);
        }
    } else {
        for (uint8_t iface = 0; iface < 8 && !s_gps; iface++) {
            if (iface == s_civIface || iface == s_civIface + 1) continue;      // the CI-V port's own two interfaces
            s_gps = openPort(s_civVid, PK_CDC, s_civDev, iface, (uint32_t)s_app->cfg.gpsBaud, gpsRx);
            if (s_gps) ESP_LOGI(TAG, "GPS port opened: USB address %d, interface %d", s_civDev, iface);
        }
    }
}

// Serial ports of one kind look identical on the bus, and a CP210x can belong to an Icom or a
// Yaesu. Open each port in turn and try every radio that could be behind it: Icom CI-V at the
// configured address and at the factory addresses, Yaesu CAT ("ID;" answered with four digits),
// QMX CAT (three digits). The first one that answers the control task's polls is kept, so a
// different radio can be plugged in without changing settings. Settings > Radio type narrows the
// search to one kind.
struct Probe { int proto; uint8_t addr; uint32_t baud; };

static int buildProbes(uint8_t protos, Probe* out) {
    int n = 0;
    const int type = s_app->cfg.radioType;                       // 0 = any
    const uint8_t configured = (uint8_t)s_app->cfg.civAddr;
    if ((protos & (1 << RP_CIV)) && (type == 0 || type == 1)) {
        const uint32_t baud = (uint32_t)s_app->cfg.civBaud;
        out[n++] = {RP_CIV, configured, baud};
        // factory addresses: IC-7100, IC-705, IC-7300, IC-9700, IC-7610, IC-905
        static const uint8_t kAddrs[] = {0x88, 0xA4, 0x94, 0xA2, 0x98, 0xAC};
        for (uint8_t a : kAddrs) if (a != configured) out[n++] = {RP_CIV, a, baud};
    }
    // Text CAT over a CP210x: the configured rate first, then the usual ones (FTDX10 / FT-991A /
    // K2 default 4800, TS-2000 9600, FTX-1 38400, FX-4CR 115200). A USB-native port has no rate: one try.
    uint32_t bauds[6] = {(uint32_t)s_app->cfg.catBaud, 4800, 9600, 19200, 38400, 115200};
    int nb = 1;
    for (int i = 1; i < 6; i++) if (bauds[i] != bauds[0]) bauds[nb++] = bauds[i];
    if ((protos & (1 << RP_YAESU)) && (type == 0 || type == 2)) for (int i = 0; i < nb; i++) out[n++] = {RP_YAESU, 0, bauds[i]};
    if ((protos & (1 << RP_KENWOOD)) && (type == 0 || type == 4)) for (int i = 0; i < nb; i++) out[n++] = {RP_KENWOOD, 0, bauds[i]};
    if ((protos & (1 << RP_QMX)) && (type == 0 || type == 3)) out[n++] = {RP_QMX, 0, 115200};
    return n;
}

// Before a planned restart (update, Restart button): close the serial port and switch the USB port off, so the
// radio sees the host go away cleanly instead of in the middle of an audio transfer.
void usbradio_stop_for_restart() {
    xSemaphoreTake(s_cdcMtx, portMAX_DELAY);
    cdc_acm_dev_hdl_t c = s_cdc, g = s_gps;
    s_cdc = nullptr; s_gps = nullptr;
    xSemaphoreGive(s_cdcMtx);
    if (g) cdc_acm_host_close(g);
    if (c) cdc_acm_host_close(c);
    usb_host_lib_set_root_port_power(false);
    ESP_LOGI(TAG, "USB port switched off for the restart");
    vTaskDelay(pdMS_TO_TICKS(400));
}

static void serialTask(void*) {
    int gpsFails = 0;
    int64_t gpsNextUs = 0, gpsOpenedUs = 0, gpsFixAt = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint8_t addrs[12];
        int n = 0;
        // Settings > Radio "No CAT" (SignaLink, or a Digirig without a CAT cable): no radio to find.
        // The frequency is the one the band buttons set; a serial port, if there is one, is opened
        // only for PTT on RTS / DTR.
        Radio& rad = s_app->radio;
        if (s_app->cfg.radioType == 5) {
            if (rad.proto != RP_NONE) {
                xSemaphoreTake(s_cdcMtx, portMAX_DELAY);
                cdc_acm_dev_hdl_t old = s_cdc;
                s_cdc = nullptr;
                xSemaphoreGive(s_cdcMtx);
                if (old) cdc_acm_host_close(old);
                rad.setProto(RP_NONE);
                rad.freq = (uint64_t)s_app->cfg.dialHz;
                rad.mode = s_app->cfg.dialMode;
                rad.dataMode = s_app->cfg.dialData;
                s_app->usbSerial = true;
                ESP_LOGI(TAG, "no CAT: dial %.3f MHz, PTT by %s", s_app->cfg.dialHz / 1e6,
                         rad.pttMethod == PTT_RTS ? "RTS" : rad.pttMethod == PTT_DTR ? "DTR" : "VOX (the sound card keys the radio)");
            }
            s_app->usbSerial = true;                                 // also after a PTT port came and went
            const int m = rad.pttMethod.load();
            if (!s_cdc && (m == PTT_RTS || m == PTT_DTR) && usb_host_device_addr_list_fill(sizeof addrs, addrs, &n) == ESP_OK) {
                for (int i = 0; i < n && !s_cdc; i++)
                    for (const auto& pt : kPortTypes)
                        for (uint8_t iface = 0; iface < pt.ifaces && !s_cdc; iface++) {
                            cdc_acm_dev_hdl_t h = openPort(pt.vid, pt.kind, addrs[i], iface, 9600, ignoreRx);
                            if (!h) continue;
                            xSemaphoreTake(s_cdcMtx, portMAX_DELAY);
                            s_cdc = h; s_civDev = addrs[i]; s_civIface = iface; s_civKind = pt.kind; s_civVid = pt.vid;
                            xSemaphoreGive(s_cdcMtx);
                            ESP_LOGI(TAG, "serial port for PTT: USB address %d, interface %d", addrs[i], iface);
                        }
            }
            continue;
        }
        if (rad.proto == RP_NONE) {                                  // switched back from "No CAT"
            xSemaphoreTake(s_cdcMtx, portMAX_DELAY);
            cdc_acm_dev_hdl_t old = s_cdc;
            s_cdc = nullptr;
            xSemaphoreGive(s_cdcMtx);
            if (old) cdc_acm_host_close(old);
            s_app->usbSerial = false;
            rad.setProto(RP_CIV);
            rad.addr = (uint8_t)s_app->cfg.civAddr;
        }
        if (s_cdc) {
            // The radio's GPS port is only open while it is needed: until it has set the clock (plus
            // half a minute of averaging), then it is closed again and reopened an hour later for a
            // fresh sync. With internet time (NTP) it is not opened at all. Kept open, it holds USB
            // and internal memory the audio streams and the microSD card also need.
            const int64_t nowUs = esp_timer_get_time();
            if (s_gps) {
                const int64_t openMs = (nowUs - gpsOpenedUs) / 1000;
                const int64_t sinceFix = clk::monoMs() - s_app->gpsLastMono.load();
                bool synced = gpsFixAt == 0 && sinceFix < 2000 && openMs > 2000;
                if (synced) gpsFixAt = nowUs;
                const char* why = nullptr;
                if (s_app->ntpSynced) why = "internet time (NTP) is in use";
                else if (gpsFixAt && nowUs - gpsFixAt > 30 * 1000000LL) why = "clock set";
                else if (!gpsFixAt && openMs > 180000) why = "no time from it in 3 minutes";
                if (why) {
                    xSemaphoreTake(s_cdcMtx, portMAX_DELAY);
                    cdc_acm_dev_hdl_t g = s_gps;
                    s_gps = nullptr;
                    xSemaphoreGive(s_cdcMtx);
                    if (g) cdc_acm_host_close(g);
                    int mins = gpsFixAt ? 60 : 10;
                    gpsNextUs = nowUs + (int64_t)mins * 60 * 1000000;
                    ESP_LOGI(TAG, "radio's GPS port closed (%s); next sync in %d min", why, mins);
                }
                continue;
            }
            // The GPS port: when it cannot be opened (on the IC-705 the USB host often has no endpoint
            // memory left next to CI-V and audio), back off instead of retrying every second; each
            // failed attempt costs memory and floods the log.
            if (s_app->usbSerial && s_app->cfg.gpsClock && !s_app->ntpSynced && !s_gps && gpsFails < 3 && nowUs >= gpsNextUs &&
                usb_host_device_addr_list_fill(sizeof addrs, addrs, &n) == ESP_OK) {
                openGpsPort(addrs, n);
                if (s_gps) { gpsFails = 0; gpsOpenedUs = esp_timer_get_time(); gpsFixAt = 0; }
                else {
                    // Each failed open can leave USB host memory behind that the audio streams need when
                    // they restart, so give up after 3 tries until the radio is plugged in again.
                    static const int kWaitS[] = {5, 30, 120};
                    int w = kWaitS[gpsFails < 2 ? gpsFails : 2];
                    gpsFails++;
                    gpsNextUs = esp_timer_get_time() + (int64_t)w * 1000000;
                    if (gpsFails < 3) ESP_LOGW(TAG, "radio's GPS port could not be opened (attempt %d); next try in %d s", gpsFails, w);
                    else ESP_LOGW(TAG, "radio's GPS port could not be opened; not trying again until the radio is reconnected");
                }
            }
            continue;
        }
        gpsFails = 0;
        gpsNextUs = 0;
        gpsFixAt = 0;
        if (usb_host_device_addr_list_fill(sizeof addrs, addrs, &n) != ESP_OK) continue;
        // A device that failed to start (a QMX+ after a RigDeck restart answers with a short device descriptor):
        // switch the port off and on once; if it still fails, tell the operator. On the QMX+ only switching the
        // radio off and on brings it back (unplugging the cable or restarting RigDeck does not).
        {
            extern std::atomic<int> g_usbEnumErrors;
            static int seen = 0, cycles = 0;
            static int64_t errAt = 0;
            int e = g_usbEnumErrors.load();
            if (e != seen) { seen = e; errAt = esp_timer_get_time(); }
            if (errAt && esp_timer_get_time() - errAt > 3000000 && !s_app->usbSerial && !s_app->usbAudio) {
                errAt = 0;
                if (cycles < 1) {
                    cycles++;
                    ESP_LOGW(TAG, "a USB device did not start properly: switching the USB port off and on once");
                    usb_host_lib_set_root_port_power(false);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    usb_host_lib_set_root_port_power(true);
                    continue;
                }
                ESP_LOGW(TAG, "the device on USB still does not start: switch the radio off and on");
                s_app->usbProblem("The radio on USB is not answering properly: switch the radio off and on");
            }
            if (s_app->usbSerial || s_app->usbAudio) { cycles = 0; s_app->usbProblem(""); }
        }
        Radio& radio = s_app->radio;
        for (int i = 0; i < n && !s_app->usbSerial; i++) {
            for (const auto& pt : kPortTypes) {
                Probe probes[24];
                const int np = buildProbes(pt.protos, probes);
                for (uint8_t iface = 0; np && iface < pt.ifaces && !s_app->usbSerial; iface++) {
                    cdc_acm_dev_hdl_t h = openPort(pt.vid, pt.kind, addrs[i], iface, probes[0].baud, cdcRx);
                    if (!h) continue;
                    xSemaphoreTake(s_cdcMtx, portMAX_DELAY);
                    s_cdc = h;
                    xSemaphoreGive(s_cdcMtx);
                    for (int k = 0; k < np && s_cdc == h && !s_app->usbSerial; k++) {
                        const Probe& pr = probes[k];
                        if (k > 0 && pr.baud != probes[k - 1].baud && pt.kind == PK_CP210X) cp210xSetup(h, iface, pr.baud);
                        if (k > 0 && pr.baud != probes[k - 1].baud && pt.kind == PK_CH34X) ch34xSetup(h, pr.baud);
                        radio.setProto(pr.proto);
                        radio.addr = pr.addr ? pr.addr : (uint8_t)s_app->cfg.civAddr;
                        radio.lastRxMono = -100000;
                        if (pr.proto != RP_CIV) radio.requestId();
                        vTaskDelay(pdMS_TO_TICKS(k == 0 ? 1500 : 900));     // the control task is polling the radio meanwhile
                        if (s_cdc == h && radio.connected()) {
                            if (pr.proto == RP_CIV) {
                                ESP_LOGI(TAG, "CI-V port found: USB address %d, interface %d, CI-V address %02Xh (%s)",
                                         addrs[i], iface, pr.addr, civModelName(pr.addr));
                                s_app->cfg.civAddr = pr.addr;              // remembered until restart; Settings can save it
                            } else {
                                ESP_LOGI(TAG, "CAT port found: USB address %d, interface %d (%s)", addrs[i], iface, radio.modelName());
                            }
                            s_civDev = addrs[i];
                            s_civIface = iface;
                            s_civKind = pt.kind;
                            s_civVid = pt.vid;
                            s_app->usbSerial = true;
                        }
                    }
                    if (s_cdc == h && !s_app->usbSerial) {
                        xSemaphoreTake(s_cdcMtx, portMAX_DELAY);
                        s_cdc = nullptr;
                        xSemaphoreGive(s_cdcMtx);
                        cdc_acm_host_close(h);
                    }
                }
            }
        }
        if (!s_app->usbSerial && radio.proto != RP_CIV) {          // nothing found: back to the default for the status display
            radio.setProto(RP_CIV);
            radio.addr = (uint8_t)s_app->cfg.civAddr;
        }
    }
}

// ---------------------------------------------------------------------------- audio codec (UAC 1)
struct AudioEv { uint8_t kind, addr, iface; uac_host_device_handle_t h; };   // kind: 0 rx connected, 1 tx connected, 2 gone, 3 transfer error
static QueueHandle_t s_evq;
static uac_host_device_handle_t s_mic = nullptr, s_spk = nullptr;
static uac_host_stream_config_t s_micCfg, s_spkCfg;
static bool s_micRun = false, s_spkRun = false;

static void uacDriverCb(uint8_t addr, uint8_t iface, const uac_host_driver_event_t ev, void*) {
    AudioEv e = {(uint8_t)(ev == UAC_HOST_DRIVER_EVENT_TX_CONNECTED ? 1 : 0), addr, iface, nullptr};
    xQueueSend(s_evq, &e, 0);
}

static void uacDeviceCb(uac_host_device_handle_t h, const uac_host_device_event_t ev, void*) {
    if (ev == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
        AudioEv e = {2, 0, 0, h};
        xQueueSend(s_evq, &e, 0);
    } else if (ev == UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR) {
        // The UAC driver does not resubmit a failed transfer, so each error leaves the stream with one
        // transfer fewer until it goes silent. The audio task restarts the stream instead.
        AudioEv e = {3, 0, 0, h};
        xQueueSend(s_evq, &e, 0);
    }
}

// Pick a 48 kHz format: 16-bit where offered (the codecs in the Icoms and the FTX-1), else 24-bit
// (QMX).
static bool pickFormatBits(uac_host_device_handle_t h, uac_host_stream_config_t& out, int bits) {
    uac_host_dev_info_t info;
    if (uac_host_get_device_info(h, &info) != ESP_OK) return false;
    for (uint8_t alt = 1; alt <= info.iface_alt_num; alt++) {
        uac_host_dev_alt_param_t p;
        if (uac_host_get_device_alt_param(h, alt, &p) != ESP_OK || p.bit_resolution != bits) continue;
        bool ok = false;
        if (p.sample_freq_type > 0) {
            for (int i = 0; i < p.sample_freq_type && i < UAC_FREQ_NUM_MAX; i++) ok |= p.sample_freq[i] == 48000;
        } else ok = p.sample_freq_lower <= 48000 && p.sample_freq_upper >= 48000;
        if (!ok) continue;
        out = {};
        out.channels = p.channels;
        out.bit_resolution = (uint8_t)bits;
        out.sample_freq = 48000;
        return true;
    }
    return false;
}
static bool pickFormat(uac_host_device_handle_t h, uac_host_stream_config_t& out) {
    return pickFormatBits(h, out, 16) || pickFormatBits(h, out, 24);
}

static int s_restarts = 0;
static void restartStream(uac_host_device_handle_t h, const char* why) {
    s_restarts++;
    if (h == s_mic && s_micRun) { uac_host_device_stop(s_mic); s_micRun = false; }
    if (h == s_spk && s_spkRun) { uac_host_device_stop(s_spk); s_spkRun = false; }
    if (s_restarts <= 10 || s_restarts % 50 == 0)
        ESP_LOGW(TAG, "audio %s: %s, restarting the stream (%d so far)", h == s_mic ? "from radio" : "to radio", why, s_restarts);
}

static void handleEvent(const AudioEv& e) {
    if (e.kind == 3) {
        if (e.h == s_mic || e.h == s_spk) restartStream(e.h, "USB transfer error");
        return;
    }
    if (e.kind == 2) {
        if (e.h == s_mic) { s_mic = nullptr; s_micRun = false; }
        if (e.h == s_spk) { s_spk = nullptr; s_spkRun = false; }
        uac_host_device_close(e.h);
        ESP_LOGW(TAG, "audio interface disconnected");
        return;
    }
    bool tx = e.kind == 1;
    uac_host_device_config_t dc = {};
    dc.addr = e.addr;
    dc.iface_num = e.iface;
    // Ring buffers come from internal RAM, which the USB host, Wi-Fi link and display also need:
    // 100 ms / 150 ms of 16-bit stereo (about 67 / 100 ms at 24 bits); the audio task empties the
    // receive buffer every 10 ms.
    dc.buffer_size = tx ? 19200 : 28800;
    dc.buffer_threshold = dc.buffer_size / 4;
    dc.callback = uacDeviceCb;
    uac_host_device_handle_t h = nullptr;
    if (uac_host_device_open(&dc, &h) != ESP_OK) { ESP_LOGE(TAG, "cannot open audio interface %d", e.iface); return; }
    uac_host_stream_config_t fmt;
    if (!pickFormat(h, fmt)) {
        ESP_LOGE(TAG, "audio interface %d has no 48 kHz 16- or 24-bit format", e.iface);
        uac_host_device_close(h);
        return;
    }
    ESP_LOGI(TAG, "audio %s: interface %d, %d channel(s), %d-bit, 48 kHz", tx ? "to radio" : "from radio", e.iface, fmt.channels, fmt.bit_resolution);
    if (tx) { s_spk = h; s_spkCfg = fmt; } else { s_mic = h; s_micCfg = fmt; }
}

// A stream that will not start (usually: internal RAM too short for the USB transfer buffers) is
// retried after a pause, not in a tight loop, and reported once with the memory figures.
static int64_t s_startRetryMs = 0;
static int s_startFails = 0;
static bool startStream(uac_host_device_handle_t h, const uac_host_stream_config_t* cfg, const char* what) {
    int64_t now = esp_timer_get_time() / 1000;
    if (now < s_startRetryMs) { vTaskDelay(pdMS_TO_TICKS(20)); return false; }
    esp_err_t e = uac_host_device_start(h, (uac_host_stream_config_t*)cfg);
    if (e == ESP_OK) {
        if (s_startFails) ESP_LOGI(TAG, "audio %s started after %d failed attempt(s)", what, s_startFails);
        s_startFails = 0;
        return true;
    }
    s_startFails++;
    int pauseMs = s_startFails < 3 ? 250 : s_startFails < 10 ? 1000 : 5000;
    s_startRetryMs = now + pauseMs;
    if (s_startFails == 1 || s_startFails % 20 == 0)
        ESP_LOGE(TAG, "audio %s will not start (%s), attempt %d; internal RAM free %u kB, largest block %u kB",
                 what, esp_err_to_name(e), s_startFails,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    // About 20 s without the stream (it ran before, so the radio is there): what the operator would
    // do by hand - restart RigDeck - but not while transmitting and not right after a start.
    if (s_startFails == 12 && esp_timer_get_time() > 120LL * 1000000 && !s_app->txActive()) {
        s_app->note(std::string("Audio ") + what + " would not restart (USB memory); restarting RigDeck");
        vTaskDelay(pdMS_TO_TICKS(3000));
        s_app->rebootReq = true;
    }
    return false;
}

// Both streams are started once and then left running: receive audio is read all the time (and
// ignored while transmitting), and the transmit stream carries silence while receiving. Stopping
// and starting them at every transmission cost internal RAM each time (about 2 kB per cycle on the
// IC-705) until the next start failed and the radio keyed up with no audio.
// This task also runs the receive DSP (ft8_lib's FFT keeps ~31 kB of work arrays on the stack).
static void audioTask(void*) {
    static int16_t mono[960], txMono[480];
    static uint8_t raw[960 * 6], out[480 * 6];        // 20 ms in / 10 ms out of stereo at up to 24 bits
    for (;;) {
        AudioEv e;
        while (xQueueReceive(s_evq, &e, 0) == pdTRUE) handleEvent(e);
        if (!s_mic || !s_spk) {
            s_app->usbAudio = false;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        const int mch = s_micCfg.channels, mbps = s_micCfg.bit_resolution / 8;
        const int sch = s_spkCfg.channels, sbps = s_spkCfg.bit_resolution / 8;
        if (mch * mbps > 6 || sch * sbps > 6) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }   // more than stereo 24-bit
        if (!s_micRun) {
            if (!startStream(s_mic, &s_micCfg, "from radio")) continue;
            s_micRun = true;
        }
        if (!s_spkRun) {
            if (!startStream(s_spk, &s_spkCfg, "to radio")) continue;
            uac_host_device_set_mute(s_spk, false);      // not every codec has these controls: ignore errors
            uac_host_device_set_volume(s_spk, 100);
            s_spkRun = true;
        }
        s_app->usbAudio = true;
        // 10 ms from the radio (the read paces this loop) ...
        static int64_t lastRx = 0;
        int64_t nowMs = esp_timer_get_time() / 1000;
        if (lastRx == 0) lastRx = nowMs;
        if (nowMs - lastRx > 1000) {                     // nothing from the radio for a second: the stream stalled
            lastRx = nowMs;
            restartStream(s_mic, "no audio for 1 s");
            continue;
        }
        uint32_t got = 0;
        if (uac_host_device_read(s_mic, raw, 480 * mch * mbps, &got, 40) == ESP_OK && got > 0) {
            lastRx = nowMs;
            int frames = got / (mch * mbps);
            for (int i = 0; i < frames; i++) {           // left channel, top 16 bits
                const uint8_t* in = raw + (size_t)i * mch * mbps + (mbps - 2);
                mono[i] = (int16_t)(in[0] | (in[1] << 8));
            }
            if (!s_app->txActive()) s_app->rxAudio48k(mono, frames);
        }
        // ... and 10 ms to it: the transmit signal, or silence
        const int frames = 480;
        if (s_app->txActive()) s_app->txAudio48k(txMono, frames);
        else memset(txMono, 0, sizeof txMono);
        uint8_t* o = out;
        for (int i = 0; i < frames; i++) for (int c = 0; c < sch; c++) {
            if (sbps == 3) *o++ = 0;
            *o++ = (uint8_t)(txMono[i] & 0xFF);
            *o++ = (uint8_t)((uint16_t)txMono[i] >> 8);
        }
        static int writeFails = 0;
        if (uac_host_device_write(s_spk, out, frames * sch * sbps, 20) == ESP_OK) writeFails = 0;
        else if (++writeFails >= 25) {                   // a quarter second the radio would not take: restart
            writeFails = 0;
            restartStream(s_spk, "transmit buffer stuck");
        }
    }
}

// ---------------------------------------------------------------------------- host stack
static void usbLibTask(void* arg) {
    usb_host_config_t cfg = {};
    cfg.intr_flags = ESP_INTR_FLAG_LOWMED;
    // The radio's serial and audio chips are full-speed devices behind its hub. A high-speed host
    // (ESP32-P4) cannot reach them through a high-speed hub, so run the port at full speed.
    cfg.fsls_only = true;
    esp_err_t err = usb_host_install(&cfg);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "full-speed-only mode not available in this ESP-IDF; continuing without it");
        cfg.fsls_only = false;
        err = usb_host_install(&cfg);
    }
    ESP_ERROR_CHECK(err);
    xTaskNotifyGive((TaskHandle_t)arg);
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

void usbradio_start(App& app) {
    s_app = &app;
    s_cdcMtx = xSemaphoreCreateMutex();
    s_app->radio.pttLine = setPttLine;
    s_evq = xQueueCreate(8, sizeof(AudioEv));
    app.radio.write = civWrite;

    xTaskCreatePinnedToCore(usbLibTask, "usb_lib", 4096, xTaskGetCurrentTaskHandle(), 10, nullptr, 0);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    cdc_acm_host_driver_config_t cdc = {};
    cdc.driver_task_stack_size = 4096;
    cdc.driver_task_priority = 10;
    cdc.xCoreID = 0;
    ESP_ERROR_CHECK(cdc_acm_host_install(&cdc));

    uac_host_driver_config_t uac = {};
    uac.create_background_task = true;
    uac.task_priority = 10;
    uac.stack_size = 4096;
    uac.core_id = 0;
    uac.callback = uacDriverCb;
    ESP_ERROR_CHECK(uac_host_install(&uac));

    xTaskCreatePinnedToCore(serialTask, "civ_port", 4096, nullptr, 4, nullptr, 0);
    xTaskCreatePinnedToCore(audioTask, "audio", 49152, nullptr, 9, nullptr, 1);
}
