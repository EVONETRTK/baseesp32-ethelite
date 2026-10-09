#!/usr/bin/env node
// Genera il manuale del ricevitore EVONETRTK da docs/manuale/src:
//   out/manuale.html  versione web, un solo file autosufficiente
//   out/manuale.pdf   versione A4 stampabile (Paged.js + Chrome/Edge headless)
// Uso: node build.js [--solo-html]
// Nessuna dipendenza npm: serve Node >= 22 (WebSocket nativo) e Chrome o Edge.

'use strict';
const fs = require('fs');
const path = require('path');
const os = require('os');
const { spawn } = require('child_process');

const DIR = __dirname;
const OUT = path.join(DIR, 'out');
const leggi = (p) => fs.readFileSync(path.join(DIR, p), 'utf8');

const struttura = JSON.parse(leggi('struttura.json'));
// La versione e' quella del firmware nel codice (main/version.h), non quella di
// version.json, che cambia solo quando si pubblica una release.
const versione = fs.readFileSync(path.join(DIR, '..', '..', 'main', 'version.h'), 'utf8').match(/FIRMWARE_VERSION "([^"]+)"/)[1];
const MESI = ['gennaio', 'febbraio', 'marzo', 'aprile', 'maggio', 'giugno', 'luglio', 'agosto', 'settembre', 'ottobre', 'novembre', 'dicembre'];
const oggi = new Date();
const dataEdizione = `${MESI[oggi.getMonth()]} ${oggi.getFullYear()}`;

const css = leggi('manuale.css');
const logoDataUri = 'data:image/svg+xml;base64,' + Buffer.from(leggi('img/logo.svg')).toString('base64');

const escapeHtml = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
const testoPuro = (html) => html.replace(/<[^>]+>/g, '').replace(/\s+/g, ' ').trim();

