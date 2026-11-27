// USB host side of the radio link. One USB cable to the radio carries everything.
//  IC-7100: the USB-B port is a hub with two CP210x serial bridges (one is CI-V) and a USB Audio
//           Class 1 codec.
//  IC-705:  the micro-USB port is one composite device: two CDC-ACM serial ports and USB audio.
//  FTX-1:   like the IC-7100: a hub, a CP2105 dual serial bridge (first port = CAT-1) and an audio codec.
//  QMX/QMX+: one composite device: CDC-ACM serial port(s) and 24-bit USB audio.
//  IC-7300 / IC-7610 / IC-9700, FTDX10 / FT-991A / FT-710: like the IC-7100 / FTX-1 (CP210x + codec).
//  Digirig: a hub with a CP2102 (CAT, PTT on RTS) and a CM108 sound card.
//  SignaLink USB: only a sound card; it keys the radio itself (VOX) - Settings > Radio "No CAT".
//  USB GPS (u-blox "G-Mouse" VK172 / VK-162, ESP32-P4 only): a CDC-ACM serial port sending NMEA. It is
//           not a radio, so the radio search never opens it; it is opened on its own and sets the clock
//           and position. Next to a radio it needs a hub (the board has one USB-A socket).
//
// Endpoint budget: an ESP32-S3 has only 8 host channels. Hub (2) + three control pipes (3) + CI-V
// bulk in/out (2) leaves exactly one for audio, so the receive and transmit audio streams are never
// open at the same time. FT8 is half duplex anyway. ESP32-P4 (16 channels) has room to spare.
#include "platform.h"
#include "clock.h"
#include <cstdint>
#include <cstring>
#include "esp_http_server.h"
#include <algorithm>
#include <cstdarg>
#include <string>
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
#define UBLOX_VID 0x1546               // u-blox GPS receivers (VK172 G-Mouse = u-blox 7, 1546:01A7)
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
static cdc_acm_dev_hdl_t s_usbGps = nullptr;      // a USB GPS receiver of its own
static uint8_t s_civDev = 0, s_civIface = 0;      // where the CI-V port was found
static PortKind s_civKind = PK_CP210X;
static uint16_t s_civVid = 0;
static SemaphoreHandle_t s_cdcMtx;

static void wireNote(char dir, const uint8_t* d, size_t n);

static bool cdcRx(const uint8_t* data, size_t len, void*) {
    wireNote('<', data, len);
    s_app->radio.onRx(data, len);
    return true;
}

static bool ignoreRx(const uint8_t*, size_t, void*) { return true; }   // a port used only for its PTT line

static bool gpsRx(const uint8_t* data, size_t len, void*) {
    s_app->gpsRx(data, len);
    return true;
}

static bool usbGpsRx(const uint8_t* data, size_t len, void*) {
    s_app->usbGpsRx(data, len);
    return true;
}

