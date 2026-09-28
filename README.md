# EVONETRTK — baseesp32-ethelite

Firmware ESP-IDF per **T-ETH-Elite (ESP32-S3)** che dialoga con un
ricevitore GNSS via UART e lo collega al caster
[EVONETRTK](../snip), in due modalita' scelte dalla UI web (vedi sotto):

- **base**: legge le correzioni RTCM3 emesse dal GNSS e le inoltra al
  caster collegandosi come sorgente NTRIP (mountpoint dedicato). Puo'
  anche fare da **caster NTRIP locale** per rover sulla stessa rete,
  indipendentemente da internet (vedi sotto).
- **rover**: riceve le correzioni RTCM3 dal caster (client NTRIP sulla
  mountpoint scelta) e le inoltra al GNSS via UART, rimandando al caster
  le sentenze NMEA `$GxGGA` emesse dal ricevitore. In questa modalita' il
  firmware rilancia anche l'intero stream NMEA del GNSS in **broadcast
  UDP su WiFi/Ethernet**, per software di guida come AgOpenGPS/AgIO.
  Questa scheda (ESP32-S3) **non ha Bluetooth Classic** (solo BLE, non
  implementata): l'uscita dati funziona su qualunque piattaforma, incluso
  iOS, proprio perche' e' networking UDP standard invece che SPP.

Rete: WiFi preferito, con fallback automatico su cellulare (modem
SIM7600, opzione SIM868 per riuso/test hardware — vedi "Architettura")
se il WiFi non e' disponibile, piu' Ethernet opzionale (W5500 via SPI).
Configurazione e stato gestibili da un'interfaccia web multi-pagina
protetta da codice di accesso, servita direttamente dal dispositivo,
senza bisogno di ricompilare per ogni installazione. Aggiornamento
firmware **OTA** (upload da browser, da microSD, o online da GitHub) con
rollback automatico se una nuova immagine non si conferma funzionante.

Progetto separato da EVONETRTK (repo/cartella diversa), pensato per essere
il firmware installato sulle basi fisiche che alimentano il servizio.

## Interfaccia web

Il dispositivo tiene sempre attivo un proprio **access point WiFi** (modalita'
WiFi APSTA), indipendentemente dallo stato di WiFi/cellulare verso l'esterno:
collegandosi con un telefono/laptop a quella rete e navigando su
`http://192.168.4.1` si raggiunge la UI di gestione, protetta da un codice
di accesso (HTTP Basic Auth, utente fisso `admin`). E' organizzata in
schede ([main/web/index.html](main/web/index.html), un solo file HTML con
navigazione lato client — niente ricarica pagina):

- **📊 Stato**: modalita' attiva, byte RTCM inviati/ricevuti, ultimo dato
  GNSS, stato caster NTRIP, stato IMU/INS (solo con chip Bynav M21D in
  rover), stato sistema (RAM, uptime, versione firmware), pulsante di
  riavvio.
- **🌐 Rete**: connettivita' da usare (WiFi / solo cellulare / entrambe),
  credenziali WiFi (con scan reti disponibili), APN cellulare, SSID/
  password dell'AP di setup.
- **🛰️ GNSS & NTRIP**: **scelta base/rover**, **chip GNSS collegato**
  (u-blox, Unicore, Quectel LC29H, Bynav M20/M20D, Bynav M21D con IMU/
  INS) — il firmware invia la sequenza di comandi giusta per configurarlo
  coerentemente con la modalita' scelta —, selezione messaggi RTCM3 per
  singolo numero, posizione base (survey-in automatico o coordinate
  fisse), credenziali della mountpoint EVONETRTK (con ricerca mountpoint
  disponibili e test connessione), caster NTRIP locale opzionale, porta
  broadcast UDP per l'uscita NMEA verso AgOpenGPS/AgIO (solo rover).
- **🔒 Sicurezza**: codice di accesso alla UI, matricola dispositivo,
  avvisi email/WhatsApp su disconnessione caster o spostamento antenna
  base (drift), con pulsante di test.
- **📶 Segnali**: fix GNSS (RTK), stato collegamento NTRIP, grafico a
  barre dei satelliti GNSS in vista (C/N0 per satellite, popolato solo in
  rover), byte RTCM per singolo messaggio (base), qualita' dei
  collegamenti WiFi/cellulare.
- **🎛️ Hardware**: pin verso il modulo GNSS, LED RGB opzionale (WS2812 o
  3 pin PWM), display OLED opzionale (SSD1306/SH1106/SSD1309 via I2C).
