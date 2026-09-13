# baseesp32

Firmware ESP-IDF per un ESP32 che dialoga con un ricevitore GNSS via UART
e lo collega al caster [EVONETRTK](../snip), in due modalita' scelte dalla
UI web (vedi sotto):

- **base**: legge le correzioni RTCM3 emesse dal GNSS e le inoltra al
  caster collegandosi come sorgente NTRIP (mountpoint dedicato).
- **rover**: riceve le correzioni RTCM3 dal caster (client NTRIP sulla
  mountpoint scelta) e le inoltra al GNSS via UART, rimandando al caster
  le sentenze NMEA $GxGGA emesse dal ricevitore. In questa modalita' il
  firmware rilancia anche l'intero stream NMEA del GNSS in **broadcast
  UDP su WiFi ed Ethernet, e via Bluetooth Classic (SPP)**, per software
  di guida come AgOpenGPS/AgIO — su tre trasporti in parallelo, a scelta
  di come ci si collega dal tablet/PC di bordo. **Il Bluetooth Classic
  non funziona su iPhone/iOS** (limitazione della piattaforma, non del
  firmware: iOS non mostra dispositivi SPP generici non certificati MFi)
  — su iOS l'unica via e' il broadcast UDP via WiFi.

Rete: WiFi preferito, con fallback automatico su GPRS tramite modem SIM868
se il WiFi non e' disponibile. Configurazione e stato gestibili da
un'interfaccia web protetta da codice di accesso, servita direttamente dal
dispositivo, senza bisogno di ricompilare per ogni installazione.

Progetto separato da EVONETRTK (repo/cartella diversa), pensato per essere
il firmware installato sulle basi fisiche che alimentano il servizio.

## Interfaccia web

Il dispositivo tiene sempre attivo un proprio **access point WiFi** (modalita'
WiFi APSTA), indipendentemente dallo stato di WiFi/GPRS verso l'esterno:
collegandosi con un telefono/laptop a quella rete e navigando su
`http://192.168.4.1` si raggiunge la UI di gestione, protetta da un codice
di accesso (HTTP Basic Auth, utente fisso `admin`). E' organizzata in
schede ([main/web/index.html](main/web/index.html), un solo file HTML con
navigazione lato client — niente ricarica pagina):

- **Stato**: modalita' attiva, rete corrente, byte RTCM inviati/ricevuti,
  ultimo dato GNSS, pulsante di riavvio.
- **Rete**: connettivita' da usare (WiFi / solo cellulare / entrambe),
  credenziali WiFi, APN cellulare, SSID/password dell'AP di setup.
- **GNSS & NTRIP**: **scelta base/rover**, **chip GNSS collegato
  (u-blox/Unicore)** — il firmware invia la sequenza di comandi giusta per
  configurarlo coerentemente con la modalita' scelta —, credenziali della
  mountpoint EVONETRTK, e la porta broadcast UDP per l'uscita NMEA verso
  AgOpenGPS/AgIO (solo rover).
- **Sicurezza**: codice di accesso alla UI.
- **Segnali**: grafico a barre dei satelliti GNSS in vista (C/N0 per
  satellite, da $xxGSV, popolato solo in rover), intensita' WiFi/cellulare,
  e per il cellulare anche **nome operatore** (es. Very/TIM/Vodafone/WIND,
  letto via `AT+COPS?`) e **tecnologia di accesso**. Il SIM868 attuale
  supporta solo 2G (GSM/EDGE) — il campo tecnologia e' comunque scritto in
  modo generico sui codici standard 3GPP, cosi' mostrera' automaticamente
  3G/4G/5G se in futuro si passa a un modem piu' recente sempre basato su
  esp_modem.

SSID di default dell'AP: `baseesp32-XXXXXX` (XXXXXX = ultimi 3 byte del MAC
WiFi del dispositivo, cosi' basi diverse hanno SSID diversi), password AP
di default `baseesp32setup`, codice di accesso alla UI di default `1234`
— **entrambi da cambiare dalla UI alla prima configurazione**. Tutte le
impostazioni sono salvate in NVS (flash) e sopravvivono al riavvio; dopo
il salvataggio il dispositivo si riavvia per applicarle.

