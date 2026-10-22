# esp_modem dentro il progetto

Copia di `espressif/esp_modem` 1.4.3 (registro dei componenti Espressif), portata qui il 22/10/2026 invece di scaricarla dal registro, per una modifica:

- `src/esp_modem_dte.cpp`, `DTE::handle_error()`: un errore della seriale di tipo "Rx Break / formato" (`UNEXPECTED_CONTROL_FLOW`) non fa piu' ripartire il CMUX (`recover()`). Sulla scheda di riferimento (T-ETH-Elite + shield LTE LilyGO + HAT ZED-F9P) arriva un disturbo di circa 1 al secondo sulla linea dal modem all'ESP32 (GPIO4), anche con il modem fermo e con PPS dello ZED e GPS dello shield spenti. Ogni ripartenza del CMUX buttava via le trame in arrivo e, a cascata, dava migliaia di ripartenze con il 4G fermo per minuti.

Le altre parti sono identiche all'originale (esempi e prove tolti). Per aggiornare esp_modem: scaricare la nuova versione, riportare la modifica qui sopra e aggiornare questo file.