// Le immagini dei capitoli (src="img/...") vengono incorporate come data URI:
// la versione web resta un solo file da scaricare o pubblicare.
const TIPI_IMG = { '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.svg': 'image/svg+xml' };
// Tasti del pannello: un nome di pulsante scritto in grassetto nel testo
// (<strong>Salva</strong>) diventa un tasto disegnato come nel pannello, con la
// stessa icona e lo stesso colore. Nomi e stili si leggono da main/web/index.html
// (e dalla scheda di accesso, main/web/access.html), cosi' restano allineati al
// pannello anche quando un pulsante cambia.
const ICONA = /[\p{Extended_Pictographic}\u{FE0F}\u{200D}]/gu;
const decodifica = (t) => t.replace(/&nbsp;|&#160;/g, ' ').replace(/&amp;/g, '&');
const senzaIcone = (t) => decodifica(t).replace(ICONA, '').replace(/\s+/g, ' ').trim();
const chiaveTasto = (t) => senzaIcone(t).toLowerCase().replace(/[«»"']/g, '');
const tastiPannello = (() => {
  const WEB = path.join(DIR, '..', '..', 'main', 'web');
  const pannello = fs.readFileSync(path.join(WEB, 'index.html'), 'utf8');
  const accesso = fs.readFileSync(path.join(WEB, 'access.html'), 'utf8');
  const tasti = [];   // in ordine di pagina: a parita' di nome vale il primo
  const re = /<(button|a)\b([^>]*)>([\s\S]*?)<\/\1>/g;
  let m;
  while ((m = re.exec(pannello))) {
    const cls = (m[2].match(/class="([^"]*)"/) || [])[1] || '';
    if (!/\baction\b/.test(cls)) continue;
    tasti.push({ etichetta: decodifica(m[3].replace(/<[^>]+>/g, '')).replace(/\s+/g, ' ').trim(), secondario: /\bsecondary\b/.test(cls) });
  }
  // Pulsanti creati dal JavaScript del pannello (il testo non e' nell'HTML):
  // si controlla che l'etichetta ci sia ancora, per non tenere nomi spariti.
  for (const [etichetta, secondario] of [['🔄 Salva e riavvia', true], ['Mostra', true], ['Nascondi', true], ['Ripristina', true]]) {
    if (pannello.includes(`'${etichetta}'`)) tasti.push({ etichetta, secondario });
    else console.warn(`tasto del pannello non piu' trovato in index.html: ${etichetta}`);
  }
  // Scheda di accesso: blu i pulsanti normali, grigio quelli con classe "alt".
  const reA = /<button\b([^>]*)>([\s\S]*?)<\/button>/g;
  while ((m = reA.exec(accesso))) {
    const cls = (m[1].match(/class="([^"]*)"/) || [])[1] || '';
    tasti.push({ etichetta: decodifica(m[2].replace(/<[^>]+>/g, '')).replace(/\s+/g, ' ').trim(), secondario: /\balt\b/.test(cls) });
  }
  const validi = tasti.filter((t) => t.etichetta && t.etichetta.length <= 60 && senzaIcone(t.etichetta));
  // Due passate: prima i nomi completi, poi quelli senza la parentesi finale
  // («Scarica log (.txt)» -> «scarica log»), che non devono mai prendere il
  // posto del nome completo di un altro pulsante.
  const mappa = new Map();
  for (const t of validi) { const k = chiaveTasto(t.etichetta); if (k && !mappa.has(k)) mappa.set(k, t); }
  for (const t of validi) { const k = chiaveTasto(t.etichetta.replace(/\s*\(.*\)\s*$/, '')); if (k && !mappa.has(k)) mappa.set(k, t); }
  return mappa;
})();
let nTasti = 0;
// I titoli (h1-h6) restano come sono: un tasto nel titolo finirebbe anche nell'indice.
const tastiInLinea = (html) => html.replace(/<(h[1-6])\b[\s\S]*?<\/\1>|<strong>([^<]{2,60})<\/strong>/g, (orig, titolo, testo) => {
  if (titolo) return orig;
  // Un nome tra virgolette e' una citazione, non un tasto.
  if (/[«»"]|&quot;/.test(testo)) return orig;
  const t = tastiPannello.get(chiaveTasto(testo));
  const nudo = senzaIcone(testo);
  // Solo con l'iniziale maiuscola come nel pannello: «attiva» nel testo e' una parola, non il tasto.
  if (!t || !nudo || nudo[0] !== nudo[0].toUpperCase()) return orig;
  nTasti++;
  // Il testo del manuale puo' citare solo la parte prima della parentesi: si
  // mostra cosi', con l'icona del pannello davanti.
  const icone = (t.etichetta.match(/^[\p{Extended_Pictographic}\u{FE0F}\u{200D}\s]+/u) || [''])[0].trim();
  const giaConIcona = /^\p{Extended_Pictographic}/u.test(decodifica(testo).trim());
  return `<span class="tasto${t.secondario ? ' secondario' : ''}">${icone && !giaConIcona ? icone + ' ' : ''}${testo.trim()}</span>`;
});

const incorporaImmagini = (html) => html.replace(/src="(img\/[^"]+)"/g, (_, rel) => {
  const tipo = TIPI_IMG[path.extname(rel).toLowerCase()];
  if (!tipo) throw new Error(`tipo di immagine non gestito: ${rel}`);
  return `src="data:${tipo};base64,${fs.readFileSync(path.join(DIR, rel)).toString('base64')}"`;
});

// Riquadri di sicurezza (sistema ISO 3864-2 / ANSI Z535.6):
//   pericolo    morte o lesioni gravi certe se non si evita
//   avvertenza  morte o lesioni gravi possibili
//   attenzione  lesioni lievi possibili
//   avviso      solo danni alle cose, ai dati o al funzionamento (senza triangolo)
// Nei capitoli: <div class="box avvertenza" data-pitto="elettricita">
//   <span class="box-titolo">titolo facoltativo</span> testo </div>
// Il riquadro non deve contenere altri <div>.
const { PITTOGRAMMI, ALLERTA } = require('./pittogrammi');
const PAROLE = { pericolo: 'PERICOLO', avvertenza: 'AVVERTENZA', attenzione: 'ATTENZIONE', avviso: 'AVVISO' };
const riquadriSicurezza = (html) => html.replace(
  /<div class="box (pericolo|avvertenza|attenzione|avviso)"(?: data-pitto="([a-z]+)")?>([\s\S]*?)<\/div>/g,
  (_, livello, pitto, corpo) => {
    if (pitto && !PITTOGRAMMI[pitto]) throw new Error(`pittogramma sconosciuto: ${pitto}`);
    // Un titolo uguale alla parola di segnalazione sarebbe un doppione.
    corpo = corpo.replace(/^\s*<span class="box-titolo">\s*(Pericolo|Avvertenza|Attenzione|Avviso)\s*<\/span>/, '');
    const testa = `<div class="sic-testa">${livello === 'avviso' ? '' : ALLERTA}<span>${PAROLE[livello]}</span></div>`;
    const figura = pitto ? `<div class="sic-pitto">${PITTOGRAMMI[pitto]}</div>` : '';
    return `<div class="box sic ${livello}">${testa}<div class="sic-corpo">${figura}<div class="sic-testo">${corpo}</div></div></div>`;
  });

// Pittogrammi dentro il testo (tabelle dei simboli): <span data-pitto-solo="raee"></span>
const pittogrammiInLinea = (html) => html.replace(/<span data-pitto-solo="([a-z]+)"><\/span>/g, (_, p) => {
  if (!PITTOGRAMMI[p]) throw new Error(`pittogramma sconosciuto: ${p}`);
  return `<span class="pitto-solo">${PITTOGRAMMI[p]}</span>`;
});

// Spazi da completare con i dati dell'azienda: [[testo]] diventa un riquadrino
// giallo ben visibile, e lo script li conta per non dimenticarli.
let daCompletare = 0;
const segnaDaCompletare = (html) => html.replace(/\[\[([^\]]+)\]\]/g, (_, t) => {
  daCompletare++;
  return `<span class="da-completare">${t}</span>`;
});

// ---------- capitoli ----------

let numero = 0;
let lettera = 0;
const indice = [];   // [{parte, voci:[{n, titolo, id, scritto, sezioni:[{n,titolo,id}]}]}]
const corpo = [];

for (const parte of struttura.parti) {
  const voci = [];
  for (const cap of parte.capitoli) {
    const n = parte.appendici ? String.fromCharCode(65 + lettera++) : String(++numero);
    const id = parte.appendici ? `app-${n.toLowerCase()}` : `cap-${n}`;
    const voce = { n, titolo: cap.titolo, id, scritto: !!cap.file, sezioni: [] };

    if (cap.file) {
      let k = 0;
      let html = tastiInLinea(incorporaImmagini(leggi(path.join('src', cap.file)))).replace(/<h2>([\s\S]*?)<\/h2>/g, (_, t) => {
        const sid = `${id}-${++k}`;
        voce.sezioni.push({ n: `${n}.${k}`, titolo: testoPuro(t), id: sid });
        return `<h2 id="${sid}"><span class="num">${n}.${k}</span> ${t}</h2>`;
      });
      const etichetta = parte.appendici ? `Appendice ${n}` : `Capitolo ${n}`;
      corpo.push(
        `<section class="capitolo" id="${id}" data-titolo="${escapeHtml(`${n}. ${cap.titolo}`)}">\n` +
        `<span class="capitolo-numero">${etichetta}</span>\n<h1>${escapeHtml(cap.titolo)}</h1>\n${html}\n</section>`
      );
    } else {
      voce.sezioni = (cap.previsto || []).map((t, i) => ({ n: `${n}.${i + 1}`, titolo: t, id: null }));
    }
    voci.push(voce);
  }
  indice.push({ parte: parte.titolo, voci });
}

// Rimandi tra capitoli scritti per titolo: <a data-cap="Collegare il trattore"></a>
// diventa un link con numero e titolo giusti, anche se i capitoli cambiano posto.
// Un rimando a un capitolo non ancora scritto resta testo, senza link.
// La parola "firewall" e' sempre evidenziata, nel testo (non dentro i tag):
// e' la causa piu' frequente degli intoppi nelle installazioni. Non nei titoli
// dei riquadri (.box-titolo), gia' evidenziati; lo spazio dopo ⚠ non va a capo.
const evidenziaFirewall = (html) => html.replace(/<span class="box-titolo">[^<]*<\/span(?=>)|>([^<]+)(?=<)/g, (m, testo) =>
  testo === undefined ? m : '>' + testo.replace(/\bfirewall\b/gi, (p) => `<span class="firewall">⚠ ${p}</span>`));

function risolviRimandi(html) {
  return html.replace(/<a data-cap="([^"]+)"><\/a>/g, (_, titolo) => {
    const v = indice.flatMap((p) => p.voci).find((x) => x.titolo === titolo);
    if (!v) throw new Error(`rimando a un capitolo inesistente: "${titolo}"`);
    const testo = `${v.n}, <em>${escapeHtml(v.titolo)}</em>`;
    return v.scritto ? `<a href="#${v.id}">${testo}</a>` : testo;
  });
}
const postElabora = (h) => segnaDaCompletare(pittogrammiInLinea(riquadriSicurezza(evidenziaFirewall(risolviRimandi(h)))));
for (let i = 0; i < corpo.length; i++) corpo[i] = postElabora(corpo[i]);

