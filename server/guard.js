'use strict';
// Access control for the render server: one shared secret token, checked on
// every /api request, with a limit on wrong guesses.
//
// The token comes from FEATHER_TOKEN, else from token.txt next to this file; if
// neither exists a random one is generated, saved (mode 600) and shown once.
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

// No 0/O/1/l/i: it is typed on a phone keyboard.
const ALPHABET = 'abcdefghjkmnpqrstuvwxyz23456789'; // 31 symbols
const TOKEN_CHARS = 24; // ~119 bits

// Same comparison on both sides: case and separators (the dashes shown in the
// grouped form) do not matter.
const normalize = (s) => String(s == null ? '' : s).toLowerCase().replace(/[^a-z0-9]/g, '');
const sha256 = (s) => crypto.createHash('sha256').update(s).digest();

function generate() {
  let out = '';
  while (out.length < TOKEN_CHARS) {
    for (const b of crypto.randomBytes(32)) {
      if (b < 248 && out.length < TOKEN_CHARS) out += ALPHABET[b % 31]; // 248 = 31*8: no modulo bias
    }
  }
  return out.match(/.{4}/g).join('-');
}

// -> { token, created }
function loadToken(dir) {
  if (process.env.FEATHER_TOKEN) return { token: process.env.FEATHER_TOKEN, created: false };
  const file = path.join(dir, 'token.txt');
  if (fs.existsSync(file)) {
    const t = fs.readFileSync(file, 'utf8').trim();
    if (normalize(t).length >= 16) return { token: t, created: false };
  }
  const token = generate();
  fs.writeFileSync(file, token + '\n', { mode: 0o600 });
  return { token, created: true };
}

const LOOPBACK = ['::1', '127.0.0.1', '::ffff:127.0.0.1'];

// Behind a Cloudflare tunnel every request comes from cloudflared on this
// machine; the real visitor is in CF-Connecting-IP, which is only believed when
// the connection itself is local.
function clientIp(req) {
  const remote = req.socket.remoteAddress || '';
  const cf = req.headers['cf-connecting-ip'];
  return LOOPBACK.includes(remote) && cf ? String(cf) : remote;
}

const MAX_FAILURES = 10;
const WINDOW_MS = 15 * 60 * 1000;

// One shared guard per server (token + brute-force lockout state), usable
// from both the Express `/api` middleware and the raw `upgrade` handler for
// the WebSocket endpoint (which never goes through Express) -- a single
// `failures` map means a brute-force run against either transport counts
// against the same lockout instead of each having its own separate budget.
function makeGuard(token) {
  const expected = sha256(normalize(token));
  const failures = new Map(); // ip -> { count, since, blockedUntil }

  setInterval(() => {
    const now = Date.now();
    for (const [ip, f] of failures) if (f.blockedUntil < now && now - f.since > WINDOW_MS) failures.delete(ip);
  }, 60 * 1000).unref();

  // -> { ok: true } | { ok: false, retryAfterSec? }
  function check(ip, given) {
    const now = Date.now();
    const f = failures.get(ip);
    if (f && f.blockedUntil > now) {
      return { ok: false, retryAfterSec: Math.ceil((f.blockedUntil - now) / 1000) };
    }
    const norm = normalize(given || '');
    // Compare fixed-size digests in constant time.
    if (norm && crypto.timingSafeEqual(sha256(norm), expected)) {
      failures.delete(ip);
      return { ok: true };
    }
    const cur = f && now - f.since <= WINDOW_MS ? f : { count: 0, since: now, blockedUntil: 0 };
    cur.count += 1;
    if (cur.count >= MAX_FAILURES) cur.blockedUntil = now + WINDOW_MS;
    failures.set(ip, cur);
    return { ok: false };
  }

  // Express middleware: `Authorization: Bearer <token>` required.
  function requireToken(req, res, next) {
    const ip = clientIp(req);
    const m = /^Bearer\s+(.+)$/i.exec(req.headers.authorization || '');
    const r = check(ip, m ? m[1] : '');
    if (r.ok) return next();
    if (r.retryAfterSec) {
      res.set('Retry-After', String(r.retryAfterSec));
      return res.status(429).json({ error: 'Too many attempts, try again later' });
    }
    return res.status(401).json({ error: 'Missing or invalid token' });
  }

  return { check, requireToken };
}

module.exports = { loadToken, makeGuard, clientIp, normalize };
