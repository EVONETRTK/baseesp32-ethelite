# Analisi di cybersicurezza del firmware (RED art. 3.3, EN 18031)

Analisi del firmware EVONETRTK 1.33.0 rispetto alla norma EN 18031-1 (apparecchi radio collegati a internet, art. 3.3(d)) e, in parte, EN 18031-2 (dati personali, art. 3.3(e)). Fatta il 12/10/2026 leggendo il codice; i riferimenti sono file:riga del repository. I punti che dipendono dal testo esatto della norma o della sua pubblicazione in Gazzetta Ufficiale sono segnati *da verificare* con il laboratorio.

## Quadro

- **EN 18031-1 si applica**: il ricevitore va in internet (WiFi, Ethernet, LTE, VPN).
- **EN 18031-2 si applica** (*da verificare*): il ricevitore tratta dati personali — posizione del rover (BLE, UDP, GGA al caster), tracce GNSS su SD, nome e cognome di chi attiva la licenza (`license.c:788`), telefoni ed email degli avvisi, SMS letti dalla SIM (`web_ui.c:3915`).
- **EN 18031-3 probabilmente no**: nessun pagamento. Ma il pannello permette USSD e SMS liberi (`web_ui.c:3861-3877`), anche verso numeri a pagamento: meglio limitarli.
- **Restrizione nota** della pubblicazione della norma (decisione (UE) 2025/138, *da verificare*): se l'utente può non impostare o non usare una password, non c'è presunzione di conformità e serve l'**organismo notificato**. Oggi rientrano in questo caso le password di fabbrica comuni del pannello e della rete di setup e il caster locale senza credenziali.

## Stato per meccanismo

| Meccanismo | Stato | Punti principali |
|---|---|---|
| Accesso e autenticazione (ACM/AUM) | **non conforme** | Codice del pannello di fabbrica `1234` uguale per tutti (`settings.c:173`) e scritto nel manuale; cambio non obbligatorio; minimo 4 caratteri. Rete di setup WPA2 con password comune `baseesp32setup` (`settings.c:172`), sempre accesa. BLE senza accoppiamento. Caster locale anche senza credenziali. Buoni: tutte le API autenticate, protezioni CSRF e DNS rebinding, attesa crescente dopo 5 errori (per IP, tabella di 8 IP). |
| Aggiornamento sicuro (SUM) | **non conforme** | Nessuna firma del firmware: si verifica solo l'integrità SHA-256 (`ota_update.c:126`). Quattro canali senza autenticità: pannello, online da GitHub (HTTPS), **microSD all'avvio senza autenticazione** (`sd_update.c:188-216`), archivio SD. Manifest accettato anche in http (`online_update.c:288`). Buono: ritorno automatico se l'aggiornamento non parte. |
| Archiviazione sicura e chiavi (SSM/CCK) | **non conforme** | Né flash né NVS cifrate; password, chiave WireGuard e chiave della licenza in chiaro; download via USB aperto; **copia completa della configurazione su microSD in chiaro** (`config_backup.c:27-31`). |
| Comunicazioni sicure (SCM) | **parziale** | Pannello solo HTTP (porta 80) su tutte le interfacce, anche PPP (`web_ui.c:4042`). Client NTRIP in chiaro. Conformi: OTA, licenze, avvisi e monitoraggio in TLS con verifica del certificato; WireGuard. Da chiudere: server licenze e monitoraggio in http ammessi (`LICENSE_ENFORCE 0`, `web_ui.c:1862`). |
| Resilienza (RLM) | **conforme** | Watchdog, salvavita all'avvio, limiti di connessioni (caster 4, AP 4, socket 20). |
| Monitoraggio e controllo traffico (NMM/TCM) | **non applicabile** | Nessun inoltro tra reti (IP forwarding e NAPT spenti): da motivare nel fascicolo. |
| Registro eventi (LGM, -2) | **parziale** | Registrati i login falliti e i comandi; mancano login riusciti, cambi password, aggiornamenti. |
| Capacità generali (GEC) | **parziale** | Servizi esposti: HTTP 80, mDNS, DHCP sull'AP, caster 2101 (opzionale), BLE (opzionale), broadcast UDP NMEA sempre attivo. Manca SBOM; ESP-IDF è un commit non rilasciato del ramo 5.3. Fuzzing buono su molti lettori di dati. JTAG via USB attivo; endpoint di prova nel firmware di produzione. Il reset di fabbrica non cancella la microSD. |
| Crittografia (CRY) | **parziale** | Algoritmi adeguati (TLS 1.2, ECDSA P-256, SHA-256, WireGuard, AES hardware); deboli il token del cookie senza HMAC, Basic auth e NTRIP in chiaro, BLE non cifrato. |

