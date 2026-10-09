// Sostituti dei moduli usati dalla configurazione dei ricevitori (non
// provata qui) quando un bersaglio collega gnss_unicore.c / gnss_comnav.c:
// da includere in UN solo file del bersaglio.
#pragma once

#include <string.h>
#include "settings.h"
#include "license.h"

int gnss_io_write(const void *data, size_t len) { (void) data; return (int) len; }
void settings_get_into(app_settings_t *out) { memset(out, 0, sizeof(*out)); }
uint8_t settings_nmea_rate_hz(void) { return 1; }
bool license_has(license_feature_t f) { (void) f; return true; }
bool base_measure_is_has(void) { return false; }
bool base_measure_is_active(void) { return false; }

// Impostazioni viste dai moduli (settings_peek): modificabili dal bersaglio.
static app_settings_t g_stub_impostazioni;
void settings_peek(void (*fn)(const app_settings_t *s, void *ctx), void *ctx) { fn(&g_stub_impostazioni, ctx); }
