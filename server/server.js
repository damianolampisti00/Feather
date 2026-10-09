'use strict';
const fs = require('fs');
const path = require('path');
const { randomUUID } = require('crypto');
const { execFile } = require('child_process');
const express = require('express');
const { WebSocketServer } = require('ws');
const { loadToken, makeGuard, clientIp } = require('./guard');
const { startSafeProxy } = require('./safeproxy');
const { chromium } = require('playwright');
const { createDeltaEncoder, deltaAvailable } = require('./deltaframes');
const webVideo = require('./webvideo');
const pageAudio = require('./pageaudio');

const PORT = parseInt(process.env.PORT || '8080', 10);
const ARGS = process.argv.slice(2);
// Loopback by default: reachable only from this machine (e.g. cloudflared).
// --lan (or HOST=0.0.0.0) exposes it on the local network, for a phone on Wi-Fi.
const HOST = ARGS.includes('--lan') ? '0.0.0.0' : (process.env.HOST || '127.0.0.1');
// The browser may only reach the public internet, unless explicitly relaxed
// (development against local test pages).
const ALLOW_PRIVATE = ARGS.includes('--allow-private') || process.env.ALLOW_PRIVATE === '1';
const EXTRA_PORTS = (process.env.ALLOW_PORTS || '').split(',').map((x) => parseInt(x, 10)).filter(Boolean);
const proxyRef = { current: null };
const MAX_SESSIONS = parseInt(process.env.MAX_SESSIONS || '3', 10);
const MAX_TABS = parseInt(process.env.MAX_TABS || '6', 10);
const IDLE_MS = 10 * 60 * 1000;
const VIEWPORT = { width: 720, height: 720 };
// 35-45 is markedly smaller and faster than the old fixed 60 with only a small,
// hard-to-notice loss on the 720px tile the phone actually shows (measured
// 2026-09-22). Override with JPEG_QUALITY if needed.
const JPEG_QUALITY = parseInt(process.env.JPEG_QUALITY || '40', 10);
// Frames a flow=ack WebSocket client (see the /ws handler) may have received
// but not yet confirmed as decoded; past that, newer frames wait here and only
// the newest is kept. The window, not the phone, set the pace: through the
// Cloudflare tunnel a frame's round trip is long enough that 3 in flight capped
// the phone near 15 fps while it decoded in ~15 ms on each of two cores
// (2026-10-06). 6 covers the round trip; the phone still never queues more
// than a handful (see the client's onWsBinary: 2 decoding + 1 waiting).
const FRAME_WINDOW = Math.max(1, parseInt(process.env.FRAME_WINDOW || '6', 10));
// Scroll steps of a frames=delta client are whole multiples of this many CSS px
// (= screencast frame px): the JPEG block size, see deltaframes.js.
const SCROLL_QUANTUM = Math.max(1, parseInt(process.env.SCROLL_QUANTUM || '16', 10));
const MAX_DOWNLOAD_BYTES = 100 * 1024 * 1024; // refused above this, before any byte is sent
const DOWNLOAD_TTL_MS = 15 * 60 * 1000;      // unclaimed downloads are dropped after this

// Cookies/localStorage of each session are saved here so a login survives a
// server restart or the idle/LRU eviction of the session.
const STATE_DIR = path.join(__dirname, 'sessions');
fs.mkdirSync(STATE_DIR, { recursive: true });
const stateFile = (id) => path.join(STATE_DIR, id.replace(/[^A-Za-z0-9_-]/g, '_').slice(0, 64) + '.json');

let browserPromise = null;
function getBrowser() {
  if (!browserPromise) {
    browserPromise = (async () => {
      // Every request of the browser goes through the filtering proxy.
      const proxy = await startSafeProxy({
        allowPrivate: ALLOW_PRIVATE,
        ports: [80, 443, 8080, 8443, ...EXTRA_PORTS],
        onBlocked: (host) => console.warn(`blocked request to ${host}`),
      });
      proxyRef.current = proxy;
      const opts = {
        headless: true,
        // --mute-audio (a Playwright default) is dropped when the page sound
        // goes to the phone (pageaudio.js); the browser then plays into its
        // own virtual output, never this machine's speakers.
        ignoreDefaultArgs: pageAudio.browserOptions ? ['--enable-automation', '--mute-audio'] : ['--enable-automation'],
        ...(pageAudio.browserOptions ? { env: pageAudio.browserOptions.env } : {}),
        args: [
          '--disable-blink-features=AutomationControlled',
          `--proxy-server=http://127.0.0.1:${proxy.port}`,
          '--proxy-bypass-list=<-loopback>', // loopback must go through the proxy too, to be refused
          // WebRTC could otherwise open UDP connections that skip the proxy.
          '--force-webrtc-ip-handling-policy=disable_non_proxied_udp',
          // Instant (non-animated) wheel scrolling. The client predicts motion
          // locally and shifts the shown frame to follow the finger (see
          // scrollAt in browsercontroller / tile.translationY in main.qml), so
          // Chromium's own smooth-scroll easing only added latency and a second,
          // competing animation -- which is what made scrolling look strobey.
          '--disable-smooth-scrolling',
        ],
      };
      // The installed Edge/Chrome (BROWSER_CHANNEL=msedge|chrome; default msedge on Windows)
      // is what a person really signs in with, so it matches login.js; if it is
      // missing, fall back to Playwright's bundled Chromium.
      // BROWSER_PATH=/usr/bin/chromium: a specific executable (Linux servers), no lookup.
      if (process.env.BROWSER_PATH) return chromium.launch({ ...opts, executablePath: process.env.BROWSER_PATH });
      const channel = process.env.BROWSER_CHANNEL || (process.platform === 'win32' ? 'msedge' : '');
      if (!channel) return chromium.launch(opts);
      return chromium.launch({ ...opts, channel }).catch((e) => {
        console.warn(`Could not start "${channel}" (${String(e.message).split('\n')[0]}); using bundled Chromium`);
        return chromium.launch(opts);
      });
    })();
  }
  return browserPromise;
}

// sessionId -> { id, lastUsed, queue, ctxPromise, tabs, activeTabId, zoom,
//                 viewH, lastSave, downloads, newDownloads }
// The entry is created synchronously so concurrent requests for the same
// sessionId always share one entry (and one queue); the browser context is
// created lazily inside the queue.
//
// Each session can hold several browser TABS (`tabs`, a user-visible list the
// client can switch between), all sharing the one BrowserContext (so cookies
// and logins are shared across tabs, like a real browser). Each tab itself
// has its own `stack`: stack[0] is the page the user navigates. A page a site
// opens (window.open / target=_blank, e.g. "Sign in with Google") becomes a
// TAB of its own, made active, that remembers its opener tab (`opener`):
// when it closes itself (the end of an OAuth sign-in) the view goes back to
// the opener. Only when MAX_TABS are already open does it stack on top of
// the opener tab instead (later stack entries), as before: the top of a
// tab's stack is what the client sees for that tab.
// Pages the server opens itself (ownNewPage) never count as popups.
const sessions = new Map();

function newTab(s, opener = null) {
  const tab = { id: randomUUID().slice(0, 8), stack: [], title: '', url: '', opener };
  s.tabs.push(tab);
  return tab;
}

// A page the server opens itself (a new tab, a replacement for a closed
// one): flagged so context.on('page') doesn't take it for a site's popup.
async function ownNewPage(s, ctx) {
  s.creatingPage = (s.creatingPage || 0) + 1;
  try {
    return await ctx.newPage();
  } finally {
    s.creatingPage--;
  }
}

// A popup's tab is gone (its page closed, or it could not render): drop it
// and, if it was on screen, show the tab that opened it.
function dropPopupTab(s, tab) {
  const i = s.tabs.indexOf(tab);
  if (i < 0) return;
  s.tabs.splice(i, 1);
  if (s.activeTabId === tab.id) {
    const back = s.tabs.find((t) => t.id === tab.opener) || s.tabs[Math.min(i, s.tabs.length - 1)];
    s.activeTabId = back ? back.id : null;
  }
  if (s.tabs.length === 0) s.activeTabId = newTab(s).id; // activePage gives it a page
}

function findTab(s, id) {
  return s.tabs.find((t) => t.id === id) || s.tabs[0];
}

// Which tab (if any) a given Page object belongs to.
function tabOf(s, page) {
  return s.tabs.find((t) => t.stack.includes(page));
}

