// Fuzzing delle funzioni di validazione di main/web_ui.c (estratte a ogni
// giro in build/web_ui_estratto.c da web_ui_estrai.ps1, come per le prove):
// numeri di telefono, caratteri ammessi, pin GPIO.
// Input: [opzioni][pin 8 x 2 byte][testo]. Opzioni: bit 0-4 dimensione del
// buffer di uscita del numero (2..33), bit 5-7 modo del LED RGB.
#include "motore.h"
#include "web_ui_estratto.c"

void bersaglio_avvio(void)
{
    fuzz_imposta_max_len(300);
}

void bersaglio_semi(void)
{
    uint8_t b[64] = { 20 | (1 << 5), 43, 0, 44, 0, 17, 0, 18, 0, 0xFF, 0xFF, 1, 0, 2, 0, 15, 0 };
    const char *t = "+39 (351) 123-4567";
    memcpy(b + 17, t, strlen(t));
    fuzz_seme(b, 17 + strlen(t));
    b[0] = 5 | (2 << 5);
    const char *t2 = "+393511234567+";
    memcpy(b + 17, t2, strlen(t2));
    fuzz_seme(b, 17 + strlen(t2));
}

void bersaglio_esegui(const uint8_t *d, size_t n)
{
    if (n < 17) return;
    uint8_t opz = d[0];
    int pin[8];
    for (int i = 0; i < 8; i++) pin[i] = (int16_t) (d[1 + 2 * i] | (d[2 + 2 * i] << 8));
    char *s = fuzz_stringa(d + 17, n - 17);

    // da 2 byte: con 1 byte la funzione scrive oltre (solo con "+", vedi
    // test_web_ui.c, numero_con_buffer_di_un_byte); il firmware usa 24 byte.
    size_t cap = 2 + (opz & 31);
    char *out = (char *) fuzz_buffer(cap);
    if (clean_phone_number(s, out, cap)) {
        FUZZ_VERIFICA(strlen(out) < cap);
    }
    (void) only_chars(s, "+0123456789");
    (void) sms_number_ok(s);

    for (int i = 0; i < 8; i++) (void) pin_allowed(pin[i]);
    static app_settings_t st;
    memset(&st, 0, sizeof(st));
    st.gnss_i2c = (opz >> 5) & 1;
    st.gnss_uart_tx_pin = pin[0];
    st.gnss_uart_rx_pin = pin[1];
    st.oled_sda_pin = pin[2];
    st.oled_scl_pin = pin[3];
    st.rgb_led_mode = (opz >> 6);
    st.rgb_led_ws2812_pin = pin[4];
    st.rgb_led_pwm_r_pin = pin[5];
    st.rgb_led_pwm_g_pin = pin[6];
    st.rgb_led_pwm_b_pin = pin[7];
    char *verr = (char *) fuzz_buffer(VERR_SIZE);
    verr[0] = '\0';
    (void) pins_conflict(&st, verr);
    FUZZ_VERIFICA(strlen(verr) < VERR_SIZE);
    verr[0] = '\0';
    verr_set(verr, "Campo %s non valido (%d)", s, (int) n);
    FUZZ_VERIFICA(strlen(verr) < VERR_SIZE);
}
