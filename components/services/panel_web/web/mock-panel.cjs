// Panel Manager on the PC, against a pretend panel. Serves the real panel.html/js/css and
// answers /api/v1 like the firmware does (components/services/panel_web), with made-up data kept in
// memory. For trying UI changes before a panel is flashed; not part of the product.
//
//   node components/services/panel_web/web/mock-panel.cjs [port]      then open http://127.0.0.1:<port>
//   Sign in as alex / "correct horse battery" (system master), installer / "installer password"
//   (installer), pat / "partition master" (partition master) or sam / "standard user" (standard).
//   Keypad codes (asked for changes): 1234 Alex, 4321 installer, 5555 Pat, 1111 Sam.
//   MOCK_CODE_MIN=6: the system's shortest allowed code (GET people code_min; 4 when unset).
//   MOCK_LOCKED=1: the panel is armed (changes refused, as on the panel; arming and disarming still work).
//   MOCK_SCENARIO=ready|notready|motion|exit|armed|stay|entry|alarm|memory|fire|troubles: the first partition's
//   starting state, for screenshots (ready is the default; with MOCK_LOCKED and no scenario it starts armed).
//   troubles: reports not getting through and a siren problem, a motion sensor not reporting and a
//   leak sensor's battery low, the trouble beeps waiting for an acknowledgement.
//   fire: the hallway smoke detector in alarm while disarmed, siren on; acknowledge silences it. Trouble
//   beeps wait for an acknowledgement there and in the garage (MOCK_PARTITIONS=2) too.
//   MOCK_FAST=1: the arming delays run 20 times faster (for tests). MOCK_AUTOLOGIN=guest signs in as the dog walker.
//   MOCK_CLOCK=ok|lost|failed|other: a battery-backed clock module is fitted (docs/features/35);
//   without it the panel has none, like most panels.
//   MOCK_WALK=1: a walk test runs on the first partition from the start, started by the installer at
//   the panel (docs/features/40 WALK-024). Once one starts, a pretend tester trips a zone every
//   MOCK_WALK_STEP_MS (4000; 0: nobody walks), a repeat and a cover among them.
//   MOCK_AUTOMATIONS=0|empty|full|clock|readonly|newer|arm: the automations (docs/features/41); 0 is a build without them;
//   arm adds the arming and disarming ones (phase C, slots 5..10) with their statuses.
//   MOCK_AUTO_WARN=on|hour|night|day: automation 5 (Arm Night) warns now, or was put off by Pat (an hour, not tonight,
//   not today): the Overview's card or note (docs/features/41 D19, D20, D27). POST mock/automation-warning {state} sets it.
//   MOCK_AUTO_REMIND=1: a "Remind to arm" automation at 11:00 PM found Home disarmed: its steady note on the Overview
//   (BL-224). POST mock/automation-reminder {on} sets it. MOCK_AUTO_OUTPUTS=0: no wired output "For automations" (one,
//   Garden Lights, otherwise); MOCK_AUTO_NOTIFY=0: no phone service set up (the Pushover one otherwise);
//   MOCK_AUTO_DEVICES=0: a new panel with no smart device (no Home Assistant, Z-Wave or Zigbee one).
//   MOCK_GRANT=1: the autologin session is a workstation's grant, not a password sign-in (WEB-024).
//   MOCK_SETUP=unclaimed|key|network|check|confirm|recovery: a panel without a screen being set up from a phone
//   (docs/features/31 headless-setup.md, web_setup.c): the setup pages instead of the sign-in. Whoever is on the setup
//   Wi-Fi claims it (no setup code); check is the phone on the setup Wi-Fi while the home Wi-Fi is tried (it joins after 3 s),
//   confirm the same seen from the home network, recovery a set-up panel whose BOOT button opened recovery
//   (the recovery key is KEYAB-CDEFG-HJKMN-PQRST-UVWXY).
'use strict';
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const liveScreen = require('./mock-screen.cjs');

const FILES = {
  '/': ['panel.html', 'text/html; charset=utf-8'],
  '/panel.css': ['panel.css', 'text/css; charset=utf-8'],
  '/panel.js': ['panel.js', 'text/javascript; charset=utf-8'],
  '/panel-pages.js': ['panel-pages.js', 'text/javascript; charset=utf-8'],
  '/panel-words.js': ['panel-words.js', 'text/javascript; charset=utf-8'],
  '/panel-keypad.js': ['panel-keypad.js', 'text/javascript; charset=utf-8'],
};
const CODES = {'1234': 2, '4321': 1, '5555': 5, '1111': 3}; // keypad code -> user (confirms changes)
// Browser sign-ins: user name -> [password, user]. The authority comes from the user record.
const LOGINS = {'alex': ['correct horse battery', 2], 'installer': ['installer password', 1],
  'pat': ['partition master', 5], 'sam': ['standard user', 3],
  'dog walker': ['guest password one', 4]}; // a guest has no browser sign-in (webLogins): only MOCK_AUTOLOGIN=guest is them
const temporary = new Set(); // users whose password is temporary: change it first
if (process.env.MOCK_TEMPORARY) temporary.add(2);
const AUTHORITY = {1: 'installer', 2: 'master', 3: 'partition_master', 4: 'standard', 5: 'arm_only', 6: 'guest', 7: 'duress'};
// The firmware's table (panel_web.c get_access/post_access): who may use which path.
const ACC = {anyone: () => true, users: a => ['installer', 'master', 'partition_master'].includes(a),
  shared: a => ['installer', 'master', 'partition_master'].includes(a), system: a => ['installer', 'master'].includes(a),
  installer: a => a === 'installer'};
const GET_ACCESS = p => ['status', 'frame', 'screen', 'history', 'activity', 'time', 'zone-status', 'troubles', 'storage', 'config/display', 'languages', 'voices'].includes(p) ? 'anyone'
  : p === 'people' ? 'users' : ['config/network', 'network-check', 'wifi-scan', 'walk-test'].includes(p) ? 'shared'
  : ['updates', 'config/mqtt', 'config/ntfy', 'config/pushover', 'home_assistant', 'notification-status', 'automations', 'automations/devices'].includes(p) ? 'system' : 'installer';
const POST_ACCESS = (p, body) => p === 'config/display'
  ? (body.values && ('lang' in body.values || 'region' in body.values) ? 'shared' : 'anyone') // docs/features/14
  // A walk test's start and extend: those who may start one (WALK-001); its end is the engine's to decide (WALK-025).
  : p === 'partition' ? (['walk_test_start', 'walk_test_extend'].includes(body.action) ? 'shared' : 'anyone')
  : ['logout', 'password', 'control'].includes(p) ? 'anyone' : p === 'people' ? 'users'
  : ['config/network', 'wifi', 'time', 'network-check', 'wifi-scan', 'languages'].includes(p) ? 'shared'
  : p === 'updates' ? (['server_check', 'server', 'channel'].includes(body.action) ? 'installer' : 'system') // OTA-017..OTA-022
  : p === 'config/mqtt' ? (body.values && 'allow_panic' in body.values ? 'installer' : 'system') // BL-107
  : ['config/ntfy', 'config/pushover', 'home_assistant', 'storage', 'notification-test', 'ntfy-topic', 'certificate-authority', 'automations'].includes(p) ? 'system' : 'installer';
const lockedEnv = !!process.env.MOCK_LOCKED;

// The panel's lock (alarm_snapshot_locked): a partition armed from its exit time on, or in alarm.
const isLocked = () => lockedEnv || parts.some(p => p.state !== 'disarmed' || p.alarm);
const ABORT_WINDOW_MS = 30000; // the engine's default abort_window_s
// ---- Z-Wave (docs/features/36 screens.md §11): a radio with two devices and SmartStart's list,
// kept as the panel keeps it; a code is checked as the panel checks it (zw_qr.c). MOCK_ZWAVE=0:
// a panel without Z-Wave.
const zwaveOn = process.env.MOCK_ZWAVE !== '0';
const zwave = {
  radio: {state: 'working', region_known: true, region: 1, devices: 9, not_heard: 2, firmware: '1.50', sdk: '7.22.1', update_advised: false,
    software: {check: 'none', version: '', words: ''},
    jammed: process.env.MOCK_ZWAVE === 'jammed', noise_dbm: process.env.MOCK_ZWAVE === 'jammed' ? -48 : -95,
    storage_full: process.env.MOCK_ZWAVE === 'full'}, // ZW-039: MOCK_ZWAVE=full, Add answers 'full'
  devices: [
    {node: 2, generation: 1, model: 'Zooz ZSE41 open/close sensor', kind: 1, security: 5, state: 0, unreachable: false, listening: false,
      signal: 'good', heard_s: 340, battery: 92, battery_low: false, zones: [3]},
    {node: 3, generation: 2, model: '', kind: 3, security: 4, state: 0, unreachable: false, listening: true, heard_s: 12, battery_low: false, zones: []},
    {node: 5, generation: 4, model: 'Keypad', kind: 2, security: 6, state: 5, setup_done: 15, setup_total: 16, unreachable: true, listening: true, battery_low: false, zones: []},
    // One device in each setup state (ZW-019), and one not heard for two hours
    {node: 7, generation: 6, model: 'Smoke alarm', kind: 1, security: 4, state: 1, setup_done: 15, setup_total: 16, unreachable: false, listening: false, signal: 'good', heard_s: 20, battery_low: false, zones: []},
    {node: 8, generation: 7, model: 'Door sensor', kind: 1, security: 4, state: 2, setup_done: 15, setup_total: 16, unreachable: false, listening: false, signal: 'fair', heard_s: 90, battery_low: false, zones: []},
    {node: 9, generation: 8, model: 'Glass break sensor', kind: 1, security: 1, state: 3, unreachable: false, listening: false, signal: 'weak', heard_s: 60, battery_low: false, zones: []},
    {node: 10, generation: 9, model: 'Hand-held remote', kind: 0, security: 1, state: 4, unreachable: false, listening: false, battery_low: false, zones: []},
    {node: 11, generation: 10, model: 'Window sensor', kind: 1, security: 4, state: 6, unreachable: false, listening: false, battery_low: false, zones: []},
    {node: 12, generation: 11, model: 'Garage sensor', kind: 1, security: 5, state: 0, unreachable: true, listening: false, signal: 'weak', heard_s: 7300, battery: 55, battery_low: false, zones: []},
  ],
  // the network operation running, as zw_progress says it (screens.md §4, §7)
  progress: {op: 'none', step: 'idle', result: 'none', seconds_left: 0, requested: 0, csa: false, dsk: '', node: 0, generation: 0,
    security: 0, s2_failure: 'ok', setup_done: 0, setup_total: 0, asleep: false, stalled: false, model: '', zones: [], replacing: false,
    routes_done: 0, routes_total: 0, routes_failed: 0, routes_skipped: 0, routes_waiting: 0, routes_stopped: false, routes_list: []},
  diag: {home_id: 'C0FFEE01', node_id: 1, suc_node_id: 1, chip: '08 v1', library: 'Z-Wave 7.22', library_type: 7, product: '027A:0004:0611',
    noise_dbm: [-96, -95, -97], jam_threshold_dbm: -60, power_dbm: 0, lr_power_dbm: 14, power_came_with_dbm: 0,
    link_ups: 3, link_downs: 0, restarts: 1, recoveries: 0, foreign: 0, malformed: 1,
    nodes: [{node: 2, wrong_level: 0, decrypt_failures: 0, duplicates: 1}, {node: 3, wrong_level: 2, decrypt_failures: 0, duplicates: 0},
      {node: 5, wrong_level: 0, decrypt_failures: 1, duplicates: 0}]},
  smartstart: [], // {groups: [8 numbers], name, state, failures, long_range, requested, grant, node, generic, product}
};
function zwParse(code) {
  const plain = String(code || '').replace(/\s/g, '');
  const digits = plain.replace(/-/g, '');
  if (/^[\d-]+$/.test(plain) && digits.length === 40) {
    const groups = digits.match(/\d{5}/g).map(Number);
    return groups.some(g => g > 65535) ? {error: 'bad_code'} : {groups, requested: 0, grant: 7, generic: 0};
  }
  if (!/^90\d{50,}$/.test(plain) || plain.length > 512) return {error: 'bad_code'};
  const version = Number(plain.slice(2, 4));
  if (version > 1) return {error: 'bad_code'};
  const hash = crypto.createHash('sha1').update(plain.slice(9)).digest();
  if (Number(plain.slice(4, 9)) !== hash[0] * 256 + hash[1]) return {error: 'checksum'};
  if (version === 0) return {error: 's2_only'};
  const requested = Number(plain.slice(9, 12)) & 7;
  const groups = plain.slice(12, 52).match(/\d{5}/g).map(Number);
  if (!requested || groups.some(g => g > 65535)) return {error: 'bad_code'};
  const out = {groups, requested, grant: requested, generic: 0, product: ''};
  for (let at = 52; at + 4 <= plain.length;) {
    const type = Number(plain.slice(at, at + 2)) >> 1, length = Number(plain.slice(at + 2, at + 4)), data = plain.slice(at + 4, at + 4 + length);
    if (type === 0 && data.length === 10) out.generic = Number(data.slice(0, 5)) >> 8;
    if (type === 1 && data.length === 20) out.product = [0, 5, 10].map(i => Number(data.slice(i, i + 5)).toString(16).toUpperCase().padStart(4, '0')).join(':');
    at += 4 + length;
  }
  return out;
}
const ZW_MODELS = {'027A:7000:E001': 'Zooz ZSE41 open/close sensor'}; // the panel's quirks table, in part
const zwId = e => e.groups.slice(1).map(g => g.toString(16).padStart(4, '0')).join('');
const zwEntry = e => ({id: zwId(e), key: 'xxxxx-' + e.groups.slice(1).map(g => String(g).padStart(5, '0')).join('-'), name: e.name, state: e.state,
  failures: e.failures, long_range: false, requested: e.requested, grant: e.grant, node: e.node, generic: e.generic,
  ...(ZW_MODELS[e.product] ? {model: ZW_MODELS[e.product]} : {}), ...(e.product ? {product: e.product} : {})});
zwave.smartstart.push({...zwParse('900132782003515253545541424344453132333435212223242500100435301537022065520001000000300578'),
  name: 'Hall light', state: 'waiting', failures: 0, node: 0});
// A joined sensor of a known model with no name: the model names it and its row links to device 7.
zwave.smartstart.push({groups: [11111, 22222, 33333, 44444, 55555, 12121, 23232, 34343], requested: 4, grant: 4, generic: 0x20,
  product: '027A:7000:E001', name: '', state: 'joined', failures: 0, node: 7});
// A pretend network operation: the steps the panel's zw_progress walks through, on timers. The
// joining device's PIN is 12345 (its label), anything else is "wrong_pin".
const zwTimers = [];
function zwSet(changes) { Object.assign(zwave.progress, changes); }
function zwAfter(ms, f) { zwTimers.push(setTimeout(f, ms)); }
function zwClear() { zwTimers.splice(0).forEach(clearTimeout); }
function zwStart(op, changes = {}) {
  zwClear();
  Object.assign(zwave.progress, {op, step: 'starting', result: 'none', seconds_left: 0, requested: 0, csa: false, dsk: '', node: 0,
    generation: 0, security: 0, s2_failure: 'ok', setup_done: 0, setup_total: 0, asleep: false, stalled: false, model: '', zones: [],
    replacing: false, routes_done: 0, routes_total: 0, routes_failed: 0, routes_skipped: 0, routes_waiting: 0,
    routes_stopped: false, routes_list: [], fw_done: 0, fw_total: 0, fw_why: 0, fw_error: 0}, changes);
}
// The radio's own update (network.md §14): 13 blocks, one every 400 ms, then back as 2.30.
// A file named with "refuse" in it is refused by the bootloader (error 0x18).
function zwRadioUpdate(refuse) {
  zwStart('radio_update', {step: 'working', fw_total: 13});
  zwave.radio.state = 'updating';
  for (let i = 1; i <= 13; i++) zwAfter(400 * i, () => zwSet({fw_done: i}));
  if (refuse) {
    zwClear();
    zwAfter(800, () => { zwave.radio.state = 'working'; zwDone('fw_failed', {fw_done: 2, fw_why: 4, fw_error: 0x18}); });
    return;
  }
  zwAfter(400 * 13 + 600, () => {
    zwave.radio.state = 'working';
    zwave.radio.firmware = '2.30';
    zwave.radio.sdk = '7.24.2';
    zwave.radio.software = {check: 'none', version: '', words: ''};
    zwDone('fw_updated', {fw_done: 13});
  });
}
function zwListen(then) {
  zwSet({step: 'listening', seconds_left: 60});
  const tick = () => { if (zwave.progress.step === 'listening' && zwave.progress.seconds_left > 0) { zwave.progress.seconds_left--; zwAfter(1000, tick); } };
  zwAfter(1000, tick);
  zwAfter(4000, then);
}
function zwDone(result, changes = {}) { zwClear(); zwSet({step: 'done', result, seconds_left: 0, ...changes}); }
function zwOperation(body) {
  const g = zwave.progress, a = body.action;
  // what web_zwave.c refuses before the panel is asked
  if (a === 'allow' && !(Number.isInteger(body.keys) && body.keys >= 1 && body.keys <= 7)) return 'bad_request';
  if (a === 'pin' && !(Number.isInteger(body.pin) && body.pin >= 0 && body.pin <= 65535)) return 'bad_request';
  if (['remove_failed', 'ping', 'setup'].includes(a) && !(Number.isInteger(body.node) && body.node > 0 && Number.isInteger(body.generation))) {
    return 'bad_request';
  }
  const busy = g.op !== 'none' && g.step !== 'done';
  const dev = zwave.devices.find(d => d.node === body.node && d.generation === body.generation);
  if (a === 'dismiss') { if (g.step === 'done') zwStart('none', {step: 'idle'}); return 'ok'; }
  if (a === 'stop') { if (!busy) return 'invalid'; zwDone(g.step === 'reading' ? 'setup_paused' : 'stopped'); return 'ok'; }
  if (a === 'keep_listening') { if (g.step !== 'listening') return 'invalid'; g.seconds_left = 60; return 'ok'; }
  if (a === 'allow') {
    if (g.step !== 'ask_allow' || !body.keys) return 'invalid';
    g.granted = body.keys;
    zwSet({step: 'ask_pin', seconds_left: 240, dsk: 'xxxxx-40311-11111-22222-33333-44444-55555-60000'});
    return 'ok';
  }
  if (a === 'insecure') {
    if (g.step !== 'ask_allow' && g.step !== 'ask_pin') return 'invalid';
    zwSet({step: 'reading', setup_done: 1, setup_total: 16, security: 1, s2_failure: 'cancelled'});
    zwAfter(3000, () => { zwave.devices.push({node: 6, generation: 5, model: 'Motion sensor', kind: 1, security: 1, state: 0, unreachable: false,
      listening: false, heard_s: 1, battery_low: false, zones: []}); zwDone('added_insecure', {node: 6, generation: 5}); });
    return 'ok';
  }
  if (a === 'pin') {
    if (g.step !== 'ask_pin' || !Number.isInteger(body.pin)) return 'invalid';
    if (body.pin !== 12345) { zwDone('added_insecure', {node: 6, generation: 5, s2_failure: 'wrong_pin', security: 7}); return 'ok'; }
    zwSet({step: 'securing', seconds_left: 0});
    zwAfter(1500, () => zwSet({step: 'reading', setup_done: 3, setup_total: 16, security: g.granted & 4 ? 5 : 4}));
    zwAfter(3000, () => zwSet({setup_done: 11}));
    zwAfter(4500, () => {
      zwave.devices.push({node: 6, generation: 5, model: 'Zooz ZSE41 open/close sensor', kind: 1, security: g.security, state: 0,
        unreachable: false, listening: false, heard_s: 1, battery: 100, battery_low: false, zones: []});
      zwave.radio.devices = zwave.devices.length;
      zwDone('added', {node: 6, generation: 5, model: 'Zooz ZSE41 open/close sensor'});
    });
    return 'ok';
  }
  if (busy) return 'busy';
  if (a === 'add' && zwave.radio.storage_full) return 'full'; // ZW-039 (zwz_panel_request)
  if (a === 'add') {
    zwStart('add');
    zwListen(() => {
      zwSet({step: 'connecting', node: 6, generation: 5});
      zwAfter(1200, () => zwSet({step: 'ask_allow', requested: 0x06, seconds_left: 240}));
    });
    return 'ok';
  }
  if (a === 'remove') {
    zwStart('remove');
    zwListen(() => {
      const gone = zwave.devices.find(d => d.node === 3);
      if (!gone) return zwDone('removed_foreign');
      zwave.devices.splice(zwave.devices.indexOf(gone), 1);
      zwave.radio.devices = zwave.devices.length;
      zwDone('removed', {node: 3, generation: 2, model: 'Switch', zones: []});
    });
    return 'ok';
  }
  if (a === 'restart') { zwStart('restart', {step: 'working'}); zwAfter(2500, () => zwDone('done')); return 'ok'; }
  if (a === 'radio_check') {
    zwave.radio.software = {check: 'checking', version: '', words: ''};
    zwAfter(1200, () => {
      zwave.radio.software = zwave.radio.firmware === '2.30' ? {check: 'up_to_date', version: '', words: ''}
        : {check: 'available', version: '2.30', words: 'Z-Wave SDK 7.24.2.'};
    });
    return 'ok';
  }
  if (a === 'radio_install') {
    if (zwave.radio.software.check !== 'available') return 'invalid';
    zwave.radio.software = {check: 'downloading', version: '2.30', words: ''};
    zwAfter(1000, () => { zwave.radio.software = {check: 'none', version: '', words: ''}; zwRadioUpdate(false); });
    return 'ok';
  }
  if (a === 'routes') {
    const n = body.node ? 1 : zwave.devices.length;
    zwStart('routes', {step: 'working', routes_total: n, node: body.node || 0, generation: body.generation || 0});
    for (let i = 1; i <= n; i++) zwAfter(1500 * i, () => zwSet({routes_done: i}));
    zwAfter(1500 * n + 200, () => zwDone('done'));
    return 'ok';
  }
  if (!dev) return 'invalid';
  if (a === 'ping') {
    if (!dev.listening) return 'invalid';
    zwStart('ping', {step: 'working', node: dev.node, generation: dev.generation});
    zwAfter(1200, () => zwDone(dev.unreachable ? 'no_answer' : 'answered'));
    return 'ok';
  }
  if (a === 'remove_failed') {
    zwStart('remove_failed', {step: 'working', node: dev.node, generation: dev.generation, model: dev.model});
    zwAfter(2000, () => {
      if (!dev.unreachable) return zwDone('still_answers');
      zwave.devices.splice(zwave.devices.indexOf(dev), 1);
      zwave.radio.devices = zwave.devices.length;
      zwDone('removed', {zones: dev.zones});
    });
    return 'ok';
  }
  // BL-152: a stopped setup continues, a finished one is read again
  if (a === 'setup') { if (![0, 3, 5].includes(dev.state)) return 'invalid'; dev.state = 1; zwAfter(3000, () => { dev.state = 0; }); return 'ok'; }
  return 'bad_request';
}
// ---- Zigbee (docs/features/20 devices.md §14, ZB-025; the pages BL-145 draws) ---------------------
// What web_zigbee.c answers from the zigbee service: the radio, the devices (rows, and a device's
// page with ?device=<id>), the operation running and the install codes waiting, in the same JSON
// names. Fictional devices and addresses. MOCK_ZIGBEE=0: a panel without Zigbee; MOCK_ZIGBEE_RADIO
// picks the radio's state (paused, not_connected, socket_zwave...).
const zigbeeOn = process.env.MOCK_ZIGBEE !== '0';
const ZB_ROW_KEYS = ['id', 'name', 'model', 'state', 'kind', 'power', 'router', 'battery', 'low', 'heard_s', 'signal', 'lqi', 'rssi',
  'unreachable', 'device_class', 'join', 'setup_done', 'setup_total', 'zones'];
