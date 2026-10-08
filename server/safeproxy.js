'use strict';
// A tiny local HTTP/HTTPS proxy that every browser request goes through, so the
// render server cannot be used to reach the machine's own network (SSRF).
//
// Checking the URL is not enough: a public-looking name can resolve to
// 127.0.0.1 or 192.168.x.x, a page can redirect there, or the DNS answer can
// change between the check and the connection. So the proxy resolves the name
// ITSELF, refuses private/loopback/link-local/metadata addresses, and connects
// to the exact IP it checked. It also allows only a few destination ports
// (no SMTP/SSH/database relaying).
const http = require('http');
const net = require('net');
const dns = require('dns').promises;

const blocked = new net.BlockList();
for (const [addr, prefix] of [
  ['0.0.0.0', 8], ['10.0.0.0', 8], ['100.64.0.0', 10], ['127.0.0.0', 8],
  ['169.254.0.0', 16],            // link-local, cloud metadata (169.254.169.254)
  ['172.16.0.0', 12], ['192.0.0.0', 24], ['192.0.2.0', 24], ['192.168.0.0', 16],
  ['198.18.0.0', 15], ['198.51.100.0', 24], ['203.0.113.0', 24],
  ['224.0.0.0', 4], ['240.0.0.0', 4],
]) blocked.addSubnet(addr, prefix, 'ipv4');
for (const [addr, prefix] of [
  ['::', 128], ['::1', 128], ['fc00::', 7], ['fe80::', 10], ['fec0::', 10], ['ff00::', 8],
  ['64:ff9b::', 96], ['2002::', 16], ['100::', 64],
]) blocked.addSubnet(addr, prefix, 'ipv6');

// IPv4-mapped IPv6 (::ffff:a.b.c.d) could smuggle 127.0.0.1 / 10.x past an IPv6-only
// check. It must NOT go in the BlockList: Node then matches every plain IPv4
// address against ::ffff:0:0/96 and blocks the whole internet. Handled here.
function isMappedIPv6(address) {
  try { return new URL(`http://[${address}]/`).hostname.startsWith('[::ffff:'); } catch (_) { return true; }
}

function isBlockedAddress(address) {
  const family = net.isIP(address);
  if (!family) return true; // not an IP at all: refuse
  if (family === 6 && isMappedIPv6(address)) return true;
  return blocked.check(address, family === 6 ? 'ipv6' : 'ipv4');
}

const DEFAULT_PORTS = [80, 443, 8080, 8443];
const IDLE_MS = 5 * 60 * 1000;   // a tunnel with no traffic for this long is closed
const MAX_TUNNELS = 500;         // open tunnels at once (a browser uses a few dozen)

class Blocked extends Error {}

// -> the addresses to try, or throws Blocked. ANY forbidden address blocks the
// whole name (no mixing a public and a private answer).
async function resolveAllowed(host, allowPrivate) {
  host = host.replace(/^\[|\]$/g, '');
  const addrs = net.isIP(host)
    ? [{ address: host, family: net.isIP(host) }]
    : await dns.lookup(host, { all: true, verbatim: true });
  if (!addrs.length) throw new Error('no address for ' + host);
  if (!allowPrivate) {
    for (const a of addrs) if (isBlockedAddress(a.address)) throw new Blocked('private address');
  }
  return addrs;
}

function connectFirst(addrs, port) {
  return new Promise((resolve, reject) => {
    let i = 0;
    const next = (lastErr) => {
      if (i >= addrs.length) return reject(lastErr || new Error('connect failed'));
      const s = net.connect({ host: addrs[i++].address, port });
      s.setTimeout(15000, () => s.destroy(new Error('connect timeout')));
      s.once('connect', () => { s.setTimeout(0); s.removeAllListeners('error'); resolve(s); });
      s.once('error', (e) => next(e));
    };
    next();
  });
}

function splitHostPort(s, defaultPort) {
  const m = /^(\[[^\]]+\]|[^:]+)(?::(\d+))?$/.exec(s);
  return m ? { host: m[1], port: m[2] ? parseInt(m[2], 10) : defaultPort } : null;
}

