# Attivazione del ricevitore e licenze

Specifica condivisa tra firmware (questo repository) e server EVONETRTK (gestionale del caster, repository `snip`). Stato: **bozza 3, 09/10/2026** (allineata al firmware 1.30.3 e al server `snip-licenze`). Le decisioni commerciali sono dell'azienda; i dettagli tecnici possono cambiare finché non c'è la prima versione in campo.

## 1. Decisioni

| Punto | Decisione |
|---|---|
| Modello | Acquisto + abbonamento: le funzioni base sono comprese nel prezzo del ricevitore e restano per sempre; le funzioni extra hanno un canone annuale. |
| Chi attiva | Il **cliente**, con un codice di attivazione (in fattura o nel portale), accettando lui le condizioni nel pannello. |
| Pacchetti | Ogni ricevitore ha **un solo pacchetto**, che decide anche il modo di lavoro: **Base** oppure **Rover**. Le funzioni extra si comprano a parte e valgono solo con il Pacchetto Rover. |
| Scadenza | Avvisi a 30 e a 7 giorni. Dopo la scadenza le funzioni extra si spengono **solo al riavvio successivo**, mai durante il lavoro. Il pacchetto (per sempre) non si spegne mai. |
| Senza internet | La licenza è firmata e verificata dal ricevitore: funziona anche offline. Rinnovo automatico quando c'è rete; tolleranza di 30 giorni se il server non risponde: passato `renew_after`, se l'ultimo contatto riuscito con il server (o, se non c'è mai stato, l'emissione della licenza) è più vecchio di 30 giorni, le funzioni **extra** non partono al riavvio successivo finché il ricevitore non si collega di nuovo al server; il pacchetto resta. Il pannello lo spiega («Licenza non rinnovata da N giorni: funzioni extra sospese…»; da 7 giorni senza contatto un preavviso). |

## 2. Funzioni

Codici usati nella licenza e nel firmware (`license_has()`).

| Codice | Livello | Cosa sblocca |
|---|---|---|
| `base` | pacchetto | Lavoro come base: invio delle correzioni al caster, misura della posizione (anche con Galileo HAS), servizi avanzati (caster locale, VPN, monitoraggio remoto, dati grezzi PPP, avvisi), controllo dello spostamento dell'antenna. |
| `rover` | pacchetto | Lavoro come rover: correzioni dal caster, uscita NMEA **1 Hz**. |
| `nmea_fast` | extra (solo rover) | Uscita per l'autosterzo a 5 e 10 Hz (`nmea_rate_hz`). |
| `has` | extra (solo rover) | Riserva Galileo HAS del rover quando mancano le correzioni. |
| `ins` | extra (solo rover) | IMU, doppia antenna, prua e compensazione dell'inclinazione (ComNav K922). |

Il server rifiuta licenze con zero o due pacchetti, e il firmware ignora gli extra senza il Pacchetto Rover. Nel pannello le funzioni non comprese sono in grigio con il lucchetto; quelle comprate ma non ancora in uso portano il segno «attiva dal prossimo riavvio».

Restano sempre disponibili, anche senza licenza valida, tutto ciò che serve alla sicurezza e all'assistenza: configurazione di rete, aggiornamento del firmware, log, ripristino, avviso 1005.

**Prova (firmware attuale)**: `LICENSE_ENFORCE 0` in `license.c`: il firmware calcola e mostra la licenza ma non blocca nulla. Si mette a 1 quando il server vero e la chiave vera sono pronti.

## 3. Stati del ricevitore

