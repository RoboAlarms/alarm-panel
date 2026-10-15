/* RoboAlarms Flasher page (docs/features/34): a guided setup in three parts - Plan (the panel and
 * what is connected), Wire (get ready, the full wiring, a check before power; no panel needed,
 * BL-117) and Install (Connect, Check the panel, Install, Update or Reset, Test and finish) - and
 * Advanced tools.
 * Talks only to the local engine (server.py) with the per-run token from the URL fragment. Text
 * from boards is always set as textContent, never as HTML; icons and drawings are built from
 * constants. Nothing animates or blinks (FLS-017); live words change at most once a second. */
'use strict';

const TOKEN = location.hash.slice(1);

/* ---- small helpers ------------------------------------------------------------------------ */
function h(tag, attrs, ...kids) {
  const el = document.createElement(tag);
  setAttrs(el, attrs);
  append(el, kids);
  return el;
}
const SVGNS = 'http://www.w3.org/2000/svg';
function s(tag, attrs, ...kids) {
  const el = document.createElementNS(SVGNS, tag);
  setAttrs(el, attrs);
  append(el, kids);
  return el;
}
function setAttrs(el, attrs) {
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === undefined || v === null || v === false) continue;
    if (k === 'text') el.textContent = v;
    else if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2), v);
    else if (v === true) el.setAttribute(k, '');
    else el.setAttribute(k, String(v));
  }
}
function append(el, kids) {
  for (const kid of kids.flat(3)) {
    if (kid === null || kid === undefined || kid === false || kid === '') continue;
    el.append(kid instanceof Node ? kid : document.createTextNode(String(kid)));
  }
}
const $ = (id) => document.getElementById(id);
const put = (el, ...kids) => { append(el, kids); return el; };
const clear = (el) => { while (el.firstChild) el.removeChild(el.firstChild); return el; };
const cap = (t) => (t ? t[0].toUpperCase() + t.slice(1) : t);
const ordinal = (n) => n + (n % 100 >= 11 && n % 100 <= 13 ? 'th' : ['th', 'st', 'nd', 'rd'][n % 10] || 'th');

/* Lucide icons (ISC licence): constant markup, the set the panel's touchscreen uses. */
const ICONS = {
  usb: '<circle cx="10" cy="7" r="1"/><circle cx="4" cy="20" r="1"/><path d="M4.7 19.3 19 5"/><path d="m21 3-3 1 2 2Z"/><path d="M9.26 7.68 5 12l2 5"/><path d="m10 14 5 2 3.5-3.5"/><path d="m18 12 1-1 1 1-1 1Z"/>',
  cpu: '<path d="M12 20v2"/><path d="M12 2v2"/><path d="M17 20v2"/><path d="M17 2v2"/><path d="M2 12h2"/><path d="M2 17h2"/><path d="M2 7h2"/><path d="M20 12h2"/><path d="M20 17h2"/><path d="M20 7h2"/><path d="M7 20v2"/><path d="M7 2v2"/><rect x="4" y="4" width="16" height="16" rx="2"/><rect x="8" y="8" width="8" height="8" rx="1"/>',
  radio: '<path d="M16.247 7.761a6 6 0 0 1 0 8.478"/><path d="M19.075 4.933a10 10 0 0 1 0 14.134"/><path d="M4.925 19.067a10 10 0 0 1 0-14.134"/><path d="M7.753 16.239a6 6 0 0 1 0-8.478"/><circle cx="12" cy="12" r="2"/>',
  clock: '<circle cx="12" cy="12" r="10"/><path d="M12 6v6h4"/>',
  check: '<path d="M20 6 9 17l-5-5"/>',
  circleCheck: '<circle cx="12" cy="12" r="10"/><path d="m16 9-5.5 5.5L8 12"/>',
  triangle: '<path d="m21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3"/><path d="M12 9v4"/><path d="M12 17h.01"/>',
  chevron: '<path d="m9 18 6-6-6-6"/>',
  sliders: '<path d="M14 17H5"/><path d="M19 7h-9"/><circle cx="17" cy="17" r="3"/><circle cx="7" cy="7" r="3"/>',
  terminal: '<path d="M12 19h8"/><path d="m4 17 6-6-6-6"/>',
  refresh: '<path d="M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8"/><path d="M21 3v5h-5"/><path d="M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16"/><path d="M8 16H3v5"/>',
  download: '<path d="M12 2v8"/><path d="m16 6-4 4-4-4"/><rect width="20" height="8" x="2" y="14" rx="2"/><path d="M6 18h.01"/><path d="M10 18h.01"/>',
  shield: '<path d="M20 13c0 5-3.5 7.5-7.66 8.95a1 1 0 0 1-.67-.01C7.5 20.5 4 18 4 13V6a1 1 0 0 1 1-1c2 0 4.5-1.2 6.24-2.72a1.17 1.17 0 0 1 1.52 0C14.51 3.81 17 5 19 5a1 1 0 0 1 1 1z"/><path d="m9 12 2 2 4-4"/>',
  plug: '<path d="M12 22v-5"/><path d="M15 8V2"/><path d="M17 8a1 1 0 0 1 1 1v4a4 4 0 0 1-4 4h-4a4 4 0 0 1-4-4V9a1 1 0 0 1 1-1z"/><path d="M9 8V2"/>',
  info: '<circle cx="12" cy="12" r="10"/><path d="M12 16v-4"/><path d="M12 8h.01"/>',
  folder: '<path d="m6 14 1.5-2.9A2 2 0 0 1 9.24 10H20a2 2 0 0 1 1.94 2.5l-1.54 6a2 2 0 0 1-1.95 1.5H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h3.9a2 2 0 0 1 1.69.9l.81 1.2a2 2 0 0 0 1.67.9H18a2 2 0 0 1 2 2v2"/>',
  siren: '<path d="M7 18v-6a5 5 0 1 1 10 0v6"/><path d="M5 21a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-1a2 2 0 0 0-2-2H7a2 2 0 0 0-2 2z"/><path d="M21 12h1"/><path d="M18.5 4.5 18 5"/><path d="M2 12h1"/><path d="M12 2v1"/><path d="m4.929 4.929.707.707"/><path d="M12 12v6"/>',
  right: '<path d="M5 12h14"/><path d="m12 5 7 7-7 7"/>',
  left: '<path d="m12 19-7-7 7-7"/><path d="M19 12H5"/>',
  wrench: '<path d="M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.106-3.105c.32-.322.863-.22.983.218a6 6 0 0 1-8.259 7.057l-7.91 7.91a1 1 0 0 1-2.999-3l7.91-7.91a6 6 0 0 1 7.057-8.259c.438.12.54.662.219.984z"/>',
  archive: '<rect width="20" height="5" x="2" y="3" rx="1"/><path d="M4 8v11a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8"/><path d="M10 12h4"/>',
  search: '<path d="m21 21-4.34-4.34"/><circle cx="11" cy="11" r="8"/>',
  x: '<path d="M18 6 6 18"/><path d="m6 6 12 12"/>',
  tower: '<path d="M4.9 16.1C1 12.2 1 5.8 4.9 1.9"/><path d="M7.8 4.7a6.14 6.14 0 0 0-.8 7.5"/><circle cx="12" cy="9" r="2"/><path d="M16.2 4.8c2 2 2.26 5.11.8 7.47"/><path d="M19.1 1.9a9.96 9.96 0 0 1 0 14.1"/><path d="M9.5 18h5"/><path d="m8 22 4-11 4 11"/>',
  door: '<path d="M10 21H2"/><path d="M10 3H7a2 2 0 00-2 2v16"/><path d="M14 12h.01"/><path d="M19 21V5a2 2 0 00-1.675-1.974l-6.163-1.013A1 1 0 0010 3v18a1 1 0 001.124.992z"/><path d="M22 21h-3"/>',
  undo: '<path d="M9 14 4 9l5-5"/><path d="M4 9h10.5a5.5 5.5 0 0 1 5.5 5.5a5.5 5.5 0 0 1-5.5 5.5H11"/>',
  book: '<path d="M12 7v14"/><path d="M3 18a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1h5a4 4 0 0 1 4 4 4 4 0 0 1 4-4h5a1 1 0 0 1 1 1v13a1 1 0 0 1-1 1h-6a3 3 0 0 0-3 3 3 3 0 0 0-3-3z"/>',
  printer: '<path d="M6 18H4a2 2 0 0 1-2-2v-5a2 2 0 0 1 2-2h16a2 2 0 0 1 2 2v5a2 2 0 0 1-2 2h-2"/><path d="M6 9V3a1 1 0 0 1 1-1h10a1 1 0 0 1 1 1v6"/><rect x="6" y="14" width="12" height="8" rx="1"/>',
  power: '<path d="M12 2v10"/><path d="M18.4 6.6a9 9 0 1 1-12.77.04"/>',
  listChecks: '<path d="M13 5h8"/><path d="M13 12h8"/><path d="M13 19h8"/><path d="m3 17 2 2 4-4"/><path d="m3 7 2 2 4-4"/>',
  sun: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2m0 16v2M2 12h2m16 0h2M5 5l1.5 1.5m11 11L19 19M5 19l1.5-1.5m11-11L19 5"/>',
};
const iconCache = {};
function icon(name) {
  if (!iconCache[name]) {
    const t = document.createElement('template');
    t.innerHTML = `<svg class="ic" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${ICONS[name] || ''}</svg>`;
    iconCache[name] = t.content.firstChild;
  }
  return iconCache[name].cloneNode(true);
}

async function api(name, body, quiet) {
  try {
    const r = await fetch('/api/' + name, {
      method: 'POST',
      headers: { 'X-Flasher-Token': TOKEN, 'Content-Type': 'application/json' },
      body: JSON.stringify(body || {}),
    });
    const j = await r.json();
    if (!j.ok && !quiet) {
      const e = j.error || { title: 'Something went wrong.' };
      showError(e);
    }
    return j;
  } catch (err) {
    showError({ title: 'The Flasher engine is not answering.', advice: 'Close this window and start the Flasher again.' });
    return { ok: false, error: { title: 'The Flasher engine is not answering.' } };
  }
}

/* A failed action stays on the screen it belongs to until the next action (not a toast). */
function showError(e) {
  S.error = { title: e.title || 'Something went wrong.', advice: e.advice || '' };
  render(false);
}
function errorNote() {
  if (!S.error) return null;
  return h('div', { class: 'note bad', role: 'alert' }, icon('triangle'),
    h('div', {}, h('b', { text: S.error.title }), S.error.advice ? ' ' + S.error.advice : ''));
}

let toastTimer = 0;
function toast(text) {
  const t = $('toast');
  t.textContent = text;
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, 4500);
}

function ask({ title, body, ok = 'Continue', danger = false, check = '' }) {
  return new Promise((resolve) => {
    $('modal-title').textContent = title;
    const b = clear($('modal-body'));
    for (const para of [].concat(body)) if (para) b.append(para instanceof Node ? para : h('p', { text: para }));
    let box = null;
    if (check) {
      box = h('input', { type: 'checkbox' });
      b.append(h('label', {}, box, h('span', { text: check })));
    }
    const okBtn = $('modal-ok');
    okBtn.textContent = ok;
    okBtn.className = 'btn primary' + (danger ? ' danger' : '');
    okBtn.disabled = !!box;
    if (box) box.addEventListener('change', () => { okBtn.disabled = !box.checked; });
    $('modal').hidden = false;
    (box || okBtn).focus();
    const done = (v) => {
      $('modal').hidden = true;
      okBtn.onclick = null;
      $('modal-cancel').onclick = null;
      resolve(v);
    };
    okBtn.onclick = () => done(true);
    $('modal-cancel').onclick = () => done(false);
  });
}
document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape' && !$('modal').hidden) $('modal-cancel').click();
});

function mb(bytes) { return Math.round(bytes / 1048576) + ' MB'; }
function ago(t) {
  const secs = Math.max(0, Math.round(Date.now() / 1000 - t));
  if (secs < 5) return 'just now';
  if (secs < 60) return secs + ' s ago';
  if (secs < 3600) return Math.round(secs / 60) + ' min ago';
  return Math.round(secs / 3600) + ' h ago';
}
function versionTuple(v) { return (String(v || '').split('-')[0].match(/\d+/g) || []).slice(0, 3).map(Number); }
function older(a, b) {
  const x = versionTuple(a), y = versionTuple(b);
  for (let i = 0; i < 3; i++) { if ((x[i] || 0) !== (y[i] || 0)) return (x[i] || 0) < (y[i] || 0); }
  return false;
}

/* ---- page state --------------------------------------------------------------------------- */
const S = {
  state: null,        // the engine's snapshot, kept current by its events
  mode: 'guided',
  port: null,         // the panel being set up
  step: 0,            // guided: 0 parts, 1 get ready, 2 full wiring, 3 check before power, 4 connect, 5 check, 6 install, 7 test
  sub: '',            // guided, off the steps: 'places' (advanced) or 'print'
  choice: null,       // the connected parts (see parts.py)
  choiceFor: '',      // the port the choice belongs to
  readingParts: false,
  action: 'update',   // a panel running RoboAlarms: 'update' or 'reset'
  backup: true,
  language: '',       // the panel's language at install: '' English, or a locale the bundle carries
  resetOk: false,
  imageOk: false,
  skipParts: false,   // the run keeps the panel's own parts settings (set when it starts)
  touched: false,     // the parts were changed here (else a panel running RoboAlarms keeps its own)
  tick: {},           // the full wiring's tick list
  ready: {},          // get ready's tick lists
  checks: {},         // check before power
  fullFocus: '',      // the full wiring's filter: a part's key, or '' for all
  checkFocus: '',     // the same filter on Check before power, its own so that page opens on every part
  movedNote: {},      // a part moved to make room for another: its key -> words
  pickBoard: false,
  planBoard: '',      // the panel chosen in step 1, before the panel itself says (a catalogue id)
  boardTold: {},      // port -> the step-1 choice was passed on to the engine for it
  devAsked: {},       // board -> its only-a-development-build pop-up was shown (FLS-033)
  runJob: '',         // the job this guided run started
  finished: false,    // the guided run reached "done"
  tool: 'ports',
  showOther: false,
  showQuiet: false,
  open: {},
  logs: {},           // port -> {el, seq}
  inputs: {},         // port -> the console's command box
  history: [],
  histPos: -1,
  rfPort: null,
  restorePick: '',
  error: null,
  unplugged: false,
};

function defaultChoice() {
  return { rf: 'none', rf_place: 'socket', rf_pin: null, clock: false, clock_place: 'j13', zigbee: false,
    zwave: false, zwave_place: 'socket' };
}
S.choice = defaultChoice();

/* ---- the engine's state ------------------------------------------------------------------- */
async function loadState() {
  const r = await fetch('/api/state', { headers: { 'X-Flasher-Token': TOKEN } });
  const j = await r.json();
  S.state = j.state;
  autoSelect();
  render(false);
}

async function streamEvents() {
  for (;;) {
    try {
      const r = await fetch('/api/events', { headers: { 'X-Flasher-Token': TOKEN } });
      const reader = r.body.getReader();
      const dec = new TextDecoder();
      let buf = '';
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        buf += dec.decode(value, { stream: true });
        let i;
        while ((i = buf.indexOf('\n\n')) >= 0) {
          const block = buf.slice(0, i);
          buf = buf.slice(i + 2);
          for (const line of block.split('\n')) {
            if (line.startsWith('data: ')) dispatch(JSON.parse(line.slice(6)));
          }
        }
      }
    } catch (e) { /* the engine is restarting or gone: try again */ }
    await new Promise((res) => setTimeout(res, 1000));
    try { await loadState(); } catch (e) { /* not back yet */ }
  }
}

function onEvent(ev) {
  const st = S.state;
  if (!st) return;
  switch (ev.type) {
    case 'ports':
      st.ports = ev.ports;
      st.driver_issues = ev.driver_issues || [];
      st.devices = ev.devices;
      if (!zbPorts().length && !jobRunning()) { S.zb.alone = false; S.zb.read = {}; S.zb.reading = {}; }   // unplugged: the tick is asked again
      autoSelect();
      render(false);
      break;
    case 'device':
      st.devices[ev.port] = ev.device;
      if (!liveOnly()) render(false);
      break;
    case 'bundle':
      st.bundle = ev.bundle;
      st.bundle_error = ev.error;
      if (ev.firmware !== undefined) st.firmware = ev.firmware;
      Object.assign(st.devices, ev.devices || {});
      S.zb.read = {}; S.zb.done = false;   // another file: what the module needs is looked at again
      render(false);
      break;
    case 'firmware':      // the firmware server: a release looked up, downloading or ready (FLS-032)
      st.firmware = ev.firmware;
      st.bundle = ev.bundle;
      st.bundle_error = ev.bundle_error || '';
      if (ev.catalogue) st.catalogue = ev.catalogue;
      Object.assign(st.devices, ev.devices || {});
      syncProgram();
      if (!fillFirmware() && !liveOnly()) render(false);
      break;
    case 'job': {
      const was = st.job && st.job.id === ev.job.id ? st.job.state : '';
      st.job = ev.job;
      if (ev.job.id === S.radioJob && ev.job.state === 'done') { S.zb.done = true; S.zb.read = {}; }
      if (ev.job.id === S.radioJob && ev.job.state !== 'running') { S.zb.alone = false; S.zb.keep = false; }   // the next module: both asked again
      if (ev.job.state !== 'running' && was === 'running') refreshBackups();
      if (!fillJobCard() || ev.job.state !== was) render(false);
      break;
    }
    case 'parts': {
      st.parts = st.parts || {};
      st.parts[ev.port] = ev.parts;
      const a = S.cp.applying;
      if (a && a.port === ev.port && (!ev.parts || ev.parts.state !== 'running')) {   // Undo goes back to what the panel now uses (FLS-029)
        if (ev.parts && ev.parts.state === 'done') { S.cp.panel = { pins: a.pins, resetNC: a.resetNC, clockPlace: a.clockPlace }; S.cp.cleared = false; }
        S.cp.applying = null;
      }
      if (!fillPartsRun() || !ev.parts || ev.parts.state !== 'running') render(false);
      break;
    }
    case 'console': {
      const list = (st.consoles[ev.port] = st.consoles[ev.port] || []);
      list.push(...ev.lines);
      if (list.length > 3000) list.splice(0, list.length - 3000);
      appendLog(ev.port);
      break;
    }
    case 'rf': {
      st.rf[ev.port] = ev.rf;
      if (ev.saved) toast('Recording saved: ' + ev.saved);
      if (ev.capture_error) toast('Recording not saved: ' + ev.capture_error);   // RF22: only a whole one is saved
      const rec = $('rf-record');
      if (rec) rec.disabled = rfRecording();   // RF21: one recording at a time
      fillRfLive();
      break;
    }
  }
}

/* PERF21: after the engine dropped events for this page (it fell too far behind), the snapshot
 * a reconnect fetches; the job, the port list and a parts run being applied go through their own
 * handlers, so a job that ended meanwhile ends here too and the guided run moves on. The snapshot
 * is older than any event that arrives while it is fetched: those (all but console rows) are held
 * and replayed in order after it is applied, so a job that ended meanwhile does not go back to
 * running; a resync asked meanwhile is coalesced into one more after this one. */
let resyncing = false, resyncAgain = false, held = [];
function dispatch(ev) {
  if (ev.type === 'resync') { if (resyncing) resyncAgain = true; else resync(); return; }
  if (resyncing && ev.type !== 'console') { held.push(ev); return; }
  onEvent(ev);
}
async function resync() {
  resyncing = true;
  try {
    await fetchSnapshot();
  } finally {
    resyncing = false;
    const later = held; held = [];
    for (const ev of later) onEvent(ev);
    if (resyncAgain) { resyncAgain = false; resync(); }
  }
}
async function fetchSnapshot() {
  let fresh;
  try {
    const r = await fetch('/api/state', { headers: { 'X-Flasher-Token': TOKEN } });
    fresh = (await r.json()).state;
  } catch (e) { return; }   // the stream reconnects and loads it then
  const old = S.state;
  if (!old) { S.state = fresh; autoSelect(); render(false); return; }
  const j = fresh.job;
  fresh.job = old.job;
  S.state = fresh;
  onEvent({ type: 'ports', ports: fresh.ports, driver_issues: fresh.driver_issues, devices: fresh.devices });
  if (j) onEvent({ type: 'job', job: j }); else S.state.job = null;
  const a = S.cp.applying;
  if (a) onEvent({ type: 'parts', port: a.port, parts: (fresh.parts || {})[a.port] || null });
  render(false);
}

/* A list (a select) the person has opened: a re-render would close it. Pressing it or opening it
 * from the keyboard sets this; a choice, Escape, Tab or the focus moving on clears it, so a select
 * that keeps the focus after a choice no longer holds device updates back (FLS-029). */
let listOpen = false;
document.addEventListener('pointerdown', (e) => { listOpen = !!(e.target.closest && e.target.closest('select')); }, true);
document.addEventListener('keydown', (e) => {
  if (e.target.tagName !== 'SELECT') return;
  if (e.key === 'Escape' || e.key === 'Tab') listOpen = false;
  else if (e.key === ' ' || e.key === 'F4' || (e.altKey && (e.key === 'ArrowDown' || e.key === 'ArrowUp'))) listOpen = true;
}, true);
for (const type of ['change', 'focusout']) document.addEventListener(type, (e) => { if (e.target.tagName === 'SELECT') listOpen = false; }, true);

/* True while the person types in the console or has a list open: device updates then leave the page alone. */
function liveOnly() {
  const a = document.activeElement;
  return !!(a && $('app').contains(a) && ((a.tagName === 'INPUT' && a.type === 'text') || (a.tagName === 'SELECT' && listOpen)));
}

async function refreshBackups() {
  try {
    const r = await fetch('/api/state', { headers: { 'X-Flasher-Token': TOKEN } });
    S.state.backups = (await r.json()).state.backups;
    if (!liveOnly()) render(false);   // a new backup shows in the list at once
  } catch (e) { /* keep the old list */ }
}

/* ---- what is known ------------------------------------------------------------------------ */
const ports = () => (S.state && S.state.ports) || [];
const portInfo = (port) => ports().find((p) => p.device === port);
const dev = (port) => ((S.state && S.state.devices) || {})[port || S.port] || null;
const job = () => (S.state && S.state.job) || null;
const jobRunning = () => { const j = job(); return !!(j && j.state === 'running'); };
const partsRun = (port) => ((S.state && S.state.parts) || {})[port || S.port] || null;
const rfRecording = (port) => { const rf = ((S.state && S.state.rf) || {})[port || S.port]; return !!(rf && rf.recording); };
const rfOf = (port) => ((S.state && S.state.rf) || {})[port || S.port] || { status: {}, heard: [], raw_on: false };
function boardOf(port) {
  const d = dev(port);
  if (!d) return null;
  if (d.board_info) return d.board_info;
  const det = d.detection || {};
  return (S.state.catalogue || {})[det.board] || (d.probe && (S.state.catalogue || {})[d.probe.board]) || null;
}
function chosenImage(port) {
  const d = dev(port);
  return d ? (d.options || []).find((o) => o.key === d.image_key) || null : null;
}
const runsRoboAlarms = (port) => { const d = dev(port); return !!(d && d.probe && d.probe.answered); };

/* The firmware server (FLS-032): the firmware comes from it unless a file was opened by hand. A
 * device's `firmware` is its board's release on its channel: looked up, downloading, ready. */
const fw = () => (S.state && S.state.firmware) || null;
const serving = () => { const f = fw(); return !!(f && !f.from_file); };
const CHANNELS = { stable: 'Stable', beta: 'Beta', alpha: 'Alpha', dev: 'Dev' };
const FOUND = ['available', 'downloading', 'ready'];
const fwKey = (t) => `${t.board}/${t.channel}`;
const fwFraction = (t) => (t.size ? Math.max(0, Math.min(1, t.done / t.size)) : 0);
const boardName = (id) => ((S.state.catalogue || {})[id] || {}).name || id;
/* The channel the person chose for a board this run (the version list, or Software for all), or null. */
const picked = (board) => ((fw() || {}).picked || {})[board] || null;
const fwTarget = (board, channel) => ((fw() || {}).targets || []).find((t) => t.board === board && t.channel === channel) || null;
/* A channel's own release for a board: a channel also sees the steadier ones, which are theirs. */
function ownRelease(board, c) {
  const t = fwTarget(board, c);
  return t && FOUND.includes(t.state) && t.release_channel === c ? t : null;
}
function fwWords(t) {
  const name = boardName(t.board);
  switch (t.state) {
    case 'ready': return `RoboAlarms ${t.ready_version} for the ${name} is downloaded` + (t.offline ? ' (the firmware server can\'t be reached: this is the one downloaded before)' : '.');
    case 'downloading': return `Downloading RoboAlarms ${t.version} for the ${name}: ${Math.round(fwFraction(t) * 100)} %`;
    case 'available': return t.dev_offered ? `Only a development version, RoboAlarms ${t.version}, is available for the ${name}.` : `RoboAlarms ${t.version} is the newest. It downloads when a panel needs it.`;
    case 'none': return `There is no RoboAlarms release for the ${name}` + (picked(t.board) ? ` on ${CHANNELS[t.channel] || t.channel}` : '') + ' yet.';
    case 'failed': return (t.error && t.error.title) || 'The firmware could not be downloaded.';
    default: return `Asking the firmware server for the newest RoboAlarms for the ${name}…`;
  }
}
/* What stands between the device and an install, from the firmware server; null when nothing does. */
function fwNote(d) {
  const t = d && d.firmware;
  if (!serving() || !t) return null;
  if (t.dev_offered) {
    return h('div', { class: 'note info' }, icon('info'), h('div', {}, h('b', { text: fwWords(t) }), ' There is no Stable, Beta or Alpha release for it yet.',
      h('div', {}, h('button', { class: 'btn small', type: 'button', 'data-act': 'fw-dev', 'data-v': t.board }, 'Use the development version…'))));
  }
  if (t.state === 'ready') return t.offline ? note('info', 'info', 'The firmware server can\'t be reached.', `RoboAlarms ${t.ready_version} was downloaded before: that is what installs.`) : null;
  if (t.state === 'failed') {
    return h('div', { class: 'note bad' }, icon('triangle'), h('div', {}, h('b', { text: (t.error && t.error.title) || 'The firmware could not be downloaded.' }), ' ', (t.error && t.error.advice) || '',
      h('div', {}, h('button', { class: 'btn small', type: 'button', 'data-act': 'fw-get', 'data-v': t.board }, icon('refresh'), 'Try again'))));
  }
  if (t.state === 'none') return note('info', 'info', fwWords(t), picked(t.board) ? 'Choose another version in the list at the bottom left.' : '');
  return h('div', { class: 'card fw-dl', 'data-fw': fwKey(t) }, h('b', { class: 'fw-words', text: fwWords(t) }),
    bar(fwFraction(t)),
    h('small', { class: 'muted', text: 'From the firmware server, checked before anything is installed. The guided setup goes on meanwhile.' }));
}
/* Why the next step waits on the firmware server, or ''. */
function fwWhy(d) {
  const t = d && d.firmware;
  if (!serving() || !t || t.state === 'ready') return '';
  if (t.dev_offered) return 'Only a development version is available: choose it first.';
  return t.state === 'failed' ? 'The firmware could not be downloaded.' : t.state === 'none' ? 'There is no release for this board yet.' : 'Waiting for the firmware to download.';
}
/* The board the version list is for: the panel's, else the one chosen in step 1. */
function fwBoard() {
  const cat = S.state.catalogue || {};
  const d = dev();
  const id = d && (d.board || (d.detection || {}).board || (d.probe || {}).board);
  if (id && cat[id]) return id;
  const pb = planBoard();
  return pb ? Object.keys(cat).find((k) => cat[k] === pb) || '' : '';
}
/* What a development build is, said before one is used (FLS-033, decided 2026-10-12). */
function askDev(board, only) {
  const t = fwTarget(board, 'dev') || {};
  return ask({
    title: only ? `Only a development version is available for the ${boardName(board)}` : 'Use the development version?',
    body: [(only ? 'There is no Stable, Beta or Alpha release for it yet. ' : '') + (t.version ? `RoboAlarms ${t.version} is` : 'It is') + ' a development build, for testing RoboAlarms.',
      'It comes with extra tools for developers: eight test zones to try the alarm with, the service console with its test commands, and fuller logs. Use it on a test panel, not on one that protects a home.'],
    ok: 'Use the development version',
  });
}
/* The person's version for this run (FLS-033): Dev only once they have read what it is. */
async function pickVersion(board, channel, every) {
  if (channel === 'dev' && !(await askDev(every ? '' : board, false))) { render(false); return; }
  api('firmware_channel', every ? { channel } : { channel, board });
}
/* A board with nothing but a development build: offered once, in a pop-up. */
function offerDev() {
  const f = fw();
  if (!serving() || !f || !$('modal').hidden) return;
  const board = fwBoard();
  if (!board || !(f.dev_offered || []).includes(board) || S.devAsked[board]) return;
  S.devAsked[board] = true;
  setTimeout(async () => { if (await askDev(board, true)) api('firmware_channel', { channel: 'dev', board }); }, 0);
}
/* A download's progress, filled in where it is shown without building the page again; false when
 * something else changed (a release ready, failed, another version), which needs a render. */
let fwSig = '';
function fillFirmware() {
  const f = fw();
  const ts = (f && f.targets) || [];
  const sig = JSON.stringify([f && f.from_file, f && f.pick, f && f.picked, f && f.chosen, f && f.dev_offered, f && f.offline, f && f.error, f && f.program, f && f.boards_version,
    ts.map((t) => [t.board, t.channel, t.state, t.version, t.release_channel, t.ready_version, t.error && t.error.title, t.offline]), ((S.state.bundle || {}).images || []).length]);
  if (sig !== fwSig) { fwSig = sig; return false; }
  for (const el of document.querySelectorAll('[data-fw]')) {
    const t = ts.find((x) => fwKey(x) === el.dataset.fw);
    if (!t) continue;
    const w = el.querySelector('.fw-words');
    if (w) w.textContent = fwWords(t);
    const i = el.querySelector('.bar > i');
    if (i) i.style.width = Math.round(fwFraction(t) * 100) + '%';
  }
  return true;
}
/* The header's word of a newer Flasher: a link to its download, never an update by itself. */
function syncProgram() {
  const f = fw();
  const btn = $('flasher-update');
  if (!btn) return;
  const p = f && f.program;
  btn.hidden = !p;
  if (p) clear(btn).append(icon('download'), `Flasher ${p.version} is available`);
}
const busyReason = (port) => { const d = dev(port); return (d && d.probe && d.probe.busy_reason) || ''; };
/* A port with Espressif's own USB id may be the Zigbee module: never the panel (FLS-030). */
const isModulePort = (p) => { const d = dev(p.device); return !!(d && d.needs_alone) || p.usb_id === '303A:1001'; };
const candidates = () => ports().filter((p) => (p.kind === 'esp' || p.kind === 'usb') && !isModulePort(p));

function autoSelect() {
  const list = ports();
  if (S.port && list.some((p) => p.device === S.port && !isModulePort(p))) return;
  if (S.port && !(jobRunning() && job().port === S.port)) {
    S.unplugged = true;          // the panel went away: planning and wiring carry on without it
    if (S.mode === 'guided' && !S.finished && S.step >= 5) S.step = 4;
  }
  const esp = list.filter((p) => p.kind === 'esp' && !isModulePort(p));
  const pick = esp[0] || list.find((p) => p.kind === 'usb' && !isModulePort(p));
  selectPort(pick ? pick.device : null);
}

function selectPort(port) {
  if (S.rfPort && S.rfPort !== port) { api('rf_stop', { port: S.rfPort }, true); S.rfPort = null; }
  if (port !== S.port) {
    S.port = port;
    S.choiceFor = '';
    S.customOk = null;   // asked again by parts_read for this panel
    S.pickBoard = false;
    S.error = null;
  }
  if (port) {
    S.unplugged = false;
    const p = portInfo(port);
    const d = dev(port);
    if (p && p.kind === 'esp' && d && !d.probe && !d.busy && !jobRunning()) api('probe', { port }, true);
  }
}