// Plain, JSON-safe summary of every open tab, for the client's tab switcher.
function tabsSummary(s) {
  return s.tabs.map((t) => ({ id: t.id, active: t.id === s.activeTabId, title: t.title, url: t.url }));
}

async function saveState(s) {
  s.lastSave = Date.now();
  try {
    const ctx = await s.ctxPromise;
    if (ctx) await ctx.storageState({ path: stateFile(s.id) });
  } catch (_) {}
}

function closeEntry(id) {
  const s = sessions.get(id);
  if (!s) return;
  sessions.delete(id);
  // Let in-flight work on this session finish before tearing it down
  s.queue = s.queue.then(async () => {
    try {
      await saveState(s);
      const ctx = await s.ctxPromise;
      if (ctx) await ctx.close();
    } catch (_) {}
  });
}

function getEntry(id) {
  let s = sessions.get(id);
  if (!s) {
    while (sessions.size >= MAX_SESSIONS) {
      let oldest = null;
      for (const [k, v] of sessions) if (!oldest || v.lastUsed < sessions.get(oldest).lastUsed) oldest = k;
      closeEntry(oldest);
    }
    s = { id, lastUsed: Date.now(), queue: Promise.resolve(), ctxPromise: null,
         tabs: [], activeTabId: null, lastSave: 0,
         downloads: new Map(), newDownloads: [] };
    sessions.set(id, s);
  }
  s.lastUsed = Date.now();
  return s;
}

function trackPage(s, tab, page) {
  if (tab.stack.includes(page)) return;
  tab.stack.push(page);
  // A click/link that turns into a file download. The browser (on this PC) keeps
  // downloading in the background; the client is told and asks the user first.
  page.on('download', (download) => {
    const id = randomUUID();
    const name = download.suggestedFilename() || 'download';
    s.downloads.set(id, { download, name, created: Date.now() });
    s.newDownloads.push({ id, name });
  });
  page.on('close', () => {
    const i = tab.stack.indexOf(page);
    if (i >= 0) tab.stack.splice(i, 1);
    if (tab.opener && tab.stack.length === 0) dropPopupTab(s, tab);
  });
}

function getContext(s) {
  if (!s.ctxPromise) {
    s.ctxPromise = (async () => {
      const browser = await getBrowser();
      // Headless Chromium's default UA says "HeadlessChrome", which Google and
      // others answer with a bot check; present as ordinary desktop Chrome.
      const major = browser.version().split('.')[0];
      const userAgent = `Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/${major}.0.0.0 Safari/537.36`;
      const opts = { viewport: VIEWPORT, deviceScaleFactor: 1, userAgent };
      const file = stateFile(s.id);
      if (fs.existsSync(file)) opts.storageState = file;
      const context = await browser.newContext(opts);
      // A page a site opened becomes a new active tab that remembers the tab
      // it came from (via its opener page; else the active tab, e.g. for
      // rel=noopener links). Pages opened by the server itself (ownNewPage)
      // are tracked where they were created.
      context.on('page', async (p) => {
        if (s.creatingPage) {
          trackPage(s, findTab(s, s.activeTabId), p);
          return;
        }
        let from = null;
        try { const opener = await p.opener(); if (opener) from = tabOf(s, opener); } catch (_) {}
        if (!from) from = findTab(s, s.activeTabId);
        if (s.tabs.length < MAX_TABS) {
          const tab = newTab(s, from.id);
          s.activeTabId = tab.id;
          trackPage(s, tab, p);
        } else {
          trackPage(s, from, p); // tabs full: on top of its opener, as a popup
        }
      });
      const firstTab = newTab(s);
      s.activeTabId = firstTab.id;
      trackPage(s, firstTab, await ownNewPage(s, context));
      return context;
    })();
  }
  return s.ctxPromise;
}

// The active tab (creates the session's first tab on first use).
async function activeTab(s) {
  await getContext(s);
  return findTab(s, s.activeTabId);
}

// The page the user is looking at within the active tab (newest popup, else
// that tab's main page).
async function activePage(s) {
  const ctx = await getContext(s);
  const tab = await activeTab(s);
  if (tab.stack.length === 0) trackPage(s, tab, await ownNewPage(s, ctx)); // main page was closed
  return tab.stack[tab.stack.length - 1];
}

async function closePopups(s) {
  const tab = await activeTab(s);
  for (const p of tab.stack.slice(1)) await p.close().catch(() => {});
}

// Serialize work per session
function enqueue(id, fn) {
  const s = getEntry(id);
  const run = s.queue.then(() => fn(s));
  s.queue = run.catch(() => {});
  return run;
}

setInterval(() => {
  const now = Date.now();
  for (const [id, s] of sessions) if (now - s.lastUsed > IDLE_MS) closeEntry(id);
}, 60 * 1000).unref();

const isHttp = (u) => /^https?:\/\//i.test(u);

// After a FAILED navigation Chromium is still loading its own error page; a new
// goto() issued right then is cancelled ("interrupted by another navigation to
// chrome-error://chromewebdata/"). Retry it a couple of times.
async function gotoWithRetry(page, url) {
  for (let attempt = 0; ; attempt++) {
    try {
      return await page.goto(url, { waitUntil: 'domcontentloaded', timeout: 20000 });
    } catch (e) {
      if (attempt >= 2 || !/interrupted by another navigation/i.test(String(e.message))) throw e;
      await new Promise((r) => setTimeout(r, 600));
    }
  }
}

async function settle(page, ms) {
  try { await page.waitForLoadState('networkidle', { timeout: ms }); } catch (_) {}
}