| Stato | Quando | Cosa funziona |
|---|---|---|
| `da_attivare` | Nuovo o dopo il ripristino di fabbrica, nessuna licenza valida | Solo pannello: rete, pagina di attivazione, aggiornamento firmware. Nessuna correzione. |
| `attivo` | Licenza valida | Le funzioni elencate nella licenza. |
| `in_scadenza` | Extra che scadono entro 30 giorni (contano solo con il Pacchetto Rover) | Tutto; avviso nel pannello e promemoria email/WhatsApp a 30 e a 7 giorni, se configurati. |
| `extra_scaduti` | Extra scaduti (contano solo con il Pacchetto Rover) | Fino al riavvio: tutto come prima. Dal riavvio: solo il pacchetto. |
| (`attivo` con nota) | Nessun contatto con il server da oltre 30 giorni, `renew_after` passato | Dal riavvio: solo il pacchetto, extra sospesi; tornano al primo riavvio dopo un rinnovo riuscito. Lo stato resta quello della licenza, il motivo è nel campo `note` (e nel messaggio della licenza). |
| `revocato` | Il server ha revocato il ricevitore (furto, mancato pagamento dell'acquisto) | Come `da_attivare`, con il motivo. Applicato **solo al riavvio**. |

Le funzioni effettive si calcolano **una volta all'avvio** e non cambiano fino al riavvio successivo: nessuna funzione si spegne mentre il trattore lavora.

## 4. Identità e chiavi

- **Matricola** (`device_serial`) e **ID del chip** (MAC di base dagli eFuse, `esp_efuse_mac_get_default`).
- **Chiave del ricevitore**: coppia ECDSA P-256 generata sul ricevitore alla prima attivazione; la privata resta nella NVS (non cifrata: vedi §8), la pubblica va al server. Serve a firmare le richieste di rinnovo: una licenza copiata su un altro ricevitore non si rinnova.
- **Chiave del server**: coppia ECDSA P-256. La privata solo sul server (meglio in un KMS o in un file con permessi ristretti, mai nel repository). La **pubblica è compilata nel firmware** (`main/license_pubkey.h`). Il firmware ne accetta fino a due, per poterla cambiare senza fermare i ricevitori.

Algoritmo: ECDSA P-256 con SHA-256 (mbedTLS nel firmware, `crypto/ecdsa` in Go). Ed25519 non è disponibile nell'mbedTLS di ESP-IDF 5.3.

## 5. Licenza

Documento JSON firmato. Si firmano i **byte esatti** del JSON (niente ricodifica), trasportati in base64:

```json
{
  "license": "<base64 dei byte del JSON qui sotto>",
  "sig": "<base64 della firma ECDSA DER>",
  "kid": "srv-2026-1"
}
```

Contenuto:

```json
{
  "v": 1,
  "serial": "123456",
  "chip": "a0b1c2d3e4f5",
  "device_pub": "<base64 della chiave pubblica del ricevitore, formato X9.62 non compresso>",
  "customer": "C-000123",
  "issued": 1791504000,
  "renew_after": 1792108800,
  "features": {
    "rover": 0,
    "nmea_fast": 1823040000,
    "has": 1823040000
  },
  "revoked": false
}
```

- `features`: codice → scadenza (Unix, UTC); `0` = per sempre.
- `renew_after`: da quando il ricevitore deve chiedere il rinnovo (di solito 7 giorni dopo `issued`). Senza rinnovo il pacchetto resta valido; gli extra restano validi fino alla loro scadenza, ma solo finché l'ultimo contatto riuscito con il server (o l'emissione) non è più vecchio di 30 giorni (tolleranza, §1): oltre, si sospendono dal riavvio successivo fino al rinnovo. Il server non deve emettere scadenze extra oltre la data pagata.
- Il ricevitore rifiuta una licenza con `serial` o `chip` diversi dai suoi, o con `issued` precedente a quella che ha già (niente ritorno a una licenza vecchia).
- Ora: il ricevitore usa NTP o l'ora dai satelliti. Senza un'ora valida non considera scaduto nulla. Per non farsi ingannare da un orologio portato indietro, il ricevitore conta come "adesso" almeno la piu' recente fra `issued`, l'ultimo contatto riuscito con il server e l'ultima ora valida salvata (`lasttime`). `lasttime` non scende mai, tranne quando arriva una licenza nuova: l'ora del server fa fede, e un `lasttime` o un ultimo contatto oltre un giorno dopo `issued` (orologio finito nel futuro) vengono riportati a `issued`. Con una licenza nuova l'ultimo contatto è `issued`; con la risposta `304` (non firmata) è l'ora del ricevitore, se valida; senza ora valida non si aggiorna.
- Salvataggio nel ricevitore: licenza, firma e `kid` stanno in **un solo blob NVS** (`pack`: lunghezza e `kid`, lunghezza e firma, poi il JSON), scritto in un'unica operazione: un'interruzione di corrente non lascia mai una licenza mescolata. Le chiavi separate delle versioni precedenti si leggono ancora.
- Chiave del ricevitore: rigenerata solo se manca davvero (`ESP_ERR_NVS_NOT_FOUND`), mai per un errore di lettura né se c'è ma non si decodifica; generata con il generatore casuale hardware acceso.
- Matricola: con una licenza salvata non si cambia dal pannello (la licenza è legata alla matricola).

