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

// Solo i campi che servono qui, senza copiare tutta la configurazione
// (~2,4 KB) sullo stack: insieme alla copia dentro online_update_check() e
// ai buffer degli URL il task da 5 KB traboccava al primo controllo, 15
// minuti dopo ogni avvio (revisione del 09/10/2026).
typedef struct {
    bool enable;
    bool has_url;
    uint16_t interval_h;
} auto_cfg_t;

static void peek_auto_cfg(const app_settings_t *s, void *ctx)
{
    auto_cfg_t *c = (auto_cfg_t *) ctx;
    c->enable = s->auto_update_check_enable;
    c->has_url = s->ota_update_url[0] != '\0';
    c->interval_h = s->auto_update_check_interval_h;
}

static void auto_update_task(void *arg)
{
    int64_t last_check_us = 0;
    // Statici: niente buffer sullo stack del task (un solo task li usa).
    static char version[32];
    static char url[192];
    static char msg[96];

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));

        auto_cfg_t c = { 0 };
        settings_peek(peek_auto_cfg, &c);
        if (!c.enable || !c.has_url) {
            continue;
        }

        uint16_t interval_h = c.interval_h > 0 ? c.interval_h : 24;
        int64_t interval_us = (int64_t) interval_h * 3600LL * 1000000LL;
        int64_t now_us = esp_timer_get_time();
        if (last_check_us != 0 && (now_us - last_check_us) < interval_us) {
            continue;
        }
        last_check_us = now_us;

        version[0] = '\0';
        url[0] = '\0';
        msg[0] = '\0';
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
    // 10 KB: online_update_check() fa una richiesta HTTPS (handshake TLS)
    // con i buffer degli URL sullo stack; con 5 KB traboccava al primo
    // controllo automatico.
    xTaskCreate(auto_update_task, "auto_update", 10240, NULL, 3, NULL);
}
