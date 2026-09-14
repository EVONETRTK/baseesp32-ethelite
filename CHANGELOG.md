# Changelog

Versionamento semantico (MAJOR.MINOR.PATCH). Vedi `main/version.h` per la versione corrente.

## 1.17.2

- Corretto un altro pezzo dello stesso problema "non riesco a collegarmi al WiFi": i nomi di rete rilevati dalla scansione WiFi possono contenere byte non validi come UTF-8 (non tutti i router usano UTF-8 per nomi con caratteri speciali) - il browser li mostrava come punti interrogativi, e se l'utente cliccava su quel risultato per compilare il campo SSID, veniva salvato il nome CON i punti interrogativi al posto dei byte veri: una rete che di fatto non esiste, causa di "SSID non trovato" nonostante password corretta. Ora i byte grezzi dell'SSID vengono trattati come Latin-1 e convertiti in UTF-8 valido prima di mandarli al browser (mai piu' punti interrogativi, nessuna perdita di informazione) e riconvertiti esattamente agli stessi byte originali quando l'utente si collega o salva - la connessione usa quindi sempre il nome vero della rete.

- **Trovata e corretta la causa reale** del problema "il dispositivo continua a disconnettersi e non ricorda la password del WiFi", segnalato dall'utente e diagnosticato con una sessione di log seriale dal vivo: il task del pulsante "Connetti (senza riavviare)" (`wifi_test_connect_task`, stack 4096 byte) andava in **stack overflow reale** esattamente nel momento in cui, dopo una connessione riuscita, chiamava `settings_save()` per salvare le credenziali appena verificate - il crash interrompeva il salvataggio prima di `nvs_commit()`, quindi la password digitata non veniva **mai** scritta su memoria, nonostante l'utente vedesse "connesso" per un istante prima del riavvio improvviso. Stack portato a 8192 byte (stessa causa/fix gia' visto altrove in questo progetto per lo stesso tipo di problema). Aggiunta anche una diagnostica permanente in `settings_save()` (una riga di log ad ogni salvataggio, con verifica di rilettura immediata) che ha reso possibile individuare il crash con certezza invece che per ipotesi.
- Le vecchie letture "gnss_uart_num non valido (5005)" viste nei log altro non erano che lo stesso identico blob NVS mai aggiornato con successo per questo motivo, da prima ancora dei fix di persistenza di 1.13.2/1.14.0 - una volta che un salvataggio va a buon fine con questa versione, si corregge da solo, senza bisogno di cancellare di nuovo la NVS.

- **Archivio firmware su SD** (`main/fw_archive.c`): ad ogni aggiornamento applicato (da browser, SD o online/automatico) il dispositivo salva da solo su `/sdcard/firmware/` una copia della versione appena sostituita, tenendo le ultime 2. Nuova sezione "Archivio firmware" nella pagina Firmware per ripristinare manualmente una qualunque versione archiviata, ignorando deliberatamente il controllo versione (a differenza degli aggiornamenti normali) - per i casi in cui un problema si scopre solo dopo piu' aggiornamenti, oltre il singolo passo gia' coperto dal rollback automatico del bootloader. Mai automatico, richiede sempre un'azione esplicita con conferma.
- **Log diagnostici persistenti su SD** (`main/diag_log.c`): oltre al buffer in RAM (8KB, perso al riavvio) gia' esistente, ogni sessione di avvio scrive ora il proprio log anche su `/sdcard/diag_logs/` (un file per avvio, rotazione tra 5, tetto ~200KB ciascuno) - utile per diagnosticare un problema successo quando nessuno era collegato alla pagina web. Scarico non bloccante e periodico (ogni 30s), monta la SD solo per la durata dello scarico per non bloccare le altre funzioni che la usano.

- Diagnosticato un problema reale di connettivita' WiFi segnalato dall'utente: il log seriale ha mostrato che il nome della rete AP di setup, l'host del caster NTRIP e i pin UART del GNSS salvati risultavano vuoti/non validi nella configurazione caricata da NVS (la causa originaria non e' stata individuata con certezza nonostante l'analisi - non sembra riconducibile ai fix di persistenza gia' fatti in 1.13.2/1.14.0, che restano corretti). Aggiunta una rete di sicurezza aggiuntiva in `settings_init()`: se il nome o la password della rete AP risultano vuoti, vengono rigenerati (stesso meccanismo gia' esistente per la matricola), cosi' il dispositivo non diventa mai irraggiungibile per questo motivo specifico. Cancellata la NVS del dispositivo di test per ripartire da una configurazione pulita - richiede una riconfigurazione completa da parte dell'utente.

- Aggiunta nella pagina Stato una barra del segnale cellulare (stessa forma/colori di quella WiFi gia' presente) con operatore e tecnologia, cosi' non serve piu' passare dalla pagina Segnali per vederli. Solo modifiche alla pagina web.

## 1.16.0

- Nuovo controllo automatico degli aggiornamenti online (pagina Firmware, sotto "Aggiornamento online") - **disattivato di default**, si attiva esplicitamente dalla UI. Se attivo, il dispositivo controlla da solo ogni N ore (configurabile) se c'e' un firmware piu' recente all'indirizzo manifest configurato e, se si', lo scarica e si riavvia da solo - stesse protezioni anti-brick gia' esistenti (rollback automatico se la nuova immagine non si conferma valida). Pensato per un dispositivo raggiungibile solo via cellulare (SIM7600/SIM868): il controllo/download e' un collegamento in USCITA, funziona anche dietro il NAT condiviso dall'operatore che invece impedisce di raggiungere la pagina web da remoto per avviarlo a mano. Nuovo `main/auto_update.c`.

- Nuova funzione "Registrazione dati per PPP" (pagina GNSS & NTRIP, solo modalita' base): registra su microSD il flusso RTCM3 grezzo del GNSS per diverse ore da un punto fisso, con avvio/arresto **sempre manuale dalla UI web**, mai automatico. Il file scaricato dal browser si converte in RINEX (nella versione richiesta da ciascun servizio - 2.11, 3.03, ecc.) con lo strumento gratuito `convbin` di RTKLIB sul proprio PC: un'unica registrazione basta per generare il formato richiesto da qualunque ente di post-processing PPP (CSRS-PPP, OPUS, AUSPOS...). Verificato: CSRS-PPP (Natural Resources Canada) non offre un indirizzo email di invio diretto - richiede login con account e upload autenticato via modulo web, quindi l'invio ai servizi PPP resta un passaggio manuale fatto dall'utente, non automatizzato dal firmware (nessuna credenziale di servizi esterni salvata sul dispositivo per questo).
- Nuovo `main/ppp_log.c`: registrazione non bloccante (stream buffer + task dedicato, stesso schema gia' usato per il server caster NTRIP locale) per non rallentare la lettura UART del GNSS; nuovi endpoint `/api/ppp-log/start`, `/api/ppp-log/stop`, `/api/ppp-log/download` (a blocchi, mai l'intero file in RAM).

- Interfaccia web ridisegnata per essere piu' leggibile a colpo d'occhio, pensata per chi non conosce il firmware nel dettaglio (es. un operatore in campo): icone su tutte le schede/sezioni, e una nuova barra di riepilogo rapido in cima alla pagina (visibile su ogni scheda) con 4 indicatori colorati - Rete, NTRIP, Fix RTK, Scheda SD - verde/giallo/rosso a seconda dello stato, aggiornati automaticamente come il resto della pagina. Icone di stato (✅/⚠️/❌) aggiunte anche ai testi di stato gia' esistenti (NTRIP, fix RTK, SD, spostamento base) per rinforzare il colore con una forma riconoscibile anche in bianco e nero o per chi ha difficolta' a distinguere i colori.
- **Fix di affidabilita' del boot**: durante i test di questa versione e' ricomparso un crash al boot in `uart_driver_install` per un `gnss_uart_num` non valido letto dalla configurazione salvata - con causa non del tutto chiarita (il blob NVS coinvolto aveva gia' il nuovo magic "bs02" introdotto in 1.13.2, quindi non sembra lo stesso caso gia' risolto la'; risolto contingentemente cancellando la partizione NVS del dispositivo di test). Invece di continuare a inseguire l'esatta causa nel formato di salvataggio, `settings_init()` ora valida esplicitamente `gnss_uart_num` (deve essere 0/1/2) e `gnss_uart_baud` (deve essere >= 1200) dopo averli caricati, riportandoli al default di Kconfig se fuori range - una rete di sicurezza che rende impossibile un crash al boot per questa causa specifica, qualunque cosa finisca nel blob salvato in futuro.

## 1.13.2

- **Fix importante**: gli aggiornamenti firmware che aggiungono un nuovo campo alle impostazioni (WiFi, matricola, GNSS, ecc.) non cancellano piu' l'intera configurazione salvata. Finora un confronto esatto della dimensione del blob NVS scartava tutto (tornando ai default di fabbrica, incluso il WiFi) ad ogni singolo aggiornamento che aggiungeva anche un solo campo - causa dei ripetuti "non si collega piu' al WiFi dopo l'aggiornamento" di questa sessione. Ora si copiano solo i byte realmente presenti nel salvataggio precedente, lasciando i campi nuovi al valore di default finche' non li si imposta dalla UI - ma **solo** per i blob salvati da questa versione in poi (nuovo magic "bs02"): i blob di versioni precedenti vengono scartati come prima (un ultimo reset, inevitabile) invece di essere fusi, perche' il loro layout non garantisce che i campi siano stati solo aggiunti in fondo (in questa sessione un campo e' stato cambiato di tipo/dimensione a parita' di posizione - `oled_is_sh1106` -> `oled_controller` - e un primo tentativo di merge tollerante su quei byte ha prodotto un `gnss_uart_num` spazzatura e un crash al boot, individuato e corretto prima del commit). Da qui in avanti la regola "solo aggiunte in fondo alla struct" e' garantita (vedi commento in `main/settings.h`), quindi il merge e' sicuro.

## 1.13.1

- Nella pagina GNSS & NTRIP, quando e' disponibile una posizione rilevata dalla base (vedi 1.13.0), compare ora un link "Vedi la posizione rilevata su Google Maps" per un controllo visivo immediato senza dover trascrivere le coordinate altrove.

## 1.13.0

- Aggiunta la posizione base "manuale" (solo chip Quectel LC29H per ora): oltre al survey-in automatico ad ogni avvio (comportamento storico), ora si possono impostare coordinate fisse note - ad esempio ottenute da un servizio di post-processing PPP (CSRS-PPP, OPUS, ecc.), molto piu' precise del solo survey-in. Nuovo `main/geo_convert.c` per la conversione lat/lon/quota WGS84 <-> ECEF (richiesta dal comando `$PQTMCFGSVIN` del modulo in modalita' "fixed"). Per evitare errori di trascrizione, la UI web propone come default l'ultima posizione rilevata dal ricevitore stesso (dai messaggi RTCM 1005/1006 gia' inoltrati al caster, ora tracciati anche oltre alla sola baseline dell'avviso di spostamento) con un pulsante "Usa posizione attuale rilevata": nella maggior parte dei casi basta verificare e confermare, senza scrivere numeri a mano.

## 1.12.2

- La scheda "Scheda microSD" nello Stato mostra ora anche la capacita' totale e lo spazio usato con una barra colorata (verde/giallo/rosso in base alla percentuale occupata), letti con `esp_vfs_fat_info()` subito dopo il mount.

## 1.12.1

- Aggiunto "Scheda microSD" nella scheda Stato della UI web: mostra se l'ultimo controllo (all'avvio o dal pulsante manuale) ha rilevato la scheda, colorato (verde rilevata / rosso non rilevata), invece di dover guardare il log seriale per saperlo.

## 1.12.0

- La scheda microSD viene ora controllata automaticamente **ad ogni avvio** (prima solo premendo il pulsante "Controlla e aggiorna da SD" nella UI web) - riusa la stessa funzione gia' esistente, quindi stesse sicurezze: nessun effetto se non c'e' una scheda inserita o non c'e' un aggiornamento piu' recente di quello attuale, e il file `firmware.bin` viene rinominato dopo l'uso per non riapplicarlo ad ogni riavvio.

## 1.11.1

- Aggiunta una schermata all'OLED (in rotazione con le altre) che mostra il nome della rete WiFi di configurazione e l'indirizzo fisso `192.168.4.1` per raggiungere la pagina web - utile a chi e' fisicamente in campo e non sa gia' a memoria come collegarsi, senza dover passare da un laptop o da un altro dispositivo gia' configurato.

## 1.11.0

- Aggiunto **SSD1309** come terza opzione esplicita per il controller del display OLED (tipico sui moduli da 2,42"), oltre a SSD1306 e SH1106 gia' presenti - stesso percorso di SSD1306 nel driver (compatibile a livello di comandi nella stragrande maggioranza dei moduli in commercio), ma mostrato come scelta distinta nella UI invece di dover selezionare "SSD1306" per un chip diverso. Il campo `oled_is_sh1106` (booleano) e' diventato `oled_controller` (tre valori: ssd1306/sh1106/ssd1309).

## 1.10.0

- Aggiunto un server caster NTRIP locale opzionale (solo modalita' base): oltre a inoltrare l'RTCM3 al caster esterno gia' configurato, il dispositivo puo' ora accettare direttamente connessioni da rover (protocollo NTRIP standard, porta di default 2101, con mountpoint/utente/password dedicati) - utile in campo senza internet, dove base e rover si scambiano le correzioni sulla stessa rete WiFi locale senza bisogno di un caster esterno. Funziona sempre in locale; per essere raggiungibile anche da internet senza passare da un caster serve configurare il port forwarding sul proprio router (e un DNS dinamico se l'IP pubblico non e' fisso) - **non funzionera' quasi certamente sui dati del modem cellulare**, dietro NAT condiviso dall'operatore nella grande maggioranza dei casi, un limite di rete che nessun firmware puo' aggirare. Nuovo modulo `ntrip_caster_server.c`, fino a 4 rover collegati insieme.

## 1.9.0

- Aggiunto avviso (email/WhatsApp, stessi canali gia' configurabili per il caster) se l'antenna della base si sposta rispetto alla posizione registrata al primo avvio - pensato per accorgersi se la base viene urtata da un mezzo agricolo o spostata dal vento, cosa che altrimenti manderebbe correzioni sbagliate a tutti i rover collegati senza nessun segnale visibile sul posto. Nuovo modulo `rtcm3_1005.c`: analizza (senza alterarlo) lo stesso stream RTCM3 gia' inoltrato al caster in modalita' base, cercando frame di tipo 1005/1006 (posizione ECEF dell'antenna) - preambolo, CRC24Q e bit layout presi dallo standard RTCM 10403.x ufficiale, verificati ma non ancora contro un flusso RTCM reale (nessun modulo GNSS fisico collegato in questa sessione). Soglia di distanza configurabile (default 5 m) nella scheda Sicurezza, con stato "posizione di riferimento"/"spostamento attuale" visibile in tempo reale.

## 1.8.0

- Aggiunto supporto per il ricevitore GNSS Quectel LC29H (varianti BA/CA/DA/EA) come terza opzione oltre a u-blox e Unicore, sia in modalita' base (survey-in + RTCM3 MSM7) sia rover. Comandi ($PQTM..., $PAIR...) presi dalla documentazione ufficiale Quectel (protocollo V1.4), con checksum NMEA calcolato a runtime - non ancora verificato su hardware reale (modulo non disponibile in questa sessione). Nuovo modulo `gnss_lc29h.c`.
- **Nota per chi usa questo modulo**: a differenza di u-blox/Unicore, cambiare modalita' base/rover su questo chip richiede uno spegnimento/riaccensione fisico del modulo GNSS (non basta "Salva e riavvia", che riavvia solo l'ESP32) - limite del chip stesso, documentato da Quectel, non di questo firmware. Inoltre il modulo di fabbrica usa 460800 baud (non 115200): va impostato nella scheda Hardware.

## 1.7.1

- Fix (confermato su hardware reale: "spesso si blocca" durante il test WiFi): il pulsante "Connetti (senza riavviare)" bloccava l'intero server web fino a 15s in attesa dell'esito - durante il tentativo la radio deve spostarsi sul canale della rete di destinazione per autenticarsi, il che puo' disturbare momentaneamente il collegamento della pagina stessa (sempre sull'AP di setup, canale 1); se capitava proprio in quel momento, la richiesta restava sospesa a tempo indeterminato senza nessun errore visibile. Il test ora gira in un task separato (stesso schema gia' usato per l'aggiornamento online), con la pagina che interroga l'esito periodicamente e tollera qualche fallimento di rete transitorio invece di restare bloccata in attesa di un'unica risposta.

## 1.7.0

- Aggiunto avviso email e/o WhatsApp quando la connessione al caster NTRIP resta interrotta oltre una soglia configurabile (default 15 minuti), piu' un secondo avviso quando torna a funzionare - pensato per una base lasciata incustodita in campo. Email via client SMTP minimale integrato (SMTPS, AUTH LOGIN - va bene Gmail con una password per le app); WhatsApp tramite il servizio gratuito di terze parti CallMeBot. Nuova sezione "Avviso caster disconnesso" nella scheda Sicurezza, con un pulsante "Invia avviso di prova" che usa subito i valori del form senza dover salvare prima. Nuovo modulo `alerts.c`.
- **Nota**: questa versione aggiunge nuovi campi a `app_settings_t` - come per ogni cambio di questo tipo, la configurazione salvata (WiFi, NTRIP, ecc.) viene reimpostata ai default al primo avvio con questo firmware e va reinserita.

## 1.6.6

- Aggiunta temperatura interna del chip nella scheda Stato (sezione Sistema), colorata (verde sotto 50°C, arancione 50-70°C, rosso oltre 70°C) - soglie puramente indicative per capire a colpo d'occhio se il dispositivo si sta scaldando piu' del normale (es. sole diretto su una custodia chiusa in campo), non un limite di sicurezza del chip. Usa il sensore di temperatura interno dell'ESP32-S3 (`sys_stats.c`, componente `esp_driver_tsens`).

## 1.6.5

- La Matricola di default ora usa lo stesso numero breve gia' mostrato nel nome della rete WiFi di configurazione (es. "EVONETRTK-893428" -> "893428"), invece del MAC per esteso introdotto nella 1.6.4 - stesso identificativo in entrambi i punti della UI invece di due formati diversi. Migrazione automatica anche per i dispositivi gia' passati alla 1.6.4.

## 1.6.4

- La Matricola ora ha come default il MAC address di fabbrica del chip (identificativo hardware unico), invece di restare vuota - resta comunque modificabile dalla UI web con un proprio codice. Applicato anche ai dispositivi gia' configurati in precedenza (migrazione automatica al primo avvio con questo firmware, solo se il campo era ancora vuoto).

## 1.6.3

- Aggiunto stato della connessione al caster NTRIP, ben visibile in cima alla scheda Stato: CONNESSO/NON CONNESSO (colorato), da quanto tempo, numero di riconnessioni dal boot e motivo dell'ultimo errore (DNS, connessione rifiutata, caster che ha chiuso, ecc.) - prima non c'era nessuna visibilita' su questo, solo log interni. Funziona sia in modalita' base (upload RTCM) sia rover (download RTCM), tracciato nel modulo condiviso `status.c`.
- Fix critico (confermato su hardware reale: crash/riavvio in loop appena provava a connettersi al caster): lo stack dei task NTRIP (4096 byte) andava in overflow proprio nel nuovo codice di tracciamento stato aggiunto sopra, in particolare al fallimento della DNS lookup - portato a 8192 byte per entrambi i task (base e rover), stesso margine gia' usato altrove nel progetto per problemi analoghi.

## 1.6.2

- Aggiunto uso CPU (per core, con barra colorata) e memoria (libera, minima mai raggiunta, totale, PSRAM se presente) nella scheda Stato. Nuovo modulo `sys_stats.c`, basato sul tempo di esecuzione del task IDLE di ciascun core (richiede `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`, overhead trascurabile). La percentuale CPU compare dal secondo aggiornamento in poi (serve un campione di riferimento).

## 1.6.1

- Aggiunta scheda "Log" nella UI web: ultime righe di log (fino a 8KB, dall'avvio) leggibili dal browser senza bisogno di un cavo USB/seriale, utile soprattutto sul campo. Nuovo modulo `log_buffer.c` che intercetta i log ESP-IDF gia' in transito verso la UART (che continuano a funzionare come sempre) e ne tiene una copia in un buffer circolare in RAM.

## 1.6.0

- Aggiunto stato del fix GNSS/RTK in tempo reale: tipo di fix (RTK fisso/float/GPS/DGPS/nessun fix, colorato rosso/blu/arancione/verde), satelliti usati nel fix, HDOP, eta' delle correzioni RTCM ed eta' dell'ultimo fix ricevuto - un riepilogo rapido nella scheda Stato e i dettagli completi nella scheda Segnali. Letto dalle sentenze $xxGGA gia' in transito verso il client NTRIP rover (nuovo modulo `gnss_fix.c`), nessun impatto sul percorso dati esistente. Come per il grafico satelliti, popolato solo in modalita' rover mentre il ricevitore trasmette NMEA.

## 1.5.14

- Nessun cambio funzionale: build di test per verificare su hardware reale che la barra mostri l'esito corretto dopo il fix del riavvio (1.5.13), a partire da un dispositivo gia' su 1.5.13.

## 1.5.13

- Fix (causa reale della barra ancora bloccata, confermato su hardware: aggiornamento riuscito - versione nuova visibile dopo aver ricaricato la pagina a mano - ma la barra ferma): dopo il riavvio il nuovo avvio del firmware riparte con uno stato di avanzamento "vuoto" identico a quello di "mai avviato" (running e done entrambi false) - se la richiesta della pagina web riesce comunque contro il server appena ripartito, invece di fallire come previsto, il codice non se ne accorgeva e restava in attesa per sempre. Ora, se si era visto un download realmente in corso e poi lo stato torna improvvisamente "vuoto" senza mai essere passato da "completato", viene trattato come un riavvio riuscito allo stesso modo di una connessione persa (verifica automatica della versione via /api/status).

## 1.5.12

- Nessun cambio funzionale: build di test per verificare su hardware reale che la barra di avanzamento mostri l'esito corretto (non piu' bloccata) dopo un aggiornamento riuscito, a partire da un dispositivo gia' su 1.5.11.

## 1.5.11

- Fix reale trovato: l'aggiornamento online in realta' funzionava gia' (confermato su hardware: il dispositivo si riavviava correttamente sulla nuova versione) - il problema era che l'asset del firmware sulle release GitHub non si chiamava mai davvero "firmware.bin" come da URL previsto (era rimasto "baseesp32-ethelite.bin", il nome del file locale: la sintassi `file#nome` di `gh release create` imposta solo un'etichetta visualizzata, non il nome scaricabile) - `firmware.bin` era un 404 reale su ogni release pubblicata finora. Da qui in avanti l'asset viene rinominato localmente prima della pubblicazione.
- Fix UI: la barra di avanzamento restava bloccata all'ultima percentuale vista se il dispositivo si riavviava (con successo) prima che il browser ricevesse l'ultima risposta "completato" - confermato su hardware reale ("bloccato a 55%" con aggiornamento in realta' riuscito). Ora, se la connessione cade dopo che il download era gia' partito, la pagina interroga automaticamente lo stato del dispositivo ogni 2s finche' non torna raggiungibile, confrontando la versione per dare un esito reale invece di restare bloccata.

## 1.5.10

- Nessun cambio funzionale: build di test per verificare su hardware reale il download/installazione completo dopo il fix del redirect (1.5.9), a partire da un dispositivo gia' su 1.5.9.

## 1.5.9

- Fix (causa esatta confermata dal log): "Aggiornamento fallito, avvio del download non riuscito" - la risoluzione dell'URL finale del firmware usava una richiesta HEAD, ma l'endpoint di redirect degli asset di GitHub Releases risponde "404 File not found" a una HEAD pur reindirizzando correttamente con GET (confermato su hardware: "Hop 2 esito: status=404" seguito da "esp_https_ota: File not found(404)"). Sostituita con una GET con header "Range: bytes=0-0", che ottiene lo stesso risultato (nessun corpo scaricato inutilmente) senza l'errore.

## 1.5.8

- Nessun cambio funzionale: build di test per verificare su hardware reale il download/installazione con la nuova barra di avanzamento (1.5.7), a partire da un dispositivo gia' aggiornato a 1.5.7.

## 1.5.7

- Aggiunta barra di avanzamento (percentuale + KB scaricati) durante il download/installazione dell'aggiornamento online - prima la pagina restava bloccata in attesa senza nessuna indicazione, ora l'installazione gira in un task separato e la UI interroga lo stato ogni 800ms.
- Fix probabile della causa di "Aggiornamento fallito" sull'installazione online (non ancora confermato su hardware): applicato anche qui lo stesso fix "Accept-Encoding: identity" gia' confermato necessario per il controllo versione (1.5.4), perche' esp_https_ota() usa un client HTTP interno che non condivide quel fix - un'immagine ricevuta compressa verrebbe scritta cosi' com'e' e fallirebbe la validazione. Il messaggio d'errore ora distingue anche un download interrotto da un'immagine ricevuta ma non valida.

## 1.5.6

- Nessun cambio funzionale: build di test per verificare il download/installazione reale (non solo il controllo) dell'aggiornamento online, dopo che v1.5.5 ha risolto la catena di controllo.

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