function zbDevice(id, o) {
  return {id, name: '', model: '', state: 'ready', kind: 'sensor', power: 'battery', router: false, battery: 100, low: false, heard_s: 120,
    signal: 'good', lqi: 190, rssi: -58, unreachable: false, device_class: 1, join: 'standard_key', setup_done: 5, setup_total: 5, zones: [],
    ieee: '02ABCD00000000' + String(id).padStart(2, '0'), nwk: 0x1A00 + id, manufacturer: 'LUMI', model_id: 'lumi.sensor_magnet', sw_build: '3.0.1',
    joined_at: Math.floor(Date.now() / 1000) - 86400 * id, sleepy: true, check_in_s: 3600, setup_step: 7, can_identify: false,
    wake_words: '', add_words: '', note: '', inputs: [], ...o};
}
const zigbee = {
  radio: {state: process.env.MOCK_ZIGBEE_RADIO || 'working', paused: process.env.MOCK_ZIGBEE_RADIO === 'paused' ? 'no_answer' : '', jammed: false,
    devices: 0, not_heard: 0, codes_waiting: 0, learn_s: 0, channel: 15, pan: '1A62', ext_pan: '02ABCD00000000FF', ieee: '02ABCD00000000FF',
    version: '1.4.1', noise: -92, survey: [{channel: 11, dbm: -71}, {channel: 15, dbm: -86}, {channel: 20, dbm: -97}, {channel: 25, dbm: -95}],
    quietest: 20, restarts: process.env.MOCK_ZIGBEE_RADIO === 'paused' ? 2 : 1, storage_full: false},
  devices: [
    zbDevice(1, {model: 'Aqara door sensor', battery: 92, join: 'install_code', zones: [5],
      inputs: [{endpoint: 1, device_class: 1, words: 'door contact', state: 'Closed', zone: 5}]}),
    zbDevice(2, {model: 'Aqara motion sensor', battery: 18, low: true, lqi: 112, rssi: -74, signal: 'fair', heard_s: 300, device_class: 4,
      zones: [4], model_id: 'lumi.sensor_motion', inputs: [{endpoint: 1, device_class: 4, words: 'motion', state: 'No motion', zone: 4}]}),
    zbDevice(3, {model: 'Heiman smoke and CO alarm', manufacturer: 'HEIMAN', model_id: 'SMOK_V16', lqi: 205, rssi: -55, heard_s: 60,
      device_class: 6, zones: [9, 10], inputs: [{endpoint: 1, device_class: 6, words: 'smoke', state: 'Clear', zone: 9},
        {endpoint: 2, device_class: 8, words: 'endpoint 2 · carbon monoxide', state: 'Clear', zone: 10}]}),
    zbDevice(4, {model: 'Aqara water sensor', battery: 64, lqi: 44, rssi: -88, signal: 'weak', heard_s: 9000, unreachable: true, device_class: 10,
      zones: [17], model_id: 'lumi.sensor_wleak.aq1', inputs: [{endpoint: 1, device_class: 10, words: 'water leak', state: '', zone: 17}]}),
    zbDevice(5, {model: 'Aqara motion sensor', state: 'wake_needed', setup_done: 4, lqi: 120, rssi: -70, signal: 'fair', heard_s: 20,
      device_class: 4, setup_step: 4, wake_words: 'Press its button once, then again every few seconds until it says Ready.'}),
    zbDevice(6, {name: 'Hall siren', model: 'Heiman siren', manufacturer: 'HEIMAN', model_id: 'WarningDevice', kind: 'siren', power: 'mains',
      router: true, battery: undefined, sleepy: false, check_in_s: 0, lqi: 215, rssi: -52, heard_s: 30, join: 'install_code', device_class: 0,
      can_identify: true, siren: {fit: 'available', partitions: 1, burglary: true, fire: true, strobe: true, squawk: false, volume: 2,
        supervision_min: 0, live: true, last_test: 'none'}}),
    zbDevice(7, {name: 'Entry keypad', model: 'Develco keypad', manufacturer: 'frient A/S', model_id: 'KEPZB-110', kind: 'keypad', battery: 81,
      lqi: 170, rssi: -63, heard_s: 600, join: 'install_code', device_class: 0,
      keypad: {fit: 'available', partition: 1, panic_mask: 1, show_state: true, device: 17, live: true}}),
    zbDevice(8, {name: 'Alex’s key fob', model: 'Aqara key fob', kind: 'keyfob', battery: 97, lqi: 160, rssi: -66, heard_s: 3600,
      device_class: 14, fob: {fit: 'available', user: 2, partition: 1, may_disarm: false, panic: false, live: true}}),
  ],
  codes: [{ieee: '02ABCD0000000101', added: Math.floor(Date.now() / 1000) - 660}, {ieee: '02ABCD0000000102', added: 0}],
  progress: null,
  next: 9,
};
const zbIdle = () => ({op: 'none', step: 'idle', result: 'none', seconds: 0, id: 0, old_id: 0, model: '', joined: [], zones: []});
zigbee.progress = zbIdle();
const zbTimers = [];
const zbAfter = (ms, f) => zbTimers.push(setTimeout(f, ms));
const zbClear = () => zbTimers.splice(0).forEach(clearTimeout);
const zbRunning = () => !['none'].includes(zigbee.progress.op) && zigbee.progress.step !== 'done';
const zbDone = (result, changes = {}) => { zbClear(); Object.assign(zigbee.progress, {step: 'done', result, seconds: 0, ...changes}); };
function zbStart(op, changes = {}) { zbClear(); zigbee.progress = {...zbIdle(), op, step: 'working', ...changes}; }
// The install code's CRC-16/X.25 over the rest, low byte first (zbz_codes.c).
function zbCode(text) {
  const hex = String(text || '').replace(/[\s:-]/g, '').toUpperCase();
  if (!/^[0-9A-F]+$/.test(hex) || ![16, 20, 28, 36].includes(hex.length)) return 'bad_code';
  const bytes = hex.match(/../g).map(h => parseInt(h, 16)), body = bytes.slice(0, -2);
  let c = 0xFFFF;
  for (const b of body) { c ^= b; for (let i = 0; i < 8; i++) c = (c & 1) ? (c >>> 1) ^ 0x8408 : c >>> 1; }
  return ((c ^ 0xFFFF) & 0xFFFF) === (bytes[bytes.length - 2] | (bytes[bytes.length - 1] << 8)) ? '' : 'checksum';
}
const zbAddress = text => { const hex = String(text || '').replace(/[\s:-]/g, '').toUpperCase(); return /^[0-9A-F]{16}$/.test(hex) && !/^0+$/.test(hex) ? hex : ''; };
// Listening: the window counts down once a second; a pretend device joins and is set up on the way.
function zbListen(op, changes = {}) {
  zbStart(op, {step: 'listening', seconds: 180, ...changes});
  const g = zigbee.progress;
  const tick = () => { if (g === zigbee.progress && g.step === 'listening' && g.seconds > 0) { g.seconds--; zbAfter(1000, tick); if (!g.seconds) zbDone(g.joined.length ? 'added' : 'nothing_joined'); } };
  zbAfter(1000, tick);
  const id = zigbee.next++;
  const row = {id, step: 'joining', result: 'none', asleep: false, model: op === 'add_code' ? 'Heiman door sensor' : 'Aqara door sensor'};
  zbAfter(2000, () => g.joined.push(row));
  zbAfter(3000, () => { row.step = 'reading'; });
  zbAfter(4000, () => { row.step = 'setting_up'; });
  zbAfter(5000, () => {
    Object.assign(row, {step: 'done', result: 'added'});
    const old = op === 'replace' ? zigbee.devices.find(d => d.id === g.old_id) : null;
    zigbee.devices.push(zbDevice(id, {model: row.model, join: op === 'add_code' ? 'install_code' : 'standard_key', heard_s: 1, zones: old ? old.zones : []}));
    if (old) { zigbee.devices.splice(zigbee.devices.indexOf(old), 1); zbDone('replaced', {id, zones: old.zones}); }
  });
}
function zbOperation(b) {
  const g = zigbee.progress, r = zigbee.radio, usable = r.state === 'working';
  const dev = zigbee.devices.find(d => d.id === b.id);
  const needsId = ['remove', 'forget', 'replace', 'ping', 'identify', 'setup', 'rename', 'siren', 'siren_test', 'keypad', 'fob'];
  if (needsId.includes(b.op) && !dev) return 'not_found';
  if (b.op === 'dismiss') { if (g.step === 'done') zigbee.progress = zbIdle(); return 'ok'; }
  if (b.op === 'stop') { if (g.step !== 'listening') return 'invalid'; zbDone('stopped'); return 'ok'; }
  if (b.op === 'keep') { if (g.step !== 'listening') return 'invalid'; g.seconds = 180; return 'ok'; }
  if (b.op === 'rename') { if (typeof b.name !== 'string' || Buffer.byteLength(b.name) > 23) return 'invalid'; dev.name = b.name; return 'ok'; }
  if (b.op === 'code_forget') {
    const ieee = zbAddress(b.ieee);
    if (!ieee) return 'bad_address';
    const at = zigbee.codes.findIndex(c => c.ieee === ieee);
    if (at < 0) return 'not_found';
    zigbee.codes.splice(at, 1);
    return 'ok';
  }
  if (b.op === 'siren' || b.op === 'keypad' || b.op === 'fob') {
    const now = dev[b.op], v = b[b.op];
    if (!now || now.fit !== 'available' || !v || typeof v !== 'object') return 'invalid';
    if (b.op === 'siren' && (v.supervision_min !== 0 && !(v.supervision_min >= 60))) return 'invalid';
    if (b.op === 'fob' && v.user && zigbee.devices.some(d => d !== dev && d.fob?.user === v.user)) return 'in_use';
    Object.assign(now, v);
    return 'ok';
  }
  if (zbRunning() && b.op !== 'reset') return 'busy';
  if (['add', 'add_code', 'code_add', 'replace', 'remove', 'ping', 'identify', 'setup', 'survey', 'channel', 'siren_test'].includes(b.op) && !usable) return 'no_radio';
  if (b.op === 'add') { zbListen('add'); return 'ok'; }
  if (b.op === 'add_code' || b.op === 'code_add') {
    const ieee = zbAddress(b.ieee);
    if (!ieee) return 'bad_address';
    const bad = zbCode(b.code);
    if (bad) return bad;
    if (r.storage_full || zigbee.codes.length >= 16) return 'full';
    if (!zigbee.codes.some(c => c.ieee === ieee)) zigbee.codes.push({ieee, added: Math.floor(Date.now() / 1000)});
    if (b.op === 'add_code') zbListen('add_code');
    return 'ok';
  }
  if (b.op === 'replace') { zbListen('replace', {old_id: dev.id, model: dev.model}); return 'ok'; }
  if (b.op === 'remove') {
    zbStart('remove', {id: dev.id, model: dev.model, zones: dev.zones});
    zbAfter(2000, () => {
      if (dev.unreachable) return zbDone('no_answer');
      zigbee.devices.splice(zigbee.devices.indexOf(dev), 1);
      zbDone('removed');
    });
    return 'ok';
  }
  if (b.op === 'forget') {
    zigbee.devices.splice(zigbee.devices.indexOf(dev), 1);
    zbStart('forget', {id: dev.id, model: dev.model, zones: dev.zones});
    zbDone('forgotten');
    return 'ok';
  }
  if (b.op === 'ping') { zbStart('ping', {id: dev.id, model: dev.model}); zbAfter(1000, () => zbDone(dev.unreachable ? 'no_answer' : 'answered')); return 'ok'; }
  if (b.op === 'identify' || b.op === 'setup') { zbStart(b.op, {id: dev.id, model: dev.model}); zbAfter(1000, () => zbDone('done')); return 'ok'; }
  if (b.op === 'siren_test') {
    if (!dev.siren) return 'invalid';
    zbStart('siren_test', {id: dev.id, model: dev.model});
    zbAfter(1000, () => { dev.siren.last_test = 'done'; zbDone('done'); });
    return 'ok';
  }
  if (b.op === 'survey') { zbStart('survey'); zbAfter(1000, () => zbDone('done')); return 'ok'; }
  if (b.op === 'channel') {
    if (![11, 15, 20, 25].includes(b.channel)) return 'invalid';
    zbStart('channel');
    zbAfter(1000, () => { r.channel = b.channel; zbDone('done'); });
    return 'ok';
  }
  if (b.op === 'reset') {
    if (zbRunning()) return 'busy';
    if (r.state === 'not_connected' || r.state === 'socket_zwave') return 'no_radio';
    zbStart('reset');
    zbAfter(1000, () => { zigbee.devices.splice(0); Object.assign(r, {pan: '3C07', ext_pan: 'A10E775BC209D46F'}); zbDone('done'); });
    return 'ok';
  }
  if (b.op === 'restart') {
    if (r.state === 'not_connected' || r.state === 'socket_zwave') return 'no_radio';
    zbStart('restart');
    r.state = 'starting';
    zbAfter(1500, () => { r.state = 'working'; zbDone('done'); });
    return 'ok';
  }
  if (b.op === 'retry') {
    if (r.state !== 'paused') return 'invalid';
    Object.assign(r, {state: 'working', paused: '', restarts: 0});
    return 'ok';
  }
  return 'invalid';
}
function zigbeeGet(query) {
  const r = zigbee.radio;
  Object.assign(r, {devices: zigbee.devices.length, not_heard: zigbee.devices.filter(d => d.unreachable).length,
    codes_waiting: zigbee.codes.length, learn_s: zigbee.progress.step === 'listening' ? zigbee.progress.seconds : 0});
  const row = d => Object.fromEntries(ZB_ROW_KEYS.filter(k => d[k] !== undefined).map(k => [k, d[k]]));
  const out = {available: true, may_change: !isLocked(), why: isLocked() ? 'armed' : '', radio: r, devices: zigbee.devices.map(row),
    progress: zigbee.progress, codes: zigbee.codes};
  if (!query.has('device')) return [200, out];
  const d = zigbee.devices.find(x => x.id === Number(query.get('device')));
  return d ? [200, {...out, device: d}] : [404, {error: 'not_found'}];
}
const boot = crypto.randomBytes(32).toString('hex');
const sessions = new Map();

// ---- The pretend panel -----------------------------------------------------------------
const field = (name, type, extra = {}) => ({name, type, maxLength: 0, min: 0, max: type === 'boolean' ? 1 : 0, ...extra});
const num = (name, min, max) => field(name, 'number', {min, max});
const text = (name, size) => field(name, 'text', {maxLength: size - 1});
const secret = (name, size) => field(name, 'password', {maxLength: size - 1});
const bool = name => field(name, 'boolean');
const ZONE_TYPE_NAMES = {0: 'not_used', 1: 'entry_exit_1', 2: 'entry_exit_2', 3: 'perimeter', 4: 'interior_follower', 5: 'trouble_day_alarm_night',
  6: '24h_silent', 7: '24h_audible', 8: '24h_aux', 9: 'fire', 10: 'interior_delay', 12: 'monitor', 14: 'carbon_monoxide', 16: 'fire_verified',
  20: 'arm_stay', 21: 'arm_away', 22: 'disarm', 23: 'no_response', 24: 'silent_burglary', 29: 'momentary_exit', 77: 'keyswitch'};
// The panel's languages (English until a translation ships), regions and clock choices
// (web_config.c prefs_choices).
const LANGUAGES = [{value: 'en-US', label: 'English'}];
// The Panel Manager's catalogs (docs/features/37 I18N-031). A panel makes /i18n/<locale>.json
// from its own tables; this pretend panel serves the files in MOCK_I18N=<folder> instead
// (build/i18n/pseudo: the test languages, from tools/i18n.py pseudo --build), in the same form:
// keyed by the 32-bit FNV-1a hash of the English, as panel-words.js looks them up. Each is a
// language of the pretend panel too.
const CATALOG_DIRS = [process.env.MOCK_I18N].filter(Boolean);
function wordKey(ctx, text) {
  let h = 0x811c9dc5;
  const add = bytes => { for (const b of bytes) h = Math.imul(h ^ b, 0x01000193) >>> 0; };
  if (ctx) { add(Buffer.from(ctx, 'utf8')); add([4]); }
  add(Buffer.from(text, 'utf8'));
  return h.toString(16).padStart(8, '0');
}
for (const dir of CATALOG_DIRS) {
  for (const name of fs.existsSync(dir) ? fs.readdirSync(dir) : []) {
    if (!name.endsWith('.json')) continue;
    const doc = JSON.parse(fs.readFileSync(path.join(dir, name), 'utf8'));
    if (!LANGUAGES.some(l => l.value === doc.locale))
      LANGUAGES.push({value: doc.locale, label: doc.entries?.[wordKey('this language', 'English')] || doc.locale});
  }
}
function catalogFile(locale) {
  for (const dir of CATALOG_DIRS) {
    const file = path.join(dir, locale + '.json');
    if (/^[A-Za-z]{2,3}(-[A-Za-z0-9]{2,8})*$/.test(locale) && fs.existsSync(file)) return file;
  }
  return null;
}
const REGIONS = [['', 'The language\'s'], ['AE', 'United Arab Emirates'], ['AU', 'Australia'], ['BR', 'Brazil'], ['CA', 'Canada'],
  ['CN', 'China'], ['DE', 'Germany'], ['EG', 'Egypt'], ['ES', 'Spain'], ['FR', 'France'], ['GB', 'United Kingdom'], ['IL', 'Israel'],
  ['IN', 'India'], ['IT', 'Italy'], ['JP', 'Japan'], ['KR', 'South Korea'], ['MX', 'Mexico'], ['NL', 'Netherlands'], ['PT', 'Portugal'],
  ['SA', 'Saudi Arabia'], ['TW', 'Taiwan'], ['US', 'United States']].map(([value, label]) => ({value, label}));
const CLOCKS = [{value: 0, label: 'The region\'s'}, {value: 1, label: '12-hour'}, {value: 2, label: '24-hour'}];
const APPEARANCES = [{value: 0, label: 'Dark'}, {value: 1, label: 'Light'}, {value: 2, label: 'Automatic'}];
const DEVICE_CLASS_NAMES = ['generic', 'door', 'window', 'garage_door', 'motion', 'glass_break', 'smoke', 'heat', 'carbon_monoxide', 'gas',
  'water', 'temperature', 'vibration', 'panic_button', 'keyfob', 'keyswitch'];
