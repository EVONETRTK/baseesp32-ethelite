// Prove delle funzioni di validazione di main/web_ui.c (pin consentiti,
// pin usati due volte, numero WhatsApp, caratteri ammessi).
//
// web_ui.c non si compila sul PC (server HTTP, cJSON, mbedTLS...): run.ps1
// copia queste funzioni, testo identico, in build/web_ui_estratto.c a ogni
// giro, con i pin del menuconfig presi da sdkconfig (o dai valori
// predefiniti di Kconfig.projbuild).
#include "prove.h"
#include "web_ui_estratto.c"

static bool pin_di_config(int p)
{
    const int cfg[] = {
#ifdef CONFIG_BASEESP32_CELLULAR_UART_TX_PIN
        CONFIG_BASEESP32_CELLULAR_UART_TX_PIN, CONFIG_BASEESP32_CELLULAR_UART_RX_PIN,
        CONFIG_BASEESP32_CELLULAR_PWRKEY_PIN, CONFIG_BASEESP32_CELLULAR_DTR_PIN,
#endif
#ifdef CONFIG_BASEESP32_LED_NET_PIN
        CONFIG_BASEESP32_LED_NET_PIN, CONFIG_BASEESP32_LED_DATA_PIN,
#endif
#ifdef CONFIG_BASEESP32_RESET_BUTTON_PIN
        CONFIG_BASEESP32_RESET_BUTTON_PIN,
#endif
        -1,
    };
    for (size_t i = 0; i < sizeof(cfg) / sizeof(cfg[0]); i++) {
        if (cfg[i] >= 0 && cfg[i] == p) return true;
    }
    return false;
}

PROVA(pin_riservati_della_scheda_rifiutati)
{
    // avvio, USB, inesistenti/flash/PSRAM, Ethernet, microSD
    const int no[] = { 0, 3, 19, 20, 22, 26, 30, 33, 37, 45, 46, 9, 10, 11, 12, 14, 21, 47, 48, 49, 255, -2, -100 };
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++) {
        if (pin_allowed(no[i])) prove_errore(__FILE__, __LINE__, "GPIO %d accettato, dovrebbe essere rifiutato", no[i]);
    }
}

PROVA(pin_non_usato_meno_uno_accettato)
{
    VERIFICA(pin_allowed(-1));
}

PROVA(pin_del_menuconfig_rifiutati)
{
    for (int p = 0; p <= 48; p++) {
        if (pin_di_config(p) && pin_allowed(p)) {
            prove_errore(__FILE__, __LINE__, "GPIO %d del menuconfig (modem/LED/pulsante) accettato", p);
        }
    }
}

PROVA(pin_liberi_accettati)
{
    const int si[] = { 1, 2, 4, 5, 6, 7, 8, 13, 15, 16, 17, 18, 38, 39, 40, 41, 42, 43, 44 };
    for (size_t i = 0; i < sizeof(si) / sizeof(si[0]); i++) {
        if (!pin_di_config(si[i]) && !pin_allowed(si[i])) {
            prove_errore(__FILE__, __LINE__, "GPIO %d libero ma rifiutato", si[i]);
        }
    }
}

PROVA(pin_predefiniti_di_ricevitore_e_display_accettati)
{
    // Se i pin predefiniti fossero rifiutati, il pannello non salverebbe
    // nessuna impostazione con la configurazione di fabbrica.
#ifdef CONFIG_BASEESP32_GNSS_UART_TX_PIN
    VERIFICA(pin_allowed(CONFIG_BASEESP32_GNSS_UART_TX_PIN));
    VERIFICA(pin_allowed(CONFIG_BASEESP32_GNSS_UART_RX_PIN));
#endif
#ifdef CONFIG_BASEESP32_OLED_SDA_PIN
    VERIFICA(pin_allowed(CONFIG_BASEESP32_OLED_SDA_PIN));
    VERIFICA(pin_allowed(CONFIG_BASEESP32_OLED_SCL_PIN));
#endif
#ifdef CONFIG_BASEESP32_RGB_WS2812_PIN
    VERIFICA(pin_allowed(CONFIG_BASEESP32_RGB_WS2812_PIN));
#endif
}

static app_settings_t s;

static void impostazioni_base(void)
{
    memset(&s, 0, sizeof(s));
    s.gnss_i2c = false;
    s.gnss_uart_tx_pin = 43;
    s.gnss_uart_rx_pin = 44;
    s.oled_sda_pin = 17;
    s.oled_scl_pin = 18;
    s.rgb_led_mode = RGB_LED_NONE;
    s.rgb_led_ws2812_pin = 17; // non usato con RGB_LED_NONE
    s.rgb_led_pwm_r_pin = s.rgb_led_pwm_g_pin = s.rgb_led_pwm_b_pin = -1;
}

PROVA(pin_diversi_nessun_conflitto)
{
    impostazioni_base();
    char verr[VERR_SIZE] = "";
    VERIFICA(!pins_conflict(&s, verr));
    VERIFICA_STR(verr, "");
}

PROVA(stesso_pin_per_seriale_e_display_conflitto_con_messaggio)
{
    impostazioni_base();
    s.oled_sda_pin = 43;
    char verr[VERR_SIZE] = "";
    VERIFICA(pins_conflict(&s, verr));
    VERIFICA(strstr(verr, "GPIO 43") != NULL);
    VERIFICA(strstr(verr, "TX seriale ricevitore") != NULL);
    VERIFICA(strstr(verr, "SDA display/I2C") != NULL);
}

PROVA(ricevitore_i2c_libera_i_pin_della_seriale)
{
    impostazioni_base();
    s.gnss_i2c = true;
    s.oled_sda_pin = 43;
    VERIFICA(!pins_conflict(&s, NULL));
}