/* ---- connected parts: the rules (the same as parts.py, which the engine checks again) ----- */
function norm(c) {
  const d = defaultChoice();
  return Object.assign(d, c || {});
}
function j7Place(board) { return ((board && board.rf && board.rf.places) || {}).j7 || null; }
function clockJ7(board) { return ((board && board.rtc && board.rtc.places) || {}).j7 || null; }
function zwPlace(board, id) { return ((board && board.zwave && board.zwave.places) || {})[id] || null; }
/* The Z-Wave radio on J7 and the clock on J7 may share pins (the board's J7 I2C pair). */
function zwClockClash(board) {
  const z = zwPlace(board, 'j7');
  const k = clockJ7(board);
  return !!(z && k && [z.tx, z.rx].some((n) => n === k.sda || n === k.scl));
}

function problems(choice, board) {
  const c = norm(choice);
  if (c.layout === 'custom') return [];   // its own pins: parts_check judges them (Custom pins)
  const rf = (board && board.rf) || {};
  const out = [];
  const name = { module: 'The receiver module', cc1101: 'The CC1101' }[c.rf];
  if (name) {
    if (c.rf_place === 'socket' && c.zigbee) out.push(`${name} can't use the radio socket: the Zigbee module is in it.`);
    else if (c.rf === 'cc1101' && c.rf_place === 'j7' && !j7Place(board)) out.push('This board has no J7 place for a CC1101.');
    else if (c.rf === 'module' && c.rf_place === 'j7' && !(rf.pins || []).includes(c.rf_pin)) out.push('Choose the J7 pin the receiver\'s DATA is on.');
  }
  if (c.rf === 'auto' && c.zigbee) out.push('“Not sure” can\'t be used with the Zigbee module fitted: the search would probe its pins. Choose the receiver.');
  if (c.clock && board && !board.rtc) out.push('This board has no place for a clock module.');
  if (c.clock && c.clock_place === 'j7' && !clockJ7(board)) out.push('This board has no J7 place for the clock module.');
  if (c.zwave) {
    if (!board || !board.zwave) out.push('This board has no place for a Z-Wave radio.');
    else if (!zwPlace(board, c.zwave_place)) out.push('The Z-Wave radio: unknown place.');
    else if (c.zwave_place === 'socket' && c.zigbee) out.push('The Z-Wave radio can\'t use the radio socket: the Zigbee module is in it.');
    else if (c.zwave_place === 'j7' && c.clock && c.clock_place === 'j7' && zwClockClash(board)) {
      const k = clockJ7(board);
      out.push(`The Z-Wave radio and the clock module can't both use J7 IO${k.sda} and IO${k.scl}: put the clock on the I2C connector.`);
    }
  }
  return out;
}
/* Where each part goes when only what is connected was chosen (BL-117): J7 for the receiver module
 * and the clock, the radio socket for one radio while it is free (the CC1101 first), else J7, the
 * clock on J13 when the Z-Wave radio takes its J7 pins. parts.py recommended() is the same, and its
 * test places every mix of parts without a conflict. */
function recommend(choice, board) {
  const c = norm(choice);
  const pins = ((board && board.rf) || {}).pins || [];
  let socketFree = !c.zigbee;
  if (c.rf === 'module') {
    if (pins.length) { c.rf_place = 'j7'; if (!pins.includes(c.rf_pin)) c.rf_pin = pins.includes(3) ? 3 : pins[0]; } else c.rf_place = 'socket';
  } else if (c.rf === 'cc1101') {
    if (socketFree || !j7Place(board)) { c.rf_place = 'socket'; socketFree = false; } else c.rf_place = 'j7';
  }
  if (c.zwave) {
    if (socketFree && zwPlace(board, 'socket')) { c.zwave_place = 'socket'; socketFree = false; } else if (zwPlace(board, 'j7')) c.zwave_place = 'j7';
  }
  if (c.clock) c.clock_place = clockJ7(board) && !(c.zwave && c.zwave_place === 'j7' && zwClockClash(board)) ? 'j7' : 'j13';
  return c;
}
const sameChoice = (a, b) => JSON.stringify(norm(a)) === JSON.stringify(norm(b));
function ccJ7Pins(board) {
  const p = j7Place(board);
  return p ? [p.spi.sck, p.spi.mosi, p.spi.miso, p.spi.cs, p.gdo0] : [];
}
function zonesLeft(choice, board) {
  const c = norm(choice);
  const used = new Set();
  if (c.rf === 'module' && c.rf_place === 'j7' && c.rf_pin != null) used.add(c.rf_pin);
  if (c.rf === 'cc1101' && c.rf_place === 'j7') ccJ7Pins(board).forEach((n) => used.add(n));
  return (((board && board.rf) || {}).pins || []).filter((n) => !used.has(n));
}
function choiceLong(c, board) {
  c = norm(c);
  const p = [];
  if (c.layout === 'custom') {
    const n = c.pins ? Object.keys(c.pins).length : 0;
    p.push(`your own board on J7: ${n} part${n === 1 ? '' : 's'}${c.clock && c.clock_place === 'j13' ? ', the clock on the I2C connector' : ''}`);
    if (c.zigbee) p.push('Zigbee module in the socket');
    return p.join('; ');
  }
  if (c.rf === 'module') p.push(c.rf_place === 'j7' ? `433 MHz receiver module, DATA on J7 IO${c.rf_pin}` : '433 MHz receiver module on the radio socket');
  else if (c.rf === 'cc1101') p.push(c.rf_place === 'j7' ? 'CC1101 on the J7 header' : 'CC1101 on the radio socket');
  else if (c.rf === 'auto') p.push('433 MHz receiver, found by the panel');
  else p.push('no 433 MHz receiver (switched off)');
  const k = clockJ7(board) || {};
  const z = zwPlace(board, 'j7') || {};
  if (c.clock) p.push(c.clock_place === 'j7' ? `clock module on J7 IO${k.sda} and IO${k.scl}` : 'clock module on the I2C connector');
  if (c.zigbee) p.push('Zigbee module in the socket');
  if (c.zwave) p.push(c.zwave_place === 'j7' ? `Z-Wave radio on J7 IO${z.tx} and IO${z.rx}` : 'Z-Wave radio on the radio socket');
  return p.join('; ');
}
/* The boards the Flasher installs, as [id, entry] (FLS-021: a board not offered is never listed). */
const offeredBoards = () => Object.entries((S.state && S.state.catalogue) || {}).filter(([, x]) => x.offered !== false);
/* The board the parts are planned for: the panel's once it is known, before that the one the person
 * chose in step 1 (or the only one offered). It picks firmware only where the panel can't say which
 * board it is, and then the person sees and can change it (pCheck). */
function planBoard() {
  const b = boardOf();
  if (b) return b;
  const offered = offeredBoards();
  const pick = offered.find(([id]) => id === S.planBoard) || (offered.length === 1 ? offered[0] : null);
  return pick ? pick[1] : null;
}
/* A panel drawn to its size: the screen's diagonal from its name (7", 10.1"). */
function boardArt(board) {
  const m = /([\d.]+)"/.exec((board && board.name) || '');
  const k = m ? Math.min(1, Math.max(0.5, Number(m[1]) / 10.1)) : 1;
  return artPanel(Math.round(188 * k), Math.round(120 * k));
}
/* One board as a choice: step 1's panel and "Which board is this?". */
function boardOption(id, board, checked, act, note) {
  return h('button', { class: 'option board-pick', type: 'button', role: 'radio', 'aria-checked': checked ? 'true' : 'false', 'data-act': act, 'data-v': id },
    h('span', { class: 'dot' }), boardArt(board), h('span', {}, h('b', { text: board.long_name || board.name }), note ? h('small', { text: note }) : null));
}

/* ---- what gets wired, part by part, from boards.json: the firmware's own pins -------------- */
/* Every wire gets a pin of its own: two parts on J7 take different 3V3 and GND pins. */
const PIN_COLOUR = { VCC: 'wire-vcc', GND: 'wire-gnd', DATA: 'wire-data', MOSI: 'wire-g', MISO: 'wire-data', SCK: 'wire-y', CSn: 'wire-sig',
  GDO0: 'wire-p', SDA: 'wire-data', SCL: 'wire-sig', RX: 'wire-sig', TX: 'wire-data' };
const SHORT = { 'SI (MOSI)': 'MOSI', 'SO (MISO)': 'MISO', 'SCLK': 'SCK' };
const PIN_FROM = { MOSI: 'the pin labelled SI or MOSI', MISO: 'the pin labelled SO or MISO', SCK: 'the pin labelled SCLK or SCK' };
const position = (w) => { const m = /position (\d+)/.exec(w.note || ''); return m ? 'position ' + m[1] : undefined; };
function groups(choice, board) {
  const c = norm(choice);
  const out = [];
  if (!board) return out;
  const zbGroup = () => ({ key: 'zigbee', title: 'Zigbee radio', ic: 'tower', kind: 'zigbee', short: 'Zigbee module', chip: 'Zigbee', place: 'the radio socket', label: 'Zigbee in the socket', conn: 'plug', wires: [],
    extras: ZB_STEPS.map(([id, text]) => ({ id, text })), notes: [['info', 'info', 'The panel uses it for Zigbee sensors:', 'it needs its radio software first, which the Flasher puts on in Get ready or from Advanced tools › Zigbee module.']] });
  if (c.layout === 'custom') {   // the person's own pins have a table of their own (customWiring); the Zigbee module still sits in its socket
    if (c.zigbee) { const g = zbGroup(); g.sig = g.key + '@' + g.label; g.taken = {}; out.push(g); }
    return out;
  }
  const rows = board.header ? board.header.rows : [];
  const rf = board.rf || {};
  const used = {};
  const rowName = (r) => (/^(5V|3V3)$/.test(rows[r][0]) ? rows[r][0] + ' row' : `row ${r + 1}`);
  function free(label, near) {
    let best = null;
    rows.forEach((row, r) => row.forEach((l, col) => {
      if (l !== label || used[r + ',' + col]) return;
      const score = (row.includes('3V3') ? 0 : 100) + Math.abs(col - near);
      if (!best || score < best.score) best = { score, rc: [r, col] };
    }));
    return best && best.rc;
  }
  function onJ7(g, sig) {
    const wires = sig.map(([pin, gpio, from]) => ({ pin, colour: PIN_COLOUR[pin] || 'wire-sig', rc: findPin(rows, 'IO' + gpio), to: 'J7 IO' + gpio, from: from || PIN_FROM[pin] }));
    wires.forEach((w) => { used[w.rc.join()] = g.short; });
    const gnd = free('GND', wires[0].rc[1]); used[gnd.join()] = g.short;
    const vcc = free('3V3', 0); used[vcc.join()] = g.short;
    g.wires = [{ pin: 'GND', colour: 'wire-gnd', rc: gnd, to: 'J7 GND', from: g.gndFrom }, { pin: 'VCC', colour: 'wire-vcc', rc: vcc, to: 'J7 3V3', from: g.vccFrom }, ...wires];
    g.wires.forEach((w) => { const rn = rowName(w.rc[0]); w.where = `the ${rn}, ${ordinal(w.rc[1] + 1)} pin from the ${rn.split(' ')[0]} end, printed ${rows[w.rc[0]][w.rc[1]]}`; });
    g.conn = 'j7';
    out.push(g);
  }
  function onSocket(g, wires) {
    g.conn = 'socket';
    g.wires = wires.map((w) => Object.assign({ colour: PIN_COLOUR[w.pin] || 'wire-sig', where: w.to === 'socket 3V3' ? 'the socket pin printed 3V3' : w.to === 'socket GND' ? 'the socket pin printed GND' : `the socket pin printed ${w.to.replace('socket ', '')}` }, w));
    out.push(g);
  }
  const socketWire = (w) => {
    const pin = SHORT[w.pin] || w.pin;
    const to = w.gpio !== undefined ? 'socket IO' + w.gpio : pin === 'VCC' ? 'socket 3V3' : 'socket GND';
    return { pin, to, from: position(w) || PIN_FROM[pin] };
  };
  const wired = (list) => (list || []).filter((w) => w.pin === 'VCC' || w.pin === 'GND' || w.gpio !== undefined);
  const power = (list) => [...wired(list).filter((w) => w.pin === 'GND'), ...wired(list).filter((w) => w.pin === 'VCC'), ...wired(list).filter((w) => w.gpio !== undefined)];
  if (c.rf === 'module') {
    const g = { key: 'rf', title: '433 MHz receiver module', ic: 'radio', kind: 'receiver', name: 'RECEIVER', short: 'receiver', chip: 'Receiver',
      extras: [{ id: 'ant', text: 'Antenna: its spring, or 17.3 cm of straight wire, away from the board.' }],
      notes: [['warn', 'triangle', 'VCC goes to 3V3, never 5V:', 'on 5V the receiver\'s DATA would put 5 V into the panel\'s chip, which takes 3.3 V at most.']] };
    if (c.rf_place === 'j7' && rows.length) { g.place = `J7, DATA on IO${c.rf_pin}`; g.label = `Receiver on J7 IO${c.rf_pin}`; onJ7(g, [['DATA', c.rf_pin]]); } else {
      g.place = 'the radio socket'; g.label = 'Receiver on the socket';
      onSocket(g, [{ pin: 'GND', to: 'socket GND' }, { pin: 'VCC', to: 'socket 3V3' }, { pin: 'DATA', to: 'socket IO' + rf.default_gpio }]);
    }
  } else if (c.rf === 'cc1101') {
    const g = { key: 'rf', title: 'CC1101 receiver', ic: 'radio', kind: 'cc1101', name: 'CC1101', short: 'CC1101', chip: 'CC1101',
      extras: [{ id: 'gdo2', text: 'GDO2: leave it unconnected.' }, { id: 'ant', text: 'Antenna: 17.3 cm of wire, away from the display\'s ribbon cable.' }],
      notes: [['warn', 'triangle', 'The CC1101 is a 3.3 V part.', 'Pin order differs between CC1101 boards: match the labels printed on it, not the positions.']] };
    const p = j7Place(board);
    if (c.rf_place === 'j7' && p) {
      g.place = 'J7'; g.label = 'CC1101 on J7';
      onJ7(g, p.wires.filter((w) => w.gpio !== undefined).map((w) => [SHORT[w.pin] || w.pin, w.gpio]));
    } else {
      g.place = 'the radio socket'; g.label = 'CC1101 on the socket';
      onSocket(g, power((rf.cc1101 || {}).wires).map(socketWire));
    }
  }
  if (c.clock && board.rtc) {
    const rtc = board.rtc;
    const g = { key: 'clock', title: 'Battery-backed clock', ic: 'clock', kind: 'clock', name: rtc.chip || 'CLOCK', short: 'clock', chip: 'Clock',
      extras: [{ id: 'cell', text: 'Coin cell: fit it now (a rechargeable LIR2032, or a CR2032 if the board\'s charging part has been removed).' }, { id: 'sqw', text: 'SQW and 32K: leave them unconnected.' }], notes: [] };
    const p = clockJ7(board);
    if (c.clock_place === 'j7' && p) {
      g.place = `J7, IO${p.sda} and IO${p.scl}`; g.label = 'Clock on J7';
      onJ7(g, [['SCL', p.scl], ['SDA', p.sda]]);
    } else {
      const cn = (rtc.connector || { pins: ['SCL', 'SDA', '3V3', 'GND'] }).pins;
      const cname = (rtc.connector || {}).name || 'I2C';
      g.place = `the I2C connector (${cname})`; g.label = `Clock on ${cname}`; g.conn = 'j13'; g.rows = [cn]; g.connName = cname;
      g.wires = [['GND', 'GND'], ['VCC', '3V3'], ['SCL', 'SCL'], ['SDA', 'SDA']]
        .map(([pin, to]) => ({ pin, colour: PIN_COLOUR[pin], rc: [0, cn.indexOf(to)], to: `${cname} ${to}`, where: `the ${cname} pin printed ${to}` }));
      g.notes.push(['warn', 'triangle', 'Short leads, 3V3 only.', `${cname} shares the touch screen's wiring: keep the clock beside it on 10 to 20 cm of lead, away from siren and mains wires.`]);
      out.push(g);
    }
  }
  if (c.zwave && board.zwave) {
    const zw = board.zwave;
    const p = zwPlace(board, c.zwave_place);
    const byPin = (pin) => ((p && p.wires) || []).find((w) => w.pin === pin) || {};
    const g = { key: 'zwave', title: 'Z-Wave radio', ic: 'radio', kind: 'zwave', name: (zw.name || 'Z-WAVE').replace(/^Zooz /, ''), short: 'Z-Wave radio', chip: 'Z-Wave',
      vccFrom: position(byPin('VCC')), gndFrom: position(byPin('GND')),
      extras: [{ id: 'pos', text: 'Its pins are numbered 1 to 10, like a Raspberry Pi\'s. Positions 2 and 4 stay empty.' }],
      notes: [['info', 'info', 'Crossed over:', 'the radio\'s RX goes to the panel\'s TX, and its TX to the panel\'s RX. The drawings already show it that way.']] };
    if (c.zwave_place === 'j7' && p) {
      g.place = `J7, IO${p.tx} and IO${p.rx}`; g.label = 'Z-Wave on J7';
      onJ7(g, [['RX', p.tx, position(byPin('RX'))], ['TX', p.rx, position(byPin('TX'))]]);
    } else if (p) {
      g.place = 'the radio socket'; g.label = 'Z-Wave on the socket';
      onSocket(g, power(p.wires).map(socketWire));
    }
  }
  if (c.zigbee) out.push(zbGroup());
  for (const g of out) {
    g.sig = g.key + '@' + g.label;
    g.taken = {};
    for (const [rc, who] of Object.entries(used)) if (who !== g.short && g.conn === 'j7') g.taken[rc] = 'the ' + who;
  }
  return out;
}
/* Assembly order: J7's parts, then the I2C connector's, then the radio socket's, numbered. */
const CONN_RANK = { j7: 0, j13: 1, socket: 2, plug: 2 };
function assembly(gs) {
  const out = gs.slice().sort((a, b) => CONN_RANK[a.conn] - CONN_RANK[b.conn]);
  out.forEach((g, i) => { g.num = i + 1; });
  return out;
}
const numbered = (g) => [h('span', { class: 'num-badge', text: String(g.num) }), g.title];
function items(g) {
  return g.wires.map((w) => Object.assign({ id: w.pin, kind: 'wire' }, w)).concat(g.extras.map((e) => ({ id: e.id, kind: 'extra', text: e.text })));
}
const tickKey = (g, id) => g.sig + ':' + id;
const doneOf = (g) => items(g).filter((it) => S.tick[tickKey(g, it.id)]).length;
const planned = () => assembly(groups(S.choice, planBoard()));
/* With custom pins the parts have no groups (customWiring has their table): what is on J7 and
 * whether the clock is on the I2C connector come from the choice (FLS-029). */
const ownOnJ7 = (c) => c.layout === 'custom' && (c.rf === 'cc1101' || c.rf === 'module' || c.zwave || (c.clock && c.clock_place === 'custom'));
const ownClockOnI2c = (c) => c.layout === 'custom' && c.clock && c.clock_place === 'j13';
const i2cName = (board) => ((((board || {}).rtc || {}).connector) || {}).name || 'I2C';
const i2cPlug = (board) => ((((board || {}).rtc || {}).connector) || {}).plug || '4-pin';
/* A panel that runs RoboAlarms keeps its parts' settings unless they were changed here. */
// A reset erases the settings, so a custom layout the panel reported is sent again (FLS-029).
const keepParts = () => runsRoboAlarms() && !S.touched && !(S.action === 'reset' && S.choiceFor === S.port && norm(S.choice).layout === 'custom');

/* What to have ready, from the parts and places chosen. */
function needs(gs) {
  const c = norm(S.choice);
  const jumpers = gs.filter((g) => g.conn === 'j7' || g.conn === 'socket').reduce((n, g) => n + g.wires.length, 0);
  const custom = c.layout === 'custom';
  const keys = (custom ? [c.rf !== 'none' && 'rf', c.clock && 'clock', c.zwave && 'zwave'].filter(Boolean) : []).concat(gs.map((g) => g.key));
  const out = keys.map((key) => ({ id: 'part-' + key, text: { rf: c.rf === 'cc1101' ? 'The CC1101 board' : 'The 433 MHz receiver module', clock: 'The clock board (DS3231, often sold as ZS-042)',
    zwave: 'The Zooz ZAC93 Z-Wave radio', zigbee: 'Elecrow\'s ESP32-H2 Zigbee module, with the antenna that came with it' }[key] }));
  if (c.zigbee) out.push({ id: 'zbcable', text: 'A USB-C data cable for the Zigbee module', sub: 'A charging-only cable won\'t work: if the Flasher doesn\'t find the module, try another cable.' });
  if (ownOnJ7(c)) out.push({ id: 'jumpers', text: 'Jumper wires, female to female, 10 to 20 cm long', sub: 'One for each pin in your table, and a 3V3 and a GND wire for each part. Also sold as "Dupont wires".', art: true });
  if (jumpers) out.push({ id: 'jumpers', text: `${jumpers} jumper wires, female to female, 10 to 20 cm long`, sub: 'Also sold as "Dupont wires". Each end has a small plug with a hole that slides onto a pin. A few spares help.', art: true });
  const j13 = gs.find((g) => g.conn === 'j13');
  const j13Name = j13 ? j13.connName : ownClockOnI2c(c) ? i2cName(planBoard()) : '';
  if (j13Name) out.push({ id: 'ph2', text: `A 4-wire lead with a ${i2cPlug(planBoard())} plug on one end and jumper plugs on the other`, sub: `It plugs into ${j13Name} and onto the clock board's pins.` });
  if (c.rf === 'module') out.push({ id: 'ant', text: 'An antenna for the receiver', sub: 'The spring that came with it, or 17.3 cm of plain wire.' });
  if (c.rf === 'cc1101') out.push({ id: 'ant', text: 'An antenna for the CC1101', sub: '17.3 cm of plain wire, if it didn\'t come with one.' });
  if (c.clock) out.push({ id: 'cell', text: 'A coin cell for the clock', sub: 'A rechargeable LIR2032. A plain CR2032 only if the board\'s charging part has been removed: most of these boards try to charge the cell.' });
  if (gs.some((g) => g.conn === 'j7') || ownOnJ7(c)) out.push({ id: 'meter', text: 'Only if J7\'s labels are hard to read: a multimeter', sub: 'The box below shows how to find its 5V row with it.' });
  return out;
}
const PREP = [['off', 'Switch the panel\'s POWER switch off.'], ['unplug', 'Unplug every cable from the panel: both USB cables and any power supply.'],
  ['static', 'Touch something metal, like a tap or a radiator, to get rid of static before you handle the boards.'], ['lay', 'Lay the panel face down on a soft cloth, so the back of the board faces you.']];
/* [id, words, why, danger]: nothing on 5V comes first and stands out, since anything wired to it can be
 * destroyed (decided 2026-09-30). */
const CHECKS = [['no5v', 'No wire is on a pin printed 5V.', 'A wire on 5V can destroy the part and the panel\'s chip.', true],
  ['pushed', 'Every wire is pushed all the way on, at both ends.'], ['one', 'Each pin has only one wire on it.'],
  ['vcc', 'Every VCC wire goes to a pin printed 3V3.'],
  ['touch', 'No bare metal touches anything else: pins, wire ends or the case.'], ['away', 'Antennas and loose wires lie away from the screen\'s flat ribbon cable.']];

/* ---- drawings --------------------------------------------------------------------------- */
/* The computer's cable, and with a board's "connect" advice (conn) the phone charger's too, each
 * plug named as its port is printed. */
function artConnect(found, conn) {
  const end = found ? 332 : 292;
  const on = found ? ' on' : '';
  const two = !!(conn && conn.port && conn.power_port);
  const y = two ? 76 : 90;
  const label = two ? `This computer on the panel's ${conn.port} port and a phone charger on its ${conn.power_port} port`
    : 'A computer and the panel joined by a USB-C cable';
  const svg = s('svg', { class: 'art', viewBox: two ? '0 0 560 200' : '0 0 560 180', role: 'img', 'aria-label': label },
    s('rect', { class: 'laptop', x: 40, y: 26, width: 170, height: 108, rx: 10 }),
    s('rect', { class: 'laptop-screen', x: 52, y: 38, width: 146, height: 84, rx: 4 }),
    s('path', { class: 'laptop', d: 'M22 136 h206 l-10 14 h-186 z' }),
    s('rect', { class: 'plug' + on, x: 210, y: 104, width: 10, height: 14, rx: 2 }),
    s('path', { class: 'cable' + on, d: `M220 111 C 262 111, ${end - 40} ${y}, ${end} ${y}` }),
    s('rect', { class: 'plug' + on, x: end, y: y - 7, width: 16, height: 14, rx: 3 }),
    s('rect', { class: 'frame', x: 348, y: 20, width: 190, height: 130, rx: 14 }),
    s('rect', { class: 'screen', x: 360, y: 32, width: 166, height: 106, rx: 6 }),
    s('path', { class: 'mark', d: 'M443 60 l22 17 v28 h-44 v-28 z' }),
    s('text', { class: 'caption', x: 443, y: 172, 'text-anchor': 'middle', text: 'The panel' }));
  if (two) {
    svg.append(
      s('rect', { class: 'plug', x: 236, y: 157, width: 10, height: 5, rx: 1 }),
      s('rect', { class: 'plug', x: 236, y: 168, width: 10, height: 5, rx: 1 }),
      s('rect', { class: 'laptop', x: 246, y: 150, width: 40, height: 30, rx: 6 }),
      s('path', { class: 'cable', d: `M266 150 C 266 118, ${end - 40} 112, ${end} 112` }),
      s('rect', { class: 'plug', x: end, y: 105, width: 16, height: 14, rx: 3 }),
      s('text', { class: 'port-tag', x: end + 14, y: 63, 'text-anchor': 'end', text: conn.port }),
      s('text', { class: 'port-tag', x: end + 14, y: 133, 'text-anchor': 'end', text: conn.power_port }),
      s('text', { class: 'caption', x: 266, y: 196, 'text-anchor': 'middle', text: 'Phone charger' }));
  }
  return svg;
}
function artPanel(w = 188, ht = 120, label = '') {
  return s('svg', { class: 'art', viewBox: '0 0 200 140', role: 'img', 'aria-label': 'The panel' },
    s('rect', { class: 'frame', x: 100 - w / 2, y: 70 - ht / 2, width: w, height: ht, rx: 12 }),
    s('rect', { class: 'screen', x: 100 - w / 2 + 10, y: 70 - ht / 2 + 10, width: w - 20, height: ht - 20, rx: 5 }),
    s('path', { class: 'mark', d: `M100 ${70 - ht / 5} l${ht / 6} ${ht / 8} v${ht / 5} h-${ht / 3} v-${ht / 5} z` }),
    label ? s('text', { class: 'label', x: 100, y: 70 + ht / 2 - 16, 'text-anchor': 'middle', text: label }) : null);
}

/* One part's wiring, drawn from the connector as printed (boards.json), so it is the pin map, not
 * a drawing of it. Wires run in lanes (between J7's rows, above J13's) in an order where no wire
 * crosses another or a pin it doesn't end on: VCC must never look as if it lands on 5V.
 * opt.pick: pins that can be clicked to move DATA; opt.takenRC: pins other parts use (dashed);
 * opt.key: the part, so a tick-list row can light its wires. */
function findPin(rows, label) {
  for (let r = 0; r < rows.length; r++) {
    const c = rows[r].indexOf(label);
    if (c >= 0) return [r, c];
  }
  return null;
}
function wiring(conn, wires, opt) {
  const P = 31, W = 26, H = 23, LS = 12, x0 = 250;
  const two = conn.rows.length === 2;
  const top = two ? 96 : (opt.kind === 'clock' ? 36 : 84);
  const X = (c) => x0 + c * P;
  const list = wires.filter((w) => w.rc);
  const order = two
    ? [...list.filter((w) => w.rc[0] === 0).sort((a, b) => a.rc[1] - b.rc[1]), ...list.filter((w) => w.rc[0] === 1).sort((a, b) => b.rc[1] - a.rc[1])]
    : [...list].sort((a, b) => b.rc[1] - a.rc[1]);
  const n = Math.max(1, order.length);
  const lane = (i) => (two ? top + H / 2 + 13 + i * LS : top + i * LS);
  const rowY = two ? [top, lane(n - 1) + 13 + H / 2] : [lane(n - 1) + 13 + H / 2];
  const cols = conn.rows[0].length;
  const bx = X(0) - W / 2 - 10, by = rowY[0] - H / 2 - 9;
  const bw = (cols - 1) * P + W + 20, bh = rowY[rowY.length - 1] - rowY[0] + H + 18;
  const mid = (lane(0) + lane(n - 1)) / 2;
  const mTop = Math.min(lane(0) - 24, mid - 36), mBot = Math.max(lane(n - 1) + 24, mid + 36);
  const titleY = two ? by - 10 : by + bh + 18;
  const width = Math.max(bx + bw, bx + conn.title.length * 6.6) + 8;
  const height = Math.max(two ? by + bh : titleY, mBot) + 12;
  const svg = s('svg', { viewBox: `0 0 ${width} ${height}`, width: Math.round(width * 1.2), height: Math.round(height * 1.2), role: 'img', 'aria-label': opt.label });
  if (opt.kind !== 'clock') {
    const pts = [`44,${mTop}`];
    for (let k = 1; k <= 7; k++) pts.push(`${44 + (k % 2 ? -8 : 8)},${mTop - k * 6}`);
    pts.push(`44,${mTop - 48}`);
    svg.append(s('polyline', { class: 'ant', points: pts.join(' ') }));
  }
  svg.append(s('rect', { class: 'pcb', x: 24, y: mTop, width: 130, height: mBot - mTop, rx: 7 }));
  svg.append(opt.kind === 'clock'
    ? s('circle', { class: 'cell', cx: 58, cy: mid - 8, r: 17 })
    : s('rect', { class: 'chipdark', x: 38, y: mTop + 14, width: 34, height: 26, rx: 3 }));
  svg.append(s('text', { class: 'pcb-ink', x: 34, y: mBot - 10, 'font-size': 10.5, text: opt.name }));
  order.forEach((w, i) => {
    svg.append(s('rect', { class: 'pad', x: 154, y: lane(i) - 5, width: 12, height: 10, rx: 1.5 }));
    svg.append(s('text', { class: 'pcb-ink', x: 146, y: lane(i) + 3.5, 'text-anchor': 'end', 'font-size': 9.5, text: w.pin }));
  });
  svg.append(s('text', { class: 'hdr-title', x: bx, y: titleY, text: conn.title }));
  svg.append(s('rect', { class: 'hdr', x: bx, y: by, width: bw, height: bh, rx: 9 }));
  conn.rows.forEach((row, r) => row.forEach((label, c) => {
    const pn = /^IO\d+$/.test(label) ? Number(label.slice(2)) : null;
    const mine = order.find((w) => w.rc[0] === r && w.rc[1] === c);
    const taken = !mine && opt.takenRC ? opt.takenRC[r + ',' + c] || '' : '';
    const reasons = opt.reasons || {};
    const reserved = pn !== null && !mine && !taken && (opt.reserved || []).includes(pn);
    const pickable = !!opt.pick && pn !== null && !mine && !taken && opt.pick.includes(pn);
    let cls = label === '3V3' ? 'p3' : label === '5V' ? 'p5' : label === 'GND' ? 'gnd' : 'io';
    if (taken) cls += ' taken';
    if (reserved) cls += ' reserved';
    if (pickable) cls += ' pickable';
    let tip;
    if (mine) tip = `${label}: the ${opt.short}'s ${mine.pin}.`;
    else if (taken) tip = `${label}: taken by ${taken}.`;
    else if (reserved) tip = reasons[pn] || `${label}: not offered for this part.`;
    else if (label === '5V') tip = '5V: never for these parts.';
    else if (label === '3V3') tip = '3V3: power for these parts.';
    else if (label === 'GND') tip = 'GND: ground.';
    else if (pn === null) tip = `${label}: the connector's ${label} line.`;
    else if (pickable) tip = `${label}: free. Click to put DATA here.`;
    else tip = `${label}: free.`;
    svg.append(s('g', Object.assign({ class: 'pin ' + cls, 'data-tip': tip },
      pickable ? { role: 'button', tabindex: 0, 'data-act': 'pin', 'data-v': pn, 'aria-label': `Put DATA on J7 IO${pn}` } : {}),
    s('rect', { class: 'body', x: X(c) - W / 2, y: rowY[r] - H / 2, width: W, height: H, rx: 4 }),
    s('text', { x: X(c), y: rowY[r] + 3.5, 'text-anchor': 'middle', 'font-size': 8.5, text: label })));
  }));
  order.forEach((w, i) => {
    const [r, c] = w.rc;
    const y = lane(i), x2 = X(c);
    const up = two && r === 0;
    const edge = up ? rowY[0] + H / 2 : rowY[r] - H / 2;
    const turn = up ? -6 : 6;
    svg.append(s('path', { class: 'wire w-' + w.colour, d: `M166 ${y} H ${x2 - 6} Q ${x2} ${y} ${x2} ${y + turn} V ${edge}` }));
    svg.append(s('rect', { class: 'ring w-' + w.colour, x: x2 - W / 2 - 2.5, y: rowY[r] - H / 2 - 2.5, width: W + 5, height: H + 5, rx: 5.5 }));
  });
  return svg;
}
function reasons(board) {
  const out = {};
  for (const [k, v] of Object.entries(((board.rf || {}).reserved) || {})) out[Number(k)] = v;
  return out;
}
/* A magnifying glass over the header's pins (decided 2026-09-30), so their labels read easily: a
 * circle showing the drawing about 2.4 times larger, centred on the pointer while it is on a row of
 * pins - not on the wires between the rows, nor on the parts' numbers above and below them, which
 * stay visible and clickable. A row counts as one band, gaps between its pins included, so moving
 * along it never makes the lens vanish and reappear (FLS-017). The lens takes no clicks. */
