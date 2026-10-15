/* The remote keypad in the browser (components/services/panel_web/web/panel-keypad.js), with a pretend
 * WebSocket, canvas, clock and keyboard: no device, network or dependencies. The owner's
 * complaints (2026-09-23) each have a test: keys registering twice over the VPN (a quick click is
 * one tap, timed by the panel), a broken picture after switching tabs (the canvas and its last
 * picture stay), "Take control" (being on the page is control), and typing on the computer's
 * keyboard (it goes to the panel). */
const {test} = require('node:test');
const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

// The page's words first (panel-words.js), as panel.html loads them.
// PANEL_WEB_DIR: a folder made by minify_asset.py --all, to test the shipped files (BL-122).
const web = process.env.PANEL_WEB_DIR || path.join(__dirname, '../web');
const source = readFileSync(path.join(web, 'panel-words.js'), 'utf8') + '\n' +
  readFileSync(path.join(web, 'panel-keypad.js'), 'utf8');
const settle = () => new Promise(resolve => setImmediate(resolve));

// secure: a page where WebCodecs decodes H.264; stored: what this browser remembers.
function harness({secure = false, stored = {}} = {}) {
  let now = 0, next = 0;
  const timers = new Map(); // id -> {fn, at, every}
  const sockets = [], docListeners = {}, draws = [], nodes = {};
  class FakeSocket {
    constructor(url) { this.url = url; this.sent = []; this.readyState = 0; sockets.push(this); }
    send(data) { this.sent.push(JSON.parse(data)); }
    close() { this.readyState = 3; this.closedByPage = true; }
    open() { this.readyState = 1; return this.onopen?.(); }
    text(m) { this.onmessage?.({data: JSON.stringify(m)}); }
    picture(type, n, payload = [1, 2, 3]) {
      const b = new Uint8Array(12 + payload.length);
      b[0] = type;
      b[4] = n & 255;
      b.set(payload, 12);
      this.onmessage?.({data: b.buffer});
    }
    drop() { this.readyState = 3; this.onclose?.(); }
    messages(t) { return this.sent.filter(m => m.t === t); }
  }
  const node = () => ({textContent: '', classList: {toggle() {}}, replaceChildren(...kids) { this.children = kids; }});
  for (const id of ['#screen-shell', '#frame-status', '#frame-rate', '#control-state']) nodes[id] = node();
  nodes['.screen-card'] = node();
  const canvas = () => ({
    width: 0, height: 0, tabIndex: -1, attrs: {},
    setAttribute(k, v) { this.attrs[k] = v; },
    getContext: () => ({fillRect() {}, drawImage: (...args) => draws.push(args), set fillStyle(v) {}}),
    getBoundingClientRect: () => ({left: 0, top: 0, width: 1024, height: 600}),
    setPointerCapture() {}, focus() {},
  });
  const context = {
    console, Promise, JSON, Math, Number, String, Map, Set, Error, Uint8Array, Date, TextEncoder, Intl,
    fmtNumber: n => Number(n).toFixed(0), // panel.js's
    globalThis: {isSecureContext: secure},
    VideoDecoder: secure ? class { static async isConfigSupported() { return {supported: true}; } } : undefined,
    localStorage: {getItem: k => (k in stored ? stored[k] : null), setItem: (k, v) => { stored[k] = String(v); }},
    performance: {now: () => now},
    setTimeout: (fn, ms) => { timers.set(++next, {fn, at: now + ms}); return next; },
    clearTimeout: id => timers.delete(id),
    setInterval: (fn, ms) => { timers.set(++next, {fn, at: now + ms, every: ms}); return next; },
    clearInterval: id => timers.delete(id),
    WebSocket: FakeSocket,
    Blob: class { constructor(parts, opts) { this.parts = parts; this.type = opts.type; } },
    createImageBitmap: async blob => ({blob, close() {}}),
    document: {
      createElement: () => canvas(),
      addEventListener: (t, fn) => { (docListeners[t] ||= new Set()).add(fn); },
      removeEventListener: (t, fn) => docListeners[t]?.delete(fn),
    },
    location: {protocol: 'https:', host: 'panel.test'},
    $: s => nodes[s],
    page: 'keypad', csrf: 'c'.repeat(64), renderId: 1,
    state: {capabilities: {width: 1024, height: 600, video: ['h264', 'jpeg']}},
    loginPage: message => { context.signedOut = message; },
  };
  vm.createContext(context);
  vm.runInContext(source, context);
  const h = {
    context, sockets, draws, nodes,
    get socket() { return sockets[sockets.length - 1]; },
    get canvas() { return nodes['#screen-shell'].children[0]; },
    advance(ms) { // run every timer due within ms, in order
      const end = now + ms;
      for (;;) {
        const due = [...timers].filter(([, t]) => t.at <= end).sort((a, b) => a[1].at - b[1].at)[0];
        if (!due) break;
        const [id, t] = due;
        now = t.at;
        if (t.every) t.at += t.every; else timers.delete(id);
        t.fn();
      }
      now = end;
    },
    async start() {
      vm.runInContext('startFrame()', context);
      await h.socket.open();
      h.socket.text({t: 'hello', codec: 'jpeg', width: 1024, height: 600, paused: false});
    },
    key(key, extra = {}) {
      const e = {key, code: extra.code || key, target: extra.target || h.canvas, ctrlKey: false, altKey: false, metaKey: false,
        isComposing: false, prevented: false, preventDefault() { this.prevented = true; }, ...extra};
      for (const fn of docListeners.keydown || []) fn(e);
      return e;
    },
    pointer(kind, x, y) {
      const e = {button: 0, pointerId: 7, clientX: x, clientY: y, preventDefault() {}};
      h.canvas['onpointer' + kind](e);
    },
  };
  return h;
}

