// Server finto delle licenze EVONETRTK, per provare il firmware sul PC.
// Segue docs/attivazione-licenze.md (baseesp32-ethelite). Chiave di PROVA, mai in produzione.
//   node server.js [porta]
// Prove: POST /admin/set {"serial":"..","features":{..},"revoked":false}  (modifica il ricevitore)
//        POST /admin/code {"code":".."}                                   (nuovo codice)
//        GET  /admin/db
const http = require('http');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const PORT = Number(process.argv[2] || 8095);
const DIR = __dirname;
// Chiave privata di PROVA fuori dal repository (chi la possiede crea licenze accettate dal firmware con kid "test").
const KEYDIR = process.env.LIC_KEYDIR || 'C:/Users/Utente/Desktop/chiavi_prova_licenze';
const PRIV = crypto.createPrivateKey(fs.readFileSync(path.join(KEYDIR, 'test_priv.pem')));
const DBF = path.join(KEYDIR, 'db.json');
let db = fs.existsSync(DBF) ? JSON.parse(fs.readFileSync(DBF, 'utf8'))
  : { codes: { 'EVO-TEST-0001': {}, 'EVO-TEST-0002': {} }, devices: {}, accepts: [] };
const save = () => fs.writeFileSync(DBF, JSON.stringify(db, null, 2));
const now = () => Math.floor(Date.now() / 1000);

function sign(obj) {
  const bytes = Buffer.from(JSON.stringify(obj));
  const sig = crypto.sign('sha256', bytes, PRIV); // DER
  return { license: bytes.toString('base64'), sig: sig.toString('base64'), kid: 'test' };
}
function verifyReq(body, pubB64) {
  const bytes = Buffer.from(body.req || '', 'base64');
  const key = crypto.createPublicKey({ key: Buffer.from(pubB64, 'base64'), format: 'der', type: 'spki' });
  if (!crypto.verify('sha256', bytes, key, Buffer.from(body.sig || '', 'base64'))) return null;
  return JSON.parse(bytes.toString('utf8'));
}
function license(d) {
  let issued = now();
  if (d.issued && issued <= d.issued) issued = d.issued + 1;
  d.issued = issued;
  return sign({ v: 1, serial: d.serial, chip: d.chip, device_pub: d.device_pub, customer: d.customer,
    issued, renew_after: issued + (d.renew_s || 7 * 86400), features: d.features, revoked: !!d.revoked });
}
function send(res, code, obj) {
  res.writeHead(code, { 'Content-Type': 'application/json' });
  res.end(obj === undefined ? '' : JSON.stringify(obj));
}
const err = (res, code, e, m) => send(res, code, { error: e, message: m });

http.createServer((req, res) => {
  let body = '';
  req.on('data', (c) => { body += c; });
  req.on('end', () => {
    let j = {};
    try { j = body ? JSON.parse(body) : {}; } catch { return err(res, 400, 'bad_json', 'Richiesta non valida'); }
    const url = req.url.replace(/\/+$/, '');
    console.log(new Date().toISOString(), req.method, url, body.length, 'byte');
    if (url === '/admin/db') return send(res, 200, db);
    if (url === '/admin/set') {
      const d = db.devices[j.serial];
      if (!d) return err(res, 404, 'serial_unknown', 'ricevitore sconosciuto');
      if (j.features) d.features = j.features;
      if (j.revoked !== undefined) d.revoked = j.revoked;
      if (j.renew_s) d.renew_s = j.renew_s;
      d.changed = true; save(); return send(res, 200, d);
    }
    if (url === '/admin/code') { db.codes[j.code] = {}; save(); return send(res, 200, db.codes); }

    if (url === '/api/device/v1/activate') {
      let r;
      try {
        const inner = JSON.parse(Buffer.from(j.req || '', 'base64').toString('utf8'));
        r = verifyReq(j, inner.device_pub);
      } catch (e) { return err(res, 400, 'bad_request', 'Richiesta non valida: ' + e.message); }
      if (!r) return err(res, 401, 'bad_signature', 'Firma del ricevitore non valida');
      const c = db.codes[r.code];
      if (!c) return err(res, 404, 'code_invalid', 'Codice di attivazione non valido');
      // Stesso ricevitore (stesso chip) puo' riusare il suo codice, es. dopo un ripristino di fabbrica.
      if (c.used_by && c.used_by !== r.chip) return err(res, 409, 'code_used', 'Codice gia\' usato su un altro ricevitore');
      if (!r.accept || !r.accept.terms || !r.accept.clauses_1341) return err(res, 400, 'terms_missing', 'Accettazione delle condizioni mancante');
      c.used_by = r.chip;
      const old = db.devices[r.serial] || {};
      const d = db.devices[r.serial] = { ...old, serial: r.serial, chip: r.chip, device_pub: r.device_pub,
        customer: old.customer || 'C-PROVA', features: old.features || { rtk: 0 }, revoked: false, fw: r.fw, fw_sha256: r.fw_sha256 };
      db.accepts.push({ at: now(), ip: req.socket.remoteAddress, serial: r.serial, chip: r.chip, ...r.accept });
      const out = license(d); save();
      return send(res, 200, out);
    }
    if (url === '/api/device/v1/renew') {
      let inner;
      try { inner = JSON.parse(Buffer.from(j.req || '', 'base64').toString('utf8')); } catch { return err(res, 400, 'bad_request', 'Richiesta non valida'); }
      const d = db.devices[inner.serial];
      if (!d) return err(res, 404, 'serial_unknown', 'Ricevitore non attivato');
      const r = verifyReq(j, d.device_pub);
      if (!r || r.chip !== d.chip) return err(res, 401, 'bad_signature', 'Firma del ricevitore non valida');
      d.last_seen = now(); d.fw = r.fw; d.fw_sha256 = r.fw_sha256; d.in_use = r.in_use;
      if (!d.changed && r.issued === d.issued && now() < d.issued + (d.renew_s || 7 * 86400)) { save(); res.writeHead(304); return res.end(); }
      d.changed = false;
      const out = license(d); save();
      return send(res, 200, out);
    }
    err(res, 404, 'not_found', 'Indirizzo sconosciuto');
  });
}).listen(PORT, () => console.log('Server licenze di PROVA sulla porta', PORT));
