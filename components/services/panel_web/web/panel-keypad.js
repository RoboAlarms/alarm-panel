'use strict';
// Remote keypad: the panel's own screen, live, and every press, key and typed letter sent to it
// (docs/features/31). One WebSocket (/api/v1/screen) carries both ways. Pictures come as H.264,
// decoded by the browser (WebCodecs), or as JPEG where it can't decode that; the panel sends one
// only when its screen changed, at most two ahead of what this page has drawn. Presses, keys and
// text go back as small JSON messages. Being on the page is control: a touch at the panel wins
// for a moment, and the person there can pause it with the red banner the panel shows.
// Globals come from panel.js; components/services/panel_web/test/panel_frames.test.cjs covers the gestures and flow.

const TAP_HOLD_MS = 250;   // a press held longer is a hold (the panel's Panic, a slider)...
const MOVE_SLOP = 8;       // ...and one that moves further (panel pixels) is a drag
const HOLD_KEEP_MS = 500;  // a held press is repeated this often: the panel lets go after 2 s
const MOVE_EVERY_MS = 33;  // drag positions at most this often
const HIDDEN_CLOSE_MS = 60000; // a tab left in the background this long lets the panel go
const KEYS = {Backspace: 'backspace', Enter: 'enter', NumpadEnter: 'enter', Escape: 'escape'};
const CONTROL_WORDS = {
  ok: tx('You have control. A touch at the panel takes over for a moment.'),
  local: tx('Someone is touching the panel’s screen. Your control comes back when they stop.'),
  busy: tx('Someone at the panel has a code typed or a protected page open. Wait until they finish.'),
  paused: tx('The person at the panel paused remote control. They can let it go on from the panel.'),
  other: tx('The development workstation has the panel’s screen just now.'),
};
const BYE_WORDS = {
  signed_out: tx('Your sign-in ended. Sign in again to use the remote keypad.'),
  replaced: tx('The remote keypad is open in another tab or window.'),
  not_viewer: tx('The remote keypad is open somewhere else.'),
};

let screenCanvas = null; // kept across pages and tab switches: the last picture stays up
let session = null;      // the live session: one at a time

// A canvas the size of the panel's screen, black until the first picture.
function screenFor(width, height) {
  if (!screenCanvas || screenCanvas.width !== width || screenCanvas.height !== height) {
    screenCanvas = document.createElement('canvas');
    screenCanvas.id = 'screen';
    screenCanvas.width = width;
    screenCanvas.height = height;
    screenCanvas.tabIndex = 0;
    screenCanvas.setAttribute('role', 'img');
    screenCanvas.setAttribute('aria-label', t('The alarm panel’s screen. Click to press, type to enter text.'));
    const ctx = screenCanvas.getContext('2d');
    ctx.fillStyle = '#000';
    ctx.fillRect(0, 0, width, height);
  }
  return screenCanvas;
}

// H.264 through WebCodecs where the browser has a decoder for it (it needs a secure page).
async function h264Decodes() {
  if (!globalThis.isSecureContext || typeof VideoDecoder === 'undefined') return false;
  try {
    return (await VideoDecoder.isConfigSupported({codec: 'avc1.42C01F', optimizeForLatency: true})).supported === true;
  } catch {
    return false;
  }
}
// Which pictures to ask for: video (H.264) where the browser decodes it, or JPEG when chosen on
// the page - the way out if the video ever looks wrong on a panel. Remembered in this browser.
const PICTURE_KEY = 'panel-manager.keypad-picture';
function pictureChoice() {
  try {
    return localStorage.getItem(PICTURE_KEY) === 'jpeg' ? 'jpeg' : 'h264';
  } catch {
    return 'h264';
  }
}
function pictureChoiceHtml(caps) {
  if (!(caps.video || []).includes('h264')) return '';
  const chosen = pictureChoice();
  return `<div class="filters" role="group" aria-label="${ht('Pictures')}">${[['h264', t('Video (H.264)')], ['jpeg', t('Still pictures (JPEG)')]]
    .map(([v, l]) => `<button type="button" data-picture="${v}" aria-pressed="${v === chosen}">${esc(l)}</button>`).join('')}</div>`;
}
function bindKeypadPage() {
  const buttons = $$('[data-picture]');
  const show = chosen => buttons.forEach(b => b.setAttribute('aria-pressed', String(b.dataset.picture === chosen)));
  buttons.forEach(b => b.onclick = () => {
    try { localStorage.setItem(PICTURE_KEY, b.dataset.picture); } catch { /* not remembered (private window) */ }
    show(b.dataset.picture);
    startFrame();
  });
  h264Decodes().then(ok => {
    const video = buttons.find(b => b.dataset.picture === 'h264');
    if (ok || !video) return;
    video.disabled = true;
    video.title = t('This browser can’t decode H.264 on this page');
    show('jpeg');
  });
  startFrame();
}