## 6. API del server

Base: `https://rtk.evo-net.it/api/device/v1/` (da confermare; modificabile dal pannello per le prove). Sempre HTTPS, chiamate solo in uscita dal ricevitore (funziona anche dietro il NAT della SIM). Risposte JSON; errori con `{"error": "codice", "message": "testo in italiano da mostrare nel pannello"}`: il pannello mostra il motivo («Attivazione non riuscita: …»).

**Busta firmata.** Ogni richiesta viaggia come `{"req": "<base64 dei byte del JSON>", "sig": "<firma ECDSA DER del ricevitore>"}`. Per `activate` il server verifica la firma con la `device_pub` contenuta nella richiesta (SPKI in base64), per `renew` con quella registrata all'attivazione.

**Ora della richiesta (`ts`) e richieste ripetute.** Ogni richiesta porta `ts`, l'ora del ricevitore (0 se non ha ancora un'ora valida). Il server rifiuta con `replay` una richiesta con `ts` non successivo all'ultimo accettato per quel ricevitore, o lontano piu' di un'ora dalla sua ora. `ts = 0` e' accettato (limite noto: una richiesta senza ora si puo' ripetere, ma non da' nulla in piu' di quella originale).

**Limite ai tentativi.** Contano solo i tentativi **sbagliati** (codice errato, firma non valida, ecc.) per indirizzo IP: dopo troppi errori risponde `too_many` per qualche minuto. I rinnovi corretti non sono mai limitati. Dietro un proxy, l'indirizzo vero si legge da `X-Forwarded-For` **solo** se la richiesta arriva da un proxy elencato in `trusted_proxies` della configurazione del server.

### 6.1 `POST activate`

Chiamata dalla pagina di attivazione.

```json
{
  "serial": "123456",
  "chip": "a0b1c2d3e4f5",
  "device_pub": "<base64>",
  "code": "EVO-7KQ2-MX4P-9TRA",
  "fw": "1.30.2",
  "fw_sha256": "<impronta del firmware in esecuzione>",
  "mode": "base",
  "accept": {
    "terms_version": "2026-10-08-bozza",
    "clauses_version": "2026-10-08-bozza",
    "terms": true,
    "clauses_1341": true,
    "who": "Mario Rossi",
    "at": 1791504000
  },
  "nonce": "<16 byte casuali in base64>",
  "ts": 1791504000
}
```

Risposta `200`: la licenza (§5). Errori: `code_invalid`, `code_used` (codice già usato su un altro ricevitore), `code_other_device` (codice di un'altra matricola, o codice libero di un altro cliente su una matricola già registrata), `serial_other_chip` (matricola già attivata su un'altra scheda), `revoked`, `terms_missing`, `bad_signature`, `replay`, `too_many`, `not_configured`, `key_change_recent` (riattivazione con una chiave nuova e un codice già usato mentre il ricevitore risulta attivo negli ultimi 7 giorni: dopo un ripristino di fabbrica serve un codice nuovo), `conflict` (modifica contemporanea: riprovare).

Un codice libero, al primo uso, resta legato a quel ricevitore: «scheda sostituita» lo annulla come gli altri. L'attivazione è scritta in un'unica operazione (codice, ricevitore, accettazione): se nel frattempo il ricevitore è stato revocato o attivato da un'altra scheda, non resta scritto nulla. Il registro delle accettazioni conserva anche la richiesta firmata, la firma, la chiave del ricevitore e il codice usato. Il `kid` della chiave del server è lungo al massimo 15 caratteri (limite del firmware).

Il pannello corregge da solo i codici scritti male (minuscole, spazi, trattini mancanti, prefisso `EVO-` dimenticato).

Il server registra: data e ora (sua), indirizzo IP, matricola, chip, cliente, versione del firmware, **versione dei testi accettati**, nome indicato, entrambe le spunte. Il registro non si cancella (prova dell'accettazione).

### 6.2 `POST renew`

Ogni giorno dopo `renew_after`, al primo collegamento utile, e dal pulsante «Aggiorna licenza» del pannello.

```json
{ "serial": "123456", "chip": "a0b1c2d3e4f5", "fw": "1.30.2", "fw_sha256": "<impronta del firmware in esecuzione>", "in_use": ["rover", "nmea_fast"], "issued": 1791504000, "nonce": "<base64>", "ts": 1791590400 }
```

Risposta `200`: licenza nuova (funzioni aggiornate dopo un pagamento o una disdetta, `revoked` dopo una revoca), oppure `304` se non cambia nulla. Errori: `serial_unknown`, `bad_signature`, `replay`. Il server salva la licenza nuova in modo condizionato: se nel frattempo l'amministrazione ha cambiato il ricevitore (revoca, nuove funzioni), il rinnovo non sovrascrive la modifica.

### 6.3 Portale (lato server, non usato dal firmware)

- Elenco dei ricevitori del cliente, con funzioni e scadenze.
- Codici di attivazione: uno per ricevitore venduto, monouso, legato alla matricola o libero.
- Acquisto e rinnovo delle funzioni extra, legato alla fatturazione del caster (`internal/billing`).
- Revoca e riattivazione (cambio di proprietario: il nuovo proprietario riattiva con un codice nuovo e accetta di nuovo le condizioni).
- **Ripristino di fabbrica** (pulsante BOOT, firmware 1.30.6): cancella tutti i namespace NVS tranne `license`, quindi chiave del ricevitore e licenza restano e non serve riattivare. Solo la cancellazione completa della NVS (memoria piena o versione incompatibile) perde la licenza: per proteggere anche quella serve una partizione NVS separata (da decidere, richiede un flash via USB).
- **Scheda sostituita** («sblocca la scheda»): cancella chip e chiave del ricevitore e **annulla i codici già emessi**, cosi' il vecchio codice non riattiva la scheda vecchia (per esempio rubata). Un ricevitore revocato resta revocato.
- Controlli: esattamente un pacchetto per ricevitore; la macchina collegata deve essere dello stesso cliente; base e cliente devono esistere.

## 7. Pannello del ricevitore

- **Pagina di attivazione** (stato `da_attivare`): testo delle avvertenze, note legali e garanzia, con il link al manuale; spunta 1 «Ho letto e accetto avvertenze di sicurezza, note legali e garanzia»; spunta 2, separata, con l'elenco delle clausole che limitano la responsabilità (artt. 1341-1342 c.c.); nome di chi accetta; codice di attivazione; pulsante «Attiva». Prima della pagina, la configurazione di rete (serve internet per attivare).
- **Manutenzione → Licenza**: stato, cliente, elenco delle funzioni con la scadenza, data dell'ultimo rinnovo, pulsante «Aggiorna licenza», riquadro rosso con il motivo se l'attivazione non riesce, indirizzo del server (prove), «Dimentica la licenza» (solo assistenza).
- **Funzioni non comprese**: menu e comandi in grigio con il lucchetto 🔒 e la scritta «Funzione non compresa nella licenza», non modificabili.
- **Avvisi**: «Le funzioni extra scadono il …» a 30 e 7 giorni; «Licenza non rinnovata da N giorni» oltre i 7 giorni senza contatto con il server.

## 8. Protezione del firmware

**Decisione (08/10/2026): niente Secure Boot ne' flash encryption** (eFuse irreversibili). Senza eFuse la memoria dell'ESP32 si legge e si riscrive via USB: una protezione completa non esiste. L'obiettivo e' rendere lo sblocco difficile e poco conveniente, e tenere il valore dove non si copia.

1. **Il valore resta sul server** (la protezione piu' forte, vedi §9): le correzioni RTK arrivano solo con un account del caster legato a un ricevitore con licenza valida. Un ricevitore manomesso o revocato non riceve correzioni.
2. **Licenza firmata e legata al chip**: il MAC di fabbrica (eFuse di sola lettura, nessuna scrittura) e' nella licenza; firmware e licenza copiati su un'altra scheda non valgono.
3. **Controlli sparsi**: `license_has()` non e' l'unico punto di verifica; controlli ripetuti in punti diversi del codice delle funzioni, piu' controlli di integrita' (impronta del firmware in esecuzione confrontata con quella attesa, firma della licenza ricontrollata a intervalli). Obiettivo: per sbloccare servono modifiche in molti punti del codice macchina.
4. **Segnalazioni al server**: a ogni rinnovo il ricevitore manda versione e impronta SHA-256 del firmware (`esp_ota_get_app_elf_sha256`) e l'elenco delle funzioni in uso. Il server confronta l'impronta con quelle delle release ufficiali e segnala nel portale i ricevitori con firmware non originale o funzioni attive non comprese nella licenza.
5. **Protezione fisica e legale**: contenitore con viti speciali e sigillo di garanzia, porta USB dell'ESP32 non accessibile dall'esterno; condizioni accettate all'attivazione (manomissione = garanzia decaduta e violazione del contratto).
6. **Opzione futura**: chip di sicurezza esterno (ATECC608, I2C) per la chiave del ricevitore, non copiabile. Le schede nuove di produzione potranno avere anche Secure Boot e flash encryption senza cambiare il resto del sistema.

## 9. Collegamento con il caster

- Nel gestionale ogni ricevitore e' legato a una **macchina** (rover) o a una **base** (mountpoint).
- Il caster accetta le credenziali di una macchina o di una base legata a un ricevitore solo se il ricevitore e' **attivo e non revocato**; la revoca blocca le credenziali subito (al prossimo aggiornamento dell'istantanea di autenticazione, circa 30 s), senza aspettare il riavvio del ricevitore.
- Le macchine senza ricevitore EVONETRTK (display dei trattori di altre marche) continuano a funzionare come oggi.
- Il rinnovo della licenza (`renew`) aggiorna nel gestionale la data dell'ultimo contatto, la versione e l'impronta del firmware.

## 10. Ricevitori già esistenti

I ricevitori di prova e quelli consegnati prima del sistema di licenze ricevono, al primo aggiornamento che lo introduce, una licenza con tutte le funzioni (`0` = per sempre) emessa dal server per matricola, senza codice. Vanno comunque fatte accettare le condizioni nel pannello alla prima apertura.

## 11. Prove

- Firmware: server finto sul PC (Node), chiave di prova separata da quella vera; casi: attivazione, codice errato, licenza di un altro ricevitore, licenza vecchia, firma alterata, scadenza durante il lavoro (nessun cambiamento fino al riavvio), ora non valida, server irraggiungibile per 31 giorni, revoca.
- Server: test sulle stesse risposte, con le licenze prodotte verificate da una piccola utilità che usa lo stesso codice del firmware.

## 12. Da decidere

- Prezzi, durata dell'abbonamento, eventuale prova gratuita delle funzioni extra.
- Formato dei codici di attivazione e dove si stampano (fattura, scatola).
- Testi delle spunte e delle clausole: **revisione del legale** prima dell'uso.
- Indirizzo definitivo delle API.
