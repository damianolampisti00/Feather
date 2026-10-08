#!/usr/bin/env python3
"""A web page with a video -> the stream BB10's native player can take.

Usage: webvideo.py URL [REFERER] [COOKIEFILE]
Prints ONE JSON line: {"ok": true, "url", "protocol": "file"|"hls", "headers",
"width", "height", "title", "duration", "extractor"} or {"ok": false, "reason"}.

Runs yt-dlp (its own venv, .venv-ytdlp, with curl-cffi for the sites that only
answer a real browser): ~1800 site extractors plus a generic one for plain
HTML5 <video>, JSON-LD, og:video and the usual embedded players. See
resolveWebVideo in server.js.
"""
import json
import sys


def out(obj):
    print(json.dumps(obj))
    sys.exit(0)


try:
    import yt_dlp
except Exception as e:  # noqa: BLE001
    out({"ok": False, "reason": "yt-dlp missing: %s" % e})

# The Q10/Q5 decode H.264 in hardware; the screen is 720x720.
MAX_HEIGHT = 720


def playable(f):
    url = f.get("url") or ""
    proto = f.get("protocol") or ""
    if not url.startswith(("http://", "https://")):
        return False
    if proto not in ("http", "https", "m3u8", "m3u8_native"):
        return False  # DASH, f4m, RTMP...: not for this player
    vcodec, acodec = f.get("vcodec"), f.get("acodec")
    if vcodec == "none" or acodec == "none":
        return False  # needs picture AND sound in one stream
    if vcodec and not vcodec.startswith(("avc", "h264")):
        return False  # VP9/AV1/HEVC: no decoder (None = unknown, usually H.264)
    if proto in ("http", "https") and (f.get("ext") or "mp4") not in ("mp4", "m4v", "mov"):
        return False
    return True


def score(f):
    h = f.get("height") or 0
    progressive = (f.get("protocol") or "").startswith("http")
    # Highest that fits the screen, else the smallest above it; a plain file
    # (seekable, one connection) beats HLS at the same height.
    return (h <= MAX_HEIGHT, h if h <= MAX_HEIGHT else -h, progressive, f.get("tbr") or 0)


def main():
    url = sys.argv[1]
    referer = sys.argv[2] if len(sys.argv) > 2 else ""
    cookiefile = sys.argv[3] if len(sys.argv) > 3 else ""
    opts = {
        "quiet": True,
        "no_warnings": True,
        "noplaylist": True,
        "skip_download": True,
        "socket_timeout": 15,
    }
    if cookiefile:
        opts["cookiefile"] = cookiefile
    if referer and referer != url:
        opts["http_headers"] = {"Referer": referer}
    try:
        with yt_dlp.YoutubeDL(opts) as ydl:
            info = ydl.extract_info(url, download=False)
    except Exception as e:  # noqa: BLE001
        out({"ok": False, "reason": str(e).splitlines()[0][:300]})
    if info.get("_type") == "playlist":
        entries = [e for e in (info.get("entries") or []) if e]
        if not entries:
            out({"ok": False, "reason": "empty playlist"})
        info = entries[0]
    formats = [f for f in (info.get("formats") or [info]) if playable(f)]
    if not formats:
        out({"ok": False, "reason": "no H.264 stream with sound"})
    best = max(formats, key=score)
    out({
        "ok": True,
        "url": best["url"],
        "protocol": "hls" if (best.get("protocol") or "").startswith("m3u8") else "file",
        "headers": best.get("http_headers") or info.get("http_headers") or {},
        "width": best.get("width"),
        "height": best.get("height"),
        "title": info.get("title"),
        "duration": info.get("duration"),
        "extractor": info.get("extractor_key") or info.get("extractor"),
    })


main()