test('the page says hello with its CSRF token and the pictures it can decode', async () => {
  const h = harness();
  await h.start();
  assert.equal(h.socket.url, 'wss://panel.test/api/v1/screen');
  const [hello] = h.socket.messages('hello');
  assert.equal(hello.csrf, 'c'.repeat(64));
  assert.equal(hello.codec, 'jpeg', 'no WebCodecs here: JPEG');
  assert.match(h.nodes['#control-state'].textContent, /You have control/);
});

test('the page asks for video where the browser decodes it, and still pictures when chosen', async () => {
  const video = harness({secure: true});
  await video.start();
  assert.equal(video.socket.messages('hello')[0].codec, 'h264');
  // The way out if the video ever looks wrong on a panel: "Still pictures (JPEG)" on the page.
  const still = harness({secure: true, stored: {'panel-manager.keypad-picture': 'jpeg'}});
  await still.start();
  assert.equal(still.socket.messages('hello')[0].codec, 'jpeg');
});

// Found 2026-09-23: over the VPN a key sometimes registered twice. A quick click is now
// one message, pressed and released by the panel itself.
test('a quick click is one tap, never a press held across the network', async () => {
  const h = harness();
  await h.start();
  h.pointer('down', 512, 300);
  h.advance(90);
  h.pointer('up', 512, 300);
  const sent = h.socket.sent.filter(m => m.t !== 'hello');
  assert.deepEqual(sent, [{t: 'tap', x: 512, y: 300, ms: 90}]);
});

test('a hold is streamed and kept up until it ends (the Panic button)', async () => {
  const h = harness();
  await h.start();
  h.pointer('down', 60, 560);
  h.advance(2200);
  h.pointer('up', 60, 560);
  const sent = h.socket.sent.filter(m => m.t !== 'hello' && m.t !== 'ping');
  assert.deepEqual(sent[0], {t: 'down', x: 60, y: 560});
  assert.ok(sent.filter(m => m.t === 'move').length >= 3, 'the hold is repeated so the panel keeps it');
  assert.deepEqual(sent[sent.length - 1], {t: 'up', x: 60, y: 560});
  assert.equal(sent.filter(m => m.t === 'tap').length, 0);
});