PROVA(led_ws2812_e_pwm_controllati_solo_se_attivi)
{
    impostazioni_base();
    s.rgb_led_mode = RGB_LED_WS2812;
    VERIFICA(pins_conflict(&s, NULL)); // 17 come SDA
    s.rgb_led_ws2812_pin = 2;
    VERIFICA(!pins_conflict(&s, NULL));
    s.rgb_led_mode = RGB_LED_PWM3;
    s.rgb_led_pwm_r_pin = 5;
    s.rgb_led_pwm_g_pin = 5;
    VERIFICA(pins_conflict(&s, NULL));
}

PROVA(pin_meno_uno_ripetuti_non_sono_conflitto)
{
    impostazioni_base();
    s.rgb_led_mode = RGB_LED_PWM3; // tre pin a -1
    s.oled_sda_pin = s.oled_scl_pin = -1;
    VERIFICA(!pins_conflict(&s, NULL));
}

PROVA(primo_errore_non_sovrascritto)
{
    impostazioni_base();
    s.oled_sda_pin = 43;
    char verr[VERR_SIZE] = "errore precedente";
    VERIFICA(pins_conflict(&s, verr));
    VERIFICA_STR(verr, "errore precedente");
}

PROVA(numero_whatsapp_ripulito)
{
    char o[24];
    VERIFICA(clean_phone_number("+39 123 456 7890", o, sizeof(o)));
    VERIFICA_STR(o, "+391234567890");
    VERIFICA(clean_phone_number("39-123.456/78(9)0", o, sizeof(o)));
    VERIFICA_STR(o, "391234567890");
    VERIFICA(clean_phone_number("  +39 123456", o, sizeof(o)));
    VERIFICA_STR(o, "+39123456");
}

PROVA(numero_whatsapp_vuoto_spegne_il_canale)
{
    char o[24] = "x";
    VERIFICA(clean_phone_number("", o, sizeof(o)));
    VERIFICA_STR(o, "");
}

PROVA(numero_whatsapp_non_valido_rifiutato)
{
    char o[24];
    VERIFICA(!clean_phone_number("+39 12a456", o, sizeof(o)));   // lettera
    VERIFICA(!clean_phone_number("12345", o, sizeof(o)));        // meno di 6 cifre
    VERIFICA(!clean_phone_number("+12345", o, sizeof(o)));
    VERIFICA(!clean_phone_number("39+123456", o, sizeof(o)));    // + in mezzo
    VERIFICA(!clean_phone_number("++39123456", o, sizeof(o)));
    VERIFICA(!clean_phone_number("+39\"123456", o, sizeof(o)));  // virgolette
    VERIFICA(!clean_phone_number("+", o, sizeof(o)));            // solo +
}

PROVA(numero_whatsapp_troppo_lungo_rifiutato_senza_sforare)
{
    char o[8 + 4];
    memset(o, 'Z', sizeof(o));
    VERIFICA(!clean_phone_number("123456789012", o, 8));
    VERIFICA(o[8] == 'Z' && o[11] == 'Z'); // niente scritto oltre gli 8 byte
    VERIFICA(clean_phone_number("1234567", o, 8)); // 7 cifre + terminatore
    VERIFICA_STR(o, "1234567");
}

PROVA(solo_caratteri_ammessi)
{
    VERIFICA(only_chars("*123#", "0123456789*#+"));
    VERIFICA(only_chars("", "0123456789"));
    VERIFICA(!only_chars("*123#\"", "0123456789*#+"));
    VERIFICA(!only_chars("*123#\r\nAT+CMGS", "0123456789*#+"));
    VERIFICA(!only_chars("abc DEF", "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"));
}

// Bug trovato dalle prove il 09/10/2026: il numero per il credito via SMS
// accettava un + in mezzo ("+39+123"), mentre l'invio degli SMS lo rifiutava.
PROVA(numero_sms_con_piu_solo_iniziale)
{
    VERIFICA(sms_number_ok("+393510000000"));
    VERIFICA(sms_number_ok("40916"));        // numeri brevi degli operatori
    VERIFICA(sms_number_ok(""));             // campo facoltativo vuoto
    VERIFICA(!sms_number_ok("+39+123"));
    VERIFICA(!sms_number_ok("39+"));
    VERIFICA(!sms_number_ok("+39 351"));
}

int main(void)
{
    ESEGUI(pin_riservati_della_scheda_rifiutati);
    ESEGUI(pin_non_usato_meno_uno_accettato);
    ESEGUI(pin_del_menuconfig_rifiutati);
    ESEGUI(pin_liberi_accettati);
    ESEGUI(pin_predefiniti_di_ricevitore_e_display_accettati);
    ESEGUI(pin_diversi_nessun_conflitto);
    ESEGUI(stesso_pin_per_seriale_e_display_conflitto_con_messaggio);
    ESEGUI(ricevitore_i2c_libera_i_pin_della_seriale);
    ESEGUI(led_ws2812_e_pwm_controllati_solo_se_attivi);
    ESEGUI(pin_meno_uno_ripetuti_non_sono_conflitto);
    ESEGUI(primo_errore_non_sovrascritto);
    ESEGUI(numero_whatsapp_ripulito);
    ESEGUI(numero_whatsapp_vuoto_spegne_il_canale);
    ESEGUI(numero_whatsapp_non_valido_rifiutato);
    ESEGUI(numero_whatsapp_troppo_lungo_rifiutato_senza_sforare);
    ESEGUI(solo_caratteri_ammessi);
    ESEGUI(numero_sms_con_piu_solo_iniziale);
    return prove_fine();
}