- **⬆️ Firmware**: aggiornamento via upload dal browser, da microSD, o
  online da un manifest esterno (release GitHub); archivio delle ultime
  versioni installate con ripristino manuale; backup/ripristino
  configurazione.
- **📜 Log**: log diagnostico recente del dispositivo.

SSID di default dell'AP: `EVONETRTK-XXXXXX` (XXXXXX = ultimi 3 byte del MAC
WiFi del dispositivo, cosi' basi diverse hanno SSID diversi), password AP
di default `baseesp32setup`, codice di accesso alla UI di default `1234`
— **entrambi da cambiare dalla UI alla prima configurazione**. Tutte le
impostazioni sono salvate in NVS (flash) e sopravvivono al riavvio; dopo
il salvataggio il dispositivo si riavvia per applicarle.

## Architettura

- [main/settings.c](main/settings.c) — configurazione persistita in NVS
  come blob grezzo (WiFi, NTRIP, chip GNSS, modalita', avvisi, LED/OLED,
  aggiornamenti, ecc.); i nuovi campi vanno **sempre aggiunti in fondo**
  alla struct, mai in mezzo, per non rompere la migrazione automatica dei
  dispositivi gia' in campo. Usa i valori di
  [main/Kconfig.projbuild](main/Kconfig.projbuild) come default iniziali
  finche' nessuna configurazione e' stata salvata dalla UI.
- [main/wifi_link.c](main/wifi_link.c) — WiFi in modalita' APSTA: AP di
  setup sempre attivo + connessione station con timeout configurabile e
  memoria delle reti gia' provate con successo.
- [main/cellular_link.c](main/cellular_link.c) — dial-up dati via modem
  cellulare, usando il componente ufficiale
  [`esp_modem`](https://github.com/espressif/esp-protocols/tree/master/components/esp_modem)
  di Espressif (PPP su UART, gestione comandi AT). **SIM7600** (famiglia
  SIMCom, qualunque variante regionale/Cat4 — per l'Italia serve banda
  B20, es. G-H o CE-H) e' l'hardware target del progetto; **SIM868** resta
  selezionabile solo per riutilizzare/testare il modulo del progetto
  gemello baseesp32/T-Internet-COM (risultato difettoso su quella
  scheda).
- [main/net_manager.c](main/net_manager.c) — supervisore: tenta il WiFi, se fallisce entro
  `BASEESP32_WIFI_CONNECT_TIMEOUT_MS` passa a cellulare; se il collegamento
  attivo cade, ricomincia la selezione (ritentando prima il WiFi). Non
  blocca l'avvio: la UI web e i task GNSS/NTRIP partono comunque.
- [main/eth_link.c](main/eth_link.c) — Ethernet opzionale (disattivata di
  default in Kconfig): chip **W5500 via SPI** (non LAN8720/RMII — hardware
  diverso dal progetto gemello T-Internet-COM), indipendente dalla
  selezione WiFi/cellulare, usata soprattutto per il broadcast UDP NMEA
  anche via cavo.
- [main/gnss_driver.c](main/gnss_driver.c) — smista verso il driver del
  chip GNSS scelto in `settings`, in base a modalita' base/rover:
  [main/gnss_ubx.c](main/gnss_ubx.c) (u-blox, protocollo binario UBX),
  [main/gnss_unicore.c](main/gnss_unicore.c) (Unicore, comandi ASCII),
  [main/gnss_lc29h.c](main/gnss_lc29h.c) (Quectel LC29H, comandi
  `$PQTM`/`$PAIR`), [main/gnss_bynav.c](main/gnss_bynav.c) (Bynav M20/
  M20D base/rover, e Bynav M21D con comandi INS aggiuntivi in rover —
  lever arm antenne, orientamento RBV, log prua/assetto).
- [main/ntrip_client.c](main/ntrip_client.c) — modalita' **base**: si
  collega al caster esterno come sorgente NTRIP (`SOURCE`) e spinge
  l'RTCM3 letto dalla UART GNSS.
- [main/ntrip_caster_server.c](main/ntrip_caster_server.c) — caster
  NTRIP **locale** opzionale (solo base): accetta rover direttamente
  sulla stessa rete, senza bisogno del caster esterno — utile in campo
  senza internet.
- [main/ntrip_rover_client.c](main/ntrip_rover_client.c) — modalita'
  **rover**: si collega al caster come client NTRIP (`GET` + Basic Auth) e
  scrive l'RTCM3 ricevuto sulla UART verso il GNSS; espone
  `ntrip_rover_client_forward_gga()` per rimandare al caster le righe
  `$GxGGA` lette da `gnss_nmea_reader`.
- [main/gnss_nmea_reader.c](main/gnss_nmea_reader.c) — unico task che
  legge lo stream NMEA/ASCII del GNSS in modalita' rover: ricompone le
  righe `$...` (NMEA) e `#...` (log Bynav INS) tra una lettura UART e
  l'altra, e le smista a broadcast UDP, inoltro `$GxGGA` al caster, stato
  satelliti da `$xxGSV`, e stato prua/assetto da `#INSPVAXA`/`#HEADINGA`.
- [main/nmea_udp_broadcast.c](main/nmea_udp_broadcast.c) — invia ogni riga
  NMEA in broadcast UDP (porta `settings.nmea_udp_port`) su tutte le
  interfacce di rete attive, per software come AgOpenGPS/AgIO.
- [main/gnss_signal.c](main/gnss_signal.c) — analizza le sentenze
  `$xxGSV` e mantiene l'elenco satelliti in vista (costellazione, PRN,
  C/N0) per la scheda "Segnali" della UI.
