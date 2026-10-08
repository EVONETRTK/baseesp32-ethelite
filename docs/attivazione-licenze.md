# Attivazione del ricevitore e licenze

Specifica condivisa tra firmware (questo repository) e server EVONETRTK (gestionale del caster, repository `snip`). Stato: **bozza 2, 08/10/2026**. Le decisioni commerciali sono dell'azienda; i dettagli tecnici possono cambiare finché non c'è la prima versione in campo.

## 1. Decisioni

| Punto | Decisione |
|---|---|
| Modello | Acquisto + abbonamento: le funzioni base sono comprese nel prezzo del ricevitore e restano per sempre; le funzioni extra hanno un canone annuale. |
| Chi attiva | Il **cliente**, con un codice di attivazione (in fattura o nel portale), accettando lui le condizioni nel pannello. |
| Scadenza | Avvisi a 30 e a 7 giorni. Dopo la scadenza le funzioni extra si spengono **solo al riavvio successivo**, mai durante il lavoro. Le funzioni base (RTK) non si spengono mai. |
| Senza internet | La licenza è firmata e verificata dal ricevitore: funziona anche offline. Rinnovo automatico quando c'è rete; tolleranza di 30 giorni se il server non risponde. |

## 2. Funzioni

Codici usati nella licenza e nel firmware (`license_has()`).

| Codice | Livello | Cosa sblocca |
|---|---|---|
| `rtk` | base | Base: invio correzioni al caster. Rover: correzioni dal caster, uscita NMEA **1 Hz**. Configurazione, aggiornamenti, scheda di accesso. |
| `nmea_fast` | extra | Uscita per l'autosterzo a 5 e 10 Hz (`nmea_rate_hz`). |
| `has` | extra | Misura della posizione base con Galileo HAS; riserva HAS del rover. |
| `ins` | extra | IMU, doppia antenna, prua e compensazione dell'inclinazione (ComNav K922). |
| `base_pro` | extra | Caster locale, VPN, monitoraggio remoto, registrazione dei dati grezzi (PPP), avvisi email/WhatsApp. |

Restano sempre disponibili, anche senza licenza valida, tutto ciò che serve alla sicurezza e all'assistenza: configurazione di rete, aggiornamento del firmware, log, ripristino, controllo dello spostamento dell'antenna, avviso 1005.

## 3. Stati del ricevitore

| Stato | Quando | Cosa funziona |
|---|---|---|
| `da_attivare` | Nuovo o dopo il ripristino di fabbrica, nessuna licenza valida | Solo pannello: rete, pagina di attivazione, aggiornamento firmware. Nessuna correzione. |
| `attivo` | Licenza valida | Le funzioni elencate nella licenza. |
| `in_scadenza` | Extra che scadono entro 30 giorni | Tutto; avviso nel pannello (e a 7 giorni anche email/WhatsApp, se configurati). |
| `extra_scaduti` | Extra scaduti | Fino al riavvio: tutto come prima. Dal riavvio: solo `rtk`. |
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
    "rtk": 0,
    "nmea_fast": 1823040000,
    "has": 1823040000
  },
  "revoked": false
}
```

- `features`: codice → scadenza (Unix, UTC); `0` = per sempre.
- `renew_after`: da quando il ricevitore deve chiedere il rinnovo (di solito 7 giorni dopo `issued`). Senza rinnovo la licenza resta valida fino alle scadenze delle singole funzioni; il server non deve emettere scadenze extra oltre la data pagata.
- Il ricevitore rifiuta una licenza con `serial` o `chip` diversi dai suoi, o con `issued` precedente a quella che ha già (niente ritorno a una licenza vecchia).
- Ora: il ricevitore usa NTP o l'ora dai satelliti. Senza un'ora valida non considera scaduto nulla.

## 6. API del server

Base: `https://rtk.evo-net.it/api/device/v1/` (da confermare). Sempre HTTPS, chiamate solo in uscita dal ricevitore (funziona anche dietro il NAT della SIM). Risposte JSON; errori con `{"error": "codice", "message": "testo in italiano da mostrare nel pannello"}`.

### 6.1 `POST activate`

Chiamata dalla pagina di attivazione.

```json
{
  "serial": "123456",
  "chip": "a0b1c2d3e4f5",
  "device_pub": "<base64>",
  "code": "EVO-7KQ2-MX4P-9TRA",
  "fw": "1.29.0",
  "mode": "base",
  "accept": {
    "terms_version": "2026-10-08",
    "clauses_version": "2026-10-08",
    "terms": true,
    "clauses_1341": true,
    "who": "Mario Rossi",
    "at": 1791504000
  },
  "nonce": "<16 byte casuali in base64>",
  "sig": "<firma con la chiave del ricevitore di tutti i campi sopra>"
}
```

Risposta `200`: la licenza (§5). Errori: `code_invalid`, `code_used` (codice già usato su un altro ricevitore), `serial_unknown`, `terms_missing`.

Il server registra: data e ora (sua), indirizzo IP, matricola, chip, cliente, versione del firmware, **versione dei testi accettati**, nome indicato, entrambe le spunte. Il registro non si cancella (prova dell'accettazione).

### 6.2 `POST renew`

Ogni giorno dopo `renew_after`, al primo collegamento utile, e dal pulsante «Aggiorna licenza» del pannello.

```json
{ "serial": "123456", "chip": "a0b1c2d3e4f5", "fw": "1.29.0", "fw_sha256": "<impronta del firmware in esecuzione>", "in_use": ["rtk", "nmea_fast"], "issued": 1791504000, "nonce": "<base64>", "sig": "<firma del ricevitore>" }
```

Risposta `200`: licenza nuova (funzioni aggiornate dopo un pagamento o una disdetta, `revoked` dopo una revoca), oppure `304` se non cambia nulla.

### 6.3 Portale (lato server, non usato dal firmware)

- Elenco dei ricevitori del cliente, con funzioni e scadenze.
- Codici di attivazione: uno per ricevitore venduto, monouso, legato alla matricola o libero.
- Acquisto e rinnovo delle funzioni extra, legato alla fatturazione del caster (`internal/billing`).
- Revoca e riattivazione (cambio di proprietario: il nuovo proprietario riattiva con un codice nuovo e accetta di nuovo le condizioni).

## 7. Pannello del ricevitore

- **Pagina di attivazione** (stato `da_attivare`): testo delle avvertenze, note legali e garanzia, con il link al manuale; spunta 1 «Ho letto e accetto avvertenze di sicurezza, note legali e garanzia»; spunta 2, separata, con l'elenco delle clausole che limitano la responsabilità (artt. 1341-1342 c.c.); nome di chi accetta; codice di attivazione; pulsante «Attiva». Prima della pagina, la configurazione di rete (serve internet per attivare).
- **Manutenzione → Licenza**: stato, cliente, elenco delle funzioni con la scadenza, data dell'ultimo rinnovo, pulsante «Aggiorna licenza», collegamento al portale per acquistare.
- **Funzioni non comprese**: visibili con il lucchetto 🔒 e la scritta «Funzione non compresa nella licenza», non modificabili.
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