// Figure numerate in ordine in tutto il manuale: inserire un capitolo non
// sballa i numeri scritti a mano.
let nFigura = 0;
for (let i = 0; i < corpo.length; i++) corpo[i] = corpo[i].replace(/<b>Figura \d+\.<\/b>/g, () => `<b>Figura ${++nFigura}.</b>`);

function htmlIndice() {
  const riga = (v, cls = '', nota = '') => {
    const contenuto = `<span class="n">${v.n}</span><span class="t">${escapeHtml(v.titolo)}${nota}</span>`;
    return v.id ? `<a href="#${v.id}"${cls}>${contenuto}</a>` : `<span class="voce-futura"${cls}>${contenuto}</span>`;
  };
  let h = '<ol>';
  for (const p of indice) {
    h += `<li class="parte">${escapeHtml(p.parte)}</li>`;
    for (const v of p.voci) {
      const voce = { ...v, id: v.scritto ? v.id : null };
      h += `<li>${riga(voce, '', v.scritto ? '' : ' <span class="da-scrivere">(da scrivere)</span>')}`;
      if (v.sezioni.length) {
        h += '<ol>' + v.sezioni.map((s) => `<li>${riga(s)}</li>`).join('') + '</ol>';
      }
      h += '</li>';
    }
  }
  return h + '</ol>';
}