## Architettura

- [main/settings.c](main/settings.c) — configurazione persistita in NVS
  (WiFi, APN, NTRIP, chip GNSS, modalita' base/rover, credenziali AP e
  codice di accesso UI); usa i valori di
  [main/Kconfig.projbuild](main/Kconfig.projbuild) come default iniziali
  finche' nessuna configurazione e' stata salvata dalla UI.
- [main/wifi_link.c](main/wifi_link.c) — WiFi in modalita' APSTA: AP di
  setup sempre attivo + connessione station con timeout configurabile.
- [main/cellular_link.c](main/cellular_link.c) — dial-up GPRS via modem SIM868, usando il
  componente ufficiale [`esp_modem`](https://github.com/espressif/esp-protocols/tree/master/components/esp_modem)
  di Espressif (PPP su UART, gestione AT commands), profilo `ESP_MODEM_DCE_SIM800`.
- [main/net_manager.c](main/net_manager.c) — supervisore: tenta il WiFi, se fallisce entro
  `BASEESP32_WIFI_CONNECT_TIMEOUT_MS` passa a GPRS; se il collegamento attivo
  cade, ricomincia la selezione (ritentando prima il WiFi). Non blocca
  l'avvio: la UI web e i task GNSS/NTRIP partono comunque.
- [main/gnss_driver.c](main/gnss_driver.c) — smista verso
  [main/gnss_ubx.c](main/gnss_ubx.c) (u-blox, protocollo binario UBX) o
  [main/gnss_unicore.c](main/gnss_unicore.c) (Unicore, comandi ASCII) in
  base al chip e alla modalita' (base/rover) scelti in `settings`.
- [main/ntrip_client.c](main/ntrip_client.c) — modalita' **base**: si
  collega al caster come sorgente NTRIP (`SOURCE`) e spinge l'RTCM3 letto
  dalla UART GNSS.
- [main/ntrip_rover_client.c](main/ntrip_rover_client.c) — modalita'
  **rover**: si collega al caster come client NTRIP (`GET` + Basic Auth) e
  scrive l'RTCM3 ricevuto sulla UART verso il GNSS. Non legge piu' la UART
  direttamente: espone `ntrip_rover_client_forward_gga()`, chiamata da
  `gnss_nmea_reader` con le righe `$GxGGA` da rimandare al caster sullo
  stesso socket (sincronizzato con un mutex, visto che due task toccano
  lo stesso file descriptor).
- [main/gnss_nmea_reader.c](main/gnss_nmea_reader.c) — unico task che
  legge lo stream NMEA del GNSS in modalita' rover, ricompone le righe
  (bufferizzando tra una lettura UART e l'altra — corregge un bug della
  versione precedente che assumeva una riga per lettura) e le smista a
  tre destinazioni: broadcast UDP (`nmea_udp_broadcast`), inoltro `$GxGGA`
  al caster (`ntrip_rover_client_forward_gga`), stato satelliti da
  `$xxGSV` (`gnss_signal`).
- [main/nmea_udp_broadcast.c](main/nmea_udp_broadcast.c) — invia ogni riga
  NMEA in broadcast UDP (porta `settings.nmea_udp_port`) su tutte le
  interfacce di rete attive (AP di setup, WiFi station, Ethernet), per
  software come AgOpenGPS/AgIO.
- [main/gnss_signal.c](main/gnss_signal.c) — analizza le sentenze
  `$xxGSV` e mantiene l'elenco satelliti in vista (costellazione, PRN,
  C/N0) per la scheda "Segnali" della UI.
- [main/eth_link.c](main/eth_link.c) — interfaccia Ethernet (PHY LAN8720
  via RMII, componente `esp_eth`), disabilitata di default. Indipendente
  dalla selezione WiFi/cellulare: se abilitata resta sempre attiva, usata
  soprattutto per raggiungere il broadcast UDP NMEA anche via cavo.
