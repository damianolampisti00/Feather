'use strict';
// Page sound for the phone (optional). The browser plays into a virtual
// PulseAudio/PipeWire output of its own (a null sink, AUDIO_SINK), so nothing
// comes out of this machine's speakers; `parec` captures that output in real
// time, 20 ms blocks are Opus-encoded (opusscript: libopus as WebAssembly)
// and go to every WebSocket client that asked for audio (?audio=1), as binary
// messages "MBA1" + seq u32 LE + Opus packet (48 kHz stereo).
// Silence is not sent (after a short tail), so a quiet page costs nothing.
//
// Needs pactl + parec (pulseaudio-utils, or pipewire-pulse) and the opusscript
// npm package; without them the server runs exactly as before, muted.
const { spawn, spawnSync } = require('child_process');

const SINK = process.env.AUDIO_SINK || 'feather_audio';
const RATE = 48000;
const FRAME = 960;                 // samples per channel in 20 ms
const FRAME_BYTES = FRAME * 2 * 2; // s16le stereo
const SILENCE_PEAK = 24;           // |sample| at or under this counts as silence
const TAIL_FRAMES = 15;            // keep sending 300 ms after the sound stops
const MAX_BUFFERED = 256 * 1024;   // a client this far behind skips frames

let OpusScript = null;
try { OpusScript = require('opusscript'); } catch (_) {}

function run(cmd, args) {
  const r = spawnSync(cmd, args, { encoding: 'utf8', timeout: 5000 });
  return r.error ? null : r;
}

// The null sink, created once (it survives a server restart, not a reboot).
function setupSink() {
  const list = run('pactl', ['list', 'short', 'sinks']);
  if (!list || list.status !== 0) return false;
  if (list.stdout.split('\n').some((l) => l.split('\t')[1] === SINK)) return true;
  const load = run('pactl', ['load-module', 'module-null-sink', `sink_name=${SINK}`,
    'sink_properties=device.description=Feather']);
  return !!load && load.status === 0;
}

const available = !!OpusScript && !!run('parec', ['--version']) && setupSink();
const listeners = new Set();
let started = false;

function start() {
  if (!available || started) return;
  started = true;
  const enc = new OpusScript(RATE, 2, OpusScript.Application.AUDIO);
  enc.setBitrate(96000);
  let pending = Buffer.alloc(0);
  let seq = 0;
  let quietFor = TAIL_FRAMES;
  const capture = () => {
    const p = spawn('parec', ['-d', `${SINK}.monitor`, '--raw', '--format=s16le',
      `--rate=${RATE}`, '--channels=2', '--latency-msec=20']);
    p.stdout.on('data', (chunk) => {
      pending = pending.length ? Buffer.concat([pending, chunk]) : chunk;
      while (pending.length >= FRAME_BYTES) {
        const frame = pending.subarray(0, FRAME_BYTES);
        pending = pending.subarray(FRAME_BYTES);
        let peak = 0;
        for (let i = 0; i < FRAME_BYTES; i += 2) {
          const v = Math.abs(frame.readInt16LE(i));
          if (v > peak) peak = v;
        }
        quietFor = peak <= SILENCE_PEAK ? quietFor + 1 : 0;
        if (quietFor > TAIL_FRAMES || listeners.size === 0) continue;
        const packet = enc.encode(Buffer.from(frame), FRAME);
        const msg = Buffer.alloc(8 + packet.length);
        msg.write('MBA1', 0, 'latin1');
        msg.writeUInt32LE(seq++ >>> 0, 4);
        Buffer.from(packet).copy(msg, 8);
        for (const ws of listeners) {
          if (ws.readyState === 1 && ws.bufferedAmount < MAX_BUFFERED) ws.send(msg);
        }
      }
    });
    p.on('exit', () => setTimeout(capture, 3000));
    p.on('error', () => {});
  };
  capture();
}

module.exports = {
  audioAvailable: available,
  // For chromium.launch: play into our sink, and don't let Playwright mute it.
  browserOptions: available ? { env: { ...process.env, PULSE_SINK: SINK }, unmute: true } : null,
  addListener(ws) { if (available) { listeners.add(ws); start(); } },
  removeListener(ws) { listeners.delete(ws); },
};
