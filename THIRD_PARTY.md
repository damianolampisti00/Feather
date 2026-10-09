# Third-party components

Feather itself is licensed under the GNU General Public License v3.0 (`LICENSE`).
It ships or uses the following components, all under GPL-3.0-compatible licenses:

| Component | Where | Used for | License |
|---|---|---|---|
| [mbed TLS](https://github.com/Mbed-TLS/mbedtls) 2.28.9 | `third_party/mbedtls` (headers + prebuilt armv7 library) | TLS 1.2 for the client's connection to the server | Apache-2.0 (`third_party/mbedtls/LICENSE`) |
| [libjpeg-turbo](https://libjpeg-turbo.org/) 2.0.6 | `third_party/libjpeg-turbo` (headers + armv7 library, built by `tools/build-libjpeg-turbo.sh`) | Fast NEON JPEG decoding of the page frames | IJG + Modified BSD + zlib (`third_party/libjpeg-turbo/LICENSE.md`) |
| App icon | `client/icon.png` | Home screen icon | Made for Feather by its author (generated with ChatGPT); part of Feather, GPL-3.0 |
| [libopus](https://opus-codec.org/) | `third_party/opus` (headers + prebuilt armv7 library) | Decoding the page's sound | BSD-3-Clause (`third_party/opus/COPYING`) |
| [Material Symbols](https://fonts.google.com/icons) by Google | `client/assets/images` (rendered PNGs) | Menu and navigation icons | Apache-2.0 (`client/assets/images/LICENSE-material-symbols.txt`) |
| Mozilla CA certificate bundle, via [curl](https://curl.se/docs/caextract.html) | `client/assets/cacert.pem` | Verifying the server's certificate | MPL-2.0 |
| [Playwright](https://playwright.dev/), [express](https://expressjs.com/), [ws](https://github.com/websockets/ws), [sharp](https://sharp.pixelplumbing.com/) | server, installed by `npm install` | Headless browser control, HTTP/WebSocket server, frame encoding | Apache-2.0 / MIT / MIT / Apache-2.0 |
| [opusscript](https://github.com/abalabahaha/opusscript) (optional) | server, installed by `npm install` | Encoding the page's sound | MIT (libopus inside: BSD-3-Clause) |
| [yt-dlp](https://github.com/yt-dlp/yt-dlp) (optional) | installed separately on the server | Finding the stream of videos embedded in web pages | Unlicense |
| [parth-dl](https://github.com/parthmax2/parth-dl) (optional) | installed separately on the server | Instagram reels | MIT |
| jpegtran from [libjpeg-turbo](https://libjpeg-turbo.org/) (optional) | installed separately on the server, run as a separate program | Lossless JPEG crops for scroll deltas | IJG + Modified BSD |
| BlackBerry 10 Native SDK / Cascades | build toolchain, system libraries on the phone | UI framework and platform APIs | BlackBerry SDK license; linked as system libraries |