const SCHEMA = {
  options: [num('code_min_len', 4, 10), num('abort_window_s', 0, 45), num('cancel_window_s', 0, 3600), num('bell_timeout_s', 0, 1800),
    bool('fire_bell_timeout'), num('swinger_trips', 1, 15), num('cross_zone_window_s', 10, 900), num('fire_verify_window_s', 30, 300),
    num('smoke_reset_s', 1, 30), num('exit_restart_limit', 0, 3), num('recent_closing_s', 0, 600), bool('auto_stay'), bool('exit_sounds'),
    bool('quick_arm'), bool('quick_exit'), bool('remote_arm_without_code'), bool('auto_less_secure'), bool('power_up_previous_state'), num('power_up_settle_s', 0, 300),
    num('lockout_attempts', 0, 10), num('lockout_s', 60, 1800), num('walk_test_timeout_s', 600, 14400), num('trouble_beep_mask', 0, 4294967295),
    bool('relaxed_limits'), bool('power_up_program'), bool('chime_needs_code')],
  partitions: [bool('enabled'), text('name', 24), num('exit_delay_s', 0, 600), num('entry_delay1_s', 0, 600), num('entry_delay2_s', 0, 600)],
  zones: [{...num('type', 0, 255), choices: Object.entries(ZONE_TYPE_NAMES).map(([v, l]) => ({value: Number(v), label: l}))}, num('partition', 1, 4),
    num('flags', 0, 65535), {...num('device_class', 0, 255), choices: DEVICE_CLASS_NAMES.map((l, v) => ({value: v, label: l}))}, num('user', 0, 64),
    num('swinger_trips', 0, 6), num('cross_zone', 0, 128), num('supervision_s', 0, 65535), text('name', 24)],
  devices: [bool('enabled'), num('partition', 0, 4), num('supervision_s', 0, 65535), text('name', 24)],
};
const HAS_VOICE = process.env.MOCK_VOICE !== 'absent'; // a firmware with a voice lists its volume (AUD-032)
const CONFIG_SCHEMA = {
  mqtt: [bool('enabled'), text('host', 64), num('port', 0, 65535), text('user', 64), secret('password', 64), num('tls', 0, 2), bool('discovery'),
    bool('commands'), bool('allow_panic'), text('prefix', 32), text('panel_id', 32)],
  ntfy: [bool('enabled'), bool('self_hosted'), text('server', 96), secret('topic', 64), secret('token', 64), num('cats', 0, 255), bool('silent_too')],
  pushover: [bool('enabled'), secret('user_key', 32), secret('app_token', 32), num('cats', 0, 255), bool('silent_too'), bool('emergency'), text('sound', 24)],
  network: [bool('use_static'), text('ip', 16), text('mask', 16), text('gateway', 16), text('dns1', 16), text('dns2', 16)],
  display: [num('brightness', 10, 100), num('saver_brightness', 0, 50), num('saver_after_ms', 0, 86400000),
    num('off_after_ms', 0, 86400000), bool('night_on'), num('night_start', 0, 1439), num('night_end', 0, 1439),
    num('night_brightness', 10, 100), num('night_off_after_ms', 0, 86400000),
    {...num('clock_mode', 0, 2), choices: CLOCKS}, num('volume', 0, 100), num('chime_volume', 0, 100), ...(HAS_VOICE ? [num('voice_volume', 0, 100)] : []), bool('key_click'), {...text('lang', 16), choices: LANGUAGES},
    {...text('region', 4), choices: REGIONS}, {...num('appearance', 0, 2), choices: APPEARANCES}, bool('high_contrast'),
    bool('voice_enabled'), bool('voice_status'), bool('voice_chime')], // the Announcements switches (D06; the walk test has none, WALK-016)
};
const config = {
  mqtt: {enabled: true, host: 'homeassistant.local', port: 1883, user: 'alarm', password: 'x', tls: 0, discovery: true, commands: false, allow_panic: false, prefix: 'roboalarms', panel_id: '30eda0123456'},
  ntfy: {enabled: false, self_hosted: false, server: '', topic: '', token: '', cats: 7, silent_too: false},
  pushover: {enabled: true, user_key: 'u', app_token: 'a', cats: 3, silent_too: false, emergency: true, sound: ''},
  network: {use_static: false, ip: '', mask: '', gateway: '', dns1: '', dns2: ''},
  display: {brightness: 55, saver_brightness: 10, saver_after_ms: 120000, off_after_ms: 0, night_on: false, night_start: 1320,
    night_end: 420, night_brightness: 20, night_off_after_ms: 60000, clock_mode: 0, volume: 70, chime_volume: 70, ...(HAS_VOICE ? {voice_volume: 70} : {}), key_click: true, lang: 'en-US', region: '',
    appearance: 0, high_contrast: false,
    voice_enabled: true, voice_status: true, voice_chime: true}, // D06: on, as modern panels (decided 2026-10-12)
};
const alarm = {
  revision: 41,
  options: {code_min_len: 4, abort_window_s: 30, cancel_window_s: 300, bell_timeout_s: 240, fire_bell_timeout: false, swinger_trips: 2,
    cross_zone_window_s: 300, fire_verify_window_s: 90, smoke_reset_s: 6, exit_restart_limit: 1, recent_closing_s: 120, auto_stay: true,
    exit_sounds: true, quick_arm: false, quick_exit: true, remote_arm_without_code: false, auto_less_secure: true, power_up_previous_state: true, power_up_settle_s: 60,
    lockout_attempts: 5, lockout_s: 300, walk_test_timeout_s: 3600, trouble_beep_mask: 0x3ff, relaxed_limits: false, power_up_program: true,
    chime_needs_code: true},
  panic_keys: [0, 3, 2],
  partitions: [1, 2, 3, 4].map(n => ({number: n, enabled: n === 1, name: n === 1 ? 'Home' : '', exit_delay_s: 60, entry_delay1_s: 30, entry_delay2_s: 45})),
  zones: Array.from({length: 128}, (_, i) => ({number: i + 1, type: 0, partition: 1, flags: 0, device_class: 0, user: 0, swinger_trips: 0,
    cross_zone: 0, supervision_s: 0, name: ''})),
  devices: Array.from({length: 16}, (_, i) => ({number: i + 1, enabled: false, partition: 0, supervision_s: 0, name: ''})),
};
[[1, 'Front door', 1, 1, 1], [2, 'Garage door', 2, 3, 1], [3, 'Kitchen window', 3, 2, 1], [4, 'Hallway motion', 4, 4, 0], [6, 'Living room motion', 4, 4, 2],
  [5, 'Back door', 3, 1, 1], [9, 'Hallway smoke', 9, 6, 0], [10, 'Basement CO', 14, 8, 0], [17, 'Water heater leak', 8, 10, 0],
  [18, 'Keyfob', 21, 14, 0]]
  .forEach(([n, name, type, cls, flags]) => Object.assign(alarm.zones[n - 1], {name, type, device_class: cls, flags}));
Object.assign(alarm.devices[0], {enabled: true, name: 'Panel keypad', partition: 0, supervision_s: 0});
// BL-048: service-backed keypad fixtures. Physical pairing events are test-only.
const keypadState = {available: true, capacity: 8, busy: false, keypads: [
  {number: 1, name: 'Hall keypad', home: 1, allowed: true, never: false, seconds: 30, online: true, firmware: '0.7.4', update_state: 1, update_percent: 100}],
  ...(process.env.MOCK_HEADLESS ? {} : {local: {home: 1, allowed: false, never: false, seconds: 30}})};
Object.assign(alarm.devices[0], {name: 'Hall keypad', partition: 1});
let keypadAttemptOwner = 0;
function keypadGet(user) {
  const result = JSON.parse(JSON.stringify(keypadState));
  if (keypadAttemptOwner !== user) delete result.enrollment;
  return result;
}
function keypadPost(user, b) {
  const e = keypadState.enrollment;
  if (b.op === 'start') {
    if (!/^[a-f0-9]{32}$/.test(b.attempt || '')) return 'Invalid attempt';
    if (e?.attempt === b.attempt && keypadAttemptOwner === user) return '';
    if (keypadState.busy) return 'Pairing is busy';
    if (!alarm.devices.some(d => d.number <= 8 && !d.enabled)) return 'Keypad capacity reached';
    keypadState.enrollment = {attempt: b.attempt, state: 1, fail: 0, code: '', peer: '', device: 0, left_s: 120, sequence: 0};
    keypadAttemptOwner = user; keypadState.busy = true; return '';
  }
  if (['cancel', 'confirm'].includes(b.op)) {
    if (!e || e.attempt !== b.attempt || keypadAttemptOwner !== user) return 'Pairing attempt changed; reload';
    if (!keypadState.busy) return '';
    if (b.op === 'cancel') { e.state = 6; e.fail = 10; keypadState.busy = false; return ''; }
    if (b.sequence !== e.sequence || b.code !== e.code || !e.code || typeof b.allow !== 'boolean') return 'Pairing code changed; compare again';
    if (e.state === 4) { if (!b.allow) { e.state = 6; e.fail = 10; keypadState.busy = false; } return ''; }
    if (e.state !== 3) return 'Pairing is not ready';
    e.state = b.allow ? 4 : 6; e.fail = b.allow ? 0 : 3;
    if (!b.allow) keypadState.busy = false;
    if (b.allow && e.keypadConfirmed) keypadEvent('confirmed');
    return '';
  }
  if (keypadState.busy) return 'Finish pairing first';
  const k = b.device === 0 ? keypadState.local : keypadState.keypads.find(k => k.number === b.device);
  if (!k) return 'Keypad no longer enrolled';
  if (b.op === 'remove') {
    if (!b.device) return 'The built-in keypad cannot be removed';
    keypadState.keypads.splice(keypadState.keypads.indexOf(k), 1); alarm.devices[b.device - 1].enabled = false; return '';
  }
  if (b.op !== 'save' || !alarm.partitions[b.home - 1]?.enabled || !Number.isInteger(b.seconds) || b.seconds < 1 || b.seconds > 3600 || typeof b.allowed !== 'boolean' || typeof b.never !== 'boolean') return 'Invalid keypad policy';
  Object.assign(k, {name: b.name, home: b.home, allowed: b.allowed, seconds: b.seconds, never: b.never});
  if (b.device) { k.name = b.name; Object.assign(alarm.devices[b.device - 1], {name: b.name, partition: b.home}); }
  return '';
}
function keypadEvent(event) {
  const e = keypadState.enrollment; if (!e || !keypadState.busy) return;
  if (event === 'compare') Object.assign(e, {state: 3, code: '588460', peer: 'New keypad', sequence: e.sequence + 1});
  else if (event === 'confirmed') {
    e.keypadConfirmed = true;
    if (e.state !== 4) return;
    const d = alarm.devices.find(d => !d.enabled && d.number <= 8);
    if (!d) { e.state = 6; e.fail = 6; keypadState.busy = false; return; }
    Object.assign(d, {enabled: true, name: 'Keypad ' + d.number, partition: 1});
    keypadState.keypads.push({number: d.number, name: d.name, home: 1, allowed: false, never: false, seconds: 30, online: true});
    Object.assign(e, {state: 5, device: d.number}); keypadState.busy = false;
  } else { Object.assign(e, {state: 6, fail: event === 'overlap' ? 7 : 2}); keypadState.busy = false; }
}
// MOCK_PARTITIONS=2: a second partition in use (the garage), for the tests of who sees which.
if (process.env.MOCK_PARTITIONS === '2') Object.assign(alarm.partitions[1], {enabled: true, name: 'Garage'});
// ---- Automations (docs/features/41 DESIGN.md section 10, AUT-001..AUT-016; web_automations.c) ---------
// The panel's own automations: Harbor House's, seeded as the simulator seeds them (host/ui_preview/host_panel.c
// auto_seed: uids and slots 1..4, the same sentences), its devices (what device_service_list and hc_auto_offer
// give), and the three endpoints with the firmware's gen, refusals and access. The zones are the alarm's own, as
// the firmware reads them. MOCK_AUTOMATIONS=0: a build without the feature (404, no capability); =empty, =full
// (16), =clock (the clock isn't set), =readonly (the store can't be written), =newer (slot 5 made by newer software),
// =arm (phase C: the arming and disarming automations of the touchscreen mockup screens/automations.js, slots 5..10).
const automationsMode = process.env.MOCK_AUTOMATIONS || '';
const automationsOn = automationsMode !== '0';
const AU_MAX = 16;
const AU_DAYS = ['mon', 'tue', 'wed', 'thu', 'fri', 'sat', 'sun'];
const AU_TRIGGERS = ['armed', 'disarmed', 'alarm', 'fire', 'zone_opened', 'zone_closed', 'time', 'trouble'];
// BL-224 (docs/features/41 DESIGN.md section 15): the troubles a trigger may name, the wired outputs "For automations"
// (outputs_service_auto_list: number, name, GPIO), the kinds of action that need no device.
const AU_TROUBLES = ['any', 'ac', 'battery', 'sensor', 'siren'];
const auOutputs = process.env.MOCK_AUTO_OUTPUTS === '0' ? [] : [{number: 1, name: 'Garden Lights', gpio: 47}];
const AU_PANEL = ['output', 'remind', 'still_armed', 'chime', 'notify'];
const AU_TIME_ONLY = ['remind', 'still_armed'];
const AU_ACTIONS = ['on', 'off', 'toggle', 'lock', 'unlock', 'close', 'open', 'stop', 'run'];
// hc_auto_allowed with the option on, per kind (the server's words: a thermostat is "climate", its "off" a mode).
const AU_OFFER = {light: ['on', 'off', 'toggle'], fan: ['on', 'off'], switch: ['on', 'off', 'toggle'], lock: ['lock', 'unlock'],
  cover: ['close', 'open', 'stop'], valve: ['close', 'open'], climate: ['off'], scene: ['run'], script: []};
const AU_LESS = (kind, action) => ['unlock', 'open'].includes(action) || (kind === 'cover' && action === 'stop'); // hc_auto_less_secure
const AU_CONFIRM = (kind, action) => (kind === 'scene' && action === 'run') || (kind === 'switch' && ['on', 'toggle'].includes(action)); // D8, D9
const AU_OUTSIDE = ['zone_opened', 'zone_closed', 'alarm', 'fire']; // D24: triggers someone outside may cause
// The fictional house's devices (DESIGN.md section 10's list; the simulator's refs for the four it seeds). A script is
// in the registry but never offered (AUT-002); the garage door isn't answering.
const auHouseAll = [
  ['light.porch', 'Porch Light', 'Outside', 'light'], ['light.hallway', 'Hallway Light', 'Downstairs', 'light'],
  ['switch.living_lamp', 'Living Room Lamp', 'Living Room', 'switch'], ['switch.christmas', 'Christmas Lights', 'Outside', 'switch'],
  ['lock.front_door', 'Front Door', 'Entry', 'lock'], ['cover.garage_door', 'Garage Door', 'Garage', 'cover', false],
  ['valve.water_main', 'Water Main', 'Utility', 'valve'], ['climate.hallway', 'Hallway', 'Downstairs', 'climate'],
  ['scene.good_night', 'Good Night', '', 'scene'], ['script.movie_night', 'Movie Night', 'Living Room', 'script'],
].map(([ref, name, area, kind, online = true]) => ({source: 'ha', ref, name, area, kind, online}));
const auHouse = process.env.MOCK_AUTO_DEVICES === '0' ? [] : auHouseAll;
const auLessSecure = () => alarm.options.auto_less_secure !== false; // opt.auto_less_secure (AUT-015), on by default
const auDev = (source, ref) => auHouse.find(d => d.source === source && d.ref === ref);
const auZoneClass = z => DEVICE_CLASS_NAMES[z.device_class] || 'generic';
// auto_zone_usable(): the types that may start one, then the classes (a close only for doors and windows).
const AU_TYPES = [1, 2, 3, 4, 5, 8, 10, 12, 23];
function auUsable(z, close) {
  if (!AU_TYPES.includes(z.type)) return false;
  const c = auZoneClass(z);
  if (['door', 'window', 'garage_door'].includes(c)) return true;
  return !close && ['generic', 'motion', 'glass_break', 'water', 'temperature', 'vibration'].includes(c);
}
const auAt = (h, m) => new Date().setHours(h, m, 0, 0);
const auWhen = (kind, extra = {}) => ({kind, partitions: [], levels: [], zone: 0, minute: 0, days: [], window: null, ...extra});
const auThen = (ref, action, extra = {}) => ({source: 'ha', ref, name: auHouseAll.find(d => d.ref === ref).name, action, off_after: 0, confirmed: false, ...extra});
const auStatusNone = () => ({result: 'none', tried: false, at: null, action: null, device: null, reason: null});
// Phase C (AUT-011): an arm or a disarm, as web_automations.c sends it; made_by is the name of who saved it (D30).
const auArm = (level, warn_min, made_by, extra = {}) => ({kind: 'arm', partition: 1, level, warn_min, bypass: false, made_by, ...extra});
const auDisarm = (made_by, extra = {}) => ({kind: 'disarm', partition: 1, confirmed: false, made_by, ...extra});
const auAlarmAct = a => a.kind === 'arm' || a.kind === 'disarm';
// The actions that name a partition (auto_model.c partition_action).
const auPartAct = a => auAlarmAct(a) || ['remind', 'still_armed', 'chime'].includes(a.kind);
const automations = {gen: 1, nextUid: 5, rules: []};
function auSeed() {
  const rules = [
    // "When Front door opens · Turn on Porch Light for 10 minutes", 6:00 PM to 6:00 AM
    {on: true, when: auWhen('zone_opened', {zone: 1, window: {from: 1080, to: 360}}), then: [auThen('light.porch', 'on', {off_after: 10})],
      status: {...auStatusNone(), result: 'ran', at: auAt(19, 42), action: 'on', device: 'Porch Light'}},
    // "When the panel is armed Away · Lock Front Door · Run Good Night" (the scene confirmed when picked)
    {on: true, when: auWhen('armed', {levels: ['away']}), then: [auThen('lock.front_door', 'lock'), auThen('scene.good_night', 'run', {confirmed: true})],
      status: {...auStatusNone(), result: 'failed', at: auAt(8, 2), action: 'lock', device: 'Front Door', reason: 'no_answer'}},
    // "When a fire or CO alarm sounds · Turn off Hallway" (the thermostat's "off" mode)
    {on: true, when: auWhen('fire'), then: [auThen('climate.hallway', 'off')], status: auStatusNone()},
    // "At 11:00 PM on weekdays · Close Garage Door", turned off
    {on: false, when: auWhen('time', {minute: 1380, days: AU_DAYS.slice(0, 5)}), then: [auThen('cover.garage_door', 'close')], status: auStatusNone()},
  ].map((r, i) => ({slot: i + 1, uid: i + 1, record: 'ok', ...r}));
  if (automationsMode === 'empty') rules.length = 0;
  if (automationsMode === 'arm') { // the drawing's statuses: a zone open, already armed, put off, no warning, refused, ran
    const weekdays = AU_DAYS.slice(0, 5), kitchen = alarm.zones[2].name;
    [{on: true, when: auWhen('time', {minute: 1350, days: [...AU_DAYS]}), then: [auArm('night', 5, 'Alex')],
      status: {...auStatusNone(), result: 'skipped', at: auAt(22, 30), reason: 'not_ready', zone: kitchen}},
    {on: true, when: auWhen('time', {minute: 1350, days: weekdays}), then: [auArm('stay', 5, 'Alex')], status: {...auStatusNone(), result: 'already', at: auAt(22, 30)}},
    {on: true, when: auWhen('time', {minute: 1350, days: ['sat', 'sun']}), then: [auArm('away', 10, 'Alex', {bypass: true})],
      status: {...auStatusNone(), result: 'put_off', at: auAt(22, 25), by: 'Pat', choice: 'hour', until: auAt(23, 30)}},
    {on: true, when: auWhen('time', {minute: 420, days: weekdays}), then: [auArm('away', 15, 'Alex')],
      status: {...auStatusNone(), result: 'skipped', at: auAt(7, 0), reason: 'no_warning'}},
    {on: true, when: auWhen('time', {minute: 1320, days: [...AU_DAYS]}), then: [auArm('night', 5, 'Pat')],
      status: {...auStatusNone(), result: 'failed', at: auAt(22, 0), reason: 'arm_refused', refused: 'not_authorized'}},
    // D23's example, a shop that opens at 10 AM for a worker
    {on: true, when: auWhen('time', {minute: 600, days: weekdays}), then: [auDisarm('Alex'), auThen('lock.front_door', 'unlock')],
      status: {...auStatusNone(), result: 'ran', at: auAt(10, 0)}},
    ].forEach(r => rules.push({slot: rules.length + 1, uid: rules.length + 1, record: 'ok', ...r}));
  }
  if (automationsMode === 'newer') rules.push({slot: 5, uid: 5, on: true, record: 'newer', when: null, then: [], status: auStatusNone()});
  if (process.env.MOCK_AUTO_REMIND) // BL-224: "At 11:00 PM every day · Remind to arm", which found Home disarmed
    rules.push({slot: rules.length + 1, uid: rules.length + 1, on: true, record: 'ok', when: auWhen('time', {minute: 1380, days: [...AU_DAYS]}),
      then: [{kind: 'remind', partition: 1, notify: true}], status: {...auStatusNone(), result: 'ran', at: auAt(23, 0)}});
  if (automationsMode === 'full') {
    for (let slot = rules.length + 1; slot <= AU_MAX; slot++)
      rules.push({slot, uid: slot, on: true, record: 'ok', when: auWhen('zone_opened', {zone: 3}), then: [auThen('light.hallway', 'on', {off_after: slot})], status: auStatusNone()});
  }
  automations.rules = rules;
  automations.nextUid = rules.length + 1;
}
if (automationsOn) auSeed();
// The arming warnings and holds (automation_service_warnings): automation 5's, when MOCK_AUTO_WARN says so. Night at
// the next quarter hour at least 3 minutes away; put off an hour: that time plus an hour.
const auWarnings = [];
function auSetWarning(state, by = 'Pat') {
  auWarnings.length = 0;
  if (!['on', 'hour', 'night', 'day'].includes(state)) return;
  const d = new Date(Date.now() + 3 * 60e3);
  d.setMinutes(Math.ceil(d.getMinutes() / 15) * 15, 0, 0);
  const at = Math.floor(d.getTime() / 1000) + (state === 'hour' ? 3600 : 0);
  auWarnings.push({uid: 5, partition: 1, level: 'night', at, state, by: state === 'on' ? null : by, bypass: false});
}
if (automationsOn) auSetWarning(process.env.MOCK_AUTO_WARN || '');
// The reminders' notes (automation_service_reminders, BL-224): the seeded reminder's, when MOCK_AUTO_REMIND says so.
const auReminders = [];
function auSetReminder(on) {
  auReminders.length = 0;
  const r = automations.rules.find(x => x.then.some(a => a.kind === 'remind'));
  if (on && r) auReminders.push({uid: r.uid, partition: r.then[0].partition, minute: r.when.minute});
}
if (automationsOn) auSetReminder(!!process.env.MOCK_AUTO_REMIND);
const auRemindersFor = numbers => auClone(auReminders.filter(r => numbers.includes(r.partition)));
// The phone services set up (net_notify_services): on, with what each needs to send, whatever its categories say.
const auNotify = () => process.env.MOCK_AUTO_NOTIFY === '0' ? [] : [config.ntfy.enabled && config.ntfy.topic ? 'ntfy' : '',
  config.pushover.enabled && config.pushover.user_key && config.pushover.app_token ? 'pushover' : ''].filter(Boolean);
