'use strict';
// Videos embedded in ordinary web pages, played in BB10's native player the
// way YouTube/Instagram already are: a tap on a page's video player (videoAt)
// is not clicked; the client opens its player and asks for the stream
// (resolveWebVideo), which comes either straight from the site's CDN or
// through this server (the media relay below).
//
// Why a relay: such stream URLs are often bound to the IP that asked for them
// (some carry `ip=<this server>` in a signed query) and/or want the site's
// headers (some CDNs answer 410 without them), neither of which BB10's
// MediaPlayer can provide. At home the phone gets the relay straight over the
// home network (or the CDN itself when that works without headers); away, the
// relay goes through the Cloudflare tunnel. See resolveWeb in server.js.
const fs = require('fs');
const os = require('os');
const path = require('path');
const dns = require('dns');
const http = require('http');
const https = require('https');
const crypto = require('crypto');
const { execFile } = require('child_process');
const express = require('express');
const { isBlockedAddress } = require('./safeproxy');

const YTDLP_PYTHON = process.env.YTDLP_PYTHON || venvPython(path.join(__dirname, '.venv-ytdlp'));
const HELPER = path.join(__dirname, 'webvideo.py');

// A Python virtualenv's interpreter, on Windows or elsewhere.
function venvPython(dir) {
  return process.platform === 'win32' ? path.join(dir, 'Scripts', 'python.exe') : path.join(dir, 'bin', 'python');
}
// Web videos need yt-dlp in .venv-ytdlp (or YTDLP_PYTHON); without it a tap
// on a player falls back to the page itself (videoFromPage) or the page's own player.
const ytdlpAvailable = fs.existsSync(YTDLP_PYTHON);

// --- which tap is a tap on a video player ---------------------------------

// The video player under a tap (CSS px), or null. A <video> of player size
// (not a thumbnail's hover preview or a tiny ad) that really is what's at
// that point -- not a cookie banner or age gate drawn over it -- in the page
// or in an iframe one level down. A YouTube/Vimeo embed iframe counts too.
async function videoAt(page, x, y) {
  const main = page.mainFrame();
  const hit = await main.evaluate(([px, py]) => {
    // Player-sized: most of the width, or at least a classic 300+ px embed.
    const minW = Math.min(innerWidth * 0.6, 300);
    const top = document.elementFromPoint(px, py);
    if (!top) return null;
    const inside = (r) => px >= r.left && px <= r.right && py >= r.top && py <= r.bottom;
    // `el` is what's at the point if the topmost element is it, is inside it,
    // or shares a container about its size with it (player controls over the
    // video). A big container on top instead (the page's own content box, as
    // while a site's age or cookie gate disables the player) means the player itself
    // isn't what a click there would hit.
    const owns = (el) => {
      if (top === el || el.contains(top)) return true;
      const r = el.getBoundingClientRect();
      for (let a = top; a; a = a.parentElement) {
        if (a.contains(el)) {
          const ar = a.getBoundingClientRect();
          return ar.width * ar.height <= r.width * r.height * 1.6;
        }
      }
      return false;
    };
    for (const v of document.querySelectorAll('video')) {
      const r = v.getBoundingClientRect();
      // Inside a link: a thumbnail's preview, the tap should follow the link.
      if (v.closest('a[href]')) continue;
      if (r.width >= minW && r.height >= 120 && inside(r) && owns(v)) return { kind: 'video' };
    }
    const frames = document.querySelectorAll('iframe');
    for (let i = 0; i < frames.length; i++) {
      const r = frames[i].getBoundingClientRect();
      if (r.width >= minW && r.height >= 120 && inside(r) && top === frames[i]) {
        return { kind: 'iframe', index: i, src: frames[i].src, x: px - r.left, y: py - r.top };
      }
    }
    return null;
  }, [x, y]);
  if (!hit) return null;
  if (hit.kind === 'video') return { pageUrl: page.url() };
  if (/^https?:\/\/(www\.)?(youtube(-nocookie)?\.com\/embed\/|player\.vimeo\.com\/video\/)/i.test(hit.src)) {
    return { pageUrl: page.url(), frameUrl: hit.src };
  }
  for (const f of main.childFrames()) {
    const el = await f.frameElement().catch(() => null);
    if (!el) continue;
    const same = await main.evaluate(([e, i]) => e === document.querySelectorAll('iframe')[i], [el, hit.index]).catch(() => false);
    if (!same) continue;
    const player = await f.evaluate(([px, py]) => [...document.querySelectorAll('video')].some((v) => {
      const r = v.getBoundingClientRect();
      return r.width >= innerWidth * 0.6 && px >= r.left && px <= r.right && py >= r.top && py <= r.bottom;
    }), [hit.x, hit.y]).catch(() => false);
    return player ? { pageUrl: page.url(), frameUrl: f.url() } : null;
  }
  return null;
}