let loupeSeq = 0;
function addLoupe(svg, bands) {
  const id = 'loupe' + (++loupeSeq);
  const R = 56, Z = 2.4, MARGIN = 4;
  // A plain copy of the drawing inside the same <svg>, so the page's styles reach it (a <use>
  // copy sits in a shadow tree the ".wiring ..." rules can't reach, and its wires filled black).
  // It takes no clicks, no focus and shows no tooltips.
  const copy = s('g', {});
  for (const el of svg.childNodes) copy.append(el.cloneNode(true));
  for (const el of copy.querySelectorAll('title')) el.remove();
  for (const el of copy.querySelectorAll('[tabindex], [role], [data-act], [aria-label]')) {
    for (const a of ['tabindex', 'role', 'data-act', 'data-v', 'aria-label']) el.removeAttribute(a);
  }
  const lens = s('g', { class: 'loupe', 'aria-hidden': 'true' },
    s('circle', { class: 'loupe-bg', r: R }),
    s('g', { 'clip-path': `url(#${id}-k)` }, copy),
    s('circle', { class: 'loupe-ring', r: R }));
  lens.style.display = 'none';
  svg.append(s('defs', {}, s('clipPath', { id: id + '-k' }, s('circle', { r: R }))), lens);
  let shown = false, frame = 0, last = null;
  const place = () => {
    frame = 0;
    const m = svg.getScreenCTM();
    if (!m || !last) return;
    const p = new DOMPoint(last.clientX, last.clientY).matrixTransform(m.inverse());
    const out = shown ? MARGIN : 0;
    const inside = bands.some((b) => p.x >= b.x - out && p.x <= b.x + b.w + out && p.y >= b.y - out && p.y <= b.y + b.h + out);
    if (inside !== shown) { shown = inside; lens.style.display = inside ? '' : 'none'; svg.classList.toggle('loupe-on', inside); }
    if (!inside) return;
    lens.setAttribute('transform', `translate(${p.x} ${p.y})`);
    copy.setAttribute('transform', `scale(${Z}) translate(${-p.x} ${-p.y})`);
  };
  svg.addEventListener('pointermove', (e) => { last = e; if (!frame) frame = requestAnimationFrame(place); });
  svg.addEventListener('pointerleave', () => { shown = false; lens.style.display = 'none'; svg.classList.remove('loupe-on'); });
}
/* Every part on J7 in one drawing: each part's board on the left, stacked in the order of its
 * wires' lanes, its wires fanning out from its pads into the lanes between J7's rows, in the order
 * wiring() uses so that no wire crosses a pin it doesn't end on (wires may cross each other in the
 * fan, away from the header). Each part's number (assembly order) is on its board and beside every
 * pin it uses; opt.focus (a part's key) fades the others; opt.pick: a board click picks its part.
 * Nothing changes on hover: fading follows a click only, so moving the pointer never flickers it. */
function wiringAll(rows, parts, opt) {
  // The header takes most of the width, so its labels read at the Flasher's window size.
  const P = 42, W = 36, H = 28, LS = 16, PS = 17, MG = 20, MX = 52, MW = 118;
  const PX = MX + MW + 12, FX = PX + 80, x0 = FX + 60;
  const X = (c) => x0 + c * P;
  const all = parts.flatMap((p, pi) => p.wires.filter((w) => w.rc).map((w) => Object.assign({ part: pi }, w)));
  const order = [...all.filter((w) => w.rc[0] === 0).sort((a, b) => a.rc[1] - b.rc[1]), ...all.filter((w) => w.rc[0] === 1).sort((a, b) => b.rc[1] - a.rc[1])];
  order.forEach((w, i) => { w.lane = i; });
  const n = Math.max(1, order.length);
  const top = 60;
  const lane = (i) => top + H / 2 + 13 + i * LS;
  const rowY = [top, lane(n - 1) + 13 + H / 2];
  const cols = rows[0].length;
  const bx = X(0) - W / 2 - 10, by = rowY[0] - H / 2 - 9;
  const bw = (cols - 1) * P + W + 20, bh = rowY[1] - rowY[0] + H + 18;
  const mods = parts.map((p, pi) => {
    const ws = order.filter((w) => w.part === pi);
    return { p, pi, ws, mean: ws.reduce((a, w) => a + w.lane, 0) / Math.max(1, ws.length) };
  }).filter((m) => m.ws.length).sort((a, b) => a.mean - b.mean);
  for (const m of mods) m.h = Math.max(78, 24 + (m.ws.length - 1) * PS + 40);
  const total = mods.reduce((a, m) => a + m.h, 0) + (mods.length - 1) * MG;
  let y = (lane(0) + lane(n - 1)) / 2 - total / 2;
  for (const m of mods) { m.top = y; m.ws.forEach((w, j) => { w.py = y + 24 + j * PS; }); y += m.h + MG; }
  const tagged = parts.some((p) => p.num);
  const minY = Math.min(by - (tagged ? 58 : 34), ...mods.map((m) => m.top)) - 10;
  const maxY = Math.max(by + bh + (tagged ? 28 : 0), ...mods.map((m) => m.top + m.h)) + 10;
  const width = bx + bw + 8;
  const svg = s('svg', { viewBox: `0 ${minY} ${width} ${maxY - minY}`, width: Math.round(width * 1.2), height: Math.round((maxY - minY) * 1.2), role: 'img', 'aria-label': opt.label || '', class: 'all' });
  const dim = (pi) => (opt.focus && parts[pi].key !== opt.focus ? ' dim' : '');
  const badge = (x, bY, num, r) => [s('circle', { class: 'badge', cx: x, cy: bY, r }), s('text', { class: 'badge-t', x, y: bY + r * 0.42, 'text-anchor': 'middle', 'font-size': r * 1.25, text: String(num) })];
  for (const m of mods) {
    const d = dim(m.pi);
    const g = s('g', Object.assign({ class: 'part p-' + m.p.key + d }, opt.pick ? { 'data-act': opt.pickAct || 'fullfocus', 'data-v': m.p.key, role: 'button', tabindex: 0, 'aria-label': `Show only the ${m.p.title}` } : {}),
      s('title', { text: m.p.num ? `${m.p.num}: ${m.p.title}` : m.p.title }));
    if (m.p.kind === 'receiver' || m.p.kind === 'cc1101') {
      const pts = [`${MX},${m.top + 18}`];
      for (let k = 1; k <= 6; k++) pts.push(`${MX - k * 7},${m.top + 18 + (k % 2 ? -7 : 7)}`);
      g.append(s('polyline', { class: 'ant', points: pts.join(' ') }));
    }
    g.append(s('rect', { class: 'pcb', x: MX, y: m.top, width: MW, height: m.h, rx: 7 }));
    g.append(m.p.kind === 'clock' ? s('circle', { class: 'cell', cx: MX + 46, cy: m.top + m.h / 2 - 4, r: 15 })
      : s('rect', { class: 'chipdark', x: MX + 32, y: m.top + 12, width: 34, height: 26, rx: 3 }));
    if (m.p.num) append(g, [badge(MX + 16, m.top + 16, m.p.num, 11)]);
    g.append(s('text', { class: 'pcb-ink', x: MX + 10, y: m.top + m.h - 11, 'font-size': 12, text: m.p.name }));
    m.ws.forEach((w) => {
      g.append(s('rect', { class: 'pad', x: MX + MW, y: w.py - 5, width: 12, height: 10, rx: 1.5 }));
      g.append(s('text', { class: 'pcb-ink', x: MX + MW - 8, y: w.py + 4, 'text-anchor': 'end', 'font-size': 11, text: w.pin }));
    });
    svg.append(g);
  }
  svg.append(s('text', { class: 'hdr-title hdr-big', x: bx, y: by - (tagged ? 36 : 12), text: opt.title }));
  svg.append(s('rect', { class: 'hdr', x: bx, y: by, width: bw, height: bh, rx: 9 }));
  rows.forEach((row, r) => row.forEach((label, c) => {
    const mine = order.find((w) => w.rc[0] === r && w.rc[1] === c);
    let cls = label === '3V3' ? 'p3' : label === '5V' ? 'p5' : label === 'GND' ? 'gnd' : 'io';
    if (!mine && label === 'IO25') cls += ' reserved';
    if (mine) cls += ' used';
    const tip = mine ? `${label}: the ${parts[mine.part].short}'s ${mine.pin}` : label === '5V' ? '5V: never for these parts' : `${label}: free`;
    svg.append(s('g', { class: 'pin ' + cls }, s('title', { text: tip }),
      s('rect', { class: 'body', x: X(c) - W / 2, y: rowY[r] - H / 2, width: W, height: H, rx: 4 }),
      s('text', { x: X(c), y: rowY[r] + 4, 'text-anchor': 'middle', 'font-size': 11, text: label })));
  }));
  order.forEach((w) => {
    const [r, c] = w.rc;
    const ly = lane(w.lane), x2 = X(c);
    const up = r === 0;
    const edge = up ? rowY[0] + H / 2 : rowY[1] - H / 2;
    const turn = up ? -6 : 6;
    const p = parts[w.part];
    const cls = ' part p-' + p.key + dim(w.part);
    svg.append(s('path', { class: 'wire w-' + w.colour + cls, d: `M${PX} ${w.py} C ${PX + 44} ${w.py}, ${FX - 44} ${ly}, ${FX} ${ly} H ${x2 - 6} Q ${x2} ${ly} ${x2} ${ly + turn} V ${edge}` }));
    svg.append(s('rect', { class: 'ring w-' + w.colour + cls, x: x2 - W / 2 - 2.5, y: rowY[r] - H / 2 - 2.5, width: W + 5, height: H + 5, rx: 5.5 }));
    if (p.num) svg.append(s('g', Object.assign({ class: cls.trim() + ' pinbadge' }, opt.pick ? { 'data-act': opt.pickAct || 'fullfocus', 'data-v': p.key, role: 'button', tabindex: 0, 'aria-label': `Show only the ${p.title}` } : {}), s('title', { text: `${p.num}: ${p.title}, ${w.pin}` }), badge(x2, up ? by - 14 : by + bh + 14, p.num, 9.5)));
  });
  if (opt.loupe) addLoupe(svg, rowY.map((ry) => ({ x: X(0) - W / 2, y: ry - H / 2, w: (cols - 1) * P + W, h: H })));
  return svg;
}
/* A connector as printed, small, for "Find the places" on Get ready. */
function miniHeader(rows) {
  const P = 21, W = 18, H = 15, pad = 8;
  const width = pad * 2 + (rows[0].length - 1) * P + W, height = pad * 2 + rows.length * (H + 4) - 4;
  return s('svg', { class: 'mini', viewBox: `0 0 ${width} ${height}`, role: 'img', 'aria-label': 'Its pins, as printed' },
    s('rect', { class: 'mini-bg', x: 0, y: 0, width, height, rx: 6 }),
    rows.map((row, r) => row.map((label, c) => {
      const cls = label === '3V3' ? 'p3' : label === '5V' ? 'p5' : label === 'GND' ? 'gnd' : 'io';
      return s('g', { class: 'pin ' + cls },
        s('rect', { class: 'body', x: pad + c * P, y: pad + r * (H + 4), width: W, height: H, rx: 3 }),
        s('text', { x: pad + c * P + W / 2, y: pad + r * (H + 4) + 10.5, 'text-anchor': 'middle', 'font-size': label.length > 3 ? 5.6 : 6.5, text: label }));
    })));
}
/* The radio socket's two rows of seven, as in the Zigbee module's drawing. */
function miniSocket() {
  const col = (x) => [s('rect', { class: 'sock-housing', x: x - 7, y: 6, width: 14, height: 118, rx: 3 }),
    [0, 1, 2, 3, 4, 5, 6].map((i) => s('rect', { class: 'sock-hole', x: x - 3, y: 13 + i * 16, width: 6, height: 6, rx: 1 }))];
  return s('svg', { class: 'mini', viewBox: '0 0 280 130', role: 'img', 'aria-label': 'The radio socket: two rows of seven holes' },
    s('rect', { class: 'mini-bg', x: 1, y: 1, width: 278, height: 128, rx: 8 }), col(100), col(180),
    s('text', { class: 'mini-cap', x: 140, y: 69, 'text-anchor': 'middle', 'font-size': 10, text: 'radio' }));
}
/* A jumper wire's plug going onto a pin, for "How to fit a jumper wire". */
function artFit() {
  return s('svg', { class: 'fit', viewBox: '0 0 200 136', role: 'img', 'aria-label': 'A jumper wire\'s plug pushed straight down onto a pin until it stops' },
    s('rect', { class: 'fit-pcb', x: 10, y: 98, width: 180, height: 14, rx: 3 }),
    [40, 70, 100, 130, 160].map((x) => s('rect', { class: 'fit-pin', x: x - 2, y: 66, width: 4, height: 32 })),
    s('rect', { class: 'fit-plug', x: 92, y: 28, width: 16, height: 30, rx: 2 }),
    s('path', { class: 'fit-wire', d: 'M100 28 C 100 10, 130 6, 150 4' }),
    s('path', { class: 'fit-arrow', d: 'M122 36 v 22 m -6 -7 l 6 7 l 6 -7' }),
    s('text', { class: 'fit-cap', x: 100, y: 130, 'text-anchor': 'middle', 'font-size': 11, text: 'Push straight down' }));
}
function artJumper() {
  return s('svg', { class: 'jumper', viewBox: '0 0 120 30', role: 'img', 'aria-label': 'A jumper wire with a plug at each end' },
    s('rect', { class: 'fit-plug', x: 2, y: 9, width: 18, height: 12, rx: 2 }),
    s('path', { class: 'jp-wire', d: 'M20 15 C 45 2, 75 28, 100 15' }),
    s('rect', { class: 'fit-plug', x: 100, y: 9, width: 18, height: 12, rx: 2 }));
}
/* Elecrow's ESP32-H2 module going into the radio socket. The module from above as its datasheet
 * draws it (18 x 23.7 mm, seven pins down each side at 2.54 mm, the white triangle by its USB-C, the
 * round antenna connector between B and R); the socket's two rows of seven as printed beside it,
 * matched pin for pin in docs/research/AddOnWiringResearch.md: the module's 3V3 and GND side goes
 * into the row printed 3V3 and GND, and its 5V pin into a hole the panel leaves unconnected. */
const ZB_MODULE = [['U1RXD', 'IO2', 'IO1', 'IO0', '3V3', 'GND', '5V'], ['U1TXD', 'IO14', 'IO12', 'IO11', 'IO5', 'IO4', 'IO3']];
const ZB_STEPS = [
  ['ant', 'Click the antenna lead onto the small round gold connector at the module\'s bottom edge: line it up and press straight down until it clicks.'],
  ['turn', 'Turn the module so its 3V3 and GND pins line up with the socket holes printed 3V3 and GND.'],
  ['press', 'Press it straight down into the socket, evenly on both sides, until it sits flat.'],
  ['lay', 'Lay the antenna lead away from the screen\'s flat ribbon cable and any 433 MHz antenna.']];
function artZigbee(board) {
  const socket = ((board && board.zigbee) || {}).socket_printed || [[], []];
  const MX = 96, MY = 40, MW = 126, MH = 166, PITCH = 17.8, P0 = MY + 52;
  const SX = 402, SW = 126;
  const chipLabel = (x, cy, label, anchor) => {
    const cls = label === '3V3' ? 'p3' : label === 'GND' ? 'gnd' : label === '5V' ? 'p5' : label === 'NC' ? 'io reserved' : 'io';
    const w = 44, hgt = 15, x0 = anchor === 'end' ? x - w : x;
    return s('g', { class: 'pin ' + cls }, s('rect', { class: 'body', x: x0, y: cy - hgt / 2, width: w, height: hgt, rx: 3 }),
      s('text', { x: x0 + w / 2, y: cy + 3.5, 'text-anchor': 'middle', 'font-size': 9.5, text: label }));
  };
  const svg = s('svg', { class: 'zb', viewBox: '0 0 600 280', role: 'img', 'aria-label': 'The ESP32-H2 module, seen from above, pressed down into the radio socket: its 3V3 and GND pins into the holes printed 3V3 and GND' });
  svg.append(s('text', { class: 'cap', x: MX + MW / 2, y: 16, 'text-anchor': 'middle', text: 'The module, from above' }),
    s('text', { class: 'cap', x: SX + SW / 2, y: 16, 'text-anchor': 'middle', text: 'The radio socket, labels as printed' }));
  svg.append(s('path', { class: 'lead', d: `M${MX + 63} ${MY + 152} C ${MX + 63} ${MY + 215}, ${MX - 30} ${MY + 205}, ${MX - 80} ${MY + 222}` }),
    s('text', { class: 'cap', x: MX - 84, y: MY + 238, text: 'antenna lead' }));
  svg.append(s('rect', { class: 'mod', x: MX, y: MY, width: MW, height: MH, rx: 6 }));
  svg.append(s('rect', { class: 'usb', x: MX + 32, y: MY - 5, width: 62, height: 42, rx: 7 }));
  svg.append(s('path', { class: 'tri', d: `M${MX + 7} ${MY + 18} l8 -12 l8 12 z` }));
  svg.append(s('rect', { class: 'soc', x: MX + 62, y: MY + 88, width: 30, height: 30, rx: 2 }));
  svg.append(s('circle', { class: 'btn-dot', cx: MX + 21, cy: MY + 150, r: 8 }), s('circle', { class: 'btn-dot', cx: MX + 105, cy: MY + 150, r: 8 }));
  svg.append(s('circle', { class: 'leg', cx: MX + 63, cy: MY + 152, r: 5.5 }));
  svg.append(s('text', { class: 'ink', x: MX + 63, y: MY + 133, 'text-anchor': 'middle', text: 'ESP32-H2' }));
  ZB_MODULE.forEach((col, side) => col.forEach((label, i) => {
    const py = P0 + i * PITCH;
    const x = side ? MX + MW : MX - 8;
    svg.append(s('rect', { class: 'leg', x, y: py - 4, width: 8, height: 8, rx: 1 }));
    svg.append(chipLabel(side ? x + 12 : x - 4, py, label, side ? 'start' : 'end'));
  }));
  const ay = MY + 83;
  svg.append(s('path', { class: 'arrow', d: `M300 ${ay} H 340 m -8 -7 l 8 7 l -8 7` }),
    s('text', { class: 'arrow-t', x: 320, y: ay - 12, 'text-anchor': 'middle', text: 'press in' }));
  socket.forEach((col, side) => {
    const x = side ? SX + SW : SX;
    svg.append(s('rect', { class: 'housing', x: x - 9, y: P0 - 12, width: 18, height: 6 * PITCH + 24, rx: 3 }));
    col.forEach((label, i) => {
      const py = P0 + i * PITCH;
      svg.append(s('rect', { class: 'hole', x: x - 3.5, y: py - 3.5, width: 7, height: 7, rx: 1 }));
      svg.append(chipLabel(side ? x + 14 : x - 14, py, label, side ? 'start' : 'end'));
    });
  });
  return svg;
}
function note(kind, iconName, bold, text) {
  return h('div', { class: 'note ' + kind }, icon(iconName), h('div', {}, bold ? h('b', { text: bold }) : null, bold && text ? ' ' : '', text || ''));
}
function details(key, summary, ...body) {
  return h('details', { class: 'more', 'data-key': key, open: !!S.open[key] }, h('summary', {}, summary), body);
}

/* ---- the parts: what is connected (step 1, Advanced tools > Hardware) -------------------- */
function pcard(key, on, title, text, ic, sub, disabled, readOnly) {
  // The wireless sensors' card spans both columns: its receiver choice makes it the tallest.
  return h('div', { class: 'pcard' + (key === 'rf' ? ' wide' : '') + (on ? ' on' : '') + (disabled ? ' off' : '') },
    h('button', { class: 'pcard-head', type: 'button', role: 'checkbox', 'aria-checked': String(!!on), 'data-act': 'part', 'data-v': key, disabled: disabled || readOnly },
      h('span', { class: 'tick' }, on ? icon('check') : null), h('span', { class: 'icon-tile' + (disabled ? ' plain' : '') }, icon(ic)),
      h('span', {}, h('b', { text: title }), h('small', { text }))),
    on && sub ? h('div', { class: 'subpick' }, sub) : null);
}
function partCards(board, choice, readOnly) {
  const c = norm(choice);
  const kinds = [['cc1101', 'CC1101 board', 'A small board with eight pins: VCC, GND, SI, SO, SCLK, CSN, GDO0 and GDO2.'], ['module', 'Receiver module', 'A small board with three pins: VCC, GND and DATA.']];
  if (c.rf === 'auto') kinds.push(['auto', 'Not sure', 'The panel looks for either.']);
  const rfSub = [h('b', { class: 'small', text: 'Which receiver do you have?' }),
    h('div', { class: 'choices', role: 'radiogroup', 'aria-label': 'Receiver' }, kinds.map(([k, t, d]) =>
      h('button', { class: 'choice', type: 'button', role: 'radio', 'aria-checked': String(c.rf === k), 'data-act': 'rfkind', 'data-v': k, disabled: readOnly },
        h('span', { class: 'dot' }), h('b', { text: t }), h('small', { text: d })))),
    h('p', { class: 'small muted', text: 'Not sure? Count the pins along its edge: eight is a CC1101, three is a receiver module.' })];
  // Wireless sensors across the top, then the Zigbee radio (the more common radio) beside the
  // Z-Wave radio, then the clock beside what comes later.
  return h('div', { class: 'grid2' },
    board.rf ? pcard('rf', c.rf !== 'none', 'Wireless sensors (433 MHz)', 'Lets the panel hear 433 MHz door, window and motion sensors and key-fob remotes. Needs a receiver.', 'radio', rfSub, false, readOnly) : null,
    board.zigbee ? pcard('zigbee', c.zigbee, 'Zigbee radio', `Elecrow's plug-in ${board.zigbee.name || 'Zigbee module'}, for Zigbee sensors. The Flasher puts its radio software on first, in Get ready.`, 'tower', null, false, readOnly) : null,
    board.zwave ? pcard('zwave', c.zwave, 'Z-Wave sensors', `A ${board.zwave.name || 'Z-Wave'} radio, for Z-Wave door, window, motion, smoke and water sensors.`, 'radio', null, false, readOnly) : null,
    board.rtc ? pcard('clock', c.clock, 'Battery-backed clock', `Keeps the date and time through power cuts. A small board with a coin cell (${board.rtc.chip || 'DS3231'}, often sold as ZS-042).`, 'clock', null, false, readOnly) : null,
    pcard('siren', false, 'Sirens, strobes and relays', 'Come with a later version of RoboAlarms.', 'siren', null, true));
}
/* Parts are ticked without places: a new part gets its recommended place, the others keep theirs
 * unless that no longer fits. */
function setParts(next, board) {
  const kept = norm(next);
  const rec = recommend(next, board);
  const old = norm(S.choice);
  for (const [on, place] of [[kept.rf !== 'none' && old.rf === kept.rf, 'rf_place'], [kept.clock && old.clock, 'clock_place'], [kept.zwave && old.zwave, 'zwave_place']]) {
    kept[place] = on ? old[place] : rec[place];
  }
  if (kept.rf === 'module' && !(((board && board.rf) || {}).pins || []).includes(kept.rf_pin)) kept.rf_pin = rec.rf_pin;
  S.choice = problems(kept, board).length ? rec : kept;
}

/* ---- where each part goes (advanced: step 1's fold-out, Advanced tools > Hardware) ------- */
function socketUser(c, except) {
  if (except !== 'zigbee' && c.zigbee) return 'the Zigbee module';
  if (except !== 'rf' && c.rf !== 'none' && c.rf !== 'auto' && c.rf_place === 'socket') return c.rf === 'cc1101' ? 'the CC1101' : 'the receiver';
  if (except !== 'zwave' && c.zwave && c.zwave_place === 'socket') return 'the Z-Wave radio';
  return '';
}
/* A radio card: Install's Update or Reset. */
function placeButton(act, key, title, sub, on, why, readOnly) {
  return h('button', { class: 'place', type: 'button', role: 'radio', 'aria-checked': on ? 'true' : 'false',
    'aria-disabled': why && !on ? 'true' : null, 'data-act': act, 'data-v': key, disabled: readOnly },
  h('span', { class: 'dot' }), h('span', {}, h('b', { text: title }), h('small', { text: sub }), why ? h('small', { class: 'why', text: why }) : null), h('span', {}));
}
function placeBtn(act, key, on, title, sub, pros, cons, why, recKey, readOnly) {
  return h('button', { class: 'place', type: 'button', role: 'radio', 'aria-checked': String(on), 'aria-disabled': why && !on ? 'true' : null, 'data-act': act, 'data-v': key, disabled: readOnly },
    h('span', { class: 'dot' }),
    h('span', {}, h('b', {}, title, key === recKey ? h('span', { class: 'tag', text: 'Recommended' }) : null), h('small', { text: sub }),
      pros ? h('small', { class: 'pros', text: '+ ' + pros }) : null, cons ? h('small', { class: 'cons', text: '− ' + cons }) : null,
      why && !on ? h('small', { class: 'why', text: why }) : null),
    h('span', {}));
}
function partDrawing(g, board, pick, artOnly) {
  if (g.conn === 'j7' || g.conn === 'j13') {
    const conn = g.conn === 'j7' ? { title: `${board.header.name} header, labels as printed`, rows: board.header.rows } : { title: `I2C connector (${g.connName}), as printed`, rows: g.rows };
    return h('div', { class: 'wiring-block' }, h('div', { class: 'wiring' }, wiring(conn, g.wires, { kind: g.kind, name: g.name, short: g.short, takenRC: g.taken, pick, key: g.key,
      reserved: Object.keys(reasons(board)).map(Number), reasons: reasons(board), label: `How the ${g.short} plugs in` })),
    h('p', { class: 'pin-caption', 'aria-live': 'polite', 'data-default': pick ? `DATA is on IO${norm(S.choice).rf_pin}. Click another dashed pin to move it.` : 'Point at a pin to see what it is.',
      text: pick ? `DATA is on IO${norm(S.choice).rf_pin}. Click another dashed pin to move it.` : 'Point at a pin to see what it is.' }));
  }
  if (g.conn === 'socket') {
    return h('table', { class: 'wires' }, h('thead', {}, h('tr', {}, ['Its pin', 'Goes to'].map((t) => h('th', { text: t })))),
      h('tbody', {}, g.wires.map((w) => h('tr', {}, h('td', {}, h('i', { class: 'sw s-' + w.colour }), h('b', { text: w.pin }), w.from ? h('small', { class: 'muted', text: ' (' + w.from + ')' }) : null),
        h('td', { text: cap(w.where) })))));
  }
  return h('div', { class: 'wiring-block' }, h('div', { class: 'wiring' }, artZigbee(board)),
    artOnly ? null : h('ol', { class: 'plug-steps' }, ZB_STEPS.map(([, text]) => h('li', { text }))));
}
function placeCard(g, board, rec, readOnly) {
  const c = norm(S.choice);
  let opts = [];
  const busy = (except) => socketUser(c, except) ? `${cap(socketUser(c, except))} is in the socket.` : '';
  if (g.key === 'rf' && c.rf === 'module') {
    opts = [placeBtn('rfplace', 'j7', c.rf_place === 'j7', `${board.header.name} header`, 'Three wires: power and ground, and DATA on one free pin.', 'Leaves the radio socket free for a Z-Wave or Zigbee radio.', 'Uses one of the pins wired zones can use.', '', rec.rf_place, readOnly),
      placeBtn('rfplace', 'socket', c.rf_place === 'socket', 'Radio socket', `Three wires onto the socket's pins; DATA on the pin printed IO${board.rf.default_gpio}.`, `Keeps every ${board.header.name} pin for wired zones.`, 'Nothing else can use the socket then.', busy('rf'), rec.rf_place, readOnly)];
  } else if (g.key === 'rf' && c.rf === 'cc1101') {
    opts = [placeBtn('rfplace', 'socket', c.rf_place === 'socket', 'Radio socket', 'Seven wires onto the socket\'s pins.', `Keeps ${board.header.name} free for wired zones.`, 'Nothing else can use the socket then.', busy('rf'), rec.rf_place, readOnly),
      j7Place(board) ? placeBtn('rfplace', 'j7', c.rf_place === 'j7', `${board.header.name} header`, `Seven wires onto ${board.header.name}.`, 'Leaves the radio socket free.', `Uses ${ccJ7Pins(board).length} pins wired zones could use.`, '', rec.rf_place, readOnly) : null];
  } else if (g.key === 'clock') {
    const p = clockJ7(board);
    const zw = c.zwave && c.zwave_place === 'j7' && zwClockClash(board);
    const sock = socketUser(c, 'zwave');
    opts = [p ? placeBtn('clockplace', 'j7', c.clock_place === 'j7', `${board.header.name} header, IO${p.sda} and IO${p.scl}`,
      zw && !sock ? `Four wires onto ${board.header.name}. The Z-Wave radio moves to the radio socket to make room.` : `Four wires onto ${board.header.name}.`,
      'Its own wiring, away from the touch screen. No wired zone lost.', '', zw && sock ? `The Z-Wave radio is on these pins, and ${sock} is in the radio socket, so the radio can't move there.` : '', rec.clock_place, readOnly) : null,
    placeBtn('clockplace', 'j13', c.clock_place === 'j13', `I2C connector (${(board.rtc.connector || {}).name || 'I2C'})`, 'A ready-made 4-wire lead plugs into it.', 'No loose wires on the header.', 'Shares the touch screen\'s wiring: keep the lead short.', '', rec.clock_place, readOnly)];
  } else if (g.key === 'zwave') {
    const j7 = zwPlace(board, 'j7');
    opts = [zwPlace(board, 'socket') ? placeBtn('zwplace', 'socket', c.zwave_place === 'socket', 'Radio socket', 'Four wires onto the socket\'s pins.', `No ${board.header.name} pins used.`, 'Nothing else can use the socket then.', busy('zwave'), rec.zwave_place, readOnly) : null,
      j7 ? placeBtn('zwplace', 'j7', c.zwave_place === 'j7', `${board.header.name} header, IO${j7.tx} and IO${j7.rx}`, `Four wires onto ${board.header.name}.`, 'Leaves the radio socket free. No wired zone lost.', c.clock ? 'The clock then goes on the I2C connector.' : '', '', rec.zwave_place, readOnly) : null];
  }
  const pick = !readOnly && g.key === 'rf' && c.rf === 'module' && c.rf_place === 'j7' ? (board.rf.pins || []).filter((p) => p !== c.rf_pin) : null;
  return h('section', { class: 'card part' },
    h('div', { class: 'part-head' }, h('span', { class: 'icon-tile' }, icon(g.ic)),
      h('div', {}, h('h2', { text: g.title }), h('p', { class: 'muted small', text: 'Goes on ' + g.place })), h('span', {})),
    opts.filter(Boolean).length ? h('div', { class: 'part-pick' }, h('div', { class: 'places', role: 'radiogroup', 'aria-label': 'Where it plugs in' }, opts)) : null,
    h('div', { class: 'part-body' },
      S.movedNote[g.key] ? note('info', 'info', '', S.movedNote[g.key]) : null,
      partDrawing(g, board, pick),
      g.notes.filter((x) => x[0] === 'warn').map(([k, ic, b, t]) => note(k, ic, b, t))));
}
function placesBlock(board, readOnly) {
  const c = norm(S.choice);
  const gs = groups(c, board);
  const rec = recommend(c, board);
  const issues = problems(c, board);
  const zones = zonesLeft(c, board);
  return [
    h('div', { class: 'wire-top' }, h('span', { class: 'spacer' }),
      readOnly ? null : h('button', { class: 'btn small', type: 'button', 'data-act': 'recommend', disabled: sameChoice(rec, c) }, 'Use the recommended places')),
    gs.map((g) => placeCard(g, board, rec, readOnly)),
    h('section', { class: 'card pincheck' },
      issues.length ? note('bad', 'triangle', issues.length === 1 ? 'One thing to change' : issues.length + ' things to change', issues.join(' '))
        : h('div', { class: 'pc-row' }, h('span', { class: 'icon-tile small' }, icon('circleCheck')), h('div', {}, h('b', { text: 'Everything fits' }), h('small', { text: 'No pin is used twice, and each part is in a place the panel accepts.' }))),
      board.rf ? h('div', { class: 'pc-row' }, h('span', { class: 'icon-tile small plain' }, icon('door')),
        h('div', {}, h('b', { text: `Pins left for wired door contacts: ${zones.length}` }), h('small', { text: (zones.length ? zones.map((n) => 'IO' + n).join(', ') : 'None') + ` on ${board.header.name}. A wired zone is a door or window contact wired between one of these and GND.` }))) : null)];
}

