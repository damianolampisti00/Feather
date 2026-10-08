# Feather server

The server opens web pages in a headless Chromium (through Playwright) and streams them
to the Feather app as JPEG frames over a WebSocket. Run it on a machine you control:
a Linux box, a Windows PC or a small VPS.

## 1. Install

Requirements: [Node.js](https://nodejs.org/) 18 or newer.

```
cd server
npm install
npx playwright install chromium
```

On Linux, if Chromium complains about missing libraries, `sudo npx playwright install-deps chromium`
installs them on Debian/Ubuntu. On other distributions (Arch, Fedora...) the simplest
route is the system's own Chromium or Chrome: set `BROWSER_PATH=/usr/bin/chromium` or
`BROWSER_CHANNEL=chrome` (see [Settings](#settings)).

## 2. First start and the access token

```
node server.js
```

On first start the server generates an **access token**, prints it and saves it in
`token.txt` (readable only by you). Enter it in the app: top menu > **Settings** >
Token, under the server URL. Without the right token every request is refused, and an
address that gets it wrong 10 times is blocked for 15 minutes.

The startup log also lists which [optional features](#3-optional-features) are on.

## 3. Optional features

Feather works without any of these; each one adds something when present.

| Feature | Needs | Without it |
|---|---|---|
| Scroll deltas: while scrolling only the new strip of the page is sent | `sharp` (installed by `npm install`) and `jpegtran` | Whole frames while scrolling |
| Videos embedded in web pages, played in the phone's native player | yt-dlp in `.venv-ytdlp` | A tap on the video uses the page's own player |
| Instagram reels in the native player | parth-dl in `.venv-parthdl` | Reels play in the page |

YouTube videos play in the native player without any of these: the app asks YouTube
directly.

**jpegtran** comes with libjpeg-turbo:

- Debian/Ubuntu: `sudo apt install libjpeg-turbo-progs`
- Arch: `sudo pacman -S libjpeg-turbo`
- macOS: `brew install jpeg-turbo`
- Windows: install [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo/releases)
  and set `JPEGTRAN_PATH` to its `bin\jpegtran.exe`, or add that folder to `PATH`.

**yt-dlp and parth-dl** (Python 3.9 or newer) go in their own virtual environments,
inside `server/`:

```
# Linux / macOS
python3 -m venv .venv-ytdlp   && .venv-ytdlp/bin/pip install "yt-dlp[default,curl-cffi]"
python3 -m venv .venv-parthdl && .venv-parthdl/bin/pip install parth-dl

# Windows
py -m venv .venv-ytdlp   && .venv-ytdlp\Scripts\pip install "yt-dlp[default,curl-cffi]"
py -m venv .venv-parthdl && .venv-parthdl\Scripts\pip install parth-dl
```

Sites change often: update yt-dlp every now and then
(`.venv-ytdlp/bin/pip install -U "yt-dlp[default,curl-cffi]"`), or videos stop being found.
`YTDLP_PYTHON` and `PARTHDL_PYTHON` point to another Python that has them.

Some video streams only play when fetched through the server (they are tied to its IP
address or need the site's headers): the server then relays them at
`/media/<random id>/...`. These addresses are unguessable and expire 3 hours after last
use, but they need no token (the phone's video player can't send one).

## 4. Reaching the server from the phone

The server listens on `127.0.0.1:8080` only. Choose one:

**Same Wi-Fi.** Start it with `node server.js --lan` (or `HOST=0.0.0.0`) and set the
app's server URL to `http://<server's LAN IP>:8080/api`. Traffic is not encrypted, so
use this only on a network you trust.

**From anywhere, with a Cloudflare Tunnel** (free; HTTPS without opening ports):

```
cloudflared tunnel --url http://127.0.0.1:8080
```

prints a temporary `https://….trycloudflare.com` address that changes at every start:
fine for a try. For a fixed address, create a named tunnel on a domain of yours
(`cloudflared tunnel login`, `cloudflared tunnel create feather`,
`cloudflared tunnel route dns feather feather.example.com`, a config with
`service: http://127.0.0.1:8080`, then `cloudflared tunnel run feather`, or
`cloudflared service install` to run it at boot). The app's server URL is then
`https://feather.example.com/api`.

Any other reverse proxy with a valid certificate works too (Caddy, nginx...), as long as
it passes WebSocket upgrades on `/ws`. Cloudflare Access or other login pages in front
of the server are not supported: the app can't sign in to them.

**Check the token works before you expose the server to the internet.**

## 5. Keep it running with pm2

```
npm install -g pm2
pm2 start ecosystem.config.js
pm2 save
pm2 startup        # then run the command it prints, to start at boot
pm2 logs feather
```

Settings go in the `env` section of `ecosystem.config.js`.

## 6. Signing in to Google and similar sites

Google (and a few others) refuse sign-in inside an automated browser. Sign in once in a
normal browser window on the server instead:

```
node login.js <session id> [start URL]
```

The session ID is shown in the app's Settings. A window opens: sign in (to as many sites
as you like), close it or come back to the terminal and press Enter. The login is saved
in `sessions/<session id>.json` and the phone's next page load uses it.

This needs a desktop on the server. On a headless server, run `login.js` on another
computer with the same files (`npm install` there too), then copy
`sessions/<session id>.json` into the server's `sessions/` folder. That file holds your
login cookies: treat it like a password.

## Settings

Environment variables (all optional):

| Variable | Default | Meaning |
|---|---|---|
| `PORT` | `8080` | Listening port |
| `HOST` | `127.0.0.1` | Listening address (`--lan` = `0.0.0.0`) |
| `FEATHER_TOKEN` | from `token.txt` | Access token |
| `BROWSER_PATH` | | A specific Chromium/Chrome executable |
| `BROWSER_CHANNEL` | `msedge` on Windows | An installed browser: `chrome`, `msedge`; unset elsewhere = Playwright's Chromium |
| `MAX_SESSIONS` | `3` | Phones served at once |
| `MAX_TABS` | `6` | Tabs per phone |
| `JPEG_QUALITY` | `40` | Frame quality (higher = sharper and bigger) |
| `ALLOW_PORTS` | | Extra ports the browser may reach, e.g. `3000,8000` (always: 80, 443, 8080, 8443) |
| `ALLOW_PRIVATE` | | `1` (or `--allow-private`) lets the browser reach local networks: development only |
| `JPEGTRAN_PATH` | `jpegtran` | jpegtran executable |
| `YTDLP_PYTHON`, `PARTHDL_PYTHON` | the venvs above | Python with yt-dlp / parth-dl |

A phone's session (its tabs and browser) closes after 10 minutes without activity;
logins and cookies stay saved in `sessions/`.

## Security

| | |
|---|---|
| Access token | Required on every request and on the WebSocket; 10 wrong attempts block the address for 15 minutes |
| Listening | `127.0.0.1` unless `--lan` / `HOST` |
| Local networks | The browser goes through a built-in proxy that resolves each name and refuses loopback, private and link-local addresses and cloud metadata, then connects to the checked IP (no DNS rebinding, no redirects inward). Only ports 80, 443, 8080, 8443 unless `ALLOW_PORTS` |
| Files not to share | `token.txt`, `sessions/` (your logins), `downloads/` |

Known limits: whoever has the token can browse through your server, with its saved
logins, and use its bandwidth and CPU. There is no per-second rate limit. Keep the token
private and change it (delete `token.txt` and restart) if it leaks.
