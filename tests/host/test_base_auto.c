// Prove di main/base_auto.c: scelta automatica della base piu' vicina per
// il rover (distanze, isteresi, niente cambi in RTK, attesa senza RTK).
// Distanze attese da tests/host/strumenti/riferimenti.py ("== Distanze
// basi", formula di Vincenty sulla sfera: calcolo indipendente).
#include "prove.h"
#include "../../main/base_auto.c"

#include <math.h>
#include <stdio.h>

// Gradi di latitudine per percorrere km verso nord (1 grado = 111,195080 km,
// riferimenti.py).
#define KM_LAT(km) ((km) / 111.195080)

static base_auto_entry_t voce(const char *nome, double lat, double lon)
{
    base_auto_entry_t e;
    memset(&e, 0, sizeof(e));
    strncpy(e.name, nome, sizeof(e.name) - 1);
    e.lat_deg = lat;
    e.lon_deg = lon;
    return e;
}

PROVA(distanze_note)
{
    VERIFICA_VICINO(base_auto_distance_km(41.0, 16.5, 41.1, 16.6), 13.927031, 1e-5);
    VERIFICA_VICINO(base_auto_distance_km(40.0, 15.0, 41.0, 15.0), 111.195080, 1e-5);
    VERIFICA_VICINO(base_auto_distance_km(45.0, 9.0, 45.0, 10.0), 78.626296, 1e-5);
    VERIFICA_VICINO(base_auto_distance_km(45.4642, 9.19, 41.9028, 12.4964), 476.885343, 1e-5);
    VERIFICA_VICINO(base_auto_distance_km(-33.5, -70.6, -33.5, -70.6), 0.0, 1e-9);
    VERIFICA_VICINO(base_auto_distance_km(44.0, 11.0, 44.045, 11.0), 5.003779, 1e-5);
    // Simmetrica.
    VERIFICA_VICINO(base_auto_distance_km(41.1, 16.6, 41.0, 16.5), 13.927031, 1e-5);
}

PROVA(coordinate_valide)
{
    VERIFICA(base_auto_coords_valid(44.0, 11.0));
    VERIFICA(base_auto_coords_valid(-90.0, 180.0));
    VERIFICA(base_auto_coords_valid(0.0, 11.0));  // equatore: valido
    VERIFICA(!base_auto_coords_valid(0.0, 0.0));  // coordinate lasciate a zero
    VERIFICA(!base_auto_coords_valid(90.5, 11.0));
    VERIFICA(!base_auto_coords_valid(44.0, -180.5));
    VERIFICA(!base_auto_coords_valid(NAN, 11.0));
    VERIFICA(!base_auto_coords_valid(44.0, INFINITY));
}

PROVA(elenco_vuoto)
{
    base_auto_result_t r = base_auto_choose(NULL, 0, true, 44.0, 11.0, "AGRI_MSM4", 1, 100, true);
    VERIFICA_INT(r.reason, BASE_AUTO_KEEP_NO_CANDIDATES);
    VERIFICA(!r.change);
    VERIFICA_STR(r.mountpoint, "AGRI_MSM4");
    VERIFICA_INT(r.candidates, 0);
    VERIFICA_INT(r.nearest, -1);
}

PROVA(posizione_non_valida)
{
    base_auto_entry_t l[] = { voce("NORD", 44.0 + KM_LAT(20), 11.0) };
    base_auto_result_t r = base_auto_choose(l, 1, false, 44.0, 11.0, "AGRI_MSM4", 1, 100, true);
    VERIFICA_INT(r.reason, BASE_AUTO_KEEP_NO_POSITION);
    VERIFICA(!r.change);
    VERIFICA_STR(r.mountpoint, "AGRI_MSM4");
    r = base_auto_choose(l, 1, true, 0.0, 0.0, "AGRI_MSM4", 1, 100, true);
    VERIFICA_INT(r.reason, BASE_AUTO_KEEP_NO_POSITION);
    r = base_auto_choose(l, 1, true, 95.0, 11.0, "AGRI_MSM4", 1, 100, true);
    VERIFICA_INT(r.reason, BASE_AUTO_KEEP_NO_POSITION);
    r = base_auto_choose(l, 1, true, NAN, 11.0, "AGRI_MSM4", 1, 100, true);
    VERIFICA_INT(r.reason, BASE_AUTO_KEEP_NO_POSITION);
}