static void cdcEvent(const cdc_acm_host_dev_event_data_t* ev, void*) {
    if (ev->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        ESP_LOGW(TAG, "serial port disconnected");
        xSemaphoreTake(s_cdcMtx, portMAX_DELAY);
        if (s_gps == ev->data.cdc_hdl) s_gps = nullptr;
        if (s_usbGps == ev->data.cdc_hdl) { s_usbGps = nullptr; ESP_LOGW(TAG, "USB GPS unplugged"); }
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
    wireNote(ok ? '>' : 'x', d, n);                  // x: not sent (no port, or it would not take it)
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
    cdc_acm_dev_hdl_t c = s_cdc, g = s_gps, u = s_usbGps;
    s_cdc = nullptr; s_gps = nullptr; s_usbGps = nullptr;
    xSemaphoreGive(s_cdcMtx);
    if (u) cdc_acm_host_close(u);
    if (g) cdc_acm_host_close(g);
    if (c) cdc_acm_host_close(c);
    usb_host_lib_set_root_port_power(false);
    ESP_LOGI(TAG, "USB port switched off for the restart");
    vTaskDelay(pdMS_TO_TICKS(400));
}

// USB GPS: each device that appears on the bus is checked (up to three times) for a u-blox serial port.
// One that is not a u-blox (the radio's chips) is then left alone until it is unplugged and comes back. The
// receiver stays open: on the ESP32-P4 the host has endpoints and memory to spare. Not on the ESP32-S3,
// whose 8 host channels the radio already uses up.
static void pollUsbGps() {
#if CONFIG_IDF_TARGET_ESP32P4
    static uint8_t checked[16], tries[16];             // address, and how often it was asked
    static int nChecked = 0;
    uint8_t addrs[16];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof addrs, addrs, &n) != ESP_OK) return;
    int keep = 0;                                  // forget addresses no longer on the bus
    for (int i = 0; i < nChecked; i++)
        for (int j = 0; j < n; j++) if (checked[i] == addrs[j]) { tries[keep] = tries[i]; checked[keep++] = checked[i]; break; }
    nChecked = keep;
    if (s_usbGps) return;
    for (int j = 0; j < n && !s_usbGps; j++) {
        // three tries per device: a receiver that was still starting up gets another chance
        int k = 0;
        while (k < nChecked && checked[k] != addrs[j]) k++;
        if (k == nChecked) {
            if (nChecked == (int)sizeof checked) continue;
            checked[nChecked] = addrs[j]; tries[nChecked++] = 0;
        }
        if (tries[k] >= 3) continue;
        tries[k]++;
        // u-blox receivers ignore the line coding (USB-native port); 9600 is their UART default.
        cdc_acm_dev_hdl_t h = openPort(UBLOX_VID, PK_CDC, addrs[j], 0, 9600, usbGpsRx);
        if (!h) continue;
        xSemaphoreTake(s_cdcMtx, portMAX_DELAY);
        s_usbGps = h;
        xSemaphoreGive(s_cdcMtx);
        ESP_LOGI(TAG, "USB GPS found: USB address %d; it sets the clock and position once it has a fix", addrs[j]);
    }
#endif
}

static void serialTask(void*) {
    int gpsFails = 0;
    int64_t gpsNextUs = 0, gpsOpenedUs = 0, gpsFixAt = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        pollUsbGps();
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
static uint8_t s_spkAddr = 0;                         // USB address of the radio's sound chip (playback)
static SemaphoreHandle_t s_rawMtx;                     // one raw USB client at a time (diagnostics, direct volume)
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
    if (tx) { s_spk = h; s_spkCfg = fmt; s_spkAddr = e.addr; } else { s_mic = h; s_micCfg = fmt; }
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

// ---- USB control requests sent by RigDeck itself (sound chip volume, diagnostics): a short-lived USB host
// client of its own, so the audio driver's state is not touched.
struct RawCtl { usb_host_client_handle_t cl; SemaphoreHandle_t done; };
static void rawClientCb(const usb_host_client_event_msg_t*, void*) {}
static void rawXferCb(usb_transfer_t* t) { xSemaphoreGive(((RawCtl*)t->context)->done); }

// One control request on endpoint 0. Returns the bytes of data moved, or -1.
static int rawControl(RawCtl& c, usb_device_handle_t dev, uint8_t reqType, uint8_t req, uint16_t value,
                      uint16_t index, uint8_t* data, uint16_t len) {
    usb_transfer_t* t = nullptr;
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 64, 0, &t) != ESP_OK) return -1;
    usb_setup_packet_t* sp = (usb_setup_packet_t*)t->data_buffer;
    sp->bmRequestType = reqType; sp->bRequest = req; sp->wValue = value; sp->wIndex = index; sp->wLength = len;
    if (!(reqType & 0x80) && len) memcpy(t->data_buffer + sizeof(usb_setup_packet_t), data, len);
    t->num_bytes = sizeof(usb_setup_packet_t) + len;
    t->device_handle = dev;
    t->bEndpointAddress = 0;
    t->callback = rawXferCb;
    t->context = &c;
    t->timeout_ms = 500;
    int got = -1;
    if (usb_host_transfer_submit_control(c.cl, t) == ESP_OK) {
        int64_t until = esp_timer_get_time() + 1000000;
        bool fin = false;
        while (!fin && esp_timer_get_time() < until) {
            usb_host_client_handle_events(c.cl, pdMS_TO_TICKS(10));
            fin = xSemaphoreTake(c.done, 0) == pdTRUE;
        }
        if (fin && t->status == USB_TRANSFER_STATUS_COMPLETED) {
            got = t->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
            if (got < 0) got = 0;
            if ((reqType & 0x80) && got) memcpy(data, t->data_buffer + sizeof(usb_setup_packet_t), got > len ? len : got);
        } else if (!fin) {
            ESP_LOGW(TAG, "audio to radio: volume request got no answer");
            return -1;                                   // the transfer is still owned by the host: not freed
        }
    }
    usb_host_transfer_free(t);
    return got;
}