## Piano degli interventi

### P0 — bloccanti (circa 2-3 settimane)

| # | Intervento | Impegno |
|---|---|---|
| 1 | **Password uniche per dispositivo** per pannello e rete di setup, generate in produzione e stampate sull'etichetta (il reset di fabbrica torna a quelle), oppure cambio obbligatorio prima di qualsiasi altra funzione; minimo 8 caratteri; togliere le password di fabbrica dal manuale | 2-3 giorni + produzione |
| 2 | **Firma del firmware** verificata a ogni aggiornamento (pannello, online, SD, archivio), manifest solo https, chiave di firma fuori dal repository | 2-3 giorni |
| 3 | **HTTPS sul pannello** con certificato per dispositivo; pannello non raggiungibile dall'interfaccia LTE (salvo VPN) | 2-4 giorni |
| 4 | **BLE con accoppiamento sicuro** (codice mostrato sul display o finestra di accoppiamento con pulsante) | 1-2 giorni |
| 5 | **Caster locale** con credenziali obbligatorie, o motivazione scritta (l'RTCM è pubblico, *da verificare*) | 0,5 giorni |
| 6 | `LICENSE_ENFORCE=1`; monitoraggio e manifest solo https | 1 ora |

### P1 — per un fascicolo difendibile (circa 2 settimane)

| # | Intervento | Impegno |
|---|---|---|
| 7 | In produzione: flash encryption, NVS cifrata, Secure Boot V2, JTAG spento, download mode sicuro (eFuse irreversibili: prima su schede di prova; i ricevitori in campo vanno riprogrammati via USB) | 3-5 giorni + procedura |
| 8 | Copie della configurazione su SD cifrate; reset di fabbrica che cancella anche i dati sulla SD | 1,5 giorni |
| 9 | NTRIP su TLS oppure VPN obbligatoria, con motivazione | 2-3 giorni |
| 10 | Sessioni: scadenza anche senza ora di rete, logout, password attuale per cambiarla, HMAC, limite globale ai tentativi | 1-1,5 giorni |
| 11 | SBOM, ESP-IDF a una versione rilasciata, procedura per le vulnerabilità e periodo di supporto | 1,5 giorni |

### P2 — miglioramenti e requisiti della EN 18031-2 (circa 1 settimana)

| # | Intervento | Impegno |
|---|---|---|
| 12 | Registro degli eventi di sicurezza (login riusciti, cambi password, aggiornamenti) | 1 giorno |
| 13 | Rete di setup WPA2/WPA3 con PMF e avviso per la password di fabbrica | 0,5-1 giorno |
| 14 | Interruttore per il broadcast UDP della posizione; informativa privacy | 1 giorno |
| 15 | Fuzzing di caster locale, impostazioni, import configurazione, risposte licenze e OTA | 1-2 giorni |
| 16 | Endpoint di prova esclusi dalla release; USSD e SMS limitati | 0,5 giorni |
| 17 | Capitolo "uso sicuro" nel manuale | 1 giorno |

## Nota

Con i punti P0 e P1 fatti e le scelte documentate, l'obiettivo è applicare la EN 18031 **senza restrizioni**, così da poter dichiarare la conformità da soli (modulo A) con i rapporti del laboratorio, senza organismo notificato.