PROVA(mountpoint_senza_coordinate_ignorati)
{
    // Le piu' "vicine" hanno coordinate a zero, impossibili o mancano del
    // nome: vanno saltate, vince LONTANA.
    base_auto_entry_t l[] = {
        voce("ZERO", 0.0, 0.0),
        voce("FUORI", 120.0, 11.0),
        voce("NAN", NAN, 11.0),
        voce("", 44.0, 11.0),
        voce("LONTANA", 44.0 + KM_LAT(50), 11.0),
    };
    base_auto_result_t r = base_auto_choose(l, 5, true, 44.0, 11.0, "AGRI_MSM4", 1, 0, true);
    VERIFICA_INT(r.candidates, 1);
    VERIFICA_INT(r.reason, BASE_AUTO_CHANGE);
    VERIFICA(r.change);
    VERIFICA_STR(r.mountpoint, "LONTANA");
    VERIFICA_INT(r.nearest, 4);
    VERIFICA_VICINO(r.nearest_km, 50.0, 1e-3);

    // Solo mountpoint senza coordinate: nessun candidato, resta l'attuale.
    r = base_auto_choose(l, 4, true, 44.0, 11.0, "AGRI_MSM4", 1, 100, true);
    VERIFICA_INT(r.reason, BASE_AUTO_KEEP_NO_CANDIDATES);
    VERIFICA_STR(r.mountpoint, "AGRI_MSM4");
}

PROVA(primo_collegamento_sceglie_la_piu_vicina)
{
    base_auto_entry_t l[] = {
        voce("AGRI_MSM4", 44.0 + KM_LAT(20), 11.0),
        voce("SUD", 44.0 - KM_LAT(17), 11.0), // solo 3 km meglio: al primo collegamento conta lo stesso
        voce("EST", 44.0, 11.5),
    };
    // Primo collegamento: niente attesa dei 30 s ne' isteresi.
    base_auto_result_t r = base_auto_choose(l, 3, true, 44.0, 11.0, "AGRI_MSM4", 1, 0, true);
    VERIFICA_INT(r.reason, BASE_AUTO_CHANGE);
    VERIFICA_STR(r.mountpoint, "SUD");
    VERIFICA_VICINO(r.nearest_km, 17.0, 1e-3);
    VERIFICA_INT(r.current, 0);
    VERIFICA_VICINO(r.current_km, 20.0, 1e-3);
    // ...ma mai in RTK, nemmeno al primo collegamento.
    r = base_auto_choose(l, 3, true, 44.0, 11.0, "AGRI_MSM4", 4, 0, true);
    VERIFICA_INT(r.reason, BASE_AUTO_KEEP_RTK);
    VERIFICA_STR(r.mountpoint, "AGRI_MSM4");
    // Base iniziale assente dalla tabella: si sceglie la piu' vicina.
    r = base_auto_choose(l, 3, true, 44.0, 11.0, "NON_ESISTE", 0, 0, true);
    VERIFICA_INT(r.reason, BASE_AUTO_CHANGE);
    VERIFICA_STR(r.mountpoint, "SUD");
    VERIFICA_INT(r.current, -1);
    VERIFICA_VICINO(r.current_km, -1, 0);
}

PROVA(gia_la_piu_vicina)
{
    base_auto_entry_t l[] = {
        voce("LONTANA", 44.0 + KM_LAT(80), 11.0),
        voce("AGRI_MSM4", 44.0 + KM_LAT(3), 11.0),
    };
    base_auto_result_t r = base_auto_choose(l, 2, true, 44.0, 11.0, "AGRI_MSM4", 1, 600, true);
    VERIFICA_INT(r.reason, BASE_AUTO_KEEP_NEAREST);
    VERIFICA(!r.change);
    VERIFICA_STR(r.mountpoint, "AGRI_MSM4");
    VERIFICA_INT(r.nearest, 1);
    VERIFICA_VICINO(r.current_km, 3.0, 1e-3);
}

