// Prove di main/rtk_alarm.c: allarme "RTK perso" del rover (sequenze di
// qualita' della GGA e tempi).
#include "prove.h"
#include "../../main/rtk_alarm.c"

#define S(x) ((int64_t) (x) * 1000000)

static rtk_alarm_t a;

static void azzera(void)
{
    rtk_alarm_reset(&a);
}

// Un passo ogni 100 ms da t0 a t1 (escluso) con la stessa qualita', come
// il giro di lettura della GGA nel firmware; ritorna l'ultimo risultato.
static rtk_alarm_out_t passi(int q, int64_t t0, int64_t t1, uint32_t n)
{
    rtk_alarm_out_t o = { 0 };
    for (int64_t t = t0; t < t1; t += 100000) {
        o = rtk_alarm_step(&a, q, t, n);
    }
    return o;
}

PROVA(scatta_dopo_n_secondi_e_non_prima)
{
    azzera();
    VERIFICA(!passi(4, S(0), S(10), 5).active);
    // Float da t=10: a 14,9 s (4,9 s di float) ancora spento, a 15 s acceso.
    rtk_alarm_out_t o = rtk_alarm_step(&a, 5, S(10), 5);
    VERIFICA(!o.active);
    VERIFICA_INT(o.lost_s, 0);
    o = passi(5, S(10), S(15), 5);
    VERIFICA(!o.active);
    VERIFICA_INT(o.lost_s, 4);
    o = rtk_alarm_step(&a, 5, S(15), 5);
    VERIFICA(o.active);
    VERIFICA_INT(o.lost_s, 5);
    VERIFICA_INT(o.quality, 5);
    o = rtk_alarm_step(&a, 5, S(42), 5);
    VERIFICA(o.active);
    VERIFICA_INT(o.lost_s, 32);
}

PROVA(rientra_con_il_fix)
{
    azzera();
    passi(4, S(0), S(3), 5);
    VERIFICA(passi(2, S(3), S(20), 5).active);
    rtk_alarm_out_t o = rtk_alarm_step(&a, 4, S(20), 5);
    VERIFICA(!o.active);
    VERIFICA_INT(o.lost_s, 0);
    VERIFICA_INT(o.quality, 4);
    // Nuova perdita: il conto riparte da zero.
    VERIFICA(!passi(5, S(21), S(26), 5).active);
    VERIFICA(passi(5, S(26), S(27), 5).active);
}

PROVA(fix_breve_azzera_il_conto)
{
    azzera();
    passi(4, S(0), S(2), 5);
    passi(5, S(2), S(6), 5);           // 4 s di float
    rtk_alarm_step(&a, 4, S(6), 5);    // torna fisso un attimo
    VERIFICA(!passi(5, S(6) + 100000, S(11), 5).active); // 4,9 s: non ancora
}

PROVA(mai_senza_rtk_fisso_precedente)
{
    azzera();
    rtk_alarm_out_t o = passi(5, S(0), S(60), 5);
    VERIFICA(!o.active);
    VERIFICA_INT(o.lost_s, 0);
    VERIFICA(!passi(0, S(60), S(120), 5).active);
    VERIFICA(!passi(RTK_ALARM_NO_DATA, S(120), S(200), 5).active);
    VERIFICA(!passi(2, S(200), S(300), 5).active);
}

PROVA(n_zero_spento)
{
    azzera();
    passi(4, S(0), S(5), 0);
    rtk_alarm_out_t o = passi(5, S(5), S(100), 0);
    VERIFICA(!o.active);
    VERIFICA_INT(o.lost_s, 94); // il conto c'e' comunque
    // Riacceso durante la perdita: scatta subito (perdita gia' oltre N).
    VERIFICA(rtk_alarm_step(&a, 5, S(100), 5).active);
}

PROVA(dati_assenti_contano_come_perdita)
{
    azzera();
    passi(4, S(0), S(5), 5);
    VERIFICA(!passi(RTK_ALARM_NO_DATA, S(5), S(10), 5).active);
    rtk_alarm_out_t o = rtk_alarm_step(&a, RTK_ALARM_NO_DATA, S(10), 5);
    VERIFICA(o.active);
    VERIFICA_INT(o.quality, RTK_ALARM_NO_DATA);
    VERIFICA_STR(rtk_alarm_quality_label(o.quality), "NO DATI");
    // Tornano i dati ma senza fix: la perdita continua, non riparte.
    o = rtk_alarm_step(&a, 0, S(11), 5);
    VERIFICA(o.active);
    VERIFICA_INT(o.lost_s, 6);
}

PROVA(qualita_diverse_contano_tutte_come_perdita)
{
    // Float -> DGPS -> nessun fix: un'unica perdita dal primo passo.
    azzera();
    passi(4, S(0), S(1), 5);
    passi(5, S(1), S(3), 5);
    passi(2, S(3), S(5), 5);
    rtk_alarm_out_t o = rtk_alarm_step(&a, 0, S(6), 5);
    VERIFICA(o.active);
    VERIFICA_INT(o.lost_s, 5);
    VERIFICA_STR(rtk_alarm_quality_label(o.quality), "NO FIX");
}

PROVA(scritte_della_qualita)
{
    VERIFICA_STR(rtk_alarm_quality_label(5), "FLOAT");
    VERIFICA_STR(rtk_alarm_quality_label(2), "DGPS");
    VERIFICA_STR(rtk_alarm_quality_label(0), "NO FIX");
    VERIFICA_STR(rtk_alarm_quality_label(RTK_ALARM_NO_DATA), "NO DATI");
    VERIFICA_STR(rtk_alarm_quality_label(1), "AUTONOMA");
    VERIFICA_STR(rtk_alarm_quality_label(99), "ALTRO");
    for (int q = -1; q <= 9; q++) {
        VERIFICA(strlen(rtk_alarm_quality_label(q)) <= 8); // sta sul display accanto ai secondi
    }
}

PROVA(tempo_che_torna_indietro_non_rompe)
{
    azzera();
    rtk_alarm_step(&a, 4, S(10), 5);
    rtk_alarm_step(&a, 5, S(11), 5);
    rtk_alarm_out_t o = rtk_alarm_step(&a, 5, S(9), 5);
    VERIFICA(!o.active);
    VERIFICA_INT(o.lost_s, 0);
}

int main(void)
{
    ESEGUI(scatta_dopo_n_secondi_e_non_prima);
    ESEGUI(rientra_con_il_fix);
    ESEGUI(fix_breve_azzera_il_conto);
    ESEGUI(mai_senza_rtk_fisso_precedente);
    ESEGUI(n_zero_spento);
    ESEGUI(dati_assenti_contano_come_perdita);
    ESEGUI(qualita_diverse_contano_tutte_come_perdita);
    ESEGUI(scritte_della_qualita);
    ESEGUI(tempo_che_torna_indietro_non_rompe);
    return prove_fine();
}