/* ---- guided setup: Plan, Wire, Install --------------------------------------------------- */
function runName() {
  const j = job();
  if (j && S.runJob && j.id === S.runJob) return /Reset/.test(j.title) ? 'Reset' : j.kind === 'update' ? 'Update' : 'Install';
  return runsRoboAlarms() ? (S.action === 'reset' ? 'Reset' : 'Update') : 'Install';
}
const STEPS = () => [['Plan', 'Panel and parts'], ['Wire', 'Get ready'], ['Wire', 'Full wiring'], ['Wire', 'Check before power'],
  ['Install', 'Connect'], ['Install', 'Check the panel'], ['Install', runName()], ['Install', 'Test and finish']];
const isCustom = () => norm(S.choice).layout === 'custom';
const wiresToDo = () => planned().length > 0 || isCustom();

function stepNote(i) {
  if (i >= S.step) return '';
  const d = dev();
  const b = boardOf();
  const gs = planned();
  switch (i) {
    case 0: return (planBoard() || {}).name ? `${planBoard().name} · ${isCustom() ? choiceLong(S.choice, planBoard()) : gs.length ? gs.map((g) => g.chip).join(', ') : 'nothing extra'}` : '';
    case 1: { const t = needs(gs).length + PREP.length; const n = Object.values(S.ready).filter(Boolean).length; return `${n} of ${t} ticked`; }
    case 2: { const t = gs.reduce((n, g) => n + items(g).length, 0); return `${gs.reduce((n, g) => n + doneOf(g), 0)} of ${t} ticked`; }
    case 3: return CHECKS.every(([id]) => S.checks[id]) ? 'All checked' : '';
    case 4: return S.port ? `Found on ${S.port}` : '';
    case 5: return runsRoboAlarms() ? `RoboAlarms ${d.probe.version || ''}` : (b ? b.name : '');
    case 6: {
      const img = chosenImage();
      return img ? `RoboAlarms ${img.version}` : '';
    }
    default: return '';
  }
}

function guided() {
  const locked = jobRunning() || (partsRun() && partsRun().state === 'running') || S.step === 7;
  const skip = !wiresToDo();
  const titles = STEPS();
  const rail = h('nav', { class: 'rail', 'aria-label': 'Setup steps' },
    ['Plan', 'Wire', 'Install'].map((part) => h('div', { class: 'rail-group' }, h('p', { class: 'rail-title', text: part }),
      h('ol', { class: 'steps' }, titles.map(([gp, title], i) => {
        if (gp !== part) return null;
        const sk = skip && i >= 1 && i <= 3;
        const st = sk ? 'skip' : i < S.step ? 'done' : i === S.step ? 'current' : 'todo';
        const noteText = sk ? 'Nothing to wire' : stepNote(i);
        const inner = [h('span', { class: 'num' }, st === 'done' ? icon('check') : String(i + 1)), h('span', {}, h('b', { text: title }), noteText ? h('small', { text: noteText }) : null)];
        return h('li', {}, st === 'done' && !locked
          ? h('button', { class: 'step done', type: 'button', 'data-act': 'goto', 'data-v': i, 'aria-label': `${title}: done. Go back to it` }, inner)
          : h('div', { class: 'step ' + st, 'aria-current': st === 'current' ? 'step' : null }, inner));
      })))),
    railFoot());
  const page = S.sub === 'places' ? pPlaces() : S.sub === 'print' ? pPrint() : S.sub === 'zigbee' ? pZigbee() : [pParts, pReady, pFull, pPower, pConnect, pCheck, pInstall, pTest][S.step]();
  return [rail, h('main', { class: 'content' }, h('div', { class: 'page', id: 'page' }, h('div', { class: 'inner' + (S.sub || [1, 2, 3].includes(S.step) ? ' wide' : '') }, page)),
    h('footer', { class: 'actions' }, footer()))];
}
function railFoot() {
  const b = S.state.bundle;
  if (serving()) return h('div', { class: 'rail-foot' }, versionPick());
  return h('div', { class: 'rail-foot' },
    b ? h('div', { class: 'fw' }, icon('download'), h('div', {}, h('b', { text: `RoboAlarms ${String(b.version).split(' ')[0]}` }), h('small', { text: `From ${b.name}` })))
      : h('div', { class: 'fw bad' }, icon('triangle'), h('div', {}, h('b', { text: 'No firmware loaded' }), h('small', {}, h('button', { class: 'link', type: 'button', 'data-act': 'open-bundle', text: 'Open the firmware file…' })))),
    S.state.bundle_error ? h('small', { class: 'muted', text: S.state.bundle_error }) : null);
}
/* The version list (FLS-033): the board's release on each channel, steadiest first; the one in use
 * is the steadiest there is (Stable, else Beta, else Alpha) or the person's pick. A channel with no
 * release of its own is listed greyed out, so the list says why the one in use was taken. */
function versionLabel(board, c) {
  const t = fwTarget(board, c);
  const own = ownRelease(board, c);
  if (own) return `${CHANNELS[c]} · ${own.version}`;
  if (!t || t.state === 'waiting' || t.state === 'checking') return `${CHANNELS[c]} · checking…`;
  return `${CHANNELS[c]} · ` + (t.state === 'failed' ? 'can\'t be checked' : 'none yet');
}
function versionPick() {
  const f = fw();
  const board = fwBoard();
  const bad = !!f.error && !board;
  const offered = !!board && (f.dev_offered || []).includes(board);
  const chosen = board ? (f.chosen || {})[board] : '';
  const words = 'From the firmware server' + (f.offline ? ' (offline)' : '');
  // Each item starts with its channel: the rail is narrow, and the list's label is cut at its edge.
  return h('div', { class: 'fw' + (bad ? ' bad' : '') }, icon(bad ? 'triangle' : 'download'),
    board ? h('div', { class: 'fw-pick' },
      h('select', { class: 'select', id: 'fw-version', 'data-act': 'fw-version', 'data-board': board, 'aria-label': `RoboAlarms version for the ${boardName(board)}` },
        offered ? h('option', { value: '', selected: true, disabled: true }, 'Choose a version') : null,
        f.channels.map((c) => h('option', { value: c, selected: !offered && c === chosen, disabled: !ownRelease(board, c) }, versionLabel(board, c)))),
      h('small', { text: 'RoboAlarms, f' + words.slice(1) }))
      : h('div', {}, h('b', { text: 'RoboAlarms' }), h('small', { text: words })));
}
function head(i, title, lead) {
  return [h('p', { class: 'eyebrow', text: `Step ${i + 1} of ${STEPS().length} · ${STEPS()[i][0]}` }), h('h1', { id: 'page-title', tabindex: -1, text: title }), lead ? h('p', { class: 'lead', text: lead }) : null, errorNote()];
}

/* Step 1: the panel, then what is connected - never where (the Flasher picks, BL-117). */
function pParts() {
  const board = planBoard();
  const known = boardOf();
  const offered = offeredBoards();
  const out = head(0, 'Your panel, and what you\'ll connect to it', 'Two choices. Then the Flasher shows how to wire everything, before the panel is plugged in.');
  if (!offered.length) { out.push(note('info', 'info', '', serving() ? 'The Flasher knows no panel to install. Check the firmware server under Advanced tools › Software.' : 'Open the RoboAlarms firmware file first (Advanced tools › Software).')); return out; }
  out.push(h('h2', { class: 'section', text: '1. Your panel' }));
  if (known) {
    const how = runsRoboAlarms() ? 'Your panel\'s software says so.' : 'The board you said it is when the Flasher checked it.';
    out.push(h('div', { class: 'option board-pick', role: 'radio', 'aria-checked': 'true' }, h('span', { class: 'dot' }), boardArt(known),
      h('span', {}, h('b', { text: known.long_name || known.name }), h('small', { text: how }))));
  } else {
    out.push(h('div', { class: 'boards', role: 'radiogroup', 'aria-label': 'Your panel' },
      offered.map(([id, x]) => boardOption(id, x, board === x, 'planboard', offered.length > 1 ? '' : 'The panel RoboAlarms is made for today. The Flasher checks it is this one when you plug it in.'))));
    if (offered.length > 1) out.push(h('p', { class: 'muted small', text: 'The size is the screen\'s, corner to corner, as on the box. The Flasher checks it with you when you plug the panel in.' }));
  }
  if (!board) { out.push(note('info', 'info', '', 'Choose your panel to go on: its wiring depends on it.')); return out; }
  if (!board.rf && !board.rtc) { out.push(note('info', 'info', '', 'This board has no parts to set up.')); return out; }
  out.push(h('h2', { class: 'section', text: '2. What do you want to connect?' }),
    h('p', { class: 'muted', text: 'Tick each thing you have, or plan to add. Just what it is: the Flasher works out where it goes.' }));
  if (runsRoboAlarms() && S.choiceFor !== S.port && !S.touched && !S.readingParts) setTimeout(readParts, 0);
  if (S.readingParts) { out.push(note('info', 'info', '', 'Reading the panel\'s settings…')); return out; }
  if (runsRoboAlarms() && S.choiceFor === S.port && !S.touched) out.push(note('info', 'info', 'Read from your panel:', 'these are its settings now. Change them only if you wire something new.'));
  const custom = isCustom();
  out.push(partCards(board, S.choice, custom));
  const gs = planned();
  out.push(custom ? note('info', 'info', `${cap(choiceLong(S.choice, board))}.`, 'Its pins are set in Advanced tools › Hardware › Custom pins. The Flasher keeps them when it updates the panel; the wiring steps show the table.') : gs.length ? note('good', 'circleCheck', `${gs.length === 1 ? 'One part' : gs.length + ' parts'} to connect.`, 'The Flasher picks the best place for each one, and the next steps show you how to wire it.')
    : note('info', 'info', 'Nothing extra?', 'That\'s fine: go on to plugging the panel in. You can add parts any time later.'));
  if (gs.length && !custom) {
    const own = !sameChoice(S.choice, recommend(S.choice, board));
    out.push(h('details', { class: 'more', 'data-key': 'adv', open: !!S.open.adv || own },
      h('summary', {}, icon('sliders'), 'Advanced: choose where each part goes'),
      h('p', { class: 'muted', text: 'Only if a part is already wired somewhere else, or you know you want another place. Otherwise leave it to the Flasher.' }),
      h('ul', { class: 'tight' }, gs.map((g) => h('li', { text: g.label }))),
      own ? note('info', 'info', runsRoboAlarms() && S.choiceFor === S.port && !S.touched ? 'These are the places your panel uses now.' : 'You chose these places yourself.', '') : null,
      h('div', { class: 'row-actions' }, h('button', { class: 'btn small', type: 'button', 'data-act': 'sub', 'data-v': 'places' }, 'Choose where each part goes'),
        own ? h('button', { class: 'btn small ghost', type: 'button', 'data-act': 'recommend' }, 'Use the recommended places') : null)));
  }
  return out;
}
/* Advanced, off the steps: choose where each part goes. */
function pPlaces() {
  const board = planBoard();
  return [h('p', { class: 'eyebrow', text: 'Advanced · Plan' }), h('h1', { id: 'page-title', tabindex: -1, text: 'Choose where each part goes' }),
    h('p', { class: 'lead', text: 'Only if a part is already wired somewhere else, or you want another place. The Flasher\'s own choice is marked Recommended, and the next steps follow whatever you pick.' }),
    errorNote(), ...placesBlock(board, false)];
}

/* Step 2: get ready - what you need, where the places are, the words, the panel off. */
/* Each row is a label: clicking its words ticks or unticks the box, not only the box itself. */
function tickList(list, store, act) {
  return h('ul', { class: 'need' }, list.map((it) => h('li', { class: it.danger ? 'danger' : null }, h('label', {},
    h('input', { type: 'checkbox', 'data-act': act, 'data-v': it.id, checked: !!store[it.id] }),
    h('span', {}, h('b', { text: it.text }), it.sub ? h('small', { text: it.sub }) : null),
    it.art ? artJumper() : it.danger ? icon('triangle') : h('span', {})))));
}
function connGuide(gs, board) {
  const uses = (conn) => gs.filter((g) => g.conn === conn || (conn === 'socket' && g.conn === 'plug'));
  const connCard = (title, art, find, conn) => h('div', { class: 'card conn' + (conn === 'j7' ? ' wide' : '') }, h('b', { text: title }), art, h('p', { text: find }),
    h('div', { class: 'chips' }, uses(conn).map((g) => h('span', { class: 'chip good' }, h('span', { class: 'num-badge', text: String(g.num || '') }), g.chip))));
  const cards = [];
  const hd = board.header;
  const cn = (board.rtc && board.rtc.connector) || { name: 'I2C', printed: 'I2C', pins: ['SCL', 'SDA', '3V3', 'GND'] };
  if (hd && uses('j7').length) cards.push(connCard(`${hd.name} header`, miniHeader(hd.rows), `A long double row of ${hd.rows.length * hd.rows[0].length} pins, with ${hd.name} printed beside it. One row is printed 5V, the other 3V3.`, 'j7'));
  if (uses('j13').length) cards.push(connCard(`I2C connector (${cn.name})`, miniHeader([cn.pins]), `A small ${cn.pins.length}-pin plug, printed ${cn.printed || cn.name}, next to ${cn.name}.`, 'j13'));
  if (uses('socket').length) cards.push(connCard('Radio socket', miniSocket(), 'Two short rows of seven holes, made for Elecrow\'s plug-in radio module, with their labels printed beside them.', 'socket'));
  return h('div', { class: 'grid2' }, cards);
}
function glossary() {
  return details('gloss', [icon('book'), 'New to wiring? The words you\'ll see'],
    h('dl', { class: 'gloss' },
      [['Pin', 'A small metal post on a board. A wire\'s plug slides onto it.'], ['Header', 'A row of pins. J7 is a double row of 24.'],
        ['Jumper wire', 'A wire with a small plug on each end. "Female" plugs have a hole and slide onto a pin.'],
        ['3V3', '3.3 volts: the low power these parts run on.'], ['5V', '5 volts. Never wire a part\'s VCC to 5V: it can damage the panel.'],
        ['GND', 'Ground: the return path. Every part needs one GND wire.'], ['VCC', 'A part\'s power input. It always goes to 3V3 here.'],
        ['DATA, SDA, SCL, RX, TX…', 'Signal wires. The drawings show where each one goes; you don\'t need to know what they do.']]
        .map(([term, d]) => [h('dt', { text: term }), h('dd', { text: d })])));
}
function pReady() {
  const board = planBoard();
  const gs = planned();
  const j7 = gs.some((g) => g.conn === 'j7') || ownOnJ7(norm(S.choice));
  const custom = isCustom();
  return [...head(1, 'Get ready to wire', 'Gather these, find the places on your panel, then get the panel ready. Tick each one off as you go: nothing here is checked by the Flasher.'),
    gs.some((g) => g.key === 'zigbee') ? zbReadyCard() : null,
    h('section', { class: 'card' }, h('h2', { text: 'What you need' }), tickList(needs(gs), S.ready, 'ready')),
    custom ? customWiring(board) : null,
    !custom || gs.length ? h('h2', { class: 'section', text: gs.length > 1 ? 'Find the places your parts go' : 'Find the place your part goes' }) : null,
    !custom || gs.length ? connGuide(gs, board) : null,
    glossary(),
    h('details', { class: 'more', 'data-key': 'fit', open: S.open.fit ?? true }, h('summary', {}, icon('book'), 'How to fit a jumper wire'),
      h('div', { class: 'howto' }, artFit(), h('ul', {},
        h('li', { text: 'Hold the plug, not the wire, and push it straight down onto the pin until it stops.' }),
        h('li', { text: 'One wire per pin. If a plug feels loose, use another wire.' }),
        h('li', { text: 'Don\'t bend the pins. If a plug won\'t go on, check you have the right pin.' }),
        h('li', { text: 'The colours in the drawings are only a guide. Red for VCC and black for GND help, if you have them.' })))),
    j7 ? details('meter', [icon('info'), `Can't read the 5V and 3V3 labels beside ${board.header.name}?`],
      h('p', { class: 'muted', text: 'Find the 5V row with a multimeter before you get the panel ready:' }),
      h('ol', {}, h('li', { text: `Nothing wired to ${board.header.name} yet. Plug the panel in and switch it on.` }), h('li', { text: 'Set the meter to DC volts. Black probe on a pin printed GND.' }),
        h('li', { text: 'Red probe on the end pin of each row: one reads about 5 V, the other about 3.3 V.' }), h('li', { text: 'Switch the panel off and unplug it again.' }))) : null,
    h('section', { class: 'card' }, h('h2', { text: 'Get the panel ready' }), tickList(PREP.map(([id, text]) => ({ id, text })), S.ready, 'ready')),
    note('warn', 'power', 'Keep the panel switched off and unplugged', 'until the check in step 4. Wiring it while it is on can damage it.')];
}

/* Step 3: the full wiring - connector by connector in assembly order, and the tick list. */
function fullDrawings(gs, board, focus, paper, clickable, loupe = clickable, act = 'fullfocus') {
  const out = [];
  const fg = focus ? gs.find((g) => g.key === focus) : null;
  const want = (conn) => !fg || fg.conn === conn || (conn === 'socket' && fg.conn === 'plug');
  const list = (gg) => gg.map((g) => `${g.num} ${g.title}`).join(', ');
  const j7 = gs.filter((g) => g.conn === 'j7');
  const hd = board.header;
  if (j7.length && want('j7')) {
    const caption = fg && j7.length > 1 ? `Your other parts on ${hd.name} are faded. Each pin's number says which part it belongs to.`
      : !fg && j7.length > 1 && clickable ? 'The numbers beside the pins say which part each wire belongs to. Click a part, or pick it above, to show only its wiring.' : '';
    out.push(h('section', { class: 'card fullmap' }, h('div', { class: 'fm-head' }, h('h2', { text: `${hd.name} header` }), h('span', { class: 'muted small', text: list(j7) })),
      h('div', { class: 'wiring big' }, wiringAll(hd.rows, j7, { title: `${hd.name} header, labels as printed`, focus: fg ? fg.key : '', pick: clickable, pickAct: act, loupe, label: `On ${hd.name}: ` + list(j7) })),
      caption ? h('p', { class: 'small muted', text: caption }) : null));
  }
  const j13 = gs.find((g) => g.conn === 'j13');
  if (j13 && want('j13')) {
    out.push(h('section', { class: 'card fullmap' }, h('div', { class: 'fm-head' }, h('h2', { text: `I2C connector (${j13.connName})` }), h('span', { class: 'muted small', text: list([j13]) })),
      h('div', { class: 'wiring' }, wiring({ title: `I2C connector (${j13.connName}), as printed`, rows: j13.rows }, j13.wires, { kind: 'clock', name: j13.name, short: j13.short, key: j13.key, label: 'The clock on the I2C connector' }))));
  }
  const sock = gs.filter((g) => (g.conn === 'socket' || g.conn === 'plug') && (!fg || g.key === fg.key));
  if (sock.length && want('socket')) {
    out.push(h('section', { class: 'card fullmap' }, h('div', { class: 'fm-head' }, h('h2', { text: 'Radio socket' }), h('span', { class: 'muted small', text: list(sock) })),
      sock.map((g) => h('div', { class: 'sock-part' }, h('b', {}, numbered(g)), partDrawing(g, board, null, true)))));
  }
  return out;
}
function partFilter(gs, f, act = 'fullfocus') {
  const kids = [h('button', { type: 'button', 'aria-pressed': String(f === ''), 'data-act': act, 'data-v': '' }, 'All parts')];
  let last = '';
  for (const g of gs) {
    if (CONN_RANK[g.conn] !== CONN_RANK[last]) { last = g.conn; kids.push(h('span', { class: 'seg-sep', 'aria-hidden': 'true' })); }
    kids.push(h('button', { type: 'button', 'aria-pressed': String(f === g.key), 'data-act': act, 'data-v': g.key, title: `${g.title}, on ${g.place}`, 'aria-label': `${g.num}: ${g.title}, on ${g.place}` },
      h('span', { class: 'num-badge', text: String(g.num) }), g.chip));
  }
  return h('div', { class: 'seg parts-seg', role: 'group', 'aria-label': 'Show' }, kids);
}
/* The wires to fit, part by part in assembly order, ticked off as they go on. */
function tickSection(shown) {
  const tot = shown.reduce((n, g) => n + items(g).length, 0);
  const done = shown.reduce((n, g) => n + doneOf(g), 0);
  return h('details', { class: 'more ticks', 'data-key': 'ticks', open: S.open.ticks ?? true },
    h('summary', {}, icon('listChecks'), `Tick off each wire as you fit it: ${done} of ${tot}`),
    bar(tot ? done / tot : 0),
    shown.map((g) => h('div', { class: 'tick-part' },
      h('div', { class: 'tick-head' }, h('b', {}, numbered(g)), h('span', { class: 'muted small', text: `on ${g.place} · ${doneOf(g)} of ${items(g).length}` })),
      h('ul', { class: 'ticklist' }, items(g).map((it) => {
        const k = tickKey(g, it.id);
        const on = !!S.tick[k];
        return h('li', { class: on ? 'done' : null },
          h('label', {},
            h('input', { type: 'checkbox', 'data-act': 'tick', 'data-v': k, checked: on }),
            it.kind === 'wire' ? h('i', { class: 'sw s-' + it.colour }) : h('span', { class: 'wl-ic' }, icon('info')),
            it.kind === 'wire'
              ? h('span', {}, h('b', { text: it.pin }), it.from ? ` (${it.from})` : '', ' to ', h('b', { text: it.to }), h('small', { text: cap(it.where) }))
              : h('span', { text: it.text })));
      })),
      g.notes.filter((x) => x[0] === 'warn').map(([kind, ic, b, tx]) => note(kind, ic, b, tx)))));
}
function pFull() {
  const board = planBoard();
  const gs = planned();
  const f = gs.some((g) => g.key === S.fullFocus) ? S.fullFocus : '';
  const shown = f ? gs.filter((g) => g.key === f) : gs;
  return [...head(2, 'Your full wiring', 'Everything you chose, on the panel at once, numbered in the order to fit them. Wire from the pictures, and tick each wire off below as it goes on.'),
    h('div', { class: 'wire-top' }, gs.length ? partFilter(gs, f) : null, h('span', { class: 'spacer' }),
      h('button', { class: 'btn small', type: 'button', 'data-act': 'sub', 'data-v': 'print' }, icon('printer'), 'Print or save')),
    isCustom() ? customWiring(board) : null,
    ...fullDrawings(gs, board, f, false, true),
    shown.length ? tickSection(shown) : null];
}

/* Step 4: check before power - every part's connector as on Full wiring, with the same filter (its own
 * choice, opening on every part) and the magnifier, and six checks to tick if they help; Next never
 * waits for them (decided 2026-09-30). */
function pPower() {
  const board = planBoard();
  const gs = planned();
  const all = CHECKS.every(([id]) => S.checks[id]);
  const j7 = gs.some((g) => g.conn === 'j7');
  const cf = gs.some((g) => g.key === S.checkFocus) ? S.checkFocus : '';
  return [...head(3, 'Check before you switch it on', 'A last look, with the panel still off. Compare your board with the pictures. The checks are there to tick if they help.'),
    isCustom() ? customWiring(board) : null,
    gs.length ? h('h2', { class: 'section', text: 'Your panel should look like this' }) : null,
    j7 ? h('p', { class: 'muted small', text: `On ${board.header.name}, the amber pins are 5V: nothing of yours goes there.` }) : null,
    gs.length > 1 ? h('div', { class: 'wire-top' }, partFilter(gs, cf, 'checkfocus')) : null,
    ...fullDrawings(gs, board, cf, false, true, true, 'checkfocus'),
    h('section', { class: 'card' }, h('h2', { text: 'Checks' }), tickList(CHECKS.map(([id, text, sub, danger]) => ({ id, text, sub, danger })), S.checks, 'check')),
    all ? note('good', 'shield', 'All checked.', 'Now you can plug the panel in and switch it on.')
      : note('info', 'info', '', 'Found something? Fix it with the panel still off, before you plug it in.')];
}

/* The wiring sheet to print or save as a PDF: what you need, the full wiring, each part's wires
 * to tick, the checks. Paper is light in either theme. */
function pPrint() {
  const board = planBoard();
  const gs = planned();
  const custom = isCustom();
  const sheet = h('article', { class: 'sheet' },
    h('header', { class: 'sheet-head' }, h('div', {}, h('b', { text: 'Wiring sheet' }), h('span', { class: 'muted', text: board.long_name || board.name })),
      h('span', { class: 'muted', text: 'RoboAlarms Flasher · ' + new Date().toLocaleDateString(undefined, { day: 'numeric', month: 'long', year: 'numeric' }) })),
    h('div', { class: 'sheet-warn' }, h('b', { text: 'Before you start: ' }), 'POWER switch off and every cable out. Every VCC goes to 3V3, never 5V. Match the labels printed on the board and on each part, not the wire colours.'),
    gs.length || custom ? [
      h('section', { class: 'sheet-part' }, h('h2', { text: 'What you need' }), h('ul', {}, needs(gs).map((n) => h('li', {}, h('span', { class: 'box' }), n.text)))),
      custom ? h('section', { class: 'sheet-part' }, h('h2', { text: 'Your own board on J7' }), customTable(board)) : null,
      gs.length ? h('section', { class: 'sheet-part' }, h('h2', { text: 'The full wiring' }), fullDrawings(gs, board, '', true, false)) : null,
      gs.length ? h('h2', { text: 'Part by part: tick each wire as you fit it' }) : null,
      gs.map((g) => h('section', { class: 'sheet-part' },
        h('h2', {}, numbered(g), `, on ${g.place}`),
        h('table', { class: 'wires' }, h('thead', {}, h('tr', {}, ['', 'On the part', 'Goes to', 'Where'].map((t) => h('th', { text: t })))),
          h('tbody', {}, items(g).map((it) => h('tr', {}, h('td', {}, h('span', { class: 'box' })),
            it.kind === 'wire' ? [h('td', {}, h('i', { class: 'sw s-' + it.colour }), h('b', { text: it.pin }), it.from ? ' (' + it.from + ')' : ''), h('td', { text: it.to }), h('td', { class: 'muted', text: cap(it.where) })]
              : h('td', { colspan: 3, text: it.text }))))),
        g.notes.filter((x) => x[0] === 'warn').map(([, , b, t]) => h('p', {}, h('b', { text: b }), ' ' + t)))),
      h('section', { class: 'sheet-part' }, h('h2', { text: 'Before you switch it on' }), h('ul', {}, CHECKS.map(([, t, why, danger]) => h('li', { class: danger ? 'danger' : null }, h('span', { class: 'box' }), danger ? h('b', { text: t }) : t, why ? ' ' + why : '')))),
    ] : h('p', { text: 'No extra parts chosen: nothing to wire.' }),
    h('footer', { class: 'sheet-foot', text: 'Then plug the panel in and carry on in the Flasher: it installs RoboAlarms, sets the panel up for these parts, and you test each one at the end.' }));
  return [h('div', { class: 'no-print sheet-intro' },
    h('p', { class: 'eyebrow', text: 'Print or save' }), h('h1', { id: 'page-title', tabindex: -1, text: 'Wiring sheet' }),
    h('p', { class: 'lead', text: 'Print it for the bench, or save it as a PDF (on Windows, choose Microsoft Print to PDF). It follows the parts and places you chose.' })),
  sheet];
}

function portSummary(p) {
  const d = dev(p.device);
  const j = job();
  if (j && j.port === p.device && j.state === 'running') return j.title;
  if (!d) return p.description || '';
  if (d.busy === 'probing') return 'Looking at it…';
  if (d.busy === 'identifying') return 'Checking which board it is…';
  const pr = d.probe;
  if (pr && pr.error) return pr.error.kind === 'busy' ? 'In use by another program' : 'Can\'t be opened';
  if (pr && pr.answered) return `RoboAlarms ${pr.version || ''}` + (pr.busy_reason ? ' · not idle' : '');
  const det = d.detection || {};
  const b = (S.state.catalogue || {})[det.board];
  if (b && (det.confidence === 'likely' || det.confidence === 'sure')) return b.name;
  if (d.chip) return d.chip.chip_name;
  if (pr) return 'No RoboAlarms software yet';
  return p.description || 'USB serial device';
}

/* The maker's advice for plugging a panel in (boards.json "connect", FLS-026): the board once it
 * is known, before that the one board whose USB bridge this port has - a hint for words only,
 * never for the firmware (FLS-005) - else, named, the one board that has advice. */
function connectAdvice() {
  let b = boardOf();
  const p = portInfo(S.port);
  const all = Object.values(S.state.catalogue || {});
  if (!b && p && p.usb_id) {
    const same = all.filter((x) => (x.usb || []).some((u) => u.usb_id === p.usb_id));
    if (same.length === 1) b = same[0];
  }
  if (b) return b.connect ? { board: b, sure: true } : null;
  const withAdvice = all.filter((x) => x.connect);
  return withAdvice.length === 1 ? { board: withAdvice[0], sure: false } : null;
}
function adviceList(items) {
  return (items || []).map((t) => h('li', {}, h('b', { text: t.do }), t.why ? ' ' + t.why : ''));
}
function powerCard(a) {
  return h('div', { class: 'note info' }, icon('plug'), h('div', {},
    h('b', { text: a.sure ? 'How to connect it' : `How to connect a ${a.board.name}` }),
    h('ul', { class: 'tight' }, adviceList(a.board.connect.steps))));
}

