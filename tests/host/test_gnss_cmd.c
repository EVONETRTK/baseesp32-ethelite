// Prove di main/gnss_cmd.c: invio dei comandi ai ricevitori (ASCII con
// "\r\n" per Unicore/ComNav/Bynav, $...*hh per Quectel e CASIC).
//
// Checksum attesi (XOR dei caratteri tra '$' e '*') calcolati a parte:
// "PCAS04,7" -> 0x1E, "PQTMCFGRCVRMODE,W,2" -> 0x29.
#include "prove.h"
#include "../../main/gnss_cmd.c"

#include <stdio.h>

// --- sostituto di gnss_io.c: registra cio' che verrebbe mandato -----------
static char s_inviato[512];
static size_t s_n;
static int s_scritture;

int gnss_io_write(const void *data, size_t len)
{
    s_scritture++;
    if (s_n + len < sizeof(s_inviato)) {
        memcpy(s_inviato + s_n, data, len);
        s_n += len;
        s_inviato[s_n] = '\0';
    }
    return (int) len;
}

static void azzera(void)
{
    s_n = 0;
    s_inviato[0] = '\0';
    s_scritture = 0;
}

PROVA(riga_ascii_con_a_capo)
{
    azzera();
    VERIFICA_INT(gnss_cmd_send_line("prova", "UNLOG"), ESP_OK);
    VERIFICA_STR(s_inviato, "UNLOG\r\n");
}

PROVA(comando_nmea_con_checksum)
{
    azzera();
    VERIFICA_INT(gnss_cmd_send_nmea("prova", "PCAS04,7"), ESP_OK);
    VERIFICA_STR(s_inviato, "$PCAS04,7*1E\r\n");
    azzera();
    VERIFICA_INT(gnss_cmd_send_nmea("prova", "PQTMCFGRCVRMODE,W,2"), ESP_OK);
    VERIFICA_STR(s_inviato, "$PQTMCFGRCVRMODE,W,2*29\r\n");
}

// Il send_cmd del LC29H non controllava il troncamento di snprintf: con un
// comando lungo mandava len byte (la lunghezza voluta, non quella scritta)
// leggendo oltre la fine del buffer da 128 byte. Ora non manda niente.
PROVA(comando_nmea_troppo_lungo_non_inviato)
{
    char lungo[200];
    memset(lungo, 'A', sizeof(lungo) - 1);
    lungo[sizeof(lungo) - 1] = '\0';
    azzera();
    VERIFICA_INT(gnss_cmd_send_nmea("prova", lungo), ESP_ERR_INVALID_SIZE);
    VERIFICA_INT(s_scritture, 0);

    // Il piu' lungo che ci sta: '$' + 121 + "*hh\r\n" = 127 caratteri, piu' la fine
    // stringa fanno i 128 del buffer. Uno in piu' non ci sta.
    char limite[122];
    memset(limite, 'B', sizeof(limite) - 1);
    limite[sizeof(limite) - 1] = '\0';
    azzera();
    VERIFICA_INT(gnss_cmd_send_nmea("prova", limite), ESP_OK);
    VERIFICA_INT(s_n, 127);

    char oltre[123];
    memset(oltre, 'C', sizeof(oltre) - 1);
    oltre[sizeof(oltre) - 1] = '\0';
    azzera();
    VERIFICA_INT(gnss_cmd_send_nmea("prova", oltre), ESP_ERR_INVALID_SIZE);
    VERIFICA_INT(s_scritture, 0);
}

int main(void)
{
    ESEGUI(riga_ascii_con_a_capo);
    ESEGUI(comando_nmea_con_checksum);
    ESEGUI(comando_nmea_troppo_lungo_non_inviato);
    return prove_fine();
}