// ============================================================================ diagnostics
// Kept in every build, for bringing up radios: no new firmware needed to look at what goes on.
//  - CAT trace: the last 96 CAT messages (sent '>', not sent 'x', received '<') stay in memory. At each TX start
//    those of the last 3 s go to the log, and everything sent or received is logged live from TX start + 4 s and
//    for 2 s after TX ends (or for as long as the "trace" lab command asks).
//  - TX start / end lines: radio, PTT method, dial, mode, data flag, sound chip.
//  - USB report (/api/usbreport, and in the log when the radio's sound chip connects): every USB device, its
//    configuration descriptor in hex, and the sound chip's audio controls (volume / mute / mixer) with their
//    current, minimum, maximum and step values.
//  - Lab (/api/lab, the Console section of the web page): send a raw CAT command and see the answer; read or set
//    any audio control of the sound chip.
struct WireEnt { int64_t ms; char dir; char t[72]; };
static WireEnt s_wire[96];
static int s_wireN = 0, s_wireNext = 0;
static portMUX_TYPE s_wireMux = portMUX_INITIALIZER_UNLOCKED;
static std::atomic<int64_t> s_liveUntilUs{0};

static int64_t nowMs() { return esp_timer_get_time() / 1000; }

static void wireText(const uint8_t* d, size_t n, char* t, size_t cap) {
    const bool text = s_app && s_app->radio.isText();
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t c = d[i];
        char tmp[6];
        int w;
        if (text && c >= 0x20 && c < 0x7F) { tmp[0] = (char)c; tmp[1] = 0; w = 1; }
        else if (text && (c == '\r' || c == '\n')) continue;
        else w = snprintf(tmp, sizeof tmp, text ? "<%02X>" : "%02X ", c);
        if (k + w + 4 >= cap) { memcpy(t + k, "...", 3); k += 3; break; }
        memcpy(t + k, tmp, w);
        k += w;
    }
    t[k] = 0;
}

static void wireNote(char dir, const uint8_t* d, size_t n) {
    if (!n) return;
    WireEnt e;
    e.ms = nowMs();
    e.dir = dir;
    wireText(d, n, e.t, sizeof e.t);
    portENTER_CRITICAL_SAFE(&s_wireMux);
    s_wire[s_wireNext] = e;
    s_wireNext = (s_wireNext + 1) % 96;
    if (s_wireN < 96) s_wireN++;
    portEXIT_CRITICAL_SAFE(&s_wireMux);
    if (esp_timer_get_time() < s_liveUntilUs.load()) ESP_LOGI(TAG, "cat %c %s", dir, e.t);
}

// The CAT messages since sinceMs (oldest first), at most max of them.
static int wireSince(int64_t sinceMs, WireEnt* out, int max) {
    int n = 0;
    portENTER_CRITICAL_SAFE(&s_wireMux);
    for (int k = 0; k < s_wireN; k++) {
        const WireEnt& e = s_wire[(s_wireNext - s_wireN + k + 96) % 96];
        if (e.ms >= sinceMs && n < max) out[n++] = e;
    }
    portEXIT_CRITICAL_SAFE(&s_wireMux);
    return n;
}

static void pttTraceHook(bool on) {
    static int last = -1;
    if ((int)on == last) return;
    last = on;
    Radio& r = s_app->radio;
    static const char* pm[] = {"CAT command", "serial RTS", "serial DTR", "VOX"};
    const int m = r.pttMethod.load();
    ESP_LOGI(TAG, "TX %s: radio %s (protocol %d), PTT by %s, dial %llu Hz, mode %d, data %d, sound chip %s",
             on ? "start" : "end", r.modelName(), r.proto.load(), m >= 0 && m < 4 ? pm[m] : "?",
             (unsigned long long)r.freq.load(), r.mode.load(), r.dataMode.load(), s_spk ? "open" : "none");
    if (on) {
        static WireEnt before[40];
        const int64_t t0 = nowMs();
        const int n = wireSince(t0 - 3000, before, 40);
        for (int k = 0; k < n; k++) ESP_LOGI(TAG, "cat before TX %5lld ms %c %s", (long long)(before[k].ms - t0), before[k].dir, before[k].t);
        s_liveUntilUs = esp_timer_get_time() + 4000000;
    } else {
        int64_t until = esp_timer_get_time() + 2000000;
        if (s_liveUntilUs.load() < until) s_liveUntilUs = until;
    }
}