// ---------- parti fisse ----------

const frontespizio = `
<div class="frontespizio">
  <p class="marchio">EVONETRTK</p>
  <img class="logo" src="${logoDataUri}" alt="Logo EVONETRTK">
  <h1 class="titolo">${escapeHtml(struttura.titolo)}</h1>
  <p class="sottotitolo">${escapeHtml(struttura.sottotitolo)}</p>
  <div class="parti"><span>Il mondo GNSS</span><span>Guida per l'agricoltore</span><span>Manuale tecnico</span></div>
  <div class="dati">Firmware <b>${versione}</b> · Edizione <b>${dataEdizione}</b></div>
  <div class="fascia"></div>
</div>`;

const retro = `
<div class="pagina-retro solo-stampa">
  <p><strong>EVONETRTK · ${escapeHtml(struttura.titolo)}</strong><br>
  Edizione ${dataEdizione}, per il firmware ${versione}.</p>
  <p class="retro-sicurezza"><span class="pitto-solo">${PITTOGRAMMI.manuale}</span> <strong>Prima di installare e usare il ricevitore leggete il capitolo 1, <em>Avvertenze di sicurezza</em>, e l'appendice <em>Note legali e garanzia</em>.</strong></p>
  <p>La versione più recente di questo manuale si scarica dal pulsante <em>Manuale</em> del pannello di controllo del ricevitore.</p>
  <p>Le schermate e i valori riportati possono differire leggermente da quelli della vostra base, a seconda della versione del firmware e della configurazione.</p>
  <p>GPS, Galileo, GLONASS, BeiDou e i nomi dei prodotti citati appartengono ai rispettivi proprietari.</p>
  <p>© ${oggi.getFullYear()} EVONETRTK. Tutti i diritti riservati.</p>
</div>`;

const prefazione = `<section class="prefazione" id="prefazione" data-titolo="Prefazione">${postElabora(leggi("src/00-prefazione.html"))}</section>`;
const paginaIndice = `<section class="indice-pagina" id="indice"><h1>Indice</h1><nav class="indice">${htmlIndice()}</nav></section>`;
const tuttoIlCorpo = corpo.join('\n');

// ---------- versione web ----------

