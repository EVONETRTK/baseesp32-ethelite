// Generatore delle ruote da trattore per i disegni del manuale e del logo:
// gomma alta liscia (senza tasselli) vista di tre quarti,
// fianco con le linee di gomma, cerchio scuro con bordo, disco forato,
// mozzo e bulloni. Sostituisce nei file SVG la parte tra i commenti
// <!--RUOTA:nome:cx:cy:raggio:tasselli--> e <!--/RUOTA:nome-->.
// Uso: node strumenti/ruote.js img/misure-trattore.svg img/logo.svg
'use strict';
const fs = require('fs');

const f = (n) => Math.round(n * 10) / 10;

function ruota(cx, cy, R, n) {
  const p = [];
  const step = 2 * Math.PI / n;
  const pt = (an, rr, sx = 0) => `${f(cx + sx + rr * Math.cos(an))},${f(cy + rr * Math.sin(an))}`;
  // 1) spessore della gomma visto di tre quarti (a destra), liscio
  const d = R * 0.3;
  p.push(`<circle cx="${f(cx + d)}" cy="${cy}" r="${f(R)}" fill="#111111"/>`);
  // corpo della gomma
  p.push(`<circle cx="${cx}" cy="${cy}" r="${f(R)}" fill="#202020"/>`);
  // 2) fianco della gomma
  p.push(`<circle cx="${cx}" cy="${cy}" r="${f(R * 0.82)}" fill="#222222" stroke="#151515" stroke-width="${f(R * 0.02)}"/>`);
  p.push(`<circle cx="${cx}" cy="${cy}" r="${f(R * 0.72)}" fill="none" stroke="#2b2b2b" stroke-width="${f(R * 0.02)}"/>`);
  // riflesso sul fianco in alto a sinistra
  p.push(`<path d="M${f(cx - R * 0.74)} ${f(cy - R * 0.2)} A${f(R * 0.77)} ${f(R * 0.77)} 0 0 1 ${f(cx - R * 0.2)} ${f(cy - R * 0.74)}" fill="none" stroke="#3a3a3a" stroke-width="${f(R * 0.05)}" stroke-linecap="round"/>`);
  // cerchio: bordo, disco, fori, mozzo, bulloni
  const rc = R * 0.58;
  p.push(`<circle cx="${cx}" cy="${cy}" r="${f(rc)}" fill="#5c646c" stroke="#8b949c" stroke-width="${f(R * 0.04)}"/>`);
  p.push(`<circle cx="${cx}" cy="${cy}" r="${f(rc * 0.82)}" fill="#6c757e"/>`);
  const nf = 8;
  for (let i = 0; i < nf; i++) {
    const a = (i / nf) * 2 * Math.PI + Math.PI / nf;
    p.push(`<circle cx="${f(cx + rc * 0.58 * Math.cos(a))}" cy="${f(cy + rc * 0.58 * Math.sin(a))}" r="${f(rc * 0.11)}" fill="#3b4148"/>`);
  }
  p.push(`<circle cx="${cx}" cy="${cy}" r="${f(rc * 0.38)}" fill="#8b949c" stroke="#4a5058" stroke-width="${f(R * 0.02)}"/>`);
  for (let i = 0; i < nf; i++) {
    const a = (i / nf) * 2 * Math.PI;
    p.push(`<circle cx="${f(cx + rc * 0.27 * Math.cos(a))}" cy="${f(cy + rc * 0.27 * Math.sin(a))}" r="${f(rc * 0.04)}" fill="#3b4148"/>`);
  }
  p.push(`<circle cx="${cx}" cy="${cy}" r="${f(rc * 0.15)}" fill="#4a5058"/>`);
  return p.join('\n  ');
}

for (const file of process.argv.slice(2)) {
  let s = fs.readFileSync(file, 'utf8');
  let n = 0;
  s = s.replace(/<!--RUOTA:([a-z]+):([\d.]+):([\d.]+):([\d.]+):(\d+)-->[\s\S]*?<!--\/RUOTA:\1-->/g,
    (_, nome, cx, cy, r, t) => {
      n++;
      return `<!--RUOTA:${nome}:${cx}:${cy}:${r}:${t}-->\n  ${ruota(+cx, +cy, +r, +t)}\n  <!--/RUOTA:${nome}-->`;
    });
  fs.writeFileSync(file, s);
  console.log(`${file}: ${n} ruote`);
}