- [main/base_monitor.c](main/base_monitor.c) — registra la prima
  posizione ricevuta dopo il boot (dai frame RTCM 1005/1006) come
  baseline e calcola lo spostamento dell'antenna base nel tempo, per
  rilevare urti/spostamenti non voluti.
- [main/alerts.c](main/alerts.c) — avvisi email (SMTP/TLS) e WhatsApp
  (CallMeBot) quando il caster resta disconnesso oltre una soglia o la
  base si e' spostata, con notifica anche al rientro.
- [main/ota_update.c](main/ota_update.c),
  [main/sd_update.c](main/sd_update.c),
  [main/online_update.c](main/online_update.c),
  [main/auto_update.c](main/auto_update.c),
  [main/fw_archive.c](main/fw_archive.c) — sistema di aggiornamento
  firmware: scrittura condivisa su partizione OTA, applicazione da
  microSD (`firmware.json`+`firmware.bin`), applicazione online da
  manifest esterno (es. release GitHub, via `esp_https_ota`), controllo
  automatico periodico opzionale, archivio locale delle ultime versioni
  installate con ripristino manuale. Rollback automatico del bootloader
  (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) se una nuova immagine non
  arriva mai a confermarsi funzionante.
- [main/web_ui.c](main/web_ui.c) + [main/web/index.html](main/web/index.html) — server
  HTTP (`esp_http_server`) protetto da HTTP Basic Auth (`settings.admin_code`)
  con la UI multi-pagina e le API REST (`/api/status`, `/api/settings`,
  `/api/ota/*`, `/api/wifi/*`, `/api/ntrip/*`, `/api/alerts/test`, ecc.).
- [main/status.c](main/status.c) — contatori/stato condivisi (rete
  attiva, byte RTCM, stato caster) letti dalla UI.
- `main.c` sceglie a runtime quale coppia di task avviare (base o rover)
  in base a `settings.device_mode`; entrambi i client NTRIP lavorano su
  socket TCP e sulla struct `app_settings_t`, indifferenti al trasporto
  sottostante (WiFi, cellulare o Ethernet).
- [main/status_led.c](main/status_led.c) — LED di stato opzionali (GPIO in
  uscita): uno per la rete, uno per l'attivita' dati RTCM.
- [main/rgb_led.c](main/rgb_led.c) — LED RGB opzionale (WS2812 o 3 pin
  PWM separati) che riflette lo stesso stato con un colore diverso per
  WiFi/cellulare/dati.
- [main/oled_display.c](main/oled_display.c) — display OLED opzionale
  (128x64, I2C) che alterna stato/satelliti/segnale a rotazione.
- [main/reset_button.c](main/reset_button.c) — pulsante opzionale (GPIO in
  ingresso) che, tenuto premuto, cancella la configurazione salvata e
  riporta il dispositivo ai default di fabbrica.

## LED di stato e pulsante di reset

Tutti opzionali e disattivati di default (pin = -1): vanno abilitati
impostando il numero di GPIO reale in `menuconfig` — vedi sotto. Non c'e'
modo di configurarli dalla UI web: sono legati al cablaggio fisico della
scheda, quindi restano impostazioni di build come i pin UART.

- **LED rete** (`BASEESP32_LED_NET_PIN`, default 38 = LED integrato della
  T-ETH-Elite): acceso fisso quando WiFi o cellulare sono connessi,
  lampeggiante mentre il dispositivo cerca una rete.
