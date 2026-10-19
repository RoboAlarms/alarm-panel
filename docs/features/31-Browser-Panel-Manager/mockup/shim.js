// The Panel Manager mockup's pretend panel, inside the page (docs/features/31 mockup/README.md).
// build.cjs puts this file first in app.html, followed by components/services/panel_web/web/mock-panel.cjs
// unchanged, wrapped in a function that gets the Node pieces it asks for from here. Then the page's
// fetch() and its remote keypad's WebSocket are answered by that pretend panel instead of the network,
// so the real panel.html, panel.js and panel-pages.js run as they do against `node mock-panel.cjs`,
// with nothing to install and nothing sent anywhere. Not part of the product.
//
// app.html?role=installer|master|partition|standard|guest|temporary|signedout
//   &scenario=ready|notready|motion|exit|armed|stay|entry|alarm|memory|fire|troubles  &parts=2  &zwave=0|jammed
//   &clock=ok|lost|failed|other  &walk=1  &fast=1  &theme=light|dark  &p=<proposal>[,<proposal>]
'use strict';
(function () {
  const query = new URLSearchParams(location.search);
  const MOCKUP = window.MOCKUP = {query, routes: [], proposals: [], internals: null, files: window.MOCKUP_FILES || {}};

  // ---- what the pretend panel is (mock-panel.cjs reads these as environment variables) ----------------
  const env = {};
  const role = query.get('role') || 'installer';
  if (role === 'temporary') Object.assign(env, {MOCK_AUTOLOGIN: 'master', MOCK_TEMPORARY: '1'});
  else if (role !== 'signedout') env.MOCK_AUTOLOGIN = role;
  const scenario = query.get('scenario');
  if (scenario && scenario !== 'ready') env.MOCK_SCENARIO = scenario;
  if (query.get('headless') === '1') env.MOCK_HEADLESS = '1';
  if (query.get('parts') === '2') env.MOCK_PARTITIONS = '2';
  if (query.get('zwave') === '0' || query.get('zwave') === 'jammed') env.MOCK_ZWAVE = query.get('zwave');
  if (query.get('clock') && query.get('clock') !== 'none') env.MOCK_CLOCK = query.get('clock');
  if (query.get('walk') === '1') env.MOCK_WALK = '1';
  if (query.get('fast') === '1') env.MOCK_FAST = '1';
  if (Object.keys(MOCKUP.files).some(f => f.startsWith('/i18n/'))) env.MOCK_I18N = '/i18n';
  MOCKUP.env = env;

  // ---- Buffer, as much of it as the pretend panel uses ------------------------------------------------
  const enc = new TextEncoder(), dec = new TextDecoder();
  const latin1 = s => { const b = new Buffer(s.length); for (let i = 0; i < s.length; i++) b[i] = s.charCodeAt(i) & 255; return b; };
  class Buffer extends Uint8Array {
    static from(v, encoding) {
      if (typeof v === 'string') {
        if (encoding === 'hex') { const b = new Buffer(v.length >> 1); for (let i = 0; i < b.length; i++) b[i] = parseInt(v.substr(i * 2, 2), 16); return b; }
        if (encoding === 'latin1' || encoding === 'binary' || encoding === 'ascii') return latin1(v);
        if (encoding === 'base64') return latin1(atob(v));
        const u = enc.encode(v), b = new Buffer(u.length);
        b.set(u);
        return b;
      }
      const src = v instanceof ArrayBuffer ? new Uint8Array(v) : v;
      const b = new Buffer(src.length);
      b.set(src);
      return b;
    }
    static alloc(n, fill = 0) { const b = new Buffer(n); if (fill) b.fill(fill); return b; }
    static concat(list) {
      const b = new Buffer(list.reduce((n, x) => n + x.length, 0));
      let at = 0;
      for (const x of list) { b.set(x, at); at += x.length; }
      return b;
    }
    static byteLength(s) { return enc.encode(String(s)).length; }
    static isBuffer(x) { return x instanceof Buffer; }
    view() { return new DataView(this.buffer, this.byteOffset, this.byteLength); }
    toString(encoding = 'utf8', start = 0, end = this.length) {
      const part = this.subarray(start, end);
      if (encoding === 'hex') return Array.from(part, x => x.toString(16).padStart(2, '0')).join('');
      if (encoding === 'latin1' || encoding === 'binary' || encoding === 'ascii' || encoding === 'base64') {
        let s = '';
        for (const x of part) s += String.fromCharCode(x);
        return encoding === 'base64' ? btoa(s) : s;
      }
      return dec.decode(part);
    }
    write(s, offset = 0, encoding = 'utf8') { const b = Buffer.from(s, encoding === 'ascii' ? 'latin1' : encoding); this.set(b, offset); return b.length; }
    copy(target, at = 0, start = 0, end = this.length) { target.set(this.subarray(start, end), at); return end - start; }
    equals(other) { return this.length === other.length && this.every((x, i) => x === other[i]); }
    includes(v, from = 0) {
      if (typeof v === 'number') return super.includes(v, from);
      const n = Buffer.from(v);
      outer: for (let i = from; i + n.length <= this.length; i++) {
        for (let j = 0; j < n.length; j++) if (this[i + j] !== n[j]) continue outer;
        return true;
      }
      return false;
    }
    readUInt16BE(o = 0) { return this.view().getUint16(o, false); }
    readUInt32LE(o = 0) { return this.view().getUint32(o, true); }
    readUInt32BE(o = 0) { return this.view().getUint32(o, false); }
    writeUInt32LE(v, o = 0) { this.view().setUint32(o, v >>> 0, true); return o + 4; }
    writeUInt32BE(v, o = 0) { this.view().setUint32(o, v >>> 0, false); return o + 4; }
    readBigUInt64LE(o = 0) { return this.view().getBigUint64(o, true); }
    readBigUInt64BE(o = 0) { return this.view().getBigUint64(o, false); }
    writeBigUInt64LE(v, o = 0) { this.view().setBigUint64(o, BigInt(v), true); return o + 8; }
  }

  // ---- crypto: random bytes, SHA-1 (a Z-Wave QR code's check), and a stand-in for the backup's cipher ---
  function sha1(bytes) {
    const ml = bytes.length, size = ((ml + 9 + 63) >> 6) << 6, m = new Uint8Array(size);
    m.set(bytes);
    m[ml] = 0x80;
    const dv = new DataView(m.buffer);
    dv.setUint32(size - 4, (ml * 8) >>> 0);
    dv.setUint32(size - 8, Math.floor(ml * 8 / 2 ** 32));
    const h = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0], w = new Uint32Array(80);
    for (let off = 0; off < size; off += 64) {
      for (let i = 0; i < 16; i++) w[i] = dv.getUint32(off + i * 4);
      for (let i = 16; i < 80; i++) { const x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]; w[i] = (x << 1) | (x >>> 31); }
      let [a, b, c, d, e] = h;
      for (let i = 0; i < 80; i++) {
        const f = i < 20 ? (b & c) | (~b & d) : i < 40 || i >= 60 ? b ^ c ^ d : (b & c) | (b & d) | (c & d);
        const k = i < 20 ? 0x5A827999 : i < 40 ? 0x6ED9EBA1 : i < 60 ? 0x8F1BBCDC : 0xCA62C1D6;
        const next = (((a << 5) | (a >>> 27)) + f + e + k + w[i]) >>> 0;
        e = d; d = c; c = ((b << 30) | (b >>> 2)) >>> 0; b = a; a = next;
      }
      [a, b, c, d, e].forEach((x, i) => { h[i] = (h[i] + x) >>> 0; });
    }
    const out = Buffer.alloc(20);
    h.forEach((x, i) => out.view().setUint32(i * 4, x));
    return out;
  }
  // The backup's AES-256-GCM, pretended: a key stream and a tag from SHA-1, so a file the mockup made opens
  // with its password and a wrong password is refused. Nothing here is real encryption.
  const stream = (key, data) => {
    const out = Buffer.from(data);
    for (let i = 0; i < out.length; i += 20) {
      const pad = sha1(Buffer.concat([key, Buffer.from([i >> 24 & 255, i >> 16 & 255, i >> 8 & 255, i & 255])]));
      for (let j = 0; j < 20 && i + j < out.length; j++) out[i + j] ^= pad[j];
    }
    return out;
  };
  const tagOf = (key, aad, data) => sha1(Buffer.concat([key, aad, data])).subarray(0, 16);
  function gcm(key, decrypt) {
    let aad = Buffer.alloc(0), tag = null;
    const seen = [];
    return {
      setAAD(a) { aad = Buffer.from(a); return this; },
      setAuthTag(t) { tag = Buffer.from(t); return this; },
      update(x) { const out = stream(key, x); seen.push(decrypt ? Buffer.from(x) : out); return out; },
      final() {
        if (decrypt && !tagOf(key, aad, Buffer.concat(seen)).equals(tag)) throw new Error('Unsupported state or unable to authenticate data');
        return Buffer.alloc(0);
      },
      getAuthTag() { return tagOf(key, aad, Buffer.concat(seen)); },
    };
  }
  const nodeCrypto = {
    randomBytes: n => { const b = Buffer.alloc(n); crypto.getRandomValues(b); return b; },
    createHash: () => {
      const parts = [];
      return {update(x) { parts.push(Buffer.from(x)); return this; }, digest(encoding) { const d = sha1(Buffer.concat(parts)); return encoding ? d.toString(encoding) : d; }};
    },
    pbkdf2Sync: (password, salt, iterations, length) => {
      let k = sha1(Buffer.concat([Buffer.from(password), Buffer.from(salt)]));
      while (k.length < length) k = Buffer.concat([k, sha1(k)]);
      return k.subarray(0, length);
    },
    createCipheriv: (alg, key) => gcm(Buffer.from(key), false),
    createDecipheriv: (alg, key) => gcm(Buffer.from(key), true),
    webcrypto: crypto,
  };
  MOCKUP.sha1 = sha1;

  // ---- files: the catalogs the build put in (/i18n/<locale>.json), nothing else ------------------------
  const files = MOCKUP.files;
  const fs = {
    existsSync: p => p in files || Object.keys(files).some(f => f.startsWith(p + '/')),
    readdirSync: p => Object.keys(files).filter(f => f.startsWith(p + '/')).map(f => f.slice(p.length + 1)),
    readFileSync: p => { if (p in files) return files[p]; throw new Error('ENOENT: ' + p); },
    createReadStream: p => ({pipe(res) { if (p in files) res.end(files[p]); else { res.writeHead(404); res.end('{}'); } }}),
  };
  const path = {join: (...parts) => parts.filter(x => x !== '').join('/').replace(/\/+/g, '/'), basename: p => p.split('/').pop()};

  // ---- http: the pretend panel's request handler, kept to be called by fetch() below -------------------
  let handler = null;
  const http = {createServer(h) { handler = h; return {on() { return this; }, listen() { return this; }, address: () => ({port: 0})}; }};
  const process = {env, argv: ['node', 'mock-panel.cjs'], on() {}, platform: 'browser'};
  const modules = {'node:http': http, 'node:fs': fs, 'node:path': path, 'node:crypto': nodeCrypto, './mock-screen.cjs': {attach() {}}};
  const require = name => { if (modules[name]) return modules[name]; throw new Error('The mockup has no ' + name); };
  MOCKUP.start = mockModule => {
    MOCKUP.internals = mockModule(require, process, Buffer, {exports: {}}, {}, '/mock', '/mock/mock-panel.cjs');
  };

  // ---- fetch(): /api/v1 and /i18n go to the pretend panel; anything else to the network ----------------
  const realFetch = window.fetch.bind(window);
  const jar = new Map(); // the session cookie a sign-in sets
  const headersOf = h => {
    const out = {};
    if (h instanceof Headers) h.forEach((v, k) => { out[k.toLowerCase()] = v; });
    else for (const [k, v] of Object.entries(h || {})) out[k.toLowerCase()] = String(v);
    return out;
  };
  async function bytesOf(body) {
    if (body == null) return null;
    if (typeof body === 'string') return body;
    if (body instanceof Blob) return new Uint8Array(await body.arrayBuffer());
    if (body instanceof ArrayBuffer) return new Uint8Array(body);
    if (ArrayBuffer.isView(body)) return new Uint8Array(body.buffer, body.byteOffset, body.byteLength);
    return String(body);
  }
  // A proposal's own answers (MOCKUP.route), before the pretend panel's.
  function proposalAnswer(method, url, body) {
    for (const r of MOCKUP.routes) {
      const m = r.method === method && r.path.exec(url.pathname.replace(/^\/api\/v1\//, ''));
      if (!m) continue;
      let json = {};
      try { json = typeof body === 'string' && body ? JSON.parse(body) : {}; } catch { /* not JSON */ }
      const out = r.run(m, json, MOCKUP.internals, url.searchParams);
      if (out) return new Response(JSON.stringify(out[1]), {status: out[0], headers: {'Content-Type': 'application/json'}});
    }
    return null;
  }
  async function answer(method, url, headers, rawBody) {
    const body = await bytesOf(rawBody);
    const own = proposalAnswer(method, url, body);
    if (own) return own;
    if (!handler) throw new TypeError('The pretend panel has not started');
    const cookie = [...jar].map(([k, v]) => `${k}=${v}`).join('; ');
    const chunk = () => typeof body === 'string' ? body : Buffer.from(body);
    const req = {
      method, url: url.pathname + url.search, headers: {...headers, ...(cookie ? {cookie} : {})},
      on(event, f) {
        if (event === 'data' && body != null && body.length) queueMicrotask(() => f(chunk()));
        if (event === 'end') queueMicrotask(f);
        return this;
      },
      async *[Symbol.asyncIterator]() { if (body != null && body.length) yield Buffer.from(typeof body === 'string' ? Buffer.from(body) : body); },
    };
    return new Promise((resolve, reject) => {
      const parts = [];
      const res = {
        statusCode: 200, head: {},
        writeHead(code, h = {}) { this.statusCode = code; Object.assign(this.head, h); return this; },
        setHeader(k, v) {
          if (/^set-cookie$/i.test(k)) { const m = /^([^=]+)=([^;]*)/.exec(String(v)); if (m) jar.set(m[1], m[2]); } else this.head[k] = v;
        },
        write(x) { parts.push(typeof x === 'string' ? Buffer.from(x) : x); return true; },
        end(x) {
          if (x != null) this.write(x);
          const head = Object.fromEntries(Object.entries(this.head).map(([k, v]) => [k, String(v)]));
          resolve(new Response(Buffer.concat(parts), {status: this.statusCode, headers: head}));
        },
      };
      Promise.resolve().then(() => handler(req, res)).catch(reject);
    });
  }
  window.fetch = async (input, init = {}) => {
    // Opened from a folder, '/api/v1/x' resolves to file:///C:/api/v1/x: read the page's own paths as they are.
    const raw = typeof input === 'string' ? input : input.url;
    const ours = /^\/(api\/v1\/|i18n\/)/;
    const url = ours.test(raw) ? new URL(raw, 'http://panel.mockup') : new URL(raw, location.href);
    if (!ours.test(raw) && (url.origin !== location.origin || !ours.test(url.pathname))) return realFetch(input, init);
    init.signal?.throwIfAborted?.();
    return answer((init.method || 'GET').toUpperCase(), url, headersOf(init.headers), init.body);
  };
  // A proposal answers a request of its own: MOCKUP.route('GET', /^zone\/(\d+)$/, (match, body, mock, query) => [200, {...}]).
  MOCKUP.route = (method, pathRe, run) => MOCKUP.routes.push({method, path: pathRe, run});

  // ---- the remote keypad: a pretend screen in still pictures (PNG), as mock-screen.cjs sends to a page --
  // that can't decode H.264. Taps light the pretend keys; typed text shows as blocks along the top.
  const RealSocket = window.WebSocket;
  class PretendScreen {
    constructor() {
      this.readyState = 0;
      this.binaryType = 'blob';
      this.sent = 0;
      this.scene = {lit: new Set(), text: '', held: false};
      this.canvas = document.createElement('canvas');
      this.canvas.width = 1024;
      this.canvas.height = 600;
      setTimeout(() => { this.readyState = 1; this.onopen?.({}); }, 30);
    }
    tell(obj) { if (this.readyState === 1) this.onmessage?.({data: JSON.stringify(obj)}); }
    paint() {
      const c = this.canvas.getContext('2d'), s = this.scene, p = MOCKUP.internals?.parts?.[0];
      c.fillStyle = '#0b0f14'; c.fillRect(0, 0, 1024, 600);
      c.fillStyle = '#0e141b'; c.fillRect(0, 0, 96, 600);
      if (s.held) { c.fillStyle = '#f87171'; c.fillRect(0, 0, 1024, 6); c.fillRect(0, 594, 1024, 6); }
      c.fillStyle = '#9fd8c6'; c.font = '28px system-ui, sans-serif'; c.fillText('Home', 120, 56);
      c.fillStyle = '#f1f5f9'; c.font = '600 44px system-ui, sans-serif';
      c.fillText(p ? (p.alarm ? 'Alarm' : p.state === 'disarmed' ? 'Disarmed' : p.state === 'armed' ? 'Armed' : 'Arming') : 'Disarmed', 120, 118);
      for (let i = 0; i < 12; i++) {
        c.fillStyle = s.lit.has(i) ? '#60a5fa' : '#1c2531';
        c.fillRect(112 + (i % 4) * 208, 150 + Math.floor(i / 4) * 120, 176, 96);
      }
      c.fillStyle = '#f1f5f9';
      [...s.text].slice(-30).forEach((_, i) => c.fillRect(560 + i * 14, 92, 10, 22));
      c.fillStyle = '#6f8795'; c.font = '18px system-ui, sans-serif';
      c.fillText('Pretend screen (mockup) · ' + new Date().toLocaleTimeString(), 120, 560);
    }
    picture() {
      if (this.readyState !== 1) return;
      this.paint();
      this.canvas.toBlob(async blob => {
        if (!blob || this.readyState !== 1) return;
        const data = new Uint8Array(await blob.arrayBuffer()), out = new Uint8Array(12 + data.length), v = new DataView(out.buffer);
        out[0] = 4;
        v.setUint32(4, ++this.sent, true);
        v.setUint32(8, Date.now() >>> 0, true);
        out.set(data, 12);
        this.onmessage?.({data: this.binaryType === 'arraybuffer' ? out.buffer : new Blob([out])});
      }, 'image/png');
    }
    send(raw) {
      let m;
      try { m = JSON.parse(raw); } catch { return; }
      const keys = i => ({x: 112 + (i % 4) * 208, y: 150 + Math.floor(i / 4) * 120});
      if (m.t === 'hello') {
        this.tell({t: 'hello', codec: 'jpeg', width: 1024, height: 600, paused: false});
        this.picture();
        this.timer = setInterval(() => this.picture(), 1000);
      } else if (m.t === 'tap' || m.t === 'down') {
        const i = Array.from({length: 12}, (_, n) => n).find(n => { const k = keys(n); return m.x >= k.x && m.x < k.x + 176 && m.y >= k.y && m.y < k.y + 96; });
        if (i !== undefined) { if (this.scene.lit.has(i)) this.scene.lit.delete(i); else this.scene.lit.add(i); }
        this.scene.held = m.t === 'down';
        this.picture();
      } else if (m.t === 'up') { this.scene.held = false; this.picture(); }
      else if (m.t === 'text') { this.scene.text += String(m.s).slice(0, 16); this.picture(); }
      else if (m.t === 'key' && m.k === 'backspace') { this.scene.text = [...this.scene.text].slice(0, -1).join(''); this.picture(); }
      else if (m.t === 'key-frame' || m.t === 'view') this.picture();
    }
    close() {
      if (this.readyState > 1) return;
      clearInterval(this.timer);
      this.readyState = 3;
      setTimeout(() => this.onclose?.({}), 0);
    }
  }
  window.WebSocket = function (url, protocols) {
    return /\/api\/v1\/screen$/.test(String(url)) ? new PretendScreen() : new RealSocket(url, protocols);
  };
  Object.assign(window.WebSocket, {CONNECTING: 0, OPEN: 1, CLOSING: 2, CLOSED: 3});
})();