function pConnect() {
  const list = candidates();
  const found = !!S.port && !!portInfo(S.port);
  const advice = connectAdvice();
  const out = head(4, 'Plug the panel into this computer',
    'Use a USB-C cable that carries data, not a charge-only one. The Flasher finds the panel by itself and won\'t change anything on it yet.');
  out.push(h('div', { class: 'hero' }, artConnect(found, advice && advice.board.connect)));
  if (S.unplugged && !found) out.push(note('warn', 'plug', 'The panel was unplugged.', 'Plug it in again to carry on.'));
  if (!list.length) {
    // Windows lists a USB bridge but has no COM port for it (FLS-001): say why.
    for (const i of (S.state.driver_issues || [])) {
      const driver = i.chip.startsWith('CH34') ? 'CH340' : i.chip; // one driver serves the CH340 family
      out.push(i.kind === 'missing'
        ? note('warn', 'plug', `Windows found a ${i.chip} USB chip but has no driver for it.`, `Install the ${driver} USB driver, then plug the panel in again.`)
        : note('warn', 'plug', `Windows found a ${i.chip} USB chip but couldn’t start its driver.`, `Unplug the panel and plug it in again. If that doesn’t help, reinstall the ${driver} USB driver.`));
    }
    const tips = Object.values(S.state.catalogue || {}).flatMap((x) => (x.connect || {}).not_showing || []);
    out.push(h('div', { class: 'card' }, h('div', { class: 'status-row' }, h('span', { class: 'icon-tile plain' }, icon('usb')),
      h('div', {}, h('b', { text: 'Looking for a panel…' }), h('p', { class: 'muted', text: 'Plug it in. It shows up here within a few seconds.' }))),
    h('details', { class: 'help', 'data-key': 'help', open: !!S.open.help }, h('summary', { text: 'Nothing showing up?' }),
      h('ol', {},
        adviceList(tips),
        h('li', {}, h('b', { text: 'Try another cable.' }), ' Some only charge and can\'t carry data.'),
        h('li', {}, h('b', { text: 'Try another USB port' }), ' on this computer, plugged in directly rather than through a hub.'),
        h('li', {}, h('b', { text: 'Close other programs that might be using it,' }), ' like Arduino IDE, PuTTY or another copy of the Flasher.'),
        h('li', {}, h('b', { text: 'On Windows,' }), ' if Device Manager shows the panel under “Other devices”, install the CH340 USB driver, then plug the panel in again.')))));
  } else if (list.length === 1) {
    const p = list[0];
    out.push(h('div', { class: 'card' }, h('div', { class: 'status-row' }, h('span', { class: 'icon-tile' }, icon('circleCheck')),
      h('div', { class: 'grow' }, h('b', { text: runsRoboAlarms(p.device) ? 'Found your panel' : 'Found a panel' }),
        h('p', { class: 'muted', text: `${portSummary(p)} · USB on ${p.device}` + (p.bridge ? ` (${p.bridge})` : '') })))));
  } else {
    out.push(h('h2', { class: 'section', text: 'Found these' }));
    out.push(h('div', { class: 'ports-list', role: 'radiogroup', 'aria-label': 'Panels' }, list.map((p) =>
      h('button', { class: 'port-card', type: 'button', role: 'radio', 'aria-checked': p.device === S.port ? 'true' : 'false', 'data-act': 'port', 'data-v': p.device },
        h('span', { class: 'icon-tile small' + (p.kind === 'esp' ? '' : ' plain') }, icon('usb')),
        h('span', {}, h('b', { text: portSummary(p) }), h('small', { text: `${p.device} · ${p.description || 'USB serial device'}` })),
        h('span', { class: 'dot' })))));
  }
  if (advice) out.push(powerCard(advice));
  if (wiresToDo() && !keepParts()) out.push(note('good', 'circleCheck', 'Wired and checked.', 'After installing, the Flasher sets the panel up for your parts, and you test each one at the end.'));
  return out;
}

function boardCard(b, chips, how) {
  return h('div', { class: 'card board-card' }, h('div', {}, artPanel()),
    h('div', {}, h('h2', { text: b ? b.long_name || b.name : 'Unknown board' }),
      h('div', { class: 'chips' }, chips.map(([c, t, ic]) => h('span', { class: 'chip ' + c }, ic ? icon(ic) : null, t))),
      how ? h('p', { class: 'small muted how', text: how }) : null));
}
function techDetails() {
  const d = dev();
  const p = portInfo(S.port) || {};
  const rows = [['Port', S.port], ['USB bridge', [p.bridge, p.usb_id].filter(Boolean).join(' · ') || p.description || '']];
  if (d.probe && d.probe.answered) rows.push(['Running now', `RoboAlarms ${d.probe.version || ''} (${d.probe.profile || '?'})`]);
  if (d.chip) {
    rows.push(['Chip', `${d.chip.chip_name}, silicon ${d.chip.revision_text}`]);
    if (d.chip.flash_mb) rows.push(['Flash memory', d.chip.flash_mb + ' MB']);
    rows.push(['MAC address', d.chip.mac]);
  } else if (d.probe && d.probe.chip_revision != null) {
    rows.push(['Chip', `silicon v${Math.floor(d.probe.chip_revision / 100)}.${d.probe.chip_revision % 100}`]);
  }
  const img = chosenImage();
  if (img) rows.push(['Software for it', `RoboAlarms ${img.version}, ${img.silicon} (${img.profile})`]);
  const det = d.detection || {};
  if (det.reason) rows.push(['How it was recognised', det.reason]);
  return details('tech', [icon('info'), 'Technical details'], h('dl', { class: 'facts' }, rows.map(([k, v]) => [h('dt', { text: k }), h('dd', { text: v || '' })])));
}
function boardPicker(d) {
  const opts = d.options || [];
  return h('div', { class: 'boards', role: 'radiogroup', 'aria-label': 'Firmware for this board' },
    d.manual ? h('button', { class: 'option', type: 'button', role: 'radio', 'aria-checked': 'false', 'data-act': 'image', 'data-v': '' },
      h('span', { class: 'dot' }), h('span', {}, h('b', { text: 'Let the Flasher choose' }), h('small', { text: (d.detection || {}).reason || '' })), h('span', {})) : null,
    opts.map((o) => h('button', { class: 'option', type: 'button', role: 'radio', 'aria-checked': o.key === d.image_key ? 'true' : 'false',
      'aria-disabled': o.allowed ? null : 'true', 'data-act': 'image', 'data-v': o.key },
    h('span', { class: 'dot' }), h('span', {}, h('b', { text: o.board_name }), h('small', { text: `RoboAlarms ${o.version}${o.profile === 'dev' ? ' (development build)' : ''} · ${o.chip_name}, ${o.silicon}` }),
      o.blocked ? h('small', { class: 'why', text: o.blocked }) : null, o.allowed && o.confirm ? h('small', { class: 'why', text: o.confirm }) : null), h('span', {}))));
}

function pCheck() {
  const d = dev();
  if (!d) return head(5, 'No panel', 'Go back and plug it in.');
  const pr = d.probe;
  const b = boardOf();
  const det = d.detection || {};
  const out = [];
  if (d.busy === 'probing') return head(5, 'Looking at the panel…', 'Asking it what it is, without restarting it.');
  if (d.busy === 'identifying') return head(5, 'Checking which board this is…', 'It restarts into its loader for a moment, then the Flasher reads its chip. Nothing on it changes.');
  if (pr && pr.error) {
    out.push(...head(5, 'The Flasher can\'t open this port', pr.error.advice || ''));
    out.push(note('bad', 'triangle', pr.error.message, ''));
    out.push(h('div', { class: 'row-actions' }, h('button', { class: 'btn', type: 'button', 'data-act': 'probe' }, 'Try again')));
    return out;
  }
  if (pr && pr.answered) {
    const busy = pr.busy_reason;
    out.push(...head(5, `Your panel runs RoboAlarms ${pr.version || ''}`, 'It answered by itself, so nothing needed to restart.'));
    const img = chosenImage();
    const chips = [busy ? ['bad', busy, 'triangle'] : ['good', 'Disarmed and idle: safe to update', 'check']];
    if (img && pr.version && older(pr.version, img.version)) chips.push(['', `Update available: ${img.version}`, 'download']);
    out.push(boardCard(b, chips, 'How the Flasher knows: the panel\'s own software said so.'));
  } else if (!d.chip) {
    out.push(...head(5, 'Let\'s check which board this is',
      'A new panel still runs its maker\'s demo, which can\'t say what it is. To read its chip, the Flasher restarts it for a moment. Nothing on the panel changes.'));
    out.push(h('div', { class: 'card' }, h('div', { class: 'status-row' }, h('span', { class: 'icon-tile plain' }, icon('usb')),
      h('div', {}, h('b', { text: `USB device on ${S.port}` }), h('p', { class: 'muted', text: 'No RoboAlarms software answered. That is normal for a new panel.' })))));
    if (d.error) out.push(note('bad', 'triangle', d.error.message, d.error.advice || ''));
    out.push(note('info', 'info', '', 'Is something else on this USB port, like a 3D printer? Don\'t check it: go back, unplug it and plug in the panel.'));
    return out;
  } else if (det.confidence === 'likely' || det.confidence === 'sure') {
    out.push(...head(5, `This is a ${b ? b.name : det.board}`, chosenImage() ? `RoboAlarms ${chosenImage().version} is made for this board.` : ''));
    out.push(boardCard(b, [['good', chosenImage() ? `Ready for RoboAlarms ${chosenImage().version}` : 'Recognised', 'check'], ['', 'Now running: its maker\'s software']],
      'How the Flasher knows: ' + (det.reason ? det.reason[0].toLowerCase() + det.reason.slice(1) : 'its chip and memory match this board.')));
  } else if (det.confidence === 'ambiguous') {
    const cat = S.state.catalogue || {};
    const ids = (det.candidates || []).filter((id) => cat[id]);
    if (!d.manual && ids.includes(S.planBoard) && S.boardTold[S.port] !== S.planBoard) {
      S.boardTold[S.port] = S.planBoard;       // the step-1 choice, shown checked and changeable
      setTimeout(() => api('select_board', { port: S.port, board: S.planBoard }), 0);
    }
    out.push(...head(5, 'Which board is this?', 'Its chip fits more than one panel, so the Flasher can\'t tell by itself. Choose yours: the wrong one puts the add-ons\' wiring on other pins.'));
    out.push(h('div', { class: 'boards', role: 'radiogroup', 'aria-label': 'Which board this is' },
      ids.map((id) => boardOption(id, cat[id], d.manual && d.board === id, 'board', id === S.planBoard ? 'What you chose in step 1.' : ''))));
    if (d.manual && b && chosenImage()) out.push(note('good', 'circleCheck', `Ready for RoboAlarms ${chosenImage().version} on the ${b.name}.`, 'It runs its maker\'s software now. The Flasher goes by your choice: these panels have the same chip, memory and USB connection.'));
  } else {
    out.push(...head(5, 'This board isn\'t supported yet', `This ${d.chip.chip_name} board isn't one RoboAlarms is made for yet.`));
    if (det.reason) out.push(note('info', 'info', '', det.reason));
    S.pickBoard = true;
  }
  const fwn = d.firmware && !chosenImage() ? fwNote(d) : null;
  if (fwn) out.push(fwn);
  const planned1 = (S.state.catalogue || {})[S.planBoard];
  if (planned1 && b && d.board && d.board !== S.planBoard && wiresToDo()) {
    out.push(note('warn', 'triangle', `In step 1 you chose the ${planned1.name}; this is a ${b.name}.`,
      'The wiring steps now show this board\'s pins. Go back and check your wiring against them before installing.'));
  }
  if (d.error) out.push(note('bad', 'triangle', d.error.message, d.error.advice || ''));
  if (!(pr && pr.answered)) {
    out.push(h('div', { class: 'row-actions' }, h('button', { class: 'link', type: 'button', 'data-act': 'pickboard' },
      S.pickBoard ? 'Hide the list of builds' : det.confidence === 'ambiguous' ? 'Choose the build yourself' : 'Not this board? Choose it yourself')));
    if (S.pickBoard) out.push(boardPicker(d));
  }
  out.push(techDetails());
  return out;
}

function summaryRow(k, v, step) {
  return h('div', { class: 'sum-row' }, h('span', { class: 'k', text: k }), h('span', { class: 'v', text: v }),
    step === null ? h('span', {}) : h('button', { class: 'link small', type: 'button', 'data-act': 'goto', 'data-v': step, text: 'Change' }));
}

/* The languages the chosen image may be installed with: its own release's (a file's serve every
 * image in it); English is always there; a draft pack is for development firmware only (a release
 * one refuses it). */
const sameRelease = (x, img) => !img || (x.origin || '') === (img.origin || '');
function offeredLanguages(img) {
  const b = S.state.bundle;
  return ((b && b.languages) || []).filter((l) => sameRelease(l, img) && !(l.draft && img && img.profile === 'release'));
}
/* The voices the chosen image takes (BL-204): one language's own, never another's (ADR-053). */
function offeredVoices(img) {
  const b = S.state.bundle;
  return ((b && b.voices) || []).filter((v) => sameRelease(v, img) && !(v.draft && img && img.profile === 'release'));
}
const voiceFor = (img, locale) => offeredVoices(img).some((v) => v.locale.toLowerCase() === (locale || 'en-US').toLowerCase());
function chosenLanguage(img) {
  return offeredLanguages(img).some((l) => l.locale === S.language) ? S.language : '';
}

function pInstall() {
  const d = dev();
  const j = job();
  const pr = d && d.probe;
  const runs = runsRoboAlarms();
  const img = chosenImage();
  const mine = j && j.port === S.port && (j.state === 'running' || j.id === S.runJob);
  if (mine || (partsRun() && partsRun().state === 'running')) return pRunning();
  const reset = runs && S.action === 'reset';
  const title = !img ? 'No firmware to install' : reset ? 'Reset to factory defaults' : runs ? `Ready to update to RoboAlarms ${img.version}` : `Ready to install RoboAlarms ${img.version}`;
  const lead = reset ? 'Everything on the panel is erased and RoboAlarms is installed fresh, as it came.'
    : runs ? 'Codes, users, zones, Wi-Fi and history stay as they are.' : 'Check this is right, then install.';
  const out = head(6, title, lead);
  if (serving() && !img) {
    out.push(fwNote(d) || note('info', 'info', '', 'Go back to Check the panel and choose the build for it.'));
    return out;
  }
  if (!S.state.bundle) {
    out.push(h('div', { class: 'card' }, h('h2', { text: 'Open the firmware file' }),
      h('p', { class: 'muted', text: 'Open the RoboAlarms firmware file you were given (it ends in .roboflash), or drop it onto this window.' }),
      h('div', { class: 'row-actions' }, h('button', { class: 'btn primary', type: 'button', 'data-act': 'open-bundle' }, icon('folder'), 'Open firmware…'))));
    return out;
  }
  const busy = busyReason();
  if (busy) out.push(note('bad', 'triangle', 'Not now.', busy));
  if (runs) {
    out.push(h('div', { class: 'places', role: 'radiogroup', 'aria-label': 'What to do' },
      placeButton('action', 'update', img ? `Update to RoboAlarms ${img.version}` : 'Update', 'Keeps codes, users, zones, Wi-Fi and history', !reset, '', false),
      placeButton('action', 'reset', 'Reset to factory defaults', 'Erases the whole panel and installs RoboAlarms fresh: a complete reflash', reset, '', false)));
  }
  out.push(h('div', { class: 'card summary' },
    summaryRow('Panel', `${(boardOf() || {}).long_name || (boardOf() || {}).name || 'The panel'} on ${S.port}`, 5),
    summaryRow('Software', img ? `RoboAlarms ${img.version}` + (runs && pr.version ? ` (it runs ${pr.version} now)` : `, the build for this panel's chip`) : 'None chosen', 5),
    summaryRow('Parts', keepParts() ? 'As they are on the panel (not changed)' : choiceLong(S.choice, boardOf()), 0)));
  const steps = reset || !runs
    ? [S.backup ? ['Back up what is on the panel now', 'a few minutes'] : reset ? null : ['No backup (you chose this)', ''],
      ['Erase it and install RoboAlarms', 'about a minute'], ['Start it and check it runs', 'up to 30 s']].filter(Boolean)
    : [['Check the panel is still disarmed', 'a moment'], ['Write the new software and check it', 'about a minute'], ['Restart and check it runs', 'up to 30 s']];
  if ((!runs || reset) && chosenLanguage(img)) steps.splice(steps.length - 1, 0, ['Set the panel\'s language', 'a moment']);
  if ((!runs || reset) ? voiceFor(img, chosenLanguage(img)) : offeredVoices(img).length) steps.splice(steps.length - 1, 0, [(!runs || reset) ? 'Install the language\'s voice' : 'Bring the voice up to date, if there is a newer one', 'under a minute']);
  if (!keepParts()) steps.push(['Set it up for your parts', 'one more restart']);
  out.push(h('h2', { class: 'section', text: 'What happens' }));
  out.push(h('div', { class: 'card', id: 'timeline-card' }, h('ol', { class: 'timeline' }, steps.map(([t, dd], i) =>
    h('li', {}, h('span', { class: 'tl-num', text: String(i + 1) }), h('span', { text: t }), h('span', { class: 'time', text: dd }))))));
  if (!runs || reset) {
    out.push(h('label', { class: 'option-card' + (S.backup ? ' on' : '') },
      h('input', { type: 'checkbox', 'data-act': 'backup', checked: S.backup }),
      h('span', {}, h('b', { text: reset ? 'Keep a copy of the panel as it is now' : 'Back up the panel first' }), reset ? '' : ' (recommended)',
        h('small', { text: reset ? 'Its settings, codes and history, to put back later (Advanced tools › Backups).'
          : 'The only way back to its maker\'s demo. Saved in ' + ((S.state.folders || {}).backups || 'the Backups folder') + '.' }))));
  }
  const langs = (!runs || reset) ? offeredLanguages(img) : [];
  if (langs.length) {
    const pick = chosenLanguage(img);
    out.push(h('div', { class: 'option-card' },
      h('span', {}, h('label', { for: 'panel-language' }, h('b', { text: 'Panel language' })),
        h('small', { text: 'The language the panel starts in, with its own voice. It can be changed on the panel later.' }),
        h('select', { class: 'select', id: 'panel-language', 'data-act': 'language' },
          h('option', { value: '', selected: pick === '' }, 'English' + (voiceFor(img, '') ? '' : ' (no voice)')),
          langs.map((l) => h('option', { value: l.locale, selected: pick === l.locale }, l.name + (voiceFor(img, l.locale) ? '' : ' (no voice)') + (l.draft ? ' (draft, for testing)' : '')))))));
  }
  if (reset) {
    out.push(h('label', { class: 'option-card danger' + (S.resetOk ? ' on' : '') },
      h('input', { type: 'checkbox', 'data-act': 'reset-ok', checked: S.resetOk }),
      h('span', {}, h('b', { text: 'Erase everything on this panel' }), h('small', { text: 'Codes, users, zones, Wi-Fi, integrations and the event history are gone for good. The panel starts its first-time setup afterwards.' }))));
  }
  if (img && img.confirm) {
    out.push(h('label', { class: 'option-card' + (S.imageOk ? ' on' : '') },
      h('input', { type: 'checkbox', 'data-act': 'image-ok', checked: S.imageOk }),
      h('span', {}, h('b', { text: 'I understand' }), h('small', { text: img.confirm }))));
  }
  out.push(note('good', 'shield', 'Safe to retry.', 'If the cable comes out or this computer sleeps, the panel isn\'t harmed: start again.'));
  const conn = (boardOf() || {}).connect;
  if (!runs && conn && conn.before_install) out.push(note('info', 'plug', 'Keep it powered.', conn.before_install));
  const pj = partsRun();
  if (pj && pj.state === 'failed') out.push(partsRunCard(pj));
  return out;
}

/* The running job and the parts after it: the card is filled again on every event without being
 * rebuilt, so a click on Stop is never lost to an update. */
const WEIGHTS = { check: 1, connect: 2, backup: 40, erase: 10, compare: 1, write: 25, verify: 5, language: 3, restart: 1, boot: 5 };
let leftWords = { at: 0, text: '' };
function pRunning() {
  const j = job();
  const pj = partsRun();
  const reset = j && /Reset/.test(j.title);
  const title = !j ? 'Setting up the parts' : j.state === 'running' ? (reset ? 'Resetting to factory defaults' : j.kind === 'update' ? 'Updating RoboAlarms' : 'Installing RoboAlarms')
    : j.state === 'done' ? 'Setting it up for your parts' : j.state === 'cancelled' ? 'Stopped' : 'It did not finish';
  const out = head(6, title, 'Keep the panel plugged in. You can use this computer meanwhile.');
  out.push(h('div', { class: 'card', id: 'job-card' },
    h('div', { class: 'overall' }, h('b', { id: 'job-pct' }), h('span', { class: 'muted', id: 'job-left' })),
    h('div', { class: 'bar big', role: 'progressbar', 'aria-label': 'Overall progress', 'aria-valuemin': 0, 'aria-valuemax': 100, id: 'job-bar' }, h('i', {})),
    h('ol', { class: 'job', id: 'job-steps' }),
    h('div', { id: 'job-end' })));
  if (pj || (j && !S.skipParts)) out.push(h('div', { class: 'card', id: 'parts-run' }));
  out.push(h('p', { class: 'small muted', text: 'The words change at most once a second, and nothing on this screen moves by itself.' }));
  setTimeout(() => { fillJobCard(); fillPartsRun(); }, 0);
  return out;
}
function stepRow(label, state, progress, detail) {
  return h('li', { class: state }, h('span', { class: 'mark' }, state === 'done' ? icon('check') : state === 'failed' ? icon('x') : state === 'warning' ? '!' : null),
    h('div', {}, h('b', { text: label }),
      state === 'running' && progress !== null ? bar(progress) : null,
      detail ? h('small', { text: detail }) : null));
}
function bar(fraction) {
  const i = h('i', {});
  i.style.width = Math.round(Math.max(0, Math.min(1, fraction)) * 100) + '%';
  return h('div', { class: 'bar' }, i);
}
function fillJobCard() {
  const card = $('job-card');
  const j = job();
  if (!card || !j) return false;
  let total = 0, done = 0;
  for (const st of j.steps) {
    const w = WEIGHTS[st.key] || 1;
    total += w;
    done += w * (st.state === 'done' || st.state === 'skipped' || st.state === 'warning' ? 1 : st.state === 'running' ? st.progress : 0);
  }
  const frac = total ? done / total : 0;
  $('job-pct').textContent = Math.round(frac * 100) + ' %';
  $('job-bar').firstChild.style.width = Math.round(frac * 100) + '%';
  $('job-bar').setAttribute('aria-valuenow', String(Math.round(frac * 100)));
  const now = Date.now();
  if (now - leftWords.at > 2000 || j.state !== 'running') {
    const elapsed = now / 1000 - j.started;
    let text = '';
    if (j.state === 'running' && frac > 0.08) {
      const left = Math.max(0, elapsed * (1 - frac) / frac);
      text = left < 60 ? 'Less than a minute left' : `About ${Math.round(left / 60)} minute${Math.round(left / 60) === 1 ? '' : 's'} left`;
    } else if (j.state === 'running') text = 'Starting…';
    else if (j.state === 'done') text = 'Done';
    leftWords = { at: now, text };
    $('job-left').textContent = text;
  }
  const list = clear($('job-steps'));
  for (const st of j.steps) list.append(stepRow(st.label, st.state, ['backup', 'erase', 'write', 'verify'].includes(st.key) ? st.progress : null, st.detail));
  const end = clear($('job-end'));
  if (j.state === 'running') {
    end.append(h('div', { class: 'row-actions' }, h('button', { class: 'btn', type: 'button', 'data-act': 'stop' }, icon('x'), 'Stop…')));
  } else if (j.state !== 'done') {
    const e = j.error || {};
    const again = j.kind === 'backup' || j.kind === 'restore' ? 'Close' : 'Try again';   // those start again from their own buttons
    end.append(note(j.state === 'cancelled' ? 'warn' : 'bad', 'triangle', e.title || 'It did not finish.', e.advice || ''),
      h('div', { class: 'row-actions' }, h('button', { class: 'btn primary', type: 'button', 'data-act': 'retry' }, again),
        h('button', { class: 'link', type: 'button', 'data-act': 'folder', 'data-v': 'logs', text: 'Open the logs' })));
  } else if (j.kind === 'radio') {
    end.append(note('good', 'circleCheck', `${(j.result || {}).next || 'Unplug its cable and put it aside'}.`, 'Fit it into the panel\'s radio socket with the panel switched off: Guided setup › Full wiring shows how.'),
      h('div', { class: 'row-actions' }, h('button', { class: 'btn ghost small', type: 'button', 'data-act': 'zb-again' }, 'Look at it again')));
  } else if (j.result && j.result.warning) {
    end.append(note('warn', 'triangle', 'Written, with a question.', j.result.warning));
  }
  return true;
}
function partsRunCard(pj) {
  const card = h('div', { class: 'card', id: 'parts-run' });
  setTimeout(fillPartsRun, 0);
  return card;
}
function fillPartsRun() {
  const card = $('parts-run');
  if (!card) return false;
  const pj = partsRun();
  clear(card).append(h('h2', { text: 'Set it up for your parts' }));
  if (!pj) {
    card.append(h('p', { class: 'muted small', text: 'After the panel starts: its settings for the parts you chose, then one more restart.' }));
    return true;
  }
  card.append(h('ol', { class: 'job' }, pj.steps.map((st) => stepRow(st.label, st.state, null, st.detail))));
  if (pj.state === 'failed') {
    card.append(note('bad', 'triangle', pj.error ? pj.error.title : 'It did not finish.', pj.error ? pj.error.advice : ''),
      h('div', { class: 'row-actions' }, norm(S.choice).layout === 'custom'
        ? h('button', { class: 'btn primary', type: 'button', 'data-act': 'cp-open' }, 'Check the pins, then apply again')
        : h('button', { class: 'btn primary', type: 'button', 'data-act': 'parts-apply' }, 'Try again'),
        h('button', { class: 'btn', type: 'button', 'data-act': 'to-test' }, 'Carry on without')));
  } else if (pj.state === 'done' && pj.warnings && pj.warnings.length) {
    card.append(note('warn', 'triangle', '', pj.warnings.join(' ')));
  }
  return true;
}

function pTest() {
  const j = job();
  const pj = partsRun();
  const reset = j && /Reset/.test(j.title);
  const kind = j ? j.kind : 'install';
  const title = reset ? 'Reset: RoboAlarms is running fresh' : kind === 'update' ? `Updated: RoboAlarms ${(chosenImage() || {}).version || ''} is running` : 'RoboAlarms is running on your panel';
  const out = [h('p', { class: 'eyebrow', text: `Step ${STEPS().length} of ${STEPS().length} · Install` }),
    h('div', { class: 'success' }, h('span', { class: 'big-check' }, icon('check')),
      h('div', {}, h('h1', { id: 'page-title', tabindex: -1, text: title }),
        h('p', { class: 'lead', text: kind === 'update' && !reset ? 'Its codes, zones and Wi-Fi came through unchanged.' : 'Finish on the touchscreen, then test your parts here.' }))),
    errorNote()];
  if (kind !== 'update' || reset) {
    out.push(h('div', { class: 'card' }, h('h2', { text: 'On the panel\'s touchscreen' }), h('ol', { class: 'todo' },
      h('li', { text: 'Join your Wi-Fi.' }), h('li', { text: 'Change the two factory codes when it asks (installer 69420, master 123456).' }))));
  }
  if (pj && pj.warnings && pj.warnings.length) out.push(note('warn', 'triangle', 'Check this:', pj.warnings.join(' ')));
  const c = norm(S.choice);
  out.push(h('h2', { class: 'section', text: 'Test your parts' }));
  if (S.skipParts || (c.rf === 'none' && !c.clock && !c.zigbee && !c.zwave)) {
    out.push(h('p', { class: 'muted', text: (S.skipParts ? 'You chose to set the parts up later' : 'No extra parts were chosen') + '. Add them any time: Advanced tools › Hardware.' }));
    return out;
  }
  if (c.rf !== 'none') out.push(rfTestCard());
  if (c.clock) out.push(clockCard());
  if (c.zigbee) out.push(zigbeeTestCard());
  if (c.zwave) out.push(zwaveCard(pj));
  return out;
}

/* The Z-Wave radio after "Set it up for your parts": whether it answered at the panel's start. */
function cpOffRow(part, text, label) {
  return h('div', {}, h('p', { class: 'small', text }), h('div', { class: 'row-actions' }, h('button', { class: 'btn ghost small', type: 'button', 'data-act': 'cp-off', 'data-v': part }, label)));
}
function zwaveCard(pj) {
  const found = !!(pj && pj.state === 'done' && pj.found && pj.found.zwave);
  const known = !!(pj && pj.state === 'done');
  return h('section', { class: 'card test' },
    h('div', { class: 'test-head' }, h('span', { class: 'icon-tile' }, icon('radio')),
      h('div', {}, h('h2', { text: 'Z-Wave radio' }),
        h('p', { class: 'muted small', text: found ? 'It answers. Add sensors on the touchscreen: Installer › Z-Wave › Add a device.'
          : known ? 'It didn\'t answer after the restart.' : 'Its answer shows here once the panel is set up for it.' })),
      h('span', { class: 'chip' + (found ? ' good' : ''), text: found ? 'Answering' : known ? 'Not answering' : 'Waiting' })),
    known && !found ? note('warn', 'triangle', 'Check its wiring:', 'VCC to 3V3, GND to GND, the panel\'s TX to the radio\'s RX and its RX to the radio\'s TX; then restart the panel.') : null,
    known && !found && norm(S.choice).layout === 'custom' ? cpOffRow('zwave', 'Check the TX/RX order: the radio\'s RX goes to the pin you chose for the panel\'s TX, and its TX to the pin for the panel\'s RX. Do it with the panel switched off.', 'Turn the Z-Wave radio off') : null);
}

/* ---- live receiver and clock -------------------------------------------------------------- */
function rfTestCard() {
  if (S.rfPort !== S.port && !jobRunning()) { S.rfPort = S.port; api('rf_start', { port: S.port }, true); }
  const card = h('section', { class: 'card test', id: 'rf-live' });
  setTimeout(fillRfLive, 0);
  return card;
}
function fillRfLive() {
  const card = $('rf-live');
  if (!card) return;
  const rf = rfOf();
  const st = rf.status || {};
  const kind = st.kind;
  const chip = { module: ['good', 'Working'], cc1101: ['good', 'Working'], missing: ['warn', 'Not found'], stopped: ['warn', 'Not running'],
    off: ['', 'Switched off'], absent: ['bad', 'Not in this firmware'], jammed: ['bad', 'Jammed'] }[kind] || ['', 'Asking…'];
  const facts = [st.words || 'Asking the panel…'];
  put(clear(card),
    h('div', { class: 'test-head' }, h('span', { class: 'icon-tile' }, icon('radio')),
      h('div', {}, h('h2', { text: 'Wireless sensors' }), h('p', { class: 'muted small', text: facts.join(' · ') })),
      h('span', { class: 'chip ' + chip[0] }, chip[0] === 'good' ? icon('check') : null, chip[1])),
    kind === 'missing' ? note('warn', 'triangle', 'No receiver answers where the panel listens.', 'Check VCC goes to 3V3, GND to GND and DATA to the pin shown under Connected parts, then restart the panel.') : null,
    kind === 'missing' && norm(S.choice).layout === 'custom' ? cpOffRow('rf', norm(S.choice).rf === 'cc1101'
      ? 'Check that SI and SO aren\'t swapped (SI takes what the panel sends, SO answers it), that GDO0 is on its pin, and each pin against your table. Do it with the panel switched off.'
      : 'Check that DATA is on the pin in your table, and that VCC and GND are connected. Do it with the panel switched off.', 'Turn the 433 MHz radio off') : null,
    h('p', {}, h('b', { text: rf.raw_on ? 'Listening: press a button on a remote, or open a door sensor near the panel.' : 'Start listening, then press a remote\'s button or open a sensor near the panel.' }),
      ' Each one appears below as the panel hears it.'),
    h('div', { class: 'row-actions' },
      h('button', { class: 'btn' + (rf.raw_on ? '' : ' primary'), type: 'button', 'data-act': 'listen', 'data-v': rf.raw_on ? 'off' : 'on' }, rf.raw_on ? 'Stop listening' : 'Start listening'),
      (rf.heard || []).length ? h('button', { class: 'btn ghost', type: 'button', 'data-act': 'rf-clear' }, 'Clear') : null),
    (rf.heard || []).length ? h('table', { class: 'wires' },
      h('thead', {}, h('tr', {}, ['Heard', 'Kind', 'Codes', 'Last'].map((t) => h('th', { text: t })))),
      h('tbody', {}, rf.heard.map((x) => h('tr', {}, h('td', {}, h('b', { text: x.device })), h('td', { text: x.protocol }),
        h('td', { class: 'mono', text: Object.entries(x.codes).map(([k, n]) => `${k} × ${n}`).join('  ') + (x.battery_low ? ' · battery low' : '') + (x.common ? ' · factory address' : '') }),
        h('td', { 'data-ago': x.last, text: ago(x.last) }))))) : null,
    h('p', { class: 'small muted', text: 'To use one as a zone: on the panel, Zones › Add a zone › 433 MHz sensor.' }));
}
function clockCard() {
  const card = h('section', { class: 'card test', id: 'clock-live' }, h('p', { class: 'muted', text: 'Asking the panel…' }));
  setTimeout(refreshClock, 0);
  return card;
}
async function refreshClock() {
  const port = S.port;
  const r = await api('clock_status', { port }, true);
  const card = $('clock-live');
  if (!card || S.port !== port) return;
  const c = r.ok ? r.clock : null;
  clear(card);
  if (!c) {
    card.append(h('p', { class: 'muted', text: 'The panel did not say. Is RoboAlarms running on it?' }), h('button', { class: 'btn small', type: 'button', 'data-act': 'clock-refresh' }, 'Ask again'));
    return;
  }
  const cls = { ok: 'good', lost: 'warn', failed: 'bad', other: 'warn', absent: '' }[c.module] || '';
  const diff = c.difference_s;
  // The badge says how the module is; the line under the title adds what it doesn't. The panel's
  // own words ("keeping time, 24.25 C") would say both again.
  const facts = [];
  if (typeof c.temp_c === 'number') facts.push(`${c.temp_c.toFixed(1)} °C`);
  const from = { none: 'Time not set yet', module: 'Time from the clock module', internet: 'Time from the time server',
    manual: 'Time set by hand' }[c.source];
  if (from) facts.push(from);
  put(card,
    h('div', { class: 'test-head' }, h('span', { class: 'icon-tile' }, icon('clock')),
      h('div', {}, h('h2', { text: 'Battery-backed clock' }), facts.length ? h('p', { class: 'muted small', text: facts.join(' · ') }) : null),
      h('span', { class: 'chip ' + cls }, cls === 'good' ? icon('check') : null, c.module_words || '')),
    c.available === false ? note('warn', 'triangle', c.reason || '', 'Update the panel to use a clock module.') : null,
    diff !== null && diff !== undefined ? h('p', { text: Math.abs(diff) <= 1 ? 'The panel\'s clock matches this computer to the second.' : `The panel's clock is ${Math.abs(diff)} s ${diff < 0 ? 'behind' : 'ahead of'} this computer.` }) : null,
    c.module === 'absent' ? note('warn', 'triangle', 'No clock module answered.', 'Check its four wires, then restart the panel so it looks again.') : null,
    h('div', { class: 'row-actions' },
      h('button', { class: 'btn small', type: 'button', 'data-act': 'clock-set', disabled: !c.settable }, 'Set it from this computer'),
      h('button', { class: 'btn ghost small', type: 'button', 'data-act': 'clock-refresh' }, 'Check again')));
}

