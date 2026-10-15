'use strict';
// Panel Manager core: talking to the panel, navigation, forms and dialogs.
// Pages are in panel-pages.js, the remote keypad in panel-keypad.js. No framework, CDN,
// font or telemetry: every request goes back to the panel that served this file, and the
// panel checks every permission again. Nothing on the page flashes or animates.
// Words go through panel-words.js (docs/features/37): t() for text, ht() inside HTML, and tables
// of words are functions, so they are read in the language that is loaded.

// ---------------------------------------------------------------------------------------
// Shared state. panel-keypad.js reads these names; keep them.
let state = null; // GET /api/v1/status for the signed-in session
let csrf = '';
let page = 'overview'; // id of the page on screen
let route = {id: 'overview', arg: ''};
let renderId = 0; // bumps on every navigation; late answers for an old page are dropped


const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => Array.from(root.querySelectorAll(selector));

function esc(value) {
  return String(value ?? '').replace(/[&<>"']/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'})[c]);
}
function titleCase(value) {
  return String(value).replaceAll('_', ' ').replace(/^\w/, c => c.toUpperCase());
}

// ---------------------------------------------------------------------------------------
// Talking to the panel

// What the alarm engine and ESP-IDF answer, in words a person can act on.
const ERRORS = {
  invalid_code: tx('That code is not right.'),
  locked_out: tx('Too many wrong codes. The keypad is locked for a few minutes.'),
  not_authorized: tx('Your code does not allow this change.'),
  code_required: tx('Enter your code.'),
  bad_state: tx('The panel must be disarmed and idle, with no alarm sounding.'),
  not_ready: tx('The panel is not ready.'),
  code_in_use: tx('Another user already has that code.'),
  bad_arg: tx('The panel did not accept one of the values.'),
  disabled: tx('This is turned off on the panel.'),
  unavailable: tx('The panel could not check codes just now. Try again in a moment.'),
  locked: tx('The panel is armed or an alarm is sounding. Changes wait until everything is disarmed and quiet.'),
  password_required: tx('Enter your password.'),
  wrong_password: tx('That password isn’t right.'),
  password_sign_in_required: tx('This needs your own password sign-in, not a workstation’s.'),
  // Software updates (OTA-021, OTA-026..OTA-030)
  busy: tx('Not while an update runs.'),
  update_running: tx('Not while an update runs.'),
  stale: tx('The panel’s check of that server is out of date. Check it again.'),
  no_update: tx('It isn’t running any more.'),
  not_stoppable: tx('This step can’t be stopped. It finishes in a moment.'),
  bad_domain: tx('Type a domain, like roboalarms.com.'),
  factory_code: tx('Change the installer code first: the factory one is public.'),
  not_allowed: tx('Your sign-in doesn’t allow this. At the panel it needs another code too.'),
  backup_password: tx('Choose a backup password of 8 to 64 characters (letters, digits, spaces, punctuation).'),
  backup_failed: tx('The panel could not make the backup just now. Try again in a moment.'),
  backup_format: tx('That isn’t a backup file from a panel, or it was cut short.'),
  backup_decrypt: tx('The password doesn’t open this backup, or the file was changed.'),
  backup_board: tx('That backup is from another kind of panel.'),
  backup_content: tx('The panel refused something in that backup (it may be from a newer firmware or damaged). Nothing was changed.'),
  backup_partial: tx('The restore stopped part way. The panel restarts; restore the same file again.'),
  ESP_ERR_INVALID_STATE: tx('The panel is busy or not in the right state for this.'),
  ESP_ERR_INVALID_ARG: tx('The panel did not accept one of the values.'),
  ESP_ERR_NOT_SUPPORTED: tx('This panel does not support that.'),
  ESP_ERR_NOT_FOUND: tx('Nothing to do: no update or medium was found.'),
  ESP_ERR_NO_MEM: tx('The panel is short of memory. Try again in a moment.'),
};
function friendly(message) {
  return ERRORS[message] ? t(ERRORS[message]) : message;
}

// A request that never reached the panel (no answer, or stopped inside the browser).
function unreachable(error) {
  if (error?.name === 'TimeoutError') return new Error(t('The panel did not answer in time. Try again.'));
  if (error?.name === 'AbortError') return error;
  // Browsers say only "Failed to fetch" / "Load failed" / "NetworkError": say what to check.
  return new Error(t('The request did not reach the panel. Check the connection; if it keeps happening on one page, a content or tracker blocker in this browser may be stopping it: allow this panel’s address in the blocker.'));
}

/* Browser passwords are 12 to 64 characters (web_login.h); keypad codes are 4 to 10 digits. */
const PASSWORD_MIN = 12;
const notACode = () => t('Keypad codes don’t sign in here. Use your browser password (at least 12 characters). The installer or a master turns browser sign-in on for you: Settings > Users > your name.');

async function api(path, data, options = {}) {
  const post = data !== undefined;
  let response;
  try {
    response = await fetch('/api/v1/' + path, {
      method: post ? 'POST' : 'GET',
      // A language other than English: the panel words what it sends (events, check results) in it.
      headers: {...(words.entries ? {'X-Panel-Language': words.locale} : {}),
        ...(post ? {'Content-Type': 'application/json', 'X-Panel-CSRF': csrf} : {})},
      body: post ? JSON.stringify(data) : undefined,
      credentials: 'same-origin',
      cache: 'no-store',
      signal: AbortSignal.timeout(20000),
      ...options,
    });
  } catch (error) {
    throw unreachable(error);
  }
  if (!response.ok) {
    let detail;
    try { detail = await response.json(); } catch { /* not JSON */ }
    if (response.status === 401 && path !== 'login') {
      state = null;
      // The page lost the panel a moment ago: the restart forgot every sign-in, and it says so (BL-188, OTA-029).
      const restart = restartSaved();
      if (restart && restart.lost !== 'others' && restart.version) location.hash = '#/updates';
      loginPage(restart ? restartWords(restart) : t('Your session ended. Sign in again.'), !!restart);
    }
    const error = new Error(friendly(detail?.error) || t('The panel answered {0}.', response.status));
    error.status = response.status;
    error.code = detail?.error;
    error.detail = detail; // the rest of the answer (a refused arming names its zone)
    throw error;
  }
  return response.json();
}

async function refreshStatus() {
  state = await api('status');
  state.reauthUntil = Date.now() + (state.reauth_s || 0) * 1000;
  csrf = state.csrf;
  if (typeof trStale === 'function' && trStale()) await trLoad(); // the troubles list, when its fingerprint moved
  updateChrome();
  return state;
}

// The page lost the panel while an update ran (BL-188, OTA-029): what it saw is kept for 10 minutes, in this tab,
// so the sign-in the restart makes it ask for can say why, and Software updates can say how the update went.
function restartLost(lost, version) {
  sessionStorage.setItem('panel-restart', JSON.stringify({version: version || '', lost, at: Date.now()}));
}
function restartSaved() {
  let saved = null;
  try { saved = JSON.parse(sessionStorage.getItem('panel-restart') || 'null'); } catch { /* not ours */ }
  return saved && Date.now() - saved.at < 600000 ? saved : null;
}
const restartWords = saved => saved.lost === 'others' ? t('The panel restarted to install an update. Sign in again.')
  : saved.lost === 'self-test' ? t('The panel restarted during its self-test. Sign in to see which version it runs.')
  : t('The panel restarted during the update to version {0}. Sign in to see where it stands.', saved.version);
// The header while the panel doesn't answer: Restarting while it restarts or may be going back, Not answering at any other step.
function connectionShows(lost) {
  const c = $('#connection');
  if (!c) return;
  c.innerHTML = `<span>${lost === 'other' ? ht('Not answering') : lost ? ht('Restarting') : ht('Connected')}</span>`;
  c.classList.toggle('offline', !!lost);
}
// Pages other than Software updates: while the status says an update is running and the panel stops answering,
// the header says Restarting, and the sign-in after it says the panel restarted (OTA-029).
const watching = {on: false, misses: 0, first: 0};
function watchRestart() {
  if (watching.on) return;
  watching.on = true;
  const tick = async () => {
    let wait = 5000;
    if (state && page !== 'updates' && !document.hidden && !$('#dialog')?.open) {
      try {
        const s = await api('status', undefined, {signal: AbortSignal.timeout(4000)});
        if (state) state.updating = !!s.updating;
        if (watching.misses) { watching.misses = 0; connectionShows(''); sessionStorage.removeItem('panel-restart'); }
      } catch (error) {
        if (!error.status && state?.updating) {
          if (!watching.misses++) watching.first = Date.now();
          else if (Date.now() - watching.first >= 2000) { restartLost('others', ''); connectionShows('restart'); }
        }
      }
      if (state?.updating) wait = 1000;
    }
    setTimeout(tick, wait);
  };
  watching.tick = tick;
  setTimeout(tick, 5000);
}

// Repeats fn every ms while the same page is on screen (skipped while the tab is hidden).
function poll(fn, ms) {
  const id = renderId;
  const tick = async () => {
    if (id !== renderId || !state) return;
    if (!document.hidden) {
      try { await fn(); } catch { /* the next tick tries again */ }
    }
    if (id === renderId) setTimeout(tick, ms);
  };
  setTimeout(tick, ms);
}

// ---------------------------------------------------------------------------------------
// Formatting

// A number with decimals the way the chosen language writes it (the browser's way for English).
const fmtNumber = (n, digits = 0) => Number(n).toLocaleString(wordsLocale(),
  {minimumFractionDigits: digits, maximumFractionDigits: digits});
function fmtDuration(seconds) {
  seconds = Math.round(Number(seconds) || 0);
  if (seconds < 60) return tn('{0} s', '{0} s', seconds);
  const m = Math.floor(seconds / 60), h = Math.floor(m / 60), d = Math.floor(h / 24);
  if (d) return t('{0} d {1} h', d, h % 24);
  if (h) return m % 60 ? tc('duration', '{0} h {1} min', h, m % 60) : tn('{0} h', '{0} h', h);
  return seconds % 60 && m < 5 ? t('{0} min {1} s', m, seconds % 60) : tn('{0} min', '{0} min', m);
}
function fmtBytes(n) {
  n = Number(n) || 0;
  if (n >= 1073741824) return t('{0} GB', fmtNumber(n / 1073741824, 1));
  if (n >= 1048576) return t('{0} MB', fmtNumber(n / 1048576, n >= 104857600 ? 0 : 1));
  if (n >= 1024) return t('{0} KB', fmtNumber(n / 1024));
  return tn('{0} byte', '{0} bytes', n);
}
function fmtAgo(ms) {
  const s = Math.max(0, Math.round((Date.now() - ms) / 1000));
  if (s < 45) return t('just now');
  if (s < 3600) return tn('{0} min ago', '{0} min ago', Math.round(s / 60));
  if (s < 86400) return t('{0} h ago', Math.round(s / 3600));
  return new Date(ms).toLocaleDateString(wordsLocale(), {month: 'short', day: 'numeric'});
}
function fmtWhen(event) {
  if (event.epoch_ms > 0) {
    const date = new Date(event.epoch_ms);
    return {short: fmtAgo(event.epoch_ms), long: date.toLocaleString(wordsLocale())};
  }
  const text = t('{0} after start-up', fmtDuration(event.uptime_s));
  return {short: text, long: t('Clock not set yet. Start-up {0}, {1}.', event.boot, text)};
}
function signalWords(rssi) {
  if (!rssi) return t('No signal');
  if (rssi >= -55) return tc('Wi-Fi signal', 'Excellent');
  if (rssi >= -67) return tc('Wi-Fi signal', 'Good');
  if (rssi >= -75) return tc('Wi-Fi signal', 'Fair');
  return tc('Wi-Fi signal', 'Weak');
}

// ---------------------------------------------------------------------------------------
// Pieces of markup

const ICONS = {
  home: '<path d="M3 10.5 12 3l9 7.5"/><path d="M5 9.5V21h14V9.5"/>',
  monitor: '<rect x="3" y="4" width="18" height="12" rx="2"/><path d="M8 20h8M12 16v4"/>',
  activity: '<path d="M22 12h-4l-3 9L9 3l-3 9H2"/>',
  users: '<circle cx="9" cy="8" r="4"/><path d="M2 21a7 7 0 0 1 14 0M16 3.5a4 4 0 0 1 0 8M22 21a7 7 0 0 0-4-6.3"/>',
  plug: '<path d="M9 2v6M15 2v6M6 8h12v4a6 6 0 0 1-12 0zM12 18v4"/>',
  sun: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>',
  wifi: '<path d="M2 8.5a15 15 0 0 1 20 0M5 12a10 10 0 0 1 14 0M8.5 15.5a5 5 0 0 1 7 0"/><circle cx="12" cy="19" r="1"/>',
  clock: '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>',
  drive: '<rect x="3" y="13" width="18" height="8" rx="2"/><path d="M5 13 7.5 4h9L19 13M7 17h.01"/>',
  grid: '<rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/><rect x="14" y="14" width="7" height="7" rx="1"/>',
  radio: '<circle cx="12" cy="12" r="2"/><path d="M8.5 8.5a5 5 0 0 0 0 7M15.5 8.5a5 5 0 0 1 0 7M5.6 5.6a9 9 0 0 0 0 12.8M18.4 5.6a9 9 0 0 1 0 12.8"/>',
  layers: '<path d="m12 3 9 5-9 5-9-5 9-5Z"/><path d="m3 13 9 5 9-5"/>',
  sliders: '<path d="M4 6h10M18 6h2M4 12h4M12 12h8M4 18h12"/><circle cx="16" cy="6" r="2"/><circle cx="10" cy="12" r="2"/><circle cx="18" cy="18" r="2"/>',
  key: '<circle cx="7.5" cy="15.5" r="4.5"/><path d="m10.7 12.3 9.3-9.3M17 6l3 3M14 9l2 2"/>',
  heart: '<path d="M20.8 5.6a5 5 0 0 0-7.1 0L12 7.3l-1.7-1.7a5 5 0 0 0-7.1 7.1L12 21.5l8.8-8.8a5 5 0 0 0 0-7.1Z"/>',
  download: '<path d="M12 3v12M7 10l5 5 5-5M4 21h16"/>',
  code: '<path d="m8 7-5 5 5 5M16 7l5 5-5 5"/>',
  lock: '<rect x="4" y="11" width="16" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/>',
  menu: '<path d="M4 6h16M4 12h16M4 18h16"/>',
  alert: '<path d="M12 3 2 20h20L12 3Z"/><path d="M12 10v4M12 17h.01"/>',
  info: '<circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8h.01"/>',
  check: '<circle cx="12" cy="12" r="9"/><path d="m8 12 3 3 5-6"/>',
  back: '<path d="m15 18-6-6 6-6"/>',
  shield: '<path d="M12 3 4 6v6c0 5 3.5 8 8 9 4.5-1 8-4 8-9V6l-8-3Z"/>',
  bell: '<path d="M6 8a6 6 0 0 1 12 0c0 7 3 8 3 8H3s3-1 3-8M10 20a2 2 0 0 0 4 0"/>',
  // The Overview's, Zones' and Troubles' pictures (docs/features/38 "The Overview mockup")
  away: '<path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4"/><path d="m16 17 5-5-5-5M21 12H9"/>',
  moon: '<path d="M12 3a6 6 0 0 0 9 9 9 9 0 1 1-9-9Z"/>',
  house: '<path d="M15 21v-8a1 1 0 0 0-1-1h-4a1 1 0 0 0-1 1v8"/><path d="M3 10a2 2 0 0 1 .709-1.528l7-5.999a2 2 0 0 1 2.582 0l7 5.999A2 2 0 0 1 21 10v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>',
  shieldCheck: '<path d="M12 3 4 6v6c0 5 3.5 8 8 9 4.5-1 8-4 8-9V6l-8-3Z"/><path d="m9 12 2 2 4-4"/>',
  shieldAlert: '<path d="M12 3 4 6v6c0 5 3.5 8 8 9 4.5-1 8-4 8-9V6l-8-3Z"/><path d="M12 8v4M12 16h.01"/>',
  lockOpen: '<rect x="4" y="11" width="16" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 7.9-1"/>',
  bypass: '<circle cx="12" cy="12" r="9"/><path d="m5.7 5.7 12.6 12.6"/>',
  siren: '<path d="M7 18v-6a5 5 0 0 1 10 0v6"/><path d="M5 21h14v-3H5zM12 3v1M4.2 6.2l.7.7M19.8 6.2l-.7.7M2 12h1M21 12h1"/>',
  chev: '<path d="m9 18 6-6-6-6"/>',
  cloudOff: '<path d="m2 2 20 20"/><path d="M5.782 5.782A7 7 0 0 0 9 19h8.5a4.5 4.5 0 0 0 1.307-.193"/><path d="M21.532 16.5A4.5 4.5 0 0 0 17.5 10h-1.79A7.008 7.008 0 0 0 10 5.07"/>',
  cpu: '<rect width="16" height="16" x="4" y="4" rx="2"/><rect width="6" height="6" x="9" y="9" rx="1"/><path d="M15 2v2M15 20v2M2 15h2M2 9h2M20 15h2M20 9h2M9 2v2M9 20v2"/>',
  bellOff: '<path d="M10.268 21a2 2 0 0 0 3.464 0"/><path d="M17 17H4a1 1 0 0 1-.74-1.673C4.59 13.956 6 12.499 6 8a6 6 0 0 1 .258-1.742"/><path d="m2 2 20 20"/><path d="M8.668 3.01A6 6 0 0 1 18 8c0 2.687.77 4.653 1.707 6.05"/>',
  doorOpen: '<path d="M11 20H2"/><path d="M11 4.562v16.157a1 1 0 0 0 1.242.97L19 20V5.562a2 2 0 0 0-1.515-1.94l-4-1A2 2 0 0 0 11 4.561z"/><path d="M11 4H8a2 2 0 0 0-2 2v14"/><path d="M14 12h.01"/><path d="M22 20h-3"/>',
  door: '<path d="M6 21V4a1 1 0 0 1 1-1h10a1 1 0 0 1 1 1v17M3 21h18M14 12h.01"/>',
  window: '<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M3 10h18M12 4v16"/>',
  motion: '<circle cx="13" cy="4" r="2"/><path d="m7 21 3-6 3 2v5M10 15l1-6 4 3 3-1M11 9 7 10l-1 3"/>',
  smoke: '<path d="M4 5h16l-2 4H6z"/><path d="M8 13v2M12 13v5M16 13v2"/>',
  co: '<path d="M17.5 19H7a5 5 0 1 1 .9-9.9A6 6 0 0 1 19 11a4 4 0 0 1-1.5 8Z"/>',
  water: '<path d="M12 3s6 6.5 6 11a6 6 0 0 1-12 0c0-4.5 6-11 6-11Z"/>',
  glass: '<rect x="4" y="3" width="16" height="18" rx="1"/><path d="m9 3 2 6-3 3 4 9M20 9l-6 2"/>',
  garage: '<path d="M3 21V9l9-6 9 6v12M7 21v-9h10v9M7 15h10M7 18h10"/>',
  battery: '<rect x="2" y="7" width="17" height="10" rx="2"/><path d="M22 11v2M7 10v4"/>',
  zap: '<path d="M13 2 4 14h7l-1 8 9-12h-7z"/>',
  // The status bar's own battery pictures (Lucide battery-full / -medium / -low / -charging), and a plug
  batFull: '<rect x="2" y="7" width="16" height="10" rx="2"/><path d="M22 11v2M6 11v2M10 11v2M14 11v2"/>',
  batMedium: '<rect x="2" y="7" width="16" height="10" rx="2"/><path d="M22 11v2M6 11v2M10 11v2"/>',
  batLow: '<rect x="2" y="7" width="16" height="10" rx="2"/><path d="M22 11v2M6 11v2"/>',
  batCharging: '<path d="M15 7h1a2 2 0 0 1 2 2v6a2 2 0 0 1-2 2h-2M6 7H4a2 2 0 0 0-2 2v6a2 2 0 0 0 2 2h1M22 11v2"/><path d="m11 7-3 5h4l-3 5"/>',
  plugOff: '<path d="M9 2v6M15 2v4M6 8h6M18 12a6 6 0 0 1-8.5 5.4M6 12V8M12 18v4M2 2l20 20"/>',
  wifiOff: '<path d="M2 8.5a15 15 0 0 1 4.2-2.8M9.8 4.6A15 15 0 0 1 22 8.5M5 12a10 10 0 0 1 4.1-2.4M15.3 10A10 10 0 0 1 19 12M8.5 15.5a5 5 0 0 1 7 0M2 2l20 20"/><circle cx="12" cy="19" r="1"/>',
  // The walk test's pictures (docs/features/40; Lucide, ISC licence)
  walk: '<path d="M4 16v-2.38C4 11.5 2.97 10.5 3 8c.03-2.72 1.49-6 4.5-6C9.37 2 10 3.8 10 5.5c0 3.11-2 5.66-2 8.68V16a2 2 0 1 1-4 0Z"/><path d="M20 20v-2.38c0-2.12 1.03-3.12 1-5.62-.03-2.72-1.49-6-4.5-6C14.63 6 14 7.8 14 9.5c0 3.11 2 5.66 2 8.68V20a2 2 0 1 0 4 0Z"/><path d="M16 17h4"/><path d="M4 13h4"/>',
  volume: '<path d="M11 4.7a.7.7 0 0 0-1.2-.5L6.4 7.6a1.4 1.4 0 0 1-1 .4H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.4a1.4 1.4 0 0 1 1 .4l3.4 3.4a.7.7 0 0 0 1.2-.5z"/><path d="M16 9a5 5 0 0 1 0 6"/><path d="M19.4 18.4a9 9 0 0 0 0-12.8"/>',
  vibrate: '<path d="m2 8 2 2-2 2 2 2-2 2"/><path d="m22 8-2 2 2 2-2 2 2 2"/><rect width="8" height="14" x="8" y="5" rx="1"/>',
  speech: '<path d="M8.8 20v-4.1l1.9.2a2.3 2.3 0 0 0 2.2-2.1V8.3A5.4 5.4 0 0 0 2 8.3c0 2.8.7 3 1 4.5a5.8 5.8 0 0 1 0 2.8L2 20"/><path d="M19.8 17.8a7.5 7.5 0 0 0 0-10.6"/><path d="M17 15a3.5 3.5 0 0 0 0-5"/>',
  phone: '<rect width="14" height="20" x="5" y="2" rx="2"/><path d="M12 18h.01"/>',
  plus: '<path d="M5 12h14"/><path d="M12 5v14"/>',
  // Automations (docs/features/41): Lucide's lightbulb, toggle-right, droplet, thermometer, sparkles, play
  bulb: '<path d="M15 14c.2-1 .7-1.7 1.5-2.5 1-.9 1.5-2.2 1.5-3.5A6 6 0 0 0 6 8c0 1 .2 2.2 1.5 3.5.7.7 1.3 1.5 1.5 2.5"/><path d="M9 18h6M10 22h4"/>',
  toggle: '<rect x="2" y="6" width="20" height="12" rx="6"/><circle cx="16" cy="12" r="2"/>',
  drops: '<path d="M12 22a7 7 0 0 0 7-7c0-2-1-3.9-3-5.5S12.5 5 12 2c-.5 3-2 4.9-4 6.5S5 13 5 15a7 7 0 0 0 7 7z"/>',
  thermo: '<path d="M14 4v10.5a4 4 0 1 1-4 0V4a2 2 0 0 1 4 0Z"/>',
  sparkles: '<path d="m12 3 1.9 5.8L20 10l-6.1 1.2L12 17l-1.9-5.8L4 10l6.1-1.2z"/><path d="M19 17v4M17 19h4"/>',
  play: '<path d="M6 3l14 9-14 9z"/>',
  stop: '<rect width="14" height="14" x="5" y="5" rx="2"/>',
  untested: '<path d="M10.1 2.2a10 10 0 0 1 3.8 0"/><path d="M13.9 21.8a10 10 0 0 1-3.8 0"/><path d="M17.6 3.7a10 10 0 0 1 2.7 2.7"/><path d="M2.2 13.9a10 10 0 0 1 0-3.8"/><path d="M20.3 17.6a10 10 0 0 1-2.7 2.7"/><path d="M21.8 10.1a10 10 0 0 1 0 3.8"/><path d="M3.7 6.4a10 10 0 0 1 2.7-2.7"/><path d="M6.4 20.3a10 10 0 0 1-2.7-2.7"/>',
  report: '<path d="M15 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V7Z"/><path d="M14 2v4a2 2 0 0 0 2 2h4"/><path d="M12 18v-6"/><path d="m9 15 3 3 3-3"/>',
  again: '<path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"/><path d="M3 3v5h5"/>',
  keyfob: '<rect x="7" y="2" width="10" height="20" rx="5"/><circle cx="12" cy="8.5" r="1.6"/><circle cx="12" cy="14.5" r="1.6"/>',
  // The eye on password boxes and the people pages' user (BL-136; Lucide, ISC licence)
  eye: '<path d="M2.06 12.35a1 1 0 0 1 0-.7 10.75 10.75 0 0 1 19.88 0 1 1 0 0 1 0 .7 10.75 10.75 0 0 1-19.88 0"/><circle cx="12" cy="12" r="3"/>',
  'eye-off': '<path d="M10.73 5.08a10.74 10.74 0 0 1 11.2 6.57 1 1 0 0 1 0 .7 10.75 10.75 0 0 1-1.44 2.49"/><path d="M14.08 14.16a3 3 0 0 1-4.24-4.24"/><path d="M17.48 17.5a10.75 10.75 0 0 1-15.42-5.15 1 1 0 0 1 0-.7 10.75 10.75 0 0 1 4.45-5.14"/><path d="m2 2 20 20"/>',
  user: '<circle cx="12" cy="8" r="5"/><path d="M20 21a8 8 0 0 0-16 0"/>',
};
function icon(name, cls = '') {
  return `<svg class="icon ${cls}" viewBox="0 0 24 24" aria-hidden="true">${ICONS[name] || ''}</svg>`;
}
// The eye (BL-136): every password box the Panel Manager draws can show what was typed, until pressed again.
// A box hides again when its form is sent, so the browser can still offer to save the password.
function eyes(root) {
  for (const input of root.querySelectorAll('input[type=password]:not([data-eye])')) {
    input.dataset.eye = '1';
    const wrap = document.createElement('span');
    wrap.className = 'pc-secret';
    input.before(wrap);
    wrap.append(input);
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'pc-eye';
    if (input.id) button.setAttribute('aria-controls', input.id);
    eyeLook(button, input, false);
    button.onclick = () => {
      const shown = input.type === 'password';
      input.type = shown ? 'text' : 'password';
      eyeLook(button, input, shown);
      input.focus();
    };
    wrap.append(button);
  }
}
function eyeLook(button, input, shown) {
  const code = input.inputMode === 'numeric';
  const label = shown ? (code ? t('Hide the code') : t('Hide the password')) : (code ? t('Show the code') : t('Show the password'));
  button.innerHTML = icon(shown ? 'eye-off' : 'eye');
  button.setAttribute('aria-label', label);
  button.setAttribute('aria-pressed', String(shown));
  button.title = label;
}
function hideSecrets(root) {
  for (const input of root.querySelectorAll?.('input[data-eye]') || []) {
    if (input.type !== 'text') continue;
    input.type = 'password';
    const button = input.parentElement.querySelector('.pc-eye');
    if (button) eyeLook(button, input, false);
  }
}
document.addEventListener('submit', event => hideSecrets(event.target), true);
function pageHead(title, description = '', actions = '', back = null) {
  return `${back ? `<a class="back" href="#/${esc(back[0])}">${icon('back')}${esc(back[1])}</a>` : ''}
    <div class="page-head"><div><h1>${esc(title)}</h1>${description ? `<p>${esc(description)}</p>` : ''}</div>
    ${actions ? `<div class="actions">${actions}</div>` : ''}</div>`;
}
function notice(kind, title, text = '', extra = '') {
  const glyph = kind === 'good' ? 'check' : kind === 'info' ? 'info' : 'alert';
  return `<div class="notice ${kind}" role="${kind === 'bad' ? 'alert' : 'status'}">${icon(glyph)}
    <div><strong>${esc(title)}</strong>${text ? `<p>${esc(text)}</p>` : ''}${extra}</div></div>`;
}
function tag(text, kind = '') {
  return `<span class="tag ${kind}">${esc(text)}</span>`;
}
function kv(pairs) {
  return `<dl class="kv">${pairs.filter(p => p && p[1] !== undefined && p[1] !== '')
    .map(([k, v]) => `<dt>${esc(k)}</dt><dd>${esc(v)}</dd>`).join('')}</dl>`;
}

// ---------------------------------------------------------------------------------------
// Forms. A field spec says how a value is shown and edited:
//   {name, label, help, type, unit, scale, min, max, maxLength, options, showIf, placeholder, locked}
// type: switch | text | secret | number | range | select | bits | textarea
// scale: panel value = shown value × scale (seconds shown for milliseconds: scale 1000).
// options: [[value, label, help?], ...] for select and bits.
// showIf(values): the row is shown only while it returns true.
// locked: shown but not editable (someone else's to change); it is never sent, as it can't change.

let fieldSerial = 0;
function fieldRow(spec, value, data = {}) {
  const id = `f${++fieldSerial}-${spec.name}`;
  const label = `<label class="label" for="${id}">${esc(spec.label)}</label>`;
  const helpText = spec.describe ? spec.describe(value) : spec.help;
  const help = helpText || spec.describe ? `<span class="help">${esc(helpText)}</span>` : '';
  const shown = spec.scale ? Number(value) / spec.scale : value;
  const unit = spec.unit ? `<span class="unit">${esc(spec.unit)}</span>` : spec.type === 'number' ? '<span class="unit"></span>' : '';
  const attrs = `id="${id}" name="${esc(spec.name)}"${spec.required ? ' required' : ''}${spec.locked ? ' disabled' : ''}`;
  let control;
  switch (spec.type) {
    case 'switch':
      control = `<input type="checkbox" class="switch" role="switch" ${attrs}${value ? ' checked' : ''}>`;
      break;
    case 'select': {
      const options = [...spec.options];
      if (!options.some(o => String(o[0]) === String(value)) && value !== undefined && value !== '')
        options.push([value, spec.unknown ? spec.unknown(value) : t('Current ({0})', value)]);
      control = `<select ${attrs}>${options.map(([v, l]) =>
        `<option value="${esc(v)}"${String(v) === String(value) ? ' selected' : ''}>${esc(l)}</option>`).join('')}</select>`;
      break;
    }
    case 'bits':
      control = `<div class="checks" role="group" aria-labelledby="${id}-l">${spec.options.map(([bit, l]) =>
        `<label><input type="checkbox" data-bit="${bit}"${(Number(value) & bit) ? ' checked' : ''}>${esc(l)}</label>`).join('')}</div>`;
      return `<div class="row${spec.wide ? ' wide' : ''}" data-field="${esc(spec.name)}"><div><span class="label" id="${id}-l">${esc(spec.label)}</span>${help}</div>
        <div class="control" data-bits="${esc(spec.name)}">${control}</div></div>`;
    case 'range':
      control = `<input type="range" ${attrs} min="${spec.min}" max="${spec.max}" step="${spec.step || 1}" value="${esc(shown)}">
        <output class="range-value" for="${id}">${esc(shown)}${esc(spec.unit || '')}</output>`;
      return `<div class="row" data-field="${esc(spec.name)}"><div>${label}${help}</div><div class="control">${control}</div></div>`;
    case 'secret': {
      const saved = spec.setFlag ? data[spec.setFlag] : undefined;
      const status = saved === undefined ? '' : `<span class="secret-state">${saved ? ht('A value is saved. Leave blank to keep it.') : ht('Nothing saved yet.')}</span>`;
      const clear = saved ? `<label class="checks"><input type="checkbox" name="clear_${esc(spec.name)}"> ${ht('Remove the saved value')}</label>` : '';
      control = `<input type="password" ${attrs} autocomplete="new-password" spellcheck="false"${spec.inputmode ? ` inputmode="${spec.inputmode}"` : ''}${spec.maxLength ? ` maxlength="${spec.maxLength}"` : ''} placeholder="${esc(spec.placeholder || (saved ? '••••••••' : ''))}">${status}${clear}`;
      return `<div class="row" data-field="${esc(spec.name)}"><div>${label}${help}</div><div class="control stack">${control}</div></div>`;
    }
    case 'textarea':
      control = `<textarea ${attrs} rows="6" spellcheck="false"${spec.maxLength ? ` maxlength="${spec.maxLength}"` : ''}>${esc(value)}</textarea>`;
      return `<div class="row wide" data-field="${esc(spec.name)}"><div>${label}${help}</div><div class="control stack">${control}</div></div>`;
    case 'number': {
      const min = spec.min !== undefined ? ` min="${spec.scale ? spec.min / spec.scale : spec.min}"` : '';
      const max = spec.max !== undefined ? ` max="${spec.scale ? spec.max / spec.scale : spec.max}"` : '';
      control = `<input type="number" inputmode="numeric" ${attrs}${min}${max} step="${spec.step || 'any'}" value="${esc(shown)}">${unit}`;
      break;
    }
    default:
      control = `<input type="${spec.inputType || 'text'}" ${attrs} value="${esc(value)}" spellcheck="false" autocomplete="off"${spec.maxLength ? ` maxlength="${spec.maxLength}"` : ''}${spec.placeholder ? ` placeholder="${esc(spec.placeholder)}"` : ''}${spec.inputmode ? ` inputmode="${spec.inputmode}"` : ''}>${unit}`;
  }
  return `<div class="row" data-field="${esc(spec.name)}"><div>${label}${help}</div><div class="control">${control}</div></div>`;
}
function fieldRows(specs, values, data = {}) {
  return specs.map(spec => fieldRow(spec, values[spec.name], data)).join('');
}

// Reads one field back as the panel's value (undefined: keep what the panel has).
function readField(root, spec) {
  if (spec.type === 'bits') {
    return $$(`[data-bits="${spec.name}"] input[data-bit]`, root)
      .reduce((mask, box) => box.checked ? mask | Number(box.dataset.bit) : mask, 0);
  }
  const input = root.querySelector(`[name="${spec.name}"]`);
  if (!input) return undefined;
  if (spec.type === 'switch') return input.checked;
  if (spec.type === 'secret') {
    if (root.querySelector(`[name="clear_${spec.name}"]`)?.checked) return '';
    return input.value === '' ? undefined : input.value;
  }
  if (spec.type === 'number' || spec.type === 'range' || (spec.type === 'select' && spec.numeric)) {
    if (input.value === '') return NaN;
    const n = Number(input.value) * (spec.scale || 1);
    return Math.round(n);
  }
  return input.value;
}
function formatValue(spec, value) {
  if (spec.type === 'secret') return value === '' ? t('Removed') : t('New value');
  if (spec.type === 'switch') return value ? t('On') : t('Off');
  if (spec.type === 'select') {
    const option = spec.options.find(o => String(o[0]) === String(value));
    return option ? option[1] : String(value);
  }
  if (spec.type === 'bits') {
    const names = spec.options.filter(([bit]) => Number(value) & bit).map(o => o[1]);
    return names.length ? names.join(', ') : t('None');
  }
  if (spec.format) return spec.format(value);
  const shown = spec.scale ? value / spec.scale : value;
  return value === '' ? t('(empty)') : `${shown}${spec.unit ? ' ' + spec.unit.trim() : ''}`;
}
// Compares a form with the values it started from. Returns the changed panel values and a
// readable list for the review dialog ({name, label, from, to}), or throws with the first invalid
// field.
function collect(root, specs, before) {
  const values = {}, changes = [];
  for (const spec of specs) {
    const row = root.querySelector(`[data-field="${spec.name}"]`);
    if (!row || row.hidden) continue;
    const value = readField(root, spec);
    if (value === undefined) continue;
    if (Number.isNaN(value)) throw new Error(t('{0}: enter a number.', spec.label));
    if (typeof value === 'number' && spec.type !== 'bits' && spec.type !== 'select') {
      if ((spec.min !== undefined && value < spec.min) || (spec.max !== undefined && value > spec.max))
        throw new Error(t('{0}: allowed {1} to {2}.', spec.label, formatValue(spec, spec.min), formatValue(spec, spec.max)));
    }
    // The panel keeps text in bytes (UTF-8), where an accented letter takes two and most other
    // scripts three; the page's maxlength counts characters (docs/features/37 I18N-010).
    if (typeof value === 'string' && spec.maxLength && new TextEncoder().encode(value).length > spec.maxLength)
      throw new Error(t('{0}: too long for the panel ({1} bytes; an accented letter counts as two, most other scripts as three).', spec.label, spec.maxLength));
    if (spec.type !== 'secret' && value === before[spec.name]) continue;
    values[spec.name] = value;
    const was = before[spec.name];
    if (spec.type === 'bits') { // name only what was switched on or off
      const on = spec.options.filter(([bit]) => (value & bit) && !(was & bit)).map(o => o[1]);
      const off = spec.options.filter(([bit]) => !(value & bit) && (was & bit)).map(o => o[1]);
      changes.push({name: spec.name, label: spec.label, from: '', to: [on.length ? t('On: {0}', on.join(', ')) : '', off.length ? t('Off: {0}', off.join(', ')) : ''].filter(Boolean).join(' · ')});
      continue;
    }
    changes.push({name: spec.name, label: spec.label, from: spec.type === 'secret' || was === '' || was === undefined ? '' : formatValue(spec, was), to: formatValue(spec, value)});
  }
  return {values, changes};
}
// Wires range read-outs and showIf rules of the fields inside root.
function liveFields(root, specs) {
  const update = () => {
    const current = {};
    for (const spec of specs) {
      const v = readField(root, spec);
      current[spec.name] = v;
    }
    for (const spec of specs) {
      const row = root.querySelector(`[data-field="${spec.name}"]`);
      if (row && spec.showIf) row.hidden = !spec.showIf(current);
      const help = row && spec.describe ? row.querySelector('.help') : null;
      if (help) help.textContent = spec.describe(current[spec.name]);
    }
    // An "Advanced" section whose every row is hidden has nothing to open.
    for (const details of $$('details.advanced', root)) {
      const rows = $$('[data-field]', details);
      details.hidden = rows.length > 0 && rows.every(r => r.hidden);
    }
    for (const range of $$('input[type=range]', root)) {
      const spec = specs.find(s => s.name === range.name);
      const out = range.parentElement.querySelector('output');
      if (out) out.textContent = range.value + (spec?.unit || '');
    }
  };
  root.addEventListener('input', update);
  root.addEventListener('change', update);
  update();
}
// Combines this page's field specs with the schema the firmware sent: the firmware decides
// which fields exist and their limits; fields this page doesn't know yet still appear.
function withSchema(specs, schema, appendUnknown = true) {
  if (!schema) return specs;
  const byName = new Map(schema.map(f => [f.name, f]));
  const merged = [];
  for (const spec of specs) {
    const field = byName.get(spec.name);
    if (!field && !spec.local) continue;
    const out = {...spec};
    if (field) {
      if (out.min === undefined && field.type === 'number') out.min = field.min;
      if (out.max === undefined && field.type === 'number') out.max = field.max;
      if (out.maxLength === undefined && field.maxLength) out.maxLength = field.maxLength;
      // A list the panel knows best (its languages, regions): the panel's choices, as it names them.
      if (out.type === 'select' && !out.options && field.choices) {
        out.options = field.choices.map(c => [c.value, c.label]);
        out.numeric = typeof field.choices[0]?.value === 'number';
      }
    }
    merged.push(out);
  }
  for (const field of appendUnknown ? schema : []) {
    if (specs.some(s => s.name === field.name)) continue;
    merged.push({
      name: field.name, label: titleCase(field.name),
      type: field.choices ? 'select' : field.type === 'boolean' ? 'switch' : field.type === 'password' ? 'secret' : field.type,
      options: field.choices?.map(c => [c.value, titleCase(c.label)]), numeric: !!field.choices,
      min: field.min, max: field.max, maxLength: field.maxLength,
    });
  }
  return merged;
}

// ---------------------------------------------------------------------------------------
// Sound and speech for the walk test (docs/features/40 WALK-014). Browsers play nothing before a tap
// (iOS above all): one listener for the whole app, set at start, unlocks both on the first tap anywhere,
// so a test started at the panel and followed on a phone beeps with no "turn on sound" step.

let audioCtx = null;
function unlockAudio() {
  try {
    audioCtx = audioCtx || new (window.AudioContext || window.webkitAudioContext)();
    if (audioCtx.state === 'suspended') audioCtx.resume();
  } catch { /* no Web Audio here */ }
  try { // iOS speaks only what a tap started: a silent word opens it
    const u = new SpeechSynthesisUtterance(' ');
    u.volume = 0;
    speechSynthesis.speak(u);
  } catch { /* no speech here */ }
}
// The panel's own beep: 1760 Hz, 70 ms on and 60 off, three times; a repeat trip a single short tick.
function beep(repeat) {
  try {
    if (!audioCtx) return;
    if (audioCtx.state === 'suspended') audioCtx.resume(); // after the phone slept
    const t0 = audioCtx.currentTime + 0.02;
    for (const [at, len] of repeat ? [[0, 0.04]] : [[0, 0.07], [0.13, 0.07], [0.26, 0.07]]) {
      const o = audioCtx.createOscillator(), g = audioCtx.createGain();
      o.frequency.value = 1760;
      g.gain.value = 0.12;
      o.connect(g).connect(audioCtx.destination);
      o.start(t0 + at);
      o.stop(t0 + at + len);
    }
  } catch { /* nothing to hear */ }
}
const canSpeak = () => typeof speechSynthesis !== 'undefined' && typeof SpeechSynthesisUtterance !== 'undefined';
// Says text in the page's language; done() runs once when it has been said (or could not be, or the browser
// never said it was done: some don't).
function speakNow(text, done) {
  let finished = false;
  const finish = () => { if (!finished) { finished = true; done?.(); } };
  try {
    const u = new SpeechSynthesisUtterance(text);
    u.lang = document.documentElement.lang || words.locale;
    u.onend = u.onerror = finish;
    speechSynthesis.speak(u);
    setTimeout(finish, 3000 + text.length * 120);
  } catch { finish(); }
}
function listenForFirstTap() {
  const events = ['click', 'touchend', 'keydown'];
  const once = () => { for (const e of events) document.removeEventListener(e, once, true); unlockAudio(); };
  for (const e of events) document.addEventListener(e, once, true);
}

// ---------------------------------------------------------------------------------------
// Dialogs and messages

function toast(message, kind = '') {
  const box = $('#toast');
  box.textContent = message;
  box.className = 'toast ' + kind;
  box.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { box.hidden = true; }, kind === 'error' ? 9000 : 6000);
}
function openDialog(html) {
  const dialog = $('#dialog');
  $('#dialog-content').innerHTML = html;
  eyes($('#dialog-content'));
  if (!dialog.open) dialog.showModal();
  const first = $('#dialog-content input:not([type=hidden]), #dialog-content select, #dialog-content textarea');
  (first || $('#dialog-content button'))?.focus();
  return dialog;
}
function closeDialog() {
  const dialog = $('#dialog');
  if (dialog.open) dialog.close();
}
// WEB-016, BL-135: no keypad codes here. A few changes (remote access, a workstation's enrolment, a backup, a
// reset) ask for your password again, only when the panel's window (status.reauth_s) has run out.
const REAUTH_MS = 10 * 60e3;
function passwordDue() { return Date.now() >= (state?.reauthUntil || 0); }
function passwordConfirmed() { if (state) state.reauthUntil = Date.now() + REAUTH_MS; }
function passwordField() {
  return `<div class="code-field"><label for="me-password">${ht('Your password')}</label>
    <input id="me-password" name="me_password" type="password" autocomplete="current-password" maxlength="64" required>
    <small>${ht('The one you sign in with, to confirm it’s you. Not asked again for 10 minutes.')}</small></div>`;
}
// The dialog's password box, if one is on screen (or put there when the panel says the window ran out).
function dialogPassword() { return $('#me-password'); }
// Called when the panel answers password_required or wrong_password: show the box (once) and say why.
function askPasswordAgain(error, body) {
  if (error.code !== 'password_required' && error.code !== 'wrong_password') return false;
  if (state) state.reauthUntil = 0;
  let input = dialogPassword();
  if (!input) {
    (body || $('#dialog-content .dialog-body') || $('#dialog-content form')).insertAdjacentHTML('beforeend', passwordField());
    eyes($('#dialog-content'));
    input = dialogPassword();
  } else input.value = '';
  $('#dialog-error').textContent = error.code === 'wrong_password' ? t('That password isn’t right.') : t('Enter your password.');
  input?.focus();
  return true;
}
// The box for a dialog of its own (backup, reset, enrolment): only when due. passwordOf() reads it (and refuses an
// empty one); withPassword() is what the request carries: nothing when there was no box.
function passwordBox() { return passwordDue() ? passwordField() : ''; }
function passwordOf(form) {
  const input = form.querySelector('[name=me_password]');
  if (input && !input.value) { input.focus(); throw new Error(t('Enter your password.')); }
  return input ? input.value : '';
}
function withPassword(mePassword) { return mePassword ? {me_password: mePassword} : {}; }
// The panel's answer to a fetch of ours that isn't ok, as an Error that carries the panel's code.
function answerError(detail, status) {
  const error = new Error(friendly(detail?.error) || t('The panel answered {0}.', status));
  error.code = detail?.error;
  return error;
}
// Shows what will change and asks to confirm, then runs the change.
//   changes: [{label, from, to}]; password: true for a protected change (asks for it only when due)
//   run(mePassword): does the change; throwing keeps the dialog open with the message.
function review({title = t('Review changes'), intro = '', changes = [], password = false, confirm = t('Save changes'),
                 danger = false, done = t('Saved on the panel.'), run, after}) {
  const list = changes.length ? `<ul class="changes">${changes.map(c =>
    `<li><span class="what">${esc(c.label)}</span><span class="to">${c.from !== '' && c.from !== undefined ? `<span class="from">${esc(c.from)}</span>` : ''}${esc(c.to)}</span></li>`).join('')}</ul>` : '';
  openDialog(`<form id="review-form" novalidate>
    <div class="dialog-head"><h2 id="dialog-title">${esc(title)}</h2>${intro ? `<p>${esc(intro)}</p>` : ''}</div>
    <div class="dialog-body">${list}${password && passwordDue() ? passwordField() : ''}</div>
    <p class="dialog-error" id="dialog-error" role="alert"></p>
    <div class="dialog-foot"><button type="button" id="dialog-cancel">${ht('Cancel')}</button>
    <button type="submit" class="${danger ? 'danger solid' : 'primary'}">${esc(confirm)}</button></div></form>`);
  $('#dialog-cancel').onclick = closeDialog;
  $('#review-form').onsubmit = async event => {
    event.preventDefault();
    const button = event.submitter || $('#review-form [type=submit]');
    const input = dialogPassword();
    if (input && !input.value) {
      $('#dialog-error').textContent = t('Enter your password.');
      input.focus();
      return;
    }
    const mePassword = input ? input.value : '';
    button.disabled = true;
    $('#dialog-error').textContent = '';
    try {
      await run(mePassword);
      if (password && mePassword) passwordConfirmed();
      if (input) input.value = '';
      closeDialog();
      if (done) toast(done);
      if (after) await after(); else await show();
    } catch (error) {
      if (!askPasswordAgain(error)) $('#dialog-error').textContent = error.message;
    } finally {
      button.disabled = false;
    }
  };
}
// An editor in a dialog: fields, then a review of what changed.
function editDialog({title, intro = '', specs, values, initial = values, data = {}, readOnly = false, extraButtons = '',
                     reviewOptions, submit, bindExtra}) {
  openDialog(`<form id="edit-form" novalidate>
    <div class="dialog-head"><h2 id="dialog-title">${esc(title)}</h2>${intro ? `<p>${esc(intro)}</p>` : ''}</div>
    <div class="dialog-body"><fieldset${readOnly ? ' disabled' : ''}><div class="rows">${fieldRows(specs, initial, data)}</div></fieldset></div>
    <p class="dialog-error" id="dialog-error" role="alert"></p>
    <div class="dialog-foot">${extraButtons}<button type="button" id="dialog-cancel">${readOnly ? ht('Close') : ht('Cancel')}</button>
    ${readOnly ? '' : `<button type="submit" class="primary">${ht('Review')}</button>`}</div></form>`);
  const form = $('#edit-form');
  liveFields(form, specs);
  $('#dialog-cancel').onclick = closeDialog;
  bindExtra?.(form);
  form.onsubmit = event => {
    event.preventDefault();
    try {
      const result = collect(form, specs, values);
      if (submit) return submit(result, form);
      if (!result.changes.length) return toast(t('Nothing changed.'));
      review({...reviewOptions, changes: result.changes, run: code => reviewOptions.run(result.values, code)});
    } catch (error) {
      $('#dialog-error').textContent = error.message;
    }
  };
}

// ---------------------------------------------------------------------------------------
// Navigation. PAGES comes from panel-pages.js: {id: {title, group, icon, installer, nav,
// parent, when, render(arg) -> html | {html, bind}}}.

function parseHash() {
  const [id, ...rest] = location.hash.replace(/^#\/?/, '').split('/');
  return {id: PAGES[id] ? id : 'overview', arg: decodeURIComponent(rest.join('/'))};
}
function go(id, arg = '') {
  const hash = '#/' + id + (arg !== '' ? '/' + encodeURIComponent(arg) : '');
  if (location.hash === hash) show(); else location.hash = hash;
}
// May the signed-in user open this page? The panel's own table (docs/features/14) decides on every
// request; the menu only leaves out what would be refused, as the panel's Settings would.
function pageAllowed(p) {
  return !!p && (!p.when || p.when()) && (!p.access || !!state?.access?.[p.access]);
}
function renderNav() {
  const current = PAGES[page]?.parent || page;
  let html = '', group = null;
  for (const [id, p] of Object.entries(PAGES)) {
    if (p.nav === false || !pageAllowed(p)) continue;
    if (p.group !== group) {
      group = p.group;
      if (group) html += `<div class="nav-group">${esc(t(group))}</div>`;
    }
    // A page may count what waits there (Troubles): a steady number, never a blinking dot.
    const count = p.badge?.() || 0;
    html += `<a href="#/${id}"${id === current ? ' aria-current="page"' : ''}>${icon(p.icon)}<span>${esc(t(p.title))}</span>${count ? `<span class="badge${p.badgeKind ? ' ' + p.badgeKind : ''}">${esc(count)}</span>` : ''}</a>`;
  }
  $('#nav').innerHTML = html;
}
function openMenu(open) {
  document.body.classList.toggle('menu-open', open);
  $('#scrim').hidden = !open;
  $('#menu').setAttribute('aria-expanded', String(open));
  $('#menu').setAttribute('aria-label', open ? t('Close menu') : t('Open menu'));
}
function updateChrome() {
  if (!state) return;
  const name = state.partitions?.[0]?.name || t('Alarm panel');
  $('#panel-name').textContent = name;
  $('#panel-address').textContent = `${location.host} · ${state.panel_id}`;
  $('#account-name').textContent = `${state.name || tc('default user name', 'User {0}', state.user)} · ${roleWord(state.authority) || t('User')}`;
  $('#account').hidden = false;
  $('#change-password').hidden = !state.has_password;
  $('#menu').hidden = false;
  const connection = $('#connection');
  connection.innerHTML = `<span>${ht('Connected')}</span>`;
  connection.classList.remove('offline');
}

async function show() {
  stopFrame();
  openMenu(false);
  closeDialog();
  const previous = route.id;
  if (previous === 'walk') walkLeave(); // lets the phone's screen sleep again
  route = parseHash();
  if (!state) return loginPage();
  if (!pageAllowed(PAGES[route.id])) route = {id: 'overview', arg: ''};
  // A temporary password signs in only to be changed (the panel refuses everything else).
  if (state.password_change_required) route = {id: 'password', arg: ''};
  page = route.id;
  const definition = PAGES[page], id = ++renderId;
  renderNav();
  $('#crumb').textContent = t(definition.title);
  document.title = t('{0} · Panel Manager', t(definition.title));
  if (previous !== page) window.scrollTo(0, 0);
  $('#main').innerHTML = `<p class="loading">${ht('Loading…')}</p>`;
  try {
    const out = await definition.render(route.arg);
    if (id !== renderId) return;
    const view = typeof out === 'string' ? {html: out} : out;
    $('#main').innerHTML = view.html;
    eyes($('#main'));
    $$('#main [data-go]').forEach(b => { b.onclick = () => go(b.dataset.go, b.dataset.arg || ''); });
    networkNotice();
    lockedNotice();
    view.bind?.();
    $('#main').focus({preventScroll: true});
  } catch (error) {
    if (id !== renderId || !state) return;
    $('#main').innerHTML = pageHead(t(definition.title)) +
      notice('bad', t('This page could not be loaded'), error.message) + `<button id="retry" class="primary">${ht('Try again')}</button>`;
    $('#retry').onclick = show;
  }
}

function loginPage(message = '', note = false) {
  stopFrame();
  closeDialog();
  state = null;
  csrf = '';
  ++renderId;
  $('#nav').replaceChildren();
  $('#account').hidden = true;
  $('#menu').hidden = true;
  $('#panel-name').textContent = t('Your alarm panel');
  $('#panel-address').textContent = location.host;
  $('#crumb').textContent = t('Sign in');
  document.title = t('{0} · Panel Manager', t('Sign in'));
  const connection = $('#connection');
  connection.innerHTML = `<span>${ht('Signed out')}</span>`;
  connection.classList.add('offline');
  $('#main').innerHTML = `<div class="login"><div class="card"><h1>${ht('Sign in')}</h1>
    <p>${ht('Use your user name and browser password. Your user name is your name in Settings > Users on the panel.')}</p>
    <form id="login-form"><label for="login-name">${ht('User name')}</label>
    <input id="login-name" name="username" type="text" autocomplete="username" autocapitalize="none" spellcheck="false" maxlength="47" required>
    <label for="login-password">${ht('Password')}</label>
    <input id="login-password" name="password" type="password" autocomplete="current-password" maxlength="64" required>
    <button class="primary">${ht('Sign in')}</button><p class="error" id="login-error" role="alert">${esc(message)}</p></form>
    <p class="gap"><small>${ht('Keypad codes don’t sign in here. No password yet? Whoever looks after the panel’s users (the installer or a master) turns on your browser sign-in, at the panel in Settings > Users > your name, or here. After five wrong tries, sign-in waits a minute.')}</small></p></div></div>`;
  eyes($('#main'));
  if (note) { // said as a note, not in red: nothing went wrong with the sign-in
    $('#login-error').classList.add('note');
    $('#login-error').classList.remove('error');
    $('#login-error').setAttribute('role', 'status');
  }
  $('#login-name').focus();
  $('#login-form').onsubmit = async event => {
    event.preventDefault();
    const button = event.target.querySelector('button'), name = $('#login-name'), input = $('#login-password');
    $('#login-error').textContent = '';
    $('#login-error').className = 'error';
    $('#login-error').setAttribute('role', 'alert');
    if (input.value.length < PASSWORD_MIN) {   /* a keypad code: say so without a wrong try (B010) */
      input.value = '';
      $('#login-error').textContent = notACode();
      return;
    }
    button.disabled = true;
    try {
      const result = await api('login', {username: name.value.trim(), password: input.value});
      input.value = '';
      csrf = result.csrf;
      await connect();
    } catch (error) {
      input.value = '';
      $('#login-error').textContent = error.status === 429 ? t('Too many tries. Wait a moment before trying again.')
        : error.code === 'password_too_short' ? notACode()
        : error.code === 'invalid_login' ? t('That user name and password don’t match a sign-in set on this panel. Keypad codes don’t work here: use your browser password.')
        : error.message;
    } finally {
      button.disabled = false;
    }
  };
}

async function connect() {
  try {
    await refreshStatus();
    await show();
    watchRestart();
  } catch (error) {
    if (!state) loginPage(); else toast(error.message, 'error');
  }
}

// ---------------------------------------------------------------------------------------
// Changes that outlive the page: a network trial and a firmware update

const randomToken = () => Array.from(crypto.getRandomValues(new Uint8Array(16)), x => x.toString(16).padStart(2, '0')).join('');

// The panel tries new network settings for two minutes and goes back unless confirmed.
async function changeNetwork(path, body, newAddress = '') {
  const operation = randomToken();
  sessionStorage.setItem('panel-network', JSON.stringify({operation, started: Date.now(), address: newAddress}));
  try {
    await api(path, {...body, operation});
  } catch (error) {
    if (error.status) { sessionStorage.removeItem('panel-network'); throw error; }
    // No answer: the connection is probably moving with the network. That is expected.
  }
}
// Armed (from the exit time on) or an alarm sounding: nothing changes until everything is
// disarmed and quiet, exactly as at the panel. The remote keypad is how you disarm.
function lockedNotice() {
  // Automations says it itself, in its own words (one notice, never two).
  if (!state?.locked || ['keypad', 'overview', 'activity', 'walk', 'password', 'automations'].includes(page)) return;
  const box = document.createElement('div');
  box.innerHTML = notice('warn', t('The panel is armed or an alarm is sounding'),
    t('Changes wait until everything is disarmed and quiet, as at the panel.'));
  $('#main').prepend(box.firstElementChild);
}
function networkNotice() {
  const saved = sessionStorage.getItem('panel-network');
  if (!saved || !state) return;
  const trial = JSON.parse(saved);
  const left = 120000 - (Date.now() - trial.started);
  if (left <= 0) { sessionStorage.removeItem('panel-network'); return; }
  const link = trial.address && trial.address !== location.hostname
    ? `<p>${hx('If this page stops answering, open {0}.', `<a href="https://${esc(trial.address)}/#network=${esc(trial.operation)}">https://${esc(trial.address)}</a>`)}</p>` : '';
  const box = document.createElement('div');
  box.innerHTML = notice('warn', t('Keep the new network settings?'),
    t('The panel is trying them. It goes back to the previous settings in about {0} min unless you confirm.', Math.ceil(left / 60000)),
    `${link}<div class="actions"><button type="button" class="primary" id="keep-network">${ht('Keep the new settings')}</button></div>`);
  $('#main').prepend(box.firstElementChild);
  $('#keep-network').onclick = async () => {
    try {
      await api('network-confirm', {operation: trial.operation});
      sessionStorage.removeItem('panel-network');
      toast(t('New network settings kept.'));
      show();
    } catch (error) { toast(error.message, 'error'); }
  };
}

async function uploadFirmware(file, allowOlder, progress) {
  const data = await file.arrayBuffer();
  const sha = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', data)), x => x.toString(16).padStart(2, '0')).join('');
  const staged = await new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('PUT', '/api/v1/upload');
    xhr.setRequestHeader('X-Panel-CSRF', csrf);
    xhr.setRequestHeader('X-Image-SHA256', sha);
    if (allowOlder) xhr.setRequestHeader('X-Allow-Older', 'yes');
    xhr.timeout = 330000;
    xhr.upload.onprogress = e => progress?.(Math.round(e.loaded / e.total * 100));
    xhr.onload = () => {
      let result;
      try { result = JSON.parse(xhr.responseText); } catch { /* not JSON */ }
      if (xhr.status === 200) resolve(result); else reject(new Error(friendly(result?.error) || t('Upload failed.')));
    };
    xhr.onerror = () => reject(new Error(t('The connection was lost during the upload. The running firmware is unchanged.')));
    xhr.ontimeout = () => reject(new Error(t('The upload timed out. The running firmware is unchanged.')));
    xhr.send(data);
  });
  sessionStorage.setItem('panel-update', JSON.stringify({...staged, started: Date.now()}));
  try { await api('upload-install', {operation: staged.operation}); } catch { /* the restart may cut the answer */ }
  await followUpdate();
}
async function followUpdate() {
  const saved = sessionStorage.getItem('panel-update');
  if (!saved) return;
  const operation = JSON.parse(saved);
  stopFrame();
  ++renderId;
  $('#main').innerHTML = pageHead(t('Installing the update'), t('Keep this page open while the panel restarts and checks itself.')) +
    `<div class="card pad"><h2 id="update-progress">${ht('Waiting for the panel to restart…')}</h2>
    <p class="gap">${ht('If the new firmware does not start properly, the panel goes back to the previous one by itself.')}</p></div>`;
  while (Date.now() - operation.started < 240000) {
    try {
      const result = await api('update-confirm', {operation: operation.operation, elf: operation.elf});
      if (result.complete) {
        sessionStorage.removeItem('panel-update');
        toast(t('The update is installed and the panel passed its checks.'));
        state = null;
        await connect();
        return;
      }
      $('#update-progress').textContent = t('The new firmware is running. Health checks in progress…');
    } catch { /* restarting */ }
    await new Promise(resolve => setTimeout(resolve, 3000));
  }
  $('#update-progress').textContent = t('The update did not confirm in time. Reload to see which firmware is running.');
}

