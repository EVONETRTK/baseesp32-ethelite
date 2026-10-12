# Fascicolo tecnico — Ricevitore GNSS RTK EVONETRTK

Bozza della documentazione tecnica richiesta dalla direttiva RED 2014/53/UE, allegato V. Va conservata per 10 anni dall'immissione sul mercato e mostrata alle autorità su richiesta. Le parti tra [[ ]] sono da completare.

Stato: bozza del 12/10/2026, firmware di riferimento 1.33.0.

## 1. Fabbricante

- Ragione sociale: [[da completare]]
- Indirizzo: [[da completare]]
- Partita IVA, contatti: [[da completare]]
- Iscrizione al Registro AEE (RAEE): [[da completare]]

## 2. Descrizione generale del prodotto

**Nome commerciale:** Ricevitore GNSS RTK EVONETRTK. **Modello/i:** [[da definire: es. EVONETRTK-B (base), EVONETRTK-R (rover)]].

**Uso previsto:** stazione base GNSS RTK fissa, che invia correzioni a un caster NTRIP via internet, oppure ricevitore rover su macchine agricole, che riceve correzioni dal caster e fornisce la posizione a software di guida (AgOpenGPS) e app su telefono. Uso professionale e privato (agricoltura).

**Funzioni radio:**

| Radio | Banda | Potenza massima | Modulo |
|---|---|---|---|
| WiFi IEEE 802.11 b/g/n | 2412–2472 MHz | 19,9 dBm EIRP | ESP32-S3-WROOM-1 |
| Bluetooth LE | 2402–2480 MHz | 10 dBm EIRP | ESP32-S3-WROOM-1 |
| LTE (opzionale) | [[bande del modem scelto]] | [[potenza]] | [[modem definitivo]] |
| GNSS (solo ricezione) | L1/L2 GPS, GLONASS, Galileo, BeiDou | — | u-blox ZED-F9P [[o altri moduli venduti]] |

**Interfacce:** Ethernet 10/100 con PoE IEEE 802.3af (base), USB-C (configurazione e alimentazione), microSD, [[RS-232 e ISOBUS se aggiunte]], alimentazione [[PoE 36–57 V / 12–24 V con regolatore]].

**Fotografie esterne e interne:** [[da aggiungere: prodotto chiuso, aperto, etichetta]].

## 3. Versioni di hardware e firmware

| Elemento | Versione |
|---|---|
| Scheda principale | LilyGO T-ETH-Elite [[revisione]] |
| Shield LTE | LilyGO T-ETH-Elite LTE Shield rev. 2412 |
| Scheda GNSS | Syneda uRTK6.0, u-blox ZED-F9P firmware HPG 1.51 |
| Firmware | 1.33.0 (ESP-IDF 5.3), aggiornabile via internet; oggi con verifica di integrità ma senza firma (da introdurre, vedi analisi-en18031.md) |
| Tabella delle partizioni | 16 MB, firmware A/B da 4 MB (docs/partizioni.md) |

Il firmware influisce sulla conformità radio (potenze e bande dei moduli non sono modificabili dall'utente) e sulla cybersicurezza (art. 3.3).

## 4. Schemi, disegni e distinta

- Schema a blocchi del ricevitore: [[da disegnare]]
- Schemi delle schede di terzi: T-ETH-Elite e LTE Shield (repository LilyGO), Syneda uRTK6 (da chiedere)
- Disposizione delle antenne e cablaggi interni: [[da completare]]
- Distinta dei componenti con fornitori: vedi `certificati-componenti.md`

## 5. Norme armonizzate applicate

| Requisito RED | Norme | Come si dimostra |
|---|---|---|
| Art. 3.1(a) sicurezza | EN IEC 62368-1 | Rapporto di prova del prodotto finito [[laboratorio]] |
| Art. 3.1(a) salute | EN 50665, EN IEC 62311 (valutazione con più radio attive) | Valutazione/rapporto [[laboratorio]] |
| Art. 3.1(b) EMC | EN 301 489-1, -17 (WiFi/BLE), -19 (GNSS), -52 (LTE); EN 55032, EN 55035; per l'uso su macchine agricole anche ISO 14982 | Rapporto di prova del prodotto finito |
| Art. 3.2 radio | EN 300 328 (WiFi/BLE), EN 303 413 (GNSS), EN 301 908-1/-13 (LTE) | Rapporti dei moduli certificati, più verifica in configurazione finale (antenne) |
| Art. 3.3(d)(e) cybersicurezza | EN 18031-1 (ed eventualmente -2) | Valutazione del firmware: vedi `analisi-en18031.md` |
| RoHS | EN IEC 63000 | Dichiarazioni dei fornitori |

## 6. Analisi dei rischi

[[Da completare]] con almeno: scossa elettrica e surriscaldamento (alimentazione PoE e 12/24 V), interferenze radio, esposizione ai campi, uso in autosterzo (posizione errata o persa: avvisi e allarme RTK perso già presenti), accesso non autorizzato e manomissione del firmware, perdita di alimentazione. Collegare ogni rischio alle misure adottate e alle avvertenze del manuale (capitolo "Avvertenze di sicurezza").

## 7. Informazioni per l'utente

- Manuale in italiano (docs/manuale, allegato a ogni release), con avvertenze di sicurezza, istruzioni di installazione e uso.
- Da aggiungere prima della vendita: bande di frequenza e potenza massima (art. 10(8) RED), dichiarazione di conformità semplificata con indirizzo internet della dichiarazione completa, dati del fabbricante, smaltimento RAEE.
- Etichetta del prodotto: marchio CE, fabbricante, modello, matricola, alimentazione.

## 8. Rapporti di prova e certificati

| Documento | Stato |
|---|---|
| Certificato UE del tipo ESP32-S3-WROOM-1 (Eurofins 0980) | ✅ raccolto |
| Dichiarazione UE Espressif | ✅ raccolta |
| Dichiarazione UE u-blox ZED-F9P | da scaricare |
| Dichiarazione UE del modem LTE | da raccogliere |
| Rapporti EMC e sicurezza del prodotto finito | da fare in laboratorio |
| Valutazione EN 18031-1 | in corso |

## 9. Dichiarazione UE di conformità

[[Da redigere dopo le prove]]: fabbricante, prodotto e modelli, firmware, direttive (RED 2014/53/UE, RoHS 2011/65/UE), norme applicate, eventuale organismo notificato, luogo, data e firma.
