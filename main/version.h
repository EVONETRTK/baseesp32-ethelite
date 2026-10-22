#pragma once

// Versione del firmware (semantic versioning: MAJOR.MINOR.PATCH). Da
// incrementare ad ogni modifica rilasciata, con una riga di changelog
// corrispondente in CHANGELOG.md. Mostrata nella UI web (scheda Stato) e
// loggata all'avvio - indipendente dalla versione ESP-IDF/git-describe
// che compare comunque nel log di boot.
#define FIRMWARE_VERSION "1.35.5"

// Riassunto breve di questa versione (stessa frase della riga corrispondente
// in CHANGELOG.md, accorciata) - mostrato nella pagina Firmware cosi' chi
// aggiorna sa cosa cambia senza dover andare su GitHub. Da aggiornare ad
// ogni bump di FIRMWARE_VERSION assieme al changelog.
#define FIRMWARE_RELEASE_NOTES "Piu' memoria libera (16 KB sul rover con il Bluetooth) e 4G richiuso subito quando la seriale del modem va in errore."
