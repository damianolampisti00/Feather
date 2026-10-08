'use strict';
// One-off helper: sign in to a site (e.g. Google) in a *real, visible* browser
// on this PC, then hand that login to a Feather session.
//
// Google refuses sign-in inside an automated/headless browser ("This browser
// or app may not be secure"), but accepts a normal Chrome/Edge window. So the
// password is typed here, once; the server then just reuses the saved cookies.
//
//   node login.js <sessionId> [startUrl]
//
// <sessionId> is shown in the app's Settings (Session ID).
// Env: BROWSER_PATH / BROWSER_CHANNEL as for the server (default: Edge on
// Windows, else Playwright's Chromium), PORT = server port (8080).
// Run it on the server machine, with a desktop session (the window is visible).
const fs = require('fs');
const path = require('path');
const readline = require('readline');
const { chromium } = require('playwright');

const id = process.argv[2];
const startUrl = process.argv[3] || 'https://accounts.google.com/';
if (!id) {
  console.error('usage: node login.js <sessionId> [startUrl]');
  process.exit(1);
}

const STATE_DIR = path.join(__dirname, 'sessions');
fs.mkdirSync(STATE_DIR, { recursive: true });
const file = path.join(STATE_DIR, id.replace(/[^A-Za-z0-9_-]/g, '_').slice(0, 64) + '.json');

(async () => {
  const browser = await chromium.launch({
    ...(process.env.BROWSER_PATH ? { executablePath: process.env.BROWSER_PATH }
      : { channel: process.env.BROWSER_CHANNEL || (process.platform === 'win32' ? 'msedge' : undefined) }),
    headless: false,
    ignoreDefaultArgs: ['--enable-automation'],
    args: ['--disable-blink-features=AutomationControlled'],
  });
  const opts = { viewport: null }; // a normal window, not the 720px phone view
  if (fs.existsSync(file)) opts.storageState = file; // keep what the session already has
  const context = await browser.newContext(opts);
  const page = await context.newPage();
  await page.goto(startUrl).catch(() => {});

  let closed = false;
  browser.on('disconnected', () => { closed = true; });
  const save = async () => {
    if (closed) return;
    try { await context.storageState({ path: file }); } catch (_) {}
  };
  const timer = setInterval(save, 3000); // closing the window by hand loses nothing

  console.log('');
  console.log('Sign in in the window that just opened (several sites, if you like).');
  console.log('When you are done, come back here and press Enter.');
  await new Promise((resolve) => {
    const rl = readline.createInterface({ input: process.stdin });
    rl.once('line', resolve);
    rl.once('close', resolve);
    browser.on('disconnected', resolve);
  });
  clearInterval(timer);
  await save();
  await browser.close().catch(() => {});
  console.log('Session saved in ' + file);

  // If the server is running with this session in memory, make it drop it so the
  // next request reloads the file (it would otherwise overwrite it with its own).
  try {
    const r = await fetch(`http://localhost:${process.env.PORT || 8080}/admin/drop`, {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sessionId: id }),
    });
    console.log(r.ok ? 'Server updated: the next page load uses the new login.' : 'Server: ' + r.status);
  } catch (_) {
    console.log('Server not reachable: it will use the saved login when it starts.');
  }
  process.exit(0);
})().catch((e) => {
  console.error(String(e.message || e));
  process.exit(1);
});