// ---------------------------------------------------------------------------------------
// Start-up

async function startup() {
  await loadWords(); /* this browser's language, before anything is drawn */
  translatePage();
  $('#menu').innerHTML = icon('menu');
  $('#menu').onclick = () => openMenu(!document.body.classList.contains('menu-open'));
  $('#scrim').onclick = () => openMenu(false);
  document.addEventListener('keydown', e => { if (e.key === 'Escape') openMenu(false); });
  listenForFirstTap();
  $('#change-password').onclick = () => go('password');
  $('#signout').onclick = async () => {
    try { await api('logout', {}); } catch { /* signed out either way */ }
    loginPage(t('You are signed out.'));
  };
  window.addEventListener('hashchange', () => { if (state) show(); });
  // The remote keypad keeps its last picture while the tab is in the background (no pictures
  // come meanwhile) and picks up again when it is back.
  document.addEventListener('visibilitychange', () => {
    if (page === 'keypad' && state) screenVisible(!document.hidden);
  });
  window.addEventListener('pagehide', stopFrame);

  // One-time fragments: #grant=... (from panelctl) and #network=... (a network trial).
  const fragment = new URLSearchParams(location.hash.slice(1));
  if (fragment.has('grant') || fragment.has('network')) history.replaceState(null, '', location.pathname);
  if (fragment.has('network'))
    sessionStorage.setItem('panel-network', JSON.stringify({operation: fragment.get('network'), started: Date.now()}));
  if (fragment.has('grant')) {
    try { csrf = (await api('login', {grant: fragment.get('grant')})).csrf; } catch (error) { toast(error.message, 'error'); }
  }
  if (sessionStorage.getItem('panel-update')) return followUpdate();
  await connect();
}
startup();