test('a drag goes at once, its positions coalesced', async () => {
  const h = harness();
  await h.start();
  h.pointer('down', 100, 300);
  for (let x = 110; x <= 500; x += 10) { h.advance(5); h.pointer('move', x, 300); }
  h.advance(40);
  h.pointer('up', 505, 300);
  const sent = h.socket.sent.filter(m => m.t !== 'hello' && m.t !== 'ping');
  assert.deepEqual(sent[0], {t: 'down', x: 100, y: 300});
  const moves = sent.filter(m => m.t === 'move');
  assert.ok(moves.length >= 2 && moves.length <= 8, `${moves.length} moves for 40 positions in 200 ms`);
  assert.deepEqual(sent[sent.length - 1], {t: 'up', x: 505, y: 300});
});

test('the computer’s keyboard types on the panel, but not into this page’s own fields', async () => {
  const h = harness();
  await h.start();
  assert.ok(h.key('a').prevented);
  h.key('Backspace');
  h.key('Enter');
  h.key('Escape');
  h.key('é');
  h.key('c', {ctrlKey: true});               // a browser shortcut stays the browser's
  h.key('x', {target: {tagName: 'INPUT'}});  // typing in a form field
  h.key('Shift');
  assert.deepEqual(h.socket.sent.filter(m => m.t === 'text' || m.t === 'key'),
    [{t: 'text', s: 'a'}, {t: 'key', k: 'backspace'}, {t: 'key', k: 'enter'}, {t: 'key', k: 'escape'}, {t: 'text', s: 'é'}]);
});

test('pictures are drawn and acknowledged; the last one stays when the page comes back', async () => {
  const h = harness();
  await h.start();
  h.socket.picture(3, 1);
  await settle();
  h.socket.picture(3, 2);
  await settle();
  assert.deepEqual(h.socket.messages('ack'), [{t: 'ack', n: 1}, {t: 'ack', n: 2}]);
  assert.equal(h.draws.length, 2);
  const canvas = h.canvas;
  vm.runInContext('stopFrame()', h.context);
  assert.ok(h.sockets[0].closedByPage, 'leaving closes the socket');
  await h.start(); // back on the page: the same canvas, still showing the last picture
  assert.equal(h.canvas, canvas);
  assert.equal(h.sockets.length, 2);
});

test('a background tab stops the pictures, and lets the panel go after a minute', async () => {
  const h = harness();
  await h.start();
  vm.runInContext('screenVisible(false)', h.context);
  assert.deepEqual(h.socket.messages('view').pop(), {t: 'view', on: false});
  vm.runInContext('screenVisible(true)', h.context);
  assert.deepEqual(h.socket.messages('view').pop(), {t: 'view', on: true});
  vm.runInContext('screenVisible(false)', h.context);
  h.advance(61000);
  assert.ok(h.socket.closedByPage, 'still holding the panel after a minute in the background');
});

test('the panel’s answers reach the page; a dropped connection comes back by itself', async () => {
  const h = harness();
  await h.start();
  h.socket.text({t: 'control', state: 'paused'});
  assert.match(h.nodes['#control-state'].textContent, /paused remote control/);
  h.socket.text({t: 'control', state: 'local'});
  assert.match(h.nodes['#control-state'].textContent, /touching the panel/);
  h.socket.drop();
  h.advance(600);
  assert.equal(h.sockets.length, 2, 'no reconnection');
  // Somebody else has it: say who, and don't keep knocking.
  await h.socket.open();
  h.socket.text({t: 'bye', why: 'in_use:Alex'});
  h.socket.drop();
  h.advance(10000);
  assert.equal(h.sockets.length, 2);
  assert.match(h.nodes['#control-state'].textContent, /Alex is using the remote keypad/);
});

test('a sign-in that ended goes back to the sign-in page', async () => {
  const h = harness();
  await h.start();
  h.socket.text({t: 'bye', why: 'signed_out'});
  assert.match(h.context.signedOut, /Sign in again/);
});
