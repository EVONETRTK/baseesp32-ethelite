// Prove di main/base_check.c: verifica della base di correzione del rover
// (tipi RTCM3 arrivati e distanza). I tre casi "reali" vengono dalla prova
// del 10/10/2026 sul caster rtk.evo-net.it (tabella delle sorgenti).
#include "prove.h"
#include "../../main/base_check.c"

#define N(a) (sizeof(a) / sizeof((a)[0]))

// ALTAMURA: MSM7 di 5 costellazioni, 1005/1006, 1230, effemeridi.
static const uint16_t ALTAMURA[] = { 1004, 1005, 1006, 1008, 1012, 1019, 1020, 1033, 1042,
                                     1045, 1046, 1077, 1087, 1097, 1107, 1127, 1230 };
// "puglia": solo messaggi vecchi GPS/GLONASS, 1006, niente 1230.
static const uint16_t PUGLIA[] = { 1004, 1006, 1008, 1012, 1013, 1033 };

PROVA(base_completa_vicina_ok)
{
    base_check_t r = base_check_eval(ALTAMURA, N(ALTAMURA), 12.0);
    VERIFICA_INT(r.level, BASE_CHECK_LEVEL_OK);
    VERIFICA_INT(r.issues, 0);
    VERIFICA_INT(r.n_systems, 5); // GPS, GLONASS, Galileo, SBAS, BeiDou
    VERIFICA(r.systems & BASE_CHECK_GALILEO);
    VERIFICA(r.systems & BASE_CHECK_BEIDOU);
    VERIFICA(r.has_position);
    VERIFICA(r.has_glo_bias);
    VERIFICA_VICINO(r.distance_km, 12.0, 1e-9);
}

PROVA(solo_messaggi_vecchi_attenzione)
{
    base_check_t r = base_check_eval(PUGLIA, N(PUGLIA), 20.0);
    VERIFICA_INT(r.level, BASE_CHECK_LEVEL_WARN);
    VERIFICA(r.issues & BASE_CHECK_LEGACY_ONLY);
    VERIFICA(r.issues & BASE_CHECK_FEW_SYSTEMS);
    VERIFICA(r.issues & BASE_CHECK_GLO_NO_1230);
    VERIFICA(!(r.issues & BASE_CHECK_NO_POSITION)); // la 1006 c'e'
    VERIFICA_INT(r.n_systems, 2);
    VERIFICA_INT(r.msm_systems, 0);
}

PROVA(base_a_centinaia_di_km_problema)
{
    base_check_t r = base_check_eval(ALTAMURA, N(ALTAMURA), 640.0);
    VERIFICA_INT(r.level, BASE_CHECK_LEVEL_BAD);
    VERIFICA(r.issues & BASE_CHECK_TOO_FAR);
    VERIFICA(!(r.issues & BASE_CHECK_FAR)); // uno solo dei due avvisi
}

PROVA(soglie_di_distanza)
{
    VERIFICA_INT(base_check_eval(ALTAMURA, N(ALTAMURA), 35.0).level, BASE_CHECK_LEVEL_OK);
    base_check_t r = base_check_eval(ALTAMURA, N(ALTAMURA), 35.1);
    VERIFICA_INT(r.level, BASE_CHECK_LEVEL_WARN);
    VERIFICA(r.issues & BASE_CHECK_FAR);
    VERIFICA_INT(base_check_eval(ALTAMURA, N(ALTAMURA), 60.0).level, BASE_CHECK_LEVEL_WARN);
    VERIFICA_INT(base_check_eval(ALTAMURA, N(ALTAMURA), 60.1).level, BASE_CHECK_LEVEL_BAD);
}

PROVA(distanza_sconosciuta_nessun_giudizio)
{
    base_check_t r = base_check_eval(ALTAMURA, N(ALTAMURA), -1);
    VERIFICA_INT(r.level, BASE_CHECK_LEVEL_OK);
    VERIFICA_VICINO(r.distance_km, -1, 1e-9);
    r = base_check_eval(ALTAMURA, N(ALTAMURA), 0.0 / 0.0); // NaN
    VERIFICA_INT(r.level, BASE_CHECK_LEVEL_OK);
    VERIFICA_VICINO(r.distance_km, -1, 1e-9);
}