/* ---- the footer ---------------------------------------------------------------------------- */
const nextStep = () => (S.step === 0 && !wiresToDo() ? 4 : Math.min(7, S.step + 1));
const prevStep = () => (S.step === 4 && !wiresToDo() ? 0 : Math.max(0, S.step - 1));
function nextBtn(text, disabled) {
  return h('button', { class: 'btn primary', type: 'button', 'data-act': 'next', disabled }, text, icon('right'));
}
function footer() {
  const out = [];
  const locked = jobRunning() || (partsRun() && partsRun().state === 'running');
  const d = dev();
  if (S.sub === 'zigbee') return zbFooter();
  if (S.sub === 'places') {
    const issues = problems(S.choice, planBoard());
    out.push(h('span', { class: 'spacer' }));
    if (issues.length) out.push(h('span', { class: 'why', text: 'Change what the check marks' }));
    out.push(h('button', { class: 'btn primary', type: 'button', 'data-act': 'sub', 'data-v': '', disabled: issues.length > 0 }, icon('check'), 'Done'));
    return out;
  }
  if (S.sub === 'print') {
    out.push(h('button', { class: 'btn ghost', type: 'button', 'data-act': 'sub', 'data-v': '' }, icon('left'), 'Back'), h('span', { class: 'spacer' }),
      h('button', { class: 'btn primary', type: 'button', 'data-act': 'print' }, icon('printer'), 'Print or save as PDF'));
    return out;
  }
  if (S.step > 0 && S.step < 7 && !locked) out.push(h('button', { class: 'btn ghost', type: 'button', 'data-act': 'back' }, icon('left'), 'Back'));
  out.push(h('span', { class: 'spacer' }));
  if (S.step === 0) {
    const board = planBoard();
    const issues = board ? problems(S.choice, board) : [];
    if (issues.length) out.push(h('span', { class: 'why', text: 'Change the places under Advanced to go on.' }));
    out.push(nextBtn(wiresToDo() ? 'Next: get ready to wire' : 'Next: plug the panel in', !board || issues.length > 0 || S.readingParts));
  } else if (S.step === 1) {
    if (planned().some((g) => g.key === 'zigbee') && !zbDone()) out.push(h('span', { class: 'why', text: 'The Zigbee module still needs its radio software.' }));
    out.push(h('button', { class: 'btn', type: 'button', 'data-act': 'sub', 'data-v': 'print' }, icon('printer'), 'Print the wiring'), nextBtn('Next: see the full wiring'));
  } else if (S.step === 2) {
    out.push(nextBtn('Next: check before power'));
  } else if (S.step === 3) {
    out.push(nextBtn('Next: plug the panel in'));
  } else if (S.step === 4) {
    out.push(nextBtn('Next: check the panel', !S.port || !d || !!d.busy));
  } else if (S.step === 5) {
    const needCheck = d && !(d.probe && d.probe.answered) && !d.chip && d.busy !== 'identifying' && !(d.probe && d.probe.error);
    if (needCheck) {
      out.push(h('button', { class: 'btn primary', type: 'button', 'data-act': 'identify', disabled: !!(d && d.busy) || jobRunning() }, icon('search'), 'Check this panel'));
    } else {
      const img = chosenImage();
      const which = ((d && d.detection) || {}).confidence === 'ambiguous';
      const why = !img ? (which && !d.manual ? 'Choose which board this is first.' : fwWhy(d) || 'Choose the build for this board first.') : !img.allowed ? img.blocked : '';
      if (why) out.push(h('span', { class: 'why', text: why }));
      out.push(nextBtn('Next: review', !!why || !!(d && d.busy)));
    }
  } else if (S.step === 6) {
    const j = job();
    const mine = j && j.port === S.port && (j.state === 'running' || j.id === S.runJob);
    if (!(mine || (partsRun() && partsRun().state === 'running'))) {
      const img = chosenImage();
      const runs = runsRoboAlarms();
      const reset = runs && S.action === 'reset';
      const blocked = !S.state.bundle || !img || !img.allowed || !!busyReason() || (reset && !S.resetOk) || (img && img.confirm && !S.imageOk) || jobRunning();
      out.push(h('button', { class: 'btn primary' + (reset ? ' danger' : ''), type: 'button', 'data-act': 'start', disabled: blocked },
        icon(reset ? 'undo' : 'download'), reset ? 'Reset to factory defaults' : runs ? `Update to ${img ? img.version : ''}` : `Install RoboAlarms ${img ? img.version : ''}`));
    }
  } else {
    out.push(h('button', { class: 'btn primary', type: 'button', 'data-act': 'finish' }, 'Finish'));
  }
  return out;
}

/* ---- the Zigbee module's radio software (FLS-030): Advanced tools, and a page in Get ready ------- */
/* The module is out of the panel, on its own USB port (303A:1001). The engine says which ports may be
 * one (device.role, device.maybe) and the page never decides a role itself; nothing opens such a port
 * until the person has ticked that the module is on its own and pressed a button. */
S.radioJob = '';      // the radio job this page started: never S.runJob, which is the panel's install
S.zb = { alone: false, keep: false, read: {}, reading: {}, done: false };   // keep: a copy of what it runs now (off unless ticked)
S.zbTest = null;      // Test and finish: what the panel's `zigbee status` said

const zbRole = (p) => {
  const d = dev(p.device);
  if (!d) return '';
  if (d.role === 'zigbee_module') return 'is';
  if (d.maybe === 'zigbee_module') return 'maybe';
  return d.needs_alone && d.chip ? 'not' : '';   // read, and it is another chip
};
const zbBundle = () => ((((S.state || {}).bundle) || {}).radios || []).find((x) => x.kind === 'zigbee') || null;
const zbJobPort = () => { const j = job(); return S.radioJob && j && j.id === S.radioJob ? j.port : ''; };
function zbList() {   // the module's ports, and the port of this page's radio job even while it re-appears
  const list = zbPorts();
  const jp = zbJobPort();
  if (jp && !list.some((p) => p.device === jp)) list.push({ device: jp });
  return list;
}
const zbPorts = () => ports().filter((p) => zbRole(p));
function zbCurrent() {
  const list = zbList();
  const jp = zbJobPort();
  return list.find((p) => p.device === jp) || list.find((p) => zbRole(p) !== 'not') || null;
}
function zbNow(p) {
  const j = job();
  if (S.radioJob && j && j.id === S.radioJob && j.port === p.device) return { st: j.state === 'running' ? 'running' : j.state === 'done' ? 'done' : 'failed', j };
  const r = S.zb.read[p.device];
  if (r) return { st: r.same ? 'same' : 'found', r };
  return { st: zbRole(p) === 'not' ? 'not' : 'seen' };
}
const zbDone = () => S.zb.done || Object.values(S.zb.read).some((r) => r && r.same);
const zbChip = (kind, ic, text) => h('span', { class: 'chip ' + kind }, ic ? icon(ic) : null, text);
const zbChipName = (c) => (/^esp32[a-z]\d$/.test(c || '') ? 'ESP32-' + c.slice(5).toUpperCase() : String(c || 'chip').toUpperCase());
const zbRev = (n) => (n ? `v${Math.floor(n / 100)}.${n % 100}` : '');
function zbRuns(r) {   // what the module runs now, in words
  const what = r.project ? ` (${r.project}${r.version ? ' ' + r.version : ''})` : '';
  if (r.runs === 'ours') {
    if (r.same) return `Runs RoboAlarms radio ${r.version}: the same as in this file.`;
    if (!r.radio_version) return `Runs RoboAlarms radio ${r.version}.`;
    return r.older ? `Runs RoboAlarms radio ${r.version}, older than the ${r.radio_version} in this file.` : `Runs RoboAlarms radio ${r.version}, not the ${r.radio_version} in this file.`;
  }
  if (r.runs === 'factory') return `Runs Elecrow's factory software${what}, not the radio software the panel needs.`;
  if (r.runs === 'other') return `Runs other software${what}, not the radio software the panel needs.`;
  return 'Has no software on it yet.';
}
function artModule() {
  return s('svg', { class: 'art zb radio-art', viewBox: '0 0 460 170', role: 'img', 'aria-label': 'The Zigbee module on its own, its USB-C cable to this computer' },
    s('rect', { class: 'mod', x: 40, y: 20, width: 80, height: 118, rx: 6 }),
    [0, 1, 2, 3, 4, 5, 6].map((i) => [s('rect', { class: 'leg', x: 32, y: 32 + i * 14, width: 8, height: 6, rx: 1 }), s('rect', { class: 'leg', x: 120, y: 32 + i * 14, width: 8, height: 6, rx: 1 })]),
    s('text', { class: 'ink', x: 80, y: 74, 'text-anchor': 'middle' }, 'ESP32-H2'),
    s('rect', { class: 'usb', x: 68, y: 132, width: 24, height: 10, rx: 3 }),
    s('path', { class: 'cable on', d: 'M80 142 V 156 Q 80 162 86 162 H 300 Q 306 162 306 156 V 128' }),
    s('rect', { class: 'laptop', x: 262, y: 30, width: 150, height: 92, rx: 6 }),
    s('rect', { class: 'laptop-screen', x: 272, y: 40, width: 130, height: 72, rx: 3 }),
    s('path', { class: 'laptop', d: 'M246 122 h182 l-10 12 h-162 z' }),
    s('text', { class: 'cap', x: 80, y: 12, 'text-anchor': 'middle' }, 'Out of the panel'),
    s('text', { class: 'cap', x: 337, y: 80, 'text-anchor': 'middle' }, 'This computer'),
    s('text', { class: 'cap', x: 190, y: 156, 'text-anchor': 'middle' }, 'USB-C data cable'));
}
const zbHead = (title, sub, c) => h('div', { class: 'list-row' }, h('span', { class: 'icon-tile small' }, icon('tower')), h('div', {}, h('b', { text: title }), h('small', { text: sub })), h('div', { class: 'row-actions' }, c));
/* The required tick: without it nothing opens the module's port. */
const zbTick = () => h('label', { class: 'option-card' + (S.zb.alone ? ' on' : '') },
  h('input', { type: 'checkbox', 'data-act': 'zb-alone', checked: S.zb.alone }),
  h('span', {}, h('b', { text: 'The module is on its own, not in a panel' }), h('small', { text: 'Looking at it restarts it. In a running panel, that would restart the panel too.' })));
const zbKeep = () => h('label', { class: 'option-card' + (S.zb.keep ? ' on' : '') },
  h('input', { type: 'checkbox', 'data-act': 'zb-keep', checked: S.zb.keep }),
  h('span', {}, h('b', { text: 'Keep a copy of what it runs now' }), h('small', { text: '4 MB, about a minute. The copy is kept in Backups.' })));
function zbFacts(p, r) {
  const pi = portInfo(p.device) || {};
  const bun = zbBundle();
  const runs = r.runs === 'ours' ? `RoboAlarms radio ${r.version}` : r.runs === 'empty' ? 'nothing' : `${r.project || 'unknown software'}${r.version ? ' ' + r.version : ''}`;
  const rows = [['Port', p.device], ['USB', [pi.description, pi.usb_id ? 'USB ' + pi.usb_id : ''].filter(Boolean).join(' · ')],
    ['Chip', `${zbChipName(r.chip)}${r.revision ? ', silicon ' + zbRev(r.revision) : ''}`], ['Runs now', runs],
    ['Software for it', bun ? `RoboAlarms radio ${bun.version}, ${serving() ? 'from the firmware server' : 'in ' + S.state.bundle.name}` : serving() ? 'not downloaded yet: it comes with the panel\'s firmware' : 'none in the loaded file']];
  return details('zbfacts', [icon('info'), 'Technical details'], h('dl', { class: 'facts' }, rows.map(([k, v]) => [h('dt', { text: k }), h('dd', { text: v })])));
}
function zbHelp() {
  return details('zbhelp', [icon('info'), 'Nothing showing up?'], h('ol', {},
    h('li', { text: 'Unplug the module\'s cable and plug it in again.' }), h('li', { text: 'Use another USB-C cable: many only charge.' }), h('li', { text: 'Try another USB port on this computer.' }),
    h('li', { text: 'Still nothing: hold the module\'s button printed BOOT, press and release the one printed RST (or RESET), then release BOOT. (Not tried on this module yet.)' })));
}
function zbNoneCard(guided) {
  const bun = zbBundle();
  return h('div', { class: 'card stack' },
    guided ? null : zbNoneHead(bun),
    h('ol', { class: 'plug-steps' }, h('li', { text: 'Keep the module out of the panel. If it is already in its socket, switch the panel off and lift the module straight out.' }),
      h('li', { text: 'Plug a USB-C data cable into the module and into this computer.' })),
    zbTick(),
    guided ? h('div', { class: 'status-row' }, h('span', { class: 'icon-tile small plain' }, icon('usb')), h('div', { class: 'grow' }, h('b', { text: 'Waiting for the module' }), h('p', { class: 'small muted', text: 'Nothing new is plugged in yet.' }))) : null);
}
const zbNoneHead = (bun) => zbHead('Zigbee module (ESP32-H2)', bun ? `RoboAlarms radio ${bun.version} is in this file. No module is plugged in.` : 'No module is plugged in.', zbChip('', '', 'Not found'));
function zbCard(p, guided) {
  const port = p.device;
  const { st, r, j } = zbNow(p);
  const d = dev(port) || {};
  const bun = zbBundle();
  const busy = jobRunning() || !!d.busy || !!S.zb.reading[port];
  const onPort = `Zigbee module (ESP32-H2) on ${port}`;
  const btn = (cls, act, text, off) => h('div', { class: 'row-actions' }, h('button', { class: 'btn ' + cls, type: 'button', 'data-act': act, 'data-v': port, disabled: off }, text));
  if (st === 'not') {
    return h('div', { class: 'card stack' }, zbHead(`Not a Zigbee module · ${port}`, `It is an ${zbChipName((d.chip || {}).chip)}. The Flasher leaves it alone.`, zbChip('warn', 'triangle', 'Not a Zigbee module')));
  }
  if (st === 'seen') {
    const known = zbRole(p) === 'is';
    return h('div', { class: 'card stack' },
      zbHead(known ? onPort : `Something from Espressif on ${port}`, S.zb.reading[port] ? 'Looking at it…' : known ? 'An ESP32-H2, as Elecrow\'s Zigbee module is.' : 'USB 303A:1001. It may be the Zigbee module: the Flasher hasn\'t looked yet.', null),
      guided ? h('div', { class: 'status-row' }, h('span', { class: 'icon-tile small' }, icon('usb')), h('div', { class: 'grow' }, h('b', { text: S.zb.alone ? 'Ready to check' : 'Tick the box above first' }), h('p', { class: 'small muted', text: 'Check the module looks at what it runs.' })))
        : btn('primary', 'zb-read', S.zb.reading[port] ? 'Looking…' : 'Check it', !S.zb.alone || busy));
  }
  if (st === 'found' || st === 'same') {
    const up = st === 'same';
    return h('div', { class: 'card stack' },
      zbHead(onPort, zbRuns(r), up ? zbChip('good', 'check', 'Up to date') : zbChip('warn', 'triangle', r.runs === 'ours' ? 'Not this version' : 'Needs its radio software')),
      up ? null : zbKeep(),
      bun ? null : note('warn', 'triangle', 'The loaded firmware file has no radio software for the Zigbee module.', 'Open one that has it: Advanced tools › Software.'),
      guided ? null : btn(up ? '' : 'primary', 'zb-install', up ? 'Install it again' : 'Install the radio software', busy || !bun || !S.zb.alone),
      zbFacts(p, r));
  }
  // running, done or failed: the job card, filled by fillJobCard
  const card = h('div', { class: 'card stack', id: 'job-card' },
    zbHead(onPort, st === 'done' ? `RoboAlarms radio ${(j.result || {}).radio_version || ''}, written and checked.` : j.title, st === 'done' ? zbChip('good', 'check', 'Done') : st === 'failed' ? zbChip('bad', 'x', 'Did not finish') : null),
    h('div', { class: 'overall' }, h('b', { id: 'job-pct' }), h('span', { class: 'muted', id: 'job-left' })),
    h('div', { class: 'bar big', role: 'progressbar', 'aria-label': 'Overall progress', 'aria-valuemin': 0, 'aria-valuemax': 100, id: 'job-bar' }, h('i', {})),
    h('ol', { class: 'job', id: 'job-steps' }), h('div', { id: 'job-end' }));
  setTimeout(fillJobCard, 0);
  return card;
}
function zbBody(guided) {
  const list = zbList();
  const cur = zbCurrent();
  const st = cur ? zbNow(cur).st : 'none';
  const out = [];
  if (!cur || st === 'seen') out.push(h('div', { class: 'hero' }, artModule()));
  if (!cur) out.push(zbNoneCard(guided));
  if (list.some((p) => zbNow(p).st === 'seen' || (!S.zb.alone && ['found', 'same'].includes(zbNow(p).st)))) out.push(zbTick());   // one tick, above the modules it lets the Flasher look at
  out.push(...list.map((p) => zbCard(p, guided)));
  if (guided && (!cur || st === 'seen')) out.push(zbHelp());
  return out;
}
function toolZigbee() {
  const copies = (S.state.backups || []).filter((b) => b.chip === 'esp32h2' || /^zigbee_module-/.test(b.name));
  return [...toolHead('Zigbee module', 'Puts the radio software on Elecrow\'s ESP32-H2 Zigbee module. The module is out of the panel, on its own USB-C cable: no panel is needed.'),
    ...zbBody(false),
    h('h2', { class: 'section', text: 'Copies' }),
    h('div', { class: 'card' },
      listRow('archive', 'What the module ran before', copies.length ? `${copies.length} cop${copies.length === 1 ? 'y' : 'ies'}, kept in Backups beside the panels' copies.` : 'No copies yet. Tick "Keep a copy of what it runs now" before installing; each copy is kept in Backups.',
        [h('button', { class: 'btn small', type: 'button', 'data-act': 'tool', 'data-v': 'backups' }, 'Open Backups')]),
      copies.map((b) => listRow('archive', b.name, `${mb(b.size)} · ${b.created || ''}`)))];
}
/* Guided, off the steps like places and print: Get ready opens it when the Zigbee module is ticked. */
function pZigbee() {
  const cur = zbCurrent();
  const now = cur ? zbNow(cur) : { st: 'none' };
  const eyebrow = h('p', { class: 'eyebrow', text: `Step 2 of ${STEPS().length} · Wire · Zigbee module` });
  if (now.st === 'done') {
    return [eyebrow, h('div', { class: 'success' }, h('span', { class: 'big-check' }, icon('check')),
      h('div', {}, h('h1', { id: 'page-title', tabindex: -1, text: 'The Zigbee module is ready' }), h('p', { class: 'lead', text: `RoboAlarms radio ${(now.j.result || {}).radio_version || ''}, written and checked.` }))), errorNote(),
      h('div', { class: 'card' }, h('h2', { text: 'Now' }), h('ol', { class: 'todo' }, h('li', { text: 'Unplug its USB cable.' }), h('li', { text: 'Put it aside. Full wiring shows where it goes, with the panel switched off.' }))),
      h('div', { class: 'row-actions' }, h('button', { class: 'btn ghost small', type: 'button', 'data-act': 'zb-again' }, 'Look at it again'))];
  }
  return [eyebrow, h('h1', { id: 'page-title', tabindex: -1, text: 'Put the radio software on the Zigbee module' }),
    h('p', { class: 'lead', text: 'Do this before the module goes into the panel. The panel isn\'t needed: it stays switched off.' }), errorNote(),
    ...zbBody(true),
    now.st === 'running' ? note('info', 'info', 'Leave the cable in until it says done.', 'If it comes out, nothing is harmed: plug it in and start again.') : null];
}
function zbFooter() {
  const cur = zbCurrent();
  const st = cur ? zbNow(cur).st : 'none';
  const d = (cur && dev(cur.device)) || {};
  const busy = jobRunning() || !!d.busy || !!(cur && S.zb.reading[cur.device]);
  const out = [];
  if (st !== 'running') out.push(h('button', { class: 'btn ghost', type: 'button', 'data-act': 'sub', 'data-v': '' }, icon('left'), 'Back'));
  out.push(h('span', { class: 'spacer' }));
  if (st === 'none' || st === 'seen' || st === 'found') {
    out.push(h('button', { class: 'btn ghost', type: 'button', 'data-act': 'sub', 'data-v': '', title: 'Leave the module out of the panel until it has its software.' }, 'Do this later'));
  }
  if (st === 'none' || st === 'seen') {
    out.push(h('button', { class: 'btn primary', type: 'button', 'data-act': 'zb-read', 'data-v': cur ? cur.device : '', disabled: !cur || !S.zb.alone || busy }, 'Check the module', icon('right')));
  } else if (st === 'found') {
    out.push(h('button', { class: 'btn primary', type: 'button', 'data-act': 'zb-install', 'data-v': cur.device, disabled: busy || !zbBundle() || !S.zb.alone }, 'Install the radio software', icon('right')));
  } else if (st === 'same' || st === 'done') {
    out.push(h('button', { class: 'btn primary', type: 'button', 'data-act': 'sub', 'data-v': '' }, 'Back to Get ready', icon('right')));
  }
  return out;
}
function zbReadyCard() {
  const done = zbDone();
  return h('section', { class: 'card' }, h('div', { class: 'test-head' }, h('span', { class: 'icon-tile' }, icon('tower')),
    h('div', {}, h('h2', { text: 'First: the Zigbee module\'s radio software' }), h('p', { class: 'muted small', text: done ? 'The Zigbee module has the panel\'s radio software.' : 'It comes with Elecrow\'s demo software. Put the panel\'s radio software on it before it goes in: about 2 minutes, the panel isn\'t needed.' })),
    done ? zbChip('good', 'check', 'Done') : zbChip('warn', 'triangle', 'Not done yet')),
  h('div', { class: 'row-actions' }, h('button', { class: 'btn ' + (done ? '' : 'primary'), type: 'button', 'data-act': 'sub', 'data-v': 'zigbee' }, done ? 'Look again' : 'Prepare the Zigbee module', icon('right'))));
}
/* Test and finish: the panel's own word on its Zigbee radio (`zigbee status`). The version is for information only. */
async function askZigbee() {
  const port = S.port;
  if (!port) return;
  S.zbTest = { port, asking: true, got: null };
  render(false);
  const r = await api('zigbee_status', { port }, true);
  S.zbTest = { port, asking: false, got: r.ok ? r.zigbee : null, failed: !r.ok, why: r.ok ? '' : ((r.error && r.error.title) || '') };
  render(false);
}
function zigbeeTestCard() {
  const z = S.zbTest;
  if ((!z || z.port !== S.port) && S.port) setTimeout(askZigbee, 0);
  const head = (sub, c, plain) => h('div', { class: 'test-head' }, h('span', { class: 'icon-tile' + (plain ? ' plain' : '') }, icon('tower')), h('div', {}, h('h2', { text: 'Zigbee radio' }), h('p', { class: 'muted small', text: sub })), c);
  const again = h('div', { class: 'row-actions' }, h('button', { class: 'btn small', type: 'button', 'data-act': 'zb-ask' }, 'Check again'));
  if (!z || z.port !== S.port || z.asking) return h('section', { class: 'card test' }, head('Asking the panel…', zbChip('', '', 'Waiting'), true));
  const g = z.got;
  if (z.failed || !g) return h('section', { class: 'card test' }, head('In the socket', zbChip('', '', 'Not known'), true), h('p', { text: `${(z.why || 'The panel did not answer').replace(/\.$/, '')}. Check again in a moment.` }), again);
  if (!g.known) return h('section', { class: 'card test' }, head('In the socket', zbChip('', '', 'Not known'), true), h('p', { class: 'muted', text: 'This RoboAlarms is older and does not report on its Zigbee radio, so the Flasher can\'t tell.' }));
  if (!g.enabled) return h('section', { class: 'card test' }, head('In the socket', zbChip('', '', 'Switched off'), true), h('p', { text: g.words || 'Zigbee is switched off on the panel.' }));
  if (g.ready) {
    return h('section', { class: 'card test' }, head(`In the socket${g.channel ? ' · ready on channel ' + g.channel : ''}`, zbChip('good', 'check', 'Working')),
      h('p', { text: 'The panel hears the Zigbee module. Add Zigbee sensors on the touchscreen: Installer › Zigbee › Add a device.' }),
      g.version ? details('zbver', [icon('info'), 'Radio software'], h('dl', { class: 'facts' }, h('dt', { text: 'Reported' }), h('dd', { text: String(g.version) }))) : null);
  }
  return h('section', { class: 'card test' }, head('In the socket', zbChip('warn', 'triangle', 'Not heard'), true),
    h('p', { text: 'The panel doesn\'t hear the Zigbee module. With the panel switched off, check it sits fully in its socket. If it does, it may still need its radio software: Advanced tools › Zigbee module.' }),
    h('div', { class: 'row-actions' }, h('button', { class: 'btn small', type: 'button', 'data-act': 'zb-ask' }, 'Check again'),
      h('button', { class: 'btn small ghost', type: 'button', 'data-act': 'zb-open' }, 'Open Zigbee module')));
}

/* ---- guided setup on custom pins (FLS-029): the table, read-only; the pins are chosen in Advanced tools ---- */
function customRows(c, board) {
  const rows = [];
  const pins = c.pins || {};
  const where = (g) => { if (typeof g !== 'number') return ''; const bp = cpBoardPins(board).find((x) => x.gpio === g); return bp ? `${bp.connector} ${bp.label}` : 'IO' + g; };
  const add = (mod, got) => { for (const sg of CP_MODS[mod].signals) rows.push([CP_MODS[mod].short, sg.pin, where((got || {})[sg.k]) || (sg.optional ? 'Not connected' : 'Not chosen')]); };
  if (c.rf === 'cc1101' || c.rf === 'module') add(c.rf, pins.rf);
  if (c.zwave) add('zwave', pins.zwave);
  if (c.clock) {
    if (pins.clock) add('clock', pins.clock);
    else rows.push(['Clock', 'Its four pins', `The I2C connector (${i2cName(board)})`]);
  }
  return rows;
}
function customTable(board) {
  const rows = customRows(norm(S.choice), board);
  return h('div', { class: 'tablewrap' }, h('table', { class: 'wires' }, h('thead', {}, h('tr', {}, ['Part', 'Module pin', 'Goes to'].map((t) => h('th', { text: t })))),
    h('tbody', {}, rows.map((r) => h('tr', {}, h('td', { text: r[0] }), h('td', { class: 'mono', text: r[1] }), h('td', { class: 'mono', text: r[2] }))))));
}
function customWiring(board) {
  return h('section', { class: 'card' }, h('h2', { text: 'Your own board on J7' }),
    h('p', { class: 'muted small', text: 'These are the pins you chose, set in Advanced tools › Hardware › Custom pins. Each module pin goes to the board pin beside it; every module also needs 3V3 and GND, as that tab says. Nothing is chosen here.' }),
    customTable(board));
}

/* ---- advanced tools ------------------------------------------------------------------------ */
const TOOLS = [['ports', 'usb', 'Panels and ports'], ['console', 'terminal', 'Console'], ['hardware', 'wrench', 'Hardware'], ['zigbee', 'tower', 'Zigbee module'],
  ['backups', 'archive', 'Backups'], ['software', 'download', 'Software'], ['logs', 'folder', 'Logs']];
function tools() {
  const d = dev();
  const pi = portInfo(S.port);
  const nav = h('nav', { class: 'rail', 'aria-label': 'Advanced tools' },
    h('p', { class: 'rail-title', text: 'Advanced tools' }),
    h('div', { class: 'panel-pick' }, h('span', { class: 'icon-tile small' }, icon('cpu')),
      h('div', {}, h('b', { text: S.port ? ((boardOf() || {}).name || pi && pi.description || S.port) : 'No panel' }),
        h('small', { text: S.port ? `${S.port} · ${portSummary(pi || { device: S.port })}` : 'Plug one in' }))),
    h('ul', { class: 'tool-list' }, TOOLS.map(([k, ic, t]) => h('li', {}, h('button', { class: 'tool' + (S.tool === k ? ' on' : ''), type: 'button', 'data-act': 'tool', 'data-v': k, 'aria-current': S.tool === k ? 'page' : null }, icon(ic), t)))),
    h('div', { class: 'rail-foot' }, h('button', { class: 'link small', type: 'button', 'data-act': 'mode', 'data-v': 'guided' }, icon('left'), 'Back to guided setup')));
  const page = { ports: toolPorts, console: toolConsole, hardware: toolHardware, zigbee: toolZigbee, backups: toolBackups, software: toolSoftware, logs: toolLogs }[S.tool]();
  return [nav, h('main', { class: 'content' }, h('div', { class: 'page', id: 'page' }, h('div', { class: 'inner wide' }, page)))];
}
function toolHead(title, lead) { return [h('h1', { id: 'page-title', tabindex: -1, text: title }), h('p', { class: 'lead', text: lead }), errorNote()]; }
function listRow(ic, title, sub, actions) {
  return h('div', { class: 'list-row' }, h('span', { class: 'icon-tile small plain' }, icon(ic)), h('div', {}, h('b', { text: title }), sub ? h('small', { text: sub }) : null),
    h('div', { class: 'row-actions' }, actions || []));
}
function needPanel() {
  return note('info', 'info', '', 'Plug a panel in and choose it under Panels and ports.');
}