- [main/bt_spp.c](main/bt_spp.c) — Bluetooth Classic (Bluedroid) con
  profilo SPP: il dispositivo appare come una porta seriale a un
  tablet/PC accoppiato (nome Bluetooth uguale all'SSID dell'AP, es.
  `baseesp32-65FBFC`) e riceve lo stesso stream NMEA del broadcast UDP,
  scritto da `gnss_nmea_reader`. Attivo solo in modalita' rover.
- [main/web_ui.c](main/web_ui.c) + [main/web/index.html](main/web/index.html) — server
  HTTP (`esp_http_server`) protetto da HTTP Basic Auth (`settings.admin_code`)
  con la pagina di stato/configurazione e le API `GET /api/status`,
  `POST /api/settings`, `POST /api/reboot`.
- [main/status.c](main/status.c) — contatori/stato condivisi (rete attiva,
  byte RTCM inviati o ricevuti a seconda della modalita') letti dalla UI.
- `main.c` sceglie a runtime quale coppia di task avviare (base o rover)
  in base a `settings.device_mode`; entrambi i client NTRIP lavorano su
  socket TCP e sulla struct `app_settings_t`, indifferenti al trasporto
  sottostante (WiFi o PPP via GPRS).
- [main/status_led.c](main/status_led.c) — LED di stato opzionali (GPIO in
  uscita): uno per la rete (acceso fisso se connesso, lampeggiante se no),
  uno per l'attivita' dati RTCM (lampeggia ad ogni pacchetto).
- [main/reset_button.c](main/reset_button.c) — pulsante opzionale (GPIO in
  ingresso) che, tenuto premuto, cancella la configurazione salvata e
  riporta il dispositivo ai default di fabbrica.

## LED di stato e pulsante di reset

Entrambi sono opzionali e disattivati di default (pin = -1): vanno
abilitati impostando il numero di GPIO reale in `menuconfig` — vedi sotto.
Non c'e' modo di configurarli dalla UI web: sono legati al cablaggio
fisico della scheda, quindi restano impostazioni di build come i pin
UART.

- **LED rete** (`BASEESP32_LED_NET_PIN`): acceso fisso quando WiFi o GPRS
  sono connessi, lampeggiante mentre il dispositivo cerca una rete.
- **LED dati** (`BASEESP32_LED_DATA_PIN`): lampeggia ad ogni pacchetto
  RTCM3 inviato (base) o ricevuto (rover) — utile per capire a colpo
  d'occhio se le correzioni stanno effettivamente circolando.
- Se i LED montati sono collegati con il catodo verso il GPIO (logica
  invertita, comune su molte dev board con LED integrati), abilitare
  `BASEESP32_LED_ACTIVE_LOW`.
- **Pulsante di reset** (`BASEESP32_RESET_BUTTON_PIN`): va collegato tra
  il GPIO scelto e massa (il firmware abilita il pull-up interno, non
  serve una resistenza esterna). Tenendolo premuto per
  `BASEESP32_RESET_BUTTON_HOLD_MS` (default 5s) cancella tutta la
  configurazione in NVS — WiFi, NTRIP, chip GNSS, modalita', SSID/password
  dell'AP **e il codice di accesso alla UI** — e riavvia con i default di
  fabbrica (AP `baseesp32-XXXXXX` / password `baseesp32setup`, codice UI
  `1234`). E' il modo per recuperare l'accesso se si dimentica il codice.

## Ethernet e uscita dati per AgOpenGPS/AgIO

- **Ethernet** (`BASEESP32_ETHERNET_ENABLE`, disabilitata di default): PHY
  LAN8720 via RMII, confermato che e' questa la scheda giusta (non una
  T-ETH-Lite, che usa un W5500 via SPI — chip e driver completamente
  diversi). I pin dati/clock RMII sono fissati dall'hardware ESP32
  (GPIO 0, 19, 21, 22, 25, 26, 27). Per MDC/MDIO/reset/alimentazione/
  indirizzo PHY i default sono stati aggiornati con valori trovati su
  piu' fonti pubbliche indipendenti per la T-Internet-COM (pagina
  rivenditore + manuale): MDC=23, MDIO=18, RST=5, indirizzo PHY=0, e un
  pin di **alimentazione del PHY** (GPIO4) che il codice originale non
  gestiva affatto — senza portarlo alto il chip LAN8720 potrebbe restare
  spento anche con tutto il resto corretto. Non sono pero' presi dallo
  schema elettrico ufficiale letto direttamente: restano da confermare
  con un test reale (finora solo verificato che il codice compila, mai
  provato con un cavo Ethernet collegato).
