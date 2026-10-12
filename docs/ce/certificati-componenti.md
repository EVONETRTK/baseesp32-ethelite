# Certificati dei componenti per la marcatura CE

Documenti da raccogliere per il fascicolo tecnico del ricevitore EVONETRTK (direttiva RED 2014/53/UE, RoHS 2011/65/UE). Stato al 12/10/2026.

Legenda: ✅ trovato e scaricato · 🔎 esiste, da scaricare · ✉️ da chiedere al fornitore · ❓ da verificare · ➖ non applicabile

## Componenti radio (articoli 3.1a, 3.1b, 3.2)

| Componente | Funzione | Documento | Stato | Note |
|---|---|---|---|---|
| **Espressif ESP32-S3-WROOM-1** (sulla T-ETH-Elite) | WiFi 2,4 GHz e Bluetooth LE | Certificato di esame UE del tipo, Eurofins (organismo notificato 0980), 26/01/2022 | ✅ | Hardware V1.3. WiFi 2412-2472 MHz, 19,9 dBm EIRP; BLE 2402-2480 MHz, 10 dBm EIRP; antenna PCB 3,26 dBi. Norme: EN 300 328 V2.2.2, EN 301 489-1 V2.2.3 e -17 V3.2.4, EN 55032/55035, EN IEC 62368-1, EN 50665, EN IEC 62311. Link: https://documentation.espressif.com/ESP32-S3-WROOM-1%20CE%c2%a0Certification.pdf |
| Espressif (tutti i moduli e le schede) | — | Dichiarazione UE di conformità, 23/10/2025 | ✅ | RoHS (UE) 2015/863 e RED art. 3.1a, 3.1b, 3.2. https://documentation.espressif.com/EU%20Declaration%20of%20Conformity%20for%20Espressif%20Modules%20and%20Development%20Boards.pdf |
| **u-blox ZED-F9P** (sulla Syneda uRTK6) | Ricevitore GNSS (solo ricezione) | Dichiarazione UE di conformità u-blox per ZED-F9P | 🔎 | Dal sito u-blox, sezione documentazione del prodotto. Annotare la versione del modulo (ZED-F9P-01B / -02B / -04B…) |
| **Modem LTE** dello shield | Cellulare LTE | Certificato CE / DoC del modello montato | ✉️ / 🔎 | Decidere il modello definitivo (SIM7600G-H o E-H, oppure A7670E…). Nel repository LilyGO c'è il certificato CE del SIM7670G (2023). Il SIM868 non va venduto (2G, modulo di prova) |
| Quectel L76K (sullo shield) | GNSS dello shield | DoC Quectel | ➖ / ❓ | Solo se resta montato e acceso; con il DIP "GPS" spento conviene dichiararlo comunque come presente |
| Antenna GNSS | Ricezione GNSS multibanda | Scheda tecnica, RoHS | ✉️ | Antenna passiva/attiva: annotare guadagno e LNA |
| Antenna LTE | Trasmissione cellulare | Scheda tecnica, guadagno | ✉️ | Il guadagno entra nella valutazione di esposizione e nei limiti del modem |

## Schede e componenti non radio (articoli 3.1a, 3.1b, RoHS)

| Componente | Documento | Stato | Note |
|---|---|---|---|
| **LilyGO T-ETH-Elite** (ESP32-S3, Ethernet W5500, PoE) | Dichiarazione CE / RoHS della scheda, eventuali rapporti EMC | ✉️ | Nessun documento pubblico trovato (wiki, GitHub, sito). Chiedere a LilyGO |
| Modulo PoE della T-ETH-Elite (DP9900M-5V, IEEE 802.3af) | Scheda tecnica, sicurezza | 🔎 | Scheda tecnica nel repository LilyGO (datasheet/ETH-POE-DP9900M-5V.pdf) |
| **LilyGO T-ETH-Elite LTE Shield** (rev. 2412) | Dichiarazione CE / RoHS | ✉️ | Chiedere a LilyGO insieme alla scheda principale |
| **Syneda uRTK6.0** (ZED-F9P) | Dichiarazione CE / RoHS della scheda | ✉️ | Chiedere a Syneda |
| Display OLED SH1106 (se fornito) | RoHS | ✉️ | |
| microSD (se fornita) | RoHS | ✉️ | |
| Alimentatore / iniettore PoE (se fornito) | Dichiarazione CE (LVD, EMC), RoHS | ✉️ | Se lo forniamo noi rientra nella valutazione di sicurezza |
| Regolatore 12/24 V → 5 V per il trattore (se fornito) | CE / RoHS, protezioni ISO 7637 | ✉️ | |
| Contenitore, connettori, cavi, passacavi | RoHS, grado IP del contenitore | ✉️ | |

## Configurazioni da definire prima del laboratorio

La conformità vale per la configurazione provata. Prima delle prove vanno fissati:

1. **Le versioni vendute**: base (PoE, Ethernet, WiFi, opzionale LTE) e rover (12/24 V, WiFi, BLE, LTE). Va deciso se sono un unico prodotto con varianti o due prodotti.
2. **Il ricevitore GNSS** di ogni versione: ZED-F9P su Syneda, Unicore UM980/UM982, ComNav K922. Ognuno ha la sua dichiarazione.
3. **Il modem LTE** definitivo e le sue antenne.
4. **Le porte esterne**: Ethernet/PoE, eventuali RS-232 e ISOBUS (vanno provate in EMC), USB.
5. **Il firmware** per la cybersicurezza (art. 3.3, EN 18031-1): vedi `analisi-en18031.md`.

## Richieste ai fornitori

Bozze delle email in `richieste-fornitori.md`.
