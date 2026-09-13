# Changelog

Versionamento semantico (MAJOR.MINOR.PATCH). Vedi `main/version.h` per la versione corrente.

## 1.4.4

- Esito del pulsante "Connetti" WiFi ora colorato e in maiuscolo (verde CONNESSO / rosso NON connesso), piu' facile da notare a colpo d'occhio.
- Pulsante "Mostra/Nascondi" aggiunto automaticamente ad ogni campo password della pagina, per verificare cosa si e' digitato.

## 1.4.3

- Barra del segnale WiFi (colorata, rosso/giallo/verde) aggiunta anche nella scheda Stato, sempre visibile appena aperta la pagina - prima era solo nella scheda Segnali.

## 1.4.2

- Fix: "Controlla aggiornamenti online" falliva con "esp-tls: Failed to create socket" - il limite globale di socket lwIP (10 di default) era gia' quasi saturo tra server web, trasmissione UDP NMEA e client NTRIP. Alzato a 16.

## 1.4.1

- Fix critico: "Controlla aggiornamenti online" faceva andare in crash/riavvio la scheda (stack del task del server web troppo piccolo per l'handshake HTTPS verso GitHub) - portato a 10240 byte, stessa causa del fix precedente sullo stack del task main. Aggiunte anche barre colorate (rosso/giallo/verde) per l'intensita' del segnale, incluso subito dopo "Connetti" in Rete.

## 1.4.0

- Nuovo pulsante "Connetti (senza riavviare)" nella scheda Rete: prova subito una rete WiFi dal vivo, senza dover salvare e riavviare per scoprire se le credenziali sono giuste. Se la connessione riesce viene salvata automaticamente.
- I tentativi di connessione manuali e quelli automatici in background sono ora serializzati (nuovo lock in wifi_link.c) per evitare conflitti sulla stessa interfaccia.

## 1.3.4

- Fix: la ricerca reti WiFi poteva bloccarsi indefinitamente (oltre 30s, confermato su hardware) in conflitto con la connessione automatica in background - aggiunto timeout di sicurezza (10s), stesso schema gia' usato per l'aggiornamento da microSD.

## 1.3.3

- Log di ogni richiesta HTTP in arrivo (metodo + percorso), per poter diagnosticare da seriale se un problema e' di connettivita' (richiesta mai arrivata) o applicativo (richiesta arrivata ma fallita).

## 1.3.2

- Fix: la ricerca reti WiFi falliva quando andava in conflitto con un tentativo di connessione automatica gia' in corso in background - aggiunti tentativi automatici.

## 1.3.1

- Ricerca reti WiFi dalla UI web (scheda Rete): pulsante "Cerca reti WiFi" che mostra le reti disponibili con segnale, click per compilare l'SSID automaticamente.

## 1.3.0

- Repository pubblicato su GitHub (github.com/EVONETRTK/baseesp32-ethelite).
- URL del manifest per l'aggiornamento online precompilato di default sulla release GitHub del progetto.

## 1.2.2

- Aggiornamento da microSD: aggiunto timeout di sicurezza (15s) e log dettagliati sull'inizializzazione SPI/mount, per non lasciare mai la UI web bloccata su un problema hardware della scheda.

## 1.2.1

- Nessun cambio funzionale: build di test per verificare l'aggiornamento da microSD.

## 1.2.0

- Schema di partizioni OTA (otadata + ota_0 + ota_1) con rollback automatico abilitato: un aggiornamento che non si conferma funzionante entro il primo avvio viene annullato da solo dal bootloader, tornando alla versione precedente.
- Nuova scheda "Firmware" nella UI web con tre modalita' di aggiornamento: upload diretto dal browser, da microSD (senza rete), online da un manifest JSON esterno (es. GitHub Releases).
- Nuovo modulo di aggiornamento condiviso (`ota_update.c`) usato da tutte e tre le modalita'.

## 1.1.0

- Rebranding: SSID AP di setup e UI web rinominati da `baseesp32` a `EVONETRTK`.
- Aggiunto campo "Matricola" (identificativo assegnato all'unita' fisica), configurabile dalla UI web (scheda Sicurezza) e mostrato in Stato.
- Aggiunto numero di versione firmware, mostrato in Stato e loggato all'avvio.
- Inizializzato il repository git per questo progetto.

## 1.0.0

Stato del firmware prima dell'introduzione del versionamento esplicito: driver display OLED (SSD1306/SH1106) con schermate di stato/satelliti/segnale, LED RGB opzionale (WS2812 o 3 pin PWM), scheda "Hardware" per pin GNSS/RGB/OLED configurabili da UI, correzione stack overflow WiFi (stack task main portato a 8192 byte), supporto SIM7600X/SIM868 selezionabile per lo slot LTE.
