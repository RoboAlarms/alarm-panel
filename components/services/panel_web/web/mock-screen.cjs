// The mock panel's remote keypad (components/services/panel_web/web/mock-panel.cjs): the WebSocket the firmware
// serves at /api/v1/screen (components/services/panel_web/src/web_screen.c), with a pretend screen.
// Its pictures are real H.264 (every macroblock I_PCM, so no encoder library is needed; an
// unchanged screen is an all-skip P picture) or PNG where the page can't decode H.264, so the
// page's WebCodecs path runs for real in a browser. Taps light the pretend keys, typed text
// shows as blocks along the top, a held press draws a red frame; a bar at the bottom moves once a
// second. Not part of the product.
'use strict';
const crypto = require('node:crypto');
const zlib = require('node:zlib');

const W = 1024, H = 600, HM = 608; // the screen; H.264 in whole macroblocks, cropped by the SPS

// ---- bits and NAL units --------------------------------------------------------------------
class Bits {
  constructor() { this.bytes = []; this.cur = 0; this.n = 0; }
  u(v, n) { for (let i = n - 1; i >= 0; i--) { this.cur = (this.cur << 1) | ((v / 2 ** i) & 1); if (++this.n === 8) this.flush(); } return this; }
  flush() { this.bytes.push(this.cur & 255); this.cur = 0; this.n = 0; }
  ue(v) { const x = v + 1, len = Math.floor(Math.log2(x)); return this.u(0, len).u(x, len + 1); }
  se(v) { return this.ue(v <= 0 ? -2 * v : 2 * v - 1); }
  align0() { while (this.n) this.u(0, 1); return this; }
  raw(buf) { for (const b of buf) this.bytes.push(b); return this; } // byte-aligned only
  trailing() { this.u(1, 1); while (this.n) this.u(0, 1); return Buffer.from(this.bytes); }
}
function nal(header, rbsp) { // start code, header, emulation prevention
  const out = [0, 0, 0, 1, header];
  let zeros = 0;
  for (const b of rbsp) {
    if (zeros >= 2 && b <= 3) { out.push(3); zeros = 0; }
    out.push(b);
    zeros = b === 0 ? zeros + 1 : 0;
  }
  return Buffer.from(out);
}
function sps() { // as the panel sends it: esp_h264's SPS rewritten by components/core/h264_sps
  return nal(0x67, new Bits().u(66, 8).u(0xC0, 8).u(31, 8).ue(0).ue(4).ue(2).ue(1).u(0, 1).ue(W / 16 - 1).ue(HM / 16 - 1)
    .u(1, 1).u(0, 1).u(1, 1).ue(0).ue(0).ue(0).ue((HM - H) / 2)
    .u(1, 1).u(0, 2).u(1, 1).u(5, 3).u(0, 1).u(1, 1).u(6, 8).u(6, 8).u(6, 8).u(0, 1)
    .u(1, 1).u(1, 32).u(30, 32).u(1, 1).u(0, 3)
    .u(1, 1).u(1, 1).ue(2).ue(1).ue(16).ue(16).ue(0).ue(1).trailing());
}
function pps() {
  return nal(0x68, new Bits().ue(0).ue(0).u(0, 1).u(0, 1).ue(0).ue(0).ue(0).u(0, 1).u(0, 2).se(0).se(0).se(0)
    .u(1, 1).u(0, 1).u(0, 1).trailing());
}
function idr(yuv, id) { // every macroblock I_PCM: the picture exactly as it is
  const b = new Bits().ue(0).ue(7).ue(0).u(0, 8).ue(id).u(0, 1).u(0, 1).se(0).ue(1);
  const mbw = W / 16, mbh = HM / 16;
  for (let my = 0; my < mbh; my++) for (let mx = 0; mx < mbw; mx++) {
    b.ue(25).align0();
    const block = Buffer.alloc(384);
    for (let y = 0; y < 16; y++) yuv.y.copy(block, y * 16, (my * 16 + y) * W + mx * 16, (my * 16 + y) * W + mx * 16 + 16);
    for (let y = 0; y < 8; y++) {
      yuv.u.copy(block, 256 + y * 8, (my * 8 + y) * (W / 2) + mx * 8, (my * 8 + y) * (W / 2) + mx * 8 + 8);
      yuv.v.copy(block, 320 + y * 8, (my * 8 + y) * (W / 2) + mx * 8, (my * 8 + y) * (W / 2) + mx * 8 + 8);
    }
    b.raw(block);
  }
  return nal(0x65, b.trailing());
}
function skipped(frameNum) { // nothing changed: every macroblock copied from the last picture
  return nal(0x41, new Bits().ue(0).ue(5).ue(0).u(frameNum & 255, 8).u(0, 1).u(0, 1).u(0, 1).se(0).ue(1)
    .ue((W / 16) * (HM / 16)).trailing());
}

