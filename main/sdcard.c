// microSD card (Waveshare BSP: SDMMC slot 0, 4-bit, powered from the P4's LDO channel 4). The
// Wi-Fi chip shares the SDMMC controller (slot 1); IDF 6.1 lets both slots live side by side and a
// failed mount removes only slot 0. The card is looked for once, at start-up (the slot has no
// card-detect line): insert or remove it with the power off.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "bsp/esp-bsp.h"
#include "sdcard.h"

static const char *TAG = "sdcard";
static bool s_mounted = false;

bool sdcard_start(char *info, size_t n)
{
    info[0] = 0;
    esp_err_t err = bsp_sdcard_mount();
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no microSD card in use (%s); the log stays in the board's memory", esp_err_to_name(err));
        if (err == ESP_FAIL) snprintf(info, n, "cannot read it (use FAT32)");
        else snprintf(info, n, "none");
        return false;
    }
    s_mounted = true;
    mkdir(BSP_SD_MOUNT_POINT "/RigDeck", 0777);
    uint64_t total = 0, freeb = 0;
    esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &freeb);
    snprintf(info, n, "%s, %.1f GB", bsp_sdcard && bsp_sdcard->cid.name[0] ? bsp_sdcard->cid.name : "card", total / 1e9);
    ESP_LOGI(TAG, "microSD card %s: %llu MB, %llu MB free; logs in %s", info, (unsigned long long)(total >> 20),
             (unsigned long long)(freeb >> 20), SDCARD_LOG_DIR);
    return true;
}

bool sdcard_info(uint64_t *total, uint64_t *used)
{
    if (!s_mounted) return false;
    uint64_t t = 0, f = 0;
    if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &t, &f) != ESP_OK) return false;
    *total = t;
    *used = t > f ? t - f : 0;
    return true;
}