- **Uscita NMEA per software di guida** (solo rover): ogni riga NMEA
  emessa dal GNSS (posizione, satelliti, ecc.) viene inoltrata in
  broadcast UDP sulla porta configurabile `nmea_udp_port` (default 5005),
  su AP di setup, WiFi station ed Ethernet. E' il meccanismo con cui
  AgOpenGPS/AgIO ricevono tipicamente la posizione GPS via rete locale —
  verificare che la porta corrisponda a quella configurata lato AgIO.
- **Bluetooth Classic SPP** (solo rover, sempre attivo, nessun interruttore
  Kconfig dedicato): stessa idea ma via Bluetooth invece che rete — il
  dispositivo appare come una porta seriale accoppiabile da un tablet,
  nome Bluetooth uguale all'SSID dell'AP. Visibile nella scheda **Stato**
  della UI (nome dispositivo, se un client e' accoppiato). Serve una
  partizione app piu' grande (vedi sotto) perche' lo stack Bluedroid e'
  pesante, e le ottimizzazioni IRAM del WiFi vanno disattivate per fare
  spazio (`CONFIG_ESP_WIFI_IRAM_OPT=n`, overhead trascurabile per il
  traffico di questo progetto). **Non funziona su iPhone/iOS**: e' una
  limitazione della piattaforma Apple, non del firmware — iOS mostra solo
  accessori Bluetooth Classic certificati MFi tramite il framework
  "External Accessory", un dispositivo SPP generico come questo resta
  invisibile nelle Impostazioni indipendentemente da quanto sia corretta
  l'implementazione. Su iOS l'unica via e' il broadcast UDP via WiFi
  (funziona su qualunque piattaforma, e' networking standard).

## Stato

Verificato su ESP-IDF v5.3 il 2026-09-12, incluse le funzionalita' di
questa sessione (modalita' base/rover, WiFi+cellulare+Ethernet+Bluetooth,
UI multi-pagina, broadcast NMEA, grafico segnali):

- `idf.py build` compila pulito in tutte le configurazioni provate,
  inclusa `BASEESP32_ETHERNET_ENABLE=y` (verificata una volta a parte,
  poi lasciata disabilitata di default) e con Bluetooth Classic sempre
  attivo. La partizione app e' stata allargata da 1MB a 4MB
  ([partitions.csv](partitions.csv), flash dichiarata 16MB invece di 2MB
  — la scheda ne ha davvero 16, rilevati a runtime) perche' Bluedroid da
  solo non ci stava nella partizione originale; il binario finale
  (WiFi+cellulare+Ethernet-code+Bluetooth+UI) occupa comunque solo il 38%
  della nuova partizione.
- **Flashato e avviato su hardware reale** (ESP32-D0WD-V3, CH9102): boot
  pulito senza crash in modalita' **base**, **rover**, e con **Bluetooth
  SPP attivo** (verificato separatamente attivando temporaneamente la
  modalita' rover, poi ripristinata a base come default). L'**AP WiFi di
  setup e' stato raggiunto con successo da telefono e PC**, incluso un
  salvataggio impostazioni riuscito dalla UI multi-pagina — quindi la
  UI, il selettore di rete e i campi APN sono confermati funzionanti dal
  vivo, non solo a codice. Nessun GNSS/SIM868/caster reale ne' client
  Bluetooth collegato durante i test, quindi restano non verificati: se
  il ricevitore accetta davvero la configurazione UBX/Unicore inviata; il
  parsing GSV e il broadcast UDP/Bluetooth su un dato NMEA vero; se un
  tablet riesce davvero ad accoppiarsi e leggere dati dalla porta SPP.
- **Un guasto hardware ha inizialmente impedito ogni test**: l'AP WiFi
  risultava invisibile a due dispositivi indipendenti (PC e telefono)
  anche a pochi centimetri di distanza, pur con i log del firmware che
  confermavano un avvio corretto — sintomo di un problema fisico
  all'antenna (connettore allentato durante le manipolazioni ripetute
  della scheda), non del codice. Risolto lato hardware dall'utente.
- **Prima scheda usata per i test era gia' cifrata (flash encryption
  Release mode) da un uso precedente**: un flash forzato l'ha resa
  inutilizzabile in modo permanente (bootloop, chiave AES bruciata e non
  recuperabile). Tutti i test successivi sono su una seconda scheda
  vergine. Prima di flashare qualunque scheda, controllare lo stato con
  `espefuse.py -p <PORTA> summary` (sola lettura) — se
  `DISABLE_DL_ENCRYPT`/`DISABLE_DL_DECRYPT` risultano `True`, non
  procedere.

## Prerequisiti

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/) v5.x
  installato e attivato (`idf.py` nel PATH; su Windows: `. $env:USERPROFILE\esp\esp-idf\export.ps1`
  in ogni nuova shell). Il primo `idf.py build` richiede internet per
  scaricare la dipendenza `espressif/esp_modem` dichiarata in
  [main/idf_component.yml](main/idf_component.yml).
