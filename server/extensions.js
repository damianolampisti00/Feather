'use strict';
// Browser extensions for the server's Chromium.
//
// Every folder in server/extensions/ (or EXTENSIONS_DIR) that holds a
// manifest.json is an unpacked extension, loaded into every session.
// Chromium only runs extensions in a persistent profile, so while any are
// installed each session gets its own browser with a profile of its own
// (sessions/<id>.profile/), instead of a context of the one shared browser.
//
// Installing from the Chrome Web Store (run on the server, then restart it):
//   node extensions.js add <extension id or Web Store link>
//   node extensions.js list
//   node extensions.js update            (every store extension)
//   node extensions.js remove <id>
//
// Only Manifest V3 extensions run: Chromium 139+ no longer loads V2 ones
// (e.g. uBlock Origin; uBlock Origin Lite is its V3 version).
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const crypto = require('crypto');

const EXT_DIR = process.env.EXTENSIONS_DIR || path.join(__dirname, 'extensions');
// The Web Store only serves extensions that support the asking browser's version.
const PROD_VERSION = process.env.EXTENSIONS_CHROME_VERSION || '150.0.0.0';

function readManifest(dir) {
  try {
    return JSON.parse(fs.readFileSync(path.join(dir, 'manifest.json'), 'utf8').replace(/^﻿/, ''));
  } catch (_) {
    return null;
  }
}

// Chromium's id for an extension key: the first 128 bits of the key's SHA-256,
// written with the letters a-p instead of hex digits.
function idFromKey(der) {
  return crypto.createHash('sha256').update(der).digest('hex').slice(0, 32)
    .replace(/./g, (c) => String.fromCharCode(97 + parseInt(c, 16)));
}

// The installed extensions: [{ dir, id, name, version, manifestVersion, optionsUrl }].
// `id` is known only for extensions whose manifest carries a key (all those
// installed with `add`); Chromium makes one up from the folder path for the rest.
function listExtensions() {
  let names = [];
  try { names = fs.readdirSync(EXT_DIR); } catch (_) { return []; }
  const out = [];
  for (const name of names.sort()) {
    const dir = path.join(EXT_DIR, name);
    const m = fs.statSync(dir).isDirectory() && readManifest(dir);
    if (!m) continue;
    const id = m.key ? idFromKey(Buffer.from(m.key, 'base64')) : null;
    const options = (m.options_ui && m.options_ui.page) || m.options_page || null;
    out.push({
      dir, id, name: localizedName(dir, m), version: m.version, manifestVersion: m.manifest_version,
      optionsUrl: id && options ? `chrome-extension://${id}/${options.replace(/^\//, '')}` : null,
    });
  }
  return out;
}

// "__MSG_appName__" -> the name in the extension's default locale.
function localizedName(dir, m) {
  const k = /^__MSG_(.+)__$/.exec(m.name || '');
  if (!k) return m.name || path.basename(dir);
  try {
    const msgs = JSON.parse(fs.readFileSync(path.join(dir, '_locales', m.default_locale || 'en', 'messages.json'), 'utf8').replace(/^﻿/, ''));
    const hit = Object.keys(msgs).find((x) => x.toLowerCase() === k[1].toLowerCase());
    if (hit && msgs[hit].message) return msgs[hit].message;
  } catch (_) {}
  return path.basename(dir);
}

// Chromium switches that load the extensions in `list`.
function launchArgs(list) {
  if (!list.length) return [];
  const dirs = list.map((e) => e.dir).join(',');
  return [`--disable-extensions-except=${dirs}`, `--load-extension=${dirs}`];
}

// ---- Web Store install -----------------------------------------------------