// ---- PNG, for a page without H.264 ------------------------------------------------------------
const CRC = new Int32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; return c; });
const crc32 = buf => { let c = -1; for (const b of buf) c = CRC[(c ^ b) & 255] ^ (c >>> 8); return (c ^ -1) >>> 0; };
function chunk(type, data) {
  const out = Buffer.alloc(12 + data.length);
  out.writeUInt32BE(data.length, 0);
  out.write(type, 4, 'ascii');
  data.copy(out, 8);
  out.writeUInt32BE(crc32(out.subarray(4, 8 + data.length)), 8 + data.length);
  return out;
}
function png(rgb) {
  const raw = Buffer.alloc((W * 3 + 1) * H);
  for (let y = 0; y < H; y++) rgb.copy(raw, y * (W * 3 + 1) + 1, y * W * 3, (y + 1) * W * 3);
  const head = Buffer.alloc(13);
  head.writeUInt32BE(W, 0);
  head.writeUInt32BE(H, 4);
  head.set([8, 2, 0, 0, 0], 8);
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', head),
    chunk('IDAT', zlib.deflateSync(raw, {level: 1})), chunk('IEND', Buffer.alloc(0))]);
}

// ---- the pretend screen -----------------------------------------------------------------------
const KEYS = Array.from({length: 12}, (_, i) => ({x: 112 + (i % 4) * 208, y: 150 + Math.floor(i / 4) * 120, w: 176, h: 96}));
function paint(scene) {
  const rgb = Buffer.alloc(W * H * 3);
  const fill = (x0, y0, w, h, [r, g, b]) => {
    for (let y = Math.max(0, y0); y < Math.min(H, y0 + h); y++)
      for (let x = Math.max(0, x0); x < Math.min(W, x0 + w); x++) { const i = (y * W + x) * 3; rgb[i] = r; rgb[i + 1] = g; rgb[i + 2] = b; }
  };
  fill(0, 0, W, H, [11, 15, 20]);
  fill(0, 0, 96, H, [14, 20, 27]); // the rail
  if (scene.held) { fill(0, 0, W, 6, [248, 113, 113]); fill(0, H - 6, W, 6, [248, 113, 113]); }
  KEYS.forEach((k, i) => fill(k.x, k.y, k.w, k.h, scene.lit.has(i) ? [96, 165, 250] : [28, 37, 49]));
  [...scene.text].slice(-40).forEach((_, i) => fill(120 + i * 22, 60, 16, 28, [241, 245, 249]));
  fill(120, 540, Math.floor((scene.seconds % 60) / 60 * 880), 16, [52, 211, 153]);
  return rgb;
}
function yuvOf(rgb) { // BT.601, limited range, 4:2:0 (what the P4's PPA makes)
  const y = Buffer.alloc(W * HM, 16), u = Buffer.alloc(W / 2 * HM / 2, 128), v = Buffer.alloc(W / 2 * HM / 2, 128);
  for (let py = 0; py < H; py++) for (let px = 0; px < W; px++) {
    const i = (py * W + px) * 3, r = rgb[i], g = rgb[i + 1], b = rgb[i + 2];
    y[py * W + px] = Math.round(16 + (65.738 * r + 129.057 * g + 25.064 * b) / 256);
    if (!(px & 1) && !(py & 1)) {
      const c = (py / 2) * (W / 2) + px / 2;
      u[c] = Math.round(128 + (-37.945 * r - 74.494 * g + 112.439 * b) / 256);
      v[c] = Math.round(128 + (112.439 * r - 94.154 * g - 18.285 * b) / 256);
    }
  }
  return {y, u, v};
}

// ---- WebSocket framing ------------------------------------------------------------------------
function frame(opcode, payload) {
  const len = payload.length;
  const head = len < 126 ? Buffer.from([0x80 | opcode, len])
    : len < 65536 ? Buffer.from([0x80 | opcode, 126, len >> 8, len & 255])
      : Buffer.concat([Buffer.from([0x80 | opcode, 127, 0, 0, 0, 0]), (() => { const b = Buffer.alloc(4); b.writeUInt32BE(len); return b; })()]);
  return Buffer.concat([head, payload]);
}
function readFrames(state, data, onText, onClose) {
  state.buf = Buffer.concat([state.buf, data]);
  for (;;) {
    const b = state.buf;
    if (b.length < 2) return;
    const opcode = b[0] & 15;
    let len = b[1] & 127, at = 2;
    if (len === 126) { if (b.length < 4) return; len = b.readUInt16BE(2); at = 4; }
    else if (len === 127) { if (b.length < 10) return; len = Number(b.readBigUInt64BE(2)); at = 10; }
    if (b.length < at + 4 + len) return;
    const mask = b.subarray(at, at + 4), payload = Buffer.from(b.subarray(at + 4, at + 4 + len));
    for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
    state.buf = b.subarray(at + 4 + len);
    if (opcode === 8) return onClose();
    if (opcode === 1) onText(payload.toString('utf8'));
  }
}