- **LED dati** (`BASEESP32_LED_DATA_PIN`): lampeggia ad ogni pacchetto
  RTCM3 inviato (base) o ricevuto (rover).
- **LED RGB** (`BASEESP32_RGB_*`, opzionale, configurabile anche da UI):
  stesso stato dei LED semplici ma con un colore per canale di rete
  (blu=WiFi, verde=cellulare) e un lampo bianco sui dati.
- Se i LED montati sono collegati con il catodo verso il GPIO (logica
  invertita), abilitare `BASEESP32_LED_ACTIVE_LOW`.
- **Pulsante di reset** (`BASEESP32_RESET_BUTTON_PIN`): va collegato tra
  il GPIO scelto e massa (il firmware abilita il pull-up interno). Tenuto
  premuto per `BASEESP32_RESET_BUTTON_HOLD_MS` (default 5s) cancella
  tutta la configurazione in NVS — WiFi, NTRIP, chip GNSS, modalita',
  SSID/password dell'AP **e il codice di accesso alla UI** — e riavvia
  con i default di fabbrica (AP `EVONETRTK-XXXXXX` / password
  `baseesp32setup`, codice UI `1234`). E' il modo per recuperare
  l'accesso se si dimentica il codice.

## Ethernet e uscita dati per AgOpenGPS/AgIO

- **Ethernet** (`BASEESP32_ETHERNET_ENABLE`, disattivata di default in
  Kconfig): chip **W5500 via SPI** (non LAN8720/RMII — hardware diverso
  dal progetto gemello T-Internet-COM). Pin da Kconfig, presi dal
  repository ufficiale LilyGO-T-ETH-Series per il blocco
  `LILYGO_T_ETH_ELITE_ESP32S3`.
- **Uscita NMEA per software di guida** (solo rover): ogni riga NMEA
  emessa dal GNSS viene inoltrata in broadcast UDP sulla porta
  configurabile `nmea_udp_port` (default 5005), su AP di setup, WiFi
  station ed Ethernet — verificare che la porta corrisponda a quella
  configurata lato AgIO. Questa scheda (ESP32-S3) non ha Bluetooth
  Classic: l'UDP e' l'unico trasporto, ma funziona su qualunque
  piattaforma incluso iOS.

## Prerequisiti

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/) v5.x
  installato e attivato (`idf.py` nel PATH; su Windows: `. $env:USERPROFILE\esp\esp-idf\export.ps1`
  in ogni nuova shell). Il primo `idf.py build` richiede internet per
  scaricare le dipendenze dichiarate in
  [main/idf_component.yml](main/idf_component.yml) (`esp_modem`, `mdns`,
  `led_strip`).
- Un modulo GNSS RTK capace di funzionare in modalita' base fissa e di
  emettere messaggi RTCM3 su UART — u-blox, Unicore, Quectel LC29H o
  Bynav M20/M20D/M21D, selezionabile dalla UI web.
- SIM dati **attiva presso l'operatore** e **con PIN disabilitato** (il
  firmware non gestisce l'inserimento del PIN), se si usa il fallback
  cellulare.
- Credenziali della mountpoint sul caster EVONETRTK (host, porta,
  mountpoint, password sorgente) — impostabili dalla UI web dopo il primo
  flash, non serve conoscerle prima di compilare.

## Setup

```
idf.py set-target esp32s3
idf.py menuconfig
```

Nel menu "baseesp32-ethelite Configuration" ci sono solo le impostazioni
legate all'hardware/cablaggio della scheda (fisse per build, uguali su
tutte le basi con lo stesso hardware): numero UART e pin verso il modulo
GNSS, UART/pin/PWRKEY verso il modem cellulare, pin del lettore microSD,
timeout di rete, pin di LED/OLED/pulsante di reset, opzioni Ethernet.

SSID/password WiFi, APN, host/porta/mountpoint/password NTRIP e scelta del
chip GNSS sono invece pensati per essere impostati **dopo il flash, dalla
UI web** — i valori in `menuconfig` per questi campi servono solo come
default del primo avvio.

`sdkconfig` non e' versionato: ogni installazione va configurata
localmente.

## Build / flash

```
idf.py build
idf.py -p <PORTA_SERIALE> flash monitor
```

Al primo avvio, collegarsi all'AP `EVONETRTK-XXXXXX` (vedi log seriale per
il SSID esatto) e configurare tutto da `http://192.168.4.1`.

## Note / da verificare

