# Changelog

Versionamento semantico (MAJOR.MINOR.PATCH). Vedi `main/version.h` per la versione corrente.

## 1.5.5

- Fix (vera causa, confermata dal log completo): l'URL firmato finale include anche un token JWT lungo, per un totale ben oltre i 600 byte gia' aumentati - confermato troncato a meta' del JWT nel log, producendo una richiesta malformata verso il CDN. Buffer URL portato a 1536 byte.

## 1.5.4

- Fix: i 457 byte ricevuti dal CDN di GitHub Releases risultavano vuoti/non leggibili come testo - probabile contenuto compresso (gzip) che esp_http_client non decomprime da solo. Aggiunto header "Accept-Encoding: identity" per richiedere esplicitamente contenuto non compresso.

## 1.5.3

- Fix (finalmente confermato con la causa esatta): il CDN di GitHub Releases (release-assets.githubusercontent.com) restituisce a volte uno status HTTP non valido (es. 618) pur trasferendo il contenuto correttamente (457 byte ricevuti, coerenti col file reale, nessun errore di trasporto) - probabile limite del parser dello status in esp_http_client su questo servizio. Non ci si affida piu' allo status come unico segnale: se il contenuto ricevuto e' JSON valido (per il manifest) o la risoluzione dell'URL non ha avuto errori di trasporto (per il download), si procede comunque.

## 1.5.2

- Diagnostica: lo status 618 (non standard) sul controllo aggiornamenti online si ripresenta nonostante il fix dei buffer - aggiunto log dettagliato di ogni hop (URL, esito, lunghezza contenuto, header Location) per capire esattamente dove/come accade, indipendentemente da quando si legge il log.

## 1.5.1

- Aggiunto mDNS: la scheda e' ora raggiungibile anche con un nome fisso (es. http://EVONETRTK-893428.local) invece del solo indirizzo IP, che cambia a seconda della rete a cui ci si collega (AP di setup vs rete WiFi di casa). Mostrato nella scheda Stato.

## 1.5.0

- Login della pagina web ora "ricordato" con un cookie di sessione (~30 giorni) dopo il primo accesso, invece di affidarsi solo alla cache del popup Basic Auth del browser - utile soprattutto passando tra l'IP dell'AP e quello della rete di casa, prima trattati come siti diversi con richiesta di credenziali separata.

## 1.4.9

- Fix: i redirect ora funzionano (confermato: 2 hop seguiti correttamente fino al CDN di GitHub), ma l'URL finale firmato (token SAS, molto lungo) veniva troncato dai buffer da 256 byte, causando una richiesta corrotta. Buffer portati a 600 byte, piu' margine sui buffer HTTP interni (2048 byte).

## 1.4.8

- Fix: la risposta 302 di GitHub arrivava correttamente ma esp_http_client_get_header() non trovava l'header Location dopo esp_http_client_perform() (confermato su hardware: "Redirect (status 302) senza header Location"). L'header ora viene catturato durante la ricezione, tramite il gestore eventi HTTP_EVENT_ON_HEADER, non piu' letto a posteriori.

## 1.4.7

- Fix: la connessione WiFi entrava in un ciclo continuo di connessione/disconnessione ogni 2-3s, mai stabile - confermato su hardware (stati auth->assoc->run->init ripetuti). Causa: un riconnessione immediata e senza pausa dentro il gestore eventi ad ogni disconnessione, in competizione col ritentativo gia' gestito da net_manager_task. Rimossa - ora solo net_manager_task ritenta, con timeout pieno. Aggiunto anche il log del motivo di ogni disconnessione WiFi per diagnosi future.

## 1.4.6

- Fix: "Controlla aggiornamenti online" falliva sempre con status 302 - confermato su hardware ("err=ESP_FAIL status=302"): il redirect automatico di esp_http_client verso gli URL "latest" di GitHub Releases non funzionava in modo affidabile. Ora i redirect vengono seguiti manualmente (fino a 3 hop), sia per il manifest sia per il file .bin prima di passarlo a esp_https_ota().

## 1.4.5

- Fix: il pulsante "Connetti" WiFi andava in timeout (15s) senza mai completarsi se la scheda era gia' connessa a un'altra rete - il driver WiFi rifiuta una nuova connessione senza prima disconnettersi esplicitamente da quella attuale. Confermato su hardware ("sta is connected, disconnect before connecting to new ap").

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