function toolPorts() {
  const all = ports();
  const main = all.filter((p) => p.kind !== 'other');
  const other = all.filter((p) => p.kind === 'other');
  const row = (p) => {
    const role = zbRole(p);
    const sub = [p.description, p.bridge, p.usb_id ? 'USB ' + p.usb_id : ''].filter(Boolean).join(' · ');
    if (role === 'is' || role === 'maybe') {   // a Zigbee module is listed by itself, never chosen as the panel, never looked at here
      return listRow('tower', role === 'is' ? `Zigbee module (ESP32-H2) · ${p.device}` : `Espressif USB device · ${p.device}`, sub,
        [role === 'is' ? h('span', { class: 'chip good', text: 'Zigbee module' }) : null,
          h('button', { class: 'btn small', type: 'button', 'data-act': 'tool', 'data-v': 'zigbee' }, role === 'is' ? 'Open Zigbee module' : 'Is it the Zigbee module?')]);
    }
    return listRow(p.kind === 'esp' ? 'cpu' : 'usb', `${portSummary(p)} · ${p.device}`, sub,
      isModulePort(p) ? [] : [p.device === S.port ? h('span', { class: 'chip good', text: 'Chosen' }) : h('button', { class: 'btn small', type: 'button', 'data-act': 'port', 'data-v': p.device }, 'Choose'),
        p.kind === 'esp' ? h('button', { class: 'btn ghost small', type: 'button', 'data-act': 'probe', 'data-v': p.device, disabled: jobRunning() }, 'Look again') : null]);
  };
  return [...toolHead('Panels and ports', 'Everything plugged into this computer. Looking at a panel never restarts it.'),
    h('div', { class: 'card' }, main.length ? main.map(row) : h('p', { class: 'muted', text: 'No USB serial device is plugged in.' })),
    other.length ? h('button', { class: 'link', type: 'button', 'data-act': 'show-other' }, `${S.showOther ? 'Hide' : 'Show'} other ports (${other.length})`) : null,
    S.showOther && other.length ? h('div', { class: 'card' }, other.map(row)) : null];
}

const QUICK = ['status', 'health', 'rf', 'rf place', 'rtc', 'rtc place', 'zwave status', 'power', 'zones', 'c6 status', 'help'];
function logFor(port) {
  let entry = S.logs[port];
  if (!entry) entry = S.logs[port] = { el: h('div', { class: 'log', role: 'log', 'aria-label': 'Panel console', tabindex: 0 }), seq: 0 };
  entry.el.classList.toggle('hide-q', !S.showQuiet);
  return entry;
}
function lineEl(l) {
  let cls = l.level || '';
  if (l.source === 'you') cls = 'you';
  if (l.source === 'tool') cls = 'tool';
  if (l.quiet) cls += ' q';
  return h('div', { class: cls, text: (l.source === 'you' ? '> ' : '') + l.text });
}
function appendLog(port) {
  const entry = S.logs[port];
  if (!entry) return;
  const lines = (S.state.consoles[port] || []).filter((l) => l.seq > entry.seq);
  if (!lines.length) return;
  const el = entry.el;
  const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 40;
  for (const l of lines) el.append(lineEl(l));
  entry.seq = lines[lines.length - 1].seq;
  while (el.childNodes.length > 3000) el.removeChild(el.firstChild);
  if (atBottom) el.scrollTop = el.scrollHeight;
}
function consoleInput(port) {
  if (!S.inputs[port]) {
    const input = h('input', { type: 'text', 'aria-label': 'Console command', autocomplete: 'off', spellcheck: 'false', placeholder: 'Type a command, e.g. status' });
    input.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') sendCommand(port);
      else if (e.key === 'ArrowUp' && S.history.length) { S.histPos = Math.max(0, S.histPos - 1); input.value = S.history[S.histPos] || ''; e.preventDefault(); }
      else if (e.key === 'ArrowDown') { S.histPos = Math.min(S.history.length, S.histPos + 1); input.value = S.history[S.histPos] || ''; e.preventDefault(); }
    });
    S.inputs[port] = input;
  }
  return S.inputs[port];
}
function sendCommand(port) {
  const input = consoleInput(port);
  const text = input.value.trim();
  if (!text) return;
  S.history.push(text);
  S.histPos = S.history.length;
  input.value = '';
  api('console_send', { port, text });
}
function toolConsole() {
  if (!S.port) return [...toolHead('Console', 'The panel\'s service console.'), needPanel()];
  const d = dev() || {};
  const open = d.console_open;
  const busyJob = jobRunning() && job().port === S.port;
  const entry = logFor(S.port);
  appendLog(S.port);
  const input = consoleInput(S.port);
  input.disabled = !open;
  setTimeout(() => { entry.el.scrollTop = entry.el.scrollHeight; }, 0);
  return [...toolHead('Console', 'The panel\'s service console. Connecting doesn\'t restart the panel, and keypad codes are never typed here.'),
    h('div', { class: 'card' },
      h('div', { class: 'console-bar' },
        open ? h('button', { class: 'btn small', type: 'button', 'data-act': 'console-close' }, 'Disconnect')
          : h('button', { class: 'btn primary small', type: 'button', 'data-act': 'console-open', disabled: busyJob }, 'Connect'),
        QUICK.map((cmd) => h('button', { class: 'qc', type: 'button', 'data-act': 'quick', 'data-v': cmd, disabled: !open }, cmd)),
        h('span', { class: 'spacer' }),
        h('label', { class: 'check' }, h('input', { type: 'checkbox', 'data-act': 'show-quiet', checked: S.showQuiet }), 'Show background'),
        h('button', { class: 'btn small', type: 'button', 'data-act': 'console-save' }, 'Save log'),
        h('button', { class: 'btn small danger', type: 'button', 'data-act': 'console-restart', disabled: !open || !!busyReason() }, 'Restart panel…')),
      entry.el,
      h('div', { class: 'send' }, input, h('button', { class: 'btn primary small', type: 'button', 'data-act': 'console-send', disabled: !open }, 'Send')))];
}

/* ---- Hardware > Custom pins (FLS-029): the person's own board, every pin chosen by them ------- */
/* The pin keys are the engine's (parts.py CUSTOM_KEYS); `pin` is the name printed on the module and
 * `dir` the direction from the module's side. For the Z-Wave radio tx and rx are the panel's: its
 * TX goes to the radio's RX. */
const CP_MODS = {
  cc1101: { name: 'CC1101 board', short: 'CC1101', icon: 'radio', signals: [
    { k: 'mosi', pin: 'SI (MOSI)', dir: 'module SI ← panel MOSI', what: 'Data into the CC1101' },
    { k: 'miso', pin: 'SO (MISO)', dir: 'module SO → panel MISO', what: 'Data out of the CC1101' },
    { k: 'sck', pin: 'SCLK', dir: 'module SCLK ← panel SCK', what: 'The clock the panel sends' },
    { k: 'cs', pin: 'CSn', dir: 'module CSn ← panel CS', what: 'Selects the CC1101' },
    { k: 'gdo0', pin: 'GDO0', dir: 'module GDO0 → panel', what: 'What it hears' }],
    fixed: [['GDO2', 'Not connected'], ['VCC', 'Any 3V3 pin, never 5V. It may share one with other modules.'], ['GND', 'Any GND pin. It may share one too.']] },
  module: { name: 'Receiver module', short: 'Receiver', icon: 'radio', signals: [
    { k: 'data', pin: 'DATA', dir: 'module DATA → panel', what: 'What it hears' }],
    fixed: [['VCC', 'Any 3V3 pin, never 5V: DATA would carry 5 V into the panel.'], ['GND', 'Any GND pin. It may share one with other modules.']] },
  zwave: { name: 'Z-Wave radio (ZAC93)', short: 'Z-Wave', icon: 'radio', signals: [
    { k: 'tx', pin: 'RX (position 8)', dir: 'module RX ← panel TX', what: 'Takes what the panel sends' },
    { k: 'rx', pin: 'TX (position 10)', dir: 'module TX → panel RX', what: 'What the radio sends' },
    { k: 'reset', pin: 'RESET (position 7)', dir: 'module RESET ← panel', what: 'Optional: lets the panel restart the radio. Boards etched Version 2.0.', optional: true }],
    fixed: [['Position 1 (VCC)', 'Any 3V3 pin, never 5V.'], ['Position 6 (GND)', 'Any GND pin.'], ['Positions 2 and 4', 'Not connected: 5 V on a Raspberry Pi.']] },
  clock: { name: 'Battery-backed clock (DS3231)', short: 'Clock', icon: 'clock', signals: [
    { k: 'sda', pin: 'SDA', dir: 'both ways', what: 'Data' },
    { k: 'scl', pin: 'SCL', dir: 'module SCL ← panel SCL', what: 'The clock the panel sends' }],
    fixed: [['SQW, 32K', 'Not connected'], ['VCC', 'Any 3V3 pin, never 5V.'], ['GND', 'Any GND pin.']] },
};
// A pin is null (not chosen yet) or a GPIO number; the Z-Wave reset's "Not connected" is resetNC.
const cpBlankPins = () => ({ cc1101: { mosi: null, miso: null, sck: null, cs: null, gdo0: null }, module: { data: null },
  zwave: { tx: null, rx: null, reset: null }, clock: { sda: null, scl: null } });
const cpFresh = (ticks) => ({ pins: cpBlankPins(), resetNC: false, clockPlace: null, panel: null, cleared: false, notes: [], stash: null,
  applying: null, view: null, problems: [], checking: false, checked: '', seq: 0, timer: 0, ticks: ticks || { rev: false, unplug: false } });
S.hwTab = null;      // 'places' | 'custom'; null: custom when the panel reports a custom layout
S.customOk = null;   // the panel's RoboAlarms takes custom pins (parts_read)
S.cp = cpFresh();    // the Custom pins tab's draft: kept while the Flasher is open

const cpBoardPins = (board) => (((board || {}).custom || {}).pins || []);
function cpPartsActive(c) {
  const a = [];
  if (c.rf === 'cc1101' || c.rf === 'module') a.push(c.rf);
  if (c.zwave) a.push('zwave');
  if (c.clock && S.cp.clockPlace === 'custom') a.push('clock');
  return a;
}
function cpBadgeNo(c, mod) {   // the number each module carries on the J7 summary and its card
  const order = [];
  if (c.rf === 'cc1101' || c.rf === 'module') order.push(c.rf);
  if (c.zwave) order.push('zwave');
  if (c.clock) order.push('clock');
  return order.indexOf(mod) + 1;
}
function cpUses(c) {           // gpio -> [{mod, sig}]
  const u = {};
  for (const mod of cpPartsActive(c)) for (const sg of CP_MODS[mod].signals) {
    const g = S.cp.pins[mod][sg.k];
    if (typeof g === 'number') (u[g] = u[g] || []).push({ mod, sig: sg });
  }
  return u;
}
function cpWhoElse(c, g, mod, k) {   // only the modules on this tab: zones never block a pin here
  const u = (cpUses(c)[g] || []).filter((x) => !(x.mod === mod && x.sig.k === k));
  return u.length ? `used by ${CP_MODS[u[0].mod].short} ${u[0].sig.pin}` : '';
}
/* The choice the engine takes: the ticked parts of S.choice with the pins chosen on this tab. */
function cpChoice() {
  const c = norm(S.choice);
  if (c.rf !== 'cc1101' && c.rf !== 'module') c.rf = 'none';
  const pins = {};
  if (c.rf !== 'none') pins.rf = Object.assign({}, S.cp.pins[c.rf]);
  if (c.clock && S.cp.clockPlace === 'custom') pins.clock = Object.assign({}, S.cp.pins.clock);
  if (c.zwave) pins.zwave = Object.assign({}, S.cp.pins.zwave);
  delete c.notes;
  return Object.assign(c, { layout: 'custom', zwave_place: 'custom', clock_place: S.cp.clockPlace || '', pins });
}
function cpSync(touch) {
  S.choice = cpChoice();
  if (touch) S.touched = true;
  cpSchedule();
}
function cpSchedule() {   // ask the engine about the choice, a moment after the last change
  clearTimeout(S.cp.timer);
  S.cp.checking = true;
  S.cp.timer = setTimeout(cpCheck, 350);
}
async function cpCheck() {
  const parts = norm(S.choice);
  const key = JSON.stringify(parts);
  const seq = ++S.cp.seq;
  const r = await api('parts_check', { port: S.port, parts }, true);
  if (seq !== S.cp.seq) return;
  S.cp.checking = false;
  S.cp.checked = key;
  S.cp.problems = r.ok ? (r.problems || []) : [(r.error && r.error.title) || 'The Flasher could not check these pins.'];
  if (!fillCpCheck()) render(false);
}
/* The check's answer in place, the pin lists left alone: a list opened while it ran stays open
 * (FLS-029). False when the Custom pins tab isn't showing. */
function fillCpCheck() {
  const card = $('cp-check');
  const row = $('cp-actions');
  if (!card || !row || !S.cp.view) return false;
  const c = norm(S.choice);
  card.replaceWith(cpCheckCard(c, S.cp.view.board));
  row.replaceWith(cpActions(c, S.cp.view.runs));
  return true;
}
function cpEnter() {   // the tab opens on the parts ticked so far, every pin still to choose
  const c = norm(S.choice);
  if (c.layout === 'custom') return;
  S.cp.stash = { choice: JSON.parse(JSON.stringify(c)), touched: S.touched };
  S.choice = c;
  cpSync(false);
}
function cpLeave(board) {   // back to the suggested places
  const cur = norm(S.choice);
  if (cur.layout !== 'custom') return;
  const st = S.cp.stash;
  const base = st ? norm(st.choice) : defaultChoice();
  const same = st && ['clock', 'zwave', 'zigbee'].every((k) => base[k] === cur[k]) && (base.rf === cur.rf || (base.rf === 'auto' && cur.rf === 'none'));
  if (same) { S.choice = base; S.touched = st.touched; return; }
  const c = Object.assign(base, { rf: cur.rf, clock: cur.clock, zwave: cur.zwave, zigbee: cur.zigbee, layout: 'places', rf_place: 'socket', zwave_place: 'socket', clock_place: 'j13' });
  delete c.pins; delete c.notes;
  S.choice = recommend(c, board);
  S.touched = true;
}
function cpSeed(parts) {   // what a panel with a custom layout reports
  const cp = cpFresh(S.cp.ticks);
  const p = parts.pins || {};
  const take = (dst, src) => { for (const k of Object.keys(dst)) dst[k] = src && typeof src[k] === 'number' ? src[k] : null; };
  if (p.rf) take(cp.pins[parts.rf === 'cc1101' ? 'cc1101' : 'module'], p.rf);
  if (p.clock) take(cp.pins.clock, p.clock);
  if (p.zwave) { take(cp.pins.zwave, p.zwave); cp.resetNC = p.zwave.reset == null; }
  cp.clockPlace = parts.clock ? (parts.clock_place === 'custom' ? 'custom' : 'j13') : null;
  cp.notes = parts.notes || [];
  cp.panel = { pins: JSON.parse(JSON.stringify(cp.pins)), resetNC: cp.resetNC, clockPlace: cp.clockPlace };
  S.cp = cp;
}
function cpRestore() {
  const p = S.cp.panel;
  S.cp.pins = JSON.parse(JSON.stringify(p.pins));
  S.cp.resetNC = p.resetNC;
  S.cp.clockPlace = p.clockPlace;
  S.cp.cleared = false;
}
const hwTabNow = () => S.hwTab || (norm(S.choice).layout === 'custom' ? 'custom' : 'places');

function hwTabs(on) {
  const tab = (k, t) => h('button', { type: 'button', role: 'tab', 'aria-selected': String(on === k), 'aria-pressed': String(on === k), 'data-act': 'hw-tab', 'data-v': k }, t);
  return h('div', { class: 'seg tabs', role: 'tablist', 'aria-label': 'How the parts are placed' }, tab('places', 'Suggested places'), tab('custom', 'Custom pins'));
}
function cpPcard(key, on, title, text, ic, sub, extra) {
  return h('div', { class: 'pcard' + (on ? ' on' : '') + (extra ? ' ' + extra : '') },
    h('button', { class: 'pcard-head', type: 'button', role: 'checkbox', 'aria-checked': String(on), 'data-act': 'cp-part', 'data-v': key },
      h('span', { class: 'tick' }, on ? icon('check') : null), h('span', { class: 'icon-tile' }, icon(ic)), h('span', {}, h('b', { text: title }), h('small', { text }))),
    on && sub ? h('div', { class: 'subpick' }, sub) : null);
}
function cpModules(c) {
  const kinds = [['cc1101', 'CC1101 board', 'Eight pins: VCC, GND, SI, SO, SCLK, CSN, GDO0 and GDO2.'], ['module', 'Receiver module', 'Three pins: VCC, GND and DATA.']];
  const rfSub = [h('b', { class: 'small', text: 'Which one is on your board?' }),
    h('div', { class: 'choices', role: 'radiogroup', 'aria-label': '433 MHz radio' }, kinds.map(([k, t, d]) =>
      h('button', { class: 'choice', type: 'button', role: 'radio', 'aria-checked': String(c.rf === k), 'data-act': 'cp-rfkind', 'data-v': k }, h('span', { class: 'dot' }), h('b', { text: t }), h('small', { text: d })))),
    h('p', { class: 'small muted', text: 'One 433 MHz radio at a time: the panel listens with one.' })];
  return h('div', { class: 'grid2' },
    cpPcard('rf', c.rf !== 'none', '433 MHz radio', 'For 433 MHz door, window and motion sensors and key-fob remotes.', 'radio', rfSub, 'wide'),
    cpPcard('zwave', c.zwave, 'Z-Wave radio', 'A Zooz ZAC93, for Z-Wave door, window, motion, smoke and water sensors.', 'radio'),
    cpPcard('clock', c.clock, 'Battery-backed clock', 'Keeps the date and time through power cuts (DS3231, often sold as ZS-042).', 'clock'),
    cpPcard('zigbee', c.zigbee, 'Zigbee module', 'Elecrow\'s ESP32-H2 module. It goes in its own radio socket, not on J7: nothing to choose.', 'tower'));
}
function cpJ7Map(c, board) {
  const rows = (board.header && board.header.rows) || [];
  if (!rows.length) return null;
  const reach = new Set(cpBoardPins(board).map((p) => p.gpio));
  const u = cpUses(c);
  const cell = (lab) => {
    if (lab === '5V') return h('div', { class: 'j7cell p5' }, '5V', h('small', { text: 'never' }));
    if (lab === '3V3') return h('div', { class: 'j7cell p3' }, '3V3', h('small', { text: 'power' }));
    if (lab === 'GND') return h('div', { class: 'j7cell gnd' }, 'GND', h('small', { text: 'ground' }));
    const g = Number(String(lab).replace(/^IO/, ''));
    if (!reach.has(g)) return h('div', { class: 'j7cell off' }, lab, h('small', { text: 'not offered' }));
    if (u[g]) { const x = u[g][0]; return h('div', { class: 'j7cell used' }, h('span', { class: 'num-badge', text: String(cpBadgeNo(c, x.mod)) }), lab, h('small', { text: x.sig.pin.replace(/ \(.*\)/, '') })); }
    return h('div', { class: 'j7cell' }, lab, h('small', { text: 'free' }));
  };
  return h('section', { class: 'card stack' }, h('h2', { text: `${board.header.name || 'J7'}, as printed on the board` }),
    h('div', { class: 'tablewrap j7wrap' }, h('div', { class: 'j7map', role: 'img', 'aria-label': 'J7 with the pins your modules use' }, rows.flat().map(cell))),
    h('p', { class: 'small muted', text: 'The numbers are your modules below. Power and ground aren\'t set here: wire them to any 3V3 and GND pins. Which row is 5V hasn\'t been confirmed on every board: find it with a meter before you wire power.' }));
}
function cpPinTable(c, mod, board) {
  const def = CP_MODS[mod];
  const row = (sg) => {
    const cur = S.cp.pins[mod][sg.k];
    const nc = sg.optional && S.cp.resetNC;
    const sel = h('select', { class: 'select', id: `pin-${mod}-${sg.k}`, 'data-act': 'cp-pin', 'data-v': `${mod}.${sg.k}`, 'aria-label': `${def.short} ${sg.pin}: board pin` },
      h('option', { value: '', selected: cur === null && !nc, text: 'Choose a pin…' }),
      sg.optional ? h('option', { value: 'nc', selected: nc, text: 'Not connected' }) : null,
      cpBoardPins(board).map((p) => {
        const why = cpWhoElse(c, p.gpio, mod, sg.k);
        const name = `${p.label} - ${p.connector}`;
        return h('option', { value: String(p.gpio), disabled: !!why && cur !== p.gpio, selected: cur === p.gpio, text: why ? `${name} - ${why}` : name });
      }));
    return h('tr', {}, h('td', { text: sg.pin }), h('td', { class: 'dir', text: sg.dir }), h('td', {}, sel), h('td', { class: 'what', text: sg.what }));
  };
  return h('div', { class: 'tablewrap' }, h('table', { class: 'wires pins' },
    h('thead', {}, h('tr', {}, h('th', { text: 'On the module' }), h('th', { text: 'Direction' }), h('th', { text: 'On the panel' }), h('th', { text: '' }))),
    h('tbody', {}, def.signals.map(row), def.fixed.map(([p, t]) => h('tr', { class: 'fixed' }, h('td', { text: p }), h('td', { class: 'dir', text: '' }), h('td', { colspan: 2, text: t }))))));
}
function cpModCard(c, mod, board) {
  const def = CP_MODS[mod];
  const n = def.signals.filter((x) => !x.optional).length;
  const kids = [h('div', { class: 'part-head' }, h('span', { class: 'icon-tile' }, icon(def.icon)),
    h('div', {}, h('h2', {}, h('span', { class: 'num-badge', text: String(cpBadgeNo(c, mod)) }), def.name),
      h('p', { class: 'muted small', text: mod === 'clock' ? 'On the I2C connector, or two pins of your own' : `${n} pin${n > 1 ? 's' : ''} to set${def.signals.some((x) => x.optional) ? ', and whether its RESET is wired' : ''}` })), h('span'))];
  if (mod === 'clock') {
    const place = (v, t, d) => h('button', { class: 'place', type: 'button', role: 'radio', 'aria-checked': String(S.cp.clockPlace === v), 'data-act': 'cp-clockplace', 'data-v': v },
      h('span', { class: 'dot' }), h('span', {}, h('b', { text: t }), h('small', { text: d })), h('span'));
    kids.push(h('div', { class: 'part-pick' }, h('div', { class: 'places', role: 'radiogroup', 'aria-label': 'Where the clock is wired' },
      place('j13', 'The I2C connector (J13)', 'A lead into J13, not your board. No pins to choose.'),
      place('custom', 'Two pins on your board', 'On your board, with its own wiring away from the touch screen.'))));
    if (S.cp.clockPlace !== 'custom') return h('section', { class: 'card part' }, kids);
  }
  const body = [cpPinTable(c, mod, board)];
  if (mod === 'clock') body.push(note('warn', 'triangle', 'Pull-ups on your board.', 'SDA and SCL each need a resistor to 3V3 (most DS3231 boards have them). The panel\'s own are weak.'));
  if (mod === 'zwave') body.push(note('info', 'info', 'TX and RX cross over.', 'The radio\'s RX takes the panel\'s TX and the other way round: the usual mistake is wiring them straight.'));
  if (mod === 'cc1101') body.push(note('info', 'info', '', 'Keep the leads short (10 to 15 cm) and the antenna away from the panel\'s board.'));
  return h('section', { class: 'card part' }, kids, h('div', { class: 'part-body' }, body));
}
/* What is still wrong with the pins: the engine's answer, and the Z-Wave RESET not chosen yet
 * (the engine reads a blank RESET as "not connected", so only the page can ask for it). */
function cpProblems(c) {
  const open = cpPartsActive(c).includes('zwave') && S.cp.pins.zwave.reset == null && !S.cp.resetNC;
  return (open ? ['The Z-Wave radio: choose its RESET pin, or "Not connected".'] : []).concat(S.cp.problems);
}
function cpCheckCard(c, board) {
  const p = cpProblems(c);
  const used = cpUses(c);
  const left = cpBoardPins(board).filter((x) => !used[x.gpio]);
  const waiting = S.cp.checking || S.cp.checked !== JSON.stringify(norm(S.choice));
  return h('section', { class: 'card pincheck', id: 'cp-check' },
    waiting ? h('div', { class: 'pc-row' }, h('span', { class: 'icon-tile small plain' }, icon('cpu')), h('div', {}, h('b', { text: 'Checking the pins…' })))
      : p.length ? h('div', { class: 'pc-row' }, h('span', { class: 'icon-tile small bad' }, icon('triangle')), h('div', {}, h('b', { text: 'Not ready to apply' }), p.map((t) => h('small', { text: t }))))
        : h('div', { class: 'pc-row' }, h('span', { class: 'icon-tile small' }, icon('circleCheck')), h('div', {}, h('b', { text: 'Everything fits' }), h('small', { text: 'No pin is used twice, and every pin is one the panel accepts.' }))),
    h('div', { class: 'pc-row' }, h('span', { class: 'icon-tile small plain' }, icon('plug')), h('div', {}, h('b', { text: `Pins left for wired door contacts: ${left.length}` }),
      h('small', { text: left.length ? left.map((x) => x.label).join(', ') + '.' : 'None.' }))),
    h('div', { class: 'pc-row' }, h('span', { class: 'icon-tile small plain' }, icon('cpu')), h('div', {}, h('b', { text: 'The panel checks again' }), h('small', { text: 'It refuses a pin used twice or one it can\'t use, and stores nothing until every pin fits.' }))));
}
/* The board revision the pins are for, which the person confirms (boards.json "revision"). */
const cpRevision = () => { const b = boardOf(); return b && b.revision ? b : null; };
const cpTicked = () => (!cpRevision() || S.cp.ticks.rev) && S.cp.ticks.unplug;
function cpReadyCard(mode) {
  const box = (k, t, sm) => h('li', {}, h('label', {}, h('input', { type: 'checkbox', id: 'tick-' + k, 'data-act': 'cp-tick', 'data-v': k, checked: S.cp.ticks[k] }), h('span', {}, h('b', { text: t }), sm ? h('small', { text: sm }) : null), h('span')));
  return h('section', { class: 'card' }, h('h2', { text: mode === 'install' ? 'Before you install' : mode === 'update' ? 'Before you update' : 'Before you apply' }),
    note('warn', 'triangle', 'Every module\'s VCC goes to a 3V3 pin, never to 5V; GND goes to a GND pin.', 'Modules may share the same 3V3 and GND pins. J7\'s pins take 3.3 V at most: 5 V on a signal pin damages the panel.'),
    note('info', 'info', 'Which row of J7 is 5V:', 'it hasn\'t been confirmed on every board. Find it with a meter before you wire power.'),
    h('ul', { class: 'need' },
      cpRevision() ? box('rev', `This is a ${cpRevision().revision} board`, cpRevision().revision_note || '') : null,
      box('unplug', mode === 'apply' ? 'I\'ll apply first, then switch off and plug my board in' : `I'll ${mode} first, then switch off and plug my board in`, 'Until the panel restarts, it still drives the pins it used before.')));
}
function cpMode(runs) { return !runs ? 'install' : S.customOk === true ? 'apply' : 'update'; }
function cpButtonText(mode) {
  if (mode === 'install') return 'Install RoboAlarms with these pins…';
  if (mode === 'update') return `Update to ${(chosenImage() || S.state.bundle || {}).version || 'the new version'} with these pins…`;
  return 'Apply and restart the panel';
}
/* The apply button and why it waits, which the check's answer changes (fillCpCheck). */
function cpActions(c, runs) {
  const mode = cpMode(runs);
  const pj = partsRun();
  const waiting = S.cp.checking || S.cp.checked !== JSON.stringify(c);
  const ready = !waiting && !cpProblems(c).length && cpTicked();
  const why = waiting ? 'Checking the pins…' : cpProblems(c).length ? 'Choose the pins above first.' : !cpTicked() ? (cpRevision() ? 'Tick both checks first.' : 'Tick the check first.') : '';
  return h('div', { class: 'row-actions', id: 'cp-actions' },
    h('button', { class: 'btn primary', type: 'button', 'data-act': 'cp-apply', disabled: !ready || !!busyReason() || jobRunning() || !!(pj && pj.state === 'running') }, mode === 'apply' ? null : icon('download'), cpButtonText(mode)),
    why ? h('span', { class: 'why-inline', text: why }) : null,
    busyReason() ? h('span', { class: 'why', text: busyReason() }) : null,
    runs ? h('button', { class: 'btn ghost', type: 'button', 'data-act': 'parts-reread' }, 'Read the panel again') : null);
}
function toolCustomPins(board, runs) {
  const c = norm(S.choice);
  const mode = cpMode(runs);
  const pj = partsRun();
  S.cp.view = { board, runs };
  const out = [];
  if (S.cp.panel && runs) {
    out.push(S.cp.cleared
      ? h('div', { class: 'card' }, listRow('undo', 'Every pin is cleared', 'Choose them for your new board. The panel keeps using its current pins until you apply.',
        [h('button', { class: 'btn small', type: 'button', 'data-act': 'cp-undo' }, 'Undo')]))
      : h('div', { class: 'card' }, listRow('cpu', 'Read from the panel: the pins it uses now', 'New hardware? Reset them and choose each pin again. Nothing changes on the panel until you apply.',
        [h('button', { class: 'btn small', type: 'button', 'data-act': 'cp-reset' }, icon('undo'), 'Reset the pins')])));
  }
  if (S.cp.notes.length) out.push(note('warn', 'triangle', 'The panel refused some of its stored pins:', S.cp.notes.join(' ')));
  if (mode === 'install') out.push(note('info', 'download', 'This panel doesn\'t run RoboAlarms yet.', 'Choose your pins anyway. The Flasher installs RoboAlarms, starts it, then sets these pins, all in one go.'));
  if (mode === 'update') out.push(note('info', 'download', 'This panel\'s RoboAlarms can\'t take custom pins.', 'Choose your pins anyway. The Flasher updates it, keeping its codes, zones and settings, then sets these pins, all in one go.'));
  out.push(note('info', 'info', 'For your own board on J7.', 'Pick the modules on it, then the board pin each module pin is wired to. Power isn\'t asked for: every module\'s VCC goes to a 3V3 pin and its GND to a GND pin, and they may share.'));
  out.push(h('h2', { class: 'section', text: 'Your modules' }), cpModules(c));
  out.push(h('h2', { class: 'section', text: 'Which pin each one uses' }), cpJ7Map(c, board));
  if (c.rf === 'cc1101' || c.rf === 'module') out.push(cpModCard(c, c.rf, board));
  if (c.zwave) out.push(cpModCard(c, 'zwave', board));
  if (c.clock) out.push(cpModCard(c, 'clock', board));
  if (c.zigbee) out.push(h('section', { class: 'card part' }, h('div', { class: 'part-head' }, h('span', { class: 'icon-tile' }, icon('tower')),
    h('div', {}, h('h2', { text: 'Zigbee module' }), h('p', { class: 'muted small', text: 'In its socket - nothing to choose. It goes into the radio socket with the panel switched off and never shares a pin with J7.' })),
    h('span', { class: 'chip', text: 'Nothing to choose' }))));
  out.push(cpCheckCard(c, board), cpReadyCard(mode));
  out.push(mode === 'apply'
    ? note('warn', 'triangle', 'Apply first, then plug your board in.', 'Apply with your board unplugged. When the panel has restarted, switch it off, plug your board in and switch it on. Until it restarts, the panel still drives the pins it used before.')
    : note('warn', 'triangle', `${mode === 'install' ? 'Install' : 'Update'} first, then plug your board in.`, `The pins are set after the ${mode === 'install' ? 'install' : 'update'}, once RoboAlarms has started. Keep your board unplugged until the Flasher has finished. Then switch the panel off, plug your board in and switch it on.`));
  out.push(cpActions(c, runs));
  if (pj) out.push(partsRunCard(pj));
  if (runs) {
    out.push(h('h2', { class: 'section', text: 'Test' }));
    if (c.rf !== 'none') out.push(rfTestCard());
    if (c.clock) out.push(clockCard());
    if (c.zwave) out.push(zwaveCard(pj));
  }
  return out;
}
/* Apply on a panel that takes custom pins: the zones on the chosen pins are named first. */
async function cpApply() {
  const port = S.port;
  const runs = runsRoboAlarms();
  if (cpMode(runs) !== 'apply') {   // install or update: today's page, the choice kept (it rides in the job's parts)
    S.mode = 'guided'; S.action = 'update'; S.resetOk = false; S.backup = !runs; S.touched = true; go(6);
    return;
  }
  const parts = norm(S.choice);
  const z = await api('parts_zones', { port, parts }, true);
  const zones = z.ok ? (z.zones || []) : [];
  const list = [];
  for (const mod of cpPartsActive(cpChoice())) for (const sg of CP_MODS[mod].signals) {
    const g = S.cp.pins[mod][sg.k];
    list.push(h('li', { text: `${CP_MODS[mod].short} ${sg.pin}: ${typeof g === 'number' ? 'IO' + g : 'not connected'}` }));
  }
  if (parts.clock && S.cp.clockPlace === 'j13') list.push(h('li', { text: 'Clock: on the I2C connector (J13)' }));
  const ok = await ask({ title: 'Apply these pins and restart the panel?', ok: 'Apply and restart',
    body: [h('p', {}, 'The panel checks the pins, stores them and restarts once. ', h('b', { text: 'Your board stays unplugged until the end.' })), h('ul', { class: 'tight' }, list),
      ...zones.map((x) => h('p', { class: 'small' }, h('b', { text: `Zone ${x.zone}${x.name ? ` (${x.name})` : ''} is wired to IO${x.gpio} today. ` }),
        'Your board takes that pin: after the restart the zone shows Check until you give it another terminal or remove it.'))] });
  if (!ok) return;
  S.error = null;
  const r = await api('parts_apply', { port, parts });
  if (r.ok) {
    S.state.parts = S.state.parts || {}; S.state.parts[port] = r.parts_job;
    S.cp.applying = { port, pins: JSON.parse(JSON.stringify(S.cp.pins)), resetNC: S.cp.resetNC, clockPlace: S.cp.clockPlace };
  }
  render(false);
}