const auMaster = () => nameOf(2); // ALARM_USER_MASTER: whom an installer's Arm acts as (BL-224)
const auIsInstaller = s => users.find(u => u.number === s.user)?.authority === 1;
// The ones on the partitions this person sees (status's automation_warnings; GET automations has them all).
const auWarningsFor = numbers => auClone(auWarnings.filter(w => numbers.includes(w.partition)));
const auClone = x => JSON.parse(JSON.stringify(x));
// GET automations: a record other than ok sends when: null, then: [] (AUT-008).
function automationsJson() {
  const zones = alarm.zones.filter(z => z.type).map(z => ({number: z.number, name: z.name, class: auZoneClass(z), partition: z.partition,
    usable_open: auUsable(z, false), usable_close: auUsable(z, true)}));
  const rows = automations.rules.map(r => {
    const out = {slot: r.slot, uid: r.uid, on: r.on, record: r.record, status: auClone(r.status)};
    if (r.record !== 'ok') return {...out, when: null, then: []};
    const when = {...auClone(r.when), zone_name: alarm.zones[r.when.zone - 1]?.name || ''};
    const then = r.then.map(a => {
      if (a.kind === 'arm') return {saved_by: null, ...auClone(a)}; // BL-224: who set it up when that isn't its maker
      if (auAlarmAct(a) || AU_PANEL.includes(a.kind)) return auClone(a);
      const d = auDev(a.source, a.ref);
      return {kind: 'device', source: a.source, ref: a.ref, device: d ? d.name : a.name, area: d ? d.area : '', found: !!d, online: !!d?.online,
        action: a.action, off_after: a.off_after, confirmed: a.confirmed};
    });
    return {...out, when, then};
  });
  return {maximum: AU_MAX, used: automations.rules.length, gen: automations.gen, clock_set: automationsMode !== 'clock',
    read_only: automationsMode === 'readonly', less_secure: auLessSecure(), automations: rows, zones,
    partitions: alarm.partitions.filter(p => p.enabled).map(p => ({number: p.number, name: p.name})),
    outputs: auClone(auOutputs), notify: auNotify(), master: auMaster(),
    warnings: auWarningsFor(alarm.partitions.map(p => p.number)), reminders: auRemindersFor(alarm.partitions.map(p => p.number))};
}
// GET automations/devices: only devices with an offered action; Unlock, Open and Stop only with the option on (D25).
function automationDevicesJson() {
  const less = auLessSecure();
  return {devices: auHouse.map(d => ({...d, actions: (AU_OFFER[d.kind] || []).filter(a => less || !AU_LESS(d.kind, a))
    .map(action => ({action, confirm: AU_CONFIRM(d.kind, action)}))})).filter(d => d.actions.length)};
}
const auError = (code, error, index = -1) => [code, index >= 0 ? {error, index} : {error}];
const auInt = (v, lo, hi) => Number.isInteger(v) && v >= lo && v <= hi;
// auto_rule_check's order (auto_model.c): incomplete, not allowed, less secure off (a disarm and "Bypass open zones"
// too, D31), the arm fields, arm alone, one alarm action, zone, partition, undoes arming, time, window, needs confirm,
// same device. As web_automations.c maps them: a partition not in use is incomplete.
const auLessAct = a => a.kind === 'disarm' || (a.kind === 'device' && AU_LESS(auDev(a.source, a.ref)?.kind, a.action)); // auto_action_less_secure
// auto_action_needs_trigger_confirm (D24, D31): a less secure action after a door, window, motion or alarm trigger;
// after an armed trigger an unlock, an open or a stop (a disarm there is refused, undoes_arming).
const auNeedsTriggerConfirm = (kind, a) => AU_OUTSIDE.includes(kind) ? auLessAct(a) : kind === 'armed' && a.kind === 'device' && auLessAct(a);
// byInstaller: the saver is the installer, who never disarms (BL-187) and whose Arm acts as the system master (BL-224).
function auCheck(r, byInstaller = false) {
  const w = r.when, zoneTrig = ['zone_opened', 'zone_closed'].includes(w.kind);
  if (!r.then.length || (zoneTrig && !w.zone)) return ['incomplete'];
  const kindOf = a => auDev(a.source, a.ref)?.kind;
  let i = r.then.findIndex(a => a.kind === 'device' && (!(AU_OFFER[kindOf(a)] || []).includes(a.action) || (a.off_after && a.action !== 'on') ||
    (AU_CONFIRM(kindOf(a), a.action) && !a.confirmed)));
  if (i >= 0) return ['action_not_allowed', i];
  i = r.then.findIndex(a => a.kind === 'disarm');
  if (i >= 0 && byInstaller) return ['action_not_allowed', i];
  i = r.then.findIndex(a => auLessAct(a) || a.bypass);
  if (i >= 0 && !auLessSecure()) return ['less_secure_off', i];
  if (r.then.some(a => a.kind === 'arm' && !(['away', 'stay', 'night'].includes(a.level) && auInt(a.warn_min, 1, 15)))) return ['incomplete'];
  i = r.then.findIndex(a => AU_TIME_ONLY.includes(a.kind)); // BL-224: they look at the panel at their time
  if (i >= 0 && w.kind !== 'time') return ['needs_time', i];
  if (r.then.some(a => a.kind === 'arm' || AU_TIME_ONLY.includes(a.kind)) && r.then.length !== 1) return ['arm_alone'];
  if (r.then.filter(auAlarmAct).length > 1) return ['same_device', 1];
  if (zoneTrig) {
    const z = alarm.zones[w.zone - 1];
    if (!z || !z.type || !auUsable(z, w.kind === 'zone_closed')) return ['bad_zone'];
  }
  const enabled = n => !!alarm.partitions.find(p => p.number === n && p.enabled);
  if (!w.partitions.every(enabled) || r.then.some(a => auPartAct(a) && !enabled(a.partition))) return ['incomplete'];
  i = r.then.findIndex(a => a.kind === 'disarm' && w.kind === 'armed' && (!w.partitions.length || w.partitions.includes(a.partition)));
  if (i >= 0) return ['undoes_arming', i];
  if (w.window && (w.kind === 'time' || w.window.from === w.window.to)) return ['incomplete'];
  i = r.then.findIndex(a => a.kind === 'output' && !auOutputs.some(o => o.number === a.output)); // BL-224: "For automations" only
  if (i >= 0) return ['bad_output', i];
  i = r.then.findIndex(a => auNeedsTriggerConfirm(w.kind, a) && !a.confirmed);
  if (i >= 0) return ['needs_confirm', i];
  if (r.then.length === 2 && r.then.every(a => a.kind === 'device') && r.then[0].source === r.then[1].source && r.then[0].ref === r.then[1].ref) return ['same_device', 1];
  const [x, y] = r.then; // one chime, one notification, one output each (AUT-007)
  if (y && x.kind === y.kind && (['chime', 'notify'].includes(x.kind) || (x.kind === 'output' && x.output === y.output))) return ['same_device', 1];
  return null;
}
// POST automations: {action: save|switch|delete|try, slot, gen, automation} (web_automations_post).
function automationsPost(s, body) {
  const S = automations;
  if (typeof body.action !== 'string' || !auInt(body.slot ?? 0, 0, AU_MAX) || !auInt(body.gen, 0, 65535)) return auError(400, 'bad_request');
  const slot = body.slot || 0;
  if (!['save', 'switch', 'delete', 'try'].includes(body.action) || (body.action !== 'save' && !slot)) return auError(400, 'bad_request');
  const at = S.rules.find(r => r.slot === slot);
  const done = (result, extra = {}) => { S.gen = (S.gen + 1) & 0xffff; return [200, {result, gen: S.gen, ...extra}]; };
  if (body.action === 'save') {
    const a = body.automation;
    if (!a || typeof a !== 'object') return auError(400, 'bad_request');
    if (slot && !at) return auError(409, 'changed');
    if (at && at.record !== 'ok') return auError(409, 'newer');
    if (a.on !== undefined && a.on !== null && typeof a.on !== 'boolean') return auError(400, 'bad_request');
    const w = a.when;
    if (!w || typeof w !== 'object' || (w.kind !== undefined && w.kind !== 'none' && !AU_TRIGGERS.includes(w.kind))) return auError(400, 'bad_request');
    if (!Array.isArray(a.then) || a.then.length > 2) return auError(400, 'bad_request');
    const days = Array.isArray(w.days) ? w.days.filter(d => AU_DAYS.includes(d)) : [];
    if (w.kind === 'time' && !auInt(w.minute, 0, 1439)) return auError(400, 'bad_request');
    if (w.kind === 'trouble' && w.trouble !== undefined && w.trouble !== null && !AU_TROUBLES.includes(w.trouble)) return auError(400, 'bad_request');
    if (w.window && !(auInt(w.window.from, 0, 1439) && auInt(w.window.to, 0, 1439))) return auError(400, 'bad_request');
    const then = [];
    let actErr = null;
    for (const x of a.then) {
      if (x && (x.kind === 'arm' || x.kind === 'disarm')) { // parse_alarm_action: its own fields; made_by is never read
        const arm = x.kind === 'arm', bool = v => v === undefined || v === null || typeof v === 'boolean';
        if (!auInt(x.partition, 1, 4) || !bool(x.confirmed) || !bool(x.bypass) || (!arm && x.bypass === true) ||
          (arm && (!['away', 'stay', 'night'].includes(x.level) || !auInt(x.warn_min, 0, 255) || x.confirmed === true))) return auError(400, 'bad_request');
        // the installer's Arm acts as the system master; saved_by keeps who set it up (BL-224)
        const installer = auIsInstaller(s);
        then.push(arm ? auArm(x.level, x.warn_min, installer ? auMaster() : nameOf(s.user), {partition: x.partition, bypass: x.bypass === true, saved_by: installer ? nameOf(s.user) : null})
          : auDisarm(nameOf(s.user), {partition: x.partition, confirmed: x.confirmed === true}));
        continue;
      }
      if (x && AU_PANEL.includes(x.kind)) { // parse_panel_action: its own fields, in the model's ranges (BL-224)
        const bool = (v, need = false) => need ? typeof v === 'boolean' : v === undefined || v === null || typeof v === 'boolean';
        if (x.kind === 'output') {
          if (!auInt(x.output, 1, 4) || !['on', 'off'].includes(x.action) || !auInt(x.off_after ?? 0, 0, 1440) || (x.off_after && x.action !== 'on')) return auError(400, 'bad_request');
          then.push({kind: 'output', output: x.output, action: x.action, off_after: x.off_after || 0});
        } else if (x.kind === 'notify') then.push({kind: 'notify'});
        else {
          if (!auInt(x.partition, 1, 4) || (x.kind === 'remind' && !bool(x.notify)) || (x.kind === 'chime' && !bool(x.on, true))) return auError(400, 'bad_request');
          then.push(x.kind === 'remind' ? {kind: 'remind', partition: x.partition, notify: x.notify === true}
            : x.kind === 'chime' ? {kind: 'chime', partition: x.partition, on: x.on} : {kind: 'still_armed', partition: x.partition});
        }
        continue;
      }
      if (!x || x.kind !== 'device' || !['ha', 'zigbee', 'zwave'].includes(x.source) || typeof x.ref !== 'string' || !x.ref.length ||
        Buffer.byteLength(x.ref) > 95 || !AU_ACTIONS.includes(x.action) || !auInt(x.off_after ?? 0, 0, 1440)) return auError(400, 'bad_request');
      if (!auDev(x.source, x.ref) && !actErr) actErr = ['missing_device', then.length];
      then.push({kind: 'device', source: x.source, ref: x.ref, name: auDev(x.source, x.ref)?.name || '', action: x.action,
        off_after: x.action === 'on' ? x.off_after || 0 : 0, confirmed: x.confirmed === true});
    }
    if (!w.kind || w.kind === 'none' || !then.length || (w.kind === 'time' && !days.length)) return auError(409, 'incomplete');
    if (actErr) return auError(409, ...actErr);
    if (body.gen !== S.gen) return auError(409, 'changed'); // the store's gen: another edit came first
    if (automationsMode === 'readonly') return auError(409, 'read_only');
    if (!at && S.rules.length >= AU_MAX) return auError(409, 'full');
    const rule = {slot: at ? at.slot : 0, uid: at ? at.uid : S.nextUid, on: a.on !== false, record: 'ok',
      when: {kind: w.kind, partitions: (w.partitions || []).filter(p => auInt(p, 1, 4)), levels: (w.levels || []).filter(l => ['away', 'stay', 'night'].includes(l)),
        zone: Number(w.zone) || 0, minute: w.kind === 'time' ? w.minute : 0, days: w.kind === 'time' ? days : [], window: w.window || null,
        ...(w.kind === 'trouble' ? {trouble: w.trouble || 'any'} : {})},
      then, status: at ? at.status : auStatusNone()};
    const why = auCheck(rule, auIsInstaller(s));
    if (why) return auError(409, ...why);
    if (at) S.rules[S.rules.indexOf(at)] = rule;
    else {
      for (let n = 1; n <= AU_MAX; n++) if (!S.rules.some(r => r.slot === n)) { rule.slot = n; break; } // the first free slot
      S.nextUid++;
      S.rules.push(rule);
      S.rules.sort((x, y) => x.slot - y.slot);
    }
    return done('saved', {slot: rule.slot});
  }
  if (body.action === 'delete') { // by slot: a newer or broken record too (AUT-008)
    if (body.gen !== S.gen || !at) return auError(409, 'changed');
    if (automationsMode === 'readonly') return auError(409, 'read_only');
    S.rules.splice(S.rules.indexOf(at), 1);
    return done('deleted');
  }
  if (!at) return auError(409, 'changed');
  if (body.action === 'switch') {
    const on = typeof body.on === 'boolean' ? body.on : body.automation?.on;
    if (typeof on !== 'boolean') return auError(400, 'bad_request');
    if (at.record !== 'ok') return auError(409, 'newer');
    if (body.gen !== S.gen) return auError(409, 'changed');
    if (automationsMode === 'readonly') return auError(409, 'read_only');
    at.on = on;
    return done('switched');
  }
  // try: Try it now runs it once as the runner would with the option on, and says why it can't (AUT-009)
  if (body.gen !== S.gen) return auError(409, 'changed');
  if (at.record !== 'ok') return auError(409, 'newer');
  if (at.then.some(a => auAlarmAct(a) || AU_TIME_ONLY.includes(a.kind))) return auError(409, 'action_not_allowed', 0); // nothing arms without its warning
  if (at.then.some(a => AU_PANEL.includes(a.kind))) { // BL-224: an output, the chime, a notification run at once
    const note = at.then.findIndex(a => a.kind === 'notify');
    const out = at.then.find(a => a.kind === 'output');
    at.status = note >= 0 && !auNotify().length ? {...auStatusNone(), result: 'skipped', tried: true, at: Date.now(), action: null, device: null, reason: 'no_service'}
      : {...auStatusNone(), result: 'tried', tried: true, at: Date.now(), action: out ? out.action : null, device: out ? auOutputs.find(o => o.number === out.output)?.name || '' : null};
    return [200, {result: 'tried', gen: S.gen, status: auClone(at.status)}];
  }
  const missing = at.then.findIndex(a => !auDev(a.source, a.ref));
  if (missing >= 0) return auError(409, 'missing_device', missing);
  if (!auLessSecure() && at.then.some(a => AU_LESS(auDev(a.source, a.ref).kind, a.action))) { // nothing was sent (D25)
    at.status = {...auStatusNone(), result: 'skipped', tried: true, at: Date.now(), reason: 'less_secure_off'};
    return [200, {result: 'tried', gen: S.gen, status: auClone(at.status)}];
  }
  const off = at.then.find(a => !auDev(a.source, a.ref).online);
  const first = off || at.then[0];
  at.status = {result: off ? 'failed' : 'tried', tried: true, at: Date.now(), action: first.action, device: auDev(first.source, first.ref).name,
    reason: off ? 'unreachable' : null};
  return [200, {result: 'tried', gen: S.gen, status: auClone(at.status)}];
}
const users = [{number: 1, name: 'Installer', authority: 1, partitions: 15}, {number: 2, name: 'Alex', authority: 2, partitions: 15},
  {number: 3, name: 'Sam', authority: 4, partitions: 1}, {number: 4, name: 'Dog walker', authority: 6, partitions: 1},
  {number: 5, name: 'Pat', authority: 3, partitions: 1}];
const webLogins = new Set([1, 2, 3, 5]); // users with a browser sign-in
const authorityOf = n => AUTHORITY[users.find(u => u.number === n)?.authority] || 'none';
// alarm_config_user_manages(): the engine's rule for who manages whom.
function manages(actor, target) {
  const a = users.find(u => u.number === actor), t = users.find(u => u.number === target);
  if (!a) return false;
  if (a.authority === 1) return true;
  if (a.authority === 2) return target !== 1;
  if (a.authority === 3) return target === actor || (target > 2 && (!t || t.authority >= 4) && ((t?.partitions || 0) & ~a.partitions) === 0);
  return false;
}
const loginPossible = u => u.number === 1 ? u.authority === 1 : [2, 3, 4].includes(u.authority);
const now = Date.now();
const events = [
  ['Disarmed', 'By Alex · keypad', 2, 1, 0.3], ['Front Door', 'Opened while disarmed', 2, 3, 0.35], ['Armed Away', 'By Sam', 2, 3, 9],
  ['Trouble: Water heater leak', 'Sensor battery low', 1, 2, 20], ['Trouble cleared', 'Water heater leak battery replaced', 1, 1, 26],
  ['Burglary alarm', 'Back door', 0, 7, 50], ['Alarm canceled', 'Disarmed by Alex', 0, 1, 50.1], ['Settings changed', 'By Installer from the browser', 4, 0, 70],
  ['Panel started', 'Firmware 0.6.0', 4, 0, 71],
].map(([title, body, category, accent, hours], i) => ({title, body, category, accent, index: 400 - i, boot: 12, uptime_s: 3600,
  epoch_ms: now - hours * 3600e3, partition: 1, ...({Disarmed: {event: 'disarmed', zone: 0, user: 2}, 'Armed Away': {event: 'armed', zone: 0, user: 3},
    'Burglary alarm': {event: 'alarm', zone: 5, user: 0}}[title] || {event: 'x', zone: 0, user: 0})}));
events.push({title: 'Panel started', body: 'Before the clock was set', category: 4, accent: 0, index: 380, boot: 11, uptime_s: 4, epoch_ms: 0, event: 'system_start'});
const bindings = new Map([[1, {kind: 2, input: 1, id: '0'}], [2, {kind: 2, input: 2, id: '0'}], [3, {kind: 4, input: 0, id: '0', ref: 'binary_sensor.kitchen_window'}],
  [4, {kind: 3, input: 1, id: '9015763223311', ref: ''}], [9, {kind: 2, input: 5, id: '0'}]]);
// ---- Arming (BL-113): a small copy of what the engine does for one partition, on timers. -----
// The rules follow alarm_core (arming.c, config.c): who may act, what blocks arming, what a
// bypass needs. MOCK_FAST=1 runs the delays 20 times faster for tests.
const FAST = process.env.MOCK_FAST ? 20 : 1;
const timers = new Set();
const later = (ms, f) => { const t = setTimeout(() => { timers.delete(t); f(); }, ms / FAST); timers.add(t); return t; };
const stopTimers = () => { timers.forEach(clearTimeout); timers.clear(); };
process.on('exit', stopTimers);
const LEVELS = ['away', 'stay', 'night'];
const LEVEL_WORDS = {away: 'Away', stay: 'Stay', night: 'Night'};
const BURGLARY = [1, 2, 3, 4, 5, 10, 24], H24_BYPASSABLE = [6, 7, 8], ALWAYS_ON = [6, 7, 8, 9, 12, 14, 16], PERIMETER = [1, 2, 3, 24];
const zoneCfg = n => alarm.zones[n - 1];
// alarm_zone_armed_in_level / alarm_zone_bypassable / the entry time a zone starts
const armedIn = (z, level) => PERIMETER.includes(z.type) || ([4, 5, 10].includes(z.type) && (level === 'away' || (level === 'night' && !!(z.flags & 2))));
const bypassable = z => BURGLARY.includes(z.type) || (H24_BYPASSABLE.includes(z.type) && !!(z.flags & 4));
const entrySeconds = z => {
  const p = alarm.partitions[z.partition - 1];
  return [1, 10].includes(z.type) ? p.entry_delay1_s : z.type === 2 ? p.entry_delay2_s : 0;
};
// What the sensors say (missed: no check-in, unreachable), and the panel's own troubles (status.troubles)
// keypads: the ones not responding; restored: system troubles that ended and wait for an acknowledgement.
const live = {open: new Set(), lowbat: new Set(), missed: new Set(), troubles: 0, keypads: new Set(), restored: 0};
const parts = [1, 2, 3, 4].map(number => ({number, state: 'disarmed', level: 'none', no_entry_delay: false, silent_exit: false, armed_by: 0,
  deadline: 0, timer: null, chime: false, alarm: false, alarm_kind: '', siren: false, memory: [], canceled: false, bypass: new Set(), trouble_unack: false}));
// GET troubles (docs/features/31 BL-137): what the touchscreen lists, as the firmware builds it. since_ms is the
// time of the newest matching trouble event in the history, 0 when there is none.
const SYSTEM_TROUBLES = ['Mains power lost', 'Panel battery low', 'Reports not getting through', 'Network down', 'Clock not set',
  'Wi-Fi module not responding', 'Sensor radio problem', 'Siren problem', 'Panel tamper', 'Storage problem'];