function parseId(arg) {
  const m = /(?:^|\/)([a-p]{32})(?:[/?#]|$)/.exec(String(arg || '').trim());
  return m ? m[1] : null;
}

// A minimal protobuf reader: the fields of one message as [{ field, value }]
// (length-delimited values as Buffers, varints as numbers).
function protoFields(buf) {
  const out = [];
  let i = 0;
  const varint = () => {
    let v = 0, mul = 1, b;
    do { b = buf[i++]; v += (b & 0x7f) * mul; mul *= 128; } while (b & 0x80 && i < buf.length);
    return v;
  };
  while (i < buf.length) {
    const tag = varint(), field = Math.floor(tag / 8), type = tag & 7;
    if (type === 0) out.push({ field, value: varint() });
    else if (type === 2) { const n = varint(); out.push({ field, value: buf.subarray(i, i + n) }); i += n; }
    else if (type === 1) i += 8;
    else if (type === 5) i += 4;
    else throw new Error('bad CRX header');
  }
  return out;
}

// A CRX3 file -> { zip, key }: the zip archive, and the public key whose id is `id`.
function openCrx(buf, id) {
  if (buf.toString('latin1', 0, 4) !== 'Cr24') throw new Error('not a CRX file');
  const version = buf.readUInt32LE(4);
  if (version !== 3) throw new Error(`unsupported CRX version ${version}`);
  const headerLen = buf.readUInt32LE(8);
  const header = buf.subarray(12, 12 + headerLen);
  let key = null;
  for (const f of protoFields(header)) {
    // CrxFileHeader: 2 = sha256_with_rsa, 3 = sha256_with_ecdsa proofs; each proof's field 1 is a public key
    if (f.field !== 2 && f.field !== 3) continue;
    const pub = protoFields(f.value).find((p) => p.field === 1);
    if (pub && idFromKey(pub.value) === id) key = pub.value;
  }
  return { zip: buf.subarray(12 + headerLen), key };
}

// Extracts a zip archive (stored or deflated entries) into `dest`.
function unzip(zip, dest) {
  let eocd = -1;
  for (let i = zip.length - 22; i >= Math.max(0, zip.length - 65557); i--) {
    if (zip.readUInt32LE(i) === 0x06054b50) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error('broken zip archive');
  const count = zip.readUInt16LE(eocd + 10);
  let p = zip.readUInt32LE(eocd + 16);
  const root = path.resolve(dest);
  for (let n = 0; n < count; n++) {
    if (zip.readUInt32LE(p) !== 0x02014b50) throw new Error('broken zip directory');
    const method = zip.readUInt16LE(p + 10);
    const size = zip.readUInt32LE(p + 20);
    const nameLen = zip.readUInt16LE(p + 28), extraLen = zip.readUInt16LE(p + 30), commentLen = zip.readUInt16LE(p + 32);
    const local = zip.readUInt32LE(p + 42);
    const name = zip.toString('utf8', p + 46, p + 46 + nameLen);
    p += 46 + nameLen + extraLen + commentLen;
    const target = path.resolve(root, name);
    if (target !== root && !target.startsWith(root + path.sep)) throw new Error(`unsafe path in archive: ${name}`);
    if (name.endsWith('/')) { fs.mkdirSync(target, { recursive: true }); continue; }
    const start = local + 30 + zip.readUInt16LE(local + 26) + zip.readUInt16LE(local + 28);
    const data = zip.subarray(start, start + size);
    fs.mkdirSync(path.dirname(target), { recursive: true });
    if (method === 0) fs.writeFileSync(target, data);
    else if (method === 8) fs.writeFileSync(target, zlib.inflateRawSync(data));
    else throw new Error(`unsupported zip method ${method} (${name})`);
  }
}

async function install(arg) {
  const id = parseId(arg);
  if (!id) throw new Error(`not an extension id or Web Store link: ${arg}`);
  const url = 'https://clients2.google.com/service/update2/crx?response=redirect' +
    `&prodversion=${PROD_VERSION}&acceptformat=crx2,crx3&x=id%3D${id}%26uc`;
  const res = await fetch(url);
  if (!res.ok) throw new Error(`Web Store download failed: HTTP ${res.status}`);
  const buf = Buffer.from(await res.arrayBuffer());
  if (buf.length === 0) throw new Error(`no such extension on the Web Store (or not for Chrome ${PROD_VERSION}): ${id}`);
  const { zip, key } = openCrx(buf, id);

  fs.mkdirSync(EXT_DIR, { recursive: true });
  const dest = path.join(EXT_DIR, id);
  const tmp = `${dest}.tmp-${process.pid}`;
  fs.rmSync(tmp, { recursive: true, force: true });
  try {
    unzip(zip, tmp);
    // Chromium refuses unpacked extensions with names starting with "_"
    // other than _locales; _metadata is the Web Store's signature folder.
    fs.rmSync(path.join(tmp, '_metadata'), { recursive: true, force: true });
    const m = readManifest(tmp);
    if (!m) throw new Error('the extension has no readable manifest.json');
    // Its own key keeps the Web Store id (and so its settings) when loaded unpacked.
    if (key) m.key = key.toString('base64');
    fs.writeFileSync(path.join(tmp, 'manifest.json'), JSON.stringify(m, null, 2));
    fs.rmSync(dest, { recursive: true, force: true });
    fs.renameSync(tmp, dest);
  } catch (e) {
    fs.rmSync(tmp, { recursive: true, force: true });
    throw e;
  }
  return listExtensions().find((e) => e.dir === dest);
}

function describe(e) {
  const warn = e.manifestVersion < 3 ? '  (Manifest V2: current Chromium will not run it)' : '';
  return `${e.id || path.basename(e.dir)}  ${e.name} ${e.version}${warn}`;
}

module.exports = { EXT_DIR, listExtensions, launchArgs };

if (require.main === module) {
  const [cmd, ...rest] = process.argv.slice(2);
  (async () => {
    if (cmd === 'add' && rest.length) {
      for (const a of rest) console.log('installed: ' + describe(await install(a)));
      console.log('Restart the server to load it.');
    } else if (cmd === 'update') {
      const ids = listExtensions().map((e) => e.id).filter(Boolean);
      for (const id of ids) console.log('updated: ' + describe(await install(id)));
      if (ids.length) console.log('Restart the server to load the new versions.');
    } else if (cmd === 'remove' && rest.length) {
      for (const a of rest) {
        const e = listExtensions().find((x) => x.id === parseId(a) || path.basename(x.dir) === a);
        if (!e) throw new Error(`not installed: ${a}`);
        fs.rmSync(e.dir, { recursive: true, force: true });
        console.log('removed: ' + describe(e));
      }
      console.log('Restart the server to unload it.');
    } else if (cmd === 'list' || !cmd) {
      const list = listExtensions();
      if (!list.length) console.log(`No extensions in ${EXT_DIR}`);
      for (const e of list) console.log(describe(e));
    } else {
      console.log('usage: node extensions.js add <id|Web Store link>... | update | remove <id>... | list');
      process.exitCode = 1;
    }
  })().catch((e) => {
    console.error(String(e.message || e));
    process.exitCode = 1;
  });
}