// options: { allowPrivate, ports:[...], onBlocked(host) }
// -> Promise<{ port, close(), state }>; listens on 127.0.0.1 only.
function startSafeProxy(options = {}) {
  const allowPrivate = !!options.allowPrivate;
  const ports = new Set(options.ports || DEFAULT_PORTS);
  const state = { lastBlockedAt: 0, lastBlockedHost: '', tunnels: 0 };
  const maxTunnels = options.maxTunnels || MAX_TUNNELS;

  const portAllowed = (p) => allowPrivate || ports.has(p);
  const noteBlocked = (host) => {
    state.lastBlockedAt = Date.now();
    state.lastBlockedHost = host;
    if (options.onBlocked) options.onBlocked(host);
  };

  const server = http.createServer(async (req, res) => {
    // Plain http:// through a proxy arrives with an absolute URL.
    let u;
    try { u = new URL(req.url); } catch (_) { u = null; }
    if (!u || u.protocol !== 'http:') { res.writeHead(400); return res.end(); }
    const port = u.port ? parseInt(u.port, 10) : 80;
    try {
      if (!portAllowed(port)) throw new Blocked('port');
      const addrs = await resolveAllowed(u.hostname, allowPrivate);
      const headers = { ...req.headers };
      delete headers['proxy-connection'];
      delete headers['proxy-authorization'];
      let lastErr;
      for (const a of addrs) {
        try {
          await new Promise((resolve, reject) => {
            const up = http.request({ host: a.address, port, method: req.method, path: u.pathname + u.search, headers, timeout: 60000 }, (r) => {
              res.writeHead(r.statusCode, r.headers);
              r.pipe(res);
              r.on('end', resolve);
            });
            up.on('error', reject);
            up.on('timeout', () => up.destroy(new Error('upstream timeout')));
            req.on('aborted', () => up.destroy());
            req.pipe(up);
          });
          return;
        } catch (e) {
          lastErr = e;
          if (res.headersSent) return res.destroy();
        }
      }
      throw lastErr || new Error('upstream failed');
    } catch (e) {
      if (e instanceof Blocked) noteBlocked(u.hostname);
      if (!res.headersSent) res.writeHead(e instanceof Blocked ? 403 : 502);
      res.end();
    }
  });

  // https:// (and everything TLS) arrives as CONNECT host:port.
  server.on('connect', async (req, clientSocket, head) => {
    const hp = splitHostPort(req.url, 443);
    const fail = (code) => { clientSocket.end(`HTTP/1.1 ${code}\r\nConnection: close\r\n\r\n`); };
    clientSocket.on('error', () => {});
    if (!hp) return fail('400 Bad Request');
    if (state.tunnels >= maxTunnels) return fail('503 Service Unavailable');
    try {
      if (!portAllowed(hp.port)) throw new Blocked('port');
      const addrs = await resolveAllowed(hp.host, allowPrivate);
      const upstream = await connectFirst(addrs, hp.port);
      clientSocket.write('HTTP/1.1 200 Connection Established\r\n\r\n');
      if (head && head.length) upstream.write(head);
      upstream.pipe(clientSocket);
      clientSocket.pipe(upstream);
      state.tunnels++;
      let done = false;
      const close = () => {
        if (!done) { done = true; state.tunnels--; }
        upstream.destroy();
        clientSocket.destroy();
      };
      upstream.on('error', close);
      clientSocket.on('close', close);
      upstream.on('close', close);
      // Idle timeouts: a stalled peer must not hold a file descriptor forever.
      upstream.setTimeout(IDLE_MS, close);
      clientSocket.setTimeout(IDLE_MS, close);
    } catch (e) {
      if (e instanceof Blocked) { noteBlocked(hp.host); fail('403 Forbidden'); } else fail('502 Bad Gateway');
    }
  });

  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => {
      resolve({ port: server.address().port, close: () => server.close(), state });
    });
  });
}

module.exports = { startSafeProxy, isBlockedAddress, resolveAllowed };