const ZONE_TROUBLE_WORDS = ['Sensor fault', 'Tamper', 'Low battery', 'Not reporting', 'Open during the day'];
function troubleList(s) {
  const since = match => events.find(e => e.event === 'trouble' && match(e))?.epoch_ms || 0;
  const mine = new Set(viewerPartitions(s).map(p => p.number)); // zones only in the viewer's partitions, as the firmware
  const out = [];
  for (let kind = 0; kind < SYSTEM_TROUBLES.length; kind++)
    for (const [bits, restored] of [[live.troubles, false], [live.restored, true]])
      if (bits & (1 << kind)) out.push({family: 'system', kind, number: 0, restored, since_ms: since(e => !e.zone && e.title === SYSTEM_TROUBLES[kind])});
  for (const n of [...live.keypads].sort((a, b) => a - b))
    out.push({family: 'keypad', kind: 240, number: n, restored: false, since_ms: since(e => e.title === `Keypad ${n} not responding`)});
  for (const z of alarm.zones.filter(x => x.type && mine.has(x.partition)))
    for (const [set, kind] of [[live.lowbat, 2], [live.missed, 3]])
      if (set.has(z.number)) out.push({family: 'zone', kind, number: z.number, restored: false,
        since_ms: since(e => e.zone === z.number && e.title.endsWith(ZONE_TROUBLE_WORDS[kind]))});
  return out;
}
// status.trouble_sig and GET troubles' sig: any value that moves when a line comes, goes or is restored
const troubleSig = list => list.map(x => `${x.family},${x.kind},${x.number},${x.restored ? 1 : 0};`).join('')
  .split('').reduce((h, c) => Math.imul(h ^ c.charCodeAt(0), 16777619) >>> 0, 2166136261);
const nameOf = n => users.find(u => u.number === n)?.name || '';
const blockersFor = (p, level) => alarm.zones.filter(z => z.type && z.partition === p.number && live.open.has(z.number) && armedIn(z, level) && !p.bypass.has(z.number));
const zoneFlags = z => (live.open.has(z.number) ? 1 : 0) | (parts[z.partition - 1].bypass.has(z.number) ? 2 : 0) |
  (parts[z.partition - 1].memory.includes(z.number) ? 4 : 0) | (live.lowbat.has(z.number) ? 0x20 : 0) | (live.missed.has(z.number) ? 0x40 : 0);
const zoneLive = n => ({zone: n, flags: zoneFlags(zoneCfg(n)), trouble: (live.lowbat.has(n) ? 4 : 0) | (live.missed.has(n) ? 8 : 0), bound: bindings.has(n),
  known: bindings.has(n) && n !== 4, active: live.open.has(n), unreachable: live.missed.has(n), check: live.lowbat.has(n) || live.missed.has(n), heard_at_ms: 0});
let eventIndex = 500;
// A history entry as the panel writes it (newest first); by is where it came from ("keypad", "Panel Manager").
function addEvent(event, title, body, category, accent, p, zone, user, epoch = Date.now()) {
  events.unshift({title, body, category, accent, index: eventIndex++, boot: 12, uptime_s: 3600, epoch_ms: epoch, event, zone, partition: p.number, user});
}
function tripZone(p, zone) {
  p.state = 'armed';
  p.deadline = 0;
  if (!p.alarm) p.alarm_at = Date.now();
  p.alarm = p.siren = true;
  p.alarm_kind = 'burglary';
  if (!p.memory.includes(zone)) p.memory.push(zone);
  addEvent('alarm', 'Burglary alarm', zoneCfg(zone).name, 0, 7, p, zone, 0);
}
function beginExit(p, ms) {
  p.state = 'exit_delay';
  p.deadline = Date.now() + ms / FAST;
  p.timer = later(ms, () => { p.state = 'armed'; p.deadline = 0; });
}
function beginEntry(p, zone) {
  const ms = (entrySeconds(zoneCfg(zone)) || alarm.partitions[p.number - 1].entry_delay1_s) * 1000;
  p.state = 'entry_delay';
  p.deadline = Date.now() + ms / FAST;
  p.timer = later(ms, () => tripZone(p, zone));
}
// The siren stops and the alarm ends into its memory (the engine's eng_silence).
const silence = p => Object.assign(p, {alarm: false, alarm_kind: '', siren: false});
function disarm(p, by, source) {
  if (p.timer) { clearTimeout(p.timer); timers.delete(p.timer); p.timer = null; }
  if (p.state === 'disarmed') { // code + OFF while disarmed: silences an alarm, else ends a walk test, else clears the memory
    if (p.alarm) silence(p);
    else if (walks[p.number - 1].running) walkEnd(p, by, 'disarm'); // WALK-025
    else { p.memory = []; p.canceled = false; p.trouble_unack = false; live.restored = 0; } // and acknowledges the troubles (arming.c)
    return;
  }
  // Disarmed inside the abort window: the report never went out (the engine's ALARM_MEM_CANCELED)
  if (p.alarm && Date.now() - (p.alarm_at || 0) < ABORT_WINDOW_MS / FAST) p.canceled = true;
  silence(p);
  Object.assign(p, {state: 'disarmed', level: 'none', no_entry_delay: false, silent_exit: false, armed_by: 0, deadline: 0});
  p.bypass.clear();
  addEvent('disarmed', 'Disarmed', `By ${nameOf(by)} · ${source}`, 2, 1, p, 0, by);
}
function arm(p, level, body, by, source) {
  p.memory = [];
  p.canceled = false;
  Object.assign(p, {level, no_entry_delay: !!body.no_entry_delay, silent_exit: level === 'away' && !!body.silent_exit, armed_by: by});
  addEvent('armed', `Armed ${LEVEL_WORDS[level]}`, `By ${nameOf(by)} · ${source}`, 2, 3, p, 0, by);
  const seconds = alarm.partitions[p.number - 1].exit_delay_s * (p.silent_exit ? 2 : 1);
  if (seconds) beginExit(p, seconds * 1000);
  else p.state = 'armed';
}
// The engine's authority rules (can_arm, can_disarm, can_bypass); the chime is any user's.
// USR-001: the installer neither arms nor disarms; ending a walk test they started stays theirs (WALK-025).
const CAN_DISARM = ['master', 'partition_master', 'standard', 'guest'];
const CAN_BYPASS = ['installer', 'master', 'partition_master', 'standard'];
const viewerPartitions = s => alarm.partitions.filter(p => p.enabled && (ACC.system(s.authority) ||
  (users.find(u => u.number === s.user)?.partitions & (1 << (p.number - 1)))));
// ---- The walk test (docs/features/40): alarm_core's walk.c in small, per partition. ------------
// can_test(): who starts and extends one (WALK-001); whoever may disarm the partition ends it (WALK-025).
const CAN_TEST = ['installer', 'master', 'partition_master'];
// alarm_zone_type_walk_counts(): fire and CO stay live; 24-hour zones only with "Include panic and 24-hour zones".
const walkCounts = (type, life24) => [1, 2, 3, 4, 5, 10, 24, 23, 12, 29, 20, 21, 22, 77].includes(type) || (life24 && [6, 7, 8].includes(type));
const walks = parts.map(() => ({running: false, until: 0, started_at: 0, started_by: 0, started_from: '', life24: false, seq: 0,
  tested: 0, total: 0, ended_by: 0, end_how: '', limit: null, steps: []}));
const marks = new Map(); // zone -> {tested, cover, how, first_s, trips}: a zone belongs to one partition
const walkZones = p => alarm.zones.filter(z => z.type && z.partition === p.number);
const cancel = t => { if (t) { clearTimeout(t); timers.delete(t); } };
const WALK_STEP_MS = process.env.MOCK_WALK_STEP_MS !== undefined ? Number(process.env.MOCK_WALK_STEP_MS) : 4000;
// eng_walk_mark(): the first mark says how and when; repeats only count trips (and beep, on a panel).
function walkMark(p, zone, how) {
  const w = walks[p.number - 1];
  if (!w.running) return;
  const m = marks.get(zone) || {tested: false, cover: false, how: '', first_s: 0, trips: 0};
  marks.set(zone, m);
  if (!m.tested && !m.cover) { m.how = how; m.first_s = Math.floor((Date.now() - w.started_at) * FAST / 1000); }
  const bit = how === 'cover' ? 'cover' : 'tested';
  if (!m[bit]) {
    m[bit] = true;
    w.seq++;
    if (bit === 'tested' && walkCounts(zoneCfg(zone).type, w.life24)) w.tested++;
    addEvent('walk_test_zone', 'Walk test', `${zoneCfg(zone).name} tested`, 3, 0, p, zone, 0);
  }
  m.trips = Math.min(255, m.trips + 1);
  if (how === 'fault') { live.open.add(zone); later(1000, () => live.open.delete(zone)); }
}
// A pretend tester walking the house: some doors and windows, the hallway twice, the keyfob, the back
// door's cover only, the CO detector's own test button (marked, never counted); the living room is missed.
const WALK_ROUTE = [[1, 'fault'], [4, 'fault'], [18, 'button'], [3, 'fault'], [4, 'fault'], [5, 'cover'], [2, 'fault'], [10, 'sensor_test']];
function walkStart(p, by, from, life24) {
  const w = walks[p.number - 1];
  walkZones(p).forEach(z => marks.delete(z.number));
  Object.assign(w, {running: true, started_at: Date.now(), started_by: by, started_from: from, life24: !!life24, tested: 0,
    total: walkZones(p).filter(z => walkCounts(z.type, !!life24)).length, ended_by: 0, end_how: ''});
  w.seq++;
  walkLimit(p);
  addEvent('walk_test_start', 'Walk test started', `By ${nameOf(by)} · ${from === 'panel_manager' ? 'Panel Manager' : from}`, 3, 0, p, 0, by);
  if (WALK_STEP_MS > 0 && p.number === 1)
    w.steps = WALK_ROUTE.map(([zone, how], i) => later(WALK_STEP_MS * (i + 1), () => walkMark(p, zone, how)));
}
function walkLimit(p) { // the full limit again (a start, Extend)
  const w = walks[p.number - 1], ms = alarm.options.walk_test_timeout_s * 1000;
  cancel(w.limit);
  w.until = Date.now() + ms / FAST;
  w.limit = later(ms, () => walkEnd(p, 0, 'time_limit'));
}
function walkEnd(p, by, how) {
  const w = walks[p.number - 1];
  cancel(w.limit);
  w.steps.forEach(cancel);
  Object.assign(w, {running: false, until: 0, limit: null, steps: [], ended_by: by, end_how: how});
  addEvent('walk_test_end', 'Walk test ended', `${w.tested} of ${w.total} zones tested`, 3, 0, p, 0, by);
}
const walkLeft = w => w.running ? Math.ceil(Math.max(0, w.until - Date.now()) * FAST / 1000) : 0;
const walkTrips = p => walkZones(p).reduce((n, z) => n + (marks.get(z.number)?.trips || 0), 0);
const MOCK_BOOT = Date.now();
// The poll's key (panel_web.c's FNV-1a over the same things; only equality matters).
function walkKey(mine) {
  const text = JSON.stringify([MOCK_BOOT, mine.map(c => {
    const w = walks[c.number - 1];
    return [c.number, w.running, w.seq, w.started_at, w.end_how, w.ended_by, w.life24,
      walkZones(parts[c.number - 1]).map(z => [z.number, marks.get(z.number), zoneFlags(z) & 1, zoneLive(z.number).check])];
  })]);
  let h = 2166136261;
  for (const ch of text) h = Math.imul(h ^ ch.codePointAt(0), 16777619) >>> 0;
  return h;
}
// GET walk-test (panel_web.c walk_test_get): the viewer's partitions; ?key= answers "unchanged" when nothing the page shows
// moved (a mark, a trip, a start, an end, an extend, a zone opening, closing or getting a CHECK, a restart).
function walkTest(s, query) {
  const mine = viewerPartitions(s), seq = mine.reduce((n, c) => n + walks[c.number - 1].seq, 0);
  const trips = mine.reduce((n, c) => n + walkTrips(parts[c.number - 1]), 0);
  const key = walkKey(mine);
  if (query.get('key') === String(key))
    return {unchanged: true, key, seq, trips, partitions: mine.map(c => ({number: c.number, running: walks[c.number - 1].running, remaining_s: walkLeft(walks[c.number - 1])}))};
  return {key, seq, trips, partitions: mine.map(c => {
    const p = parts[c.number - 1], w = walks[p.number - 1], any = w.running || !!w.end_how, zones = walkZones(p);
    return {number: p.number, name: c.name, running: w.running, remaining_s: walkLeft(w), started_by: w.started_by, started_by_name: nameOf(w.started_by),
      started_from: any ? w.started_from : '', started_s_ago: any ? Math.floor((Date.now() - w.started_at) * FAST / 1000) : 0, seq: w.seq,
      tested: w.tested, total: w.total, life24: w.life24, ended_by: w.ended_by, ended_by_name: nameOf(w.ended_by), end_how: w.end_how,
      zones: zones.filter(z => walkCounts(z.type, w.life24)).map(z => {
        const m = marks.get(z.number) || {tested: false, cover: false, how: '', first_s: 0, trips: 0}, f = zoneFlags(z);
        return {number: z.number, name: z.name, tested: m.tested, cover: m.cover, how: m.how, first_s: m.first_s, trips: m.trips,
          open: !!(f & 1), check: zoneLive(z.number).check}; // CHECK as zone-status says it
      }),
      live: zones.filter(z => !walkCounts(z.type, w.life24)).map(z => ({number: z.number, name: z.name})),
      trips: walkTrips(p)};
  })};
}
function partitionAction(s, body, res) {
  const answer = (code, json) => send(res, code, json);
  const refuse = (error, zone = 0) => answer(409, {error, zone});
  const isBool = v => v === undefined || typeof v === 'boolean';
  const zoneNumber = Number.isInteger(body.zone) && body.zone >= 1 && body.zone <= 128 ? body.zone : 0;
  if (!Number.isInteger(body.number) || !['arm', 'disarm', 'clear', 'acknowledge', 'chime', 'bypass', 'unbypass',
    'walk_test_start', 'walk_test_end', 'walk_test_extend'].includes(body.action) ||
      (body.action === 'arm' && !LEVELS.includes(body.level)) || (['bypass', 'unbypass'].includes(body.action) && !zoneNumber) ||
      !isBool(body.no_entry_delay) || !isBool(body.silent_exit) || !isBool(body.bypass_open) || !isBool(body.life24)) return answer(400, {error: 'bad_request'});
  if (!viewerPartitions(s).some(x => x.number === body.number)) return answer(403, {error: 'not_allowed'});
  const p = parts[body.number - 1], a = s.authority, source = 'Panel Manager';
  const disarmed = p.state === 'disarmed';
  // Each action only in the state it is for, checked before anything else (the firmware checks it
  // on the alarm task, alarm_service_command_as): disarm while armed, in exit or entry time or in
  // an alarm; acknowledge in any state; everything else while disarmed (and, on a panel, with
  // installer mode closed - the mock never opens it).
  const needs = {disarm: !disarmed || p.alarm, acknowledge: true}[body.action] ?? disarmed;
  if (!needs) return refuse('bad_state');
  const w = walks[p.number - 1];
  switch (body.action) {
    case 'walk_test_start': // WALK-001, WALK-002: never while one runs or an alarm sounds
      if (!CAN_TEST.includes(a)) return refuse('not_authorized');
      if (w.running || p.alarm) return refuse('bad_state');
      walkStart(p, s.user, 'panel_manager', body.life24);
      break;
    case 'walk_test_extend': // WALK-004
      if (!CAN_TEST.includes(a)) return refuse('not_authorized');
      if (!w.running) return refuse('bad_state');
      walkLimit(p);
      addEvent('walk_test_extend', 'Walk test extended', `By ${nameOf(s.user)} · ${source}`, 3, 0, p, 0, s.user);
      break;
    case 'walk_test_end': // WALK-025: whoever may disarm it (a disarmed partition: anyone who disarms)
      if (!CAN_DISARM.includes(a) && a !== 'installer') return refuse('not_authorized');
      if (!w.running) return refuse('bad_state');
      walkEnd(p, s.user, 'command');
      break;
    case 'arm': {
      if (a === 'installer') return refuse('not_authorized'); // USR-001
      if (w.running) return refuse('bad_state'); // WALK-005: nothing arms it during a walk test
      const blockers = blockersFor(p, body.level);
      if (blockers.length && !(body.bypass_open && blockers.every(bypassable))) return refuse('not_ready', blockers[0].number);
      blockers.forEach(z => p.bypass.add(z.number));
      arm(p, body.level, body, s.user, source);
      break;
    }
    case 'disarm':
      // A guest or the installer disarms only what they armed themselves.
      if (!CAN_DISARM.includes(a) || (!disarmed && ['guest'].includes(a) && p.armed_by !== s.user)) return refuse('not_authorized');
      disarm(p, s.user, source);
      break;
    case 'clear':
      if (!CAN_DISARM.includes(a)) return refuse('not_authorized');
      disarm(p, s.user, source);
      break;
    case 'acknowledge': // acknowledges trouble beeps and silences fire, CO and auxiliary sirens
      p.trouble_unack = false;
      if (['fire', 'co', 'aux'].includes(p.alarm_kind)) silence(p);
      break;
    case 'chime':
      p.chime = !p.chime;
      break;
    default: { // bypass, unbypass. The engine names the zone only when it can't be bypassed
      // (cmd_bypass sets not_ready_zone for NOT_BYPASSABLE); other refusals name none.
      if (!CAN_BYPASS.includes(a)) return refuse('not_authorized');
      const z = zoneCfg(zoneNumber);
      if (!z.type || z.partition !== p.number) return refuse('bad_arg');
      if (body.action === 'bypass') {
        if (!bypassable(z)) return refuse('not_bypassable', zoneNumber);
        p.bypass.add(zoneNumber);
      } else p.bypass.delete(zoneNumber);
    }
  }
  return answer(200, {ok: true});
}
// MOCK_SCENARIO: where the first partition starts, with the history that led there.
function applyScenario(name) {
  const p = parts[0], t = Date.now();
  const armed = (level, ms) => { Object.assign(p, {level, armed_by: 2}); addEvent('armed', `Armed ${LEVEL_WORDS[level]}`, 'By Alex · keypad', 2, 3, p, 0, 2, t - ms); };
  const tripped = (zone, ms) => { p.memory.push(zone); addEvent('alarm', 'Burglary alarm', zoneCfg(zone).name, 0, 7, p, zone, 0, t - ms); };
  if (name === 'notready') { live.open.add(5); live.lowbat.add(17); p.trouble_unack = true; }
  else if (name === 'troubles') { // ALARM_TRBL_COMM_FAIL and _BELL; the living room motion missed its check-ins
    live.troubles = 4 | 128;
    live.missed.add(6);
    live.lowbat.add(17);
    p.trouble_unack = true;
    addEvent('trouble', 'Siren problem', 'Check the siren wiring', 1, 2, p, 0, 0, t - 3 * 86400e3);
    addEvent('trouble', 'Water heater leak · Low battery', 'Check the sensor', 1, 2, p, 17, 0, t - 26 * 3600e3);
    addEvent('trouble', 'Living room motion · Not reporting', 'Check the sensor', 1, 2, p, 6, 0, t - 5 * 3600e3);
    addEvent('trouble', 'Reports not getting through', "A connected service can't be reached", 1, 2, p, 0, 0, t - 40 * 60e3);
    live.keypads.add(2); // not responding, and the panel's cover that was opened and closed (restored)
    live.restored = 1 << 8;
    addEvent('trouble', 'Keypad 2 not responding', 'Check its power and signal', 1, 2, p, 0, 0, t - 55 * 60e3);
    addEvent('trouble', 'Panel tamper', 'The panel was opened', 1, 2, p, 0, 0, t - 19 * 3600e3);
  }
  else if (name === 'motion') live.open.add(4);
  else if (name === 'exit') { armed('away', 0); beginExit(p, alarm.partitions[0].exit_delay_s * 1000); }
  else if (name === 'armed') { armed('away', 3600e3); p.state = 'armed'; live.open.add(3); p.bypass.add(3); }
  else if (name === 'stay') { armed('stay', 1800e3); p.state = 'armed'; }
  else if (name === 'entry') { armed('away', 600e3); live.open.add(1); beginEntry(p, 1); }
  else if (name === 'alarm') {
    armed('away', 480e3);
    p.state = 'armed';
    p.alarm = p.siren = true;
    p.alarm_kind = 'burglary';
    p.alarm_at = t - 60e3;
    live.open.add(1);
    live.open.add(4);
    tripped(1, 60e3);
    tripped(4, 41e3);
    later(6000, () => { if (p.alarm) { live.open.add(3); tripZone(p, 3); } });
  } else if (name === 'memory') {
    armed('away', 360e3);
    tripped(1, 180e3);
    tripped(4, 161e3);
    tripped(3, 140e3);
    p.state = 'armed';
    disarm(p, 2, 'keypad');
    events[0].epoch_ms = t - 120e3;
    p.canceled = false;
    p.memory = [1, 4, 3];
  } else if (name === 'fire') { // a smoke detector in alarm while disarmed: acknowledge silences it
    live.open.add(9);
    Object.assign(p, {alarm: true, alarm_kind: 'fire', siren: true, alarm_at: t - 20e3});
    p.trouble_unack = parts[1].trouble_unack = true; // Troubles' Acknowledge must not silence the fire
    p.memory.push(9);
    addEvent('alarm', 'Fire alarm', zoneCfg(9).name, 0, 7, p, 9, 0, t - 20e3);
  }
}
applyScenario(process.env.MOCK_SCENARIO || (lockedEnv ? 'armed' : 'ready'));
if (process.env.MOCK_WALK && parts[0].state === 'disarmed' && !parts[0].alarm) walkStart(parts[0], 1, 'panel', false); // WALK-024
if (process.env.MOCK_SCENARIO === 'notready' && !process.env.MOCK_CLOCK) process.env.MOCK_CLOCK = 'lost';
// notify_zone_state() in words, as the firmware adds them (the panel's Zones view).
const zoneWords = z => {
  const cls = alarm.zones[z.zone - 1].device_class;
  if (z.trouble) return ['Low battery', 2];
  if (z.flags & 1) return [[1, 2, 3].includes(cls) ? 'Open' : cls === 4 ? 'Motion' : 'Active', [6, 7, 8, 9].includes(cls) ? 7 : 2];
  return [[1, 2, 3].includes(cls) ? 'Closed' : cls === 4 ? 'No motion' : cls === 10 ? 'Dry' : 'Normal', 1];
};
// The update server (OTA-017..OTA-022): pretend servers with their signing keys; MOCK_FACTORY_CODE=1 keeps the installer code the
// factory one, MOCK_WITHDRAWN=1 withdraws the running version and offers an important fix.
const SERVERS = {'roboalarms.com': {name: 'RoboAlarms release', fingerprint: '3F9A 1C2B 77D0 E415', sha256: '3f9a1c2b77d0e415' + '5c0e'.repeat(12)},
  'example.net': {name: 'Example Builds', fingerprint: '8C41 0E9D 52A7 B3F6', sha256: '8c410e9d52a7b3f6' + 'a1d2'.repeat(12)}};
