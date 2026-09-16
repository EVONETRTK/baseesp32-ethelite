# Changelog

Versionamento semantico (MAJOR.MINOR.PATCH). Vedi `main/version.h` per la versione corrente.

## 1.19.43

- Richiesta dell'utente (altri suggerimenti UX proposti e accettati): cinque novita':
  1. Le checkbox RTCM non supportate dal chip GNSS scelto (es. 1007/1008/1019/1020 su u-blox) si disattivano e sbiadiscono da sole invece di restare spuntabili e scoprire solo dal log che vengono ignorate.
  2. Pulsante "Annulla modifiche non salvate" nel riquadro RTCM - ricarica dal dispositivo i valori gia' salvati, scartando esperimenti fatti sul form.
  3. Stato Ethernet (collegato/IP) visibile nella scheda Stato - prima l'interfaccia, appena attivata in 1.19.40, non aveva nessuna visibilita' in UI.
  4. Validazione di formato per l'host NTRIP prima di salvare (niente spazi, "http://" davanti o "/" in fondo) - un errore di battitura si scopre subito, non solo quando la connessione fallisce.
  5. Nuovo riquadro "Byte RTCM per messaggio" in Segnali (solo base): quanto pesa davvero ciascun messaggio scelto, dal boot. Nuovo modulo `rtcm3_stats.c` (scanner di frame RTCM3 leggero, senza validazione CRC - solo diagnostica, riusa lo stesso schema di stato gia' verificato in `rtcm3_1005.c`).

## 1.19.42

- **Richiesta dell'utente ("altri software si può scegliere 1005 1007 ecc")**: la selezione messaggi RTCM di 1.19.41 (menu a tendina off/MSM4/MSM7 per costellazione) è stata sostituita da **14 checkbox indipendenti, uno per numero messaggio** (1005, 1007, 1008, 1019, 1020, 1074, 1077, 1084, 1087, 1094, 1097, 1124, 1127, 1230), nessun vincolo tra loro - stile piu' vicino ad altri software NTRIP/base. Aggiunto anche il supporto per **1007/1008** (descrittore antenna) e **1019/1020** (effemeridi GPS/GLONASS), non presenti nella selezione precedente.
- Verificato prima di aggiungerli che **u-blox non supporta affatto 1007/1008/1019/1020 in uscita** (nessuna chiave CFG-MSGOUT corrispondente esiste, stessa fonte SparkFun gia' usata per il fix delle chiavi in 1.19.41): se spuntati su un dispositivo configurato per u-blox vengono ignorati con un log invece di fallire in silenzio o fingere supporto. Stesso trattamento onesto per Quectel LC29H (nessun controllo individuale su questi messaggi, solo l'interruttore MSM4/7 globale gia' documentato in 1.19.41).
- I 4 campi `rtcm_*_msm` di 1.19.41 restano nella struct impostazioni (rinominati `_DEPRECATED`, non piu' letti/scritti) per non spostare l'offset dei campi salvati su NVS - regola "solo in fondo, mai rimosso" gia' seguita per tutto il resto del progetto.

## 1.19.41

- **Richiesta dell'utente: i messaggi RTCM3 inviati dalla base sono ora selezionabili dalla UI** invece di un set fisso deciso dal firmware ("devo poter dire io 1005 1074 ecc, alcune antenne potrebbero riempire la memoria"). Nuovo riquadro "Messaggi RTCM inviati" (pagina GNSS & NTRIP): livello MSM (spento/MSM4/MSM7) per ciascuna costellazione (GPS/GLONASS/Galileo/BeiDou) + interruttori per 1005 (posizione base) e 1230 (bias GLONASS), con 3 preset rapidi (Standard MSM4, Massima precisione MSM7, Solo GPS+GLONASS) oltre alla scelta libera. Applicato a tutti e tre i chip supportati (u-blox, Unicore, Quectel LC29H) - il LC29H ha pero' solo un interruttore MSM4/MSM7 globale lato modulo, non per costellazione: documentato onestamente in UI invece di far finta di un controllo che l'hardware non offre.
- **Bug reale trovato e corretto nel driver u-blox verificando le chiavi contro una fonte affidabile** (sparkfun/SparkFun_u-blox_GNSS_Arduino_Library) prima di aggiungere le nuove: quasi tutte le chiavi UBX-CFG-MSGOUT-RTCM_3X_TYPE*_UART1 gia' in uso erano sbagliate (spostate di una posizione, es. 1005 era 0x209102bd invece di 0x209102be), e le due chiavi CFG-TMODE-SVIN-MIN-DUR/SVIN-ACC-LIMIT erano scambiate tra loro (60 secondi finiva nella chiave di precisione, 2500 in quella di durata). La configurazione RTCM3 della base su hardware u-blox molto probabilmente non ha mai funzionato correttamente prima di questo fix - una chiave sbagliata viene rifiutata in silenzio dal ricevitore, senza errore visibile.

## 1.19.40

- **Richiesta dell'utente: attivata davvero l'Ethernet (W5500 via SPI) sul T-ETH-Elite**, prima disattivata di default e mai compilata con successo. Verificato prima il pinout ufficiale contro `Xinyuan-LilyGO/LilyGO-T-ETH-Series` (`examples/HelloServer/utilities.h`, sezione `LILYGO_T_ETH_ELITE_ESP32S3`): i pin gia' presenti nel Kconfig del progetto (MISO=47, MOSI=21, SCLK=48, CS=45, INT=14, RST=-1, indirizzo PHY=1) combaciano esattamente - nessuna modifica necessaria li'. Due bug reali trovati compilando per la prima volta con l'opzione attiva:
  1. `eth_link.c` non includeva `esp_eth_mac_spi.h` (le dichiarazioni W5500 non arrivano da `esp_eth_mac.h`).
  2. Serviva anche l'opzione Kconfig del *componente* `esp_eth` stesso (`CONFIG_ETH_SPI_ETHERNET_W5500`), separata e indipendente dalle opzioni del progetto (`BASEESP32_ETH_*`, che configurano solo i pin) - senza quella, l'intero blocco W5500 di `esp_eth_mac_spi.h` non viene nemmeno compilato nel componente.
  Flash: 28% libero (da 29%), impatto minimo.

## 1.19.39

- **Fix segnalato dall'utente ("la ricerca non funziona, man mano che scrivo devono apparire le voci")**: la ricerca tra le impostazioni (1.19.38) non mostrava mai i risultati - `results.style.display = ''` rimuoveva lo stile inline ma la regola CSS base per `#settings-search-results` e' `display:none`, quindi il riquadro restava sempre nascosto anche con risultati trovati. Corretto impostando esplicitamente `'block'`.

## 1.19.38

- Richiesta dell'utente (ulteriori suggerimenti UX proposti e accettati "tutti"): cinque novita':
  1. Ricerca tra le impostazioni (casella in alto): digita qualche lettera del nome di un parametro e salta direttamente al riquadro giusto, evidenziato, invece di ricordare a memoria in quale scheda si trova.
  2. "Ultimo salvataggio" per riquadro (localStorage lato browser, non richiede nulla dal firmware): sai quando hai davvero applicato una config, anche dopo aver ricaricato la pagina.
  3. Pulsante "Scarica log (.txt)" nella scheda Log, oltre alla sola vista a schermo.
  4. Nuovo "Scarica per clonare su un nuovo dispositivo" nel Backup configurazione: come l'esportazione normale ma senza matricola/SSID AP (identita' che deve restare unica per ogni base fisica).
  5. Avviso ⚠️ in Firmware se il controllo automatico aggiornamenti e' attivo ma non risulta riuscito da piu' del doppio dell'intervallo configurato - nuovo tracciamento `status_note_online_update_checked()`/`status_get_last_online_update_check_us()` (status.h/.c), aggiornato da `online_update_check()` (condiviso da controllo manuale e automatico) solo quando arriva davvero un manifest valido, non per un tentativo fallito per rete assente.

## 1.19.37

- Richiesta dell'utente (altri suggerimenti UX proposti e accettati "tutti"): cinque miglioramenti:
  1. Conferma esplicita prima di "Riavvia dispositivo" (scheda Stato) - un click accidentale non interrompe piu' il flusso RTCM senza preavviso.
  2. Puntino ambra sui pulsanti dei tab di navigazione se quella pagina ha campi modificati-non-salvati - visibile anche cambiando scheda.
  3. Nuovo pulsante "Prova connessione al caster" nel riquadro NTRIP rover: testa subito l'handshake (host/porta/mountpoint/credenziali, anche non salvati) invece di aspettare il ciclo di retry automatico. Nuova `ntrip_rover_client_test_connect()` + endpoint `POST /api/ntrip/test-connect`.
  4. Note di rilascio della versione corrente mostrate nella pagina Firmware (nuovo `FIRMWARE_RELEASE_NOTES` in version.h, esposto come `firmware_release_notes`).
  5. Pulsante "copia" (📋) accanto a IP attuale e indirizzo mDNS in Stato - comodo da mobile.

## 1.19.36

- Richiesta dell'utente: la matricola (scheda Sicurezza) e' ora di sola lettura per default - va spuntata esplicitamente "Forza modifica" per poterla cambiare, e il salvataggio chiede conferma esplicita (come gia' per admin_code/AP). Nuovo campo `device_serial_from_mac` in `/api/status` (funzione `settings_device_serial_from_mac()`, stessa logica del default gia' usata in `apply_defaults()`): se la matricola salvata non corrisponde a quella ricavata dal MAC del chip, un avviso ⚠️ compare sia in Sicurezza sia accanto alla Matricola nella scheda Stato.

## 1.19.35

- Richiesta dell'utente (suggerimenti UX proposti e accettati "tutti"): sei miglioramenti alla UI web:
  1. Conferma esplicita prima di salvare `admin_code` o `ap_ssid`/`ap_password` - campi che, se sbagliati, possono tagliare fuori dalla pagina.
  2. Campi modificati ma non ancora salvati evidenziati (bordo/sfondo ambra) finche' non si preme il 💾 del loro riquadro.
  3. I riquadri Hardware che richiedono un riavvio (Antenna GNSS, LED RGB, Display OLED) lo segnalano subito nel messaggio dopo il salvataggio, non solo in una nota generica in fondo pagina.
  4. Nuova scheda "💾 Backup configurazione" (pagina Firmware): scarica un JSON con tutte le impostazioni (password escluse, il dispositivo non le restituisce mai) e lo puo' reimportare - utile per backup o per clonare la config su un'altra base.
  5. Il messaggio "Salvato"/"Errore" di un riquadro resta visibile finche' non si ritocca quel riquadro, invece di sparire da solo dopo ~2 secondi.
  6. I chip di stato in alto (📶 Rete, 📡 NTRIP, 🎯 Fix, 💾 SD) sono ora cliccabili e portano direttamente alla pagina pertinente.

## 1.19.34

- Richiesta dell'utente: il pulsante 💾 per-campo introdotto in 1.19.32 (uno per ciascuno dei 56 parametri) e' stato sostituito da **un pulsante di salvataggio per riquadro** (18 in totale, uno per fieldset con almeno un campo modificabile) - es. "Server caster NTRIP locale" (porta, mountpoint, username, password) o "Modalita' operativa" (funzione dispositivo, chip GNSS) si salvano insieme con un solo click, invece di uno per campo. La scheda WiFi resta invariata (il pulsante "Connetti" gia' salva da solo al successo).

## 1.19.33

- Richiesta dell'utente ("raggruppali per tipologia"): il fieldset "Avvisi (email/WhatsApp)" nella scheda Sicurezza, che mescolava avviso disconnessione, avviso spostamento base, configurazione email e configurazione WhatsApp in un unico blocco, e' stato diviso in 4 gruppi separati per argomento (🔌 disconnessione, 📍 drift, ✉️ email, 💬 WhatsApp) + un piccolo gruppo per il pulsante di prova. Nessun campo aggiunto o rimosso, solo riorganizzato.

## 1.19.32

- **Richiesta dell'utente ("il pulsante salva va messo a fianco di ogni parametro... e non sotto la pagina")**: rimossi i pulsanti "Salva" in fondo alle 4 schede (Rete, GNSS & NTRIP, Sicurezza, Hardware) e il pulsante globale in fondo a Firmware - ogni singolo campo (56 in totale) ha ora il proprio pulsante 💾 accanto, che salva solo quel valore (il backend lascia invariato tutto il resto). Le password vuote ("lascia vuoto per non modificare") non vengono inviate, mostrano solo "—" invece di un falso "Salvato".

## 1.19.31

- **Richiesta dell'utente ("gli IP aggiornati devono essere sempre in bella vista")**, dopo un caso reale di rover scollegato perche' il PC del caster aveva cambiato rete/IP senza che nessuno se ne accorgesse: la scheda Stato mostra ora "Indirizzo IP attuale" del dispositivo (letto dal driver via `esp_netif_get_ip_info()`, non dalle impostazioni salvate) e "Caster NTRIP configurato" (host:porta/mountpoint), cosi' un disallineamento salta subito all'occhio.

## 1.19.30

- Richiesta dell'utente: pulsante "Salva" aggiunto in fondo a ciascuna delle 4 pagine con impostazioni modificabili (Rete, GNSS & NTRIP, Sicurezza, Hardware), non solo in fondo a tutto il modulo - non serve piu' scorrere fino in fondo/cambiare pagina per salvare. Nessuna duplicazione di codice: essendo tutte le pagine dentro lo stesso form, ogni pulsante usa lo stesso salvataggio gia' esistente.

## 1.19.29

- **Richiesta dell'utente ("il riavvio dobbiamo farlo solo se strettamente necessario")**: il pulsante "Salva e riavvia" riavviava SEMPRE il dispositivo dopo ogni salvataggio, anche per campi che si applicano gia' da soli (WiFi, NTRIP rover/host/mountpoint/credenziali) - inutile e, prima del fix della sessione di stasera, rischioso ad ogni singolo salvataggio. Ora "Salva" non riavvia piu' automaticamente; il riavvio resta disponibile come azione separata e deliberata ("Riavvia dispositivo", scheda Rete) per le poche impostazioni che lo richiedono davvero (pin GNSS/OLED/RGB nella scheda Hardware).

## 1.19.28

- Richiesta dell'utente: nella pagina Segnali, un "recipiente" visivo per il collegamento NTRIP rover, riempito da due "tubi" - RTCM ricevuti dal caster e GGA inviati al rover - pieno e verde solo se ENTRAMBE le direzioni sono vive negli ultimi 8s, arancione se solo una, vuoto se nessuna. Nuovo tracciamento lato firmware dell'ultimo GGA inviato (`status_note_gga_sent()`), esposto in `/api/signals`.

## 1.19.27

- Richiesta dell'utente ("l'app mi dovrebbe proporre lei i mountpoint, non devo scriverli a mano"): aggiunto un pulsante "Cerca mountpoint disponibili" nella scheda NTRIP rover, che legge il sourcetable pubblico del caster (`ntrip_rover_client_fetch_mountpoints()`, nuovo endpoint `GET /api/ntrip/mountpoints`) e mostra l'elenco cliccabile, con la descrizione di ciascuna - come gia' funziona la ricerca reti WiFi.

## 1.19.26

- **LA CAUSA VERA, FINALMENTE, di tutta la classe di bug "campi con valori a caso dopo il riavvio" vista in questa sessione (ap_ssid vuoto, gnss_uart_num=5005 uguale al default di nmea_udp_port, pin OLED con i valori del campo GNSS, l'SSID dell'AP di setup che spariva) - MAI stata vera corruzione ne' un blob di una vecchia versione**: `stored_cfg_t` (il blob salvato in NVS: `magic` + `app_settings_t`) contiene dei campi `double` (le coordinate della base fissa), che richiedono allineamento a 8 byte - il compilatore inserisce quindi 4 byte di padding invisibili tra `magic` (4 byte) e i dati veri. Il codice di caricamento calcolava pero' l'inizio dei dati come `sizeof(magic)` (4 byte), non `offsetof(stored_cfg_t, s)` (8 byte reali) - un errore di 4 byte che faceva leggere OGNI campo della configurazione shiftato di una posizione, ad OGNI singolo riavvio successivo a un salvataggio, fin dalla primissima volta che questo formato ha incluso un campo double. Il salvataggio stesso era sempre stato corretto (scrive la struct intera con un'assegnazione normale, che include gia' il padding giusto) - solo il caricamento sbagliava. Corretto usando `offsetof()` invece di `sizeof(magic)`.

## 1.19.25

- **Confermato su hardware reale: l'archiviazione automatica del firmware su SD funziona davvero, dall'avvio alla scrittura completa** - task avviato, SD montata, ~1.45MB copiati (~3s), file salvato e verificato. Aggiunto anche un log per il caso "SD non disponibile" che prima falliva in silenzio, utile se ricapita in futuro. Rimossa la diagnostica temporanea di debug (aveva gia' fatto il suo lavoro).

## 1.19.23

- **Trovata la vera causa finale dell'archiviazione SD che falliva, grazie all'errno aggiunto in diagnostica temporanea (1.19.22)**: non era la contesa tra moduli (gia' risolta in 1.19.21 col mutex condiviso, comunque un fix corretto da tenere), ma il filesystem FAT configurato per i soli nomi file classici "8.3" (`CONFIG_FATFS_LFN_NONE`) - il nome `v1.19.23.bin` (due punti, piu' di 8 caratteri prima dell'estensione) non e' un nome 8.3 valido, `fopen()` falliva con `errno=22` (Invalid argument). Attivato il supporto ai nomi file lunghi (`CONFIG_FATFS_LFN_HEAP` - sull'heap, non sullo stack, per non aggiungere pressione allo stack dei task SD dopo piu' di uno stack overflow reale gia' visto in questo progetto).

## 1.19.21

- **Fix vero (non un ritardo) della contesa SD**: aggiunto `sd_mutex.c/.h`, un mutex condiviso tra tutti e quattro i moduli che montano/smontano la microSD in modo indipendente (`sd_update.c`, `diag_log.c`, `fw_archive.c`, `ppp_log.c`) - prima ognuno montava per conto proprio senza sapere degli altri, e chi arrivava per secondo falliva silenziosamente invece di aspettare il proprio turno. Ricorsivo (non un mutex semplice): `sd_update` puo' tenerlo gia' preso quando chiama `ota_update_apply()`, che a sua volta chiama `fw_archive_save_current()` - un mutex normale si sarebbe bloccato da solo in questo caso. Tolto anche il ritardo di 8s introdotto in 1.19.20 (mitigazione temporanea, non piu' necessaria con la vera causa risolta).

## 1.19.20

- Il fix di 1.19.19 evitava il crash ma l'archiviazione su SD falliva comunque all'avvio ("Impossibile creare... archiviazione saltata"): il nuovo task si scontrava con `diag_log` (avviato subito dopo, monta/smonta la SD ogni 30s per tutta la vita del dispositivo) per l'uso della stessa scheda, senza alcun coordinamento tra i moduli SD del firmware (nessuno dei quattro - SD update, diag log, archivio, PPP log - usa un mutex condiviso). Aggiunto un ritardo di 8s prima del primo tentativo di archiviazione, per evitare la finestra di conflitto piu' probabile all'avvio. Non risolve la contesa in generale (richiederebbe un mutex condiviso tra tutti i moduli SD - refactor piu' ampio, rimandato).

## 1.19.19

- **Fix di uno stack overflow reale introdotto da me stesso in 1.19.18**: chiamare `fw_archive_save_current()` direttamente dentro `app_main()` faceva traboccare lo stack del task "main" (troppo piccolo per le operazioni SD/FAT coinvolte) - confermato su hardware reale, crash e riavvio automatico appena dopo il controllo SD all'avvio. Spostata in un task dedicato con stack da 8192 byte, stessa tecnica gia' usata altrove nel progetto per lo stesso tipo di operazione.

## 1.19.18

- Su richiesta dell'utente ("una versione funzionante deve essere sempre sulla SD"): `fw_archive_save_current()` (esisteva gia', ma partiva solo prima di applicare un aggiornamento tramite le funzioni OTA del firmware) ora gira anche ad ogni avvio - cosi' l'archivio su SD si popola anche per un dispositivo che ha ricevuto il firmware via flash USB diretto (come in questa sessione), non solo tramite aggiornamento online/SD/browser. Salta la scrittura se la versione attuale e' gia' archiviata, per non consumare inutilmente la SD ad ogni riavvio.

## 1.19.17

- **Causa vera del "dice non connesso ma in realta' e' collegato a un'altra rete" segnalato dall'utente**: quando un test falliva (password sbagliata, segnale debole, rete fuori portata), il messaggio "NON connesso" non diceva a quale rete il dispositivo fosse EFFETTIVAMENTE tornato tramite il riconnettore automatico (che riparte comunque dopo un test fallito) - lasciando intendere erroneamente che fosse rimasto scollegato del tutto. Ora il messaggio di fallimento mostra anche la rete reale a cui si e' ricollegato nel frattempo, con un controllo ritardato di qualche secondo per dare tempo alla riconnessione automatica di completarsi prima di mostrare "senza rete".

## 1.19.16

- Il fix di 1.19.14 (password corretta per le reti gia' note) era gia' giusto lato firmware, ma la pagina metteva comunque il cursore nel campo password dopo aver cliccato una rete dall'elenco scansione - dando l'impressione sbagliata che andasse digitata di nuovo, anche per una rete gia' nota (verde). Corretto: per le reti gia' note il focus va sul pulsante "Connetti", non sul campo password - si seleziona e si preme, senza scrivere nulla.

## 1.19.15

- Su richiesta dell'utente, tolta la scritta "(senza riavviare)" dal pulsante "Connetti" della scheda WiFi.

## 1.19.14

- **Bug reale segnalato dall'utente**: selezionando dalla scansione una rete gia' nota (evidenziata in verde) ma diversa dalla "principale" attuale, e lasciando il campo password vuoto per riusare quella gia' salvata, il dispositivo provava con la password della rete SBAGLIATA (sempre quella della principale, mai quella specifica della rete scelta). Corretto: ora cerca la password giusta anche tra le reti "conosciute" (non solo nella principale) quando il campo e' lasciato vuoto - selezionare una rete verde e premere "Connetti" senza scrivere nulla funziona come dovrebbe.

## 1.19.13

- Su richiesta dell'utente, accorciato il messaggio di esito del test WiFi: solo `CONNESSO a "NomeRete"`, tolta la spiegazione aggiuntiva ("salvato automaticamente, nessun riavvio necessario") ormai ridondante.

## 1.19.12

- Richiesta dell'utente: il messaggio di esito del test WiFi ora mostra il nome REALE della rete a cui ci si e' effettivamente collegati ("CONNESSO a "NomeRete"") invece del solo generico "CONNESSO!" - utile soprattutto per verificare a colpo d'occhio, senza dover controllare IP o log, se il dispositivo si e' davvero collegato alla rete appena scelta.

## 1.19.11

- **Causa vera (parte 2) del "torna sulla rete vecchia da solo" segnalato dall'utente, anche dopo il fix di 1.19.8**: `wifi_link_connect_known()` (usata dal riconnettore automatico) sceglieva sempre la rete nota col SEGNALE MIGLIORE fra tutte quelle conosciute, principale compresa - non necessariamente quella scelta per ultima dall'utente. Appena finiva un test manuale riuscito verso una rete nuova, il riconnettore automatico ripartiva (correttamente, dopo il fix precedente) ma poteva comunque scegliere la rete VECCHIA se questa aveva un segnale piu' forte, ignorando la scelta esplicita appena fatta. Ora la rete "principale" (l'ultima scelta esplicitamente) viene sempre preferita se visibile nella scansione, indipendentemente dal segnale delle altre - le altre reti "conosciute" restano un ripiego solo se la principale non si vede affatto.

## 1.19.10

- Su richiesta dell'utente, rimosso dalla pagina il campo "Reti ricordate": nonostante i fix di 1.19.5/1.19.6/1.19.8 la lista continuava a mostrare occasionalmente voci con caratteri corrotti, e la riconnessione automatica funziona comunque correttamente senza bisogno di mostrarla (confermato piu' volte su hardware: ComunicareWiFi <-> iPhone). La memoria delle reti resta attiva dietro le quinte, solo non piu' visibile/gestibile dalla UI.
- Colore verde delle reti gia' note nell'elenco di scansione reso piu' marcato (era troppo tenue).

## 1.19.9

- Corretto bug reale segnalato dall'utente: il pulsante "Mostra" (rivela password) compariva due volte nella scheda WiFi. Causa: un vecchio campo password "esca" nascosto, di un tentativo precedente contro l'autocompilamento del browser ormai sostituito dal timer di pulizia (vedi 1.19.1), era rimasto nella pagina - lo script che aggiunge "Mostra" ad ogni campo password lo trovava e ne creava uno anche per quello, invisibile ma presente. Rimosso il campo morto (e il `readonly` collegato, anch'esso superato dallo stesso timer).
- Richiesta dell'utente: le reti WiFi gia' collegate con successo in passato vengono ora evidenziate in verde nell'elenco della scansione (con un segno di spunta), cosi' si vede a colpo d'occhio quali sceglierebbe da solo il riconnettore automatico invece di doverle ricordare a memoria.

## 1.19.8

- **Trovata la causa vera (non solo cosmetica) del comportamento WiFi imprevedibile, grazie al log byte-per-byte aggiunto in 1.19.7**: `net_manager_task` (il riconnettore automatico in background) e il test di connessione manuale dalla UI ("Connetti") usavano entrambi la stessa funzione di basso livello per collegarsi (gia' protetta da mutex, quindi nessuna vera corruzione di memoria) - ma NESSUNO dei due sapeva dell'intenzione dell'altro: quando l'utente testava una rete diversa da quella attuale, la disconnessione che ne risultava veniva letta dal riconnettore automatico come "rete persa", che ripartiva per conto suo riconnettendo alla MIGLIOR rete gia' nota - spesso vincendo la corsa e riportando il dispositivo sulla rete vecchia pochi secondi dopo un test riuscito verso quella nuova. Confermato su hardware reale: un test di connessione a "iPhone" (dati puliti in arrivo dal browser, nessun errore) e' finito comunque con il dispositivo di nuovo su "ComunicareWiFi" per questo esatto motivo. Aggiunto un controllo (`web_ui_wifi_test_in_progress()`) che mette in pausa il riconnettore automatico finche' un test manuale non e' finito.
- Rimossa la diagnostica byte-per-byte temporanea di 1.19.7 (aveva gia' fatto il suo lavoro: i byte in arrivo dal browser erano puliti, quindi il problema non era li' ma nel comportamento sopra descritto).

## 1.19.6

- Il fix di 1.19.5 (rifiuta un secondo test WiFi mentre uno e' in corso) non bastava: l'utente continuava a vedere caratteri strani nell'elenco reti ricordate anche dopo. Trovata una seconda causa concreta della stessa famiglia di bug: il salvataggio delle impostazioni generali (pulsante "Salva" della pagina principale) e il test di connessione WiFi in background NON si escludevano a vicenda - potevano leggere/modificare/scrivere la configurazione in parallelo su due percorsi di codice indipendenti, ciascuno ignaro dell'altro, con l'ultimo che salva a sovrascrivere il lavoro dell'altro. Aggiunto un mutex condiviso attorno all'intera sequenza "leggi-modifica-scrivi" in tutti e tre i punti del firmware che salvano davvero le impostazioni (form generale, test WiFi, "dimentica reti").

## 1.19.5

- **Confermato su hardware reale (finalmente): il salvataggio/memoria delle password WiFi funziona** - connessione a due reti diverse in sequenza (rete WiFi normale + hotspot del telefono), entrambe salvate correttamente, riconnessione automatica confermata tra le due senza reinserire nulla.
- Bug reale trovato con l'utente: una voce con caratteri strani appariva nell'elenco "reti ricordate", trattata come una rete a se stante invece che riconosciuta come duplicato. Causa probabile: `wifi_test_connect_post_handler` non rifiutava un secondo tentativo di connessione partito mentre il primo era ancora in corso (il flag "running" esisteva gia' ma non veniva controllato) - due tentativi concorrenti possono leggere/modificare/scrivere l'elenco delle reti conosciute senza sapere l'uno dell'altro. Ora un secondo tentativo mentre uno e' gia' in corso viene rifiutato.
- Aggiunto un pulsante "Dimentica reti ricordate" nella scheda WiFi per svuotare subito l'elenco (nuovo endpoint `POST /api/wifi/forget-known`), utile per ripulire un elenco confuso senza dover cancellare tutta la configurazione.

## 1.19.4

- **Stessa causa di 1.19.1 (cache del browser), ma sugli endpoint dati**: l'header anti-cache era stato aggiunto solo alla pagina HTML, non alle risposte JSON di `/api/status`, scansione WiFi, ecc. - il browser poteva quindi continuare a mostrare dati vecchi (es. "reti ricordate" con voci non piu' corrispondenti a quelle salvate davvero) anche a firmware/NVS aggiornati. Aggiunto `Cache-Control: no-store, no-cache, must-revalidate` a tutti i 14 endpoint JSON.

## 1.19.3

- **Verifica approfondita richiesta dall'utente dopo il "?" nell'indirizzo di aggiornamento online** - trovate due cose:
  1. **Rete di sicurezza estesa**: oltre a gnss_uart_num/baud e ap_ssid/password (gia' protetti), ora anche ntrip_host, ntrip_mountpoint, ntrip_username, cellular_apn, ota_update_url, i campi degli avvisi email/WhatsApp e i campi del caster locale vengono validati al caricamento - se contengono byte non validi (non ASCII stampabile) vengono azzerati invece di continuare a mostrare punti interrogativi. Confermato sul dispositivo dell'utente: il campo `ota_update_url` risultava davvero corrotto.
  2. **Causa vera, probabilmente dietro tutta questa classe di problemi in questa sessione (ap_ssid vuoto, gnss_uart_num con valori spazzatura diversi ogni volta, ora ota_update_url)**: `s_settings` (la configurazione condivisa, oltre 1.7KB) non era mai stata protetta da un mutex - ogni lettura (`settings_get()`, chiamata da quasi ogni modulo del firmware, alcuni periodicamente) e scrittura (`settings_save()`) copiava l'intera struct senza alcuna sincronizzazione. Su un chip dual-core con piu' task che leggono/scrivono in continuo, una copia cosi' grossa non e' atomica: una scrittura poteva essere interrotta a meta' da una lettura concorrente (o viceversa), risultando in campi "a pezzi" - un campo diverso corrotto ogni volta, esattamente il comportamento osservato. Aggiunto un mutex che rende atomica ogni lettura/scrittura.

## 1.19.1

- **Probabile causa vera del "non cambia niente" segnalato dall'utente su piu' fix consecutivi**: la pagina web non aveva MAI un header `Cache-Control` nella risposta - senza niente da confrontare (nessun ETag/Last-Modified) e con lo stesso indirizzo ad ogni visita, il browser puo' tenersi la pagina in cache per giorni, mostrando sempre lo stesso HTML/JavaScript vecchio anche dopo aver installato un nuovo firmware con la pagina cambiata. Aggiunto `Cache-Control: no-store, no-cache, must-revalidate` - il browser scarichera' sempre la versione vera e aggiornata da qui in poi. **Serve comunque un ricaricamento forzato una volta sola** (Ctrl+Shift+R su PC, o svuotare la cache del browser) per scaricare questa stessa versione, dato che la pagina vecchia in cache non sa ancora del nuovo header.

- **Fix definitivo (si spera) dell'autocompilamento password WiFi sbagliata**: il trucco di 1.18.4 (campo esca + readonly) evidentemente non bastava per il browser dell'utente. Cambiato approccio: invece di provare a impedire l'autocompilamento del browser, per i primi 2 secondi dopo il caricamento della pagina (il momento in cui tipicamente scatta) si ripulisce qualunque valore compaia nel campo password SENZA che l'utente lo abbia davvero digitato (rilevato dall'evento di digitazione reale) - piu' robusto perche' non dipende dall'euristica specifica di un singolo browser.
- **Nome della rete WiFi mostrato quando connesso**: la scheda Stato ora mostra "WiFi — connesso a "NomeRete"" invece del solo generico "WiFi" - usa il nome della rete a cui ci si e' DAVVERO associati in questo momento (dal driver, non dalle impostazioni salvate), utile anche perche' con le reti "conosciute" (1.18.0) puo' non essere la rete "principale" configurata. Nuovo `wifi_link_get_current_ssid()`.

- Corretto un altro bug reale segnalato dall'utente (confermato con domande mirate: non un firmware/NVS, un browser): il gestore password del browser autocompilava il campo password del WiFi con una password salvata di un'ALTRA rete, indipendentemente dal nome rete scritto sopra - `autocomplete="new-password"` da solo non basta per tutti i browser (Chrome in particolare tende a ignorarlo su form che sembrano un login). Aggiunto un campo "esca" nascosto prima di quello vero (i browser tendono ad autocompilare il primo campo password che trovano) e reso il campo vero `readonly` finche' non lo si tocca (altro trucco rispettato dai browser per non autocompilarlo da solo) - nessuna modifica al firmware, solo alla pagina web.

- Corretto un bug reale segnalato dall'utente: selezionando una rete diversa dall'elenco della scansione WiFi, il campo password non veniva svuotato - restava quella digitata per la rete precedente, con il rischio concreto di provare a collegarsi alla rete nuova con la password sbagliata senza accorgersene. Ora si svuota sempre al cambio rete. Aggiunto anche `autocomplete="new-password"` al campo per ridurre le password suggerite/precompilate dal gestore password del browser.

- **Causa vera del crash di 1.18.1, questa volta risolta alla radice**: raddoppiare lo stack di `net_manager_task` non bastava - `wifi_ap_record_t` (usato dalla scansione WiFi) e' molto piu' grande di quel che sembra (include le info 802.11ax/HE), e un array di 32 sullo stack e' troppo pesante per qualunque dimensione ragionevole di stack. Il buffer della scansione (`wifi_link_scan_impl()` in wifi_link.c) ora vive sull'heap (malloc/free) invece che sullo stack - stesso principio gia' usato altrove in questo progetto per lo stesso problema, ma qui alla fonte invece che rincorrendo la dimensione giusta per ogni nuovo chiamante.

## 1.18.1

- **Fix di un crash introdotto in 1.18.0**: `net_manager_task` andava in stack overflow reale (confermato su hardware) non appena il nuovo `wifi_link_connect_known()` faceva la sua prima scansione prima di collegarsi - stesso tipo di causa gia' vista piu' volte in questo progetto (stack troppo piccolo per un array grande tenuto in locale, qui 32 `wifi_ap_record_t` della scansione). Stack di quel task portato da 4096 a 8192 byte.

## 1.18.0

- **Reti WiFi "conosciute"**: ogni volta che "Connetti" verifica con successo una rete, viene ricordata insieme a quelle gia' provate in passato (fino a 5, le piu' vecchie escono) - spostando il dispositivo tra reti diverse gia' usate almeno una volta (es. WiFi di casa e hotspot in campo), il riavvio si ricollega da solo a quella visibile, senza dover reinserire SSID/password ogni volta. Nuovo `app_settings_remember_wifi()` in settings.c e `wifi_link_connect_known()` in wifi_link.c (scansiona, sceglie tra le reti note quella col segnale migliore tra quelle visibili). Elenco (solo nomi, mai le password) visibile nella pagina Rete. Le reti vengono ricordate solo dopo una connessione VERIFICATA (mai da un semplice salvataggio del form senza test), per non riempire l'elenco con errori di battitura.

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