// ---- audio control layout of a USB Audio (class 1) device
struct AcUnit { uint8_t id, kind, nSrc, src[8], nch; int at; bool usb; };
static int acUnits(const uint8_t* p, int total, AcUnit* u, int max, int& acIface) {
    int nu = 0, curIface = -1, curClass = -1, curSub = -1;
    acIface = -1;
    for (int i = 0; i + 2 <= total && p[i] >= 2 && i + p[i] <= total; i += p[i]) {
        const int L = p[i];
        if (p[i + 1] == 0x04 && L >= 9) { curIface = p[i + 2]; curClass = p[i + 5]; curSub = p[i + 6]; continue; }
        if (p[i + 1] != 0x24 || curClass != 1 || curSub != 1 || L < 4 || nu >= max) continue;
        acIface = curIface;
        AcUnit x = {};
        x.id = p[i + 3]; x.kind = p[i + 2]; x.at = i;
        switch (x.kind) {
            case 0x02: if (L >= 8) { x.usb = (p[i + 4] | (p[i + 5] << 8)) == 0x0101; x.nch = p[i + 7]; } break;
            case 0x03: if (L >= 8) { x.nSrc = 1; x.src[0] = p[i + 7]; } break;
            case 0x04: case 0x05:
                if (L >= 5) {
                    x.nSrc = p[i + 4] > 8 ? 8 : p[i + 4];
                    for (int k = 0; k < x.nSrc && 5 + k < L; k++) x.src[k] = p[i + 5 + k];
                    if (x.kind == 0x04 && 5 + p[i + 4] < L) x.nch = p[i + 5 + p[i + 4]];
                }
                break;
            case 0x06: if (L >= 7) { x.nSrc = 1; x.src[0] = p[i + 4]; } break;
            case 0x07: case 0x08:
                if (L >= 7) { x.nSrc = p[i + 6] > 8 ? 8 : p[i + 6]; for (int k = 0; k < x.nSrc && 7 + k < L; k++) x.src[k] = p[i + 7 + k]; }
                break;
            default: continue;
        }
        u[nu++] = x;
    }
    for (int pass = 0; pass < 4; pass++)                // feature / selector units: their source's channels
        for (int k = 0; k < nu; k++)
            if (!u[k].nch && (u[k].kind == 0x06 || u[k].kind == 0x05) && u[k].nSrc)
                for (int m = 0; m < nu; m++) if (u[m].id == u[k].src[0] && u[m].nch) { u[k].nch = u[m].nch; break; }
    return nu;
}

struct RawDev { RawCtl c; usb_device_handle_t dev; };
static bool rawOpen(RawDev& r, uint8_t addr, std::string* err) {
    r = {};
    usb_host_client_config_t cc = {};
    cc.is_synchronous = false;
    cc.max_num_event_msg = 3;
    cc.async.client_event_callback = rawClientCb;
    if (usb_host_client_register(&cc, &r.c.cl) != ESP_OK) { if (err) *err = "no USB client"; return false; }
    r.c.done = xSemaphoreCreateBinary();
    if (!r.c.done || usb_host_device_open(r.c.cl, addr, &r.dev) != ESP_OK) {
        if (err) *err = "cannot open USB address " + std::to_string(addr);
        if (r.c.done) vSemaphoreDelete(r.c.done);
        usb_host_client_deregister(r.c.cl);
        return false;
    }
    return true;
}
static void rawClose(RawDev& r) {
    usb_host_device_close(r.c.cl, r.dev);
    vSemaphoreDelete(r.c.done);
    usb_host_client_deregister(r.c.cl);
}

static void appendf(std::string& o, const char* f, ...) __attribute__((format(printf, 2, 3)));
static void appendf(std::string& o, const char* f, ...) {
    char b[200];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    o += b;
}