// "avc1.PPCCLL" from the SPS in a key picture (profile, constraints, level).
function codecOf(bytes) {
  for (let i = 0; i + 7 < bytes.length; i++) {
    if (bytes[i] === 0 && bytes[i + 1] === 0 && bytes[i + 2] === 1 && (bytes[i + 3] & 0x1f) === 7) {
      const hex = n => n.toString(16).padStart(2, '0').toUpperCase();
      return `avc1.${hex(bytes[i + 4])}${hex(bytes[i + 5])}${hex(bytes[i + 6])}`;
    }
  }
  return 'avc1.42C01F';
}

function stopFrame() {
  if (session) session.close();
  session = null;
}

function startFrame() {
  stopFrame();
  const caps = state.capabilities;
  const shell = $('#screen-shell');
  if (!shell) return;
  const canvas = screenFor(caps.width, caps.height);
  shell.replaceChildren(canvas);
  session = openSession(canvas, caps);
}

// The tab went to the background or came back: pictures stop meanwhile, control stays.
function screenVisible(visible) {
  if (!session) {
    if (visible && page === 'keypad' && state) startFrame();
    return;
  }
  session.visible(visible);
}

function openSession(canvas, caps) {
  const ctx = canvas.getContext('2d');
  const s = {closed: false, ws: null, decoder: null, codec: '', waiting: [], drawn: [], retry: 0,
    gesture: null, holdTimer: 0, keepTimer: 0, moveTimer: 0, pingTimer: 0, hiddenTimer: 0, control: ''};
  const text = (selector, words) => { const el = $(selector); if (el) el.textContent = words; };
  const send = message => { if (s.ws?.readyState === 1) s.ws.send(JSON.stringify(message)); };
  const ack = n => send({t: 'ack', n});

  // The words under the screen change at most once a second, however fast pictures come.
  function drew(n) {
    ack(n);
    const now = performance.now();
    s.drawn.push(now);
    s.drawn = s.drawn.filter(t => now - t < 3000);
    if (now - (s.said || 0) < 1000) return;
    s.said = now;
    const rate = s.drawn.length > 1 ? (s.drawn.length - 1) * 1000 / (now - s.drawn[0]) : 0;
    text('#frame-rate', rate >= 1 ? t('{0} pictures/second', fmtNumber(rate)) : t('Up to date'));
    text('#frame-status', t('Live · {0} · {1}', s.codec === 'h264' ? 'H.264' : 'JPEG', new Date().toLocaleTimeString(wordsLocale())));
  }
  function decoder() {
    s.waiting = [];
    s.decoder = new VideoDecoder({
      output: frame => {
        ctx.drawImage(frame, 0, 0, caps.width, caps.height);
        frame.close();
        const n = s.waiting.shift();
        if (n !== undefined) drew(n);
      },
      error: () => { // a damaged or unexpected picture: start again from a key picture
        s.decoder = null;
        send({t: 'key-frame'});
      },
    });
  }
  function picture(bytes) {
    const type = bytes[0];
    const n = (bytes[4] | bytes[5] << 8 | bytes[6] << 16 | bytes[7] << 24) >>> 0;
    const ms = (bytes[8] | bytes[9] << 8 | bytes[10] << 16 | bytes[11] << 24) >>> 0;
    const data = bytes.subarray(12);
    if (type === 3 || type === 4) { // JPEG (the panel), PNG (the PC mock panel)
      createImageBitmap(new Blob([data], {type: type === 3 ? 'image/jpeg' : 'image/png'}))
        .then(bitmap => { ctx.drawImage(bitmap, 0, 0, caps.width, caps.height); bitmap.close(); drew(n); })
        .catch(() => ack(n));
      return;
    }
    if (!s.decoder || s.decoder.state === 'closed') decoder();
    if (s.decoder.state === 'unconfigured') {
      if (type !== 1) { ack(n); send({t: 'key-frame'}); return; } // nothing to build on yet
      s.decoder.configure({codec: codecOf(data), optimizeForLatency: true});
    }
    s.waiting.push(n);
    s.decoder.decode(new EncodedVideoChunk({type: type === 1 ? 'key' : 'delta', timestamp: ms * 1000, data}));
  }
  function control(state) {
    s.control = state;
    text('#control-state', t(CONTROL_WORDS[state] || CONTROL_WORDS.busy));
    $('.screen-card')?.classList.toggle('controlling', state === 'ok');
  }
  function message(m) {
    if (m.t === 'hello') {
      s.codec = m.codec;
      s.retry = 0;
      if (m.codec !== 'h264' && s.decoder) { s.decoder.close(); s.decoder = null; }
      text('#frame-status', t('Live · {0}', m.codec === 'h264' ? 'H.264' : 'JPEG'));
      control(m.paused ? 'paused' : 'ok');
    } else if (m.t === 'codec') {
      s.codec = m.codec;
      if (s.decoder) { s.decoder.close(); s.decoder = null; }
    } else if (m.t === 'control') {
      control(m.state);
    } else if (m.t === 'bye') {
      const why = String(m.why || '');
      if (why === 'signed_out') { s.closed = true; state = null; loginPage(t(BYE_WORDS.signed_out)); return; }
      if (why.startsWith('in_use:') || BYE_WORDS[why]) {
        s.closed = true;
        text('#frame-status', t('Not connected'));
        text('#control-state', why.startsWith('in_use:')
          ? t('{0} is using the remote keypad just now. Try again when they have finished.', why.slice(7) || t('Someone'))
          : t(BYE_WORDS[why]));
      }
    }
  }
  function connect() {
    if (s.closed) return;
    text('#frame-status', s.retry ? t('Reconnecting…') : t('Connecting…'));
    const ws = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/api/v1/screen`);
    ws.binaryType = 'arraybuffer';
    s.ws = ws;
    ws.onopen = async () => {
      const wanted = pictureChoice() === 'h264' && (caps.video || []).includes('h264') && await h264Decodes() ? 'h264' : 'jpeg';
      if (ws.readyState === 1) ws.send(JSON.stringify({t: 'hello', csrf, codec: wanted}));
    };
    ws.onmessage = event => {
      if (typeof event.data === 'string') {
        try { message(JSON.parse(event.data)); } catch { /* not ours */ }
      } else {
        picture(new Uint8Array(event.data));
      }
    };
    ws.onclose = () => {
      if (s.ws !== ws || s.closed) return;
      s.ws = null;
      if (s.decoder) { try { s.decoder.close(); } catch { /* closed already */ } s.decoder = null; }
      text('#frame-status', t('Connection lost · Reconnecting…'));
      setTimeout(connect, Math.min(5000, 500 * ++s.retry));
    };
  }

  // ---- presses: a quick click is one tap, timed by the panel; a hold or a drag is streamed ----
  const point = e => {
    const r = canvas.getBoundingClientRect();
    return [Math.min(caps.width - 1, Math.max(0, Math.floor((e.clientX - r.left) * caps.width / r.width))),
      Math.min(caps.height - 1, Math.max(0, Math.floor((e.clientY - r.top) * caps.height / r.height)))];
  };
  function stream(g) {
    if (g.streamed) return;
    g.streamed = true;
    clearTimeout(s.holdTimer);
    send({t: 'down', x: g.x, y: g.y});
    s.keepTimer = setInterval(() => send({t: 'move', x: g.lx, y: g.ly}), HOLD_KEEP_MS);
  }
  function finish(e, cancelled) {
    const g = s.gesture;
    if (!g || (e && e.pointerId !== g.id)) return;
    s.gesture = null;
    clearTimeout(s.holdTimer);
    clearInterval(s.keepTimer);
    clearTimeout(s.moveTimer);
    s.moveTimer = 0;
    if (g.streamed) {
      const [x, y] = e && !cancelled ? point(e) : [g.lx, g.ly];
      send({t: 'up', x, y});
    } else if (!cancelled) {
      send({t: 'tap', x: g.x, y: g.y, ms: Math.round(performance.now() - g.at)});
    }
  }
  canvas.onpointerdown = e => {
    if (e.button !== 0 || s.gesture) return;
    e.preventDefault();
    canvas.focus({preventScroll: true});
    canvas.setPointerCapture(e.pointerId);
    const [x, y] = point(e);
    const g = {id: e.pointerId, x, y, lx: x, ly: y, at: performance.now(), streamed: false};
    s.gesture = g;
    s.holdTimer = setTimeout(() => stream(g), TAP_HOLD_MS);
  };
  canvas.onpointermove = e => {
    const g = s.gesture;
    if (!g || e.pointerId !== g.id) return;
    [g.lx, g.ly] = point(e);
    if (!g.streamed && Math.hypot(g.lx - g.x, g.ly - g.y) > MOVE_SLOP) stream(g);
    if (g.streamed && !s.moveTimer) {
      s.moveTimer = setTimeout(() => { s.moveTimer = 0; if (s.gesture === g) send({t: 'move', x: g.lx, y: g.ly}); }, MOVE_EVERY_MS);
    }
  };
  canvas.onpointerup = e => finish(e, false);
  canvas.onpointercancel = e => finish(e, true);
  canvas.onlostpointercapture = e => finish(e, true);
  canvas.oncontextmenu = e => e.preventDefault();

  // ---- the computer's keyboard: into the text box or code pad on the panel's screen ----
  const typing = target => target && target !== canvas && target !== document.body &&
    (/^(INPUT|TEXTAREA|SELECT)$/.test(target.tagName) || target.isContentEditable);
  s.onKey = e => {
    if (page !== 'keypad' || e.isComposing || e.metaKey || (e.ctrlKey !== e.altKey) || typing(e.target) || $('#dialog')?.open) return;
    const key = KEYS[e.code] && e.key !== 'Process' ? KEYS[e.code] : KEYS[e.key];
    if (key) {
      e.preventDefault();
      send({t: 'key', k: key});
    } else if ([...e.key].length === 1) {
      e.preventDefault();
      send({t: 'text', s: e.key});
    }
  };
  s.onPaste = e => {
    if (page !== 'keypad' || typing(e.target)) return;
    const pasted = (e.clipboardData?.getData('text') || '').replace(/[\u0000-\u001f\u007f]/g, '');
    if (!pasted) return;
    e.preventDefault();
    const chars = [...pasted].slice(0, 64);
    for (let i = 0; i < chars.length; i += 4) send({t: 'text', s: chars.slice(i, i + 4).join('')});
  };
  document.addEventListener('keydown', s.onKey);
  document.addEventListener('paste', s.onPaste);

  s.visible = visible => {
    clearTimeout(s.hiddenTimer);
    send({t: 'view', on: visible});
    if (!visible) s.hiddenTimer = setTimeout(() => { if (session === s) stopFrame(); }, HIDDEN_CLOSE_MS);
  };
  s.close = () => {
    if (s.closed && !s.ws) return;
    finish(null, true);
    s.closed = true;
    clearInterval(s.pingTimer);
    clearTimeout(s.hiddenTimer);
    document.removeEventListener('keydown', s.onKey);
    document.removeEventListener('paste', s.onPaste);
    canvas.onpointerdown = canvas.onpointermove = canvas.onpointerup = canvas.onpointercancel = canvas.onlostpointercapture = null;
    if (s.decoder) { try { s.decoder.close(); } catch { /* closed already */ } s.decoder = null; }
    const ws = s.ws;
    s.ws = null;
    if (ws) ws.close();
  };
  s.pingTimer = setInterval(() => send({t: 'ping'}), 5000); // the panel lets a silent page go after 20 s
  connect();
  return s;
}