function toolHardware() {
  if (!S.port) return [...toolHead('Hardware', 'The parts wired to the panel.'), needPanel()];
  const b = boardOf();
  const runs = runsRoboAlarms();
  if (!b || (!b.rf && !b.rtc)) return [...toolHead('Hardware', 'The parts wired to the panel.'), note('info', 'info', '', 'This board has no parts to set up.')];
  if (runs && S.choiceFor !== S.port && !S.readingParts) readParts();
  const out = toolHead('Hardware', runs ? 'What is wired to the panel, and where. Change it here, then apply: the panel stores it and restarts once.'
    : 'What is wired to the panel. Install RoboAlarms first (guided setup): the panel is set up for these parts after it starts.');
  if (S.readingParts) { out.push(note('info', 'info', '', 'Reading the panel\'s settings…')); return out; }
  const pj = partsRun();
  if (b.custom) {
    const tab = hwTabNow();
    out.push(hwTabs(tab));
    if (tab === 'custom') {
      cpEnter();
      if (!S.cp.checking && S.cp.checked !== JSON.stringify(norm(S.choice))) cpSchedule();
      out.push(...toolCustomPins(b, runs));
      return out;
    }
    cpLeave(b);
    if (S.cp.panel && runs) out.push(note('info', 'info', 'This panel uses custom pins.', 'They were set on the Custom pins tab. Applying suggested places here replaces them.'));
  }
  out.push(h('h2', { class: 'section', text: 'What is connected' }), partCards(b, S.choice, false));
  if (groups(S.choice, b).length) out.push(h('h2', { class: 'section', text: 'Where each part goes' }), ...placesBlock(b, false));
  const issues = problems(S.choice, b);
  out.push(h('div', { class: 'row-actions' },
    runs ? h('button', { class: 'btn primary', type: 'button', 'data-act': 'parts-apply', disabled: issues.length > 0 || !!busyReason() || jobRunning() || (pj && pj.state === 'running') }, 'Apply and restart the panel')
      : h('button', { class: 'btn primary', type: 'button', 'data-act': 'places-install', disabled: issues.length > 0 || jobRunning() }, icon('download'), 'Install RoboAlarms with these parts…'),
    runs ? h('button', { class: 'btn ghost', type: 'button', 'data-act': 'parts-reread' }, 'Read the panel again') : null,
    busyReason() ? h('span', { class: 'why', text: busyReason() }) : null));
  if (pj) out.push(partsRunCard(pj));
  if (runs) {
    out.push(h('h2', { class: 'section', text: 'Test' }));
    if (norm(S.choice).rf !== 'none') out.push(rfTestCard());
    if (norm(S.choice).clock) out.push(clockCard());
    out.push(h('div', { class: 'card' }, listRow('radio', 'Record 3 seconds of raw pulses', 'Saved for the decoder\'s tests', [
      h('button', { class: 'btn small', type: 'button', 'data-act': 'capture', id: 'rf-record', disabled: rfRecording() }, 'Record'),
      h('button', { class: 'btn ghost small', type: 'button', 'data-act': 'folder', 'data-v': 'captures' }, 'Open the folder')])));
  }
  return out;
}

function toolBackups() {
  const backups = S.state.backups || [];
  const d = dev();
  const blocked = !S.port || !!busyReason() || !!(d && d.busy) || jobRunning();
  // The last backup or restore of this panel stays here once it ends: how it went, or why not.
  const j = job();
  const mine = j && (j.kind === 'backup' || j.kind === 'restore') && j.port === S.port;
  if (mine) setTimeout(fillJobCard, 0);
  return [...toolHead('Backups', 'Whole-flash copies of a panel. A backup of a new panel is the only way back to its maker\'s demo.'),
    h('div', { class: 'card' },
      backups.length ? backups.map((b) => listRow('archive', b.name, `${mb(b.size)} · ${b.created || ''}${b.mac ? ' · MAC ' + b.mac : ''}`,
        [h('button', { class: 'btn small danger', type: 'button', 'data-act': 'restore', 'data-v': b.path, disabled: blocked }, 'Restore…')]))
        : h('p', { class: 'muted', text: 'No backups yet.' })),
    h('div', { class: 'row-actions' },
      h('button', { class: 'btn', type: 'button', 'data-act': 'backup-now', disabled: blocked }, 'Back up this panel now'),
      h('button', { class: 'btn ghost', type: 'button', 'data-act': 'folder', 'data-v': 'backups' }, icon('folder'), 'Open the Backups folder')),
    mine ? h('div', { class: 'card', id: 'job-card' }, h('h2', { text: j.title }),
      h('div', { class: 'overall' }, h('b', { id: 'job-pct' }), h('span', { class: 'muted', id: 'job-left' })),
      h('div', { class: 'bar big', role: 'progressbar', 'aria-label': 'Overall progress', 'aria-valuemin': 0, 'aria-valuemax': 100, id: 'job-bar' }, h('i', {})),
      h('ol', { class: 'job', id: 'job-steps' }), h('div', { id: 'job-end' })) : null];
}

function toolSoftware() {
  const b = S.state.bundle;
  const d = dev();
  const f = fw();
  const out = toolHead('Software', 'Where RoboAlarms comes from, which build goes on the panel, and the choices the guided setup makes for you.');
  if (f) out.push(fwServerCard(f));
  if (!f || f.from_file) {
    out.push(h('div', { class: 'card' },
      listRow('folder', b ? `RoboAlarms ${b.version}` : 'No firmware loaded', b ? `${b.images.length} build${b.images.length === 1 ? '' : 's'}${(b.voices || []).map((v) => `, ${v.locale} voice r${v.revision}`).join('')}${(b.radios || []).map((r) => `, Zigbee module radio ${r.version}`).join('')} · ${b.name}` : 'Open a .roboflash file',
        [h('button', { class: 'btn small', type: 'button', 'data-act': 'open-bundle' }, 'Open another…')]),
      S.state.bundle_error ? note('bad', 'triangle', S.state.bundle_error, '') : null));
  } else {
    out.push(h('div', { class: 'card' }, listRow('folder', 'A firmware file instead', 'A .roboflash file you were given: the Flasher then uses it until you go back to the firmware server.',
      [h('button', { class: 'btn small', type: 'button', 'data-act': 'open-bundle' }, 'Open a file…')]),
    S.state.bundle_error ? note('bad', 'triangle', S.state.bundle_error, '') : null));
  }
  if (d && b) {
    out.push(h('h2', { class: 'section', text: 'Build for this panel' }), boardPicker(d));
    out.push(h('h2', { class: 'section', text: 'Start over' }));
    out.push(h('div', { class: 'card' }, listRow('undo', runsRoboAlarms() ? 'Reset to factory defaults' : 'Install fresh',
      runsRoboAlarms() ? 'Erases the whole panel - codes, users, zones, Wi-Fi and history - and installs RoboAlarms again: a complete reflash.' : 'Erases the board and installs RoboAlarms.',
      [h('button', { class: 'btn small danger', type: 'button', 'data-act': 'go-reset', disabled: !!busyReason() || jobRunning() }, runsRoboAlarms() ? 'Reset…' : 'Install…')])));
  }
  return out;
}

/* The firmware server (FLS-032, FLS-033): its address, the version choice, and each board's release
 * on the channel it installs from. */
function fwServerCard(f) {
  const cat = S.state.catalogue || {};
  const offered = Object.keys(cat).filter((id) => cat[id].offered !== false);
  const rows = offered.map((id) => {
    const t = fwTarget(id, (f.chosen || {})[id]);
    return t && Object.assign({}, t, { dev_offered: (f.dev_offered || []).includes(id) });
  }).filter(Boolean);
  const when = f.checked ? new Date(f.checked * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }) : '';
  const status = f.error ? f.error.title : f.offline ? 'Can\'t be reached: what was downloaded before is used.' : when ? `Checked at ${when}.` : 'Checking…';
  return h('div', { class: 'card' },
    listRow('download', 'The firmware server', `${f.address} · ${status}`,
      f.from_file ? [h('button', { class: 'btn small primary', type: 'button', 'data-act': 'fw-server' }, 'Use the firmware server')]
        : [h('button', { class: 'btn small', type: 'button', 'data-act': 'fw-refresh' }, icon('refresh'), 'Check again')]),
    f.from_file ? note('info', 'info', 'A file opened by hand is in use.', 'Its builds go on the panels until you go back to the firmware server.') : null,
    h('div', { class: 'list-row' }, h('span', { class: 'icon-tile small plain' }, icon('sliders')),
      h('div', {}, h('label', { for: 'fw-channel' }, h('b', { text: 'Version' })),
        h('small', { text: 'Automatic takes the steadiest release there is: Stable, else Beta, else Alpha. Dev is development builds, with test zones and developer tools: the Flasher asks before using one. A choice here lasts until the Flasher closes.' })),
      h('select', { class: 'select', id: 'fw-channel', 'data-act': 'fw-channel' },
        h('option', { value: 'auto', selected: !f.pick }, 'Automatic (recommended)'),
        f.channels.map((c) => h('option', { value: c, selected: c === f.pick }, CHANNELS[c])))),
    rows.map((t) => h('div', { class: 'list-row', 'data-fw': fwKey(t) }, h('span', { class: 'icon-tile small plain' }, icon(t.state === 'failed' ? 'triangle' : t.state === 'ready' ? 'circleCheck' : 'cpu')),
      h('div', {}, h('b', { text: (cat[t.board] || {}).name + ` · ${CHANNELS[t.channel] || t.channel}` }),
        h('small', { class: 'fw-words', text: fwWords(t) + (t.state === 'failed' && t.error && t.error.advice ? ' ' + t.error.advice : '') }),
        t.state === 'downloading' ? bar(fwFraction(t)) : null),
      h('div', { class: 'row-actions' }, t.dev_offered && !f.from_file
        ? h('button', { class: 'btn small', type: 'button', 'data-act': 'fw-dev', 'data-v': t.board }, 'Use it…')
        : ['available', 'failed'].includes(t.state) && !f.from_file
          ? h('button', { class: 'btn small', type: 'button', 'data-act': 'fw-get', 'data-v': t.board }, t.state === 'failed' ? 'Try again' : 'Download now') : null))));
}

function toolLogs() {
  const j = job();
  return [...toolHead('Logs', 'What the Flasher did: every command, every write, and how it recognised each board.'),
    h('div', { class: 'card' }, listRow('folder', 'The Flasher\'s log files', (S.state.folders || {}).logs || '',
      [h('button', { class: 'btn small', type: 'button', 'data-act': 'folder', 'data-v': 'logs' }, 'Open')])),
    j ? h('div', { class: 'card' }, h('h2', { text: `Last job: ${j.title}` }), h('pre', { class: 'log', text: (j.log || []).join('\n') })) : null];
}

/* ---- rendering ----------------------------------------------------------------------------- */
let rfPage = '';     // the page last drawn (RF23)
function render(moved) {
  if (!S.state) return;
  syncProgram();
  $('mode-guided').setAttribute('aria-pressed', String(S.mode === 'guided'));
  $('mode-tools').setAttribute('aria-pressed', String(S.mode === 'tools'));
  const app = $('app');
  const old = $('page');
  const keep = old ? old.scrollTop : 0;
  const a = document.activeElement;
  const focusKey = a && a.dataset ? `${a.dataset.act}|${a.dataset.v}` : '';
  const hadInput = a && a.tagName === 'INPUT' && a.type === 'text' ? a : null;
  put(clear(app), S.mode === 'guided' ? guided() : tools());
  // RF23 (FLS-014): another page without the receiver test stops listening, as Finish does. Only
  // on a move to another page: a re-render of the same page (its card hidden a moment) doesn't.
  const where = `${S.mode}|${S.tool}|${S.step}|${S.sub}|${S.hwTab}`;
  if (where !== rfPage) {
    rfPage = where;
    if (S.rfPort && !$('rf-live')) { api('rf_stop', { port: S.rfPort }, true); S.rfPort = null; }
  }
  const page = $('page');
  if (moved) {
    if (page) page.scrollTop = 0;
    const t = $('page-title');
    if (t) t.focus({ preventScroll: true });
  } else {
    if (page) page.scrollTop = keep;
    if (hadInput && app.contains(hadInput)) hadInput.focus({ preventScroll: true });
    else if (focusKey !== 'undefined|undefined') {
      const same = [...app.querySelectorAll('[data-act]')].find((el) => `${el.dataset.act}|${el.dataset.v}` === focusKey);
      if (same) same.focus({ preventScroll: true });
    }
  }
  offerDev();
}

function go(step) {
  if (step === 7) S.zbTest = null;
  S.step = step;
  S.sub = '';
  S.error = null;
  render(true);
}
async function readParts() {
  const port = S.port;
  S.readingParts = true;
  const r = await api('parts_read', { port }, true);
  S.readingParts = false;
  if (S.port !== port) return;
  if (r.ok) {
    S.choice = Object.assign(defaultChoice(), r.parts, { zigbee: norm(S.choice).zigbee && S.choiceFor === port }); S.touched = false;
    S.customOk = !!r.custom_ok;
    S.hwTab = null;
    if (S.choice.layout === 'custom') cpSeed(S.choice);
    else { S.cp.panel = null; S.cp.cleared = false; S.cp.notes = []; }
    delete S.choice.notes;
  }
  S.choiceFor = port;
  render(false);
}

async function startInstall() {
  const d = dev();
  const img = chosenImage();
  const runs = runsRoboAlarms();
  const reset = runs && S.action === 'reset';
  S.skipParts = keepParts();
  const body = { port: S.port, image_key: img.key, confirmed: S.imageOk, parts: S.skipParts ? null : norm(S.choice) };
  if (runs && !reset) {
    body.kind = 'update';
    if (d.probe.version && older(img.version, d.probe.version)) {
      const ok = await ask({ title: `Go back to ${img.version}?`, body: `The panel runs ${d.probe.version}, which is newer. Settings made with newer firmware may not all be understood by the older one.`, ok: 'Go back' });
      if (!ok) return;
      body.downgrade_ok = true;
    }
  } else {
    body.kind = 'install';
    body.backup = S.backup;
    body.language = chosenLanguage(img);
    if (reset) {
      body.reset = true;
      body.erase_settings_ok = true;
      const ok = await ask({ title: 'Reset this panel to factory defaults?', body: ['Everything on it is erased: codes, users, zones, Wi-Fi, integrations and the event history. RoboAlarms is installed fresh and starts its first-time setup.'],
        ok: 'Erase and reset', danger: true });
      if (!ok) return;
    } else if (!S.backup) {
      const ok = await ask({ title: 'Install without a backup?', body: 'Without a backup, what is on the board now (its maker\'s demo) is gone for good.', ok: 'Install without a backup', danger: true });
      if (!ok) return;
    }
  }
  S.error = null;
  const r = await api('job', body);
  if (r.ok) {
    S.runJob = r.job.id;
    S.state.job = r.job;
    render(true);
  }
}

/* ---- events from the page -------------------------------------------------------------------- */
document.addEventListener('click', async (e) => {
  const t = e.target.closest('[data-act]');
  if (!t || t.disabled || t.getAttribute('aria-disabled') === 'true') return;
  if (t.tagName === 'INPUT' || t.tagName === 'LABEL') return;   // checkboxes: see 'change'
  const act = t.dataset.act;
  const v = t.dataset.v;
  const port = S.port;
  const b = boardOf();
  const pb = S.mode === 'tools' ? b : planBoard();
  let moved = false;
  if (act !== 'clockplace' && act !== 'zwplace') S.movedNote = {};
  switch (act) {
    case 'mode': S.mode = v; S.error = null; moved = true; if (v === 'tools' && !S.port) S.tool = 'ports'; break;
    case 'tool': S.tool = v; S.error = null; moved = true; break;
    case 'next':
      if (S.step === 5) S.action = 'update';
      go(nextStep()); return;
    case 'back': go(prevStep()); return;
    case 'goto': go(Number(v)); return;
    case 'sub': S.sub = v; S.error = null; moved = true; break;
    case 'print': window.print(); return;
    case 'fullfocus': S.fullFocus = t.tagName === 'g' && S.fullFocus === v ? '' : v; break;
    case 'checkfocus': S.checkFocus = t.tagName === 'g' && S.checkFocus === v ? '' : v; break;
    case 'port': selectPort(v); break;
    case 'probe': api('probe', { port: v || port }); return;
    case 'identify': S.error = null; api('identify', { port }); return;
    case 'pickboard': S.pickBoard = !S.pickBoard; break;
    case 'image': api('select_image', { port, key: v }); S.imageOk = false; return;
    case 'board': api('select_board', { port, board: v }); S.imageOk = false; return;
    case 'planboard':
      S.planBoard = v; setParts(S.choice, (S.state.catalogue || {})[v]);
      if (serving()) api('firmware_get', { board: v }, true);   // a head start: it downloads while the panel is wired
      break;
    case 'part': {        // what is connected: the Flasher places it (BL-117)
      const c = norm(S.choice);
      if (v === 'rf') c.rf = c.rf === 'none' ? 'cc1101' : 'none';   // the first receiver offered
      else c[v] = !c[v];
      setParts(c, pb); S.touched = true; break;
    }
    case 'rfkind': setParts(Object.assign(norm(S.choice), { rf: v }), pb); S.touched = true; break;
    case 'rfplace': {
      const c = Object.assign(norm(S.choice), { rf_place: v });
      if (v === 'j7' && c.rf === 'module' && !(((pb && pb.rf) || {}).pins || []).includes(c.rf_pin)) c.rf_pin = recommend(c, pb).rf_pin;
      S.choice = c; S.touched = true; break;
    }
    case 'pin': S.choice = Object.assign(norm(S.choice), { rf_pin: Number(v) }); S.touched = true; break;
    case 'clockplace': {  // the clock's J7 pins are the Z-Wave radio's too: it moves to the socket
      const c = Object.assign(norm(S.choice), { clock_place: v });
      if (v === 'j7' && c.zwave && c.zwave_place === 'j7' && zwClockClash(pb)) { c.zwave_place = 'socket'; S.movedNote = { zwave: 'Moved to the radio socket: the clock now uses these J7 pins.' }; }
      S.choice = c; S.touched = true; break;
    }
    case 'zwplace': {
      const c = Object.assign(norm(S.choice), { zwave_place: v });
      if (v === 'j7' && c.clock && c.clock_place === 'j7' && zwClockClash(pb)) { c.clock_place = 'j13'; S.movedNote = { clock: 'Moved to the I2C connector: the Z-Wave radio now uses its J7 pins.' }; }
      S.choice = c; S.touched = true; break;
    }
    case 'recommend': S.choice = recommend(S.choice, pb); S.touched = true; break;
    case 'action': S.action = v; S.resetOk = false; S.backup = v !== 'reset' ? S.backup : false; break;
    case 'start': startInstall(); return;
    case 'stop': {
      const j = job();
      const writing = j.steps.some((st) => st.state === 'running' && ['erase', 'write', 'verify'].includes(st.key));
      const ok = await ask({ title: 'Stop?', body: writing ? 'The board is being written. If you stop now it has no working firmware until you install again (trying again cannot harm it).' : 'Nothing has been written yet.', ok: 'Stop', danger: writing });
      if (ok) api('cancel', { id: j.id });
      return;
    }
    case 'retry': S.runJob = ''; S.radioJob = ''; S.zb.read = {}; S.state.job = null; break;
    case 'zb-read': {      // the module's own port, only after the tick: what it runs (FLS-030)
      if (!S.zb.alone || !v) return;
      S.error = null; S.zb.reading[v] = true; render(false);
      const r = await api('radio_read', { port: v, alone: true });
      S.zb.reading[v] = false;
      if (r.ok) S.zb.read[v] = r.radio;
      render(false); return;
    }
    case 'zb-install': {
      if (!S.zb.alone || !v) return;
      S.error = null;
      const r = await api('job', { kind: 'radio', port: v, alone: true, backup: S.zb.keep });
      if (r.ok) { S.radioJob = r.job.id; S.zb.done = false; S.state.job = r.job; }
      render(false); return;
    }
    case 'zb-again': S.radioJob = ''; S.zb.read = {}; S.zb.done = false; S.error = null; break;
    case 'zb-open': S.mode = 'tools'; S.tool = 'zigbee'; S.error = null; moved = true; break;
    case 'zb-ask': askZigbee(); return;
    case 'cp-open': S.mode = 'tools'; S.tool = 'hardware'; S.hwTab = 'custom'; S.error = null; moved = true; break;
    case 'parts-apply': {
      if (norm(S.choice).layout === 'custom') { S.mode = 'tools'; S.tool = 'hardware'; S.hwTab = 'custom'; moved = true; break; }
      const r = await api('parts_apply', { port, parts: norm(S.choice) });
      if (r.ok) { S.state.parts = S.state.parts || {}; S.state.parts[port] = r.parts_job; }
      break;
    }
    case 'parts-reread': S.choiceFor = ''; readParts(); break;
    case 'to-test': go(7); return;
    case 'finish': S.step = 0; S.sub = ''; S.runJob = ''; S.finished = false; S.skipParts = false; S.action = 'update'; S.resetOk = false; S.backup = true; S.choiceFor = ''; moved = true;
      S.touched = false; S.choice = defaultChoice(); S.hwTab = null; S.cp = cpFresh(); S.tick = {}; S.ready = {}; S.checks = {}; S.fullFocus = ''; S.checkFocus = ''; S.zbTest = null;
      if (S.rfPort) { api('rf_stop', { port: S.rfPort }, true); S.rfPort = null; }
      break;
    case 'listen': api('rf_listen', { port, on: v === 'on' }); return;
    case 'rf-clear': api('rf_clear', { port }); return;
    case 'capture': {   // RF21: Record waits while one is in flight on this port
      t.disabled = true;
      const r = await api('rf_capture', { port, ms: 3000 });
      if (r.ok) toast('Recording 3 seconds: press the remote now.');
      else t.disabled = rfRecording();
      return;
    }
    case 'clock-refresh': refreshClock(); return;
    case 'clock-set': {
      const ok = await ask({ title: 'Set the panel\'s clock from this computer?', body: ['The panel takes this computer\'s time, to the second, and writes it into the clock module.', 'Check this computer\'s own clock is right first.'], ok: 'Set the clock' });
      if (!ok) return;
      const r = await api('clock_set', { port });
      if (r.ok) { toast(r.reply + '.'); refreshClock(); }
      return;
    }
    case 'console-open': api('console_open', { port }); return;
    case 'console-close': api('console_close', { port }); return;
    case 'console-send': sendCommand(port); return;
    case 'quick': api('console_send', { port, text: v }); return;
    case 'console-save': { const r = await api('console_save', { port }); if (r.ok) toast('Saved ' + r.path); return; }
    case 'console-restart': {
      const ok = await ask({ title: 'Restart the panel?', body: 'Like pressing its reset button. It starts again by itself in a few seconds.', ok: 'Restart' });
      if (ok) api('console_restart', { port });
      return;
    }
    case 'show-other': S.showOther = !S.showOther; break;
    case 'backup-now': {
      const ok = await ask({ title: 'Back up the whole flash?', body: 'The board restarts into its loader, its flash is read into a file (a few minutes for 16 MB), then it starts again.', ok: 'Back up' });
      if (ok) api('job', { kind: 'backup', port });
      return;
    }
    case 'restore': {
      const bk = (S.state.backups || []).find((x) => x.path === v);
      const d = dev();
      const other = d && d.chip && bk.mac && d.chip.mac !== bk.mac;
      const ok = await ask({ title: 'Restore this backup?', body: [`${bk.name}, taken ${bk.created || 'earlier'}.`, 'Everything on the board now is replaced: firmware, settings and logs.',
        other ? `This backup came from another board (MAC ${bk.mac}); this one is ${d.chip.mac}.` : ''], ok: 'Restore', danger: true, check: other ? 'Restore it onto this board anyway' : '' });
      if (ok) api('job', { kind: 'restore', port, path: v, mac_ok: !!other });
      return;
    }
    case 'hw-tab': S.hwTab = v; if (v === 'custom') cpEnter(); else cpLeave(b); break;
    case 'cp-part': {
      const c = cpChoice();
      if (v === 'rf') c.rf = c.rf === 'none' ? 'cc1101' : 'none'; else c[v] = !c[v];
      S.choice = c; cpSync(true); break;
    }
    case 'cp-rfkind': S.choice = Object.assign(cpChoice(), { rf: v }); cpSync(true); break;
    case 'cp-clockplace': S.cp.clockPlace = v; cpSync(true); break;
    case 'cp-reset': S.cp.pins = cpBlankPins(); S.cp.resetNC = false; S.cp.clockPlace = null; S.cp.cleared = true; cpSync(true); break;
    case 'cp-undo': cpRestore(); cpSync(false); break;
    case 'cp-apply': cpApply(); return;
    case 'cp-off': {      // a part that doesn't answer on custom pins: switch it off here, then apply
      const c = norm(S.choice);
      if (v === 'rf') c.rf = 'none'; else c[v] = false;
      S.choice = c; S.mode = 'tools'; S.tool = 'hardware'; S.hwTab = 'custom'; moved = true;
      cpSync(true); toast('Switched off here. Apply it to switch it off on the panel.'); break;
    }
    case 'places-install': S.mode = 'guided'; S.action = 'update'; S.resetOk = false; S.backup = true; S.touched = true; go(6); return;
    case 'go-reset': S.mode = 'guided'; S.action = runsRoboAlarms() ? 'reset' : 'update'; S.resetOk = false; S.backup = !runsRoboAlarms(); S.touched = false; go(6); return;
    case 'open-bundle': $('bundle-file').click(); return;
    case 'fw-refresh': api('firmware_refresh'); toast('Checking the firmware server…'); return;
    case 'fw-get': api('firmware_get', { board: v }); return;
    case 'fw-dev': pickVersion(v, 'dev'); return;
    case 'fw-server': api('firmware_use_server'); return;
    case 'folder': api('open_folder', { which: v }); return;
    default: return;
  }
  e.preventDefault();
  render(moved);
});
document.addEventListener('change', (e) => {
  const t = e.target;
  const act = t.dataset && t.dataset.act;
  if (act === 'backup') S.backup = t.checked;
  else if (act === 'zb-alone') S.zb.alone = t.checked;
  else if (act === 'zb-keep') S.zb.keep = t.checked;
  else if (act === 'language') S.language = t.value;
  else if (act === 'fw-channel') { pickVersion('', t.value, true); return; }
  else if (act === 'fw-version') { pickVersion(t.dataset.board, t.value); return; }
  else if (act === 'reset-ok') S.resetOk = t.checked;
  else if (act === 'image-ok') S.imageOk = t.checked;
  else if (act === 'show-quiet') { S.showQuiet = t.checked; if (S.port) logFor(S.port); return; }
  else if (act === 'tick') S.tick[t.dataset.v] = t.checked;
  else if (act === 'ready') S.ready[t.dataset.v] = t.checked;
  else if (act === 'cp-tick') S.cp.ticks[t.dataset.v] = t.checked;
  else if (act === 'cp-pin') {
    const [mod, k] = t.dataset.v.split('.');
    if (mod === 'zwave' && k === 'reset') { S.cp.resetNC = t.value === 'nc'; S.cp.pins.zwave.reset = null; if (t.value !== 'nc' && t.value !== '') S.cp.pins.zwave.reset = Number(t.value); }
    else S.cp.pins[mod][k] = t.value === '' ? null : Number(t.value);
    cpSync(true);
  }
  else if (act === 'check') S.checks[t.dataset.v] = t.checked;
  else return;
  render(false);
});
document.addEventListener('toggle', (e) => {
  const d = e.target;
  if (d.dataset && d.dataset.key) S.open[d.dataset.key] = d.open;
}, true);
document.addEventListener('keydown', (e) => {
  const t = e.target.closest && e.target.closest('svg g[role=button]');
  if (t && (e.key === 'Enter' || e.key === ' ')) { e.preventDefault(); t.dispatchEvent(new MouseEvent('click', { bubbles: true })); }
});
function showTip(e) {
  const box = e.target.closest && e.target.closest('.wiring-block');
  if (!box) return;
  const cap = box.querySelector('.pin-caption');
  if (!cap) return;   // a drawing without a caption line (the Zigbee module's)
  const g = e.target.closest('g.pin');
  cap.textContent = g ? g.getAttribute('data-tip') : cap.dataset.default;
}
document.addEventListener('mouseover', showTip);
document.addEventListener('focusin', showTip);

setInterval(() => {   // "last heard" words: once a second at most
  for (const td of document.querySelectorAll('[data-ago]')) td.textContent = ago(Number(td.dataset.ago));
}, 1000);

/* The guided run follows the job it started: to "Test and finish" once it and the parts are done. */
setInterval(() => {
  if (!S.state || S.mode !== 'guided' || S.step !== 6 || !S.runJob) return;
  const j = job();
  const pj = partsRun();
  if (!j || j.id !== S.runJob || j.state !== 'done') return;
  const partsPending = !S.skipParts && (!pj || pj.state === 'running');
  if (!partsPending && (!pj || pj.state !== 'failed')) { S.finished = true; go(7); }
}, 700);

/* ---- theme, firmware file, drag and drop --------------------------------------------------- */
function syncTheme() {
  const dark = document.documentElement.dataset.theme === 'dark';
  const btn = $('theme-toggle');
  btn.setAttribute('aria-label', `Switch to ${dark ? 'light' : 'dark'} theme`);
  btn.title = btn.getAttribute('aria-label');
  clear(btn).append(icon('sun'));
}
$('flasher-update').addEventListener('click', () => api('open_flasher_download'));
$('theme-toggle').addEventListener('click', () => {
  const next = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
  document.documentElement.dataset.theme = next;
  try { localStorage.setItem('roboalarms-flasher-theme', next); } catch (e) { /* the preference is optional */ }
  syncTheme();
});
syncTheme();

async function uploadBundle(file) {
  if (!file) return;
  const r = await fetch('/api/bundle', { method: 'POST',
    headers: { 'X-Flasher-Token': TOKEN, 'X-Filename': file.name, 'Content-Type': 'application/octet-stream' },
    body: await file.arrayBuffer() });
  const j = await r.json();
  if (j.ok) toast('Firmware loaded: ' + file.name);
  else showError(j.error || { title: 'That file could not be opened.' });
}
$('bundle-file').addEventListener('change', (e) => { uploadBundle(e.target.files[0]); e.target.value = ''; });
let dragDepth = 0;
window.addEventListener('dragenter', (e) => { e.preventDefault(); dragDepth++; $('drop').hidden = false; });
window.addEventListener('dragleave', () => { if (--dragDepth <= 0) { dragDepth = 0; $('drop').hidden = true; } });
window.addEventListener('dragover', (e) => e.preventDefault());
window.addEventListener('drop', (e) => {
  e.preventDefault();
  dragDepth = 0;
  $('drop').hidden = true;
  const f = e.dataTransfer && e.dataTransfer.files[0];
  if (f) uploadBundle(f);
});

if (!TOKEN) {
  document.body.textContent = 'Start the RoboAlarms Flasher from its program, not from a bookmark.';
} else {
  loadState().then(streamEvents);
}
