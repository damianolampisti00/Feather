'use strict';
// frames=delta (see the /ws handler in server.js): while a page scrolls, most
// of each new frame is the previous one moved by the scroll -- the idea of
// VNC's CopyRect. Such a frame goes out as an "MBD1" message: the shift, plus
// JPEGs of only the rectangles that changed or just scrolled into view. The
// client (applyDelta in browsercontroller.cpp) moves the pixels it already
// has and pastes those rectangles in.
//
// The comparison is EXACT, never "close enough": the server keeps every scroll
// step a multiple of 16 px (SCROLL_QUANTUM in server.js), the size of the
// screencast JPEG's blocks (4:2:0 MCUs), so the part of the page that only
// moved is encoded, and decodes, bit-identically. Measured 2026-10-06 on
// Wikipedia pages: 92-98% of the moved 16x16 tiles identical with aligned
// steps, while with arbitrary ones (30, 37, 44 px) the JPEG noise differed
// almost everywhere. Anything that really changed -- even one pixel -- is
// resent, so errors can't pile up.
//
// sharp (libvips) compares decoded frames; jpegtran crops changed regions
// directly from the source JPEG without recompressing their DCT coefficients.
// Optional sharp: without it every frame is simply sent whole.

let sharp = null;
try {
  sharp = require('sharp');
  sharp.cache(false); // every input is a fresh buffer: nothing to reuse
} catch (e) {
  console.warn(`[frames] sharp unavailable, frames=delta disabled: ${e.message}`);
}

const { spawn, spawnSync } = require('node:child_process');
const JPEGTRAN = process.env.JPEGTRAN_PATH || 'jpegtran';
// jpegtran (libjpeg-turbo's tools: apt/pacman "libjpeg-turbo", or
// libjpeg-turbo-progs) is optional too: without it frames go out whole.
const jpegtranOk = !spawnSync(JPEGTRAN, ['-version'], { stdio: 'ignore' }).error;
if (sharp && !jpegtranOk) console.warn(`[frames] ${JPEGTRAN} not found, frames=delta disabled`);

const TILE = 16;
// Rectangles have MCU-aligned origins and extents (tiles are 16x16), so
// jpegtran can crop their original DCT coefficients without decoding or
// recompressing them. This keeps the screencast JPEG's exact quality.
const MAX_RECTS = 24; // past this, whole-width bands instead (fewer JPEG headers)
const GAP = 2;        // clean tiles bridged inside a run: cheaper than another rectangle
const KEY_AREA = 0.5; // more than this changed: just send the whole frame

async function decode(jpeg) {
  const { data, info } = await sharp(jpeg).raw().toBuffer({ resolveWithObject: true });
  return { rgb: data, w: info.width, h: info.height, ch: info.channels };
}

function cropJpeg(jpeg, r) {
  return new Promise((resolve, reject) => {
    const child = spawn(JPEGTRAN, ['-copy', 'none', '-crop', `${r.w}x${r.h}+${r.x}+${r.y}`], { stdio: ['pipe', 'pipe', 'pipe'] });
    const chunks = [];
    let stderr = '';
    child.stdout.on('data', (chunk) => chunks.push(chunk));
    child.stderr.on('data', (chunk) => { if (stderr.length < 2048) stderr += chunk.toString(); });
    child.once('error', reject);
    child.once('close', (code) => {
      if (code === 0 && chunks.length) resolve(Buffer.concat(chunks));
      else reject(new Error(`jpegtran crop failed (${code}): ${stderr.trim()}`));
    });
    child.stdin.once('error', (err) => { if (err.code !== 'EPIPE') reject(err); });
    child.stdin.end(jpeg);
  });
}