const scriptWeb = `
(function () {
  var btn = document.querySelector('.menu-btn');
  btn.addEventListener('click', function () { document.body.classList.toggle('menu-aperto'); });
  document.querySelectorAll('.barra a').forEach(function (a) {
    a.addEventListener('click', function () { document.body.classList.remove('menu-aperto'); });
  });
  // Ricerca: elenca le sezioni che contengono il testo cercato.
  var campo = document.getElementById('cerca');
  var esiti = document.getElementById('esiti');
  var sezioni = [];
  document.querySelectorAll('section.capitolo h2').forEach(function (h) {
    var testo = '', el = h.nextElementSibling;
    while (el && el.tagName !== 'H2') { testo += ' ' + el.textContent; el = el.nextElementSibling; }
    sezioni.push({ id: h.id, titolo: h.textContent, testo: (h.textContent + testo).toLowerCase() });
  });
  campo.addEventListener('input', function () {
    var q = campo.value.trim().toLowerCase();
    esiti.innerHTML = '';
    if (q.length < 2) { esiti.hidden = true; return; }
    var trovate = sezioni.filter(function (s) { return s.testo.indexOf(q) >= 0; });
    esiti.hidden = false;
    esiti.innerHTML = trovate.length
      ? trovate.map(function (s) { return '<a href="#' + s.id + '">' + s.titolo.replace(/</g, '&lt;') + '</a>'; }).join('')
      : '<p class="nessuno">Nessun risultato.</p>';
  });
})();`;

const cssWeb = `
@media screen {
  #esiti[hidden] { display: none; }
  #esiti { display: flex; flex-direction: column; gap: .2rem; margin: -.4rem 0 1rem; padding: .5rem; background: #fff; border: 1px solid var(--border); border-radius: 6px; }
  #esiti a { text-decoration: none; color: var(--brand-dark); font-size: .88rem; }
  #esiti .nessuno { margin: 0; color: var(--muted); font-size: .88rem; }
  h2 .num { color: var(--brand); margin-right: .2em; }
  .indice .voce-futura { display: flex; gap: .5em; color: #8a928c; }
  .barra .edizione { margin: -.4rem 0 .8rem; font-size: .8rem; color: var(--muted); }
  .indice .voce-futura .n { min-width: 2.2em; font-weight: 700; }
  .barra .indice .voce-futura .n { min-width: 1.8em; }
}`;

const web = `<!doctype html>
<html lang="it">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>EVONETRTK · ${escapeHtml(struttura.titolo)} (firmware ${versione})</title>
<link rel="icon" href="${logoDataUri}">
<style>${css}${cssWeb}</style>
</head>
<body>
<button type="button" class="menu-btn">☰ Indice</button>
<div class="layout">
  <aside class="barra">
    <a class="barra-testa" href="#"><img src="${logoDataUri}" alt=""><span>EVONETRTK</span></a>
    <p class="edizione">Firmware <b>${versione}</b> · edizione ${dataEdizione}</p>
    <input type="search" id="cerca" placeholder="Cerca nel manuale…" aria-label="Cerca nel manuale">
    <div id="esiti" hidden></div>
    <a class="scarica-pdf" href="manuale.pdf" download>⬇ Scarica il PDF da stampare</a>
    <nav class="indice">${htmlIndice()}</nav>
  </aside>
  <main class="contenuto">
    ${frontespizio}
    ${prefazione}
    ${tuttoIlCorpo}
  </main>
</div>
<script>${scriptWeb}</script>
</body>
</html>`;

// ---------- versione stampa ----------

const cssStampa = `
h2 .num { color: var(--brand); margin-right: .25em; }
.indice .voce-futura { display: flex; gap: .5em; color: #9aa29c; }
.indice .voce-futura .n { min-width: 2.2em; font-weight: 700; }
@media print { .indice .da-scrivere { display: inline; } }
@page { @bottom-right { content: "Firmware ${versione} · ${dataEdizione}"; font-size: 7.5pt; color: #5d645f; font-family: "Segoe UI", Arial, sans-serif; } }
@page :left { @bottom-right { content: none; } @bottom-left { content: "Firmware ${versione} · ${dataEdizione}"; font-size: 7.5pt; color: #5d645f; font-family: "Segoe UI", Arial, sans-serif; } }
@page copertina { @bottom-right { content: none; } @bottom-left { content: none; } }
@page :blank { @bottom-right { content: none; } @bottom-left { content: none; } }`;