PROVA(isteresi_dei_5_km)
{
    base_auto_entry_t l[] = {
        voce("AGRI_MSM4", 44.0 + KM_LAT(20), 11.0),
        voce("SUD", 44.0 - KM_LAT(15.5), 11.0),  // 4,5 km meglio: non basta
    };
    base_auto_result_t r = base_auto_choose(l, 2, true, 44.0, 11.0, "AGRI_MSM4", 1, 120, false);
    VERIFICA_INT(r.reason, BASE_AUTO_KEEP_HYSTERESIS);
    VERIFICA(!r.change);
    VERIFICA_STR(r.mountpoint, "AGRI_MSM4");
    VERIFICA_VICINO(r.nearest_km, 15.5, 1e-3);

    l[1].lat_deg = 44.0 - KM_LAT(14.5); // 5,5 km meglio: si cambia
    r = base_auto_choose(l, 2, true, 44.0, 11.0, "AGRI_MSM4", 1, 120, false);
    VERIFICA_INT(r.reason, BASE_AUTO_CHANGE);
    VERIFICA(r.change);
    VERIFICA_STR(r.mountpoint, "SUD");
    VERIFICA_VICINO(r.current_km, 20.0, 1e-3);
    VERIFICA_VICINO(r.nearest_km, 14.5, 1e-3);
}

PROVA(niente_cambio_in_rtk_fisso_o_float)
{
    base_auto_entry_t l[] = {
        voce("AGRI_MSM4", 44.0 + KM_LAT(60), 11.0),
        voce("VICINA", 44.0 + KM_LAT(2), 11.0),
    };
    for (int q = 4; q <= 5; q++) {
        base_auto_result_t r = base_auto_choose(l, 2, true, 44.0, 11.0, "AGRI_MSM4", q, 3600, false);
        VERIFICA_INT(r.reason, BASE_AUTO_KEEP_RTK);
        VERIFICA(!r.change);
        VERIFICA_STR(r.mountpoint, "AGRI_MSM4");
        VERIFICA_VICINO(r.nearest_km, 2.0, 1e-3); // le distanze si calcolano comunque
    }
    // Altre qualita' (autonoma, DGPS, nessun fix, nessun dato) dopo 30 s: cambio.
    int altre[] = { -1, 0, 1, 2, 6 };
    for (size_t i = 0; i < sizeof(altre) / sizeof(altre[0]); i++) {
        base_auto_result_t r = base_auto_choose(l, 2, true, 44.0, 11.0, "AGRI_MSM4", altre[i], 30, false);
        VERIFICA_INT(r.reason, BASE_AUTO_CHANGE);
    }
}

PROVA(cambio_dopo_30_s_senza_rtk)
{
    base_auto_entry_t l[] = {
        voce("AGRI_MSM4", 44.0 + KM_LAT(40), 11.0),
        voce("VICINA", 44.0 - KM_LAT(10), 11.0),
    };
    base_auto_result_t r = base_auto_choose(l, 2, true, 44.0, 11.0, "AGRI_MSM4", 1, 0, false);
    VERIFICA_INT(r.reason, BASE_AUTO_KEEP_WAIT_NO_RTK);
    r = base_auto_choose(l, 2, true, 44.0, 11.0, "AGRI_MSM4", 1, 29, false);
    VERIFICA_INT(r.reason, BASE_AUTO_KEEP_WAIT_NO_RTK);
    VERIFICA(!r.change);
    VERIFICA_STR(r.mountpoint, "AGRI_MSM4");
    r = base_auto_choose(l, 2, true, 44.0, 11.0, "AGRI_MSM4", 1, 30, false);
    VERIFICA_INT(r.reason, BASE_AUTO_CHANGE);
    VERIFICA_STR(r.mountpoint, "VICINA");
    VERIFICA_VICINO(r.nearest_km, 10.0, 1e-3);
    // Base in uso non piu' in tabella (o senza coordinate): cambio dopo i
    // 30 s, l'isteresi non si puo' calcolare.
    r = base_auto_choose(l + 1, 1, true, 44.0, 11.0, "AGRI_MSM4", 2, 45, false);
    VERIFICA_INT(r.reason, BASE_AUTO_CHANGE);
    VERIFICA_STR(r.mountpoint, "VICINA");
}

