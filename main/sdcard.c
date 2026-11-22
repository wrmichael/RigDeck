// microSD card (Waveshare board: SDMMC slot 0, 4-bit, IOMUX pins, powered from the P4's LDO channel
// 4). The Wi-Fi chip shares the SDMMC controller (slot 1); IDF 6.1 lets both slots live side by side
// and a failed mount removes only slot 0. The card is looked for at start-up (the slot has no
// card-detect line): insert or remove it with the power off.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdcard.h"
#include "sdlog.h"

static const char *TAG = "sdcard";
#define MOUNT "/sdcard"
static sdmmc_card_t *s_card = NULL;
static sd_pwr_ctrl_handle_t s_pwr = NULL;
static bool s_mounted = false;

static esp_err_t mount(bool formatIfUnreadable)
{
    esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = formatIfUnreadable,
        .max_files = 10,                            // log files, a rewrite's temp file, the console log, downloads
        .allocation_unit_size = 64 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    if (!s_pwr) {
        sd_pwr_ctrl_ldo_config_t ldo = {.ldo_chan_id = 4};
        esp_err_t e = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr);
        if (e != ESP_OK) return e;
    }
    host.pwr_ctrl_handle = s_pwr;
    const sdmmc_slot_config_t slot = {               // as the board support package: slot 0 pins are fixed (IO MUX)
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = 0,
    };
    return esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &mc, &s_card);
}

// "SD64G, card 63.9 GB, files 63.8 GB": the card's own size and the file system's, which differ
// when the card was partitioned small (or the card is not what its label says).
static void describe(char *info, size_t n)
{
    uint64_t total = 0, freeb = 0;
    esp_vfs_fat_info(MOUNT, &total, &freeb);
    double card = s_card ? (double)(uint64_t)s_card->csd.capacity * s_card->csd.sector_size / 1e9 : 0;
    char name[8] = "card";
    if (s_card && s_card->cid.name[0]) { memcpy(name, s_card->cid.name, 7); name[7] = 0; }
    snprintf(info, n, "%s, card %.1f GB, files %.1f GB", name, card, total / 1e9);
}

bool sdcard_start(char *info, size_t n)
{
    info[0] = 0;
    esp_err_t err = mount(false);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no microSD card in use (%s); the log stays in the board's memory", esp_err_to_name(err));
        snprintf(info, n, "%s", err == ESP_FAIL ? "in, but not readable (format it: FAT32)" : "none");
        return false;
    }
    s_mounted = true;
    mkdir(MOUNT "/RigDeck", 0777);
    describe(info, n);
    ESP_LOGI(TAG, "microSD card: %s; logs in %s", info, SDCARD_LOG_DIR);
    return true;
}

int sdcard_format(char *msg, size_t n)
{
    esp_err_t err;
    sdlog_pause(true);                                 // no file open on the card while it is formatted
    if (s_mounted) {
        ESP_LOGW(TAG, "formatting the microSD card (FAT32, whole card)");
        err = esp_vfs_fat_sdcard_format(MOUNT, s_card);
    } else {
        ESP_LOGW(TAG, "mounting the microSD card, formatting it if it cannot be read");
        err = mount(true);
        if (err == ESP_OK) {                           // it may have been readable after all: start clean
            s_mounted = true;
            err = esp_vfs_fat_sdcard_format(MOUNT, s_card);
        }
    }
    if (err != ESP_OK) {
        snprintf(msg, n, "Formatting the card failed (%s)%s", esp_err_to_name(err), s_mounted ? "" : ": is a card in?");
        sdlog_pause(false);
        return -1;
    }
    s_mounted = true;
    mkdir(MOUNT "/RigDeck", 0777);
    sdlog_pause(false);
    describe(msg, n);
    ESP_LOGI(TAG, "formatted: %s", msg);
    return 0;
}

bool sdcard_info(uint64_t *total, uint64_t *used)
{
    if (!s_mounted) return false;
    uint64_t t = 0, f = 0;
    if (esp_vfs_fat_info(MOUNT, &t, &f) != ESP_OK) return false;
    *total = t;
    *used = t > f ? t - f : 0;
    return true;
}