// Schermate molto alte (piu' di una pagina A4): nel PDF si tagliano in pezzi
// affiancati in colonne. Nel sorgente: <figure class="alta" data-tagli="2332">
// con i punti di taglio in pixel dell'immagine originale, scelti in uno spazio
// vuoto tra due riquadri. La versione web mostra l'immagine intera.
const schermateAlteInColonne = (html) => html.replace(
  /<figure class="alta" data-tagli="([\d ,]+)">\s*<img class="schermata" src="(data:image\/png;base64,([^"]+))"([^>]*)>/g,
  (_, tagli, src, b64, resto) => {
    const testa = Buffer.from(b64.slice(0, 32), 'base64');
    const w = testa.readUInt32BE(16), h = testa.readUInt32BE(20);
    const punti = [0, ...tagli.split(/[ ,]+/).map(Number), h];
    const n = punti.length - 1;
    const L = Math.min(68, (160 - 8 * (n - 1)) / n);   // larghezza di ogni pezzo, mm
    const mm = (px) => (px / w * L).toFixed(2);
    let pezzi = '';
    for (let i = 0; i < n; i++) {
      pezzi += `<div class="pezzo" style="width:${L}mm;height:${mm(punti[i + 1] - punti[i])}mm">` +
        `<img src="${src}" style="width:${L}mm;margin-top:-${mm(punti[i])}mm"${i ? ' alt=""' : resto}></div>`;
    }
    return `<figure class="alta"><div class="pezzi">${pezzi}</div>`;
  });