// --- finding the stream ----------------------------------------------------

function netscapeCookies(cookies) {
  const lines = ['# Netscape HTTP Cookie File'];
  for (const c of cookies) {
    const domain = c.domain;
    const expires = c.expires > 0 ? Math.floor(c.expires) : 0;
    lines.push([domain, domain.startsWith('.') ? 'TRUE' : 'FALSE', c.path || '/',
                c.secure ? 'TRUE' : 'FALSE', expires, c.name, c.value].join('\t'));
  }
  return lines.join('\n') + '\n';
}

function runHelper(url, referer, cookieFile) {
  return new Promise((resolve) => {
    // execFile, no shell: the URLs are plain argv entries.
    execFile(YTDLP_PYTHON, [HELPER, url, referer || '', cookieFile || ''],
      { timeout: 40000, maxBuffer: 1 << 20 }, (err, stdout) => {
        try {
          resolve(JSON.parse(String(stdout).trim().split('\n').pop()));
        } catch (_) {
          resolve({ ok: false, reason: err ? String(err.message).split('\n')[0] : 'bad helper output' });
        }
      });
  });
}

// Plan B when yt-dlp knows nothing: what the page itself says -- a <video>
// with a plain file/HLS URL (not a blob: of a script-fed player), og:video,
// JSON-LD VideoObject.contentUrl.
async function videoFromPage(page) {
  for (const f of page.frames()) {
    const found = await f.evaluate(() => {
      const media = (u) => (/^https?:\/\//i.test(u || '') && /\.(mp4|m4v|m3u8)(\?|#|$)/i.test(u) ? u : null);
      const plain = (u) => (/^https?:\/\//i.test(u || '') ? u : null);
      for (const v of document.querySelectorAll('video')) {
        if (v.getBoundingClientRect().width < innerWidth * 0.5) continue;
        const u = plain(v.currentSrc) || plain(v.getAttribute('src') && v.src)
          || [...v.querySelectorAll('source')].map((s) => plain(s.src)).find(Boolean);
        if (u) return u;
      }
      for (const m of document.querySelectorAll('meta[property^="og:video"]')) {
        const u = media(m.content);
        if (u) return u;
      }
      for (const s of document.querySelectorAll('script[type="application/ld+json"]')) {
        const m = /"contentUrl"\s*:\s*"([^"]+)"/.exec(s.textContent);
        if (m && media(m[1].replace(/\\\//g, '/'))) return m[1].replace(/\\\//g, '/');
      }
      return null;
    }).catch(() => null);
    if (found) return { url: found, frameUrl: f.url() };
  }
  return null;
}

// -> { ok, url, protocol: 'file'|'hls', headers, width, height, title, extractor } | { ok: false, reason }
async function resolveWebVideo(ctx, page, url, pageUrl) {
  const cookieFile = path.join(os.tmpdir(), `mb-cookies-${crypto.randomUUID()}.txt`);
  let r;
  try {
    // This session's cookies: age gates, logins, consent the user already gave.
    fs.writeFileSync(cookieFile, netscapeCookies(await ctx.cookies().catch(() => [])), { mode: 0o600 });
    r = await runHelper(url, pageUrl, cookieFile);
  } finally {
    fs.unlink(cookieFile, () => {});
  }
  if (r.ok) return r;
  const found = page ? await videoFromPage(page).catch(() => null) : null;
  if (!found) return r;
  const ua = await page.evaluate(() => navigator.userAgent).catch(() => '');
  return {
    ok: true, url: found.url, protocol: /\.m3u8(\?|#|$)/i.test(found.url) ? 'hls' : 'file',
    headers: { 'User-Agent': ua, Referer: found.frameUrl || pageUrl }, title: await page.title().catch(() => ''),
    extractor: 'page',
  };
}

// The headers the relay sends upstream: the stream's own (yt-dlp's) plus this
// session's cookies for that host.
async function relayHeaders(ctx, r) {
  const h = {};
  for (const [k, v] of Object.entries(r.headers || {})) if (v != null && !/^accept-encoding$/i.test(k)) h[k] = String(v);
  if (!Object.keys(h).some((k) => /^cookie$/i.test(k))) {
    const jar = await ctx.cookies(r.url).catch(() => []);
    if (jar.length) h.Cookie = jar.map((c) => `${c.name}=${c.value}`).join('; ');
  }
  return h;
}

// This server's public IP (Cloudflare's trace endpoint), cached.
let publicIp = { ip: null, at: 0 };
function serverPublicIp() {
  if (publicIp.ip && Date.now() - publicIp.at < 10 * 60 * 1000) return Promise.resolve(publicIp.ip);
  return new Promise((resolve) => {
    const req = https.get('https://1.1.1.1/cdn-cgi/trace', { timeout: 4000 }, (res) => {
      let body = '';
      res.on('data', (c) => { body += c; });
      res.on('end', () => {
        const m = /^ip=(.+)$/m.exec(body);
        if (m) publicIp = { ip: m[1].trim(), at: Date.now() };
        resolve(m ? m[1].trim() : null);
      });
    });
    req.on('timeout', () => req.destroy());
    req.on('error', () => resolve(null));
  });
}

// Would the phone's player get this stream straight from the CDN? Asked
// without the site's headers, like BB10's MediaPlayer will (some CDNs
// answer 410 then, even to the right IP). Only meaningful from the same IP.
function probeDirect(url) {
  return new Promise((resolve) => {
    let u;
    try { u = new URL(url); } catch (_) { resolve(false); return; }
    const mod = u.protocol === 'https:' ? https : http;
    const req = mod.get(u, {
      timeout: 4000,
      headers: { Range: 'bytes=0-1', 'User-Agent': 'Mozilla/5.0 (BB10; Kbd) AppleWebKit/537.35+ (KHTML, like Gecko) Version/10.3.3.3216 Mobile Safari/537.35+' },
    }, (res) => {
      res.resume();
      resolve(res.statusCode === 200 || res.statusCode === 206);
    });
    req.on('timeout', () => req.destroy());
    req.on('error', () => resolve(false));
  });
}

// --- the media relay -------------------------------------------------------
// GET /media/<id>/video.mp4|m3u8 streams one resolved video (Range passes
// through, so the player can seek). The id is a random capability, like a
// download link: the phone's MediaPlayer can't send the token header. HLS
// playlists are rewritten so their variants, segments and keys come through
// here too (/media/<id>/r/<name>?u=<url>), limited to the hosts the playlists
// themselves named.
const RELAY_TTL_MS = 3 * 60 * 60 * 1000; // since last use
const MAX_RELAYS = 64;
const relays = new Map();

function addRelay(url, headers) {
  if (relays.size >= MAX_RELAYS) {
    let oldest = null;
    for (const [id, m] of relays) if (!oldest || m.lastUsed < relays.get(oldest).lastUsed) oldest = id;
    relays.delete(oldest);
  }
  const id = crypto.randomBytes(16).toString('hex');
  relays.set(id, { url, headers, hosts: new Set([new URL(url).host]), lastUsed: Date.now() });
  return id;
}
setInterval(() => {
  for (const [id, m] of relays) if (Date.now() - m.lastUsed > RELAY_TTL_MS) relays.delete(id);
}, 10 * 60 * 1000).unref();

// This machine's address on the home network (where the phone is when it
// shares our public IP), or null: the first private IPv4 of a real interface.
function lanAddress() {
  for (const [name, addrs] of Object.entries(os.networkInterfaces())) {
    if (/^(lo|docker|br-|veth|virbr|tun|tap|wg)/.test(name)) continue;
    for (const a of addrs || []) {
      if (a.family === 'IPv4' && !a.internal && /^(10\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.)/.test(a.address)) return a.address;
    }
  }
  return null;
}

// At most this many responses streaming at once per video: a player that
// seeks opens a new request and may leave the old one hanging, and through
// the Cloudflare tunnel stalled streams hold back everything else on it (on
// 2026-10-07 a session of them froze the tunnel, the browser's own WebSocket
// included, until cloudflared was restarted).
const MAX_STREAMS_PER_VIDEO = 3;

// Registers GET /media/... on `app` (reached through the tunnel) and, when
// lanPort is set, on a second listener on the home network: at home the
// video then goes phone <-> this machine directly, never through the tunnel.
// -> { lanBase(): 'http://<lan ip>:<port>' once listening, else null }
function installMediaRelay(app, { allowPrivate, lanHost, lanPort }) {
  // Upstream connections only to public addresses (same rule as the
  // browser's filtering proxy), checked on the address actually connected to.
  function safeLookup(hostname, options, cb) {
    dns.lookup(hostname, { ...options, all: true }, (err, addrs) => {
      if (err) return cb(err);
      if (!allowPrivate && addrs.some((a) => isBlockedAddress(a.address))) return cb(new Error('private address'));
      if (options && options.all) return cb(null, addrs);
      cb(null, addrs[0].address, addrs[0].family);
    });
  }

  function fetchUpstream(url, headers, redirectsLeft, holder, onResponse, onError) {
    let u;
    try { u = new URL(url); } catch (e) { onError(e); return; }
    const mod = u.protocol === 'https:' ? https : u.protocol === 'http:' ? http : null;
    if (!mod) { onError(new Error('bad protocol')); return; }
    // timeout = socket idle: also what ends a response whose player stopped
    // reading (the pipe pauses the upstream, which then goes quiet).
    const req = mod.request(u, { headers, lookup: safeLookup, timeout: 15000 }, (up) => {
      if ([301, 302, 303, 307, 308].includes(up.statusCode) && up.headers.location && redirectsLeft > 0) {
        up.resume();
        fetchUpstream(new URL(up.headers.location, u).href, headers, redirectsLeft - 1, holder, onResponse, onError);
        return;
      }
      onResponse(up, u.href);
    });
    holder.req = req;
    req.on('timeout', () => req.destroy(new Error('upstream timeout')));
    req.on('error', onError);
    req.end();
  }

  // The scheme+host the phone used to reach us (through cloudflared).
  function publicBase(req) {
    const proto = String(req.headers['x-forwarded-proto'] || (req.headers['cf-ray'] ? 'https' : 'http')).split(',')[0].trim();
    return `${proto}://${req.headers['x-forwarded-host'] || req.headers.host}`;
  }

  function rewritePlaylist(text, baseUrl, m, prefix) {
    const via = (ref) => {
      const abs = new URL(ref, baseUrl);
      m.hosts.add(abs.host);
      const name = encodeURIComponent(abs.pathname.split('/').pop() || 'x');
      return `${prefix}/r/${name}?u=${encodeURIComponent(abs.href)}`;
    };
    return text.split(/\r?\n/).map((line) => {
      const t = line.trim();
      if (!t) return line;
      if (t.startsWith('#')) return line.replace(/URI="([^"]+)"/g, (_, ref) => `URI="${via(ref)}"`);
      return via(t);
    }).join('\n');
  }

  function handle(req, res) {
    const m = relays.get(req.params.id);
    if (!m) return res.status(404).end();
    m.lastUsed = Date.now();
    const via = req.headers['cf-ray'] ? 'tunnel' : 'lan';
    const started = Date.now();
    m.active = m.active || [];
    m.active.push(res);
    while (m.active.length > MAX_STREAMS_PER_VIDEO) m.active.shift().destroy();
    res.on('close', () => {
      m.active = m.active.filter((r) => r !== res);
      if (Date.now() - started > 1000) {
        console.log(`[video] relay ${req.params.id.slice(0, 6)} ${via} ${req.headers.range || 'whole'}: ` +
                    `${(Date.now() - started) / 1000}s${res.writableFinished ? '' : ', cut'}`);
      }
    });
    let target = m.url;
    if (req.path.includes('/r/')) {
      let u;
      try { u = new URL(String(req.query.u || '')); } catch (_) { return res.status(400).end(); }
      if (!/^https?:$/.test(u.protocol) || !m.hosts.has(u.host)) return res.status(403).end();
      target = u.href;
    }
    const headers = { ...m.headers };
    if (req.headers.range) headers.Range = req.headers.range;
    const holder = {};
    res.on('close', () => { if (holder.req) holder.req.destroy(); });
    fetchUpstream(target, headers, 5, holder, (up, finalUrl) => {
      const ct = String(up.headers['content-type'] || '');
      if (up.statusCode < 300 && (/mpegurl/i.test(ct) || /\.m3u8$/i.test(new URL(finalUrl).pathname))) {
        const chunks = [];
        let size = 0;
        up.on('data', (c) => { size += c.length; if (size > (4 << 20)) up.destroy(); else chunks.push(c); });
        up.on('end', () => {
          const body = rewritePlaylist(Buffer.concat(chunks).toString('utf8'), finalUrl, m, `${publicBase(req)}/media/${req.params.id}`);
          res.set({ 'Content-Type': 'application/vnd.apple.mpegurl', 'Cache-Control': 'no-store' }).send(body);
        });
        up.on('error', () => res.destroy());
        return;
      }
      res.status(up.statusCode);
      for (const h of ['content-type', 'content-length', 'content-range', 'accept-ranges', 'last-modified', 'etag']) {
        if (up.headers[h]) res.setHeader(h, up.headers[h]);
      }
      res.setHeader('Cache-Control', 'no-store');
      up.pipe(res);
    }, (e) => {
      if (!res.destroyed) console.warn(`[video] relay ${req.params.id.slice(0, 6)} ${via}: ${e.message}`);
      if (!res.headersSent) res.status(502).end();
      else res.destroy();
    });
  }

  const routes = ['/media/:id/r/:name', '/media/:id/:name'];
  app.get(routes, handle);
  let lanBase = null;
  if (lanHost && lanPort) {
    const lan = express();
    lan.get(routes, handle);
    lan.use((req, res) => res.status(404).end());
    const server = lan.listen(lanPort, lanHost, () => {
      lanBase = `http://${lanHost}:${lanPort}`;
      console.log(`[video] media relay also on ${lanBase} (home network)`);
    });
    server.on('error', (e) => console.warn(`[video] home-network relay not started: ${e.message}`));
  }
  return { lanBase: () => lanBase };
}

module.exports = { videoAt, resolveWebVideo, relayHeaders, addRelay, serverPublicIp, probeDirect, lanAddress, installMediaRelay, venvPython, ytdlpAvailable, YTDLP_PYTHON };
