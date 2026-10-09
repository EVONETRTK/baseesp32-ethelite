#pragma once

// Versione del firmware (semantic versioning: MAJOR.MINOR.PATCH). Da
// incrementare ad ogni modifica rilasciata, con una riga di changelog
// corrispondente in CHANGELOG.md. Mostrata nella UI web (scheda Stato) e
// loggata all'avvio - indipendente dalla versione ESP-IDF/git-describe
// che compare comunque nel log di boot.
#define FIRMWARE_VERSION "1.30.4"

// Riassunto breve di questa versione (stessa frase della riga corrispondente
// in CHANGELOG.md, accorciata) - mostrato nella pagina Firmware cosi' chi
// aggiorna sa cosa cambia senza dover andare su GitHub. Da aggiornare ad
// ogni bump di FIRMWARE_VERSION assieme al changelog.
#define FIRMWARE_RELEASE_NOTES "Scheda Stato piu' pulita: le spie in alto (rete, caster, posizione, SD) compaiono nelle altre schede, non piu' doppie sopra il giudizio e i riquadri."