- L'handshake NTRIP "source" (base, [main/ntrip_client.c](main/ntrip_client.c))
  e "client" (rover, [main/ntrip_rover_client.c](main/ntrip_rover_client.c))
  usano il protocollo NTRIP 1.0 classico (`SOURCE`/`GET` + risposta attesa
  `ICY 200 OK`, o `HTTP/1.1 200` per il GET). Confrontare con
  l'implementazione reale del caster EVONETRTK se emergono anomalie di
  connessione.
- **In modalita' base**, il firmware fa passthrough dei byte RTCM3 dalla
  UART al socket senza validare il framing: assume che il ricevitore GNSS
  emetta gia' RTCM3 valido. **In modalita' rover**, l'estrazione GGA
  assume che una riga `$GxGGA` arrivi intera in una singola lettura UART:
  se il ricevitore la spezza su piu' letture quella riga viene persa
  silenziosamente (la successiva arriva un secondo dopo, non e' grave, ma
  va tenuto presente).
- **I comandi verso i chip GNSS restano il punto a rischio piu' alto**:
  ricostruiti dalla documentazione pubblica di ciascun ecosistema
  (u-blox, Unicore, Quectel LC29H, Bynav), con verifica reale su hardware
  solo per i chip effettivamente collegati e testati sul campo. In
  particolare i comandi INS del **Bynav M21D**
  ([main/gnss_bynav.c](main/gnss_bynav.c)) e il relativo parsing dei log
  `#INSPVAXA`/`#HEADINGA` ([main/gnss_nmea_reader.c](main/gnss_nmea_reader.c))
  sono basati sul formato NovAtel OEM7 (stessi nomi di log riusati da
  Bynav) e **non ancora verificati contro un modulo M21D reale** — da
  controllare via log seriale prima di qualunque uso in campo, specie per
  applicazioni di guida/autosterzo.
- **UI web**: la protezione e' HTTP Basic Auth in chiaro (nessun HTTPS) —
  adeguata per un AP locale di setup, non per esporre la UI su reti non
  fidate. Il codice di default `1234` va cambiato alla prima
  configurazione.
- Nessun supporto ancora per: watchdog hardware, buffering RTCM su
  mancanza di rete prolungata (in base, i byte GNSS arrivati mentre
  nessun link e' su vengono persi se lo stream buffer si riempie), DNS
  captive-portal automatico sull'AP di setup, gestione del PIN SIM.

Cronologia dettagliata di funzionalita', fix e versioni in
[CHANGELOG.md](CHANGELOG.md).

## Errori di build gia' trovati e corretti (per chi tocca questo codice)

Problemi concreti emersi durante lo sviluppo, utili da conoscere se si
aggiunge altro codice:

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
  spento — ridefinire un vero macro 0/1 con `#ifdef .../#else` se serve
  usarlo come valore.
- Il codice dentro `#if CONFIG_X_ENABLE` (es. `eth_link.c`) **non viene
  compilato affatto** finche' quel Kconfig e' spento — un build che passa
  con l'opzione disabilitata non dice nulla sulla correttezza del codice
  al suo interno. Per verificarlo davvero serve abilitare l'opzione e
  ricompilare almeno una volta.
- Per mandare un comando AT generico e leggerne la risposta grezza da
  codice C (usato per la diagnostica cellulare) la funzione giusta e'
  `esp_modem_at(dce, cmd, buf, timeout_ms)` — dichiarata via macro in
  `esp_modem_command_declare.inc` (incluso da `esp_modem_api.h`), quindi
  non compare cercando il nome per testo dentro `esp_modem_api.h` ma e'
  comunque disponibile a compilazione.
- **Placeholder di pin sbagliati non danno errori di compilazione o di
  avvio**: il modem/periferica semplicemente non risponde mai, indistinguibile
  a prima vista da un vero problema di segnale/hardware finche' non si
  aggiunge diagnostica esplicita (`AT+CSQ`, `AT+CREG?` per il cellulare;
  log seriale grezzo per i chip GNSS) per capire se il dispositivo
  comunica affatto prima di dare per scontato che sia un problema di
  rete/segnale.
- **Una struct passata per valore in piu' punti della catena di chiamate
  di `app_main()`** (es. `app_settings_t`, ~1.8KB) puo' far ritraboccare
  lo stack del task "main" con un aumento anche piccolo delle sue
  dimensioni, esattamente nel punto di picco (init WiFi sincrona) — gia'
  successo due volte, risolto alzando `CONFIG_ESP_MAIN_TASK_STACK_SIZE`
  in [sdkconfig.defaults](sdkconfig.defaults). Dopo ogni modifica a
  `app_settings_t` conviene ricompilare **e** flashare **e** controllare
  il log di boot reale, non solo la compilazione pulita.
