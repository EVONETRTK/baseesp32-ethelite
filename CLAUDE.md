# Regole per chi lavora su questo firmware

## Manuale: va aggiornato a ogni nuova versione

Il manuale per agricoltori e tecnici sta in [docs/manuale/](docs/manuale/) e porta lo stesso numero di versione del firmware. **Ogni nuova versione deve aggiornarlo**, senza aspettare che qualcuno lo chieda:

1. **Nella stessa modifica del firmware**: se cambiano funzioni, schede, campi, testi o comportamenti del pannello (`main/web/index.html`) o della base, aggiornare i capitoli interessati in `docs/manuale/src/*.html`. Se il capitolo non è ancora scritto, aggiornare la voce prevista in `docs/manuale/struttura.json`. Se il pannello cambia aspetto, rifare le schermate in `docs/manuale/img/`.
2. **A ogni release GitHub**: rigenerare con `node docs/manuale/build.js` (la versione in copertina viene da `main/version.h`), controllare il PDF e allegare `docs/manuale/out/manuale.pdf` e `docs/manuale/out/manuale.html` alla release, così il link "latest" porta sempre al manuale giusto.
3. Nel riepilogo della modifica indicare cosa è cambiato nel manuale, oppure "manuale: nessuna modifica necessaria".

Negli esempi del manuale non usare mai matricole, indirizzi o password reali: il manuale viene distribuito ai clienti.

## Prove automatiche sul PC

Le parti di calcolo e di lettura dei dati (NMEA/GGA, conversione ETRF2000, RTCM 1005 e CRC24Q, separazione del flusso della base, UBX, log ASCII Unicore, media della misura, calcoli del controllo dello spostamento, validazioni del pannello) hanno prove che girano sul PC Windows, non sulla scheda, in [tests/host/](tests/host/):

```powershell
powershell -ExecutionPolicy Bypass -File tests\host\run.ps1            # tutte
powershell -ExecutionPolicy Bypass -File tests\host\run.ps1 nmea_etrf  # solo test_nmea_etrf.c
```

Servono solo zig (`C:\Users\Utente\tools\zig-windows-x86_64-0.13.0\zig.exe`, oppure il percorso nella variabile `ZIG`); non serve ESP-IDF. Il giro stampa "N prove, M fallite" ed esce con codice 1 se una prova fallisce o non compila.

1. **Quando**: prima di ogni build da installare su una base e prima di ogni release. Se fallisce, non si installa e non si pubblica finché non è chiaro il perché.
2. **Quando si corregge un bug** in una di queste parti, aggiungere nella stessa modifica una prova che lo mostrava (fallisce prima della correzione, passa dopo).
3. Le prove compilano i `.c` veri di `main/` (inclusi nel file di prova) con gli stub minimi di ESP-IDF/FreeRTOS in `tests/host/stub/`: non copiare codice del firmware nelle prove. Le funzioni di validazione di `web_ui.c` vengono estratte da `run.ps1` a ogni giro (se cambiano nome, aggiornare `run.ps1`).
4. Valori attesi dei calcoli (ETRF2000, frame RTCM, CRC, Klobuchar, effemeride) da `tests/host/strumenti/riferimenti.py` (Python, calcolo indipendente): se si cambia un caso di verifica, rilanciarlo e ricopiare i valori.
5. Un bug noto non ancora corretto si segna con `ESEGUI_BUG(nome, "spiegazione")`: compare come "bug noto" e non blocca il giro.
6. Niente dati privati nelle prove (indirizzi, matricole, password): coordinate di punti generici.