// GET of one control: request 0x81 CUR, 0x82 MIN, 0x83 MAX, 0x84 RES. Returns the bytes read, or -1.
static int acGet(RawDev& r, uint8_t req, uint8_t unit, int acIface, uint16_t wValue, int len, int& v) {
    uint8_t b[2] = {0, 0};
    int g = rawControl(r.c, r.dev, 0xA1, req, wValue, (uint16_t)((unit << 8) | acIface), b, (uint16_t)len);
    v = len == 1 ? (int)b[0] : (int)(int16_t)(b[0] | (b[1] << 8));
    return g;
}

static void acValue(std::string& o, RawDev& r, uint8_t unit, int acIface, uint16_t wValue, bool db) {
    const char* nm[] = {"cur", "min", "max", "res"};
    for (int q = 0; q < 4; q++) {
        int v = 0;
        if (acGet(r, (uint8_t)(0x81 + q), unit, acIface, wValue, db ? 2 : 1, v) < 0) { appendf(o, " %s -", nm[q]); continue; }
        if (db) appendf(o, " %s %.2f dB (0x%04X)", nm[q], v / 256.0f, (unsigned)(uint16_t)v);
        else appendf(o, " %s %d", nm[q], v);
        if (!db) break;                                  // mute: only the current value means anything
    }
}

// The report on one USB device (address).
static void usbReportDev(std::string& o, uint8_t addr) {
    RawDev r;
    std::string err;
    if (!rawOpen(r, addr, &err)) { appendf(o, "USB address %d: %s\n", addr, err.c_str()); return; }
    const usb_device_desc_t* dd = nullptr;
    usb_device_info_t info = {};
    usb_host_get_device_descriptor(r.dev, &dd);
    usb_host_device_info(r.dev, &info);
    std::string prod;
    if (info.str_desc_product)
        for (int i = 0; i < (info.str_desc_product->bLength - 2) / 2 && i < 40; i++) {
            uint16_t ch = info.str_desc_product->wData[i];
            prod += ch >= 0x20 && ch < 0x7F ? (char)ch : '?';
        }
    appendf(o, "USB address %d: %04X:%04X class %02X, %s speed, \"%s\"\n", addr, dd ? dd->idVendor : 0, dd ? dd->idProduct : 0,
            dd ? dd->bDeviceClass : 0, info.speed == USB_SPEED_HIGH ? "high" : info.speed == USB_SPEED_FULL ? "full" : "low", prod.c_str());
    const usb_config_desc_t* cfg = nullptr;
    if (usb_host_get_active_config_descriptor(r.dev, &cfg) == ESP_OK && cfg) {
        const uint8_t* p = (const uint8_t*)cfg;
        const int total = cfg->wTotalLength;
        appendf(o, "  configuration descriptor, %d bytes:\n", total);
        for (int i = 0; i < total; i += 32) {
            o += "   ";
            for (int k = i; k < total && k < i + 32; k++) appendf(o, " %02X", p[k]);
            o += "\n";
        }
        AcUnit u[24];
        int acIface;
        const int nu = acUnits(p, total, u, 24, acIface);
        for (int k = 0; k < nu; k++) {
            const int i = u[k].at;
            if (u[k].kind == 0x06) {
                const int csz = p[i + 5], nCtl = csz ? (p[i] - 7) / csz : 0;
                for (int ch = 0; ch < nCtl && ch < 8; ch++) {
                    const uint8_t bm = p[i + 6 + ch * csz];
                    if (!(bm & 0x03)) continue;
                    appendf(o, "  FU%d (from %d) channel %d, controls %02X:", u[k].id, u[k].src[0], ch, bm);
                    if (bm & 0x01) { o += " mute"; acValue(o, r, u[k].id, acIface, (uint16_t)((0x01 << 8) | ch), false); }
                    if (bm & 0x02) { o += "; volume"; acValue(o, r, u[k].id, acIface, (uint16_t)((0x02 << 8) | ch), true); }
                    o += "\n";
                }
            } else if (u[k].kind == 0x04) {
                const int np = p[i + 4], outN = u[k].nch;
                appendf(o, "  MU%d: %d input(s), %d output channel(s)\n", u[k].id, np, outN);
                int inCh = 0;
                for (int pin = 0; pin < np && pin < 8; pin++) {
                    int n = 1;
                    for (int m = 0; m < nu; m++) if (u[m].id == u[k].src[pin] && u[m].nch) n = u[m].nch;
                    for (int a = 1; a <= n && inCh + a <= 8; a++)
                        for (int v = 1; v <= outN && v <= 4; v++) {
                            appendf(o, "   in %d (from %d) -> out %d:", inCh + a, u[k].src[pin], v);
                            acValue(o, r, u[k].id, acIface, (uint16_t)(((inCh + a) << 8) | v), true);
                            o += "\n";
                        }
                    inCh += n;
                }
            }
        }
    }
    rawClose(r);
}

