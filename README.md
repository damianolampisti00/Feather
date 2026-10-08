# Feather

A light web browser for BlackBerry 10 phones that works on today's web.
Made for and tested on the Q5; the Q10 has the same 720x720 screen. Other BB10
phones may work but are untested.

BB10's own browser is stuck with a 2015 engine and TLS stack: most sites no longer
load or are painfully slow. Feather sidesteps that. A small server you run yourself
(a PC, a home server, a VPS) opens the pages in a real, current Chromium and sends
the phone **pictures of the page**; the phone sends back your taps, scrolls and
typing. The phone only has to show JPEG frames, so even a Q5 browses modern sites
smoothly.

```
 BlackBerry 10 phone                         your Feather server
 +------------------+   WebSocket over TLS   +----------------------------+
 |  Feather app     | <--------------------> |  Node.js + Playwright      |
 |  (Cascades/Qt)   |   frames  <---         |  headless Chromium         | ---> the web
 |                  |   taps, keys --->      |  (one session per phone)   |
 +------------------+                        +----------------------------+
```

Feather is a personal tool: **one server serves you** (and a few devices of yours),
protected by an access token. It is not meant to be a public service.

## Features

- Modern sites, logins, web apps: everything runs in desktop-class Chromium.
- Tabs (up to 6), bookmarks, zoom (1x to 2x), light and dark theme (dark also
  darkens web pages that have no dark theme of their own).
- Native text input, select boxes, date and time pickers.
- Downloads saved to the phone's Downloads folder.
- Smooth scrolling: while you scroll the server sends only the new strip of the page
  (optional, see the server's optional features).
- Videos in the phone's native player: YouTube, and optionally Instagram reels and
  videos embedded in other web pages.
- Opens `http:`/`https:` links from other apps.
- Signing in to Google and other sites that refuse automated browsers: sign in once
  in a normal window on the server (`server/login.js`), the phone reuses that login.
- English and Italian UI (follows the phone's language).

## What you need

- A BlackBerry 10 phone running 10.3, in **Development Mode**.
- A computer to run the server: Linux, Windows or macOS with Node.js 18 or newer.
  It must be reachable from the phone, on your Wi-Fi or over the internet (for example
  through a free Cloudflare Tunnel, see the server guide).
- To build the app yourself: Windows with the BlackBerry 10 Native SDK 10.3.

## Getting started

1. **Set up the server**: [server/README.md](server/README.md). At first start it
   prints an access token.
2. **Install the app** on the phone (see below).
3. Open Feather, tap the top menu, then **Settings**. Enter the server URL (for example
   `http://192.168.1.20:8080/api` on your Wi-Fi, or `https://feather.example.com/api`
   through a tunnel) and the token.

## Building the app

Requirements: Windows, the [BlackBerry 10 Native SDK 10.3](https://developer.blackberry.com/native/)
installed in `C:\bbndk` (Momentics' default), and PowerShell.

```
powershell -File client\package.ps1
```

This compiles the translations, builds the release binary and packages
`client\Feather-<version>-armv7.bar` (it also bumps the build number, so a new
package installs as an update of the old one). The NDK's own Java 1.7 runs the
packager, since newer Java versions crash it.

The client links two native libraries shipped prebuilt in `third_party/`:
mbed TLS (for TLS 1.2 to the server) and libjpeg-turbo (fast frame decoding).
`tools/build-libjpeg-turbo.sh` rebuilds the latter.

## Installing the app

BlackBerry's signing service has been shut down, so Feather is packaged in development
mode and has to be side-loaded on a phone in Development Mode
(Settings > Security and Privacy > Development Mode). Ways that work:

- **Momentics**: import `client/` as a project and use **Run** (not Debug) on the phone.
- **Rooted phone**: `package.ps1 -Install` copies the package to the phone over SSH and
  installs it with the system installer (`tools/installbar.sh`). Copy
  `client/package.config.example.ps1` to `client/package.config.ps1` and fill in the
  phone's IP and the SSH key of its root login.

## Translations

UI strings are `qsTr()` in `client/assets/main.qml` and `tr()` in the C++ sources;
translations live in `client/translations/Feather_<language>.ts` and are compiled by
`package.ps1`. To add a language, copy `Feather_it.ts` to e.g. `Feather_de.ts`, set
`language="de_DE"`, translate the strings (Qt Linguist, or any text editor) and
rebuild. Refresh the `.ts` files after changing strings with the NDK's `lupdate`:

```
C:\bbndk\ndk\host_10_3_1_12\win32\x86\usr\bin\lupdate client\Feather.pro
```

## Security

- The server listens on `127.0.0.1` only, unless started with `--lan`.
- Every request needs the access token; repeated wrong tokens block the address.
- The server's browser can reach only the public internet: local networks, loopback
  and cloud metadata addresses are refused, after DNS resolution.
- The connection from the phone is TLS 1.2 checked against the Mozilla CA bundle
  when the server URL is `https://`.

Anyone with your token can browse through your server and use its saved logins:
treat it like a password. Details in [server/README.md](server/README.md#security).

## Credits

- The client's TLS and WebSocket code comes from BBport, a modern-TLS layer for
  BlackBerry 10 apps, built on [mbed TLS](https://github.com/Mbed-TLS/mbedtls).
- Icons: [Material Symbols](https://fonts.google.com/icons) by Google.
- Third-party components and their licenses: [THIRD_PARTY.md](THIRD_PARTY.md).

## License

Feather is free software under the [GNU General Public License v3.0](LICENSE).
