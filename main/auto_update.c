#include "auto_update.h"
#include "settings.h"
#include "online_update.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "auto_update";

// Ogni quanto il task si risveglia per rileggere le impostazioni (puo'
// essere attivata/disattivata o cambiare intervallo senza riavviare) -
// non e' la frequenza dei controlli veri e propri, solo di quanto in
// fretta ci si accorge di un cambio di configurazione.
#define POLL_INTERVAL_MS (15 * 60 * 1000)

static void auto_update_task(void *arg)
{
    int64_t last_check_us = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));

        app_settings_t s = settings_get();
        if (!s.auto_update_check_enable || s.ota_update_url[0] == '\0') {
            continue;
        }

        uint16_t interval_h = s.auto_update_check_interval_h > 0 ? s.auto_update_check_interval_h : 24;
        int64_t interval_us = (int64_t) interval_h * 3600LL * 1000000LL;
        int64_t now_us = esp_timer_get_time();
        if (last_check_us != 0 && (now_us - last_check_us) < interval_us) {
            continue;
        }
        last_check_us = now_us;

        char version[32] = {0};
        char url[192] = {0};
        char msg[96] = {0};
        bool available = online_update_check(version, sizeof(version), url, sizeof(url), msg, sizeof(msg));
        if (!available) {
            ESP_LOGI(TAG, "Controllo automatico: nessun aggiornamento disponibile (%s)", msg);
            continue;
        }

        ESP_LOGW(TAG, "Controllo automatico: trovata versione %s, la applico e riavvio (nessuno da avvisare, dispositivo in campo)", version);
        // online_update_apply_async() riavvia da sola in caso di successo;
        // se fallisce, il dispositivo resta com'era e si ritenta al
        // prossimo intervallo - vedi last_check_us sopra.
        online_update_apply_async(url);
    }
}

void auto_update_start(void)
{
    xTaskCreate(auto_update_task, "auto_update", 4096, NULL, 3, NULL);
}
