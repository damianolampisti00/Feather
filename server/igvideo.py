#!/usr/bin/env python3
"""Resolves a public Instagram reel/video post to a directly playable MP4 for
the client's native video player (see resolveInstagram in server.js), using
parth-dl (pip install parth-dl, in server/.venv-parthdl).

Usage: igvideo.py <instagram url>
Prints one JSON object on stdout:
  {"ok": true, "url": ..., "width": ..., "height": ..., "title": ...}
  {"ok": false, "reason": ...}

Unlike YouTube's, Instagram's CDN URLs are not tied to the IP that asked for
them (no ip= parameter; checked 2026-10-05), so resolving here and playing on
the phone works on mobile data too. They are short-lived, though: resolve
right before playing, never cache.
"""
import json
import sys


def pick(formats):
    # BB10's player needs audio and video in one file: renditions with audio
    # only. The smallest that still covers the 720px screen, else the largest
    # (width/height are only hints: a "720x1280" format probed 480x854).
    muxed = [f for f in formats if f.get("has_audio") and f.get("url")]
    if not muxed:
        return None
    muxed.sort(key=lambda f: f.get("height") or 0)
    for f in muxed:
        if (f.get("height") or 0) >= 720:
            return f
    return muxed[-1]


def resolve(url):
    try:
        from parth_dl import get_info
        info = get_info(url)
    except Exception as e:  # login wall, private post, rate limit, network...
        return {"ok": False, "reason": ("%s: %s" % (type(e).__name__, e))[:300]}
    # A single video only: a carousel or a photo post is better shown as the page.
    if info.get("type") != "video":
        return {"ok": False, "reason": "not a single video (%s)" % info.get("type")}
    for entry in info.get("entries") or []:
        if entry.get("kind") != "video":
            continue
        f = pick(entry.get("formats") or [])
        if f:
            return {"ok": True, "url": f["url"], "width": f.get("width"),
                    "height": f.get("height"), "title": info.get("title") or ""}
    return {"ok": False, "reason": "no format with audio"}


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(json.dumps({"ok": False, "reason": "usage: igvideo.py <url>"}))
        sys.exit(2)
    print(json.dumps(resolve(sys.argv[1])))