async function collectLinks(page) {
  return page.evaluate(() => {
    const vw = window.innerWidth, vh = window.innerHeight;
    const out = [];
    for (const a of document.querySelectorAll('a[href]')) {
      const r = a.getBoundingClientRect();
      if (r.width <= 0 || r.height <= 0) continue;
      if (r.right < 0 || r.bottom < 0 || r.left > vw || r.top > vh) continue;
      const href = a.href;
      if (!/^https?:\/\//i.test(href)) continue;
      // Skip links occluded by other elements (overlays, images, hidden menus):
      // the topmost element at the visible centre must belong to this link.
      const cx = Math.min(Math.max(r.left + r.width / 2, 0), vw - 1);
      const cy = Math.min(Math.max(r.top + r.height / 2, 0), vh - 1);
      const top = document.elementFromPoint(cx, cy);
      if (!top || (top !== a && !a.contains(top))) continue;
      out.push({
        url: href,
        x: Math.round(r.x), y: Math.round(r.y),
        w: Math.round(r.width), h: Math.round(r.height),
      });
    }
    return out;
  });
}

// A click can trigger a navigation that destroys the execution context
// mid-evaluate; retry once after the page has settled.
async function retryOnNav(page, fn) {
  try { return await fn(); } catch (e) {
    // ("Not attached to an active page": a capture racing the renderer swap
    // of the navigation a click just started.)
    if (!/context was destroyed|navigation|not attached to an active page/i.test(String(e.message))) throw e;
    await page.waitForLoadState('domcontentloaded', { timeout: 10000 }).catch(() => {});
    await settle(page, 3000);
    return fn();
  }
}

// Client output is always `viewW` x `viewH` image pixels. Zoom z renders the
// page in a (viewW/z) x (viewH/z) CSS viewport at deviceScaleFactor z: text is
// z times bigger *and* sharp, and sites lay out for a narrower screen.
// Coordinates on the wire are image pixels; CSS px = image px / z.
const DEFAULT_ZOOM = 1.5;
async function applyView(s, page, body) {
  const zoom = Math.min(3, Math.max(1, Number(body.zoom) || s.zoom || DEFAULT_ZOOM));
  const viewH = Math.min(1440, Math.max(200, Math.round(Number(body.viewHeight) || s.viewH || VIEWPORT.height)));
  s.zoom = zoom;
  s.viewH = viewH;
  // Emulation state lives on each tab, so a popup gets its own.
  const st = page.__mb || (page.__mb = {});
  // The client's light/dark setting: prefers-color-scheme for sites that have
  // a dark style of their own, plus Chromium's automatic dark mode (the same
  // as Chrome's "dark theme for sites") for those that don't.
  const theme = body.theme === 'dark' || body.theme === 'light' ? body.theme : (s.theme || 'light');
  s.theme = theme;
  if (st.theme !== theme) {
    st.cdp = st.cdp || (await page.context().newCDPSession(page));
    await page.emulateMedia({ colorScheme: theme }).catch(() => {});
    await st.cdp.send('Emulation.setAutoDarkModeOverride', theme === 'dark' ? { enabled: true } : {}).catch(() => {});
    st.theme = theme;
  }
  if (st.zoom === zoom && st.viewH === viewH) return st;
  const w = Math.round(VIEWPORT.width / zoom);
  const h = Math.round(viewH / zoom);
  await page.setViewportSize({ width: w, height: h });
  st.cdp = st.cdp || (await page.context().newCDPSession(page));
  st.metrics = { width: w, height: h, deviceScaleFactor: zoom, mobile: false };
  await st.cdp.send('Emulation.setDeviceMetricsOverride', st.metrics);
  st.zoom = zoom;
  st.viewH = viewH;
  // The emulated device pixel size just changed (or this is the first call):
  // a running screencast was sized for the old one, restart it so live
  // WebSocket viewers (see subscribeToPage/ensureScreencast) get frames at
  // the new size instead of ones Chromium would otherwise have to rescale.
  if (st.screencastStarted) {
    st.screencastStarted = false;
    await st.cdp.send('Page.stopScreencast').catch(() => {});
    await ensureScreencast(page, st);
  }
  return st;
}

// Live, push-based frame delivery for WebSocket viewers (see the /ws handler
// below), on top of the request/response screenshot every action already
// takes: CDP's own Page.startScreencast makes Chromium emit a new JPEG
// itself on every repaint (mid-scroll, mid-page-load, ...) with no polling
// and no per-frame request/response round trip -- this is what lets a scroll
// or a slow page load look continuous instead of jumping once per action.
// Reuses the CDP session applyView() already opened per page (st.cdp) rather
// than a second one, and is a no-op if already running.
async function ensureScreencast(page, st) {
  if (st.screencastStarted || !st.cdp) return;
  st.screencastStarted = true;
  // Zoom != 1 is emulated with deviceScaleFactor = zoom (1.5x: a 480x420 CSS
  // px page drawn at 720x630 device px). captureScreenshot honors that, but
  // headless Chromium's screencast captures the view at its CSS-pixel size
  // regardless (480x420; Emulation's `scale` doesn't change it either --
  // measured 2026-10-05), and the client shows frames 1:1, so those frames
  // appeared shrunk in the tile's top-left corner. So at zoom != 1 the
  // screencast is only the "page changed" signal and every change is
  // captured properly instead, see captureSharpFrame.
  st.sharpCapture = (st.zoom || 1) !== 1;
  st.lastSignal = null;
  if (!st.screencastWired) {
    st.screencastWired = true;
    st.cdp.on('Page.screencastFrame', ({ data, metadata, sessionId: cdpSessionId }) => {
      st.cdp.send('Page.screencastFrameAck', { sessionId: cdpSessionId }).catch(() => {});
      if (!st.frameSubscribers || st.frameSubscribers.size === 0) return;
      if (st.sharpCapture && !st.scrolling) {
        // A capture makes Chromium draw a new frame, which the screencast
        // reports: without this check every capture caused the next one, ~30
        // captures/s on a page that wasn't changing at all. That redraw is
        // pixel-identical, so its (deterministic) JPEG is too: only a frame
        // that differs from the previous one is a real change.
        if (data === st.lastSignal) return;
        st.lastSignal = data;
        captureSharpFrame(st);
        return;
      }
      // Zoom 1, or zoom != 1 MID-SCROLL: the screencast frame itself. At zoom
      // != 1 it is CSS-pixel sized (e.g. 480x420), which the client stretches
      // to the tile: a little soft while moving -- not noticeable then -- but
      // free for the server, quicker for the phone to decode, and no sharp
      // capture competing with every scroll step for the page (see withInput).
      // The settle refresh at the end of the scroll brings the sharp frame.
      st.lastSignal = data;
      const buf = Buffer.from(data, 'base64');
      // The scroll this frame shows (CSS px), for frames=delta's shift.
      const meta = metadata ? { top: metadata.scrollOffsetY, left: metadata.scrollOffsetX, cssW: metadata.deviceWidth } : null;
      for (const cb of st.frameSubscribers) cb(buf, meta);
    });
  }
  try {
    // Full (CSS-pixel) size, so a change as small as one typed character
    // still shows up as a different frame; full quality too, since at zoom
    // != 1 these frames are also what is shown while scrolling.
    await st.cdp.send('Page.startScreencast', {
      format: 'jpeg', quality: JPEG_QUALITY,
      maxWidth: VIEWPORT.width, maxHeight: st.viewH || VIEWPORT.height, everyNthFrame: 1,
    });
  } catch (e) {
    st.screencastStarted = false;
    console.warn('startScreencast failed:', e.message);
  }
}

// One full-resolution frame of the page for its subscribers (zoom != 1, see
// ensureScreencast): ~33 ms at 720x630, which keeps up with the ~27 changes/s
// of a scroll. Changes during a capture coalesce into one more capture after it.
// Every Page.captureScreenshot of a page goes through here, strictly one at a
// time (the sharp frames below AND each action's result frame): see withInput.
// Each capture is bounded by its own timeout: one that never answers (the
// page navigating away mid-call...) must not stall every capture -- and every
// action -- queued behind it.
function captureJpeg(st) {
  const run = () => Promise.race([
    st.cdp.send('Page.captureScreenshot', { format: 'jpeg', quality: JPEG_QUALITY }),
    new Promise((_, reject) => setTimeout(() => reject(new Error('capture timeout')), 2500)),
  ]).then((r) => Buffer.from(r.data, 'base64'));
  const p = (st.captureChain || Promise.resolve()).then(run, run);
  st.captureChain = p.catch(() => {});
  return p;
}

function captureSharpFrame(st) {
  if (st.capturing || st.inputBusy) { st.captureAgain = true; return; }
  st.capturing = true;
  captureJpeg(st) // bounded by its own timeout, so `capturing` can't get stuck
    .then((buf) => {
      for (const cb of st.frameSubscribers || []) cb(buf);
    })
    .catch(() => {})
    .then(() => {
      st.capturing = false;
      if (st.captureAgain && !st.inputBusy) { st.captureAgain = false; captureSharpFrame(st); }
    });
}

// Mouse input (taps, wheel) must never overlap a Page.captureScreenshot: with
// a deviceScaleFactor emulation active (zoom != 1), Chromium swaps the
// emulation params while it captures, and a mouse event landing in that
// window is mapped with the wrong scale -- the click arrives at (x/z, y/z).
// Measured 2026-10-05: 17 of 20 clicks misplaced with captures running, 0
// without; and the sharp captures (captureSharpFrame) run all the time at zoom
// != 1, since the click's own repaint triggers one. Overlapping captures could
// even leave those params wrong afterwards (taps after a scroll kept missing),
// so all captures are serialized (captureJpeg) and the params are re-asserted
// right before the input. So: wait out the captures in flight, hold new ones
// back while the input is dispatched, catch up after.
async function withInput(st, fn) {
  st.inputBusy = (st.inputBusy || 0) + 1;
  try {
    if (st.captureChain) await st.captureChain;
    if (st.metrics) await st.cdp.send('Emulation.setDeviceMetricsOverride', st.metrics).catch(() => {});
    return await fn();
  } finally {
    st.inputBusy -= 1;
    if (!st.inputBusy && st.captureAgain && !st.capturing) { st.captureAgain = false; captureSharpFrame(st); }
  }
}

// `cb` is called with a JPEG Buffer every time this page's screencast
// produces a new frame. A page can have several subscribers at once in
// principle (not currently: each WS connection follows exactly one page,
// its session's active one) and the screencast itself is left running when
// the last subscriber leaves (see unsubscribeFromPage) -- cheap to keep
// alive, and avoids a stop/restart round trip on a quick tab-switch back.
async function subscribeToPage(page, cb) {
  const st = page.__mb || (page.__mb = {});
  if (!st.frameSubscribers) st.frameSubscribers = new Set();
  st.frameSubscribers.add(cb);
  await ensureScreencast(page, st);
}

function unsubscribeFromPage(page, cb) {
  const st = page && page.__mb;
  if (st && st.frameSubscribers) st.frameSubscribers.delete(cb);
}

// After a click/Enter a popup may have opened, or the popup we were in may have
// closed itself (end of an OAuth login): pick the tab that is now on top,
// give it the same view settings and let it load.
// The tab on top, ready to render. Some targets Edge/Chrome creates on the side
// (e.g. the throwaway tab of a file download) cannot be emulated and never show
// anything: such a tab is dropped from the stack and the one below is used.
async function usablePage(s, body) {
  for (;;) {
    const page = await activePage(s);
    try {
      await applyView(s, page, body);
      return page;
    } catch (e) {
      const tab = await activeTab(s);
      const i = tab.stack.indexOf(page);
      if (i <= 0 && !tab.opener) throw e; // a tab's own main page must work
      if (i >= 0) tab.stack.splice(i, 1);
      if (tab.opener && tab.stack.length === 0) dropPopupTab(s, tab); // a popup tab that can't render
      page.close().catch(() => {});
    }
  }
}

async function afterInteraction(s, body) {
  await new Promise((r) => setTimeout(r, 500));
  const active = await usablePage(s, body);
  await active.waitForLoadState('domcontentloaded', { timeout: 10000 }).catch(() => {});
  await settle(active, 2500);
  return active;
}

const DATE_LIKE_TYPES = ['date', 'month', 'week', 'time', 'datetime-local'];

// If an editable field is focused (after a tap), describe it so the client can
// open the matching native widget (text prompt, list picker, date picker);
// null otherwise. Drills into shadow roots (custom-element sites) and into
// same-origin iframes (embedded forms); a cross-origin iframe can't be
// introspected, but a real click still focuses whatever is inside it, so that
// case is reported generically instead of not at all.
async function focusedEditable(page) {
  return page.evaluate((DATE_LIKE_TYPES) => {
    // Where the field is, in viewport CSS px (iframe offsets added): the
    // client lays its own text field right over it (typing in place).
    let ox = 0, oy = 0;
    function box(el) {
      const r = el.getBoundingClientRect();
      return { x: Math.round(ox + r.left), y: Math.round(oy + r.top), w: Math.round(r.width), h: Math.round(r.height) };
    }
    function describe(el, depth) {
      if (!el) return null;
      // Custom-element sites (Reddit, ...) keep the real <input>/<textarea>
      // inside one or more shadow roots: activeElement is only the host.
      while (el.shadowRoot && el.shadowRoot.activeElement) el = el.shadowRoot.activeElement;
      if (el.tagName === 'IFRAME') {
        if (depth > 6) return null;
        // For a cross-origin iframe, .contentDocument is simply null (no throw)
        // -- can't read what's focused inside, but the click that got us here
        // did focus something real. Keyboard input still reaches it (dispatched
        // to whatever has focus, not through this JS context).
        let innerDoc = null;
        try { innerDoc = el.contentDocument; } catch (_) {}
        if (!innerDoc) {
          return { type: 'text', multiline: false, value: '', placeholder: '(campo in un frame esterno)', crossOrigin: true };
        }
        const fr = el.getBoundingClientRect();
        ox += fr.left + (el.clientLeft || 0);
        oy += fr.top + (el.clientTop || 0);
        return describe(innerDoc.activeElement, depth + 1);
      }
      if (el.tagName === 'SELECT') {
        const options = Array.from(el.options).map((o) => ({
          value: o.value, label: (o.textContent || '').trim(), selected: o.selected,
        }));
        return { type: el.multiple ? 'select-multiple' : 'select-one', multiple: !!el.multiple, options };
      }
      if (el.tagName === 'TEXTAREA') {
        // Google & co. use a one-row textarea as their search box: only a tall
        // one is really multi-line (Enter must submit the short ones).
        const tall = el.getBoundingClientRect().height > 70;
        return { type: 'textarea', multiline: tall, value: el.value, rect: box(el),
                 placeholder: el.placeholder || el.getAttribute('aria-label') || '' };
      }
      if (el.tagName === 'INPUT') {
        const t = (el.type || 'text').toLowerCase();
        if (DATE_LIKE_TYPES.includes(t)) {
          return { type: t, multiline: false, value: el.value, placeholder: '',
                   min: el.min || '', max: el.max || '' };
        }
        if (['text', 'search', 'email', 'url', 'tel', 'password', 'number'].includes(t)) {
          return { type: t, multiline: false, value: el.value, rect: box(el),
                   placeholder: el.placeholder || el.getAttribute('aria-label') || '' };
        }
        return null;
      }
      if (el.isContentEditable) {
        return { type: 'text', multiline: true, value: el.innerText, placeholder: '', rect: box(el) };
      }
      return null;
    }
    return describe(document.activeElement, 0);
  }, DATE_LIKE_TYPES);
}

// What's at this viewport point right now, drilling into same-origin iframes
// (a tap inside a cross-origin iframe is handled by the real mouse click
// below: Chromium hit-tests through frame boundaries on its own).
async function elementKindAt(page, x, y) {
  return page.evaluate(([x, y]) => {
    function kindAt(doc, x, y, depth) {
      const el = doc.elementFromPoint(x, y);
      if (!el) return null;
      if (el.tagName === 'IFRAME' && depth < 6) {
        const r = el.getBoundingClientRect();
        try {
          return kindAt(el.contentDocument, x - r.left, y - r.top, depth + 1);
        } catch (_) { return null; } // cross-origin: fall through to a normal click
      }
      if (el.closest && el.closest('select')) return 'select';
      if (el.tagName === 'INPUT') {
        const t = (el.type || 'text').toLowerCase();
        if (['date', 'month', 'week', 'time', 'datetime-local'].includes(t)) return 'date';
      }
      return null;
    }
    return kindAt(document, x, y, 0);
  }, [x, y]);
}

// Pauses every <video>/<audio> in every frame of the page, and keeps pausing
// any that starts for `guardMs` more (YouTube's player starts its own
// playback a moment after load): used while the client plays that video in
// BB10's native player, so this copy neither streams frames nor plays sound.
async function pauseAllMedia(page, guardMs) {
  await Promise.all(page.frames().map((f) => f.evaluate((ms) => {
    const stop = (e) => { if (e.target && e.target.pause) e.target.pause(); };
    document.addEventListener('play', stop, true); // media events don't bubble, but capture sees them
    setTimeout(() => document.removeEventListener('play', stop, true), ms);
    for (const m of document.querySelectorAll('video, audio')) m.pause();
  }, guardMs).catch(() => {})));
}

// Instagram reels for the client's native video player -- the same idea as
// its YouTube path, but resolved HERE: parth-dl is Python, and Instagram's
// CDN URLs (unlike YouTube's) are not tied to the IP that requested them, so
// the phone can play them even on mobile data. See igvideo.py.
const PARTHDL_PYTHON = process.env.PARTHDL_PYTHON || webVideo.venvPython(path.join(__dirname, '.venv-parthdl'));
const IG_HELPER = path.join(__dirname, 'igvideo.py');

function isInstagramVideoUrl(u) {
  try {
    const x = new URL(u);
    return x.protocol === 'https:' && /^(www\.|m\.)?instagram\.com$/i.test(x.hostname)
      && /^\/(?:[A-Za-z0-9._]+\/)?(?:reels?|p|tv)\/[A-Za-z0-9_-]+/.test(x.pathname);
  } catch (_) { return false; }
}

function resolveInstagram(url) {
  return new Promise((resolve) => {
    // execFile, no shell: the URL is a plain argv entry, never parsed by one.
    execFile(PARTHDL_PYTHON, [IG_HELPER, url], { timeout: 25000, maxBuffer: 1 << 20 }, (err, stdout) => {
      try {
        resolve(JSON.parse(String(stdout).trim().split('\n').pop()));
      } catch (_) {
        resolve({ ok: false, reason: err ? String(err.message).split('\n')[0] : 'bad helper output' });
      }
    });
  });
}

async function handle(s, body) {
  const { action, url, scrollY, tapX, tapY, deltaX, deltaY } = body;
  if (action === 'navigate') await closePopups(s); // a typed URL always targets the main tab
  let page = await usablePage(s, body);
  const z = s.zoom;
  let videoHit = null;

  if (action === 'navigate') {
    if (!url || !isHttp(url)) throw Object.assign(new Error('invalid url'), { status: 400 });
    try {
      await gotoWithRetry(page, url);
    } catch (e) {
      const st = proxyRef.current && proxyRef.current.state;
      if (st && Date.now() - st.lastBlockedAt < 5000 && /ERR_TUNNEL|ERR_PROXY|ERR_CONNECTION|ERR_HTTP_RESPONSE_CODE_FAILURE/.test(String(e.message))) {
        throw Object.assign(new Error('Address not allowed (private or local network)'), { status: 403 });
      }
      if (!/Download is starting/i.test(String(e.message))) {
        // let the error page finish loading so the NEXT navigation is not cancelled by it
        await page.waitForLoadState('domcontentloaded', { timeout: 1500 }).catch(() => {});
        throw e;
      }
      await new Promise((r) => setTimeout(r, 400)); // let the 'download' event land
    }
    // The client already plays this page's video natively ("Pagina" in its
    // player): show the page, but not a second, server-side copy of the video.
    if (body.pauseMedia) await pauseAllMedia(page, 8000);
    await settle(page, 3000);
  } else if (action === 'scroll') {
    const y = Number(scrollY) || 0;
    await page.evaluate((yy) => window.scrollTo(0, yy), y);
    await page.waitForTimeout(200);
  } else if (action === 'wheel') {
    // Wheel at a point, like a real mouse: scrolls whatever scrollable
    // element is under it (inner overflow containers), or the page itself.
    const clamp = (v) => Math.max(-3000, Math.min(3000, Number(v) || 0));
    let dx = clamp(deltaX) / z, dy = clamp(deltaY) / z;
    const st = page.__mb;
    if (body.align && st) {
      // frames=delta (see deltaframes.js): whole SCROLL_QUANTUM steps keep the
      // screencast JPEG's blocks aligned from frame to frame, so whatever only
      // moved is bit-identical and never resent. The remainder carries over
      // to the next step (and is dropped when the scroll settles).
      dx += st.wheelCarryX || 0;
      dy += st.wheelCarryY || 0;
      const qx = Math.trunc(dx / SCROLL_QUANTUM) * SCROLL_QUANTUM;
      const qy = Math.trunc(dy / SCROLL_QUANTUM) * SCROLL_QUANTUM;
      st.wheelCarryX = dx - qx;
      st.wheelCarryY = dy - qy;
      dx = qx;
      dy = qy;
    }
    if (dx || dy) {
      await withInput(st, async () => {
        await page.mouse.move((Number(tapX) || 0) / z, (Number(tapY) || 0) / z);
        await page.mouse.wheel(dx, dy);
      });
    }
    // Smooth scrolling is disabled at launch, so the wheel applies instantly --
    // no animation to wait out. A light step (WS, mid-scroll) returns right
    // away with just the metrics: its frames come from the screencast, and its
    // links would be stale by the next step (the settle refresh sends them).
    if (body.light) {
      page.__mb.scrolling = true;
      clearTimeout(page.__mb.scrollingTimer); // safety net if no settle ever comes
      page.__mb.scrollingTimer = setTimeout(() => { page.__mb.scrolling = false; }, 1500);
      const tab = await activeTab(s);
      const meta = await page.evaluate(() => ({
        scrollY: Math.round(window.scrollY), pageHeight: document.documentElement.scrollHeight,
      })).catch(() => ({}));
      return { image: null, links: [], editable: null, depth: tab.stack.length,
               url: page.url(), title: tab.title || '', tabs: tabsSummary(s), download: null, ...meta };
    }
    // A short pause just lets layout/repaint settle before we read back the
    // authoritative scrollY the client reconciles to.
    await page.waitForTimeout(30);
  } else if (action === 'tap') {
    const px = (Number(tapX) || 0) / z, py = (Number(tapY) || 0) / z;
    const kind = await elementKindAt(page, px, py).catch(() => null);
    if (kind === 'select' || kind === 'date') {
      // A real click would open the browser's own (invisible, not part of the
      // page/screenshot) native dropdown or calendar; focus it instead and let
      // the client show its own native picker (see 'select' action and the
      // date-like branch of 'type' below).
      await page.evaluate(([x, y]) => {
        const hit = document.elementFromPoint(x, y);
        const target = hit && (hit.closest('select') || hit);
        if (target && target.focus) target.focus();
      }, [px, py]).catch(() => {});
    } else if (!body.noVideo && (videoHit = await webVideo.videoAt(page, px, py).catch(() => null))) {
      // A video player: not clicked, so the page's own copy never starts --
      // the client plays it in BB10's player (see 'resolveVideo', web: true),
      // or sends this tap again with noVideo if that fails.
      videoHit.tapX = Number(tapX) || 0;
      videoHit.tapY = Number(tapY) || 0;
    } else {
      await withInput(page.__mb, () => page.mouse.click(px, py));
      page = await afterInteraction(s, body);
    }
  } else if (action === 'refresh') {
    // Nothing to do: applyView() above already re-laid the page out for a
    // new zoom / height; the screenshot below picks it up. A settle refresh
    // (end of a scroll) has no relayout to wait for.
    if (!body.settle) await page.waitForTimeout(250);
    else if (page.__mb) {
      page.__mb.scrolling = false;
      clearTimeout(page.__mb.scrollingTimer);
      page.__mb.wheelCarryX = page.__mb.wheelCarryY = 0;
    }
  } else if (action === 'pauseMedia') {
    // The client is playing this page's video in BB10's native player
    // (YouTube, see the client's playYoutube): stop the copy playing here.
    await pauseAllMedia(page, 4000);
    await page.waitForTimeout(100);
  } else if (action === 'type') {
    // Replace the content of the focused field, optionally submitting it.
    const text = String(body.text == null ? '' : body.text).slice(0, 5000);
    const info = await focusedEditable(page);
    if (info && DATE_LIKE_TYPES.includes(info.type)) {
      // fill() sets the value directly, exactly what picking a date natively
      // would produce, without ever opening the (invisible-to-us) native
      // calendar widget. Requires "YYYY-MM-DD" etc, which is what the client sends.
      await page.locator(':focus').fill(text, { timeout: 3000 }).catch(() => {});
    } else if (info) {
      // Set the value directly via JS on the real focused element, drilling
      // through shadow roots and same-origin iframes exactly like
      // focusedEditable() does -- more reliable than simulated keyboard input,
      // which does not always reach into a nested (even same-origin) iframe.
      // The native property setter is used so React/Vue-style controlled
      // inputs (which override the plain .value setter) see the change too.
      const applied = await page.evaluate(([text]) => {
        function resolve(el, depth) {
          if (!el) return null;
          while (el.shadowRoot && el.shadowRoot.activeElement) el = el.shadowRoot.activeElement;
          if (el.tagName === 'IFRAME' && depth < 6) {
            let doc = null;
            try { doc = el.contentDocument; } catch (_) {}
            return doc ? resolve(doc.activeElement, depth + 1) : null;
          }
          return el;
        }
        const el = resolve(document.activeElement, 0);
        if (!el) return false;
        if (el.tagName === 'TEXTAREA' || el.tagName === 'INPUT') {
          const proto = el.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
          Object.getOwnPropertyDescriptor(proto, 'value').set.call(el, text);
          el.dispatchEvent(new Event('input', { bubbles: true }));
          el.dispatchEvent(new Event('change', { bubbles: true }));
          return true;
        }
        if (el.isContentEditable) {
          el.textContent = text;
          el.dispatchEvent(new Event('input', { bubbles: true }));
          return true;
        }
        return false;
      }, [text]);
      if (!applied) {
        // Not reachable from JS here (a cross-origin iframe, info.crossOrigin):
        // real keyboard input is dispatched to whatever has focus regardless
        // of frame/process, so it is the only way left to reach it.
        await page.keyboard.press('ControlOrMeta+A');
        if (text) await page.keyboard.insertText(text);
        else await page.keyboard.press('Delete');
      }
    }
    if (body.submit) {
      await page.keyboard.press('Enter');
      page = await afterInteraction(s, body);
    }
  } else if (action === 'select') {
    // Sets a <select>'s value(s) directly (dispatches its change event, like a
    // real pick would), again without opening the native dropdown widget.
    const values = Array.isArray(body.values) ? body.values.map(String) : [];
    try {
      await page.locator(':focus').selectOption(values, { timeout: 3000 });
      // Some selects navigate on change (mobile "jump menu" language/site pickers).
      await page.waitForLoadState('domcontentloaded', { timeout: 1000 }).catch(() => {});
      await settle(page, 1200);
    } catch (e) { console.warn('select action failed:', String(e.message).split(String.fromCharCode(10))[0]); }
  } else if (action === 'newtab') {
    const withUrl = !!(url && isHttp(url));
    // A link from another app (the client's openInvokedUrl) brings its url.
    // If the active tab is still blank -- the app was just cold-started by
    // that very link -- it takes the link instead of leaving an empty tab behind.
    if (!(withUrl && page.url() === 'about:blank')) {
      if (s.tabs.length >= MAX_TABS) {
        throw Object.assign(new Error(`Too many open tabs (maximum ${MAX_TABS})${withUrl ? ': close one and open the link again' : ''}`), { status: 400 });
      }
      const ctx = await getContext(s);
      const tab = newTab(s);
      s.activeTabId = tab.id;
      trackPage(s, tab, await ownNewPage(s, ctx));
      page = await usablePage(s, body); // applies zoom/viewport to the new page
    }
    if (withUrl) {
      await gotoWithRetry(page, url).catch(() => {});
      await settle(page, 3000);
    }
  } else if (action === 'switchtab') {
    if (!body.tabId) throw Object.assign(new Error('tabId required'), { status: 400 });
    s.activeTabId = findTab(s, body.tabId).id;
    page = await usablePage(s, body);
  } else if (action === 'closetab') {
    if (!body.tabId) throw Object.assign(new Error('tabId required'), { status: 400 });
    const closing = findTab(s, body.tabId);
    const idx = s.tabs.indexOf(closing);
    for (const p of closing.stack.slice().reverse()) await p.close().catch(() => {});
    // (a popup tab may already be gone: its pages' close handler drops it)
    if (s.tabs.indexOf(closing) >= 0) s.tabs.splice(s.tabs.indexOf(closing), 1);
    if (s.tabs.length === 0 || s.tabs.every((t) => t.stack.length === 0)) {
      // Never end up with zero tabs: replace the last one with a fresh blank tab.
      const ctx = await getContext(s);
      s.tabs.length = 0;
      const fresh = newTab(s);
      trackPage(s, fresh, await ownNewPage(s, ctx));
    }
    if (s.activeTabId === closing.id || !s.tabs.some((t) => t.id === s.activeTabId)) {
      s.activeTabId = s.tabs[Math.min(idx, s.tabs.length - 1)].id;
    }
    page = await usablePage(s, body);
  } else if (action === 'poll') {
    // No page mutation: just grabs the next frame, for the client's optional
    // continuous "video mode" -- a tight loop of these,
    // one in flight at a time, chained by the client. Link collection is
    // skipped below (nothing tappable changes between polls; a real tap is
    // its own action and recomputes them normally).
  } else {
    throw Object.assign(new Error('invalid action'), { status: 400 });
  }

  const tab = await activeTab(s);
  const st = page.__mb;
  const out = await retryOnNav(page, async () => {
    const [image, links, meta] = await Promise.all([
      // Straight from CDP: Playwright's own screenshot ignores the
      // deviceScaleFactor set via Emulation.setDeviceMetricsOverride.
      captureJpeg(st),
      action === 'poll' ? Promise.resolve([]) : collectLinks(page),
      page.evaluate(() => ({
        scrollY: Math.round(window.scrollY),
        pageHeight: document.documentElement.scrollHeight,
      })),
    ]);
    const editable = action === 'tap' ? await focusedEditable(page) : null;
    const scaled = links.map((l) => ({
      url: l.url, x: Math.round(l.x * z), y: Math.round(l.y * z),
      w: Math.round(l.w * z), h: Math.round(l.h * z),
    }));
    return { image, links: scaled, editable, depth: tab.stack.length,
             url: page.url(), title: await page.title(), ...meta };
  });
  // Cache this tab's title/url so listing all tabs (tabsSummary) never needs a
  // round trip into inactive tabs' own renderers.
  tab.title = out.title;
  tab.url = out.url;
  out.tabs = tabsSummary(s);
  out.download = s.newDownloads.shift() || null; // outside the retry: consumed once
  out.video = videoHit;
  return out;
}

async function handleRequest(req, res, raw) {
  try {
    const body = req.body || {};
    if (!body.sessionId || typeof body.sessionId !== 'string') {
      return res.status(400).json({ error: 'sessionId required' });
    }
    const r = await enqueue(body.sessionId, async (s) => {
      const out = await handle(s, body);
      // Cookies change on navigation/clicks/typing (logins!), not on scrolling:
      // save right after those so a crash or hard kill loses nothing.
      if (['navigate', 'tap', 'type'].includes(body.action) || Date.now() - s.lastSave > 5000) await saveState(s);
      return out;
    });
    if (raw) {
      res.set('Content-Type', 'image/jpeg');
      res.set('X-Links', Buffer.from(JSON.stringify(r.links)).toString('base64'));
      res.set('X-Page-Url', encodeURIComponent(r.url));
      res.set('X-Scroll-Y', String(r.scrollY));
      res.set('X-Page-Height', String(r.pageHeight));
      return res.send(r.image);
    }
    res.json({
      imageJpeg: r.image.toString('base64'),
      url: r.url, title: r.title, scrollY: r.scrollY, pageHeight: r.pageHeight,
      links: r.links, editable: r.editable, depth: r.depth, download: r.download,
      tabs: r.tabs,
    });
  } catch (e) {
    res.status(e.status || 500).json({ error: String(e.message || e) });
  }
}

const TOKEN = loadToken(__dirname);
const GUARD = makeGuard(TOKEN.token);
const app = express();
app.disable('x-powered-by');
app.use(express.json({ limit: '64kb' }));
app.use('/api', GUARD.requireToken); // covers /api, /api/image, /api/download; not /admin (localhost only)
app.post('/api', (req, res) => handleRequest(req, res, false));
// Used by login.js after it saved a fresh login for a session: forget the live
// context WITHOUT saving it (that would overwrite the new file), so the next
// request reloads the saved state. localhost only.
app.post('/admin/drop', (req, res) => {
  const ip = req.socket.remoteAddress;
  if (!['::1', '127.0.0.1', '::ffff:127.0.0.1'].includes(ip)) {
    return res.status(403).json({ error: 'localhost only' });
  }
  const s = sessions.get((req.body || {}).sessionId);
  if (s) {
    sessions.delete(s.id);
    s.queue = s.queue.then(async () => {
      try { const c = await s.ctxPromise; if (c) await c.close(); } catch (_) {}
    });
  }
  res.json({ ok: true });
});

// Hands a pending download to the client, once. The file is first completed on
// this PC so its size is known: over the limit -> 413 and nothing is sent.
app.post('/api/download', async (req, res) => {
  const { sessionId, id, cancel } = req.body || {};
  const s = sessions.get(sessionId);
  const d = s && s.downloads.get(id);
  if (!d) return res.status(404).json({ error: 'download not found or expired' });
  s.downloads.delete(id); // one-shot
  try {
    if (cancel) {
      await d.download.cancel().catch(() => {});
      return res.json({ ok: true });
    }
    const file = await d.download.path();
    const failure = await d.download.failure();
    if (!file || failure) return res.status(502).json({ error: 'download failed: ' + (failure || 'no file') });
    const size = fs.statSync(file).size;
    if (size > MAX_DOWNLOAD_BYTES) {
      await d.download.delete().catch(() => {});
      const mb = Math.round(size / 1048576);
      return res.status(413).json({ error: `File too large (${mb} MB, maximum ${MAX_DOWNLOAD_BYTES / 1048576} MB)` });
    }
    res.set({
      'Content-Type': 'application/octet-stream',
      'Content-Length': String(size),
      'Cache-Control': 'no-store',
    });
    const stream = fs.createReadStream(file);
    stream.on('error', () => res.destroy());
    res.on('close', () => { d.download.delete().catch(() => {}); });
    stream.pipe(res);
  } catch (e) {
    res.status(500).json({ error: String(e.message || e) });
  }
});

setInterval(() => {
  const now = Date.now();
  for (const s of sessions.values()) {
    for (const [id, d] of s.downloads) {
      if (now - d.created > DOWNLOAD_TTL_MS) {
        s.downloads.delete(id);
        d.download.cancel().catch(() => {});
      }
    }
  }
}, 60 * 1000).unref();

app.post('/api/image', (req, res) => handleRequest(req, res, true));
// Streams of videos played natively (see webvideo.js): capability URLs, no
// token.
webVideo.installMediaRelay(app, { allowPrivate: ALLOW_PRIVATE });

// --- WebSocket transport (/ws) ---------------------------------------------
// One persistent connection per client instead of one HTTP request per
// action: on BB10 the HTTPS side is BBport's mbedTLS transport, which opens
// a brand-new TCP+TLS 1.2 connection for every single request (BB10's system
// TLS can't be pointed at a modern server directly -- see the client's
// wsclient.cpp) -- that repeated handshake, not payload size or app logic,
// was the real reason scrolling/typing never felt smooth no matter how the
// client-side request logic was tuned. A WebSocket pays that handshake once
// per session. Screencast (ensureScreencast/subscribeToPage above) then
// fills the time BETWEEN actions with real frames as Chromium repaints,
// which is what gives a scroll or a page load its "continuous video" feel
// instead of one screenshot per discrete action.
//
// Action messages ({type:'action', action, ...}) reuse handle() completely
// unchanged -- only the transport differs from the existing /api path, the
// browser-control logic is identical -- except 'wheel', which a burst of
// touch-move events turns into many small messages: those are coalesced
// server-side (scheduleScroll/flushScroll below) into one handle() call per
// burst instead of one per message, same idea as the debounce a mouse wheel
// gets in a real desktop browser.
const wss = new WebSocketServer({ noServer: true });

wss.on('connection', (ws, sessionId, flow, peerIp) => {
  // Page sound (pageaudio.js) for clients that asked for it with ?audio=1.
  if (flow && flow.audio) pageAudio.addListener(ws);
  let pendingFrame = null;
  let sendingFrame = false;
  // flow=ack clients confirm every binary frame once decoded ({type:'ack'}).
  // ws.send's callback alone only means "handed to the kernel": TCP buffers,
  // the tunnel and the phone's socket thread all soak frames up long before
  // the phone has actually decoded them, so without acks nothing here ever
  // noticed a phone falling behind (a playing video: ~20 fps of 40+ KB
  // frames) and every frame got through, piling up on the phone's UI thread.
  const ackFlow = !!(flow && flow.ack);
  // frames=delta clients (see deltaframes.js) get the shift of the previous
  // frame plus only the changed rectangles whenever that is smaller. The
  // encoder tracks what the client has, so it runs at SEND time, on frames in
  // the order they actually go out (never on ones superseded meanwhile).
  const delta = flow && flow.delta ? createDeltaEncoder() : null;
  let lastPushed = null; // newest frame, resent whole when the client asks
  let unacked = 0;
  // Round-trip diagnostics (pm2 log): send time of each unacked frame, oldest first.
  const sentAt = [];
  let rttSum = 0, rttCount = 0, sentCount = 0, statsSince = Date.now();
  let deltaCount = 0, sameCount = 0, sentBytes = 0, encodeMs = 0;
  let stallTimer = null;
  function flushFrame() {
    if (sendingFrame || pendingFrame === null || ws.readyState !== ws.OPEN) return;
    if (ackFlow && unacked >= FRAME_WINDOW) {
      // Safety valve: a client that stops acking must not freeze the view.
      if (!stallTimer) stallTimer = setTimeout(() => { stallTimer = null; unacked = 0; sentAt.length = 0; flushFrame(); }, 1500);
      return;
    }
    sendingFrame = true;
    const frame = pendingFrame;
    pendingFrame = null;
    const t0 = Date.now();
    const encoded = delta
      ? delta.encode(frame).catch((e) => {
        console.warn(`[frames] delta encoding failed, sending whole: ${e.message}`);
        delta.reset();
        return { kind: 'key', data: frame.buf };
      })
      : Promise.resolve({ kind: 'key', data: frame.buf });
    encoded.then((out) => {
      encodeMs += Date.now() - t0;
      if (out.data === null || ws.readyState !== ws.OPEN) {
        if (out.data === null) sameCount++; // identical to what the client shows
        sendingFrame = false;
        flushFrame();
        return;
      }
      if (out.kind === 'delta') deltaCount++;
      sentBytes += out.data.length;
      if (ackFlow) { unacked++; sentAt.push(Date.now()); }
      sentCount++;
      ws.send(out.data, { binary: true }, () => { sendingFrame = false; flushFrame(); });
    });
  }
  function onFrameAck() {
    const t = sentAt.shift();
    if (t) { rttSum += Date.now() - t; rttCount++; }
    if (Date.now() - statsSince >= 10000) {
      if (rttCount >= 20) {
        console.log(`[frames] ${sessionId.slice(0, 8)}: ${(sentCount * 1000 / (Date.now() - statsSince)).toFixed(1)} sent/s, ` +
                    `send->ack avg ${Math.round(rttSum / rttCount)} ms (window ${FRAME_WINDOW})` +
                    (delta ? `, ${deltaCount} deltas + ${sentCount - deltaCount} whole, ${sameCount} unchanged skipped, ` +
                             `avg ${(sentBytes / 1024 / Math.max(1, sentCount)).toFixed(1)} KB, ` +
                             `encode avg ${(encodeMs / Math.max(1, sentCount + sameCount)).toFixed(1)} ms` : ''));
      }
      rttSum = rttCount = sentCount = 0;
      deltaCount = sameCount = sentBytes = encodeMs = 0;
      statsSince = Date.now();
    }
    if (unacked > 0) unacked--;
    if (stallTimer) { clearTimeout(stallTimer); stallTimer = null; }
    flushFrame();
  }
  // Only the newest undelivered frame is ever kept: if screencast frames
  // arrive faster than this connection can actually send them (a slow phone
  // link, or the ack window above being full), older ones are simply
  // superseded rather than queued -- the client should always be looking at
  // the freshest frame available, never working through a backlog.
  function pushFrame(buf, meta) {
    pendingFrame = { buf, meta: meta || null };
    lastPushed = pendingFrame;
    flushFrame();
  }

  function sendJson(obj) {
    if (ws.readyState === ws.OPEN) ws.send(JSON.stringify(obj));
  }

  // Tracks which page's screencast this connection currently rides, so a tab
  // switch / popup open-close moves the subscription instead of leaving it
  // stuck on a page the client can no longer see.
  let subscribedPage = null;
  const subscribedCb = (buf, meta) => pushFrame(buf, meta);
  async function resyncSubscription(s) {
    const page = await activePage(s).catch(() => null);
    if (page === subscribedPage) return;
    if (subscribedPage) unsubscribeFromPage(subscribedPage, subscribedCb);
    subscribedPage = page;
    if (page) await subscribeToPage(page, subscribedCb).catch(() => {});
  }

  // --- scroll coalescing: many small 'wheel' messages -> one handle() call ---
  // At most ONE scroll step in flight: deltas arriving meanwhile pile up and go
  // out together the moment it completes. A fixed flush timer queued steps
  // faster than they ran at zoom != 1 (each waits its turn for a capture), and
  // the page kept scrolling ~2 s after the finger stopped (measured 2026-10-06).
  // Steps are 'light' (see handle): the frames come from the screencast anyway,
  // and the links -- stale by the next step -- come once, when scrolling stops.
  let scrollAccumX = 0, scrollAccumY = 0, scrollX = null, scrollY = null, scrollTimer = null;
  let scrollInFlight = false, settleTimer = null;
  let lastZoom, lastViewHeight;
  function flushScroll() {
    scrollTimer = null;
    if (scrollAccumX === 0 && scrollAccumY === 0) return;
    if (scrollInFlight) return; // picked up as soon as the step in flight completes
    const body = {
      action: 'wheel', sessionId, tapX: scrollX, tapY: scrollY, light: true, align: !!delta,
      deltaX: scrollAccumX, deltaY: scrollAccumY, zoom: lastZoom, viewHeight: lastViewHeight,
    };
    scrollAccumX = 0; scrollAccumY = 0; scrollX = null; scrollY = null;
    scrollInFlight = true;
    runAction(body).then(() => {
      scrollInFlight = false;
      if (scrollAccumX !== 0 || scrollAccumY !== 0) flushScroll();
      else scheduleSettle();
    });
  }
  function scheduleScroll(body) {
    if (Number.isFinite(Number(body.tapX))) scrollX = Number(body.tapX);
    if (Number.isFinite(Number(body.tapY))) scrollY = Number(body.tapY);
    scrollAccumX += Number(body.deltaX) || 0;
    scrollAccumY += Number(body.deltaY) || 0;
    lastZoom = body.zoom;
    lastViewHeight = body.viewHeight;
    if (settleTimer) { clearTimeout(settleTimer); settleTimer = null; }
    // A short window only to batch the samples of one touch-move burst.
    if (!scrollTimer && !scrollInFlight) scrollTimer = setTimeout(flushScroll, 20);
  }
  // The finger has stopped: one full result (screenshot, links, metrics).
  function scheduleSettle() {
    if (settleTimer) clearTimeout(settleTimer);
    settleTimer = setTimeout(() => {
      settleTimer = null;
      runAction({ action: 'refresh', sessionId, settle: true, zoom: lastZoom, viewHeight: lastViewHeight });
    }, 200);
  }

  async function runAction(body) {
    try {
      const r = await enqueue(sessionId, async (s) => {
        const out = await handle(s, body);
        if (['navigate', 'tap', 'type'].includes(body.action) || Date.now() - s.lastSave > 5000) await saveState(s);
        return out;
      });
      const s = sessions.get(sessionId);
      if (s) await resyncSubscription(s);
      if (r.image) pushFrame(r.image); // light scroll steps carry none
      sendJson({
        type: 'result', action: body.action, url: r.url, title: r.title,
        scrollY: r.scrollY, pageHeight: r.pageHeight, links: r.links,
        editable: r.editable, depth: r.depth, download: r.download, tabs: r.tabs, video: r.video,
      });
    } catch (e) {
      sendJson({ type: 'error', action: body.action, message: String(e.message || e), status: e.status || 500 });
    }
  }

  ws.on('message', (data, isBinary) => {
    if (isBinary) return; // the client only ever sends JSON text
    let msg;
    try { msg = JSON.parse(data.toString()); } catch (_) { return; }
    if (msg.type === 'ping') { sendJson({ type: 'pong' }); return; }
    if (msg.type === 'ack') { onFrameAck(); return; }
    if (msg.type === 'keyframe') {
      // The client couldn't apply a delta: start over from a whole frame.
      if (delta) delta.reset();
      if (!pendingFrame && lastPushed) pendingFrame = lastPushed;
      flushFrame();
      return;
    }
    if (msg.type === 'resolveVideo') {
      // Not a page action (the page isn't touched): outside the session queue,
      // so a slow lookup never holds up taps/scrolls. The url is echoed back so
      // the client can tell a stale answer from the one it's waiting for.
      const url = String(msg.url || '');
      if (msg.web && isHttp(url)) { resolveWeb(url, String(msg.pageUrl || url)); return; }
      if (!isInstagramVideoUrl(url)) { sendJson({ type: 'videoInfo', url, ok: false, reason: 'unsupported url' }); return; }
      resolveInstagram(url).then((r) => {
        if (!r.ok) console.warn(`[video] ${url}: ${r.reason}`);
        sendJson({ ...r, type: 'videoInfo', url, videoUrl: r.url });
      });
      return;
    }
    if (msg.type !== 'action' || !msg.action) return;
    if (msg.action !== 'wheel' && settleTimer) { clearTimeout(settleTimer); settleTimer = null; } // its own result has the links
    const body = { ...msg, sessionId };
    delete body.type;
    if (body.action === 'wheel') { scheduleScroll(body); return; }
    // A real action supersedes whatever scroll burst hasn't flushed yet
    // (e.g. the finger lifted and immediately tapped something else).
    if (scrollTimer) { clearTimeout(scrollTimer); flushScroll(); }
    runAction(body);
  });

  // A video in an ordinary page (see webvideo.js): yt-dlp or the page itself
  // finds the stream; the phone gets it direct when it shares this server's
  // public IP (home Wi-Fi: IP-bound CDN URLs work) and the CDN takes a request
  // without the site's headers, else through the media relay -- and the other
  // one as the fallback if that doesn't play.
  async function resolveWeb(url, pageUrl) {
    try {
      const s = getEntry(sessionId);
      const ctx = await getContext(s);
      const page = await activePage(s).catch(() => null);
      const r = await webVideo.resolveWebVideo(ctx, page, url, pageUrl);
      if (!r.ok) {
        console.warn(`[video] ${url}: ${r.reason}`);
        sendJson({ type: 'videoInfo', url, ok: false, reason: r.reason });
        return;
      }
      const id = webVideo.addRelay(r.url, await webVideo.relayHeaders(ctx, r));
      const relayPath = `/media/${id}/video.${r.protocol === 'hls' ? 'm3u8' : 'mp4'}`;
      // At home (same public IP): the CDN itself if it takes the phone's bare
      // request -- spares the tunnel, which a video can stall along with this
      // very WebSocket. Otherwise the relay, the CDN as the long shot.
      const home = !!peerIp && peerIp === await webVideo.serverPublicIp();
      let first = relayPath, second = r.url, how = 'relay';
      if (home && await webVideo.probeDirect(r.url)) { first = r.url; second = relayPath; how = 'direct'; }
      console.log(`[video] ${r.extractor || '?'} ${r.height || '?'}p ${r.protocol} -> ${how} first`);
      sendJson({
        type: 'videoInfo', url, ok: true, title: r.title, width: r.width, height: r.height,
        videoUrl: first, fallbackUrl: second,
      });
    } catch (e) {
      sendJson({ type: 'videoInfo', url, ok: false, reason: String(e.message || e) });
    }
  }

  ws.on('close', () => {
    pageAudio.removeListener(ws);
    if (scrollTimer) clearTimeout(scrollTimer);
    if (settleTimer) clearTimeout(settleTimer);
    if (stallTimer) clearTimeout(stallTimer);
    if (subscribedPage) unsubscribeFromPage(subscribedPage, subscribedCb);
  });

  ws.on('error', (err) => console.warn(`[ws] ${sessionId}:`, err.message));
});

function handleWsUpgrade(req, socket, head) {
  let pathname = '/', query = new URLSearchParams();
  try {
    const u = new URL(req.url, 'http://localhost');
    pathname = u.pathname;
    query = u.searchParams;
  } catch (_) {}

  if (pathname !== '/ws') {
    socket.write('HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n');
    socket.destroy();
    return;
  }

  const m = /^Bearer\s+(.+)$/i.exec(req.headers.authorization || '');
  const auth = GUARD.check(clientIp(req), m ? m[1] : '');
  if (!auth.ok) {
    // 429 while this address is locked out (see guard.js), so the client can
    // tell "wait" from "wrong token" and stop retrying a bad one.
    socket.write(auth.retryAfterSec
      ? `HTTP/1.1 429 Too Many Requests\r\nRetry-After: ${auth.retryAfterSec}\r\nConnection: close\r\n\r\n`
      : 'HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n');
    socket.destroy();
    return;
  }

  const sessionId = query.get('sessionId');
  if (!sessionId) {
    socket.write('HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n');
    socket.destroy();
    return;
  }

  // flow=ack: the client confirms each frame (see the connection handler);
  // older clients don't send it and keep the unpaced behavior.
  // frames=delta: scroll-copy deltas, see deltaframes.js.
  const flow = { ack: query.get('flow') === 'ack', delta: query.get('frames') === 'delta' && deltaAvailable,
                 audio: query.get('audio') === '1' };
  wss.handleUpgrade(req, socket, head, (ws) => wss.emit('connection', ws, sessionId, flow, clientIp(req)));
}

const server = app.listen(PORT, HOST, () => {
  console.log(`Feather server listening on ${HOST}:${PORT}`);
  // Optional features, see the README: what this server can do.
  const yes = (b) => (b ? 'on' : 'off');
  console.log(`Optional: scroll deltas ${yes(deltaAvailable)} (sharp + jpegtran), ` +
              `Instagram reels ${yes(fs.existsSync(PARTHDL_PYTHON))} (parth-dl), ` +
              `web videos ${yes(webVideo.ytdlpAvailable)} (yt-dlp), ` +
              `page sound ${yes(pageAudio.audioAvailable)} (parec + opusscript)`);
  if (TOKEN.created) {
    console.log('');
    console.log('  New access token (saved in server/token.txt):');
    console.log('    ' + TOKEN.token);
    console.log('  Enter it in the app: Settings, under the server URL.');
    console.log('');
  } else {
    console.log('Access token: server/token.txt (or the FEATHER_TOKEN variable)');
  }
  if (HOST !== '127.0.0.1' && HOST !== 'localhost' && HOST !== '::1') {
    console.warn(`WARNING: listening on ${HOST}, reachable from the network: the token is the only protection.`);
  }
  if (ALLOW_PRIVATE) console.warn('WARNING: --allow-private is on: the browser can reach the local network.');
});
server.on('upgrade', handleWsUpgrade);

async function shutdown() {
  server.close();
  const entries = [...sessions.values()];
  for (const id of [...sessions.keys()]) closeEntry(id);
  await Promise.all(entries.map((s) => s.queue));
  try { (await browserPromise)?.close(); } catch (_) {}
  process.exit(0);
}
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