- Un modulo GNSS RTK capace di funzionare in modalita' base fissa e di
  emettere messaggi RTCM3 su UART — u-blox (es. ZED-F9P) o Unicore (es.
  UM980/UM982), selezionabile dalla UI web.
- SIM dati **attiva presso l'operatore** (non basta che la SIM sia
  fisicamente valida — vedi "Stato" sotto per come diagnosticarlo) e **con
  PIN disabilitato** (il firmware non gestisce l'inserimento del PIN). Sul
  T-Internet-COM il modem SIM868 va nello slot di espansione integrato
  della scheda (non e' un collegamento libero a piacere).
- Credenziali della mountpoint sul caster EVONETRTK (host, porta,
  mountpoint, password sorgente) — impostabili dalla UI web dopo il primo
  flash, non serve conoscerle prima di compilare.

## Setup

```
idf.py set-target esp32
idf.py menuconfig
```

Nel menu "baseesp32 Configuration" ci sono solo le impostazioni legate
all'hardware/cablaggio della scheda (fisse per build, uguali su tutte le
basi con lo stesso hardware):

- numero UART e pin RX/TX verso il modulo GNSS, baud rate
- UART/pin/PWRKEY verso il modem SIM868 (default confermati per lo slot
  integrato della T-Internet-COM, vedi "Note / da verificare")
- timeout WiFi prima del fallback GPRS
- pin dei LED di stato e del pulsante di reset, se collegati (vedi sopra)

SSID/password WiFi, APN, host/porta/mountpoint/password NTRIP e scelta del
chip GNSS sono invece pensati per essere impostati **dopo il flash, dalla
UI web** (vedi sopra) — i valori in `menuconfig` per questi campi servono
solo come default del primo avvio.

`sdkconfig` non e' versionato: ogni installazione va configurata
localmente.

## Build / flash

```
idf.py build
idf.py -p <PORTA_SERIALE> flash monitor
```

Al primo avvio, collegarsi all'AP `baseesp32-XXXXXX` (vedi log seriale per
il SSID esatto) e configurare tutto da `http://192.168.4.1`.

## Note / da verificare

- L'handshake NTRIP "source" (base, [main/ntrip_client.c](main/ntrip_client.c))
  e "client" (rover, [main/ntrip_rover_client.c](main/ntrip_rover_client.c))
  usano il protocollo NTRIP 1.0 classico (`SOURCE`/`GET` + risposta attesa
  `ICY 200 OK`, o `HTTP/1.1 200` per il GET). Vanno confrontati con
  l'implementazione reale del caster EVONETRTK (NTRIP 1.0 vs 2.0, formato
  esatto delle risposte, se le credenziali rover richiedono uno username
  separato dalla password sorgente della base) prima del primo
  collegamento reale.