PROVA(nomi_lunghi_e_pari_distanza)
{
    // Nome di 32 caratteri (il massimo di rover_mountpoint): copiato intero.
    const char *lungo = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
    base_auto_entry_t l[] = {
        voce("AGRI_MSM4", 44.0 + KM_LAT(10), 11.0),
        voce(lungo, 44.0 + KM_LAT(10), 11.0), // stessa posizione: si tiene quella in uso
    };
    base_auto_result_t r = base_auto_choose(l, 2, true, 44.0, 11.0, "AGRI_MSM4", 1, 100, false);
    VERIFICA_INT(r.reason, BASE_AUTO_KEEP_NEAREST);
    VERIFICA_STR(r.mountpoint, "AGRI_MSM4");
    r = base_auto_choose(l + 1, 1, true, 44.0, 11.0, "AGRI_MSM4", 1, 100, true);
    VERIFICA_STR(r.mountpoint, lungo);
    VERIFICA(strlen(base_auto_reason_str(r.reason)) > 0);
}

// Collaudo del 22/10/2026: la base piu' vicina (0,4 km) era in tabella ma
// muta; il rover ci tornava a ogni controllo e restava senza correzioni.
PROVA(base_muta_esclusa_per_un_ora)
{
    const int64_t S = 1000000;
    base_auto_ban_t bans[BASE_AUTO_BANS];
    memset(bans, 0, sizeof(bans));
    base_auto_entry_t l[] = {
        voce("MUTA", 44.0 + KM_LAT(0.4), 11.0),
        voce("PUGLIA", 44.0 + KM_LAT(36), 11.0),
        voce("LONTANA", 44.0 + KM_LAT(80), 11.0),
    };
    int64_t t = 1000 * S;
    base_auto_ban_add(bans, "MUTA", t);
    VERIFICA(base_auto_banned(bans, "MUTA", t + 10 * S));
    VERIFICA(!base_auto_banned(bans, "PUGLIA", t + 10 * S));

    // Dalla base iniziale, senza RTK: non si torna sulla muta.
    base_auto_result_t r = base_auto_choose_ex(l, 3, true, 44.0, 11.0, "PUGLIA", 1, 600, false, bans, t + 600 * S);
    VERIFICA(!r.change);
    VERIFICA_STR(r.mountpoint, "PUGLIA");
    VERIFICA_INT(r.reason, BASE_AUTO_KEEP_NEAREST);

    // Base in uso esclusa: si passa subito alla piu' vicina delle altre,
    // anche entro i 30 s e l'isteresi, ma mai in RTK.
    r = base_auto_choose_ex(l, 3, true, 44.0, 11.0, "MUTA", 1, 0, false, bans, t + 10 * S);
    VERIFICA(r.change);
    VERIFICA_STR(r.mountpoint, "PUGLIA");
    r = base_auto_choose_ex(l, 3, true, 44.0, 11.0, "MUTA", 4, 0, false, bans, t + 10 * S);
    VERIFICA(!r.change);

    // Solo la muta in tabella: nessun candidato, nessun cambio.
    r = base_auto_choose_ex(l, 1, true, 44.0, 11.0, "PUGLIA", 1, 600, false, bans, t + 10 * S);
    VERIFICA(!r.change);
    VERIFICA_INT(r.reason, BASE_AUTO_KEEP_NO_CANDIDATES);

    // Dopo un'ora si riprova.
    int64_t dopo = t + (int64_t) BASE_AUTO_BAN_S * S + S;
    VERIFICA(!base_auto_banned(bans, "MUTA", dopo));
    r = base_auto_choose_ex(l, 3, true, 44.0, 11.0, "PUGLIA", 1, 600, false, bans, dopo);
    VERIFICA(r.change);
    VERIFICA_STR(r.mountpoint, "MUTA");
}

