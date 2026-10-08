// Pittogrammi di sicurezza del manuale, disegnati nello stile di ISO 3864/7010
// (forme e colori: triangolo giallo = pericolo, cerchio blu = obbligo, cerchio
// rosso barrato = divieto). Disegni originali, non le tavole ISO.
// Uso nei capitoli: <div class="box avvertenza" data-pitto="elettricita">.
'use strict';

const giallo = '#F9A800';
const nero = '#1a1a1a';

// Triangolo di pericolo con il simbolo dentro (coordinate 0-100).
const triangolo = (simbolo) => `<svg viewBox="0 0 100 92" xmlns="http://www.w3.org/2000/svg" class="pitto" role="img">
<path d="M50 4 L96 86 Q98 89 94 89 H6 Q2 89 4 86 Z" fill="${giallo}" stroke="${nero}" stroke-width="6" stroke-linejoin="round"/>
<g fill="${nero}" stroke="${nero}">${simbolo}</g></svg>`;

const obbligo = (simbolo) => `<svg viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg" class="pitto" role="img">
<circle cx="50" cy="50" r="47" fill="#1f5fa8"/><g fill="#fff" stroke="#fff">${simbolo}</g></svg>`;

const divieto = (simbolo) => `<svg viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg" class="pitto" role="img">
<circle cx="50" cy="50" r="44" fill="#fff" stroke="#c4161c" stroke-width="9"/>
<g fill="${nero}" stroke="${nero}">${simbolo}</g>
<path d="M19 19 L81 81" stroke="#c4161c" stroke-width="9"/></svg>`;

const PITTOGRAMMI = {
  // Pericolo generico
  generico: triangolo('<rect x="45" y="32" width="10" height="32" rx="3" stroke="none"/><circle cx="50" cy="75" r="6" stroke="none"/>'),
  // Elettricita' / folgorazione
  elettricita: triangolo('<path d="M55 28 L38 60 H50 L44 82 L64 50 H52 L60 28 Z" stroke="none"/>'),
  // Caduta dall'alto
  caduta: triangolo('<circle cx="58" cy="36" r="6" stroke="none"/><path d="M56 44 L46 58 L54 66 M48 54 L36 52 M54 50 L66 58" fill="none" stroke-width="5" stroke-linecap="round"/><path d="M22 78 H44 V70" fill="none" stroke-width="4"/>'),
  // Fulmini
  fulmine: triangolo('<path d="M33 52 Q30 40 42 39 Q46 30 56 33 Q66 31 67 41 Q75 43 72 52 Z" stroke="none"/><path d="M52 52 L44 66 H51 L46 80 L60 62 H53 L57 52 Z" stroke="none"/>'),
  // Macchina in movimento / autosterzo (volante)
  autosterzo: triangolo('<circle cx="50" cy="60" r="18" fill="none" stroke-width="5"/><circle cx="50" cy="60" r="4" stroke="none"/><path d="M50 60 V78 M50 60 L33 52 M50 60 L67 52" fill="none" stroke-width="5"/>'),
  // Onde radio (radiazioni non ionizzanti)
  radio: triangolo('<path d="M47 82 L50 52 L53 82 Z" stroke="none"/><circle cx="50" cy="48" r="4" stroke="none"/><path d="M40 40 A14 14 0 0 1 60 40 M34 34 A22 22 0 0 1 66 34" fill="none" stroke-width="4" stroke-linecap="round"/>'),
  // Incendio (cortocircuito)
  incendio: triangolo('<path d="M50 30 Q64 46 58 58 Q66 54 64 46 Q74 60 66 74 Q60 82 50 82 Q38 82 34 72 Q30 60 40 52 Q40 60 46 62 Q40 46 50 30 Z" stroke="none"/>'),
  // Obbligo: leggere il manuale
  manuale: obbligo('<path d="M24 32 Q38 28 49 34 V74 Q38 68 24 72 Z M76 32 Q62 28 51 34 V74 Q62 68 76 72 Z" stroke="none"/><rect x="47" y="22" width="6" height="6" rx="1" stroke="none" fill="#1f5fa8"/>'),
  // Divieto di aprire o modificare (cacciavite)
  modifiche: divieto('<path d="M30 70 L58 42" stroke-width="7" stroke-linecap="round"/><path d="M56 44 L66 34 L72 40 L62 50 Z" stroke="none"/>'),
  // RAEE: contenitore barrato (smaltimento)
  raee: `<svg viewBox="0 0 100 110" xmlns="http://www.w3.org/2000/svg" class="pitto" role="img">
<g fill="none" stroke="${nero}" stroke-width="5"><path d="M28 30 H72 L66 88 H34 Z"/><path d="M24 30 H76 M40 30 V22 H60 V30"/><path d="M42 42 L44 80 M50 42 V80 M58 42 L56 80" stroke-width="3"/><path d="M14 16 L86 96 M86 16 L14 96" stroke-width="6"/></g>
<rect x="18" y="100" width="64" height="8" fill="${nero}"/></svg>`,
};

// Simbolo di allerta di sicurezza (triangolo con "!") per l'intestazione dei
// riquadri PERICOLO / AVVERTENZA / ATTENZIONE.
const ALLERTA = `<svg viewBox="0 0 100 92" xmlns="http://www.w3.org/2000/svg" class="allerta" aria-hidden="true">
<path d="M50 4 L96 86 Q98 89 94 89 H6 Q2 89 4 86 Z" fill="currentColor"/>
<rect x="45" y="30" width="10" height="34" rx="3" style="fill: var(--sic-fondo)"/><circle cx="50" cy="75" r="6" style="fill: var(--sic-fondo)"/></svg>`;

module.exports = { PITTOGRAMMI, ALLERTA };