- **In modalita' base**, il firmware fa passthrough dei byte RTCM3 dalla
  UART al socket senza validare il framing: assume che il ricevitore GNSS
  emetta gia' RTCM3 valido. **In modalita' rover**, `extract_gga()` in
  [main/ntrip_rover_client.c](main/ntrip_rover_client.c) assume che una
  riga `$GxGGA` arrivi intera in una singola lettura UART: se il
  ricevitore la spezza su piu' letture (bit-rate bassa, buffer piccolo)
  quella riga viene persa silenziosamente — non e' un problema grave (la
  successiva arriva un secondo dopo) ma va tenuto presente.
- **Pin UART/PWRKEY del modem, aggiornati e confermati su hardware reale
  il 2026-09-12** (TX=33, RX=35, PWRKEY=32, presi dal repository ufficiale
  `Xinyuan-LilyGO/T-Internet-COM`, `example/Arduino/ATdebug/utilities.h`):
  i placeholder originali (TX=27, RX=26, PWRKEY=-1/disabilitato) erano
  completamente sbagliati e il modem non rispondeva affatto. Anche la
  sequenza di accensione era invertita — corretta in
  [main/cellular_link.c](main/cellular_link.c) su PWRKEY alto 300ms poi
  basso (era basso poi alto), seguendo l'esempio ufficiale. Con questi
  valori il modem risponde ai comandi AT e legge segnale reale (`AT+CSQ`
  RSSI 24-25, buono).
- **Il profilo `ESP_MODEM_DCE_SIM800`** (stesso set di comandi AT
  "dial-up" della famiglia SIM800) sembra compatibile: comunicazione AT di
  base funzionante (CSQ/CFUN/COPS rispondono regolarmente). **La
  connessione dati (PPP) non e' mai stata verificata end-to-end**, bloccata
  da un problema di lettura della SIM che sembra hardware, non firmware —
  diagnosi completa fatta il 2026-09-12/13 sulla SIM di test (Very, attiva
  presso l'operatore secondo l'utente):
  - `AT+CREG?` resta sempre `+CREG: 0,0` (non registrata, non in ricerca)
    nonostante segnale radio ottimo (RSSI 24-30/31).
  - `AT+CPIN?` e `esp_modem_get_imsi()` (funzione dedicata della libreria,
    non il comando AT generico) **falliscono entrambi in modo identico**:
    il modem non riesce a comunicare elettricamente con la SIM, mentre
    tutto cio' che non richiede la SIM (segnale, funzionalita' radio,
    modalita' operatore) funziona normalmente.
  - Forzare esplicitamente `AT+CFUN=1` + `AT+COPS=0` e riattendere non
    cambia nulla.
  - Provato a disabilitare completamente l'impulso sul pin PWRKEY/RESET
    (GPIO32, alcune fonti lo chiamano "PCIE-RST" invece di "PWRKEY" —
    nomi diversi per lo stesso pin fisico, comportamento potenzialmente
    diverso): nessun cambiamento nemmeno lasciando il pin del tutto
    intoccato.
  - Confermato che il modulo montato e' la daughter-card ufficiale LilyGO
    per questo slot (non un mismatch di scheda), che e' Nano-SIM originale
    senza adattatori, e che la SIM Very e' attiva presso l'operatore.
  - **Test conclusivo**: provata una seconda SIM di un secondo operatore
    (Wind, al posto di Very) nello stesso slot — **fallimento identico**
    (`CREG: 0,0`, RSSI 31/31 perfetto). Due SIM di due operatori diversi
    con lo stesso identico esito escludono in modo definitivo qualunque
    causa legata alla SIM.
  - **Conclusione definitiva**: guasto hardware confermato nello slot o
    nel modulo SIM868 di questa specifica T-Internet-COM — non
    risolvibile da firmware. Prossimo passo: contattare il venditore
    (OpenELAB) o LilyGO in garanzia con questa diagnosi, oppure provare lo
    stesso modulo su un'altra T-Internet-COM per isolare modulo vs scheda
    madre. Come alternativa e' stata valutata anche la combinazione
    **T-ETH-Elite (ESP32-S3) + shield LTE H744-02** (moduli
    SIM7600X/A7608X-H/A7670X, 4G vero — il SIM868 attuale non e'
    compatibile con quello shield) — ma e' un cambio di piattaforma
    sostanziale, non incrementale: l'ESP32-S3 non ha Bluetooth Classic
    (solo BLE, `bt_spp.c` da riscrivere/abbandonare) e l'Ethernet e'
    W5500/SPI invece di LAN8720/RMII (`eth_link.c` da riscrivere).
    Decisione non ancora presa.