// JPEG chroma upsampling can differ at the crop boundary. Expand each region
// by one MCU so those boundary pixels land in unchanged pixels outside the
// dirty area, then merge overlaps so one crop cannot overwrite another's edge.
function paddedRects(rects, w, h) {
  let out = rects.map((r) => {
    const x = Math.max(0, r.x - TILE), y = Math.max(0, r.y - TILE);
    const right = Math.min(w, r.x + r.w + TILE), bottom = Math.min(h, r.y + r.h + TILE);
    return { x, y, w: right - x, h: bottom - y };
  });
  let changed = true;
  while (changed) {
    changed = false;
    outer: for (let i = 0; i < out.length; i++) for (let j = i + 1; j < out.length; j++) {
      const a = out[i], b = out[j];
      if (a.x >= b.x + b.w || b.x >= a.x + a.w || a.y >= b.y + b.h || b.y >= a.y + a.h) continue;
      const x = Math.min(a.x, b.x), y = Math.min(a.y, b.y);
      const right = Math.max(a.x + a.w, b.x + b.w), bottom = Math.max(a.y + a.h, b.y + b.h);
      out[i] = { x, y, w: right - x, h: bottom - y };
      out.splice(j, 1);
      changed = true;
      break outer;
    }
  }
  return out;
}

// Tiles of B that differ from A shifted by (dx, dy) -- B(x, y) vs A(x + dx,
// y + dy) -- or whose source lies (partly) outside A, i.e. just scrolled in.
function dirtyTiles(A, B, dx, dy) {
  const { w, h, ch } = B;
  const cols = Math.ceil(w / TILE), rows = Math.ceil(h / TILE);
  const dirty = new Uint8Array(cols * rows);
  for (let ty = 0; ty < rows; ty++) {
    const y0 = ty * TILE, y1 = Math.min(h, y0 + TILE);
    for (let tx = 0; tx < cols; tx++) {
      const x0 = tx * TILE, x1 = Math.min(w, x0 + TILE);
      if (x0 + dx < 0 || x1 + dx > w || y0 + dy < 0 || y1 + dy > h) dirty[ty * cols + tx] = 1;
    }
  }
  const xa = Math.max(0, -dx), xb = Math.min(w, w - dx);
  if (xb <= xa) return { dirty, cols, rows };
  for (let y = 0; y < h; y++) {
    const sy = y + dy;
    if (sy < 0 || sy >= h) continue;
    // Whole row first (one memcmp): almost every row of a scroll matches.
    const bo = (y * w + xa) * ch, ao = (sy * w + xa + dx) * ch, len = (xb - xa) * ch;
    if (B.rgb.compare(A.rgb, ao, ao + len, bo, bo + len) === 0) continue;
    const ty = (y / TILE) | 0;
    for (let tx = 0; tx < cols; tx++) {
      if (dirty[ty * cols + tx]) continue;
      const x0 = Math.max(tx * TILE, xa), x1 = Math.min(tx * TILE + TILE, xb);
      if (x1 <= x0) continue;
      const b = (y * w + x0) * ch, a = (sy * w + x0 + dx) * ch, l = (x1 - x0) * ch;
      if (B.rgb.compare(A.rgb, a, a + l, b, b + l) !== 0) dirty[ty * cols + tx] = 1;
    }
  }
  return { dirty, cols, rows };
}

// Dirty tiles -> pixel rectangles: runs along each tile row, stacked into
// taller rectangles while the run below has the same span.
function rectsFrom(dirty, cols, rows, w, h) {
  let tiles = [];
  let open = new Map();
  for (let ty = 0; ty < rows; ty++) {
    const next = new Map();
    let tx = 0;
    while (tx < cols) {
      if (!dirty[ty * cols + tx]) { tx++; continue; }
      let end = tx + 1;
      for (let k = end; k < cols && k - end <= GAP; k++) if (dirty[ty * cols + k]) end = k + 1;
      const key = tx + ',' + end;
      let r = open.get(key);
      if (r) r.th++;
      else { r = { tx, ty, tw: end - tx, th: 1 }; tiles.push(r); }
      next.set(key, r);
      tx = end;
    }
    open = next;
  }
  if (tiles.length > MAX_RECTS) {
    const rowDirty = (r) => { for (let c = 0; c < cols; c++) if (dirty[r * cols + c]) return true; return false; };
    tiles = [];
    for (let ty = 0; ty < rows;) {
      if (!rowDirty(ty)) { ty++; continue; }
      let e = ty + 1;
      while (e < rows && rowDirty(e)) e++;
      tiles.push({ tx: 0, ty, tw: cols, th: e - ty });
      ty = e;
    }
  }
  return tiles.map((r) => {
    const x = r.tx * TILE, y = r.ty * TILE;
    return { x, y, w: Math.min(w, (r.tx + r.tw) * TILE) - x, h: Math.min(h, (r.ty + r.th) * TILE) - y };
  });
}

