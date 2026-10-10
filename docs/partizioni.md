# Tabella delle partizioni della flash

La flash della scheda (LilyGO T-ETH-Elite ESP32-S3) è da **16 MB**. Dal firmware **1.31.0** (10/10/2026) la tabella è questa (`partitions.csv`):

| Partizione | Indirizzo | Dimensione | Uso |
|---|---|---|---|
| nvs | 0x9000 | 64 KB | impostazioni, contatori, stato |
| otadata | 0x19000 | 8 KB | quale firmware avviare |
| phy_init | 0x1B000 | 4 KB | taratura della radio |
| nvs_lic | 0x1C000 | 16 KB | **licenza e chiave del ricevitore**, separate dal resto |
| ota_0 | 0x20000 | 4 MB | firmware A |
| ota_1 | 0x420000 | 4 MB | firmware B (aggiornamenti e ritorno automatico) |
| libero | 0x820000 | ~7,9 MB | usi futuri (es. log interni) |

Fino alla 1.30.7 la tabella era: nvs 24 KB, otadata, phy_init, ota_0 e ota_1 da **2 MB**, nessuna `nvs_lic`.

## Compatibilità

- La tabella delle partizioni **non si cambia via OTA**: serve il cavo USB, una volta sola per ricevitore.
- Il firmware funziona con **entrambe** le tabelle: se `nvs_lic` non esiste, la licenza resta nella NVS principale come prima (`license.c`, `lic_storage_init`). Con la tabella nuova, al primo avvio la licenza viene copiata in `nvs_lic` (la copia vecchia resta, serve se il bootloader torna a un firmware precedente).
- **REGOLA**: finché esistono ricevitori con la tabella vecchia, ogni firmware deve stare sotto **2 MB** (0x200000 byte). La compilazione controlla solo la partizione più piccola della tabella nuova (4 MB): prima di ogni release controllare la dimensione di `build/baseesp32-ethelite.bin`.

## Procedura via USB (ricevitore già in uso)

Conserva impostazioni, reti WiFi, credenziali e licenza.

1. Scaricare la copia completa della configurazione (`/api/config/export`) come riserva.
2. Collegare il ricevitore al PC con il cavo USB (porta COM della console).
3. Salvare la vecchia NVS: `esptool.py -p COMx read_flash 0x9000 0x6000 nvs_vecchia.bin`.
4. Cancellare tutta la flash e scrivere il firmware con la tabella nuova: `idf.py -p COMx erase-flash flash`.
5. Rimettere la vecchia NVS all'inizio di quella nuova (le pagine in più restano vuote e la NVS le usa): `esptool.py -p COMx write_flash 0x9000 nvs_vecchia.bin`.
6. Riavviare e controllare: versione, rete, caster, `license.stored` se il ricevitore era attivato.

Se qualcosa va storto: la configurazione si ricarica dalla copia del punto 1 (*Manutenzione → Ripristina da copia completa*); la licenza, se persa, si riattiva con un codice nuovo.

## Ricevitori nuovi

Escono dalla produzione già con la tabella nuova (`idf.py flash`).