- **Le sequenze di configurazione GNSS sono a rischio piu' alto**: le
  chiavi UBX-CFG-VALSET in [main/gnss_ubx.c](main/gnss_ubx.c) (base e
  rover) e i comandi ASCII in [main/gnss_unicore.c](main/gnss_unicore.c)
  sono stati ricostruiti a memoria dalla documentazione pubblica dei due
  ecosistemi, senza possibilita' di verifica (nessun hardware, nessun
  ESP-IDF installato). Durante l'aggiunta della modalita' rover e' stato
  trovato e corretto un errore concreto nella configurazione base
  originale (la chiave usata per abilitare l'uscita RTCM3 su UART1 era
  quella sbagliata, del gruppo INPROT invece che OUTPROT) — prova diretta
  che questi valori vanno trattati come bozze, non come dati affidabili.
  Il codice UBX non legge gli ACK di risposta, quindi una chiave sbagliata
  fallisce in silenzio (nessuna configurazione applicata, nessun errore
  visibile) finche' non si aggiunge la verifica degli ACK. Da controllare
  riga per riga contro l'Interface Description del modulo u-blox specifico
  e il manuale del modulo Unicore specifico prima dell'uso in campo.
- **UI web**: la protezione e' HTTP Basic Auth in chiaro (nessun HTTPS) —
  adeguata per un AP locale di setup, non per esporre la UI su reti non
  fidate. Il codice di default `1234` va cambiato alla prima
  configurazione ([main/web_ui.c](main/web_ui.c)).
- **Ethernet**: pin MDC/MDIO/reset/alimentazione presi da fonti pubbliche
  (non dallo schema elettrico ufficiale, vedi sopra) — mai alimentata su
  hardware reale con un cavo Ethernet collegato.
- **Parsing GSV** ([main/gnss_signal.c](main/gnss_signal.c)): la logica di
  parsing (campi separati da virgola, fino a 4 satelliti per sentenza,
  pulizia delle voci vecchie al primo messaggio del ciclo) segue lo
  standard NMEA-0183 ma non e' mai stata eseguita contro un flusso GSV
  reale — solo compilata e fatta girare a vuoto (nessun dato in arrivo).
- **`cellular_link_get_signal()`**: usa `esp_modem_get_signal_quality()`
  (comando AT+CSQ) e converte la scala 0-31 in dBm con la formula
  standard (-113 + 2*csq) — mai verificato con un modem reale collegato.
- **Broadcast UDP NMEA per AgOpenGPS/AgIO**: la porta di default (5005) e'
  un valore di comodo, non un default noto di AgIO — va allineata
  manualmente dalla UI web a quella configurata lato AgOpenGPS.
- **Bluetooth SPP**: confermato non funzionante su iPhone/iOS (limitazione
  di piattaforma, vedi sopra). Nessun accoppiamento testato con
  Windows/Android — l'avvio dello stack (`bt_spp_init`) non ha dato
  errori, ma non e' verificato che un tablet/PC reale riesca ad
  accoppiarsi e leggere dati utili dalla porta seriale emulata.
- Nessun supporto ancora per: aggiornamento OTA, watchdog hardware,
  buffering RTCM su mancanza di rete prolungata (in base, i byte GNSS
  arrivati mentre nessun link e' su vengono persi se lo stream buffer si
  riempie), DNS captive-portal automatico sull'AP di setup (bisogna
  navigare manualmente su 192.168.4.1), gestione del PIN SIM.

## Errori di build gia' trovati e corretti (per chi tocca questo codice)