// Collaudo del 23/10/2026: la base iniziale (impostata nel pannello) era
// anche la piu' vicina, e muta; non si escludeva mai e il rover ci tornava
// ogni 35 s per sempre, con puglia funzionante a 40 km.
PROVA(base_iniziale_muta_esclusa)
{
    const int64_t S = 1000000;
    base_auto_ban_t bans[BASE_AUTO_BANS];
    memset(bans, 0, sizeof(bans));
    base_auto_entry_t l[] = {
        voce("AGRI_MSM4", 44.0 + KM_LAT(0.4), 11.0),
        voce("puglia", 44.0 + KM_LAT(40), 11.0),
    };
    int silent = 0;
    int64_t t = 1000 * S;
    VERIFICA_INT(base_auto_silent_session(bans, &silent, false, "AGRI_MSM4", "AGRI_MSM4", t), BASE_AUTO_SILENT_KEEP);
    VERIFICA(!base_auto_banned(bans, "AGRI_MSM4", t));
    VERIFICA_INT(base_auto_silent_session(bans, &silent, false, "AGRI_MSM4", "AGRI_MSM4", t + 35 * S),
                 BASE_AUTO_SILENT_RECHOOSE);
    VERIFICA_INT(silent, 0);
    VERIFICA(base_auto_banned(bans, "AGRI_MSM4", t + 40 * S));
    // Scelta rifatta (first): la piu' vicina delle altre.
    base_auto_result_t r = base_auto_choose_ex(l, 2, true, 44.0, 11.0, "AGRI_MSM4", 1, 0, true, bans, t + 40 * S);
    VERIFICA(r.change);
    VERIFICA_STR(r.mountpoint, "puglia");

    // Muta anche puglia: la base iniziale e' esclusa, non ci si torna.
    VERIFICA_INT(base_auto_silent_session(bans, &silent, false, "puglia", "AGRI_MSM4", t + 100 * S), BASE_AUTO_SILENT_KEEP);
    VERIFICA_INT(base_auto_silent_session(bans, &silent, false, "puglia", "AGRI_MSM4", t + 135 * S),
                 BASE_AUTO_SILENT_RECHOOSE);

    // Base iniziale funzionante: dalla muta si torna li'.
    memset(bans, 0, sizeof(bans));
    silent = 0;
    (void) base_auto_silent_session(bans, &silent, false, "puglia", "AGRI_MSM4", t);
    VERIFICA_INT(base_auto_silent_session(bans, &silent, false, "puglia", "AGRI_MSM4", t + 35 * S),
                 BASE_AUTO_SILENT_FALLBACK);

    // Un byte arrivato azzera il conto.
    silent = 0;
    (void) base_auto_silent_session(bans, &silent, false, "AGRI_MSM4", "AGRI_MSM4", t);
    VERIFICA_INT(base_auto_silent_session(bans, &silent, true, "AGRI_MSM4", "AGRI_MSM4", t + 35 * S), BASE_AUTO_SILENT_KEEP);
    VERIFICA_INT(silent, 0);
}

PROVA(elenco_delle_escluse_pieno)
{
    const int64_t S = 1000000;
    base_auto_ban_t bans[BASE_AUTO_BANS];
    memset(bans, 0, sizeof(bans));
    char nome[8];
    for (int i = 0; i < BASE_AUTO_BANS; i++) {
        snprintf(nome, sizeof(nome), "B%d", i);
        base_auto_ban_add(bans, nome, (int64_t) (i + 1) * S);
    }
    base_auto_ban_add(bans, "B1", 10 * S); // rinnovo: nessun posto in piu'
    base_auto_ban_add(bans, "NUOVA", 11 * S); // prende il posto di B0, che scade prima
    VERIFICA(base_auto_banned(bans, "NUOVA", 12 * S));
    VERIFICA(!base_auto_banned(bans, "B0", 12 * S));
    VERIFICA(base_auto_banned(bans, "B1", 12 * S));
    VERIFICA(base_auto_banned(bans, "B2", 12 * S));
    base_auto_ban_add(bans, "", 12 * S);   // ignorati
    base_auto_ban_add(bans, NULL, 12 * S);
    base_auto_ban_add(NULL, "X", 12 * S);
    VERIFICA(!base_auto_banned(NULL, "X", 12 * S));
    VERIFICA(!base_auto_banned(bans, NULL, 12 * S));
}

int main(void)
{
    ESEGUI(distanze_note);
    ESEGUI(coordinate_valide);
    ESEGUI(elenco_vuoto);
    ESEGUI(posizione_non_valida);
    ESEGUI(mountpoint_senza_coordinate_ignorati);
    ESEGUI(primo_collegamento_sceglie_la_piu_vicina);
    ESEGUI(gia_la_piu_vicina);
    ESEGUI(isteresi_dei_5_km);
    ESEGUI(niente_cambio_in_rtk_fisso_o_float);
    ESEGUI(cambio_dopo_30_s_senza_rtk);
    ESEGUI(nomi_lunghi_e_pari_distanza);
    ESEGUI(base_muta_esclusa_per_un_ora);
    ESEGUI(base_iniziale_muta_esclusa);
    ESEGUI(elenco_delle_escluse_pieno);
    return prove_fine();
}