// ---- the session ---------------------------------------------------------------------------------
let viewer = null; // one at a time, as on the panel
function attach(req, socket, session, csrfOf) {
  const key = req.headers['sec-websocket-key'];
  const accept = crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
  const me = {socket, codec: '', sent: 0, acked: 0, viewing: true, key: true, frameNum: 0, idr: 0, alive: true, reader: {buf: Buffer.alloc(0)}};
  const scene = {lit: new Set(), text: '', held: false, seconds: Math.floor(Date.now() / 1000)};
  let drawn = '';
  const send = (op, payload) => { if (me.alive) socket.write(frame(op, payload)); };
  const text = obj => send(1, Buffer.from(JSON.stringify(obj)));
  const bye = why => { text({t: 'bye', why}); me.alive = false; setTimeout(() => socket.end(), 50); };
  const picture = () => {
    if (!me.codec || !me.viewing || me.sent - me.acked >= 2) return;
    scene.seconds = Math.floor(Date.now() / 1000);
    const look = JSON.stringify([[...scene.lit], scene.text, scene.held, scene.seconds]);
    const changed = look !== drawn || me.key;
    if (!changed) return;
    drawn = look;
    const rgb = paint(scene);
    let type, data;
    if (me.codec === 'h264') {
      type = 1;
      data = Buffer.concat([sps(), pps(), idr(yuvOf(rgb), me.idr++ & 1)]);
      me.frameNum = 0;
    } else {
      type = 4;
      data = png(rgb);
    }
    me.key = false;
    const head = Buffer.alloc(12);
    head[0] = type;
    head.writeUInt32LE(++me.sent, 4);
    head.writeUInt32LE(Date.now() >>> 0, 8);
    send(2, Buffer.concat([head, data]));
    if (me.codec === 'h264') setTimeout(() => { // and a "nothing changed" P picture after it
      if (!me.alive || me.sent - me.acked >= 2) return;
      const p = Buffer.alloc(12);
      p[0] = 2;
      p.writeUInt32LE(++me.sent, 4);
      p.writeUInt32LE(Date.now() >>> 0, 8);
      send(2, Buffer.concat([p, skipped(++me.frameNum)]));
    }, 60);
  };
  const timer = setInterval(picture, 50);
  const end = () => { if (!me.alive && viewer !== me) return; me.alive = false; clearInterval(timer); if (viewer === me) viewer = null; };
  socket.on('close', end);
  socket.on('error', end);
  socket.on('data', data => readFrames(me.reader, data, raw => {
    let m;
    try { m = JSON.parse(raw); } catch { return; }
    if (m.t === 'hello') {
      if (m.csrf !== csrfOf(session)) return bye('signed_out');
      if (viewer && viewer !== me && viewer.alive) return bye('in_use:Alex');
      viewer = me;
      me.codec = m.codec === 'h264' ? 'h264' : 'jpeg';
      text({t: 'hello', codec: me.codec, width: W, height: H, paused: false});
    } else if (m.t === 'ack') me.acked = Math.max(me.acked, m.n);
    else if (m.t === 'key-frame') { me.key = true; me.acked = me.sent; }
    else if (m.t === 'view') { me.viewing = !!m.on; me.key = me.viewing; me.acked = me.sent; }
    else if (m.t === 'tap' || m.t === 'down') {
      const i = KEYS.findIndex(k => m.x >= k.x && m.x < k.x + k.w && m.y >= k.y && m.y < k.y + k.h);
      if (i >= 0) scene.lit.has(i) ? scene.lit.delete(i) : scene.lit.add(i);
      scene.held = m.t === 'down';
    } else if (m.t === 'up') scene.held = false;
    else if (m.t === 'text') scene.text += String(m.s).slice(0, 16);
    else if (m.t === 'key' && m.k === 'backspace') scene.text = [...scene.text].slice(0, -1).join('');
    else if (m.t === 'key' && m.k === 'escape') scene.text = '';
  }, () => { end(); socket.end(); }));
}
module.exports = {attach};
