# Regole per chi lavora su questo firmware

## Manuale: va aggiornato a ogni nuova versione

Il manuale per agricoltori e tecnici sta in [docs/manuale/](docs/manuale/) e porta lo stesso numero di versione del firmware. **Ogni nuova versione deve aggiornarlo**, senza aspettare che qualcuno lo chieda:

1. **Nella stessa modifica del firmware**: se cambiano funzioni, schede, campi, testi o comportamenti del pannello (`main/web/index.html`) o della base, aggiornare i capitoli interessati in `docs/manuale/src/*.html`. Se il capitolo non è ancora scritto, aggiornare la voce prevista in `docs/manuale/struttura.json`. Se il pannello cambia aspetto, rifare le schermate in `docs/manuale/img/`.
2. **A ogni release GitHub**: rigenerare con `node docs/manuale/build.js` (la versione in copertina viene da `main/version.h`), controllare il PDF e allegare `docs/manuale/out/manuale.pdf` e `docs/manuale/out/manuale.html` alla release, così il link "latest" porta sempre al manuale giusto.
3. Nel riepilogo della modifica indicare cosa è cambiato nel manuale, oppure "manuale: nessuna modifica necessaria".

Negli esempi del manuale non usare mai matricole, indirizzi o password reali: il manuale viene distribuito ai clienti.