// One encoder per connection: it tracks the frame the client has right now.
function createDeltaEncoder() {
  let last = null;
  return {
    reset() { last = null; },
    // frame: { buf: JPEG, meta: { top, left, cssW } (screencast frames) or null }.
    // Resolves to { kind: 'key' | 'delta' | 'same', data }: the JPEG itself,
    // an "MBD1" message, or null when nothing changed (nothing to send).
    async encode(frame) {
      const key = { kind: 'key', data: frame.buf };
      let cur;
      try {
        cur = await decode(frame.buf);
      } catch (e) {
        last = null; // sent whole, but not tracked: the next frame is whole too
        throw e;
      }
      cur.meta = frame.meta || null;
      const prev = last;
      last = cur;
      if (!prev || prev.w !== cur.w || prev.h !== cur.h || prev.ch !== cur.ch) return key;
      // The shift: the page scroll between the two frames, in frame pixels.
      // Frames without scroll metadata (screenshots) are compared unshifted.
      let dx = 0, dy = 0;
      if (prev.meta && cur.meta && cur.meta.cssW > 0) {
        const s = cur.w / cur.meta.cssW;
        dx = Math.round((cur.meta.left - prev.meta.left) * s);
        dy = Math.round((cur.meta.top - prev.meta.top) * s);
      }
      if (Math.abs(dx) >= cur.w || Math.abs(dy) >= cur.h) return key;
      const { dirty, cols, rows } = dirtyTiles(prev, cur, dx, dy);
      const rects = rectsFrom(dirty, cols, rows, cur.w, cur.h);
      if (rects.length === 0 && dx === 0 && dy === 0) return { kind: 'same', data: null };
      const area = rects.reduce((a, r) => a + r.w * r.h, 0);
      if (area > KEY_AREA * cur.w * cur.h) return key;
      const patches = paddedRects(rects, cur.w, cur.h);
      // Lossless JPEG crop preserves the exact source coefficients. If the
      // utility is unavailable or rejects an edge crop, retain the sharp
      // re-encode path as a compatible fallback.
      let jpegs;
      try {
        jpegs = await Promise.all(patches.map((r) => cropJpeg(frame.buf, r)));
      } catch (e) {
        const raw = { raw: { width: cur.w, height: cur.h, channels: cur.ch } };
        jpegs = await Promise.all(patches.map((r) => sharp(cur.rgb, raw)
          .extract({ left: r.x, top: r.y, width: r.w, height: r.h })
          .jpeg({ quality: parseInt(process.env.DELTA_QUALITY || '75', 10) })
          .toBuffer()));
      }
      // "MBD1", u16 width, u16 height, i16 dx, i16 dy, u16 count, u16 reserved;
      // count x { u16 x, y, w, h, u32 length }; then the JPEGs. Little-endian.
      const head = Buffer.alloc(16 + 12 * patches.length);
      head.write('MBD1', 0, 'latin1');
      head.writeUInt16LE(cur.w, 4);
      head.writeUInt16LE(cur.h, 6);
      head.writeInt16LE(dx, 8);
      head.writeInt16LE(dy, 10);
      head.writeUInt16LE(patches.length, 12);
      patches.forEach((r, i) => {
        const o = 16 + 12 * i;
        head.writeUInt16LE(r.x, o);
        head.writeUInt16LE(r.y, o + 2);
        head.writeUInt16LE(r.w, o + 4);
        head.writeUInt16LE(r.h, o + 6);
        head.writeUInt32LE(jpegs[i].length, o + 8);
      });
      const data = Buffer.concat([head, ...jpegs]);
      return data.length < frame.buf.length ? { kind: 'delta', data } : key;
    },
  };
}

module.exports = { createDeltaEncoder, deltaAvailable: !!sharp && jpegtranOk };
