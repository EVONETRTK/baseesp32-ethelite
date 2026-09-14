#pragma once

// Task in background che, solo se abilitato nelle impostazioni
// (settings.h, auto_update_check_enable - disattivato di default),
// controlla periodicamente (ogni auto_update_check_interval_h ore) se e'
// disponibile un firmware piu' recente all'indirizzo manifest configurato
// (ota_update_url) e, se si', lo scarica e lo applica da solo tramite
// online_update_apply_async() - che riavvia il dispositivo se l'update
// riesce. Le stesse protezioni gia' esistenti restano valide: se la nuova
// immagine non si conferma valida dopo il riavvio, il bootloader torna da
// solo alla precedente (vedi ota_update.c).
//
// Pensato soprattutto per un dispositivo raggiungibile solo via cellulare
// (SIM7600/SIM868): il controllo/download e' un collegamento in USCITA dal
// dispositivo verso GitHub, funziona anche dietro il NAT condiviso degli
// operatori mobili - a differenza della pagina web (bloccata in entrata
// dallo stesso NAT), che quindi non e' raggiungibile da remoto per
// avviare un aggiornamento a mano in quel caso.
//
// Va chiamata una sola volta all'avvio; non fa nulla finche' l'utente non
// abilita esplicitamente la funzione dalla UI web.
void auto_update_start(void);