// Punteggiatura attaccata ai tasti: "( [Tasto] )." non deve andare a capo
// lasciando la parentesi o il punto da soli all'inizio della riga dopo.
const tastiConPunteggiatura = (html) => html.replace(
  /(\(?)(<span class="tasto[^"]*">[^<]*<\/span>)([.,;:!?)»]+)?/g,
  (m, prima, tasto, dopo) => (prima || dopo) ? `<span class="tasto-unito">${prima}${tasto}${dopo || ''}</span>` : m);

// Tabelle spezzate tra due pagine (class="divisibile"), gestore di Paged.js:
// - prima dell'impaginazione ogni tabella divisibile si misura intera alla
//   larghezza del testo (168 mm) e riceve un <colgroup> fisso: le due parti
//   hanno cosi' le stesse colonne;
// - la riga d'intestazione (tutta <th>) si ripete in cima alla pagina dopo;
// - un'intestazione rimasta sola in fondo alla pagina passa alla pagina dopo.
// Le righe copiate non hanno data-ref: Paged.js non le usa per i punti di taglio.
const gestoreTabelle = `
class TabelleSpezzate extends Paged.Handler {
  beforeParsed(content) {
    const prova = document.createElement('div');
    prova.style.cssText = 'position:absolute;left:-10000px;top:0;visibility:hidden;width:168mm';
    document.body.appendChild(prova);
    for (const t of content.querySelectorAll('table.divisibile')) {
      if (t.querySelector('colgroup')) continue;
      const testa = t.querySelector('tr');
      if (!testa || [...testa.children].some((c) => c.colSpan > 1)) continue;
      prova.innerHTML = '';
      const copia = t.cloneNode(true);
      prova.appendChild(copia);
      const larghezze = [...copia.querySelector('tr').children].map((c) => c.getBoundingClientRect().width);
      const tot = larghezze.reduce((s, x) => s + x, 0);
      if (!tot) continue;
      const cg = document.createElement('colgroup');
      for (const x of larghezze) { const col = document.createElement('col'); col.style.width = (x / tot * 100).toFixed(2) + '%'; cg.appendChild(col); }
      t.insertBefore(cg, t.firstChild);
      t.style.tableLayout = 'fixed';
    }
    prova.remove();
  }
  renderNode(clone, node) {
    // il clone e' ancora vuoto (Paged.js copia le celle dopo): si guarda la riga sorgente
    if (!clone || clone.nodeName !== 'TR' || !node.querySelector('td')) return;
    const tabella = clone.closest('table');
    if (!tabella || !tabella.hasAttribute('data-split-from') || tabella.dataset.intestata) return;
    tabella.dataset.intestata = '1';
    const sorgente = node.closest('table');
    if (!sorgente) return;
    const senzaRef = (el) => { el.removeAttribute('data-ref'); el.querySelectorAll('[data-ref]').forEach((e) => e.removeAttribute('data-ref')); return el; };
    const cg = sorgente.querySelector('colgroup');
    if (cg && !tabella.querySelector('colgroup')) tabella.insertBefore(senzaRef(cg.cloneNode(true)), tabella.firstChild);
    if (tabella.querySelector('tr') !== clone) return;   // intestazione gia' portata qui
    const testa = sorgente.querySelector('tr');
    if (!testa || testa.querySelector('td') || testa === node) return;
    const copia = senzaRef(testa.cloneNode(true));
    copia.classList.add('intestazione-ripetuta');
    clone.parentNode.insertBefore(copia, clone);
  }
  afterPageLayout(pagina, page, breakToken, chunker) {
    // data-split-to qui non c'e' ancora: si parte dalla riga dove riprende la pagina dopo
    const n = breakToken && breakToken.node;
    if (!n || n.nodeType !== 1) return;
    const riga = n.closest('tr');
    const tabSrc = riga && riga.closest('table');
    if (!tabSrc) return;
    const t = pagina.querySelector('table[data-ref="' + tabSrc.dataset.ref + '"]');
    if (!t || t.hasAttribute('data-split-from')) return;
    // in fondo alla pagina restano solo l'intestazione o una sola riga di dati
    // di una tabella piu' lunga: la tabella intera passa alla pagina dopo
    // (non se la tabella parte gia' in cima alla pagina: non ci starebbe comunque)
    const righeDati = [...t.querySelectorAll('tr')].filter((r) => r.querySelector('td')).length;
    const righeSrc = [...tabSrc.querySelectorAll('tr')].filter((r) => r.querySelector('td')).length;
    if (righeDati > 1 || (righeDati === 1 && righeSrc < 4)) return;
    const area = pagina.querySelector('.pagedjs_page_content');
    if (area && t.getBoundingClientRect().top - area.getBoundingClientRect().top < area.getBoundingClientRect().height * 0.3) return;
    const testa = tabSrc.querySelector('tr');
    if (!testa) return;
    breakToken.node = testa;
    breakToken.offset = 0;
    t.style.display = 'none';
  }
}
// in testa alla lista: devono agire prima dei gestori interni di Paged.js
// (target-counter dell'indice), che altrimenti vedrebbero il titolo tolto
Paged.registeredHandlers.unshift(TabelleSpezzate);

// Titoli rimasti soli in fondo alla pagina: Paged.js non sempre rispetta
// "break-after: avoid". Se l'ultimo contenuto della pagina e' un titolo
// (h2-h4) non in cima alla pagina, la pagina dopo riparte dal titolo e questo si
// toglie dalla pagina (anche l'id, per l'indice).
class TitoliOrfani extends Paged.Handler {
  afterPageLayout(pagina, page, breakToken, chunker) {
    if (!breakToken || !breakToken.node) return;
    const area = pagina.querySelector('.pagedjs_page_content');
    if (!area) return;
    const titoli = area.querySelectorAll('h2, h3, h4');
    const h = titoli[titoli.length - 1];
    if (!h || !h.dataset.ref) return;
    const pieno = (e) => e.style.display !== 'none' && (e.textContent.trim() !== '' || e.querySelector('img, svg, table, hr'));
    for (let e = h; e && e !== area; e = e.parentElement) {
      for (let s = e.nextElementSibling; s; s = s.nextElementSibling) if (pieno(s)) return;
    }
    const a = area.getBoundingClientRect(), r = h.getBoundingClientRect();
    if (r.top - a.top < a.height * 0.15) return;   // titolo in cima: spostarlo non servirebbe
    const sorgente = chunker.source.querySelector('[data-ref="' + h.dataset.ref + '"]');
    if (!sorgente) return;
    breakToken.node = sorgente;
    breakToken.offset = 0;
    h.removeAttribute('id');
    h.style.display = 'none';
  }
}
Paged.registeredHandlers.splice(1, 0, TitoliOrfani);`;

const stampa = `<!doctype html>
<html lang="it">
<head>
<meta charset="utf-8">
<title>EVONETRTK · ${escapeHtml(struttura.titolo)} (firmware ${versione})</title>
<style>${css}${cssStampa}</style>
<script>window.PagedConfig = { auto: true, after: function () { window.__impaginato = true; } };</script>
<script src="../vendor/paged.polyfill.min.js"></script>
<script>${gestoreTabelle}</script>
</head>
<body>
${frontespizio}
${retro}
${prefazione}
${paginaIndice}
${tastiConPunteggiatura(schermateAlteInColonne(tuttoIlCorpo))}
</body>
</html>`;

fs.mkdirSync(OUT, { recursive: true });
fs.writeFileSync(path.join(OUT, 'manuale.html'), web);
fs.writeFileSync(path.join(OUT, 'manuale-stampa.html'), stampa);
console.log(`manuale.html scritto (firmware ${versione}, ${corpo.length} capitoli scritti, ${nTasti} tasti del pannello)`);
if (daCompletare) console.log(`ATTENZIONE: ${daCompletare} spazi [[da completare]] con i dati dell'azienda`);

if (process.argv.includes('--solo-html')) process.exit(0);

// ---------- PDF con Chrome/Edge via DevTools Protocol ----------

function trovaBrowser() {
  const candidati = [
    process.env.CHROME_PATH,
    'C:/Program Files/Google/Chrome/Application/chrome.exe',
    'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
    '/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser',
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  ].filter(Boolean);
  const b = candidati.find((p) => fs.existsSync(p));
  if (!b) throw new Error('Chrome o Edge non trovato: impostare CHROME_PATH');
  return b;
}

const attesa = (ms) => new Promise((r) => setTimeout(r, ms));

async function generaPdf() {
  const porta = 9200 + Math.floor(Math.random() * 600);
  const profilo = fs.mkdtempSync(path.join(os.tmpdir(), 'manuale-chrome-'));
  const browser = spawn(trovaBrowser(), [
    '--headless=new', `--remote-debugging-port=${porta}`, `--user-data-dir=${profilo}`,
    '--allow-file-access-from-files', '--no-first-run', '--no-default-browser-check', 'about:blank',
  ], { stdio: 'ignore' });

  try {
    let pagine;
    for (let i = 0; i < 50 && !pagine; i++) {
      try { pagine = await (await fetch(`http://127.0.0.1:${porta}/json/list`)).json(); } catch { await attesa(200); }
    }
    const pagina = pagine.find((p) => p.type === 'page');
    const ws = new WebSocket(pagina.webSocketDebuggerUrl);
    await new Promise((ok, ko) => { ws.onopen = ok; ws.onerror = ko; });
    let prossimo = 0;
    const inAttesa = new Map();
    ws.onmessage = (ev) => {
      const m = JSON.parse(ev.data);
      if (m.id && inAttesa.has(m.id)) {
        const { ok, ko } = inAttesa.get(m.id);
        inAttesa.delete(m.id);
        m.error ? ko(new Error(m.error.message)) : ok(m.result);
      }
    };
    // Ogni comando ha un tempo massimo: se Paged.js entra in un ciclo infinito
    // la pagina non risponde piu' e senza limite lo script resterebbe appeso.
    const invia = (method, params = {}, ms = 30000) => new Promise((ok, ko) => {
      const id = ++prossimo;
      const t = setTimeout(() => { inAttesa.delete(id); ko(new Error(`${method}: nessuna risposta in ${ms / 1000} s (impaginazione bloccata?)`)); }, ms);
      inAttesa.set(id, { ok: (r) => { clearTimeout(t); ok(r); }, ko: (e) => { clearTimeout(t); ko(e); } });
      ws.send(JSON.stringify({ id, method, params }));
    });

    await invia('Page.enable');
    const url = 'file:///' + path.join(OUT, 'manuale-stampa.html').replace(/\\/g, '/');
    await invia('Page.navigate', { url });
    for (let i = 0; ; i++) {
      const r = await invia('Runtime.evaluate', { expression: 'window.__impaginato === true', returnByValue: true });
      if (r.result.value) break;
      if (i > 300) throw new Error('impaginazione non completata in 60 s');
      await attesa(200);
    }
    const nPagine = (await invia('Runtime.evaluate', { expression: 'document.querySelectorAll(".pagedjs_page").length', returnByValue: true })).result.value;
    const pdf = await invia('Page.printToPDF', {
      printBackground: true, preferCSSPageSize: true, displayHeaderFooter: false,
      generateDocumentOutline: true, generateTaggedPDF: false,
    }, 400000);
    fs.writeFileSync(path.join(OUT, 'manuale.pdf'), Buffer.from(pdf.data, 'base64'));
    console.log(`manuale.pdf scritto (${nPagine} pagine)`);
    ws.close();
  } finally {
    browser.kill();
    await attesa(500);
    try { fs.rmSync(profilo, { recursive: true, force: true }); } catch { /* Chrome a volte tiene i file ancora un attimo */ }
  }
}

generaPdf().catch((e) => { console.error('PDF non generato:', e.message); process.exit(1); });