const updateServer = {domain: 'roboalarms.com', channel: 'stable'};
// As panel_ota: the running image's key (RoboAlarms' here) is always trusted, and the panel keeps one accepted key with the
// server; keeping another server replaces it, and going back to a server signed with the image's own key drops it.
const OWN_KEY = SERVERS['roboalarms.com'].sha256;
let acceptedKey = null;
const shaTrusted = sha => sha === OWN_KEY || sha === acceptedKey;
const keyTrusted = key => shaTrusted(key.sha256);
const factoryCode = !!process.env.MOCK_FACTORY_CODE;
// MOCK_SERVER_KEY=none: the page is told of no key; unnamed: a key without a name (the firmware may send either).
const SERVER_KEY = process.env.MOCK_SERVER_KEY || '';
let withdrawn = !!process.env.MOCK_WITHDRAWN, serverCheck = null;
function serverDomain(text) {
  const d = String(text || '').trim().toLowerCase().replace(/^https?:\/\//, '').replace(/\/+$/, '');
  return /^(?=.{4,253}$)([a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z]{2,63}$/.test(d) ? d : '';
}
function finishServerCheck(domain) {
  if (serverCheck?.domain !== domain) return;
  const key = SERVERS[domain];
  const result = key ? 'ok' : domain === 'example.org' ? 'not_a_server' : domain === 'example.com' ? 'no_firmware' : 'unreachable';
  Object.assign(serverCheck, {state: 'done', result, ...(key ? {key: {...key}, trusted: keyTrusted(key)} : {})});
}
let web = {developer: true}, update = {phase: 0, latest: withdrawn ? '0.6.2' : '', available: withdrawn, reason: ''}, check = null, scanDone = false, ha = {enabled: true, paired: false, state: 0, client: '', code: '', window_s: 0};
// An update's run (BL-188, OTA-026..OTA-030), a panel's minutes in seconds: download 12 s, check it 2 s, wait for disarm, the
// restart's 3 s, then 6 s without an answer, the self-test 12 s; a stop lands in 3 s, a check takes 1.5 s. MOCK_UPDATE_SPEED=20
// runs it 20 times faster. Install starts it ("live"); POST mock/update sets any step by hand, as the tests do.
const UPDATE_SPEED = Number(process.env.MOCK_UPDATE_SPEED) || 1, UPDATE_SIZE = 2831155, UPDATE_TARGET = '0.6.2';
const run = {stage: '', kind: '', since: 0, percent: -1, last: null, running: '0.6.1', confirming: false, check: '', checkReason: '', reason: null,
  stopper: '', live: false, offline: false, signedOut: false};
const runBusy = () => !!run.stage || run.confirming;
const updating = () => ['downloading', 'verifying', 'waiting', 'installing', 'stopping', 'offline'].includes(run.stage);
function runTick() {
  if (!run.live) return;
  const now = Date.now(), gone = (now - run.since) * UPDATE_SPEED, to = (stage, more = {}) => Object.assign(run, {stage, since: now}, more);
  if (run.check === 'checking') {
    if (gone > 1500) { Object.assign(run, {check: '', live: false}); if (run.running !== UPDATE_TARGET) Object.assign(update, {latest: UPDATE_TARGET, available: true}); }
    return;
  }
  if (run.stage === 'downloading') { run.percent = Math.min(100, Math.floor(gone / 120)); if (run.percent >= 100) to('verifying'); }
  else if (run.stage === 'verifying' && gone > 2000) to('waiting');
  else if (run.stage === 'waiting' && !isLocked() && gone > 1000) to('installing');
  else if (run.stage === 'installing' && gone > 3000) to('offline', {offline: true});
  else if (run.stage === 'offline' && gone > 6000) to('self_test', {offline: false, running: UPDATE_TARGET, confirming: true, signedOut: true});
  else if (run.stage === 'self_test' && gone > 12000) to('', {confirming: false, live: false, percent: -1, kind: '',
    last: {result: 'updated', version: UPDATE_TARGET, from: '0.6.1', at: Math.floor(now / 1000)}});
  else if (run.stage === 'stopping' && gone > 3000) to('', {live: false, percent: -1, kind: '',
    last: {result: 'stopped', version: UPDATE_TARGET, from: run.running, by: run.stopper, at: Math.floor(now / 1000)}});
}
// What GET updates says of the run (the fields OTA-026..OTA-028 name).
function runAnswer() {
  runTick();
  const stage = ['self_test', 'offline'].includes(run.stage) ? '' : run.stage;
  const held = ['downloading', 'verifying', 'waiting'].includes(stage) || run.confirming;
  const percent = run.percent;
  return {stage, kind: run.kind || (stage ? 'release' : ''), target: UPDATE_TARGET, previous: '0.6.1', percent, size: UPDATE_SIZE,
    bytes: percent < 0 ? 0 : Math.round(UPDATE_SIZE * percent / 100), last: run.last, confirming: run.confirming,
    reason: run.reason ?? (held && isLocked() ? (parts.some(p => p.alarm) ? 'An alarm is sounding' : 'The system is armed') : run.check === 'failed' ? run.checkReason : ''),
    failed: run.check === 'failed' ? 'check' : run.last?.result === 'failed' ? 'install' : '',
    phase: stage ? {downloading: 2, verifying: 2, stopping: 2, wifi_module: 2, waiting: 3, installing: 4}[stage] || 0
      : run.check === 'checking' ? 1 : run.check === 'failed' ? 5 : 0};
}
let tz = 'CST6CDT,M3.2.0,M11.1.0', ntp = 'pool.ntp.org';
// The clock module (docs/features/35): its state, what set the clock last, and how far the pretend
// panel's clock is from this computer's.
let clockModule = process.env.MOCK_CLOCK || 'none', clockSource = 'internet', clockOffsetMs = -4000;
const clockJson = () => ({module: clockModule, source: clockSource, settable: ['ok', 'lost'].includes(clockModule),
  lost_at_start: clockModule === 'lost', ...(clockModule === 'ok' || clockModule === 'lost' ? {temp_c: 24.25} : {})});

// What the browser last saved for a zone, all three inputs (the firmware keeps the whole binding, hold_s too).
const savedInputs = new Map();
function binding(n) {
  if (savedInputs.has(n) && bindings.has(n)) return {inputs: savedInputs.get(n)};
  const b = bindings.get(n);
  const address = b ? {kind: b.kind, input: b.input, id: b.id, ref: b.ref || ''} : {kind: 0, input: 0, id: '0', ref: ''};
  const mapping = {mode: 0, invert: false, threshold_x100: 0, hysteresis_x100: 0, states: '', grace_s: 0, response_ms: 0, hold_s: 0};
  return {inputs: [0, 1, 2].map(role => ({role, address: role ? {kind: 0, input: 0, id: '0', ref: ''} : address, mapping}))};
}
const showsMemory = p => p.state === 'disarmed' && !p.alarm && p.memory.length > 0;
function status(s) {
  return {reauth_s: reauthLeft(s), panel_id: '30eda0123456', boot, board: 'crowpanel_p4', firmware: run.running, csrf: s.csrf, user: s.user, authority: s.authority,
    developer: web.developer, web: true, network_pending: false,
    capabilities: {width: 1024, height: 600, jpeg: true, frame_interval_ms: 100, input: true, video: ['h264', 'jpeg'], zwave: zwaveOn, zigbee: zigbeeOn, automations: automationsOn,
      integrations: ['home_assistant', 'mqtt', 'ntfy', 'pushover']},
    wifi: {ssid: 'Harbor House', ip: '192.0.2.10', rssi: -58, time_synced: process.env.MOCK_SCENARIO !== 'notready'}, clock: clockJson(),
    health: {uptime_s: 93784, internal_kb: 231, psram_kb: 25480, stale: 0, chip: 'ESP32-P4 v1.3', idf: 'v6.1', internal_largest_kb: 180, web_stack_free: 9120,
      power: {battery: 'absent', percent: 0, low: false, charging: false},
      display: {frames: 5213440, late: 0, guard_trips: 0, forced_off: false}},
    name: users.find(u => u.number === s.user)?.name || '', has_password: true, password_change_required: temporary.has(s.user),
    access: {...Object.fromEntries(['users', 'shared', 'system', 'installer'].map(k => [k, ACC[k](s.authority)])), arm: s.authority !== 'installer'}, // USR-001
    config_revision: alarm.revision, program: false, troubles: live.troubles, trouble_count: troubleList(s).length, trouble_sig: troubleSig(troubleList(s)), locked: isLocked(), updating: updating(),
    // An automation's coming arming or its hold, on the partitions this person sees (panel_web.c status, D19, D27).
    ...(automationsOn ? {automation_warnings: auWarningsFor(viewerPartitions(s).map(p => p.number)),
      automation_reminders: auRemindersFor(viewerPartitions(s).map(p => p.number))} : {}), // BL-224: a reminder's note

    // The partitions this person sees, as the firmware: all for the installer and the system master.
    partitions: viewerPartitions(s).map(c => {
      const p = parts[c.number - 1], faults = alarm.zones.filter(z => z.type && z.partition === p.number && live.open.has(z.number) && !p.bypass.has(z.number)).length;
      return {number: p.number, name: c.name, state: p.state, ready: p.state === 'disarmed' && faults === 0, faults, level: p.level,
        no_entry_delay: p.no_entry_delay, silent_exit: p.silent_exit, armed_by: p.armed_by, armed_by_name: nameOf(p.armed_by),
        delay_left_ms: p.deadline ? Math.max(0, p.deadline - Date.now()) : 0, exit_delay_s: c.exit_delay_s, entry_delay_s: [c.entry_delay1_s, c.entry_delay2_s],
        // alarm: any alarm active (burglary while armed, fire in any state); memory: only while
        // disarmed with none active (panel_web.c partition_status).
        ready_levels: LEVELS.filter(l => !blockersFor(p, l).length), chime: p.chime, alarm: p.alarm, alarm_kind: p.alarm ? p.alarm_kind : '',
        siren: p.siren, memory: showsMemory(p), canceled: showsMemory(p) && p.canceled, bypassed: p.bypass.size, trouble_unack: p.trouble_unack,
        // A walk test running on it (WALK-024), for everyone who sees the partition; zero and blank while none runs.
        ...((w = walks[p.number - 1]) => ({walk_test: w.running, walk_tested: w.running ? w.tested : 0, walk_total: w.running ? w.total : 0,
          walk_remaining_s: walkLeft(w), walk_started_by: w.running ? w.started_by : 0, walk_started_by_name: w.running ? nameOf(w.started_by) : '',
          walk_started_from: w.running ? w.started_from : ''}))()};
    })};
}
const screen = () => `<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="600"><rect width="1024" height="600" fill="#0f2430"/>
  <text x="48" y="80" fill="#9fd8c6" font-family="Segoe UI, sans-serif" font-size="30">Home</text>
  <text x="48" y="300" fill="#fff" font-family="Segoe UI, sans-serif" font-size="64" font-weight="600">Disarmed</text>
  <text x="48" y="350" fill="#b8c8d2" font-family="Segoe UI, sans-serif" font-size="26">Kitchen Window is open</text>
  <text x="48" y="560" fill="#6f8795" font-family="Segoe UI, sans-serif" font-size="20">Mock panel · ${new Date().toLocaleTimeString()}</text></svg>`;

// ---- HTTP ------------------------------------------------------------------------------
function send(res, code, body, type = 'application/json') {
  res.writeHead(code, {'Content-Type': type, 'Cache-Control': 'no-store', 'X-Panel-Boot': boot});
  res.end(type === 'application/json' ? JSON.stringify(body) : body);
}
function readBody(req) {
  return new Promise(resolve => { let s = ''; req.on('data', c => s += c); req.on('end', () => { try { resolve(JSON.parse(s || '{}')); } catch { resolve({}); } }); });
}
// MOCK_AUTOLOGIN=installer|master|partition|standard: every request is signed in (headless).
const autoLogin = LOGINS[{installer: 'installer', master: 'alex', partition: 'pat', standard: 'sam', guest: 'dog walker'}[process.env.MOCK_AUTOLOGIN]];
const autoSession = autoLogin && {user: autoLogin[1], csrf: 'auto', confirmed: 0, grant: process.env.MOCK_GRANT === '1'};
function session(req) {
  if (run.signedOut) return null; // the restart forgot every sign-in
  let s = autoSession;
  if (!s) {
    const token = /panel_session=([0-9a-f]{64})/.exec(req.headers.cookie || '')?.[1];
    s = token && sessions.get(token);
    if (s && req.method !== 'GET' && req.headers['x-panel-csrf'] !== s.csrf) return null;
  }
  if (s) s.authority = authorityOf(s.user); // as the firmware: the user record decides, every time
  return s;
}
// BL-135, WEB-016: no keypad code in a browser request. The few changes that let a computer in or wipe the panel
// ask for the signed-in person's own password, unless it was typed in the last 10 minutes (a password sign-in
// counts; a workstation's grant session starts unconfirmed, as the autologin here). Null when fine, else the answer.
const REAUTH_MS = 10 * 60e3;
const reauthLeft = s => Math.max(0, Math.ceil(((s.confirmed || 0) + REAUTH_MS - Date.now()) / 1000));
function needPassword(s, password) {
  if (reauthLeft(s) > 0) return null;
  if (!password) return {error: 'password_required'};
  if (!Object.values(LOGINS).some(([right, user]) => user === s.user && right === password)) return {error: 'wrong_password'};
  s.confirmed = Date.now();
  return null;
}

// The backup file (docs/features/14 CFG-005, CFG-006; components/core/backup/include/backup/backup.h),
// made and opened with Node's crypto exactly as the panel does with PSA: "RABK", PBKDF2-HMAC-SHA256,
// AES-256-GCM over the records. A small panel's worth of entries; the fewest iterations a file may ask.
const BACKUP_ITER = 10000;
function backupPayload(board = 'crowpanel_p4') {
  const str = s => Buffer.concat([Buffer.from([Buffer.byteLength(s)]), Buffer.from(s)]);
  const entry = (ns, key, type, value) => { const len = Buffer.alloc(4); len.writeUInt32LE(value.length);
    return Buffer.concat([Buffer.from([2]), str(ns), str(key), Buffer.from([type]), len, value]); };
  const created = Buffer.alloc(8);
  created.writeBigUInt64LE(BigInt(Math.floor(Date.now() / 1000)));
  const entries = [entry('net', 'ssid', 9, Buffer.from('Harbor House')), entry('ui', 'bright', 1, Buffer.from([80])),
    entry('zones', 'bind', 10, Buffer.from('ZBND')), entry('alarm', 'config', 10, Buffer.from('ACFG'))];
  const count = Buffer.alloc(4);
  count.writeUInt32LE(entries.length);
  return Buffer.concat([Buffer.from([1, 1]), created, str('0.6.0'), str(board), ...entries, Buffer.from([0xff]), count]);
}
function backupSeal(password, payload) {
  const salt = crypto.randomBytes(16), nonce = crypto.randomBytes(12), header = Buffer.alloc(44);
  header.write('RABK', 0);
  header.set([1, 1, 1, 0], 4);
  header.writeUInt32LE(BACKUP_ITER, 8);
  salt.copy(header, 12);
  nonce.copy(header, 28);
  header.writeUInt32LE(payload.length, 40);
  const key = crypto.pbkdf2Sync(Buffer.from(password), salt, BACKUP_ITER, 32, 'sha256');
  const gcm = crypto.createCipheriv('aes-256-gcm', key, nonce);
  gcm.setAAD(header);
  return Buffer.concat([header, gcm.update(payload), gcm.final(), gcm.getAuthTag()]);
}
// null and the reason, or the payload's summary.
function backupOpen(password, file) {
  if (file.length < 60 || file.toString('latin1', 0, 4) !== 'RABK' || file[4] !== 1 || file[5] !== 1 || file[6] !== 1 || file[7] !== 0) return {why: 'backup_format'};
  const iter = file.readUInt32LE(8), len = file.readUInt32LE(40);
  if (iter < 10000 || iter > 2000000 || len + 60 !== file.length) return {why: 'backup_format'};
  const key = crypto.pbkdf2Sync(Buffer.from(password), file.subarray(12, 28), iter, 32, 'sha256');
  const gcm = crypto.createDecipheriv('aes-256-gcm', key, file.subarray(28, 40));
  gcm.setAAD(file.subarray(0, 44));
  gcm.setAuthTag(file.subarray(44 + len));
  let payload;
  try { payload = Buffer.concat([gcm.update(file.subarray(44, 44 + len)), gcm.final()]); } catch { return {why: 'backup_decrypt'}; }
  const fwLen = payload[10], boardLen = payload[11 + fwLen];
  const board = payload.toString('latin1', 12 + fwLen, 12 + fwLen + boardLen);
  if (board !== 'crowpanel_p4') return {why: 'backup_board'};
  return {firmware: payload.toString('latin1', 11, 11 + fwLen), created: Number(payload.readBigUInt64LE(2))};
}
const hexPassword = hex => /^([0-9a-f]{2})*$/i.test(hex || '') ? Buffer.from(hex, 'hex').toString() : null;
const needInstaller = s => s.authority !== 'installer' ? 'Installer authority required' : null;
const delay = ms => new Promise(r => setTimeout(r, ms));

// Languages on this panel (docs/features/37 language-packs.md, I18N-046): a few installed packs in
// each state, one the update source offers. A download finishes after a moment (MOCK_LANG_MS).
const FIRMWARE = '0.6.1';
const languages = {
  installed: [
    {locale: 'es-US', name: 'Español (Estados Unidos)', version: FIRMWARE, draft: true, state: 'ready', missing: 0, size: 155648, loaded: true, default: false},
    {locale: 'fr-CA', name: 'Français (Canada)', version: '0.6.0', draft: false, state: 'other', missing: 12, size: 151552, loaded: true, default: false},
    {locale: 'de-DE', name: 'Deutsch', version: FIRMWARE, draft: true, state: 'refused', missing: 0, size: 147456, loaded: false, default: false},
    {locale: 'it-IT', name: 'Italiano', version: FIRMWARE, draft: false, state: 'damaged', missing: 0, size: 147456, loaded: false, default: false},
    {locale: 'en-GB', name: 'English (United Kingdom)', version: FIRMWARE, draft: false, state: 'ready', missing: 3, size: 12288, loaded: true, default: false},
  ],
  available: [{locale: 'es-US', version: FIRMWARE, size: 155648}, {locale: 'fr-CA', version: FIRMWARE, size: 155648}, {locale: 'pt-BR', version: FIRMWARE, size: 151552}],
  fetch: {locale: '', phase: 'idle', result: 'ok'},
};
const languagesJson = () => ({current: config.display?.lang || 'en-US', firmware: FIRMWARE, free: 393216 - languages.installed.reduce((n, l) => n + l.size, 0) + 400000,
  installed: languages.installed, available: languages.available, fetch: languages.fetch});
const LANG_TAG = /^[a-z]{2,3}(-[A-Za-z0-9]{2,8})*$/;
function languagesPost(body) { // -> [status, {result}]
  const a = body.action, locale = String(body.locale || '');
  if ((a !== 'remove' && a !== 'download') || !LANG_TAG.test(locale)) return [400, {error: 'Unknown language action'}];
  if (a === 'remove') {
    const i = languages.installed.findIndex(l => l.locale === locale);
    if (i < 0) return [400, {result: 'not_found'}];
    languages.installed.splice(i, 1);
    return [200, {result: 'ok'}];
  }
  const offer = languages.available.find(l => l.locale === locale);
  if (!offer) return [400, {result: 'not_found'}];
  if (languages.fetch.phase === 'downloading') return [503, {result: 'busy'}];
  languages.fetch = {locale, phase: 'downloading', result: 'ok'};
  setTimeout(() => {
    languages.installed = languages.installed.filter(l => l.locale !== locale);
    languages.installed.push({locale, name: locale, version: offer.version, draft: false, state: 'ready', missing: 0, size: offer.size, loaded: true, default: false});
    languages.fetch = {locale, phase: 'done', result: 'ok'};
  }, Number(process.env.MOCK_LANG_MS || 4000));
  return [200, {result: 'ok'}];
}
// PUT: a file of 8192 bytes to 2 MB in whole 4096 blocks; one that starts "RLNG" is a pack.
function languagesPut(file) { // -> [status, {result}]
  if (file.length < 8192 || file.length > 2 * 1024 * 1024 || file.length % 4096) return [400, {result: 'size'}];
  if (file.toString('latin1', 0, 4) !== 'RLNG') return [400, {result: 'format'}];
  const locale = file.toString('latin1', 4, 16).replace(/\0.*$/, '') || 'nl-NL';
  languages.installed = languages.installed.filter(l => l.locale !== locale);
  languages.installed.push({locale, name: locale, version: FIRMWARE, draft: false, state: 'ready', missing: 0, size: file.length, loaded: true, default: false});
  return [200, {result: 'ok'}];
}

// The voice (docs/features/08 voice-first-build.md D06, D12, D14; the proposal BL-202-voice.js): one voice pack,
// its readiness, and each zone's spoken name, as panel_web answers them (web_voice.c). MOCK_VOICE=ready (a draft
// pack, the default) | released (not a draft) | unavailable (installed, not ready) | none (no pack) | error (the
// information can't be read) | absent (a firmware without the voice: 404, no spoken names). Announcements start on
// with a ready pack, off otherwise. POST mock/voice {mode, upload} changes them for tests (the firmware has no such
// route); upload: what the next good file's install answers (ok, busy, no_space, flash, off).
const voice = {mode: process.env.MOCK_VOICE || 'ready', version: 1, upload: 'ok'};
const voiceOn = () => voice.mode !== 'absent';
function voiceJson() {
  const installed = voice.mode !== 'none', ready = voice.mode === 'ready' || voice.mode === 'released';
  return {installed, ready, locale: installed ? 'en-US' : '', name: installed ? 'qwen3-designed_female' : '', version: installed ? voice.version : 0,
    draft: installed && voice.mode !== 'released', reason: ready ? '' : installed ? 'unavailable' : 'no_pack', free: 1400000};
}
const voiceRefusal = () => { const v = voiceJson(); return v.ready ? '' : v.installed ? 'unavailable' : 'no_pack'; };
// The words' texts are the pack's; zones 2 and 5 have none, zone 3 keeps Hall Motion and needs review (D07).
const VOICE_WORDS = {101: 'Front', 102: 'Door', 103: 'Hall', 104: 'Motion', 105: 'Garage', 106: 'Side'};
const spokenWords = {1: [101, 102], 3: [103, 104], 4: [105, 106, 102]};
function spokenOf(n) {
  const words = spokenWords[n] || [], v = voiceJson();
  const approved = n !== 3;
  return {words, summary: v.ready ? words.map(id => VOICE_WORDS[id]).join(' ') : '',
    state: !words.length ? 'empty' : !approved ? 'review' : !v.installed ? 'missing' : !v.ready ? 'unavailable'
      : !config.display.voice_enabled ? 'off' : 'ready',
    binding_rev: words.length ? n + 10 : 0, approved_rev: words.length && approved ? n + 10 : 0, pack_rev: words.length && approved ? 1 : 0};
}
// PUT: the panel's refusals first (who, then the lock: armed), then the size (before reading), then the file.
function voicesPut(s, file, length) { // -> [status, {result}]
  if (!ACC.system(s.authority)) return [403, {result: 'not_allowed'}];
  if (isLocked()) return [409, {result: 'armed'}];
  if (length < 8192 || length > 970752 || length % 4096) return [400, {result: 'too_big'}];
  if (file.toString('latin1', 0, 4) !== 'RVOC') return [400, {result: 'bad_signature'}];
  if (voice.upload !== 'ok') return [{busy: 409, flash: 503}[voice.upload] || 400, {result: voice.upload}];
  voice.mode = 'ready'; // installing never turns Announcements on, and keeps the preferences
  voice.version += 1;
  return [200, {result: 'ok'}];
}

// What a source sees, as the firmware answers (web_extra.c candidates()): the wired terminals and the simulator,
// and Home Assistant filtered by the query. Input 3 of the wired terminals is a pretend door, opened and closed
// every few seconds. Each candidate says which zone already uses it (BL-134).
function sourceCandidates(body) {
  const zoneOf = (kind, same) => { for (const [zone, a] of bindings) if (a.kind === kind && same(a)) return zone; return 0; };
  if (body.kind === 1 || body.kind === 2) {
    return [1, 2, 3, 4, 5, 6, 7, 8].map(n => {
      const zone = zoneOf(body.kind, a => Number(a.input) === n);
      const open = zone ? live.open.has(zone) : body.kind === 2 && n === 3 && Math.floor(Date.now() / 4000) % 2 === 1;
      return {address: {kind: body.kind, input: n, id: '0', ref: ''}, name: body.kind === 2 ? `Input ${n}` : `Simulated input ${n}`,
        detail: '', state: open ? 'open' : 'closed', zone, device_class: 0, changed: false, best: false, unusable: false};
    });
  }
  if (body.kind === 5) { // 433 MHz (firmware: zsrc_rf.c): heard while learning first, newest first; state = when it was last heard
    const rf = (proto, device, code) => ({kind: 5, input: code + 1, id: String(proto * 2 ** 32 + device), ref: ''});
    // "codes <protocol> <device in hex>": one sensor's codes in the order it sent them, for a zone's tamper or
    // low-battery input (RF-021): A2B3C opens with 3, closes with 5 (the source advises against it), its cover sends 7
    const codes = /^codes (\d+) ([0-9A-F]+)$/.exec(String(body.query || ''));
    if (codes) {
      const proto = Number(codes[1]), device = parseInt(codes[2], 16);
      if (proto !== 1 || device !== 0xA2B3C) return [];
      return [[3, 'Sent first, twice', '40 s ago', false], [5, 'Closes the zone, twice', '30 s ago', true], [7, 'Sent third, once', 'just now', false]]
        .map(([code, detail, state, unusable]) => ({address: rf(proto, device, code), name: `Code ${code.toString(16).toUpperCase()}`, detail, state,
          zone: zoneOf(5, a => a.id === rf(proto, device, code).id && Number(a.input) === code + 1), device_class: 0, changed: true, best: false, unusable}));
    }
    return [
      {address: rf(1, 0xA2B3C, 3), name: 'Sensor A2B3C', detail: 'Codes 3 and 5, heard 4 times', state: 'just now', changed: true, best: true},
      {address: rf(1, 0x51D07, 3), name: 'Sensor 51D07', detail: 'Code 3', state: '40 s ago', changed: false, best: false},
    ].map(c => ({...c, zone: zoneOf(5, a => a.id === c.address.id && Number(a.input) === c.address.input), device_class: 0, unusable: false}));
  }
  if (body.kind === 4) {
    const q = String(body.query || '').toLowerCase();
    const list = [['binary_sensor.kitchen_window', 'Kitchen window contact', 2], ['binary_sensor.back_door', 'Back door contact', 1],
      ['binary_sensor.shed_door', 'Shed door', 1], ['binary_sensor.porch_motion', 'Porch motion', 4],
      ['binary_sensor.basement_co', 'Basement CO alarm', 8], ['binary_sensor.water_heater_leak', 'Water heater leak', 10]];
    return list.filter(([ref, name]) => !q || ref.includes(q) || name.toLowerCase().includes(q)).map(([ref, name, cls]) => {
      const zone = zoneOf(4, a => a.ref === ref);
      return {address: {kind: 4, input: 0, id: '0', ref}, name, detail: ref, state: zone && live.open.has(zone) ? 'on' : 'off', zone, device_class: cls,
        changed: false, best: false, unusable: false};
    });
  }
  return [];
}

async function apiGet(p, s, res, query = new URLSearchParams()) {
  const ok = body => send(res, 200, body);
  if (p === 'status') return ok(status(s));
  if (temporary.has(s.user)) return send(res, 403, {error: 'password_change_required'});
  if (!ACC[GET_ACCESS(p)](s.authority)) return send(res, 403, {error: 'not_allowed'});
  if (p === 'frame') { await delay(60); return send(res, 200, screen(), 'image/svg+xml'); }
  if (p.startsWith('config/')) {
    const d = p.slice(7);
    if (!CONFIG_SCHEMA[d]) return send(res, 404, {error: 'Not included'});
    const values = Object.fromEntries(CONFIG_SCHEMA[d].filter(f => f.type !== 'password').map(f => [f.name, config[d][f.name]]));
    return ok({values, fields: CONFIG_SCHEMA[d], password_set: !!config.mqtt.password, ca_loaded: false, token_set: !!config.ntfy.token,
      topic_set: !!config.ntfy.topic, user_key_set: !!config.pushover.user_key, app_token_set: !!config.pushover.app_token});
  }
  if (p === 'languages') return ok(languagesJson());
  if (p === 'voices') return !voiceOn() ? send(res, 404, {error: 'Voice is not in this firmware'})
    : voice.mode === 'error' ? send(res, 503, {error: 'Memory busy'}) : ok(voiceJson());
  if (p === 'keypads') return ok(keypadGet(s.user));
  if (p === 'alarm') return ok({...alarm, schema: SCHEMA});
  if (p === 'people') {
    const me = users.find(u => u.number === s.user);
    const seen = users.filter(u => u.authority && (me.authority !== 3 || u.number === me.number || (u.partitions & me.partitions)));
    return ok({users: seen.map(u => ({...u, you: u.number === s.user, manage: manages(s.user, u.number), web_login: webLogins.has(u.number),
      web_login_possible: loginPossible(u), web_login_temporary: temporary.has(u.number)})), maximum: 64, code_min: Number(process.env.MOCK_CODE_MIN) || 4, panel_id: '30eda0123456', can_add: me.authority <= 3});
  }
  if (p === 'history') return ok({events});
  if (p === 'troubles') { const list = troubleList(s); return ok({troubles: list, sig: troubleSig(list)}); }
  if (p === 'walk-test') return ok(walkTest(s, query)); // docs/features/40 WALK-017, WALK-018
  if (p === 'time') return ok({timezone: tz, ntp, now_ms: Date.now() + clockOffsetMs, clock: clockJson()});
  if (p === 'home_assistant') { if (ha.window_s > 0) { ha.window_s = Math.max(0, ha.window_s - 3); if (ha.window_s < 170 && !ha.code) ha.code = '482 913'; } return ok(ha); }
  if (p === 'updates') { // OTA-017..OTA-022: the system master's and the installer's; the server, key and channel the installer's only
    const key = SERVERS[updateServer.domain];
    const shown = SERVER_KEY === 'none' ? null : SERVER_KEY === 'unnamed' ? {...key, name: ''} : {...key};
    return ok({running: run.running, ...runAnswer(), latest: update.latest, available: update.available && run.running !== update.latest, has_source: true,
      withdrawn: withdrawn, critical: withdrawn && update.available, installed_channel: updateServer.channel,
      ...(s.authority === 'installer' ? {server: {domain: updateServer.domain, official: updateServer.domain === 'roboalarms.com', key: shown},
        channel: updateServer.channel, factory_code: factoryCode, check: serverCheck} : {})});
  }
  if (p === 'zone-status') {
    const mask = ['installer', 'master'].includes(s.authority) ? 15 : users.find(u => u.number === s.user).partitions;
    return ok({zones: alarm.zones.filter(z => z.type && (mask & (1 << (z.partition - 1)))).map(z => {
      const live = zoneLive(z.number), [words, accent] = zoneWords(live);
      return {...live, name: z.name, device_class: z.device_class, partition: z.partition, words, accent,
        levels: LEVELS.filter(l => armedIn(z, l)), always: ALWAYS_ON.includes(z.type), entry_s: entrySeconds(z), bypassable: bypassable(z),
        ...(voiceOn() ? {spoken: spokenOf(z.number)} : {})}; // read-only, computed (D14)
    })});
  }
  if (p === 'storage') return ok({sd: {state: 'Ready', name: 'SD32G', filesystem: 'FAT32', message: '', generation: 3, capacity_bytes: 31914983424, free_bytes: 30100000000, usage_valid: true},
    flash: {running_slot: 'ota_0', chip_bytes: 16777216, image_bytes: 2494464, slot_bytes: 4194304, update_bytes: 4194304,
      partitions: [['nvs', 24576], ['otadata', 8192], ['ota_0', 4194304], ['ota_1', 4194304], ['eventlog', 1048576], ['storage', 6291456]].map(([name, bytes]) => ({name, bytes}))}});
  if (p === 'sources') return ok({sources: [{kind: 1, capabilities: 3, usable: true, link_down: false, trouble: false}, {kind: 2, capabilities: 3, usable: true, link_down: false, trouble: false},
    {kind: 3, capabilities: 7, usable: false, link_down: true, trouble: false}, {kind: 4, capabilities: 3, usable: true, link_down: false, trouble: false},
    {kind: 5, capabilities: 7, usable: false, link_down: false, trouble: false}]});
  // BL-134: where each zone's sensor is, in one request (firmware: zone_service_sources).
  if (p === 'zone-sources') return ok({zones: [...bindings].map(([zone, a]) =>
    ({zone, address: {kind: a.kind, input: a.input || 0, id: String(a.id || '0'), ref: a.ref || ''}}))});
  const m = /^binding\/(\d+)$/.exec(p);
  if (m) return ok(binding(Number(m[1])));
  if (p === 'network-check') {
    const steps = Array.from({length: 7}, (_, i) => {
      if (!check) return {state: 0, ms: 0, detail: ''};
      const t = (Date.now() - check) / 700;
      return {state: t > i + 1 ? (i === 6 ? 4 : 2) : t > i ? 1 : 0, ms: 12, detail: t > i + 1 ? ['Harbor House, good signal', '192.0.2.10 from the router', '192.0.2.1 answers in 3 ms',
        'Names resolve in 18 ms', 'The internet answers', 'pool.ntp.org answers', 'MQTT broker: connected, but not encrypted'][i] : ''};
    });
    return ok({running: !!check && Date.now() - check < 5600, done: !!check && Date.now() - check >= 5600, steps});
  }
  if (p === 'wifi-scan') return ok({done: scanDone, networks: scanDone ? [['Harbor House', -58, true], ['Harbor House', -71, true], ['Neighbors 5G', -80, true], ['CoffeeShop', -86, false]]
    .map(([ssid, rssi, secure]) => ({ssid, rssi, secure})) : []});
  if (p === 'notification-status') return ok({providers: [{provider: 0, state: 0, sent: 0, detail: ''}, {provider: 1, state: 4, sent: 7, detail: ''}]});
  if (p === 'zwave') {
    if (!zwaveOn) return ok({available: false});
    const {granted, ...progress} = zwave.progress; // eslint-disable-line no-unused-vars
    return ok({available: true, radio: {...zwave.radio, joining: zwave.smartstart.filter(e => e.state === 'waiting').length},
      devices: zwave.devices, smartstart: zwave.smartstart.map(zwEntry), progress, diag: zwave.diag});
  }
  if (p === 'zigbee') { // devices.md §14 (web_zigbee.c)
    if (!zigbeeOn) return query.has('device') ? send(res, 404, {error: 'not_found'}) : ok({available: false});
    const [code, answer] = zigbeeGet(query);
    return send(res, code, answer);
  }
  if (p === 'automations' || p === 'automations/devices') { // docs/features/41 DESIGN.md section 10
    if (!automationsOn) return send(res, 404, {error: 'Automations are not in this firmware'});
    return ok(p === 'automations' ? automationsJson() : automationDevicesJson());
  }
  if (p === 'developer/logs') return ok({text: '1 2394 authenticated development service started\n2 11502 health=0 internal=205303 largest=112640 crypto_faults=0 wifi=4'});
  return send(res, 404, {error: 'Not included in this firmware'});
}
async function apiPost(p, s, body, res) {
  const ok = (extra = {}) => send(res, 200, {ok: true, ...extra});
  const fail = (error, code = 409) => send(res, code, {error});
  if (p === 'logout') { sessions.forEach((v, k) => { if (v === s) sessions.delete(k); }); return ok(); }
  if (p === 'password') {
    const entry = Object.values(LOGINS).find(l => l[1] === s.user);
    if (!entry || body.current !== entry[0]) return fail('The current password is not right', 400);
    if (String(body.password || '').length < 12) return fail('Use at least 12 characters', 400);
    entry[0] = body.password;
    temporary.delete(s.user);
    return ok();
  }
  if (temporary.has(s.user)) return fail('password_change_required', 403);
  if (!ACC[POST_ACCESS(p, body)](s.authority)) return fail('not_allowed', 403);
  // A walk test's start and extend are changes (locked while armed, as Settings); its end stays, as a disarm does.
  const lockFree = ['logout', 'password', 'control'].includes(p) || (p === 'partition' && body.action !== 'walk_test_start') ||
    (p === 'updates' && body.action === 'cancel'); // a stop works while armed (OTA-027)
  if (isLocked() && !lockFree) return fail('locked');
  if (p === 'languages') { const [code, answer] = languagesPost(body); return send(res, code, answer); }
  if (p === 'keypads') {
    if (s.grant) return fail('password_sign_in_required', 403);
    const password = needPassword(s, body.me_password); if (password) return send(res, 403, password);
    const why = keypadPost(s.user, body); return why ? fail(why) : ok();
  }
  if (p === 'control') return ok();
  if (p === 'automations') { // ACC_SYSTEM, refused while locked (above), as web_automations_post
    // Saving makes this person the maker an alarm action runs as: their own password sign-in, never a grant (panel_web.c).
    if (s.grant) return send(res, 403, {error: 'password_sign_in_required'});
    if (!automationsOn) return send(res, 404, {error: 'Automations are not in this firmware'});
    const [code, answer] = automationsPost(s, body);
    return send(res, code, answer);
  }
  if (p === 'partition') return partitionAction(s, body, res);
  if (p === 'zigbee') { // devices.md §14: a network reset asks the installer's password again, never a keypad code
    if (!zigbeeOn) return send(res, 404, {error: 'Zigbee is not running on this panel'});
    if (body.op === 'reset') { const no = needPassword(s, body.me_password); if (no) return send(res, 403, no); }
    return send(res, 200, {result: zbOperation(body)});
  }
  if (p === 'zwave') {
    if (!zwaveOn) return send(res, 404, {error: 'Z-Wave is not running on this panel'});
    const result = r => send(res, 200, {result: r});
    const OPS = ['add', 'remove', 'keep_listening', 'stop', 'allow', 'pin', 'insecure', 'dismiss', 'restart', 'remove_failed', 'ping',
      'setup', 'routes', 'radio_check', 'radio_install'];
    if (OPS.includes(body.action)) return result(zwOperation(body));
    if (body.action === 'register') {
      const c = zwParse(body.code);
      if (c.error) return result(c.error);
      const name = String(body.name || '').slice(0, 23);
      const same = zwave.smartstart.find(e => e.groups.join() === c.groups.join());
      if (same) { Object.assign(same, {state: same.node ? 'joined' : 'waiting', failures: 0}, name ? {name} : {}); return result('updated'); }
      if (zwave.smartstart.length >= 32) return result('full');
      zwave.smartstart.push({...c, name, state: 'waiting', failures: 0, node: 0});
      return result('added');
    }
    const e = zwave.smartstart.find(x => zwId(x) === body.id);
    if (!e) return result('not_found');
    if (body.action === 'on') Object.assign(e, {state: e.node ? 'joined' : 'waiting', failures: 0});
    else if (body.action === 'off') e.state = 'off';
    else if (body.action === 'forget') zwave.smartstart.splice(zwave.smartstart.indexOf(e), 1);
    else if (body.action === 'grant' && body.keys > 0 && !(body.keys & ~(e.requested || 7))) e.grant = body.keys;
    else return result('bad_code');
    return result('updated');
  }
  if (p.startsWith('config/')) {
    const d = p.slice(7);
    for (const [k, v] of Object.entries(body.values || {})) {
      const f = CONFIG_SCHEMA[d].find(x => x.name === k);
      if (!f) return fail('Unknown field: ' + k, 400);
      if (f.type === 'number' && (v < f.min || v > f.max)) return fail('Invalid value for ' + k, 400);
      if (f.type === 'boolean' && typeof v !== 'boolean') return fail('Invalid value for ' + k, 400);
      if (d === 'display' && f.choices && !f.choices.some(c => c.value === v)) {
        return fail(k === 'lang' ? 'This panel has no such language' : k === 'region' ? 'Unknown region' : 'Invalid value for ' + k, 400);
      }
    }
    // D06: turning Announcements on needs a ready voice; one already on stays on.
    if (d === 'display' && body.values?.voice_enabled === true && !config.display.voice_enabled && voiceRefusal())
      return fail(voiceRefusal(), 400);
    Object.assign(config[d], body.values || {});
    return ok();
  }
  if (p === 'alarm') {
    const why = needInstaller(s);
    if (why) return fail(why, 403);
    if (body.revision !== alarm.revision) return fail('Configuration changed or installer mode is already occupied; reload');
    const c = body.changes || {};
    if (keypadState.busy) return fail('Finish pairing first');
    if ((c.devices || []).some(d => keypadState.keypads.some(k => k.number === d.number))) return fail('Edit paired keypads in their popup');
    for (const part of c.partitions || []) if (part.enabled === false) {
      if (keypadState.local?.home === part.number || keypadState.keypads.some(k => k.home === part.number) ||
          alarm.zones.some(z => z.type && z.partition === part.number) ||
          alarm.devices.some(d => d.enabled && d.partition === part.number) || alarm.partitions.filter(p => p.enabled).length === 1)
        return fail('Move assigned devices and zones first');
    }

    Object.assign(alarm.options, c.options || {});
    if (c.panic_keys) alarm.panic_keys = c.panic_keys;
    for (const group of ['zones', 'partitions', 'devices']) for (const x of c[group] || []) Object.assign(alarm[group][x.number - 1], x);
    alarm.revision++;
    return ok();
  }
  if (p === 'people') {
    if (!manages(s.user, body.number)) return fail('not_authorized');
    // BL-136: your own sign-in changes only on Your password, which asks for the current one (the panel refuses here)
    if (['web-password', 'web-off'].includes(body.action) && body.number === s.user) return fail('not_allowed', 403);
    const i = users.findIndex(u => u.number === body.number);
    if (body.action === 'web-password') {
      if (i < 0 || !loginPossible(users[i])) return fail('Arm-only, guest and duress codes can\'t have a browser sign-in', 400);
      if (String(body.password || '').length < 12) return fail('Use at least 12 characters', 400);
      webLogins.add(body.number);
      const name = users[i].name.toLowerCase(); // the installer's too: their name, not the panel ID
      LOGINS[name] = [body.password, body.number];
      if (body.temporary !== false) temporary.add(body.number); else temporary.delete(body.number); // missing: temporary, as the panel
      return ok();
    }
    if (body.action === 'web-off') { webLogins.delete(body.number); temporary.delete(body.number); return ok(); }
    if (body.action === 'delete') { users.splice(i, 1); webLogins.delete(body.number); return ok(); }
    // BL-136: a code another person has is refused (ALARM_ERR_CODE_IN_USE); nothing moves
    if (body.new_code && CODES[body.new_code] && CODES[body.new_code] !== body.number) return fail('code_in_use', 409);
    const u = {number: body.number, name: body.name, authority: body.number <= 2 ? body.number : body.authority, partitions: body.partitions};
    if (u.authority === 2 && body.number > 2) return fail('bad_arg');
    if (i < 0) users.push(u); else users[i] = u;
    if (!loginPossible(u)) webLogins.delete(u.number);
    if (body.new_code) { for (const [c, n] of Object.entries(CODES)) if (n === u.number) delete CODES[c]; CODES[body.new_code] = u.number; }
    return ok();
  }
  if (p === 'time' && 'epoch_ms' in body) {
    if (!clockJson().settable) return fail("This panel can't keep a time set by hand");
    if (!(body.epoch_ms >= Date.UTC(2025, 0, 1) && body.epoch_ms < Date.UTC(2100, 0, 1))) return fail('Pick a date from 2025 to 2099');
    clockOffsetMs = body.epoch_ms - Date.now();
    clockSource = 'manual';
    clockModule = 'ok';
    return ok();
  }
  if (p === 'time') { tz = body.timezone; ntp = body.ntp; return ok(); }
  if (p === 'wifi') return ok();
  if (p === 'wifi-scan') { scanDone = false; setTimeout(() => scanDone = true, 2500); return ok(); }
  if (p === 'network-check') { check = Date.now(); return ok(); }
  if (p === 'network-confirm') return ok();
  if (p === 'home_assistant') {
    const a = body.action;
    if (a === 'enable' || a === 'disable') ha.enabled = a === 'enable';
    if (a === 'pair') ha.window_s = 180;
    if (a === 'cancel' || a === 'reject') Object.assign(ha, {window_s: 0, code: ''});
    if (a === 'accept') Object.assign(ha, {window_s: 0, code: '', paired: true, client: 'Home Assistant 2026.9'});
    if (a === 'unpair') Object.assign(ha, {paired: false, client: ''});
    return ok();
  }
  if (p === 'updates') {
    runTick();
    const a = body.action;
    if (a === 'server_check') {
      const domain = serverDomain(body.domain);
      if (!domain) return fail('bad_domain', 400);
      if (runBusy()) return fail('update_running', 409);
      serverCheck = {state: 'checking', domain, result: '', key: null, trusted: false};
      setTimeout(() => finishServerCheck(domain), 300);
      return ok();
    }
    if (a === 'server') { // as panel_web.c: the password (only for a key the panel doesn't trust), the digest, a run, then the check
      const digest = String(body.key_sha256 || ''), trusted = shaTrusted(digest);
      if (!trusted) { const no = needPassword(s, body.me_password); if (no) return send(res, 403, no); }
      if (!/^[0-9a-f]{64}$/.test(digest) || !serverDomain(body.domain)) return fail('bad_request', 400);
      if (runBusy()) return fail('update_running', 409);
      const c = serverCheck, key = c && SERVERS[c.domain];
      if (!c || c.state !== 'done' || c.result !== 'ok' || c.domain !== body.domain || key.sha256 !== digest) return fail('stale', 409);
      if (factoryCode && !keyTrusted(key)) return fail('factory_code', 409);
      acceptedKey = key.sha256 === OWN_KEY ? null : key.sha256;
      Object.assign(updateServer, {domain: c.domain});
      serverCheck = null;
      withdrawn = false;
      return ok();
    }
    if (a === 'channel') {
      if (!['stable', 'beta', 'alpha', 'dev'].includes(body.channel)) return fail('bad_request', 400);
      if (runBusy()) return fail('update_running', 409);
      updateServer.channel = body.channel;
      return ok();
    }
    if (a === 'check') {
      if (runBusy()) return fail('busy', 409);
      Object.assign(run, {check: 'checking', since: Date.now(), last: null, live: true});
      return ok();
    }
    if (a === 'install') { // armed is "locked" above, before this
      if (runBusy()) return fail('busy', 409);
      if (!update.latest) Object.assign(update, {latest: UPDATE_TARGET, available: true});
      Object.assign(run, {stage: 'downloading', kind: 'release', since: Date.now(), percent: -1, last: null, check: '', reason: null, live: true});
      return ok();
    }
    if (a === 'cancel') {
      if (run.stage === 'stopping') return ok();
      if (['wifi_module', 'file', 'rolling_back'].includes(run.stage)) return fail('not_stoppable', 409);
      if (!['downloading', 'verifying', 'waiting', 'installing'].includes(run.stage)) return fail('no_update', 409);
      Object.assign(run, {stage: 'stopping', since: Date.now(), live: true, stopper: nameOf(s.user)});
      return ok();
    }
    return ok();
  }
  if (p === 'remote') { const no = needPassword(s, body.me_password); if (no) return send(res, 403, no); web.developer = body.developer; return ok(); }
  if (p === 'storage') return body.action === 'format' && body.confirm !== 'ERASE SD CARD' ? send(res, 409, {error: 'Type ERASE SD CARD to confirm'}) : ok();
  if (p === 'notification-test') return ok();
  if (p === 'ntfy-topic') return send(res, 200, {topic: 'alarm-' + crypto.randomBytes(9).toString('hex')});
  if (p === 'certificate-authority') return ok();
  if (p === 'sources/search') return send(res, 200, {candidates: sourceCandidates(body)});
  if (p === 'sources/learn') return ok();
  const m = /^binding\/(\d+)$/.exec(p);
  if (m) {
    const why = needInstaller(s);
    if (why) return fail(why);
    if (body.action === 'unbind') { bindings.delete(Number(m[1])); savedInputs.delete(Number(m[1])); } else {
      bindings.set(Number(m[1]), body.inputs[0].address);
      savedInputs.set(Number(m[1]), body.inputs.map(x => ({role: x.role, address: x.address, mapping: {hold_s: 0, ...x.mapping}})));
    }
    return ok();
  }
  if (p === 'installer-code') return needInstaller(s) ? fail(needInstaller(s)) : ok();
  if (p === 'factory-reset') {
    if (needInstaller(s)) return fail(needInstaller(s));
    const no = needPassword(s, body.me_password);
    return no ? send(res, 403, no) : ok();
  }
  if (p === 'developer/enroll') {
    const no = needPassword(s, body.me_password);
    return no ? send(res, 403, no) : send(res, 200, {id: '30eda0123456', host: body.host, port: 6055, certificate_der: 'MII...'});
  }
  return fail('Not included in this firmware', 404);
}

// ---- setting up a panel without a screen (web_setup.c's GET and POST /api/v1/setup) ----
const SETUP_KEY = 'KEYABCDEFGHJKMNPQRSTUVWXY';
const setupMode = process.env.MOCK_SETUP || '';
const setupMock = setupMode ? {
  stage: {unclaimed: 'unclaimed', key: 'key', network: 'network', check: 'network', confirm: 'network', recovery: 'ready'}[setupMode] || 'unclaimed',
  recovery: setupMode === 'recovery', via: setupMode !== 'confirm', claim: '', fails: 0,
  trialUntil: ['check', 'confirm'].includes(setupMode) ? Date.now() + 120000 : 0, joinedAt: setupMode === 'confirm' ? 0 : Date.now() + 3000,
  ssid: ['check', 'confirm'].includes(setupMode) ? 'Harbor House' : '',
} : null;
function setupStatus(s) {
  if (!setupMock) return {setup: false};
  const m = setupMock, now = Date.now();
  if (m.trialUntil && now >= m.trialUntil) { m.trialUntil = 0; m.ssid = ''; }
  const out = {setup: m.stage !== 'ready' || m.recovery, stage: m.stage, key_pending: m.stage === 'key', recovery: m.recovery,
    via_setup_wifi: m.via, panel_id: '02a0c4e1f9ab', setup_wifi: 'RoboAlarms-F9AB', setup_address: '192.168.4.1',
    fingerprint: '4f1c9e0b7a2d3c5e6f8091a2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6', name: 'Harbor House'};
  if (m.via || s) {
    const joined = m.ssid && now >= m.joinedAt;
    out.wifi = {state: !m.ssid ? 'none' : m.ssid === 'Wrong Password' ? 'password' : joined ? 'connected' : 'connecting', ssid: m.ssid, ip: joined ? '192.0.2.40' : ''};
    out.trial_s = m.trialUntil ? Math.ceil((m.trialUntil - now) / 1000) : 0;
  }
  return out;
}
function setupAction(body, s) {
  const m = setupMock, op = body.op, admin = s && ['installer', 'master'].includes(s.authority);
  if (!m) return [404, {error: 'not_found'}];
  if (op === 'claim') {
    if (m.stage !== 'unclaimed') return [409, {error: 'setup_stage'}];
    if (!m.via) return [403, {error: 'setup_from_setup_wifi'}]; // never from the home network
    if (m.claim) return [409, {error: 'setup_busy'}];
    m.claim = crypto.randomBytes(8).toString('hex');
    return [200, {ok: true, claim: m.claim}];
  }
  if (op === 'accounts') {
    if (m.stage !== 'unclaimed') return [409, {error: 'setup_stage'}];
    if (!m.claim || body.claim !== m.claim) return [403, {error: 'not_allowed'}];
    // The installer's access is required, the household's optional (added later under People).
    const owner = String(body.owner || '').trim(), installer = String(body.installer || '').trim();
    const household = !!(owner || body.owner_password);
    if (!installer || (household && (!owner || owner.toLowerCase() === installer.toLowerCase()))) return [400, {error: 'bad_name'}];
    if (household && body.owner_password === body.installer_password) return [400, {error: 'same_password'}];
    if (String(body.installer_password || '').length < 12 || (household && String(body.owner_password || '').length < 12)) return [400, {error: 'weak_password'}];
    for (const [k, v] of Object.entries(LOGINS)) if (v[1] === 1 || (household && v[1] === 2)) delete LOGINS[k];
    LOGINS[installer.toLowerCase()] = [body.installer_password, 1];
    webLogins.add(1);
    if (household) { LOGINS[owner.toLowerCase()] = [body.owner_password, 2]; webLogins.add(2); } else webLogins.delete(2);
    m.stage = 'key'; m.claim = '';
    return [200, {ok: true}];
  }
  if (op === 'key') return !admin ? [403, {error: 'not_allowed'}] : m.stage !== 'key' && !m.keyPending ? [409, {error: 'setup_stage'}]
    : [200, {ok: true, key: SETUP_KEY.match(/.{5}/g).join('-')}];
  if (op === 'key-saved') {
    if (!admin) return [403, {error: 'not_allowed'}];
    if (m.stage !== 'key' && !m.keyPending) return [409, {error: 'setup_stage'}];
    if (m.stage === 'key') m.stage = 'network';
    m.keyPending = false;
    return [200, {ok: true}];
  }
  if (op === 'network') {
    if (!admin) return [403, {error: 'not_allowed'}];
    if (m.stage !== 'network') return [409, {error: 'setup_stage'}];
    if (m.trialUntil > Date.now()) return [409, {error: 'setup_busy'}];
    m.ssid = String(body.ssid || ''); m.trialUntil = Date.now() + 120000; m.joinedAt = Date.now() + 3000;
    return [200, {ok: true}];
  }
  if (op === 'cancel') {
    if (!admin) return [403, {error: 'not_allowed'}];
    if (!m.trialUntil) return [409, {error: 'setup_stale'}];
    m.trialUntil = 0; m.ssid = '';
    return [200, {ok: true}];
  }
  if (op === 'confirm') {
    // From the setup Wi-Fi too: the board's own check (it joined) stands for the confirmation.
    if (!admin) return [403, {error: 'not_allowed'}];
    if (m.stage !== 'network' || !m.trialUntil || Date.now() < m.joinedAt) return [409, {error: 'setup_stale'}];
    m.stage = 'ready'; m.trialUntil = 0;
    return [200, {ok: true, ssid: m.ssid, ip: '192.0.2.40'}];
  }
  if (op === 'reset') {
    if (!m.recovery) return [403, {error: 'setup_recovery'}];
    const who = LOGINS[String(body.username || '').trim().toLowerCase()];
    if (String(body.key || '').toUpperCase().replace(/[\s-]/g, '') !== SETUP_KEY || !who) return [403, {error: 'wrong_key'}];
    if (String(body.password || '').length < 12) return [400, {error: 'weak_password'}];
    who[0] = body.password;
    m.keyPending = true;
    return [200, {ok: true}];
  }
  return [404, {error: 'not_found'}];
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x');
  if (req.method === 'GET' && FILES[url.pathname]) {
    const [file, type] = FILES[url.pathname];
    res.writeHead(200, {'Content-Type': type, 'Cache-Control': 'no-cache'});
    return fs.createReadStream(path.join(process.env.PANEL_WEB_DIR || __dirname, file)).pipe(res);
  }
  // The panel's own language (I18N-031): what a browser without a choice of its own opens in.
  if (req.method === 'GET' && url.pathname === '/i18n/panel')
    return send(res, 200, {locale: config.display.lang || 'en-US'});
  const catalog = req.method === 'GET' && url.pathname.match(/^\/i18n\/([^/]+)\.json$/);
  if (catalog) {
    const file = catalogFile(decodeURIComponent(catalog[1]));
    if (!file) return send(res, 404, {error: 'Not found'});
    res.writeHead(200, {'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-cache'});
    return fs.createReadStream(file).pipe(res);
  }
  if (!url.pathname.startsWith('/api/v1/')) return send(res, 404, {error: 'Not found'});
  const p = url.pathname.slice(8);
  runTick();
  if (run.offline && !p.startsWith('mock/')) { // the panel restarting: nothing answers
    if (req.socket) return req.socket.destroy();
    throw new TypeError('Failed to fetch');
  }
  if (p === 'setup') { // before the sign-in check: setup answers anyone (web_setup.c decides)
    if (req.method === 'GET') return send(res, 200, setupStatus(session(req)));
    const [code, answer] = setupAction(await readBody(req), session(req));
    return send(res, code, answer);
  }
  if (setupMock?.stage === 'unclaimed') return send(res, 403, {error: 'setup_first'});
  if (p === 'login' && req.method === 'POST') {
    const body = await readBody(req);
    await delay(300);
    // As the panel: shorter than any browser password (a keypad code) is refused without a try (B010).
    if (String(body.password || '').length < 12) return send(res, 400, {error: 'password_too_short'});
    const who = LOGINS[String(body.username || '').trim().toLowerCase()];
    if (!who || who[0] !== body.password) return send(res, 401, {error: 'invalid_login'});
    if (!webLogins.has(who[1])) return send(res, 401, {error: 'invalid_login'});
    const token = crypto.randomBytes(32).toString('hex'), s = {user: who[1], authority: authorityOf(who[1]), csrf: crypto.randomBytes(32).toString('hex'), confirmed: Date.now()};
    sessions.set(token, s);
    run.signedOut = false;
    res.setHeader('Set-Cookie', `panel_session=${token}; Path=/; HttpOnly; SameSite=Strict`);
    return send(res, 200, {csrf: s.csrf, user: s.user, must_change: temporary.has(s.user)});
  }
  if (req.method === 'POST' && p === 'mock/update') { // for tests only (before the sign-in check: the restart forgets it): set an update's step, or let time pass in it
    const body = await readBody(req);
    if (body.reset) Object.assign(run, {stage: '', kind: '', percent: -1, last: null, running: '0.6.1', confirming: false, check: '', reason: null, live: false, offline: false, signedOut: false});
    Object.assign(run, body.set || {});
    if (body.update) Object.assign(update, body.update);
    if (body.elapse) run.since -= body.elapse / UPDATE_SPEED;
    return send(res, 200, {ok: true});
  }
  const s = session(req);
  if (!s) return send(res, 401, {error: 'Sign in to this panel'});
  await delay(80);
  if (req.method === 'GET') return apiGet(p, s, res, url.searchParams);
  // For tests only (the firmware has none): the session's last password is long ago, or just now.
  if (req.method === 'POST' && p === 'mock/automation-warning') { auSetWarning((await readBody(req)).state || ''); return send(res, 200, {ok: true}); }
  if (req.method === 'POST' && p === 'mock/automation-reminder') { auSetReminder(!!(await readBody(req)).on); return send(res, 200, {ok: true}); }
  if (req.method === 'POST' && p === 'mock/keypad-event') { keypadEvent((await readBody(req)).event); return send(res, 200, {ok: true}); }
  if (req.method === 'POST' && p === 'mock/confirmation') { s.confirmed = (await readBody(req)).recent ? Date.now() : 0; return send(res, 200, {ok: true}); }
  if (req.method === 'POST' && p === 'backup') { // CFG-005: the installer, a recent password, a backup password
    const body = await readBody(req);
    const why = needInstaller(s);
    if (why) return send(res, 403, {error: 'not_allowed'});
    const no = needPassword(s, body.me_password);
    if (no) return send(res, 403, no);
    if (!/^[\x20-\x7e]{8,64}$/.test(String(body.password || ''))) return send(res, 400, {error: 'backup_password'});
    const file = backupSeal(body.password, backupPayload());
    res.writeHead(200, {'Content-Type': 'application/octet-stream', 'Content-Length': file.length, 'Cache-Control': 'no-store',
      'Content-Disposition': `attachment; filename="panel-backup-${new Date().toISOString().slice(0, 10)}.rabk"`});
    return res.end(file);
  }
  if (req.method === 'PUT' && p === 'backup') { // CFG-006: checked in full, then "restarting"
    const chunks = [];
    for await (const c of req) chunks.push(c); // the file is read first, so an early answer doesn't cut the upload
    if (s.authority !== 'installer') return send(res, 403, {error: 'not_allowed'});
    const no = needPassword(s, hexPassword(req.headers['x-panel-password'] || ''));
    if (no) return send(res, 403, no);
    const password = hexPassword(req.headers['x-backup-password']);
    if (password === null) return send(res, 400, {error: 'backup_decrypt'});
    const opened = backupOpen(password, Buffer.concat(chunks));
    if (opened.why) return send(res, 400, {error: opened.why});
    return send(res, 200, {ok: true, restarting: true, zones: 3, users: 4, network: true, zwave: false, ...opened});
  }
  if (req.method === 'PUT' && p === 'languages') { // the file is the body; the same people as the language itself
    const chunks = [];
    for await (const c of req) chunks.push(c);
    if (!ACC.shared(s.authority)) return send(res, 403, {error: 'not_allowed'});
    if (isLocked()) return send(res, 409, {result: 'locked'});
    const [code, answer] = languagesPut(Buffer.concat(chunks));
    return send(res, code, answer);
  }
  if (req.method === 'PUT' && p === 'voices') { // the signed .robovoice is the body (web_voice.c)
    const length = Number(req.headers['content-length'] || 0);
    const chunks = [];
    for await (const c of req) chunks.push(c);
    const [code, answer] = voicesPut(s, Buffer.concat(chunks), length || Buffer.concat(chunks).length);
    return send(res, code, answer);
  }
  if (req.method === 'POST' && p === 'mock/voice') { Object.assign(voice, await readBody(req)); return send(res, 200, {ok: true}); }
  if (req.method === 'POST') return apiPost(p, s, await readBody(req), res);
  if (req.method === 'PUT' && p === 'zwave-radio') { // the installer's own .gbl (network.md §14)
    if (authorityOf(s.user) !== 'installer') return send(res, 403, {error: 'The installer signed in is required'});
    const chunks = [];
    for await (const c of req) chunks.push(c);
    const file = Buffer.concat(chunks);
    const gbl = file.length >= 16 && file.readUInt32LE(0) === 0x03A617EB;
    if (!gbl) return send(res, 200, {result: 'invalid'});
    if (zwave.progress.op !== 'none' && zwave.progress.step !== 'done') return send(res, 200, {result: 'busy'});
    zwRadioUpdate(file.includes(Buffer.from('refuse')));
    return send(res, 200, {result: 'ok'});
  }
  return send(res, 405, {error: 'Method not allowed'});
});
// The remote keypad's WebSocket (mock-screen.cjs): a signed-in session only, as on the panel.
server.on('upgrade', (req, socket) => {
  const s = new URL(req.url, 'http://x').pathname === '/api/v1/screen' && session({headers: req.headers, method: 'GET'});
  if (!s || temporary.has(s.user)) {
    socket.end('HTTP/1.1 401 Unauthorized\r\n\r\n');
    return;
  }
  liveScreen.attach(req, socket, s, x => x.csrf);
});
server.listen(process.argv[2] !== undefined ? Number(process.argv[2]) : 8531, '127.0.0.1', () =>
  console.log(`Mock panel: http://127.0.0.1:${server.address().port}  (sign in: alex / "correct horse battery", installer / "installer password", pat / "partition master", sam / "standard user"; codes 1234 Alex, 4321 installer, 5555 Pat, 1111 Sam)`));