// All devices on the bus.
static std::string usbReport() {
    std::string o;
    uint8_t addrs[16];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof addrs, addrs, &n) != ESP_OK) n = 0;
    appendf(o, "RigDeck USB report: %d device(s); radio sound chip (playback) at address %d\n", n, s_spkAddr);
    xSemaphoreTake(s_rawMtx, portMAX_DELAY);
    for (int i = 0; i < n; i++) usbReportDev(o, addrs[i]);
    xSemaphoreGive(s_rawMtx);
    return o;
}

static void logLines(const std::string& o) {
    size_t a = 0;
    while (a < o.size()) {
        size_t b = o.find('\n', a);
        if (b == std::string::npos) b = o.size();
        for (size_t c = a; c < b; c += 180) ESP_LOGI(TAG, "%.*s", (int)std::min<size_t>(180, b - c), o.c_str() + c);
        a = b + 1;
    }
}

// ---- web: /api/usbreport (text), /api/lab (form: c=cat&t=… | c=trace&s=… | c=uac&unit=&cs=&ch=[&set=][&addr=])
static std::string formVal(const std::string& body, const char* key) {
    const std::string k = std::string(key) + "=";
    size_t pos = 0;
    while (pos < body.size()) {
        size_t e = body.find('&', pos);
        if (e == std::string::npos) e = body.size();
        if (body.compare(pos, k.size(), k) == 0) {
            std::string v;
            for (size_t i = pos + k.size(); i < e; i++) {
                if (body[i] == '+') v += ' ';
                else if (body[i] == '%' && i + 2 < e) { v += (char)strtol(body.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
                else v += body[i];
            }
            return v;
        }
        pos = e + 1;
    }
    return "";
}

static esp_err_t hUsbReport(httpd_req_t* req) {
    std::string o = usbReport();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, o.data(), o.size());
}

static esp_err_t hLab(httpd_req_t* req) {
    if (req->content_len > 1024) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too long");
    std::string body(req->content_len, 0);
    for (size_t got = 0; got < body.size();) {
        int r = httpd_req_recv(req, &body[got], body.size() - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    const std::string c = formVal(body, "c");
    std::string o;
    if (c == "cat") {
        std::string t = formVal(body, "t");
        std::vector<uint8_t> b;
        if (s_app->radio.isText()) b.assign(t.begin(), t.end());
        else for (size_t i = 0; i < t.size();) {                        // CI-V: hex bytes, spaces allowed
            if (t[i] == ' ') { i++; continue; }
            b.push_back((uint8_t)strtol(t.substr(i, 2).c_str(), nullptr, 16));
            i += 2;
        }
        if (b.empty()) o = "nothing to send\n";
        else {
            int64_t until = esp_timer_get_time() + 3000000;
            if (s_liveUntilUs.load() < until) s_liveUntilUs = until;
            ESP_LOGI(TAG, "lab: CAT command from the web page");
            const int64_t t0 = nowMs();
            civWrite(b.data(), b.size());
            vTaskDelay(pdMS_TO_TICKS(800));
            static WireEnt got[40];
            const int n = wireSince(t0, got, 40);
            for (int k = 0; k < n; k++) appendf(o, "%+5lld ms %c %s\n", (long long)(got[k].ms - t0), got[k].dir, got[k].t);
            if (!n) o = "no CAT port\n";
        }
    } else if (c == "trace") {
        int sec = atoi(formVal(body, "s").c_str());
        if (sec < 0) sec = 0;
        if (sec > 600) sec = 600;
        s_liveUntilUs = esp_timer_get_time() + (int64_t)sec * 1000000;
        ESP_LOGI(TAG, "lab: CAT trace on for %d s", sec);
        appendf(o, "CAT trace in the log for %d s\n", sec);
    } else if (c == "uac") {
        const std::string a = formVal(body, "addr"), sv = formVal(body, "set");
        const uint8_t addr = a.empty() ? s_spkAddr : (uint8_t)atoi(a.c_str());
        const uint8_t unit = (uint8_t)atoi(formVal(body, "unit").c_str());
        const int cs = atoi(formVal(body, "cs").c_str()), ch = atoi(formVal(body, "ch").c_str());
        xSemaphoreTake(s_rawMtx, portMAX_DELAY);
        RawDev r;
        std::string err;
        if (!addr) o = "no sound chip\n";
        else if (!rawOpen(r, addr, &err)) o = err + "\n";
        else {
            const usb_config_desc_t* cfg = nullptr;
            int acIface = -1, kind = 0;
            AcUnit u[24];
            if (usb_host_get_active_config_descriptor(r.dev, &cfg) == ESP_OK && cfg) {
                const int nu = acUnits((const uint8_t*)cfg, cfg->wTotalLength, u, 24, acIface);
                for (int k = 0; k < nu; k++) if (u[k].id == unit) kind = u[k].kind;
            }
            if (acIface < 0 || !kind) appendf(o, "unit %d not found on USB address %d\n", unit, addr);
            else {
                const bool two = kind == 0x04 || (kind == 0x06 && cs == 2);   // mixer, or feature unit volume: 1/256 dB
                const uint16_t wValue = (uint16_t)((cs << 8) | ch);
                if (!sv.empty()) {
                    const int v = (int)strtol(sv.c_str(), nullptr, 0);
                    uint8_t d[2] = {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF)};
                    const int w = rawControl(r.c, r.dev, 0x21, 0x01, wValue, (uint16_t)((unit << 8) | acIface), d, two ? 2 : 1);
                    appendf(o, "set unit %d cs %d ch %d to %d (0x%04X): %s\n", unit, cs, ch, v, (unsigned)(v & 0xFFFF), w >= 0 ? "ok" : "refused");
                    ESP_LOGI(TAG, "lab: sound chip unit %d cs %d ch %d set to %d: %s", unit, cs, ch, v, w >= 0 ? "ok" : "refused");
                }
                appendf(o, "unit %d cs %d ch %d:", unit, cs, ch);
                acValue(o, r, unit, acIface, wValue, two);
                o += "\n";
            }
            rawClose(r);
        }
        xSemaphoreGive(s_rawMtx);
    } else o = "unknown lab command\n";
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, o.data(), o.size());
}

void usbdiag_register(void* srv) {
    static const httpd_uri_t a = {"/api/usbreport", HTTP_GET, hUsbReport, nullptr};
    static const httpd_uri_t b = {"/api/lab", HTTP_POST, hLab, nullptr};
    httpd_register_uri_handler((httpd_handle_t)srv, &a);
    httpd_register_uri_handler((httpd_handle_t)srv, &b);
}

// The playback volume of the radio's sound chip: unmuted and at the chip's own maximum. The USB audio driver's
// "100 %" relies on the range it reads from the chip, and it read the FX-4CR's C-Media CM108 wrong: 100 % came out
// at -31.8 dB and the chip sat near -28 dB, its quiet end (the radio's input level stayed at a few percent). So
// RigDeck also asks every volume control on the playback path (fed by the USB input) for its maximum itself and sets
// that when the control is more than 1 dB below it. The chip's controls go in the log first (USB report).
static void playbackToMax(uint8_t addr) {
    RawDev r;
    std::string err;
    if (!addr || !rawOpen(r, addr, &err)) { ESP_LOGW(TAG, "audio to radio: volume check: %s", addr ? err.c_str() : "no address"); return; }
    const usb_config_desc_t* cfg = nullptr;
    if (usb_host_get_active_config_descriptor(r.dev, &cfg) == ESP_OK && cfg) {
        const uint8_t* p = (const uint8_t*)cfg;
        AcUnit u[24];
        int acIface;
        const int nu = acUnits(p, cfg->wTotalLength, u, 24, acIface);
        bool reach[24] = {};
        for (int k = 0; k < nu; k++) reach[k] = u[k].kind == 0x02 && u[k].usb;
        for (bool more = true; more;) {
            more = false;
            for (int k = 0; k < nu; k++)
                for (int s2 = 0; !reach[k] && s2 < u[k].nSrc; s2++)
                    for (int m = 0; m < nu; m++) if (u[m].id == u[k].src[s2] && reach[m]) { reach[k] = more = true; break; }
        }
        for (int k = 0; k < nu && acIface >= 0; k++) {
            if (u[k].kind != 0x06 || !reach[k]) continue;
            const int i = u[k].at, csz = p[i + 5];
            const int nCtl = csz ? (p[i] - 7) / csz : 0;
            for (int ch = 0; ch < nCtl && ch < 8; ch++) {
                if (!(p[i + 6 + ch * csz] & 0x02)) continue;
                const uint16_t wValue = (uint16_t)((0x02 << 8) | ch);
                int cur = 0, mx = 0;
                if (acGet(r, 0x81, u[k].id, acIface, wValue, 2, cur) != 2 || acGet(r, 0x83, u[k].id, acIface, wValue, 2, mx) != 2) {
                    ESP_LOGW(TAG, "audio to radio: FU%d channel %d: volume not readable", u[k].id, ch);
                    continue;
                }
                if (cur >= mx - 256) {
                    ESP_LOGI(TAG, "audio to radio: FU%d channel %d volume %.1f dB (maximum %.1f dB)", u[k].id, ch, cur / 256.0f, mx / 256.0f);
                    continue;
                }
                uint8_t d[2] = {(uint8_t)(mx & 0xFF), (uint8_t)((mx >> 8) & 0xFF)};
                const int w = rawControl(r.c, r.dev, 0x21, 0x01, wValue, (uint16_t)((u[k].id << 8) | acIface), d, 2);
                int now = cur;
                acGet(r, 0x81, u[k].id, acIface, wValue, 2, now);
                ESP_LOGW(TAG, "audio to radio: FU%d channel %d volume was %.1f dB, set to the chip's maximum %.1f dB: %s, now %.1f dB",
                         u[k].id, ch, cur / 256.0f, mx / 256.0f, w >= 0 ? "ok" : "refused", now / 256.0f);
            }
        }
    }
    rawClose(r);
}

static void spkVolume(uac_host_device_handle_t h) {
    xSemaphoreTake(s_rawMtx, portMAX_DELAY);
    std::string o;
    if (s_spkAddr) usbReportDev(o, s_spkAddr);          // the sound chip's controls, for the log (every radio)
    xSemaphoreGive(s_rawMtx);
    logLines(o);
    uac_host_device_set_mute(h, false);                  // not every chip has these controls: errors are ignored
    uac_host_device_set_volume(h, 100);
    xSemaphoreTake(s_rawMtx, portMAX_DELAY);
    playbackToMax(s_spkAddr);
    xSemaphoreGive(s_rawMtx);
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
            spkVolume(s_spk);
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
        // What went to the radio during a transmission, logged when it ends: blocks written / refused and the
        // loudest sample (full scale 32767), so a radio that hears nothing can be told from one that gets nothing.
        static bool wasTx = false;
        static int txOk = 0, txRefused = 0, txPeak = 0;
        const bool tx = s_app->txActive();
        bool ok = uac_host_device_write(s_spk, out, frames * sch * sbps, 20) == ESP_OK;
        if (tx) {
            if (!wasTx) txOk = txRefused = txPeak = 0;
            ok ? txOk++ : txRefused++;
            for (int i = 0; i < frames; i++) { int a = txMono[i] < 0 ? -txMono[i] : txMono[i]; if (a > txPeak) txPeak = a; }
        } else if (wasTx) {
            ESP_LOGI(TAG, "audio to radio during TX: %d blocks of 10 ms written, %d refused, peak %d (%d %% of full scale), %d ch %d-bit",
                     txOk, txRefused, txPeak, txPeak * 100 / 32767, sch, sbps * 8);
        }
        wasTx = tx;
        if (ok) writeFails = 0;
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
    // (ESP32-P4) cannot reach them through a high-speed hub (FTX-1: no transaction translator in the USB
    // host stack), so run the port at full speed (components/espressif__usb, patched). usbFullSpeed=0 turns it off.
    cfg.fsls_only = s_app->cfg.usbFullSpeed != 0;
    esp_err_t err = usb_host_install(&cfg);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "full-speed-only mode not available in this ESP-IDF; continuing without it");
        cfg.fsls_only = false;
        err = usb_host_install(&cfg);
    }
    if (err == ESP_OK) ESP_LOGI(TAG, "radio USB port at %s", cfg.fsls_only ? "full speed (12 Mbit/s)" : "high speed");
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
    s_rawMtx = xSemaphoreCreateMutex();
    s_app->radio.pttTrace = pttTraceHook;
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
