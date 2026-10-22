// Prove della lettura delle risposte del SIM7600 (modem_health.c). Risposte
// prese dagli esempi del manuale AT SIMCom, con eco e righe vuote come
// arrivano dal modem.
#include "prove.h"

#include "../../main/modem_health.c"

PROVA(temperatura_con_eco)
{
    int t = 0;
    VERIFICA(modem_health_parse_temp("AT+CPMUTEMP\r\r\n+CPMUTEMP: 36\r\n\r\nOK\r\n", &t));
    VERIFICA_INT(t, 36);
    VERIFICA(modem_health_parse_temp("+CPMUTEMP: -5\r\n", &t));
    VERIFICA_INT(t, -5);
    VERIFICA(!modem_health_parse_temp("ERROR\r\n", &t));
    VERIFICA(!modem_health_parse_temp("+CPMUTEMP: \r\n", &t));
    VERIFICA(!modem_health_parse_temp("+CPMUTEMP: 999\r\n", &t));
}

PROVA(tensione_in_volt_e_millivolt)
{
    int mv = 0;
    VERIFICA(modem_health_parse_cbc("AT+CBC\r\r\n+CBC: 3.801V\r\n\r\nOK\r\n", &mv));
    VERIFICA_INT(mv, 3801);
    VERIFICA(modem_health_parse_cbc("+CBC: 0,85,3912\r\n", &mv));
    VERIFICA_INT(mv, 3912);
    VERIFICA(!modem_health_parse_cbc("+CBC: abc\r\n", &mv));
    VERIFICA(!modem_health_parse_cbc("+CBC: 0.2V\r\n", &mv));   // fuori scala
    VERIFICA(!modem_health_parse_cbc(NULL, &mv));
}

// Trovato dal fuzzing: un numero enorme o "nan" finiva convertito in int
// fuori scala (comportamento indefinito).
PROVA(tensione_enorme_o_non_numero_scartata)
{
    int mv = 1234;
    VERIFICA(!modem_health_parse_cbc("+CBC: 1e20V\r\n", &mv));
    VERIFICA(!modem_health_parse_cbc("+CBC: 0,85,99999999999\r\n", &mv));
    VERIFICA(!modem_health_parse_cbc("+CBC: -1e20\r\n", &mv));
    VERIFICA(!modem_health_parse_cbc("+CBC: nan\r\n", &mv));
    VERIFICA(!modem_health_parse_cbc("+CBC: inf\r\n", &mv));
    VERIFICA_INT(mv, 1234);
}

PROVA(cpsi_lte_completo)
{
    modem_health_t h;
    memset(&h, 0, sizeof(h));
    const char *r = "AT+CPSI?\r\r\n+CPSI: LTE,Online,222-88,0x5A1E,187214780,257,EUTRAN-BAND20,6300,5,5,-94,-850,-545,15\r\n\r\nOK\r\n";
    VERIFICA(modem_health_parse_cpsi(r, &h));
    VERIFICA_STR(h.mode, "LTE");
    VERIFICA(h.lte_ok);
    VERIFICA_STR(h.mcc_mnc, "222-88");
    VERIFICA_INT(h.band, 20);
    VERIFICA_INT(h.cell_id, 187214780);
    VERIFICA_VICINO(h.rsrq_db, -9.4, 1e-4);
    VERIFICA_VICINO(h.rsrp_dbm, -85.0, 1e-4);
    VERIFICA_VICINO(h.rssi_dbm, -54.5, 1e-4);
    VERIFICA_INT(h.sinr_db, 15);
}

PROVA(cpsi_valori_gia_in_unita)
{
    modem_health_t h;
    memset(&h, 0, sizeof(h));
    VERIFICA(modem_health_parse_cpsi("+CPSI: LTE,Online,222-01,0x1,42,3,EUTRAN-BAND3,1850,5,5,-11,-98,-70,8\r\n", &h));
    VERIFICA(h.lte_ok);
    VERIFICA_VICINO(h.rsrq_db, -11, 1e-4);
    VERIFICA_VICINO(h.rsrp_dbm, -98, 1e-4);
    VERIFICA_INT(h.band, 3);
}

PROVA(cpsi_senza_servizio_e_altre_reti)
{
    modem_health_t h;
    memset(&h, 0, sizeof(h));
    VERIFICA(modem_health_parse_cpsi("+CPSI: NO SERVICE,Online\r\n", &h));
    VERIFICA_STR(h.mode, "NO SERVICE");
    VERIFICA(!h.lte_ok);
    VERIFICA(modem_health_parse_cpsi("+CPSI: GSM,Online,222-01,0x1234,5678,30,EGSM 900,-70,0,40-40\r\n", &h));
    VERIFICA_STR(h.mode, "GSM");
    VERIFICA(!h.lte_ok);
    VERIFICA(!modem_health_parse_cpsi("ERROR\r\n", &h));
}

PROVA(cpsi_valori_assurdi_scartati)
{
    modem_health_t h;
    memset(&h, 0, sizeof(h));
    modem_health_parse_cpsi("+CPSI: LTE,Online,222-88,0x5A1E,1,2,EUTRAN-BAND20,6300,5,5,x,-850,-545,15\r\n", &h);
    VERIFICA(!h.lte_ok);
    modem_health_parse_cpsi("+CPSI: LTE,Online,222-88,0x5A1E,1,2,EUTRAN-BAND20,6300,5,5,-94,-9999,-545,15\r\n", &h);
    VERIFICA(!h.lte_ok);
    modem_health_parse_cpsi("+CPSI: LTE,Online,222-88\r\n", &h);
    VERIFICA(!h.lte_ok);
}

PROVA(versione_firmware)
{
    char fw[40];
    VERIFICA(modem_health_parse_cgmr("AT+CGMR\r\r\n+CGMR: LE20B04SIM7600M22\r\n\r\nOK\r\n", fw, sizeof(fw)));
    VERIFICA_STR(fw, "LE20B04SIM7600M22");
    char corto[6];
    VERIFICA(modem_health_parse_cgmr("+CGMR: LE20B04SIM7600M22\r\n", corto, sizeof(corto)));
    VERIFICA_STR(corto, "LE20B");
    VERIFICA(!modem_health_parse_cgmr("+CGMR: \r\n", fw, sizeof(fw)));
}

int main(void)
{
    ESEGUI(temperatura_con_eco);
    ESEGUI(tensione_in_volt_e_millivolt);
    ESEGUI(tensione_enorme_o_non_numero_scartata);
    ESEGUI(cpsi_lte_completo);
    ESEGUI(cpsi_valori_gia_in_unita);
    ESEGUI(cpsi_senza_servizio_e_altre_reti);
    ESEGUI(cpsi_valori_assurdi_scartati);
    ESEGUI(versione_firmware);
    return prove_fine();
}