PROVA(nessun_dato)
{
    base_check_t r = base_check_eval(NULL, 0, 5.0);
    VERIFICA_INT(r.level, BASE_CHECK_LEVEL_BAD);
    VERIFICA(r.issues & BASE_CHECK_NO_DATA);
    // Solo messaggi accessori (effemeridi, antenna): nessuna osservazione.
    static const uint16_t solo_acc[] = { 1019, 1033, 1230 };
    r = base_check_eval(solo_acc, N(solo_acc), 5.0);
    VERIFICA_INT(r.level, BASE_CHECK_LEVEL_BAD);
    VERIFICA(r.issues & BASE_CHECK_NO_DATA);
    VERIFICA(r.issues & BASE_CHECK_NO_POSITION);
}

PROVA(senza_posizione_della_base)
{
    static const uint16_t t[] = { 1077, 1087, 1097, 1127, 1230 };
    base_check_t r = base_check_eval(t, N(t), 5.0);
    VERIFICA_INT(r.level, BASE_CHECK_LEVEL_BAD);
    VERIFICA(r.issues & BASE_CHECK_NO_POSITION);
    VERIFICA_INT(r.n_systems, 4);
}

PROVA(msm4_e_tipi_ripetuti)
{
    // MSM4 di GPS, Galileo e BeiDou, tipi ripetuti come arrivano nel flusso.
    static const uint16_t t[] = { 1074, 1094, 1124, 1005, 1074, 1094, 1124, 1074 };
    base_check_t r = base_check_eval(t, N(t), 2.0);
    VERIFICA_INT(r.level, BASE_CHECK_LEVEL_OK);
    VERIFICA_INT(r.n_systems, 3);
    VERIFICA(!(r.issues & BASE_CHECK_GLO_NO_1230)); // niente GLONASS: nessun avviso
}

PROVA(tipi_non_osservazioni_ignorati)
{
    // 1070, 1080, 1100 (decine piene) e 1138 non sono MSM.
    static const uint16_t t[] = { 1005, 1070, 1080, 1100, 1138, 1013, 4072 };
    base_check_t r = base_check_eval(t, N(t), 2.0);
    VERIFICA_INT(r.systems, 0);
    VERIFICA(r.issues & BASE_CHECK_NO_DATA);
}

PROVA(nomi_e_codici)
{
    VERIFICA_STR(base_check_system_name(BASE_CHECK_GALILEO), "Galileo");
    VERIFICA(base_check_system_name(0) == NULL);
    VERIFICA(base_check_system_name(BASE_CHECK_GPS | BASE_CHECK_GLONASS) == NULL);
    VERIFICA_STR(base_check_issue_code(BASE_CHECK_TOO_FAR), "troppo_lontana");
    VERIFICA_STR(base_check_issue_code(BASE_CHECK_LEGACY_ONLY), "solo_vecchi");
    VERIFICA(base_check_issue_code(1u << 20) == NULL);
    VERIFICA_STR(base_check_level_str(BASE_CHECK_LEVEL_WARN), "attenzione");
    VERIFICA_STR(base_check_level_str(BASE_CHECK_LEVEL_BAD), "problema");
    for (int i = 0; i < BASE_CHECK_SYSTEMS; i++) {
        VERIFICA(base_check_system_name((uint8_t) (1u << i)) != NULL);
    }
}

int main(void)
{
    ESEGUI(base_completa_vicina_ok);
    ESEGUI(solo_messaggi_vecchi_attenzione);
    ESEGUI(base_a_centinaia_di_km_problema);
    ESEGUI(soglie_di_distanza);
    ESEGUI(distanza_sconosciuta_nessun_giudizio);
    ESEGUI(nessun_dato);
    ESEGUI(senza_posizione_della_base);
    ESEGUI(msm4_e_tipi_ripetuti);
    ESEGUI(tipi_non_osservazioni_ignorati);
    ESEGUI(nomi_e_codici);
    return prove_fine();
}
