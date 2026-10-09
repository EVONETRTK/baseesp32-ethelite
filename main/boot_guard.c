#include "boot_guard.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include "hal/wdt_hal.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "boot_guard";

#define RTC_MAGIC 0xB007601Du

// Memoria RTC: sopravvive ai riavvii software e ai watchdog, non a un calo di corrente.
static RTC_NOINIT_ATTR uint32_t s_rtc_magic;
static RTC_NOINIT_ATTR uint32_t s_rtc_stage;

static uint32_t s_prev_rtc_stage;
static char s_note[96];
static int64_t s_start_us; // quando il watchdog RTC e' stato portato a BOOT_GUARD_TIMEOUT_S

static const char *stage_name(uint32_t s)
{
    switch (s) {
    case BOOT_STAGE_START:       return "inizio";
    case BOOT_STAGE_NVS:         return "memoria NVS";
    case BOOT_STAGE_SETTINGS:    return "impostazioni";
    case BOOT_STAGE_OLED:        return "display e LED (bus I2C)";
    case BOOT_STAGE_LICENSE:     return "licenza";
    case BOOT_STAGE_GNSS_PROBE:  return "ricerca del ricevitore GNSS (bus I2C)";
    case BOOT_STAGE_NET:         return "avvio della rete";
    case BOOT_STAGE_WEB:         return "pannello web";
    case BOOT_STAGE_SD_UPDATE:   return "controllo aggiornamento da microSD";
    case BOOT_STAGE_GNSS_DETECT: return "riconoscimento del ricevitore GNSS";
    case BOOT_STAGE_SERVICES:    return "avvio dei servizi";
    case BOOT_STAGE_DONE:        return "completato";
    default:                     return "sconosciuto";
    }
}

static void rwdt_set(uint32_t seconds)
{
    wdt_hal_context_t ctx = RWDT_HAL_CONTEXT_DEFAULT();
    // esp_clk_slowclk_cal_get(): periodo del clock lento in us, formato Q13.19.
    uint32_t cal = esp_clk_slowclk_cal_get();
    uint64_t ticks = cal ? (((uint64_t) seconds * 1000000ULL) << 19) / cal : (uint64_t) seconds * 136000ULL;
    if (ticks > UINT32_MAX) {
        ticks = UINT32_MAX;
    }
    wdt_hal_write_protect_disable(&ctx);
    // RESET_SYSTEM: riparte il sistema digitale (CPU e periferiche, come dopo
    // un crash), non la parte RTC: la memoria RTC (traccia dell'avvio) resta
    // e i pin di avvio non vengono riletti (servirebbe WDT_STAGE_ACTION_RESET_RTC).
    wdt_hal_config_stage(&ctx, WDT_STAGE0, (uint32_t) ticks, WDT_STAGE_ACTION_RESET_SYSTEM);
    wdt_hal_enable(&ctx);
    wdt_hal_feed(&ctx);
    wdt_hal_write_protect_enable(&ctx);
}

void boot_guard_early(void)
{
#if CONFIG_BOOTLOADER_WDT_ENABLE && CONFIG_BOOTLOADER_WDT_DISABLE_IN_USER_CODE
    rwdt_set(BOOT_GUARD_TIMEOUT_S);
#else
#warning "boot_guard: watchdog RTC non attivo nel firmware (CONFIG_BOOTLOADER_WDT_DISABLE_IN_USER_CODE)"
#endif
    s_start_us = esp_timer_get_time(); // il watchdog e' appena stato ricaricato
    s_prev_rtc_stage = (s_rtc_magic == RTC_MAGIC) ? s_rtc_stage : 0;
    s_rtc_magic = RTC_MAGIC;
    s_rtc_stage = BOOT_STAGE_START;
    if (s_prev_rtc_stage != 0 && s_prev_rtc_stage != BOOT_STAGE_DONE) {
        snprintf(s_note, sizeof(s_note), "avvio precedente fermo a: %s", stage_name(s_prev_rtc_stage));
        ESP_LOGW(TAG, "L'avvio precedente non era finito: si era fermato a \"%s\" (memoria RTC)", stage_name(s_prev_rtc_stage));
    }
}

static void nvs_put_stage(uint32_t stage)
{
    nvs_handle_t h;
    if (nvs_open("bootguard", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "stage", stage);
        nvs_commit(h);
        nvs_close(h);
    }
}

void boot_guard_after_nvs(void)
{
    nvs_handle_t h;
    uint32_t prev = 0, incomplete = 0;
    if (nvs_open("bootguard", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "stage", &prev);
        nvs_get_u32(h, "incompl", &incomplete);
        if (prev != 0 && prev != BOOT_STAGE_DONE) {
            incomplete++;
            nvs_set_u32(h, "incompl", incomplete);
        }
        nvs_set_u32(h, "stage", BOOT_STAGE_NVS);
        nvs_commit(h);
        nvs_close(h);
    }
    // La NVS vede anche gli avvii interrotti da un calo di corrente (la memoria RTC no).
    if (prev != 0 && prev != BOOT_STAGE_DONE && s_note[0] == 0) {
        snprintf(s_note, sizeof(s_note), "avvio precedente fermo a: %s", stage_name(prev));
    }
    if (prev != 0 && prev != BOOT_STAGE_DONE) {
        ESP_LOGW(TAG, "L'avvio precedente si era fermato a \"%s\" (NVS; avvii non completati finora: %u)",
                 stage_name(prev), (unsigned) incomplete);
    }
    s_rtc_stage = BOOT_STAGE_NVS;
}

void boot_guard_stage(boot_stage_t stage)
{
    s_rtc_stage = stage;
    // Nella NVS solo i passi principali: ogni scrittura consuma la flash.
    if (stage == BOOT_STAGE_GNSS_PROBE || stage == BOOT_STAGE_NET || stage == BOOT_STAGE_SERVICES) {
        nvs_put_stage(stage);
    }
}

void boot_guard_done(void)
{
#if CONFIG_BOOTLOADER_WDT_ENABLE && CONFIG_BOOTLOADER_WDT_DISABLE_IN_USER_CODE
    wdt_hal_context_t ctx = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_disable(&ctx);
    wdt_hal_write_protect_enable(&ctx);
#endif
    s_rtc_stage = BOOT_STAGE_DONE;
    nvs_put_stage(BOOT_STAGE_DONE);
    ESP_LOGI(TAG, "Avvio completato: salvavita dell'avvio spento");
}

void boot_guard_restarting(void)
{
    // Riavvio voluto (aggiornamento, comando dal pannello, misura...): anche
    // a meta' avvio non e' un avvio "fermo". Nella NVS solo se serve (a
    // avvio finito c'e' gia' "completato"): ogni scrittura consuma la flash.
    if (s_rtc_stage != BOOT_STAGE_DONE) {
        s_rtc_stage = BOOT_STAGE_DONE;
        nvs_put_stage(BOOT_STAGE_DONE); // senza NVS (avvio iniziale) non fa nulla
    }
}

uint32_t boot_guard_remaining_ms(void)
{
    if (s_rtc_stage == BOOT_STAGE_DONE) {
        return UINT32_MAX;
    }
    int64_t left = (int64_t) BOOT_GUARD_TIMEOUT_S * 1000 - (esp_timer_get_time() - s_start_us) / 1000;
    return left > 0 ? (uint32_t) left : 0;
}

const char *boot_guard_prev_note(void)
{
    return s_note;
}