Tre problemi concreti emersi al primo build reale, utili da conoscere se
si aggiunge altro codice:

- `esp_timer.h` non si trova se il componente `esp_timer` non e' elencato
  in `REQUIRES` in [main/CMakeLists.txt](main/CMakeLists.txt) — l'include
  path non viene aggiunto automaticamente solo perche' un altro componente
  lo usa internamente.
- `ESP_NETIF_DEFAULT_PPP()` (in [main/cellular_link.c](main/cellular_link.c))
  esiste solo se `CONFIG_LWIP_PPP_SUPPORT=y`, disattivato di default in
  ESP-IDF — va abilitato in [sdkconfig.defaults](sdkconfig.defaults)
  (insieme a `sdkconfig`, che va cancellato e rigenerato se questo file
  cambia dopo che `sdkconfig` esiste gia').
- Un `config ... bool` di Kconfig disattivato **non genera nessuna
  `#define`** (non "definito a 0"): usarlo direttamente in un'espressione
  C invece che in un `#if` da' "undeclared identifier" solo quando e'
  spento. Successo con `BASEESP32_LED_ACTIVE_LOW` in
  [main/status_led.c](main/status_led.c) — fix: ridefinire un vero macro
  0/1 con `#ifdef .../#else`.
- Il codice dentro `#if CONFIG_X_ENABLE` (es. tutto `eth_link.c`) **non
  viene compilato affatto** finche' quel Kconfig e' spento — un build che
  passa con l'opzione disabilitata non dice nulla sulla correttezza del
  codice al suo interno. Per verificarlo davvero serve abilitare
  l'opzione e ricompilare almeno una volta (fatto per `eth_link.c`: build
  pulito con `BASEESP32_ETHERNET_ENABLE=y`, poi riportato a `n` di
  default).
- Il nome corretto per impostare il nome del dispositivo Bluetooth Classic
  e' `esp_bt_gap_set_device_name`, non `esp_bt_dev_set_device_name` (che
  non esiste - errore di compilazione immediato, facile da correggere).
- Aggiungere Bluetooth Classic a un progetto con WiFi gia' attivo puo'
  **superare l'IRAM disponibile** (~328KB, fissa, non legata alla
  dimensione della flash/partizione): l'errore e' un fallimento del
  linker ("section .iram0.text will not fit in region iram0_0_seg"), non
  un errore di compilazione. Fix: disattivare le ottimizzazioni IRAM del
  WiFi (`CONFIG_ESP_WIFI_IRAM_OPT=n`, `CONFIG_ESP_WIFI_RX_IRAM_OPT=n`),
  che spostano codice dalla flash all'IRAM per velocita' e liberano
  spazio se disattivate.
- Il controller Bluetooth su ESP32 va costruito esplicitamente in
  modalita' BR/EDR (`CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y`): il default e'
  BLE-only, quindi `esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)`
  fallisce a runtime (non in compilazione) se questa opzione manca —
  fallimento silenzioso senza log dettagliato finche' non si aggiunge
  `esp_err_to_name()` al messaggio d'errore.
- Per mandare un comando AT generico e leggerne la risposta grezza da
  codice C (usato per la diagnostica `AT+CREG?`/`AT+COPS?` in
  `cellular_link.c`) la funzione giusta e' `esp_modem_at(dce, cmd, buf,
  timeout_ms)` — dichiarata via macro in
  `esp_modem_command_declare.inc` (incluso da `esp_modem_api.h`, gia'
  usato dal progetto), quindi non compare cercando il nome per testo
  dentro `esp_modem_api.h` ma e' comunque disponibile a compilazione.
- **Placeholder di pin sbagliati non danno errori di compilazione o di
  avvio**: il modem semplicemente non risponde mai (log "modem non
  risponde"/"impossibile entrare in modalita' dati"), indistinguibile a
  prima vista da un vero problema di segnale/SIM finche' non si aggiunge
  diagnostica esplicita (`AT+CSQ`, `AT+CREG?`) per capire se il modem
  comunica affatto prima di dare per scontato che sia un problema di rete.
