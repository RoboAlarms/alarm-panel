'use strict';
// Panel Manager pages. Each page reads the panel's API and turns its numbers and bit masks
// into words (the same words the touchscreen uses: components/ui/alarm_ui/src/ui_text.c,
// ui_tz.c). The firmware's schemas decide which fields exist and their limits; the specs
// here only decide how they are shown. Core helpers are in panel.js.

// ---------------------------------------------------------------------------------------
// Words for the panel's numbers. Tables of words are functions, read when a page is drawn in the
// language then loaded (docs/features/37). Zone types, sensor kinds, user levels, troubles and
// time zones are the touchscreen's own words and contexts (components/ui/alarm_ui/src/ui_text.c,
// ui_tz.c, components/core/notify/src/notify_words.c): one translation serves both.

const zoneTypes = () => [
  [1, tc('zone type', 'Entry/exit 1'), t('The door you come in by: starts entry time 1')],
  [2, tc('zone type', 'Entry/exit 2'), t('A second door, like the garage: entry time 2')],
  [3, tc('zone type', 'Perimeter'), t('Doors and windows: alarm right away')],
  [4, tc('zone type', 'Interior follower'), t('Motion: waits while entry time runs')],
  [10, tc('zone type', 'Interior with delay'), t('Motion that starts entry time')],
  [5, tc('zone type', 'Trouble by day, alarm by night'), t("Doors that shouldn't open")],
  [24, tc('zone type', 'Silent burglary'), t('Reports an alarm without the siren')],
  [6, tc('zone type', '24-hour silent'), t('Hold-up button: always armed, silent')],
  [7, tc('zone type', '24-hour audible'), t('Panic button: always armed, siren')],
  [8, tc('zone type', '24-hour auxiliary'), t('Water, freezer, medical: keypad sounds')],
  [9, tc('zone type', 'Fire'), t('Smoke and heat detectors')],
  [16, tc('zone type', 'Fire with verification'), t('Resets once, alarms on a second trip')],
  [14, tc('zone type', 'Carbon monoxide'), t('CO detectors')],
  [12, tc('zone type', 'Monitor'), t('Logs and reports, never alarms')],
  [20, tc('zone type', 'Arm Stay button'), t('A keyfob or button that arms Stay')],
  [21, tc('zone type', 'Arm Away button'), t('A keyfob or button that arms Away')],
  [22, tc('zone type', 'Disarm button'), t('A keyfob or button that disarms')],
  [77, tc('zone type', 'Keyswitch'), t('Arms Away and disarms')],
  [29, tc('zone type', 'Momentary exit'), t('Restarts exit time when pressed')],
  [23, tc('zone type', 'No response'), t('Chime only (a doorbell)')],
];
const BUTTON_TYPES = [20, 21, 22, 77];
const zoneTypeName = type => type === 0 ? tc('zone type', 'Not used')
  : (zoneTypes().find(z => z[0] === type)?.[1] || t('Type {0}', type));
const deviceClasses = () => [[0, tc('sensor kind', 'Other sensor')], [1, tc('sensor kind', 'Door')],
  [2, tc('sensor kind', 'Window')], [3, tc('sensor kind', 'Garage door')], [4, tc('sensor kind', 'Motion')],
  [5, tc('sensor kind', 'Glass break')], [6, tc('sensor kind', 'Smoke')], [7, tc('sensor kind', 'Heat')],
  [8, tc('sensor kind', 'Carbon monoxide')], [9, tc('sensor kind', 'Gas')], [10, tc('sensor kind', 'Water leak')],
  [11, tc('sensor kind', 'Temperature')], [12, tc('sensor kind', 'Vibration')], [13, tc('sensor kind', 'Panic button')],
  [14, tc('sensor kind', 'Keyfob')], [15, tc('sensor kind', 'Keyswitch')]];
const deviceClassName = cls => deviceClasses().find(d => d[0] === cls)?.[1] || '';
const roles = () => ({
  1: [tc('user level', 'Installer'), t('Programs the alarm system and manages every user.')],
  2: [tc('user level', 'System master'), t('Manages users and household settings.')],
  3: [tc('user level', 'Partition master'), t('Manages users of their partitions.')],
  4: [tc('user level', 'Standard'), t('Arms and disarms.')],
  5: [tc('user level', 'Arm only'), t('Can arm but not disarm.')],
  6: [tc('user level', 'Guest'), t('Can only disarm what they armed.')],
  7: [tc('user level', 'Duress'), t('Disarms, and silently calls for help.')],
});
const troubles = () => [t('Mains power lost'), t('Panel battery low'), t('Reports not getting through'),
  t('Network down'), t('Clock not set'), t('Wi-Fi module not responding'), t('Sensor radio problem'),
  t('Siren problem'), t('Panel tamper'), t('Storage problem')];
const troubleBits = () => troubles().map((name, i) => [1 << i, name]);
const panicKeys = () => [[0, t('Police (siren)')], [1, t('Silent hold-up')], [2, tc('panic key', 'Fire')],
  [3, tc('panic key', 'Medical')]];
const sourceNames = () => ({1: t('Simulator'), 2: t('Wired terminals'), 3: nt('Zigbee'), 4: nt('Home Assistant'),
  5: t('Radio sensors'), 6: t('Keypad inputs')});
const sourceName = kind => sourceNames()[kind] || t('Source {0}', kind);
const notifyCats = () => [[1, t('Alarms')], [2, t('Troubles')], [4, t('Arming and disarming')], [8, t('Bypassed zones')],
  [16, t('System')]];
const notifyStates = () => [t('Off'), t('Waiting for the network'), t('Ready'), t('Sending'), t('Last one delivered'),
  t('Last one failed')];
const ACCENTS = ['', 'good', 'warn', 'info', 'violet', 'bad', 'warn', 'bad'];
const checkSteps = () => [t('Wi-Fi'), t('IP address'), t('Router'), t('Name lookup (DNS)'), t('Internet'), t('Time server'),
  t('Connected service')];
const checkStates = () => [[t('Waiting'), ''], [t('Checking'), 'info'], [t('Passed'), 'good'], [t('Failed'), 'bad'],
  [t('Warning'), 'warn'], [t('Skipped'), '']];
const updatePhases = () => [t('Idle'), t('Checking for updates'), t('Downloading'),
  t('Ready: installs once the panel is disarmed and quiet'), t('Restarting'), t('Failed')];
// The keypad's time-zone list, word for word (components/ui/alarm_ui/src/ui_tz.c).
const timezones = () => [
  [tc('time zone', 'Hawaii'), tc('time zone places', 'Honolulu · UTC-10'), 'HST10'], [tc('time zone', 'Alaska'), tc('time zone places', 'Anchorage · UTC-9'), 'AKST9AKDT,M3.2.0,M11.1.0'],
  [tc('time zone', 'Pacific Time'), tc('time zone places', 'Los Angeles, Vancouver, Seattle · UTC-8'), 'PST8PDT,M3.2.0,M11.1.0'], [tc('time zone', 'Arizona'), tc('time zone places', 'Phoenix (no daylight saving) · UTC-7'), 'MST7'],
  [tc('time zone', 'Mountain Time'), tc('time zone places', 'Denver, Calgary, Salt Lake City · UTC-7'), 'MST7MDT,M3.2.0,M11.1.0'], [tc('time zone', 'Central Time'), tc('time zone places', 'Chicago, Dallas, Winnipeg · UTC-6'), 'CST6CDT,M3.2.0,M11.1.0'],
  [tc('time zone', 'Mexico City'), tc('time zone places', 'Mexico City, Guadalajara · UTC-6'), 'CST6'], [tc('time zone', 'Eastern Time'), tc('time zone places', 'New York, Toronto, Miami · UTC-5'), 'EST5EDT,M3.2.0,M11.1.0'],
  [tc('time zone', 'Colombia and Peru'), tc('time zone places', 'Bogota, Lima · UTC-5'), '<-05>5'], [tc('time zone', 'Atlantic Time'), tc('time zone places', 'Halifax, Bermuda · UTC-4'), 'AST4ADT,M3.2.0,M11.1.0'],
  [tc('time zone', 'Chile'), tc('time zone places', 'Santiago · UTC-4'), '<-04>4<-03>,M9.1.6/24,M4.1.6/24'], [tc('time zone', 'Newfoundland'), tc('time zone places', 'St. John\'s · UTC-3:30'), 'NST3:30NDT,M3.2.0,M11.1.0'],
  [tc('time zone', 'Brazil and Argentina'), tc('time zone places', 'Sao Paulo, Buenos Aires · UTC-3'), '<-03>3'], [tc('time zone', 'UTC'), tc('time zone places', 'Coordinated Universal Time'), 'UTC0'],
  [tc('time zone', 'Iceland'), tc('time zone places', 'Reykjavik · UTC+0'), 'GMT0'], [tc('time zone', 'United Kingdom'), tc('time zone places', 'London, Edinburgh · UTC+0'), 'GMT0BST,M3.5.0/1,M10.5.0'],
  [tc('time zone', 'Ireland'), tc('time zone places', 'Dublin · UTC+0'), 'IST-1GMT0,M10.5.0,M3.5.0/1'], [tc('time zone', 'Portugal'), tc('time zone places', 'Lisbon · UTC+0'), 'WET0WEST,M3.5.0/1,M10.5.0'],
  [tc('time zone', 'West Africa'), tc('time zone places', 'Lagos, Kinshasa · UTC+1'), 'WAT-1'], [tc('time zone', 'Central Europe'), tc('time zone places', 'Berlin, Paris, Madrid, Rome, Amsterdam · UTC+1'), 'CET-1CEST,M3.5.0,M10.5.0/3'],
  [tc('time zone', 'Eastern Europe'), tc('time zone places', 'Athens, Helsinki, Kyiv, Bucharest · UTC+2'), 'EET-2EEST,M3.5.0/3,M10.5.0/4'], [tc('time zone', 'Egypt'), tc('time zone places', 'Cairo · UTC+2'), 'EET-2EEST,M4.5.5/0,M10.5.4/24'],
  [tc('time zone', 'South Africa'), tc('time zone places', 'Johannesburg, Harare · UTC+2'), 'SAST-2'], [tc('time zone', 'Turkey'), tc('time zone places', 'Istanbul · UTC+3'), '<+03>-3'],
  [tc('time zone', 'Moscow'), tc('time zone places', 'Moscow, Saint Petersburg · UTC+3'), 'MSK-3'], [tc('time zone', 'East Africa'), tc('time zone places', 'Nairobi, Addis Ababa · UTC+3'), 'EAT-3'],
  [tc('time zone', 'Iran'), tc('time zone places', 'Tehran · UTC+3:30'), '<+0330>-3:30'], [tc('time zone', 'Gulf'), tc('time zone places', 'Dubai, Abu Dhabi, Muscat · UTC+4'), '<+04>-4'],
  [tc('time zone', 'Pakistan'), tc('time zone places', 'Karachi, Lahore · UTC+5'), 'PKT-5'], [tc('time zone', 'India'), tc('time zone places', 'Mumbai, Delhi, Kolkata · UTC+5:30'), 'IST-5:30'],
  [tc('time zone', 'Nepal'), tc('time zone places', 'Kathmandu · UTC+5:45'), '<+0545>-5:45'], [tc('time zone', 'Bangladesh'), tc('time zone places', 'Dhaka · UTC+6'), '<+06>-6'],
  [tc('time zone', 'Indochina'), tc('time zone places', 'Bangkok, Hanoi, Jakarta · UTC+7'), '<+07>-7'], [tc('time zone', 'China'), tc('time zone places', 'Beijing, Shanghai · UTC+8'), 'CST-8'],
  [tc('time zone', 'Hong Kong'), tc('time zone places', 'Hong Kong · UTC+8'), 'HKT-8'], [tc('time zone', 'Singapore'), tc('time zone places', 'Singapore, Kuala Lumpur · UTC+8'), '<+08>-8'],
  [tc('time zone', 'Philippines'), tc('time zone places', 'Manila · UTC+8'), 'PST-8'], [tc('time zone', 'Western Australia'), tc('time zone places', 'Perth · UTC+8'), 'AWST-8'],
  [tc('time zone', 'Japan'), tc('time zone places', 'Tokyo, Osaka · UTC+9'), 'JST-9'], [tc('time zone', 'Korea'), tc('time zone places', 'Seoul · UTC+9'), 'KST-9'],
  [tc('time zone', 'South Australia'), tc('time zone places', 'Adelaide · UTC+9:30'), 'ACST-9:30ACDT,M10.1.0,M4.1.0/3'], [tc('time zone', 'Northern Territory'), tc('time zone places', 'Darwin · UTC+9:30'), 'ACST-9:30'],
  [tc('time zone', 'Queensland'), tc('time zone places', 'Brisbane (no daylight saving) · UTC+10'), 'AEST-10'], [tc('time zone', 'Eastern Australia'), tc('time zone places', 'Sydney, Melbourne, Canberra · UTC+10'), 'AEST-10AEDT,M10.1.0,M4.1.0/3'],
  [tc('time zone', 'New Zealand'), tc('time zone places', 'Auckland, Wellington · UTC+12'), 'NZST-12NZDT,M9.5.0,M4.1.0/3'],
];

const isInstaller = () => state?.authority === 'installer';
// The account line in the header: what the panel calls this person's code.
const roleWord = authority => ({installer: tc('user level', 'Installer'), master: tc('user level', 'System master'),
  partition_master: tc('user level', 'Partition master'), standard: t('Standard user')})[authority];

// Partitions the signed-in panel reports as in use: [[bit, name], ...].
function partitionBits() {
  return (state.partitions || []).map(p => [1 << (p.number - 1), p.name || tc('default partition name', 'Partition {0}', p.number)]);
}
function partitionNames(mask) {
  const bits = partitionBits();
  const names = bits.filter(([bit]) => mask & bit).map(b => b[1]);
  if (bits.length && names.length === bits.length) return bits.length > 1 ? t('All partitions') : names[0];
  return names.join(', ') || t('None');
}

// ---------------------------------------------------------------------------------------
// Shared page pieces

function settingsCard({id, title = '', description = '', specs, values, data = {}, readOnly = false,
                       note = t('Changes apply after you review them.'), button = t('Save changes')}) {
  const main = specs.filter(s => !s.advanced), advanced = specs.filter(s => s.advanced);
  return `<form class="card" id="${id}" novalidate>
    ${title ? `<div class="card-head"><div><h2>${esc(title)}</h2>${description ? `<p>${esc(description)}</p>` : ''}</div></div>` : ''}
    <fieldset${readOnly ? ' disabled' : ''}><div class="rows">${fieldRows(main, values, data)}</div>
    ${advanced.length ? `<details class="advanced"><summary>${ht('Advanced')}</summary><div class="rows">${fieldRows(advanced, values, data)}</div></details>` : ''}
    </fieldset>
    ${readOnly ? '' : `<div class="card-foot"><span class="note">${esc(note)}</span><button class="primary">${esc(button)}</button></div>`}</form>`;
}
function bindSettingsCard(id, specs, values, save) {
  const form = $('#' + id);
  if (!form) return;
  liveFields(form, specs);
  form.onsubmit = event => {
    event.preventDefault();
    try {
      const result = collect(form, specs, values);
      if (!result.changes.length) return toast(t('Nothing changed.'));
      save(result, form);
    } catch (error) {
      toast(error.message, 'error');
    }
  };
}
function installerNotice() {
  if (state.program) return notice('warn', t('Installer mode is open at the panel'),
    t('Changes from this browser wait until installer mode is closed on the keypad.'));
  return '';
}

// ---------------------------------------------------------------------------------------
// Live alarm pages: Overview, Zones and Troubles (docs/features/38 "The Overview mockup", BL-113).
// They read status, zone-status and history, redraw only when their words changed, and keep an
// open arming step, its switches and the focus across a redraw. Nothing flashes or animates
// (CONTRIBUTING.md "Display safety"): the countdown and its bar change once a second, that is all.

const ARM_LEVELS = ['away', 'stay', 'night'];
// ALARM_ZS_* (components/core/alarm_core/include/alarm/alarm_types.h)
const ZS = {fault: 0x01, bypassed: 0x02, alarm: 0x04, trouble: 0x08, tamper: 0x10, battery: 0x20, missed: 0x40};
const ZS_TROUBLE = ZS.trouble | ZS.tamper | ZS.battery | ZS.missed;
// What the live pages last read, and what the person has open on them.
// busy: the partitions with a command on its way (their buttons are drawn disabled until the answer),
// and 'ack' while Troubles' Acknowledge runs.
const live = {zones: null, history: [], deadlines: {}, states: '', sheet: null, showAll: new Set(), legend: false,
  redraw: null, badge: -1, busy: new Set()};
const offIf = busy => busy ? ' disabled' : '';
const drawn = {};

// The touchscreen's words for the levels (screen_main.xml's arming tiles, ui_app.c's headlines).
const levelName = l => ({away: tc('arming mode', 'Away'), stay: tc('arming mode', 'Stay'), night: tc('arming mode', 'Night')})[l] || '';
const levelHint = l => ({away: tc('arming mode', 'Everyone is leaving'), stay: tc('arming mode', 'Someone is home'),
  night: tc('arming mode', 'Going to bed')})[l] || '';
const armWords = l => ({away: t('Arm Away'), stay: t('Arm Stay'), night: t('Arm Night')})[l] || '';
const armingWords = l => ({away: t('Arming Away'), stay: t('Arming Stay'), night: t('Arming Night')})[l] || t('Arming');
const armedWords = l => ({away: t('Armed Away'), stay: t('Armed Stay'), night: t('Armed Night')})[l] || t('Armed');
const levelGlyph = l => icon(({away: 'away', stay: 'house', night: 'moon'})[l] || 'lock');

const partName = p => p.name || tc('default partition name', 'Partition {0}', p.number);
const zoneLabel = z => z.name || tc('default zone name', 'Zone {0}', z.zone);
const listWords = items => new Intl.ListFormat(wordsLocale(), {type: 'conjunction'}).format(items);
const zoneNames = list => listWords(list.map(zoneLabel));
const mmss = seconds => { const s = Math.max(0, Math.ceil(seconds)); return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`; };
const timeOfDay = (ms, seconds = false) => new Date(ms).toLocaleTimeString(wordsLocale(),
  seconds ? {hour: 'numeric', minute: '2-digit', second: '2-digit'} : {hour: 'numeric', minute: '2-digit'});
const isMotion = z => z.device_class === 4;
const isOpen = z => !!(z.flags & ZS.fault) && !(z.flags & ZS.bypassed);
// The zones that stop a level from arming: in the level, open and not bypassed.
const blockersOf = (zones, level) => zones.filter(z => (z.levels || []).includes(level) && isOpen(z));
// "Back door is open", "Hallway motion is active", "Back door and Kitchen window are open"
const openWords = list => list.length && list.every(isMotion)
  ? tn('{1} is active', '{1} are active', list.length, zoneNames(list))
  : tn('{1} is open', '{1} are open', list.length, zoneNames(list));
function zoneGlyph(z) {
  return icon(({1: 'door', 2: 'window', 3: 'garage', 4: 'motion', 5: 'glass', 6: 'smoke', 7: 'smoke', 8: 'co', 9: 'co',
    10: 'water'})[z.device_class] || 'shield');
}
// What a zone is now, most urgent first: the key into zoneMeans().
function zoneKey(z) {
  const f = z.flags || 0;
  if (f & ZS.alarm) return 'alarm';
  if (f & ZS.tamper) return 'tamper';
  if (f & ZS.missed || z.unreachable) return 'nocontact';
  if (f & ZS.battery) return 'low';
  if (f & ZS.trouble) return 'trouble';
  if (f & ZS.bypassed) return 'bypassed';
  if (f & ZS.fault) return isMotion(z) ? 'active' : 'open';
  return 'normal';
}
// Each state in the words a homeowner reads: [word, colour, what it means] ("What these words mean").
const zoneMeans = () => ({
  normal: [tc('zone state', 'Normal'), 'good', t('Closed and working')],
  active: [tc('zone state', 'Active'), 'warn', t('Motion right now')],
  open: [tc('zone state', 'Open'), 'warn', t('Blocks arming until closed or bypassed')],
  bypassed: [tc('zone state', 'Bypassed'), 'info', t('Off until the next disarm')],
  low: [tc('zone trouble', 'Low battery'), 'warn', t('Replace the battery soon')],
  trouble: [t('Check sensor'), 'warn', t('The sensor reports a problem')],
  nocontact: [t('No contact'), 'bad', t('Not heard from lately')],
  tamper: [tc('zone trouble', 'Tamper'), 'bad', t('Cover opened or moved')],
  alarm: [tc('zone state', 'Alarm'), 'bad', t('Set off the alarm')],
});
const partitionOf = number => (state?.partitions || []).find(p => p.number === number);

// ---- Troubles: GET troubles, what the touchscreen lists (BL-137, WORD-008) ----------------------
// The line under each system trouble is the touchscreen's own (components/core/notify/src/notify_words.c).
const troubleLines = () => [t('Running on battery'), t('Replace or recharge the backup battery'),
  t("A connected service can't be reached"), t('Wi-Fi is disconnected'), t('Connect Wi-Fi so the clock can sync'),
  t('The panel keeps protecting without the network'), t('Wireless sensors may not be heard'), t('Check the siren wiring'),
  t('The panel was opened'), t('Settings may not be saved')];
// ---- What each trouble means, and who fixes it ------------------------------------------------------
// [glyph, group, who fixes it, the page that does, what it means, the steps]. group: 'protect' weakens
// protection now, 'soon' doesn't yet. who: 'anyone' (a plug, a battery, a cover: the button only for those who
// may open its page), 'page' (done on that page: those who may open it) or 'installer'. The steps are for
// whoever fixes it: anyone else reads what it means and who does.
function trSystemKinds() {
  return [
    [icon('plugOff'), 'protect', 'anyone', 'health', t('The panel is running on its backup battery.'),
      [t('Check that the panel\'s power adapter is plugged in and its outlet has power.'), t('If the power is out in the house, the trouble ends by itself when it comes back.')]],
    [icon('batLow'), 'soon', 'anyone', 'health', t('The backup battery may not last through a power cut.'),
      [t('It charges while the mains power is on.'), t('If it is still low after a day, your installer replaces it.')]],
    [icon('cloudOff'), 'protect', 'page', 'integrations', t('Alarms and other reports aren\'t reaching a service the panel sends them to.'),
      [t('Open Integrations to see which one isn\'t connected.'), t('Check that service is running, and give its address or password again if they changed.')]],
    [icon('wifiOff'), 'soon', 'page', 'network', t('The panel keeps protecting, but can\'t send anything out.'),
      [t('Check that the Wi-Fi router is on.'), t('If the network\'s name or password changed, choose it again in Network.')]],
    [icon('clock'), 'soon', 'page', 'time', t('Times in the activity log and schedules may be wrong.'),
      [t('The panel sets its clock from the internet once Wi-Fi works.'), t('Or set the date and time by hand.')]],
    [icon('cpu'), 'soon', 'installer', 'health', t('The panel keeps protecting, but has no network.'),
      [t('See what System health last heard from it.'), t('If it stays, restart the panel.')]],
    [icon('radio'), 'protect', 'installer', 'zone-setup', t('Wireless sensors may not be heard while this lasts.'),
      [t('See how each source is doing at the end of Installer > Zones.'), t('Look for what may be jamming the radio nearby.')]],
    [icon('bellOff'), 'protect', 'installer', 'health', t('An alarm may not sound at the siren.'),
      [t('Check the siren and its wiring.')]],
    [icon('shieldAlert'), 'protect', 'installer', 'health', t('The panel\'s cover was opened or the panel was moved.'),
      [t('Check the panel for damage.'), t('Close its cover: the trouble ends by itself. Then acknowledge it.')]],
    [icon('drive'), 'soon', 'installer', 'storage', t('Changes to the settings may be lost at the next restart.'),
      [t('See what failed on Storage.')]],
  ];
}
// alarm_zone_trouble_t: loop, tamper, low battery, supervision, day fault
function trZoneKinds() {
  return [
    [icon('alert'), 'protect', 'anyone', 'zones', tc('zone trouble', 'Sensor fault'), t('The sensor reports a fault, so it may not notice anything.'),
      [t('Check that it is in place and undamaged.'), t('If the fault stays, your installer checks it.')]],
    [icon('shieldAlert'), 'protect', 'anyone', 'zones', tc('zone trouble', 'Tamper'), t('Its cover was opened, or it was taken off its mount.'),
      [t('Close its cover or put it back: the trouble ends by itself.')]],
    [icon('batLow'), 'soon', 'anyone', 'walk', tc('zone trouble', 'Low battery'), t('It works for now, but will stop when the battery runs out.'),
      [t('Replace its battery. The type is printed on the sensor or in its manual.'), t('Then trip it once, or run a walk test, so the panel hears it.')]],
    [icon('radio'), 'protect', 'anyone', 'walk', tc('zone trouble', 'Not reporting'), t('The panel hasn\'t heard from it in its check-in time, so it may miss it being set off.'),
      [t('Check its battery, and that it is in range of the panel.'), t('Trip it once, or run a walk test, to see whether the panel hears it.')]],
    [icon('doorOpen'), 'soon', 'anyone', 'zones', tc('zone trouble', 'Open during the day'), t('It was opened while the system was disarmed, and it is set to report that.'),
      [t('Close it. If it should stay open, your installer can change the zone.')]],
  ];
}
const TR_FAMILY = {system: 0, keypad: 1, zone: 2};
const TR_GROUP = {protect: 0, soon: 1};
const trLifeSafety = z => [6, 7, 8, 9, 10].includes(z?.device_class);

let trData = null; // GET troubles
let trSig = null; // its sig: status.trouble_sig moves when a line comes, goes or is restored
let trWanted = false; // the Overview or Troubles has shown the list: refreshStatus keeps it current
const trStale = () => trWanted && (!trData || (state?.trouble_sig ?? 0) !== trSig);
const trOpen = new Map(); // key -> open, as the person left it
async function trLoad() {
  try {
    const r = await api('troubles');
    trData = r.troubles || [];
    trSig = r.sig ?? 0;
  } catch { /* the next status poll tries again */ }
}
// After refreshStatus(), which has already reloaded a stale list: only a first visit loads here.
async function trEnsure() {
  trWanted = true;
  if (trStale()) await trLoad();
}
function trItems() {
  const sys = trSystemKinds(), zk = trZoneKinds(), lines = troubleLines(), titles = troubles();
  const zones = new Map((live.zones || []).map(z => [z.zone, z]));
  const out = (trData || []).map(x => {
    const key = `${x.family}-${x.kind}-${x.number}-${x.restored ? 'r' : 'a'}`;
    if (x.family === 'system') {
      const [glyph, group, who, page, means, steps] = sys[x.kind] || [icon('alert'), 'soon', 'installer', 'health', '', []];
      return {...x, key, glyph, group, who, page, means, steps, title: titles[x.kind] || t('System trouble {0}', x.kind), line: lines[x.kind] || '', life: false};
    }
    if (x.family === 'keypad')
      return {...x, key, glyph: icon('monitor'), group: 'soon', who: 'installer', page: 'partitions', means: t('Nobody can arm or disarm from it while it doesn\'t answer.'),
        steps: [t('Check that it has power, and that it is in range of the panel.'), t('See its state on Partitions and devices.')], title: t('Keypad {0} not responding', x.number),
        line: t('Check its power and signal'), life: false};
    const z = zones.get(x.number) || {zone: x.number};
    const [glyph, group, who, page, word, means, steps] = zk[x.kind] || zk[0];
    return {...x, key, glyph, group, who, page, means, steps, title: `${zoneLabel(z)} · ${word}`, line: t('Check the sensor'), life: trLifeSafety(z),
      bad: x.kind === 1};
  });
  for (const x of out) if (x.restored) x.line = t('Restored · acknowledge to clear');
  const rank = x => [x.restored ? 2 : TR_GROUP[x.group], x.life ? 0 : 1, TR_FAMILY[x.family], x.number, x.kind];
  return out.sort((a, b) => { const ra = rank(a), rb = rank(b); for (let i = 0; i < ra.length; i++) if (ra[i] !== rb[i]) return ra[i] - rb[i]; return 0; });
}

// "9:12 PM", "yesterday, 8:40 AM", "Tuesday, 7:02 PM", "Sep 28"
function trWhen(ms) {
  if (!ms) return '';
  const d = new Date(ms), now = new Date(), day = x => new Date(x.getFullYear(), x.getMonth(), x.getDate()).getTime();
  const days = Math.round((day(now) - day(d)) / 864e5), time = timeOfDay(ms);
  if (days <= 0) return time;
  if (days === 1) return t('yesterday, {0}', time);
  if (days < 7) return t('{0}, {1}', d.toLocaleDateString(wordsLocale(), {weekday: 'long'}), time);
  return d.toLocaleDateString(wordsLocale(), {month: 'short', day: 'numeric'});
}
// Who fixes it, when it isn't this person; else ''.
function trFixer(x) {
  if (x.who === 'installer') return state.authority === 'installer' ? '' : t('Your installer fixes this');
  return x.who === 'page' && !pageAllowed(PAGES[x.page]) ? t('A system master fixes this') : '';
}
// The button for the page that fixes it.
function trFix(x) {
  const words = x.page === 'walk' ? t('Run a walk test') : t('Open {0}', t(PAGES[x.page].title));
  return `<button type="button" class="small" data-go="${esc(x.page)}">${esc(words)}${icon('chev')}</button>`;
}
function trSteps(x) {
  const fixer = trFixer(x);
  if (fixer) return `<div class="actions">${tag(fixer)}</div>`;
  return `${x.steps.length > 1 ? `<ol>${x.steps.map(s => `<li>${esc(s)}</li>`).join('')}</ol>` : `<p>${esc(x.steps[0] || '')}</p>`}
    ${pageAllowed(PAGES[x.page]) ? `<div class="actions">${trFix(x)}</div>` : ''}`;
}
function trRow(x, alone) {
  const open = trOpen.has(x.key) ? trOpen.get(x.key) : alone;
  const when = x.since_ms ? (x.restored ? t('Ended {0}', trWhen(x.since_ms)) : t('Since {0}', trWhen(x.since_ms))) : '';
  return `<details class="tr-item${x.restored ? ' restored' : x.bad ? ' bad' : ''}" data-tr="${esc(x.key)}"${open ? ' open' : ''}>
    <summary><span class="tr-glyph">${x.restored ? icon('check') : x.glyph}</span>
      <div class="tr-main"><h3>${esc(x.title)}</h3><p>${esc(x.line)}</p></div>
      <span class="tr-when">${esc(when)}</span>${icon('chev', 'tr-chev')}</summary>
    <div class="tr-how">${x.restored
      ? `<p>${esc(t('It went away by itself. Acknowledge clears it from the list.'))}</p>`
      : `<p>${esc(x.means)}</p>${trSteps(x)}`}</div></details>`;
}
const troubleItems = trItems;
// The menu's count is the panel's (status.trouble_count: active and restored), no extra request.
const troubleBadge = () => state?.trouble_count || 0;

// ---- Reading the panel ---------------------------------------------------------------------------
async function loadZones() {
  live.zones = (await api('zone-status')).zones || [];
}
async function loadHistory() {
  live.history = (await api('history')).events || [];
}
const stateKey = () => (state?.partitions || []).map(p => `${p.number}:${p.state}:${p.alarm}:${p.memory}`).join();
// Exit and entry time: where each countdown ends here, kept steady unless the panel's differs by
// more than a second and a half (a poll's own delay shouldn't make it jump).
function noteDeadlines() {
  const next = {};
  for (const p of state?.partitions || []) {
    if ((p.state !== 'exit_delay' && p.state !== 'entry_delay') || p.alarm) continue;
    const left = Math.max(0, (p.delay_left_ms || 0) / 1000), old = live.deadlines[p.number], end = Date.now() + left * 1000;
    const total = p.state === 'exit_delay' ? (p.exit_delay_s || 0) * (p.silent_exit ? 2 : 1)
      : [...(p.entry_delay_s || [])].sort((a, b) => a - b).find(s => s >= Math.ceil(left)) || 0;
    next[p.number] = {state: p.state, end: old && old.state === p.state && Math.abs(old.end - end) < 1500 ? old.end : end,
      total: Math.max(total, Math.ceil(left), 1)};
  }
  live.deadlines = next;
}
// Exit time that has run out here, or has less than a second left: "Cancel arming" would reach a
// partition the panel may have armed meanwhile, and a disarm there asks first (WORD-005). The
// button waits for the next status instead (then the armed card, with its confirmation).
const EXIT_MARGIN_MS = 1000;
const exitRunOut = number => {
  const d = live.deadlines[number];
  return !!d && d.state === 'exit_delay' && d.end - Date.now() < EXIT_MARGIN_MS;
};
// After a change: read everything again and redraw the page on screen.
async function reloadData() {
  try {
    await Promise.all([refreshStatus(), loadZones(), loadHistory()]);
  } catch { /* the next poll tries again */ }
  noteDeadlines();
  live.states = stateKey();
}
async function reloadLive() {
  await reloadData();
  live.redraw?.(true);
}
// Redraws a live page's content when its words changed; the focused control (data-k) keeps focus.
function redraw(id, build, force = false) {
  const root = $('#' + id);
  if (!root) return;
  if (id === 'zones-live') live.legend = !!$('details.more', root)?.open;
  const html = build();
  if (!force && drawn[id] === html) return;
  const focused = root.contains(document.activeElement) ? document.activeElement?.dataset?.k : '';
  drawn[id] = html;
  root.innerHTML = html;
  $$('[data-go]', root).forEach(b => { b.onclick = () => go(b.dataset.go, b.dataset.arg || ''); });
  if (focused) $(`[data-k="${focused}"]`, root)?.focus({preventScroll: true});
  const badge = troubleBadge() + '/' + walkBadge();
  if (badge !== live.badge) { live.badge = badge; renderNav(); }
}

// The panel's refusal (409) in the touchscreen's words (notify_result_text in notify_words.c).
function refusalWords(error) {
  if (error.status !== 409) return error.message;
  const number = error.detail?.zone, z = (live.zones || []).find(x => x.zone === number);
  switch (error.code) {
    case 'not_ready':
      return number ? tn('{1} is open', '{1} are open', 1, z ? zoneLabel(z) : tc('default zone name', 'Zone {0}', number)) : t('Not ready: a zone is open');
    case 'not_bypassable': return t("That zone can't be bypassed");
    case 'bad_state': return t('Not possible right now');
    case 'held': return t('Wait: a sensor radio is updating');
    case 'disabled': return t('Not enabled');
    case 'not_authorized': return t("You can't do that here");
    default: return t('Not possible');
  }
}
// POST partition as the person signed in; a refusal is a toast. True when the panel did it.
async function sendPartition(body) {
  try {
    await api('partition', body);
    return true;
  } catch (error) {
    toast(refusalWords(error), 'error');
    return false;
  }
}
// One command per partition at a time: while one is on its way that partition's buttons are drawn
// disabled (a redraw meanwhile draws them so too) and a click on them does nothing, until the answer
// and the panel's new state are in.
async function act(body, control, build) {
  const number = body.number, focused = document.activeElement?.dataset?.k;
  if (live.busy.has(number)) return false;
  live.busy.add(number);
  if (control) control.disabled = true;
  live.redraw?.();
  let done = false;
  try {
    // build: the body made once this partition is marked busy (arming reads the zones again first)
    if (build) body = await build();
    if (body) {
      done = await sendPartition(body);
      await reloadData();
    }
  } finally {
    live.busy.delete(number);
    if (control) control.disabled = false;
  }
  live.redraw?.(true);
  // the redraw drew the button anew, disabled while the command ran: the focus goes back to it
  if (focused) $(`[data-k="${focused}"]`)?.focus({preventScroll: true});
  return done;
}

// ---------------------------------------------------------------------------------------
// Overview: the alarm command centre, one card per partition.

const choice = (cls, glyph, title, sub, attrs) => `<button type="button" class="choice ${cls}" ${attrs}>
  <span class="glyph">${glyph}</span><span><strong>${esc(title)}</strong>${sub ? `<small>${esc(sub)}</small>` : ''}</span>${icon('chev', 'chev')}</button>`;
const factsHtml = list => `<div class="facts">${list.map(([k, v]) => `<div><span>${esc(k)}</span><strong>${esc(v)}</strong></div>`).join('')}</div>`;
const timebar = number => `<div class="timebar"><progress data-bar="${number}" max="1" value="0" aria-label="${ht('Time left')}"></progress></div>`;
const countdown = number => `<span class="count countdown" data-count="${number}"></span>`;
// "30 s", "30–45 s": the entry times the level's zones start.
function entryRange(zones) {
  const secs = [...new Set(zones.map(z => z.entry_s).filter(s => s > 0))].sort((a, b) => a - b);
  if (!secs.length) return '';
  return secs.length > 1 ? t('{0}–{1} s', secs[0], secs[secs.length - 1]) : tn('{0} s', '{0} s', secs[0]);
}
// What a level watches, in a few words
function watchWords(level, zones) {
  const inLevel = l => zones.filter(z => (z.levels || []).includes(l));
  const stay = new Set(inLevel('stay').map(z => z.zone)), more = inLevel(level).filter(z => !stay.has(z.zone));
  if (!more.length) return t('Doors and windows');
  return level === 'away' ? t('Doors, windows and motion') : t('Doors, windows and {0}', zoneNames(more));
}
// Its newest event of a kind: [the panel's words, when], for "By Sam · keypad · 1 h ago".
function lastEvent(number, kind) {
  return live.history.find(e => e.partition === number && e.event === kind);
}
// The sensors that tripped since the partition was last armed, first to last, one row per zone. An
// alarm while disarmed (fire, CO, auxiliary) counts from the last disarm instead.
function alarmSequence(number, zones) {
  const events = live.history.filter(e => e.partition === number);
  const p = partitionOf(number), since = p?.alarm && p.state === 'disarmed' ? ['armed', 'disarmed'] : ['armed'];
  const armedAt = events.findIndex(e => since.includes(e.event));
  let list = (armedAt < 0 ? events : events.slice(0, armedAt)).filter(e => e.event === 'alarm');
  if (armedAt < 0) { const memory = new Set(zones.filter(z => z.flags & ZS.alarm).map(z => z.zone)); list = list.filter(e => memory.has(e.zone)); }
  const seen = new Set(), rows = [];
  for (const e of list.slice().reverse()) {
    if (seen.has(e.zone)) continue;
    seen.add(e.zone);
    const z = zones.find(x => x.zone === e.zone);
    rows.push({zone: e.zone, name: z ? zoneLabel(z) : e.body || tc('default zone name', 'Zone {0}', e.zone),
      glyph: z ? zoneGlyph(z) : icon('shield'), kind: z ? deviceClassName(z.device_class) : '', event: e});
  }
  return rows;
}
function afterFirst(first, e) {
  const secs = first.epoch_ms > 0 && e.epoch_ms > 0 ? (e.epoch_ms - first.epoch_ms) / 1000
    : first.boot === e.boot ? e.uptime_s - first.uptime_s : null;
  return secs === null ? fmtWhen(e).short : tc('time after the first sensor', '+{0}', fmtDuration(Math.max(0, secs)));
}
function sequenceHtml(number, seq) {
  if (!seq.length) return '';
  const all = live.showAll.has(number), rows = all ? seq : seq.slice(0, 5), first = seq[0].event;
  const sub = x => [x.kind, x.event.title !== first.title ? x.event.title : ''].filter(Boolean).join(' · ');
  return `<div class="sequence"><div class="sequence-head"><strong>${ht('What happened')}</strong><span>${esc(tn('{0} sensor', '{0} sensors', seq.length))}</span></div>
    <ol>${rows.map((x, n) => `<li class="${n === 0 ? 'first' : ''}"><span class="n">${n + 1}</span><span class="z">${x.glyph}</span>
      <div class="main"><strong>${esc(x.name)}${n === 0 ? `<span class="tag bad">${ht('First')}</span>` : ''}</strong><span>${esc(sub(x))}</span></div>
      <div class="when">${esc(n === 0 ? (first.epoch_ms > 0 ? timeOfDay(first.epoch_ms, true) : fmtWhen(first).short) : afterFirst(first, x.event))}</div></li>`).join('')}</ol>
    ${seq.length > 5 && !all ? `<button type="button" class="show-all" data-show-all="${number}" data-k="all-${number}">${ht('Show all {0}', seq.length)}</button>` : ''}</div>`;
}
// The arming step's "Bypass <zones>" switch belongs to the open zones it was turned on for (bypass_for).
// The panel's bypass_open bypasses whatever blocks when the command runs, so the switch goes back off
// whenever the blockers change (another zone opens, one closes), and Arm sends bypass_open only while
// the blockers are exactly those, each one bypassable.
const zoneList = list => list.map(z => z.zone).join(',');
const sheetZones = o => (live.zones || []).filter(z => z.partition === o.p);
const sheetBypass = (o, b) => !!o.bypass_open && b.length > 0 && b.every(z => z.bypassable) && o.bypass_for === zoneList(b);
function syncSheet() {
  const o = live.sheet;
  if (!o) return;
  const key = zoneList(blockersOf(sheetZones(o), o.level));
  if (o.blockers !== key) Object.assign(o, {blockers: key, bypass_open: false, bypass_for: ''});
}
// What Arm sends: Silent exit is Away's alone.
function armBody(o) {
  return {number: o.p, action: 'arm', level: o.level, no_entry_delay: !!o.no_entry_delay,
    silent_exit: o.level === 'away' && !!o.silent_exit, bypass_open: sheetBypass(o, blockersOf(sheetZones(o), o.level))};
}
// Arming, one level at a time: what it watches, only the options that level has, then the button
// (the touchscreen's arm pad). The step is the confirmation: arming asks nothing more.
function armSheet(p, zones) {
  const o = live.sheet, l = o.level, i = p.number;
  const inLevel = zones.filter(z => (z.levels || []).includes(l));
  const b = blockersOf(zones, l), canBypass = b.every(z => z.bypassable), bypassing = sheetBypass(o, b);
  const exitS = (p.exit_delay_s || 0) * (l === 'away' && o.silent_exit ? 2 : 1);
  const opts = [
    ['no_entry_delay', t('No entry delay'), t('Doors sound the alarm at once')],
    l === 'away' ? ['silent_exit', t('Silent exit'), t('No beeps · {0} to leave', mmss((p.exit_delay_s || 0) * 2))] : null,
    b.length && canBypass ? ['bypass_open', t('Bypass {0}', zoneNames(b)), t('Off until the next disarm')] : null,
  ].filter(Boolean);
  const on = k => k === 'bypass_open' ? bypassing : !!o[k];
  const stuck = b.length > 0 && !bypassing;
  const watched = inLevel.filter(z => !(z.flags & ZS.bypassed)).length - (bypassing ? b.length : 0);
  const range = entryRange(inLevel);
  const summary = [exitS ? t('{0} to leave', mmss(exitS)) : t('No exit time'),
    o.no_entry_delay || !range ? t('no entry delay') : t('{0} to get in', range),
    tn('{0} zone watched', '{0} zones watched', watched)].join(' · ');
  return `<div class="sheet ${l}">
    <div class="sheet-head"><span class="glyph">${levelGlyph(l)}</span>
      <div class="main"><h3>${esc(armWords(l))}</h3><p>${esc(watchWords(l, zones))} · ${esc(tn('{0} zone', '{0} zones', inLevel.length))}</p></div>
      <button type="button" class="small" data-sheet-close="${i}" data-k="back-${i}">${icon('back')}${ht('Back')}</button></div>
    ${stuck ? `<div class="notice warn">${icon('alert')}<div><strong>${esc(openWords(b))}</strong>${canBypass ? ''
      : `<p>${esc(tn('It can’t be bypassed', 'They can’t be bypassed', b.length))}</p>`}</div></div>` : ''}
    <div class="opts">${opts.map(([k, title, help]) => `<label class="opt"><span class="main"><strong>${esc(title)}</strong><small>${esc(help)}</small></span>
      <input type="checkbox" class="switch" data-sopt="${k}" data-k="opt-${i}-${k}"${on(k) ? ' checked' : ''}></label>`).join('')}</div>
    <p class="summary">${esc(summary)}</p>
    <div class="choices one">${choice(`fill ${l}`, icon('lock'), armWords(l), '', `data-go-arm="${i}" data-k="go-${i}"${offIf(stuck || live.busy.has(i))}`)}</div>
  </div>`;
}
// The partition's card. Without the buttons that arm or disarm for a person who may not arm (USR-001: the installer, status access.arm):
// the chime, a fire, CO or emergency alarm's Silence and the walk test's card stay.
function commandCard(p) {
  // an alarm, or the memory of one, comes before the walk test's card: fire, CO and 24-hour zones stay live in a test (WALK-007)
  if (p.walk_test && !p.alarm && !(p.state === 'disarmed' && p.memory)) return walkOverviewCard(p);
  const i = p.number, zones = (live.zones || []).filter(z => z.partition === i), off = offIf(live.busy.has(i));
  const noArm = state.access?.arm === false;
  const disarmButton = (label, attr = 'data-disarm', glyph = icon('lockOpen'), keep = false) => noArm && !keep ? '' :
    `<div class="choices one">${choice('fill go', glyph, label, '', `${attr}="${i}" data-k="${attr}-${i}"${off}`)}</div>`;
  const mode = p.alarm ? 'alarm' : p.state === 'disarmed' && p.memory ? 'memory' : p.state;
  let kind = '', glyph = icon('shield'), headline = tc('partition state', 'Unknown'), line = '', side = '', body = '';
  switch (mode) {
    case 'disarmed': {
      const ready = p.ready_levels || [], held = ARM_LEVELS.filter(l => !ready.includes(l)), free = ARM_LEVELS.filter(l => ready.includes(l));
      if (!held.length) {
        [kind, glyph, headline] = ['good', icon('shieldCheck'), t('Ready to arm')];
        line = [tn('{0} zone closed', 'All {0} zones closed', zones.length),
          p.bypassed ? tn('{0} zone bypassed', '{0} zones bypassed', p.bypassed) : ''].filter(Boolean).join(' · ');
      } else if (!free.length) {
        const open = zones.filter(isOpen);
        [kind, glyph, headline] = ['warn', icon('shieldAlert'), t('Not ready')];
        line = open.length ? openWords(open) : '';
      } else {
        const b = blockersOf(zones, held[0]);
        [kind, glyph, headline] = ['good', icon('shieldCheck'), t('Ready for {0}', listWords(free.map(levelName)))];
        line = b.length ? t('{0}: {1}', listWords(held.map(levelName)), openWords(b)) : '';
      }
      if (live.sheet?.p === i && !noArm) { body = armSheet(p, zones); break; }
      body = noArm ? '' : `<div class="choices">${ARM_LEVELS.map(l => {
        const b = blockersOf(zones, l), blocked = !ready.includes(l);
        const sub = !blocked ? levelHint(l) : !b.length ? t('Not ready') : b.every(isMotion) ? t('{0} active', zoneNames(b)) : t('{0} open', zoneNames(b));
        return choice(l + (blocked ? ' blocked' : ''), levelGlyph(l), levelName(l), sub, `data-arm="${l}" data-p="${i}" data-k="arm-${i}-${l}"${off}`);
      }).join('')}</div>`;
      body += `<div class="command-options"><label><input type="checkbox" class="switch" data-chime="${i}" data-k="chime-${i}"${(live.chime?.p === i ? live.chime.want : p.chime) ? ' checked' : ''}${off}> ${ht('Chime')}</label>
          <span class="note">${ht('Beeps when a door opens')}</span></div>`;
      break;
    }
    case 'exit_delay':
      [kind, glyph, headline] = ['info', icon('clock'), armingWords(p.level)];
      line = [p.level === 'away' ? t('Leave now through the exit door') : t('Stay inside, arming shortly'),
        p.silent_exit ? tc('exit time', 'silent') : ''].filter(Boolean).join(' · ');
      side = countdown(i);
      // disabled in the exit time's last second, until the next status says what the panel is now
      body = timebar(i) + (noArm ? '' : `<div class="choices one">${choice('neutral', icon('lockOpen'), t('Cancel arming'), '',
        `data-disarm="${i}" data-cancel="${i}" data-k="data-disarm-${i}"${offIf(live.busy.has(i) || exitRunOut(i))}`)}</div>`);
      break;
    case 'armed': {
      const inLevel = zones.filter(z => (z.levels || []).includes(p.level)), bypassed = zones.filter(z => z.flags & ZS.bypassed);
      const watching = inLevel.filter(z => !(z.flags & ZS.bypassed)).length, e = lastEvent(i, 'armed');
      [kind, glyph, headline] = ['info', icon('lock'), armedWords(p.level)];
      line = e ? [e.body, fmtWhen(e).short].filter(Boolean).join(' · ') : p.armed_by_name ? t('By {0}', p.armed_by_name) : '';
      body = factsHtml([[t('Watching'), tn('{1} of {0} zone', '{1} of {0} zones', inLevel.length, watching)],
        [t('Bypassed'), bypassed.length ? zoneNames(bypassed) : t('None')],
        [t('Entry delay'), p.no_entry_delay ? t('Off') : entryRange(inLevel) || t('None')]]) + disarmButton(t('Disarm'));
      break;
    }
    case 'entry_delay': {
      // The touchscreen's headline for entry time (ui_app.c: "Disarm now")
      const e = live.history.find(x => x.partition === i && (x.event === 'entry_delay' || x.event === 'armed'));
      const z = e?.event === 'entry_delay' ? zones.find(x => x.zone === e.zone) : zones.find(x => isOpen(x) && x.entry_s > 0);
      [kind, glyph, headline] = ['warn', icon('clock'), t('Disarm now')];
      line = z ? t('{0} opened', zoneLabel(z)) : e?.event === 'entry_delay' && e.body ? t('{0} opened', e.body) : '';
      side = countdown(i);
      body = timebar(i) + disarmButton(t('Disarm'));
      break;
    }
    case 'alarm': {
      // The headline: the touchscreen's for a fire, CO or auxiliary alarm (ui_app.c, notify_cause_title),
      // else the alarm's own title from the event log ("Burglary alarm", "Exit alarm", "Panic alarm").
      const seq = alarmSequence(i, zones), first = seq[0];
      const named = {fire: t('Fire alarm'), co: tc('alarm cause', 'Carbon monoxide'), aux: t('Emergency')}[p.alarm_kind];
      const fallback = {burglary: t('Burglary alarm'), panic: t('Panic alarm')}[p.alarm_kind] || tc('alarm cause', 'Alarm');
      [kind, glyph, headline] = ['bad', icon('siren'), named || first?.event.title || fallback];
      line = [first ? (seq.length > 1 ? tn('{1}, then {0} more', '{1}, then {0} more', seq.length - 1, first.name) : first.name) : '',
        first?.event.epoch_ms > 0 ? timeOfDay(first.event.epoch_ms) : '', p.siren ? t('Siren on') : ''].filter(Boolean).join(' · ');
      // Disarmed already (a fire, CO or auxiliary alarm): silencing is what's left, and asks nothing
      body = (p.state === 'disarmed' ? disarmButton(t('Silence'), 'data-silence', icon('bell'), true)
        : disarmButton(p.siren ? t('Disarm and silence') : t('Disarm'))) + sequenceHtml(i, seq);
      break;
    }
    case 'memory': {
      // Alarm memory. "Alarm canceled" only when the disarm stopped the report (docs/features/38 Research, round 2)
      const e = lastEvent(i, 'disarmed');
      [kind, glyph, headline] = ['warn', icon('bell'), p.canceled ? t('Alarm canceled') : t('Alarm occurred')];
      line = [p.canceled ? t('No alert was sent') : '', e?.title, e?.body, e ? fmtWhen(e).short : ''].filter(Boolean).join(' · ');
      body = sequenceHtml(i, alarmSequence(i, zones)) + disarmButton(t('Clear alarm'), 'data-clear', icon('check'));
      break;
    }
  }
  const keepsAll = p.alarm && p.state === 'disarmed'; // a fire, CO or emergency alarm: Silence acknowledges, it never disarms
  const noArmLine = !noArm || keepsAll ? '' : mode === 'memory' ? t('The installer doesn’t clear alarms. The people who use the alarm do.')
    : mode === 'disarmed' ? t('The installer doesn’t arm. The people who use the alarm do, at the panel or signed in here.')
    : t('The installer doesn’t disarm. The people who use the alarm do, at the panel or signed in here.');
  return `<section class="card command ${kind}">
    <div class="command-top"><span class="state-badge">${glyph}</span>
      <div class="state-words"><span class="part-name">${esc(partName(p))}</span><span class="state">${esc(headline)}</span>${line ? `<p>${esc(line)}</p>` : ''}</div>${side}</div>
    ${noArmLine ? `<p class="no-arm">${icon('info')}<span>${esc(noArmLine)}</span></p>` : ''}${body}</section>`;
}
// The four tiles below the cards: Zones, Troubles, Power and Connection, each opening its page.
function powerTile() {
  const power = state.health?.power, cut = (state.troubles || 0) & 1;
  if (!power) return ['', icon('plug'), t('Unknown'), t('This board does not report power.')];
  const left = t('{0}% left', power.percent);
  if (cut) return power.low ? ['bad', icon('batLow'), t('Battery low'), left] : ['warn', icon('batMedium'), t('On battery'), left];
  if (power.battery === 'present') {
    if (power.charging) return ['', icon('batCharging'), t('Plugged in'), t('Charging · {0}%', power.percent)];
    if (power.low) return ['warn', icon('batLow'), t('Plugged in'), t('Battery low')];
    return ['good', icon('batFull'), t('Plugged in'), power.percent >= 95 ? t('Battery full') : t('Battery {0}%', power.percent)];
  }
  return ['', icon('plug'), t('Plugged in'), power.battery === 'absent' ? t('No backup battery') : t('Battery state unknown')];
}
function connectionTile() {
  const w = state.wifi || {}, tr = state.troubles || 0;
  if (tr & 32) return ['warn', icon('wifiOff'), t('Offline'), t('Wi-Fi module not responding')];
  if (tr & 8 || !w.ssid) return ['warn', icon('wifiOff'), t('Offline'), t('Wi-Fi down')];
  return w.rssi && w.rssi < -75 ? ['warn', icon('wifi'), t('Weak signal'), w.ssid] : ['good', icon('wifi'), t('Online'), w.ssid];
}
function overviewTiles() {
  const zones = live.zones || [], open = zones.filter(isOpen), bypassed = zones.filter(z => z.flags & ZS.bypassed);
  const inAlarm = (state.partitions || []).filter(p => p.alarm).flatMap(p => alarmSequence(p.number, zones.filter(z => z.partition === p.number)));
  const zoneTile = inAlarm.length ? ['bad', tn('{0} zone in alarm', '{0} zones in alarm', inAlarm.length), t('{0} first', inAlarm[0].name)]
    : open.length ? ['warn', open.every(isMotion) ? tn('{0} zone active', '{0} zones active', open.length) : tn('{0} zone open', '{0} zones open', open.length), zoneNames(open)]
    : ['good', t('All closed'), bypassed.length ? tn('{0} zone bypassed', '{0} zones bypassed', bypassed.length) : tn('{0} zone', '{0} zones', zones.length)];
  const items = troubleItems();
  const troubleTile = items.length ? ['warn', tn('{0} trouble', '{0} troubles', items.length),
    items.length > 1 ? tc('the first trouble and how many more', '{0} +{1}', items[0].title, items.length - 1) : items[0].title]
    : ['good', t('None'), ''];
  const [pk, pg, pv, pd] = powerTile(), [nk, ng, nv, nd] = connectionTile();
  return [['zones', icon('grid'), t('Zones'), ...zoneTile], ['troubles', icon('alert'), t('Troubles'), ...troubleTile],
    ['health', pg, t('Power'), pk, pv, pd], [state.access?.shared ? 'network' : 'health', ng, t('Connection'), nk, nv, nd]];
}
function overviewBody() {
  // The page is always "Overview"; each card names its partition, one or several (BL-113)
  const parts = state.partitions || [];
  if (live.sheet && partitionOf(live.sheet.p)?.state !== 'disarmed') live.sheet = null;
  syncSheet();
  if (live.chime && !live.busy.has(live.chime.p)) live.chime = null;
  const events = live.history.slice(0, 6);
  return `<div class="page-head"><div><h1>${ht('Overview')}</h1><p data-now></p></div></div>
    ${state.program ? notice('info', t('Installer mode is open at the panel')) : ''}
    <div class="grid${parts.length > 1 ? ' two' : ''}">${parts.map(overviewPartition).join('')}</div>
    <div class="grid four gap">${overviewTiles().map(([to, glyph, label, kind, value, detail]) => `<a class="card tile ${kind}" href="#/${to}">
      ${icon('chev', 'go-to')}<span class="tile-label">${glyph}${esc(label)}</span><span class="tile-value">${esc(value)}</span><span class="tile-detail">${esc(detail)}</span></a>`).join('')}</div>
    <section class="section"><div class="section-head"><h2>${ht('Recent activity')}</h2><a href="#/activity">${ht('All activity')}</a></div>
    <div class="card"><ul class="list">${events.map(eventItem).join('') || `<li class="empty">${ht('Nothing recorded yet.')}</li>`}</ul></div></section>`;
}
// A partition's card exactly as it is (D19: every arm and disarm button stays), with an arming automation's warning card
// above it, or its put-off note (D20, D27), while that partition is disarmed and quiet - never while it is armed.
// Steady words, no button, nothing counts down: the put-off is at the panel, with a code (AUT-013). A reminder to arm
// (BL-224) adds its own steady line while it stands: "Not armed yet · reminder at 11:00 PM".
function overviewPartition(p) {
  const card = commandCard(p), extra = overviewAutoHtml(p);
  return extra ? `<div class="au-stack">${extra}${card}</div>` : card;
}
function overviewAutoHtml(p) {
  if (p.state !== 'disarmed' || p.alarm || p.walk_test) return '';
  const reminders = (state.automation_reminders || []).filter(r => r.partition === p.number)
    .map(r => `<p class="au-note">${icon('bell')}<span>${esc(t('Not armed yet · reminder at {0}', minuteText(r.minute)))}</span></p>`).join('');
  return reminders + (state.automation_warnings || []).filter(w => w.partition === p.number).map(w => {
    const at = w.at ? timeOfDay(w.at * 1000) : '';
    if (w.state === 'on') {
      const title = at ? ({away: t('Arming Away at {0}', at), stay: t('Arming Stay at {0}', at), night: t('Arming Night at {0}', at)})[w.level]
        : ({away: t('Arming Away soon'), stay: t('Arming Stay soon'), night: t('Arming Night soon')})[w.level];
      return title ? notice('warn', title, t('Set by an automation. Anyone can put it off at the panel, with their code.')) : '';
    }
    let line = '';
    if (w.state === 'hour' && at) line = ({away: t('Arms Away at {0} · put off by {1}', at, w.by), stay: t('Arms Stay at {0} · put off by {1}', at, w.by),
      night: t('Arms Night at {0} · put off by {1}', at, w.by)})[w.level] || '';
    else if (w.state === 'night') line = t('Won’t arm tonight · put off by {0}', w.by);
    else if (w.state === 'day') line = t('Won’t arm today · put off by {0}', w.by);
    return line ? `<p class="au-note">${icon('clock')}<span>${esc(line)}</span></p>` : '';
  }).join('');
}
// Once a second: the countdowns, their bars and the clock line (nothing else moves).
function tickOverview() {
  const root = $('#overview');
  if (!root) return;
  const now = $('[data-now]', root);
  if (now) now.textContent = new Date().toLocaleString(wordsLocale(), {weekday: 'long', month: 'long', day: 'numeric', hour: 'numeric', minute: '2-digit'});
  for (const [number, d] of Object.entries(live.deadlines)) {
    const left = Math.max(0, Math.ceil((d.end - Date.now()) / 1000));
    const count = $(`[data-count="${number}"]`, root), bar = $(`[data-bar="${number}"]`, root);
    if (count) count.textContent = mmss(left);
    if (bar) { bar.max = d.total; bar.value = Math.max(0, d.total - left); }
    const cancel = $(`[data-cancel="${number}"]`, root);
    if (cancel) cancel.disabled = live.busy.has(Number(number)) || exitRunOut(number);
  }
}
const redrawOverview = (force = false) => { redraw('overview', overviewBody, force); tickOverview(); };
// Asked before a normal disarm only: never for Cancel arming, entry time or an alarm, where
// a dialog would delay stopping the siren. The safe button has the focus.
function confirmDisarm(p) {
  const zones = (live.zones || []).filter(z => z.partition === p.number);
  const n = zones.filter(z => (z.levels || []).includes(p.level) && !(z.flags & ZS.bypassed)).length;
  openDialog(`<div class="dialog-head"><h2 id="dialog-title">${ht('Disarm {0}?', partName(p))}</h2>
    <p>${esc(tn('Stops watching {0} zone.', 'Stops watching {0} zones.', n))}${zones.some(z => z.always) ? ' ' + ht('Smoke, CO and water stay on.') : ''}</p></div>
    <div class="dialog-foot"><button type="button" id="keep-armed" autofocus>${ht('Keep armed')}</button>
    <button type="button" class="confirm" id="do-disarm">${icon('lockOpen')}${ht('Disarm')}</button></div>`);
  $('#keep-armed').onclick = closeDialog;
  $('#keep-armed').focus();
  $('#do-disarm').onclick = async () => {
    closeDialog();
    if (partitionOf(p.number)?.state === 'disarmed') return; // disarmed meanwhile: a second one would clear the memory
    await act({number: p.number, action: 'disarm'});
  };
}
// A walk test running on the partition, wherever it was started (WALK-024): who started it and from where,
// the count, and a link to the page; for someone who may not start one (no `shared` access), the count
// and End (asked first) instead of the link (WALK-025).
function walkOverviewCard(p) {
  const i = p.number, left = p.walk_remaining_s || 0, kind = left <= 300 ? 'warn' : 'info', shared = !!state.access?.shared;
  const action = shared ? choice('fill test', icon('walk'), t('Open walk test'), '', `data-go="walk" data-k="walk-${i}"`)
    : choice('neutral', icon('stop'), t('End walk test'), '', `data-walk-end="${i}" data-k="walk-end-${i}"${offIf(live.busy.has(i))}`);
  return `<section class="card command ${kind}">
    <div class="command-top"><span class="state-badge">${icon('walk')}</span>
      <div class="state-words"><span class="part-name">${esc(partName(p))}</span><span class="state">${ht('Walk test')}</span>
        <p>${esc(walkStartedWords(p.walk_started_from, walkWho(p.walk_started_by, p.walk_started_by_name)))}</p></div>
      <div class="wt-count"><span class="count">${p.walk_tested}<small>/${p.walk_total}</small></span><span class="summary">${ht('tested')}</span></div></div>
    <div class="timebar"><progress max="${Math.max(1, p.walk_total)}" value="${p.walk_tested}" aria-label="${ht('Zones tested')}"></progress></div>
    <p class="summary">${esc(tn('Arming waits for the test · ends in {0} min', 'Arming waits for the test · ends in {0} min', walkMinutes(left * 1000)))}</p>
    <div class="choices one">${action}</div></section>`;
}
function confirmWalkEnd(p) {
  openDialog(`<div class="dialog-head"><h2 id="dialog-title">${ht('End the walk test on {0}?', partName(p))}</h2>
    <p>${esc(tn('{1} of {0} zone tested so far.', '{1} of {0} zones tested so far.', p.walk_total, p.walk_tested))}</p></div>
    <div class="dialog-foot"><button type="button" id="keep-testing" autofocus>${ht('Keep testing')}</button>
    <button type="button" class="confirm" id="do-end-walk">${icon('stop')}${ht('End walk test')}</button></div>`);
  $('#keep-testing').onclick = closeDialog;
  $('#keep-testing').focus();
  $('#do-end-walk').onclick = async () => {
    closeDialog();
    if (!partitionOf(p.number)?.walk_test) return; // ended meanwhile: a second end says "not possible"
    await act({number: p.number, action: 'walk_test_end'});
  };
}
async function overviewClick(event) {
  const el = event.target.closest('button[data-arm], button[data-sheet-close], button[data-go-arm], button[data-disarm], button[data-clear], button[data-silence], button[data-show-all], button[data-walk-end]');
  if (!el || el.disabled) return;
  const d = el.dataset, root = $('#overview');
  // A command still on its way for this partition: its buttons wait for the answer
  if (live.busy.has(Number(d.goArm || d.disarm || d.clear || d.silence || d.walkEnd || d.p || 0))) return;
  if (d.arm) {
    live.sheet = {p: Number(d.p), level: d.arm, no_entry_delay: false, silent_exit: false, bypass_open: false, bypass_for: ''};
    redrawOverview();
    $(`[data-k="back-${d.p}"]`, root)?.focus();
  } else if (d.sheetClose) {
    const level = live.sheet?.level;
    live.sheet = null;
    redrawOverview();
    $(`[data-k="arm-${d.sheetClose}-${level}"]`, root)?.focus();
  } else if (d.goArm) {
    const o = live.sheet;
    if (!o) return;
    // The zones read again just before: the Bypass switch counts only for the blockers it was turned
    // on for, and a change since goes out without it (the panel then names what is open).
    const done = await act({number: o.p}, el, async () => {
      await loadZones().catch(() => {});
      syncSheet();
      return live.sheet === o ? armBody(o) : null;
    });
    if (done && live.sheet === o) live.sheet = null;
    redrawOverview(true);
  } else if (d.disarm) {
    // Decided on the newest state: an exit time that ran out meanwhile is an armed partition now (its
    // Disarm asks first); one that is running out waits for the next status.
    const p = partitionOf(Number(d.disarm));
    if (!p || (p.state === 'disarmed' && !p.alarm)) return;
    if (p.state === 'exit_delay' && exitRunOut(p.number)) return;
    if (p.state === 'armed' && !p.alarm) confirmDisarm(p);
    else await act({number: p.number, action: 'disarm'}, el);
  } else if (d.clear) {
    await act({number: Number(d.clear), action: 'clear'}, el);
  } else if (d.silence) {
    await act({number: Number(d.silence), action: 'acknowledge'}, el);
  } else if (d.showAll) {
    live.showAll.add(Number(d.showAll));
    redrawOverview();
  } else if (d.walkEnd) {
    const p = partitionOf(Number(d.walkEnd));
    if (p?.walk_test) confirmWalkEnd(p);
  }
}
async function overviewChange(event) {
  const el = event.target, d = el.dataset || {};
  if (d.sopt && live.sheet) {
    live.sheet[d.sopt] = el.checked;
    // the zones the switch was turned on for: the blockers the step shows now (syncSheet keeps them)
    if (d.sopt === 'bypass_open') live.sheet.bypass_for = el.checked ? live.sheet.blockers || '' : '';
    redrawOverview();
  } else if (d.chime) {
    const p = partitionOf(Number(d.chime));
    if (!p || p.chime === el.checked || live.busy.has(p.number)) { el.checked = p ? p.chime : el.checked; return; }
    live.chime = {p: p.number, want: el.checked};
    if (!await act({number: p.number, action: 'chime'}, el)) el.checked = p.chime;
  }
}
async function overviewPage() {
  live.sheet = null;
  const [, zones, history] = await Promise.all([refreshStatus(), api('zone-status').catch(() => null), api('history').catch(() => null)]);
  await trEnsure();
  live.zones = zones?.zones || [];
  live.history = history?.events || [];
  live.states = stateKey();
  noteDeadlines();
  renderNav();
  const html = overviewBody();
  return {
    html: `<div id="overview">${html}</div>`,
    bind() {
      const root = $('#overview');
      if (!root) return;
      drawn.overview = html;
      live.redraw = redrawOverview;
      root.addEventListener('click', overviewClick);
      root.addEventListener('change', overviewChange);
      tickOverview();
      poll(async () => {
        await refreshStatus();
        noteDeadlines();
        if (stateKey() !== live.states) { live.states = stateKey(); await loadHistory(); }
        redrawOverview();
      }, 2000);
      poll(async () => { await loadZones(); redrawOverview(); }, 3000);
      poll(async () => { await loadHistory(); redrawOverview(); }, 5000);
      poll(tickOverview, 1000);
    },
  };
}
function eventItem(event) {
  const when = fmtWhen(event);
  return `<li><span class="dot ${ACCENTS[event.accent] || ''}" aria-hidden="true"></span>
    <div class="main"><strong>${esc(event.title || titleCase(event.event))}</strong>${event.body ? `<span>${esc(event.body)}</span>` : ''}</div>
    <span class="side" title="${esc(when.long)}">${esc(when.short)}</span></li>`;
}

// ---------------------------------------------------------------------------------------
// Troubles: what needs fixing, the touchscreen's line for each and the page that fixes it.

function watches() {
  const tr = state.troubles || 0, power = state.health?.power, w = state.wifi || {};
  const battery = !power ? t('Unknown') : power.battery === 'present'
    ? (power.charging ? t('Charging · {0}%', power.percent) : power.low ? t('Battery low') : power.percent >= 95 ? t('Full') : t('{0}% left', power.percent))
    : power.battery === 'absent' ? t('Not fitted') : t('Unknown');
  const unheard = (live.zones || []).some(z => (z.flags & ZS.missed) || z.unreachable);
  const netDown = !!(tr & 8) || !w.ssid;
  return [
    [t('Mains power'), tr & 1 ? t('Out') : t('On'), !!(tr & 1)],
    [t('Backup battery'), battery, !!(tr & 2) || !!power?.low],
    [t('Network'), tr & 32 ? t('Wi-Fi module not responding') : netDown ? t('Wi-Fi down') : w.rssi && w.rssi < -75 ? t('Weak signal') : t('Connected'),
      !!(tr & 32) || netDown],
    [t('Reports'), tr & 4 ? t('Not getting through') : netDown ? t('Waiting') : t('Getting through'), !!(tr & 4)],
    [t('Clock'), tr & 16 ? t('Not set') : t('Set'), !!(tr & 16)],
    [t('Wireless sensors'), tr & 64 ? t('Radio problem') : unheard ? t('Not all heard') : t('All heard'), !!(tr & 64) || unheard],
    [t('Siren'), tr & 128 ? t('Problem') : t('Working'), !!(tr & 128)],
    [t('Storage'), tr & 512 ? t('Problem') : t('Saving'), !!(tr & 512)],
    [t('Panel cover'), tr & 256 ? t('Opened') : t('Closed'), !!(tr & 256)],
  ];
}
function troublesBody() {
  if (!trData) return `<div class="page-head"><div><h1>${ht('Troubles')}</h1><p>&nbsp;</p></div></div>`;
  const items = trItems(), active = items.filter(x => !x.restored), restored = items.length - active.length;
  const parts = (state?.partitions || []).filter(p => !p.alarm), beeping = parts.some(p => p.trouble_unack);
  const can = beeping || (restored > 0 && parts.some(p => p.state === 'disarmed'));
  const note = !items.length ? t('Everything is working') : restored
    ? `${t('{0} active', active.length)} · ${tn('{0} restored, waiting for acknowledgement', '{0} restored, waiting for acknowledgement', restored)}`
    : t('{0} active', active.length);
  const head = `<div class="page-head"><div><h1>${ht('Troubles')}</h1><p>${esc(note)}</p></div>
    ${can ? `<div class="actions"><button type="button" id="ack" data-k="ack"${offIf(live.busy.has('ack'))}>${icon('check')}${ht('Acknowledge')}</button></div>` : ''}</div>`;
  const armed = parts.some(p => p.state !== 'disarmed');
  const beeps = beeping ? `<div class="notice warn tr-beeps" role="status">${icon('bell')}<div><strong>${ht('The panel beeps for these troubles')}</strong>
    <p>${esc(restored ? (armed ? t('Acknowledge stops the beeps. The restored ones clear once the system is disarmed.') : t('Acknowledge stops the beeps and clears the restored ones.'))
      : t('Acknowledge stops the beeps.'))} ${esc(t('A trouble that is still there stays on this list until it is fixed.'))}</p></div></div>` : '';
  const groups = [['protect', t('Affects protection')], ['soon', t('Fix soon')], ['restored', t('Restored')]];
  const list = items.length ? `<div class="card tr-list">${groups.map(([g, label]) => {
    const rows = items.filter(x => (x.restored ? 'restored' : x.group) === g);
    return rows.length ? `<div class="tr-group">${esc(label)}</div>${rows.map(x => trRow(x, items.length === 1)).join('')}` : '';
  }).join('')}</div>` : notice('good', t('No troubles'));
  const checks = watches();
  const grid = `<div class="card checks-list">${checks.map(([label, now, wrong]) =>
    `<div>${icon(wrong ? 'alert' : 'check', wrong ? 'bad' : 'ok')}<div><strong>${esc(label)}</strong><span>${esc(now)}</span></div></div>`).join('')}</div>`;
  const watched = items.length
    ? `<details class="tr-checks"${trOpen.get('checks') ? ' open' : ''} data-tr="checks"><summary>${ht('What the panel watches')}</summary>${grid}</details>`
    : `<section class="section"><div class="section-head"><h2>${ht('What the panel watches')}</h2></div>${grid}</section>`;
  return head + beeps + list + watched;
}
const redrawTroubles = (force = false) => redraw('troubles-live', troublesBody, force);
// Acknowledge as at the touchscreen (code + OFF): the beeps and the restored troubles, on each disarmed
// partition with no alarm memory to see first and no walk test running; elsewhere the beeps only. Never a
// partition in alarm. Decided on a status read just before, as today.
async function acknowledgeTroubles(el) {
  if (live.busy.has('ack')) return;
  live.busy.add('ack');
  if (el) el.disabled = true;
  try {
    await refreshStatus();
    const restored = (trData || []).some(x => x.restored);
    for (const p of (state.partitions || []).filter(x => !x.alarm)) {
      const clear = p.state === 'disarmed' && !p.memory && !p.walk_test;
      if (!(clear && restored) && !p.trouble_unack) continue;
      if (!await sendPartition({number: p.number, action: clear ? 'clear' : 'acknowledge'})) break;
    }
    await reloadData();
  } catch { /* the next poll shows where things are */ } finally {
    live.busy.delete('ack');
  }
  live.redraw?.(true);
}
async function troublesPage() {
  await Promise.all([refreshStatus(), loadZones().catch(() => { live.zones = live.zones || []; })]);
  await trEnsure();
  renderNav();
  const html = troublesBody();
  return {
    html: `<div id="troubles-live">${html}</div>`,
    bind() {
      const root = $('#troubles-live');
      if (!root) return;
      drawn['troubles-live'] = html;
      live.redraw = redrawTroubles;
      root.addEventListener('click', event => {
        const el = event.target.closest('#ack');
        if (el && !el.disabled) acknowledgeTroubles(el);
      });
      // A row's open or closed state outlives the redraws ('toggle' doesn't bubble).
      root.addEventListener('toggle', event => {
        const key = event.target.dataset?.tr;
        if (key) trOpen.set(key, event.target.open);
      }, true);
      poll(async () => { await refreshStatus(); redrawTroubles(); }, 2000);
      poll(async () => { await loadZones(); redrawTroubles(); }, 3000);
    },
  };
}

// ---------------------------------------------------------------------------------------
// Remote keypad (the live screen itself is driven by panel-keypad.js)

function keypadPage() {
  const caps = state.capabilities;
  if (!caps.video?.length) return pageHead(t('Remote keypad')) + notice('info', t('This panel cannot share its screen.'));
  return {
    html: pageHead(t('Remote keypad'), t('The panel’s own screen: press its buttons here, and type on your keyboard when a text box or code pad is showing.')) +
      `<section class="card screen-card"><div class="screen-toolbar"><div><strong>${ht('Live screen')}</strong><p id="frame-status">${ht('Connecting…')}</p></div>
      <div class="screen-tools"><span class="muted" id="frame-rate"></span>${pictureChoiceHtml(caps)}</div></div>
      <div class="screen-stage"><div class="panel-shell" id="screen-shell"></div></div>
      <div class="frame-details"><span>${caps.width} × ${caps.height}</span><span id="control-state">${ht('Connecting to the panel…')}</span></div></section>
      <p class="gap muted"><small>${ht('Click to press a button; hold for a long press (Panic) and drag to scroll. While you are here the panel shows a red banner saying so, and the person at the panel can pause you from it. Settings on the panel still ask for their code, and when you leave, the panel signs out of anything you opened. If the video ever looks wrong, choose still pictures: this browser remembers it.')}</small></p>`,
    bind: bindKeypadPage,
  };
}

// ---------------------------------------------------------------------------------------
// Activity

let activityFilter = -1;
async function activityPage() {
  const data = await api('history');
  const filters = [[-1, t('All')], [0, t('Alarms')], [1, t('Troubles')], [2, t('Arming')], [3, t('Bypass')], [4, t('System')]];
  const list = () => {
    const events = data.events.filter(e => activityFilter < 0 || e.category === activityFilter);
    return events.map(eventItem).join('') || `<li class="empty">${ht('No events of this kind.')}</li>`;
  };
  return {
    html: pageHead(t('Activity'), t('The most recent events stored on the panel, newest first.')) +
      `<div class="filters" role="group" aria-label="${ht('Show')}">${filters.map(([v, l]) =>
        `<button type="button" data-filter="${v}" aria-pressed="${v === activityFilter}">${esc(l)}</button>`).join('')}</div>
      <div class="card"><ul class="list" id="events">${list()}</ul></div>`,
    bind() {
      $$('[data-filter]').forEach(b => b.onclick = () => {
        activityFilter = Number(b.dataset.filter);
        $$('[data-filter]').forEach(x => x.setAttribute('aria-pressed', String(x === b)));
        $('#events').innerHTML = list();
      });
    },
  };
}

// ---------------------------------------------------------------------------------------
// People and codes

// A page per person (BL-136, docs/features/31 WEB-017): #/people lists everyone, #/people/3 is one person,
// #/people/new adds one and #/people/3/password sets a new password for them.
// The touchscreen's tones (ui_authority_tone): installer amber, masters violet (info here), duress red.
const PC_TONE = {1: 'warn', 2: 'info', 3: 'info', 4: 'accent', 5: '', 6: '', 7: 'bad'};
const PC_ORDER = [4, 5, 6, 7, 3]; // the touchscreen's picker: the usual one first


function peoplePage(arg) { return arg ? pcPersonPage(arg) : pcListPage(); }
const pcUserNo = n => t('User {0}', String(n).padStart(2, '0'));
function pcRoleLine(n) {
  return ({1: t('Programs the panel, manages users; disarms only what it armed'), 2: t('Everything, including managing users'),
    3: t('Everything, and manages users of its area'), 4: t('Arms, disarms, bypasses, chime'), 5: t('Arms only, can’t disarm'),
    6: t('Disarms only what this code armed'), 7: t('Disarms and silently calls for help')})[n] || '';
}
const pcRoleName = n => roles()[n]?.[0] || t('Unknown');

async function pcListPage() {
  const data = await api('people');
  const users = data.users.sort((a, b) => a.number - b.number);
  const many = partitionBits().length > 1;
  return pageHead(t('People and codes'), t('Everyone with a keypad code. Codes and passwords are never shown, only replaced.'),
    data.can_add ? `<button type="button" class="primary" data-go="people" data-arg="new">${icon('plus')}${ht('Add a person')}</button>` : '') +
    `<div class="card"><ul class="list pc-list">${users.map(u => pcRow(u, many)).join('')}</ul></div>`;
}
function pcRow(u, many) {
  const name = u.name || tc('default user name', 'User {0}', u.number);
  const line = [pcUserNo(u.number), pcRoleName(u.authority), many ? partitionNames(u.partitions) : ''].filter(Boolean).join(' · ');
  const signIn = u.web_login ? (u.web_login_temporary ? tag(t('Waiting for their own password'), 'warn') : tag(t('Browser sign-in'), 'good')) : '';
  const inner = `<span class="pc-chip ${PC_TONE[u.authority] || ''}">${icon('user')}</span>
    <span class="main"><strong>${esc(name)}${u.you ? ' ' + tag(t('You'), 'info') : ''}</strong><span>${esc(line)}</span></span>${signIn}`;
  return u.manage ? `<li><a class="pc-person" href="#/people/${u.number}">${inner}${icon('chev', 'chev')}</a></li>`
    : `<li><div class="pc-person">${inner}<span class="chev"></span></div></li>`;
}

// ---- One person ----------------------------------------------------------------------------------------------------

// What the page is editing: the person as the panel had them, as changed here, and a new code or password.
const pc = {number: 0, isNew: false, self: false, before: null, now: null, code: '', password: '', setting: false, mustChange: false,
  codeMin: 4, fixedRole: false, listening: false};

async function pcPersonPage(arg) {
  const [which, mode] = String(arg).split('/');
  const data = await api('people');
  const back = ['people', t('People and codes')];
  const isNew = which === 'new';
  let number = Number(which);
  const user = isNew ? null : data.users.find(u => u.number === number);
  if (isNew) {
    if (!data.can_add) return pageHead(t('Add a person'), t('Only the installer and masters add people.'), '', back);
    number = 3;
    while (data.users.some(u => u.number === number) && number <= data.maximum) number++;
    if (number > data.maximum) return pageHead(t('Add a person'), t('Every user slot is in use.'), '', back);
  } else if (!user?.manage) {
    return pageHead(tc('default user name', 'User {0}', which), user ? t('You don’t manage this person.') : t('Nobody has this user number.'), '', back);
  }
  const all = partitionBits().reduce((mask, [bit]) => mask | bit, 0) || 1;
  const before = user ? {name: user.name || '', authority: user.authority, partitions: user.partitions, web_login: !!user.web_login,
    temporary: !!user.web_login_temporary} : {name: '', authority: 4, partitions: all, web_login: false, temporary: false};
  Object.assign(pc, {number, isNew, self: !!user?.you, before, now: {...before}, code: '', password: '', mustChange: false,
    setting: mode === 'password' && before.web_login, codeMin: data.code_min || 4, fixedRole: number <= 2 || !!user?.you});
  const title = isNew ? t('Add a person') : before.name || tc('default user name', 'User {0}', number);
  const sub = isNew ? t('{0}, the next free number', pcUserNo(number))
    : [pcUserNo(number), pcRoleName(before.authority), pc.self ? t('You') : ''].filter(Boolean).join(' · ');
  return {
    html: pageHead(title, sub, '', back) + `<form id="pc-body" novalidate>${pcBody()}</form><div id="pc-dock">${pcDock()}</div>`,
    bind() {
      const root = $('#pc-body');
      root.addEventListener('input', pcInput);
      root.addEventListener('change', pcInput);
      root.addEventListener('submit', event => event.preventDefault());
      if (!pc.listening) { $('#main').addEventListener('click', pcClick); pc.listening = true; } // #main outlives the page
      if (pc.setting) $('[data-k="password"]')?.focus();
    },
  };
}

// A row as fieldRow draws one: the label and its help on the left, the control on the right.
function pcRowHtml(id, label, help, control) {
  const helps = (Array.isArray(help) ? help : [help]).filter(Boolean).map(h => `<span class="help">${esc(h)}</span>`).join('');
  return `<div class="row"><div>${id ? `<label class="label" for="${id}">${esc(label)}</label>` : `<span class="label">${esc(label)}</span>`}${helps}</div>
    <div class="control">${control}</div></div>`;
}
const pcCard = (title, inner, cls = '') => `<section class="card gap pc-card ${cls}">${title ? `<div class="group-title">${esc(title)}</div>` : ''}${inner}</section>`;
const pcCanSignIn = () => WEB_ROLES.includes(pc.now.authority);

function pcBody() {
  return pcWhoCard() + pcRoleCard() + pcCodeCard() + `<div id="pc-login-card">${pcLoginCard()}</div>` + pcApartCard();
}
function pcWhoCard() {
  const help = pc.self ? t('Also your browser user name: changing it signs you out.')
    : WEB_ROLES.includes(pc.before.authority) ? t('Shown in the event log, and their browser user name') : t('Shown in the event log');
  const rows = [pcRowHtml('pc-name', t('Name'), help,
    `<input id="pc-name" data-f="name" data-k="name" maxlength="47" value="${esc(pc.now.name)}" placeholder="${ht('For example, Alex')}">`)];
  if (partitionBits().length > 1 && pc.number > 2 && !pc.self)
    rows.push(pcRowHtml('', t('Partitions'), t('Where their code works'), `<div class="checks">${partitionBits().map(([bit, l]) =>
      `<label><input type="checkbox" data-f="part" data-bit="${bit}"${pc.now.partitions & bit ? ' checked' : ''}>${esc(l)}</label>`).join('')}</div>`));
  return pcCard(pc.self ? t('You') : t('The person'), `<div class="rows">${rows.join('')}</div>`);
}
function pcRoleCard() {
  const n = pc.now.authority;
  if (pc.fixedRole) {
    const why = pc.number === 1 ? t('User 01 is always the installer.') : pc.number === 2 ? t('User 02 is always the system master.')
      : t('Whoever manages you can change it.');
    return pcCard(t('What this code can do'), `<div class="rows">${pcRowHtml('', pcRoleName(n), [pcRoleLine(n), why], '')}</div>`);
  }
  const choices = roleChoicesFor(pc.number).sort((a, b) => PC_ORDER.indexOf(a[0]) - PC_ORDER.indexOf(b[0]));
  return pcCard(t('What this code can do'), `<div class="opts" role="radiogroup" aria-label="${ht('What this code can do')}">${choices.map(([v, label]) =>
    `<label class="opt"><span class="pc-chip small ${PC_TONE[v] || ''}">${icon('user')}</span>
      <span class="main"><strong>${esc(label)}</strong><small>${esc(pcRoleLine(v))}</small></span>
      <input type="radio" name="pc-role" value="${v}" data-f="role" data-k="role-${v}"${v === n ? ' checked' : ''}></label>`).join('')}</div>`);
}
function pcCodeCard() {
  const range = t('{0} to {1} digits', pc.codeMin, 10);
  const help = pc.isNew ? t('{0}, typed at the keypad.', range) : pc.self ? t('{0}. Changing it signs you out here.', range)
    : t('{0}. Left empty, the code stays as it is.', range);
  return pcCard(pc.self ? t('Your code') : t('Their code'), `<div class="rows">${pcRowHtml('pc-code', pc.isNew ? t('Code') : t('New code'), help,
    `<input id="pc-code" type="password" inputmode="numeric" autocomplete="off" maxlength="10" data-f="code" data-k="code" value="${esc(pc.code)}"${pc.isNew ? '' : ` placeholder="${ht('Unchanged')}"`}>`)}</div>`);
}
const pcUserName = () => t('User name: {0} · a password, not the keypad code', pc.now.name.trim() || (pc.number === 1 ? 'Installer' : t('their name')));
function pcMustHelp() {
  return pc.mustChange ? t('They choose their own at their next sign-in, so only they know it. Until then the Panel Manager shows them nothing else.')
    : t('They keep this one. Give it to them somewhere private.');
}
function pcLoginCard() {
  const b = pc.before, on = pc.now.web_login;
  if (!pcCanSignIn()) {
    return b.web_login ? pcCard(t('Panel Manager'), `<div class="rows">${pcRowHtml('', t('Their browser sign-in ends'),
      t('Arm-only, guest and duress codes can’t sign in to the Panel Manager.'), '')}</div>`) : '';
  }
  if (pc.self) {
    return pcCard(t('Panel Manager'), `<div class="rows">${pcRowHtml('', t('Browser sign-in'), pcUserName(), tag(t('On'), 'good'))}
      ${pcRowHtml('', t('Your password'), t('It asks for your current one first.'),
        `<button type="button" class="small" data-pc="password" data-k="password-page">${ht('Change your password')}</button>`)}</div>`);
  }
  const waiting = b.web_login && b.temporary && on && !pc.setting ? tag(t('Waiting for their own password'), 'warn') : '';
  const rows = [`<div class="row"><div><label class="label" for="pc-login">${ht('Browser sign-in')}</label><span class="help" id="pc-user-name">${esc(pcUserName())}</span></div>
    <div class="control">${waiting}<input id="pc-login" type="checkbox" class="switch" data-f="login" data-k="login"${on ? ' checked' : ''}></div></div>`];
  let keep = '';
  if (on && b.web_login && !pc.setting) {
    rows.push(pcRowHtml('', t('Password'), b.temporary ? t('They choose their own when they next sign in.') : t('For when they’ve forgotten theirs.'),
      `<button type="button" class="small" data-pc="set" data-k="set">${ht('Set a new password')}</button>`));
  }
  if (on && (pc.setting || !b.web_login)) {
    rows.push(pcRowHtml('pc-password', b.web_login ? t('New password') : t('Password'), t('At least 12 characters. A few unrelated words are easy to remember.'),
      `<input id="pc-password" type="password" autocomplete="new-password" spellcheck="false" maxlength="64" data-f="password" data-k="password" value="${esc(pc.password)}">`));
    rows.push(`<label class="opt"><span class="main"><strong>${ht('Ask them to change it when they sign in')}</strong>
      <small id="pc-must-help">${esc(pcMustHelp())}</small></span>
      <input type="checkbox" class="switch" data-f="must" data-k="must"${pc.mustChange ? ' checked' : ''}></label>`);
    if (b.web_login) keep = `<p class="pc-keep"><button type="button" class="link" data-pc="keep" data-k="keep">${ht('Keep their current password')}</button></p>`;
  }
  return pcCard(t('Panel Manager'), `<div class="rows">${rows.join('')}${keep}</div>`);
}
function pcApartCard() {
  if (pc.isNew || pc.number <= 2 || pc.self) return '';
  return pcCard('', `<div class="rows">${pcRowHtml('', t('Remove this person'), t('Their code and browser sign-in stop working at once.'),
    `<button type="button" class="danger" data-pc="remove" data-k="remove">${ht('Remove')}</button>`)}</div>`, 'pc-apart');
}

// ---- Editing: every control writes the model; the bar says what changed --------------------------------------------

function pcInput(event) {
  const el = event.target, f = el.dataset?.f;
  if (!f) return;
  if (f === 'name') {
    pc.now.name = el.value;
    const line = $('#pc-user-name');
    if (line) line.textContent = pcUserName();
  } else if (f === 'part') {
    pc.now.partitions = $$('[data-f="part"]').reduce((mask, x) => x.checked ? mask | Number(x.dataset.bit) : mask, 0);
  } else if (f === 'role') {
    if (event.type !== 'change') return;
    pc.now.authority = Number(el.value);
    return pcDrawLogin();
  } else if (f === 'code') pc.code = el.value;
  else if (f === 'password') pc.password = el.value;
  else if (f === 'login') {
    if (event.type !== 'change') return;
    pc.now.web_login = el.checked;
    pc.setting = false;
    pc.password = '';
    return pcDrawLogin(el.checked && !pc.before.web_login ? 'password' : 'login');
  } else if (f === 'must') {
    pc.mustChange = el.checked;
    const help = $('#pc-must-help');
    if (help) help.textContent = pcMustHelp();
  }
  pcDockDraw();
}
function pcClick(event) {
  const el = event.target.closest('[data-pc]');
  if (!el || el.disabled || !$('#pc-body')) return;
  const act = el.dataset.pc;
  if (act === 'set') { pc.setting = true; return pcDrawLogin('password'); }
  if (act === 'keep') { Object.assign(pc, {setting: false, password: '', mustChange: false}); return pcDrawLogin('set'); }
  if (act === 'password') return go('password');
  if (act === 'undo') {
    Object.assign(pc, {now: {...pc.before}, code: '', password: '', setting: false, mustChange: false});
    $('#pc-body').innerHTML = pcBody();
    eyes($('#pc-body'));
    return pcDockDraw();
  }
  if (act === 'save') return pcSave();
  if (act === 'remove') return pcRemove();
}
// Draws the Panel Manager card again (the switch moved, the role changed), keeping the focus.
function pcDrawLogin(focus = '') {
  const box = $('#pc-login-card');
  if (box) { box.innerHTML = pcLoginCard(); eyes(box); }
  pcDockDraw();
  if (focus) $(`[data-k="${focus}"]`)?.focus();
}

const pcOnOff = on => on ? t('On') : t('Off');
function pcChanges() {
  const out = [], b = pc.before, n = pc.now, fresh = pc.isNew;
  const add = (label, from, to) => { if (fresh || from !== to) out.push({label, from: fresh ? '' : from, to}); };
  add(t('Name'), b.name || t('(empty)'), n.name || t('(empty)'));
  if (!pc.fixedRole) add(t('What this code can do'), pcRoleName(b.authority), pcRoleName(n.authority));
  if (partitionBits().length > 1 && pc.number > 2 && !pc.self) add(t('Partitions'), partitionNames(b.partitions), partitionNames(n.partitions));
  if (pc.code) out.push({label: t('Code'), from: '', to: fresh ? t('Chosen, not shown') : t('Changed, not shown')});
  const webOn = n.web_login && pcCanSignIn();
  if (webOn !== b.web_login) out.push({label: t('Browser sign-in'), from: fresh ? '' : pcOnOff(b.web_login), to: pcOnOff(webOn)});
  if (webOn && pc.password) out.push({label: t('Browser password'), from: '',
    to: pc.mustChange ? t('New: they change it when they sign in') : t('New: theirs to keep')});
  return out;
}
function pcDock() {
  const changes = pcChanges();
  if (!pc.isNew && !changes.length) return '';
  const top = pc.isNew ? t('New person · {0}', pcUserNo(pc.number)) : tn('{0} change', '{0} changes', changes.length);
  const line = pc.isNew ? [pc.now.name || t('No name yet'), pcRoleName(pc.now.authority), pc.code ? t('code chosen') : t('no code yet')].join(' · ')
    : listWords(changes.map(c => c.label));
  return `<div class="pc-dock"><span class="main"><strong>${esc(top)}</strong><span>${esc(line)}</span>
    <span class="pc-error" id="pc-error" role="alert"></span></span>
    ${pc.isNew ? '' : `<button type="button" data-pc="undo" data-k="undo">${ht('Undo')}</button>`}
    <button type="button" class="primary" data-pc="save" data-k="save">${pc.isNew ? ht('Add this person') : ht('Save')}</button></div>`;
}
function pcDockDraw() { const dock = $('#pc-dock'); if (dock) dock.innerHTML = pcDock(); }

// ---- Saving: straight from the bar, as the signed-in person; only removing someone asks first -----------------------

// The panel acts as the signed-in person (BL-135): no keypad code in any of these requests.
function pcError(message) { const line = $('#pc-error'); if (line) line.textContent = message; }
async function pcSave() {
  const n = pc.now, b = pc.before, webOn = n.web_login && pcCanSignIn();
  const stop = (message, k) => { pcError(message); $(`[data-k="${k}"]`)?.focus(); };
  const digits = /^[0-9]+$/.test(pc.code) && pc.code.length >= pc.codeMin && pc.code.length <= 10;
  if (pc.isNew && !pc.code) return stop(t('Choose a code for them.'), 'code');
  if (pc.code && !digits) return stop(t('A code has {0} to {1} digits.', pc.codeMin, 10), 'code');
  if (webOn && (pc.setting || !b.web_login) && !pc.password)
    return stop(b.web_login ? t('Type the new password, or keep their current one.') : t('Choose a browser password for them.'), 'password');
  if (webOn && pc.password && pc.password.length < 12) return stop(t('A browser password has at least 12 characters.'), 'password');
  const number = pc.number, self = pc.self, code = pc.code, temporary = pc.mustChange;
  const password = webOn ? pc.password : '';
  const record = pc.isNew || n.name !== b.name || n.authority !== b.authority || n.partitions !== b.partitions || !!code;
  const button = $('[data-pc="save"]');
  if (button) button.disabled = true;
  pcError('');
  hideSecrets($('#pc-body')); // the boxes hide again once the form is sent
  try {
    if (record) await api('people', {number, name: n.name, authority: n.authority, partitions: n.partitions, new_code: code, action: 'save'});
    if (password) await api('people', {number, password, temporary, action: 'web-password'});
    else if (!webOn && b.web_login) await api('people', {number, action: 'web-off'});
    if (code || password) toast(t('Saved.')); // the page never shows a code or a password: say it worked
    go('people', String(number));
  } catch (error) {
    pcError(error.message);
    if (button) button.disabled = false;
  }
}
function pcRemove() {
  const name = pc.before.name || tc('default user name', 'User {0}', pc.number), number = pc.number;
  review({
    title: t('Remove {0}?', name), intro: t('Their code and browser sign-in stop working at once. This can’t be undone.'),
    confirm: t('Remove'), danger: true, done: '',
    run: () => api('people', {number, action: 'delete'}),
    after: () => go('people'),
  });
}

// The roles the signed-in person can hand out: the installer and the system master make
// partition masters too; a partition master only the lower roles (the engine's own rule).
function roleChoicesFor(target) {
  const all = roles();
  if (target === 1) return [[1, all[1][0], all[1][1]]];
  if (target === 2) return [[2, all[2][0], all[2][1]]];
  const lowest = state.authority === 'partition_master' ? 4 : 3;
  return Object.entries(all).filter(([n]) => Number(n) >= lowest).map(([n, [label, help]]) => [Number(n), label, help]);
}
const WEB_ROLES = [1, 2, 3, 4]; // who can have a browser sign-in: installer, masters, standard

// ---------------------------------------------------------------------------------------
// Integrations

const integrations = () => ({
  home_assistant: [nt('Home Assistant'), t('Pair the panel with your own Home Assistant server.'), t('Home automation')],
  mqtt: [nt('MQTT'), t('Publish alarm events to a broker; optionally accept commands.'), t('Home automation')],
  ntfy: [nt('ntfy'), t('Push alerts to phones through ntfy.sh or your own server.'), t('Phone notifications')],
  pushover: [nt('Pushover'), t('Push alerts, with alarms that repeat until acknowledged.'), t('Phone notifications')],
});
async function integrationsPage() {
  const ids = state.capabilities.integrations || [];
  if (!ids.length) return pageHead(t('Integrations')) + notice('info', t('No integrations are included in this firmware.'));
  const INTEGRATIONS = integrations();
  const info = Object.fromEntries(await Promise.all(ids.map(id =>
    (id === 'home_assistant' ? api(id) : api('config/' + id)).then(d => [id, d]).catch(() => [id, null]))));
  const status = id => {
    const d = info[id];
    if (!d) return tag(t('Unknown'));
    if (id === 'home_assistant') return !d.enabled ? tag(t('Off')) : d.paired ? tag(t('Paired'), 'good') : tag(t('Not paired'), 'warn');
    return d.values.enabled ? tag(t('On'), 'good') : tag(t('Off'));
  };
  const detail = id => {
    const d = info[id];
    if (id === 'mqtt' && d?.values.enabled) return t('Broker {0}', `${d.values.host}:${d.values.port}`);
    if (id === 'home_assistant' && d?.paired) return t('Paired with {0}', d.client);
    return '';
  };
  const other = t('Other');
  const groups = [...new Set(ids.map(id => INTEGRATIONS[id]?.[2] || other))];
  return pageHead(t('Integrations'), t('Connect the panel to home automation and to your phone. Everything here is optional.')) +
    groups.map(group => `<section class="section"><div class="section-head"><h2>${esc(group)}</h2></div>
      <div class="grid two">${ids.filter(id => (INTEGRATIONS[id]?.[2] || other) === group).map(id => `
        <article class="card service"><div class="service-top"><h3>${esc(INTEGRATIONS[id]?.[0] || id)}</h3>${status(id)}</div>
        <p>${esc(INTEGRATIONS[id]?.[1] || '')}</p>${detail(id) ? `<small class="muted">${esc(detail(id))}</small>` : ''}
        <div class="actions"><button type="button" data-go="integration" data-arg="${id}">${ht('Set up')}</button></div></article>`).join('')}
      </div></section>`).join('');
}

const mqttSpecs = () => [
  {name: 'enabled', label: t('Connect to an MQTT broker'), type: 'switch'},
  {name: 'host', label: t('Broker address'), type: 'text', placeholder: t('broker.local or 192.168.1.10')},
  {name: 'port', label: t('Port'), type: 'number', help: t('Usually 1883 without encryption, 8883 with.')},
  {name: 'tls', label: t('Encryption'), type: 'select', numeric: true,
    options: [[0, t('Off: home network only')], [1, t('On: certificate from a public authority')], [2, t('On: my own certificate authority')]]},
  {name: 'user', label: t('User name'), type: 'text', help: t('Leave blank if the broker needs no sign-in.')},
  {name: 'password', label: t('Password'), type: 'secret', setFlag: 'password_set'},
  {name: 'discovery', label: t('MQTT discovery'), type: 'switch', help: t('Announce the panel and its zones so home-automation software finds them.')},
  {name: 'commands', label: t('Allow arming and disarming over MQTT'), type: 'switch', help: t('Off: the broker only receives status.')},
  // docs/features/14: panics set off alarms, so this one is the installer's (BL-107)
  {name: 'allow_panic', label: t('Allow panic alarms over MQTT'), type: 'switch', showIf: v => v.commands, locked: !isInstaller(),
    help: t('Police, fire, medical and silent panic. Only the installer can change this.')},
  {name: 'prefix', label: t('Topic prefix'), type: 'text', advanced: true},
  {name: 'panel_id', label: t('Panel identifier'), type: 'text', advanced: true, help: t('Part of every topic. Change it only if several panels share a broker.')},
];
const ntfySpecs = () => [
  {name: 'enabled', label: t('Send notifications with ntfy'), type: 'switch'},
  {name: 'self_hosted', label: t('Use my own ntfy server'), type: 'switch', help: t('Off: the public ntfy.sh service.')},
  {name: 'server', label: t('Server address'), type: 'text', placeholder: nt('https://ntfy.example.com'), showIf: v => v.self_hosted},
  {name: 'topic', label: t('Private topic'), type: 'secret', setFlag: 'topic_set',
    help: t('Anyone who knows the topic can read the alerts. Use “Make a new topic” below for a random one.')},
  {name: 'token', label: t('Access token'), type: 'secret', setFlag: 'token_set', help: t('Only if your server requires one.')},
  {name: 'cats', label: t('Send'), type: 'bits', options: notifyCats()},
  {name: 'silent_too', label: t('Include silent alarms'), type: 'switch', help: t('Duress and silent panic. Leave off if other people see this phone.')},
];
const pushoverSpecs = () => [
  {name: 'enabled', label: t('Send notifications with Pushover'), type: 'switch'},
  {name: 'user_key', label: t('User key'), type: 'secret', setFlag: 'user_key_set'},
  {name: 'app_token', label: t('Application token'), type: 'secret', setFlag: 'app_token_set'},
  {name: 'cats', label: t('Send'), type: 'bits', options: notifyCats()},
  {name: 'silent_too', label: t('Include silent alarms'), type: 'switch', help: t('Duress and silent panic. Leave off if other people see this phone.')},
  {name: 'emergency', label: t('Repeat alarms until acknowledged'), type: 'switch', help: t('Pushover emergency priority for burglary, fire and CO alarms.')},
  {name: 'sound', label: t('Notification sound'), type: 'text', advanced: true, help: t('A Pushover sound name. Blank uses your Pushover default.')},
];

async function integrationPage(id) {
  if (id === 'home_assistant') return homeAssistantPage();
  const specsFor = {mqtt: mqttSpecs, ntfy: ntfySpecs, pushover: pushoverSpecs}[id];
  if (!specsFor || !(state.capabilities.integrations || []).includes(id))
    return pageHead(t('Not available'), '', '', ['integrations', t('Integrations')]) + notice('info', t('This integration is not included in this firmware.'));
  const data = await api('config/' + id);
  const specs = withSchema(specsFor(), data.fields);
  const INTEGRATIONS = integrations();
  const provider = id === 'ntfy' ? 0 : 1;
  const delivery = id === 'mqtt' ? null : await api('notification-status').catch(() => null);
  const status = delivery?.providers?.find(p => p.provider === provider);
  const tools = [];
  if (id === 'ntfy') tools.push(`<button type="button" id="new-topic">${ht('Make a new topic')}</button>`);
  if (id !== 'mqtt') tools.push(`<button type="button" id="send-test">${ht('Send a test notification')}</button>`);
  if (id === 'mqtt' || id === 'ntfy') tools.push(`<button type="button" id="set-ca">${ht('Certificate authority…')}</button>`);
  return {
    html: pageHead(INTEGRATIONS[id][0], INTEGRATIONS[id][1], '', ['integrations', t('Integrations')]) +
      settingsCard({id: 'config-form', specs, values: data.values, data}) +
      `<section class="card gap"><div class="card-head"><div><h2>${id === 'mqtt' ? ht('Tools') : ht('Delivery')}</h2>
        ${status ? `<p>${esc(notifyStates()[status.state] || t('Unknown'))} · ${esc(tn('{0} notification sent since start-up', '{0} notifications sent since start-up', status.sent))}${status.detail ? ' · ' + esc(status.detail) : ''}</p>` : ''}
        ${id === 'mqtt' ? `<p>${data.ca_loaded ? ht('A private certificate authority is saved.') : ht('No private certificate authority is saved.')}</p>` : ''}
        ${delivery?.test_result ? `<p>${ht('Last test: {0}', delivery.test_result)}</p>` : ''}</div></div>
        <div class="card-foot">${tools.join('')}</div></section>`,
    bind() {
      bindSettingsCard('config-form', specs, data.values, ({changes, values}) =>
        review({changes, run: () => api('config/' + id, {values})}));
      if ($('#new-topic')) $('#new-topic').onclick = async () => {
        try {
          const result = await api('ntfy-topic', {});
          const input = $('#config-form [name=topic]');
          input.type = 'text';
          input.value = result.topic;
          toast(t('New topic filled in. Save changes to use it, then subscribe to it in the ntfy app.'));
        } catch (error) { toast(error.message, 'error'); }
      };
      if ($('#send-test')) $('#send-test').onclick = () => review({
        title: t('Send a test notification?'), intro: t('Every phone subscribed to this service gets it.'),
        confirm: t('Send'), done: t('Sent. Check the phone.'), run: async () => {
          const result = await api('notification-test', {provider});
          if (result.error) throw new Error(result.error);
        },
      });
      if ($('#set-ca')) $('#set-ca').onclick = () => certificateDialog(id);
    },
  };
}
function certificateDialog(provider) {
  openDialog(`<form id="ca-form"><div class="dialog-head"><h2 id="dialog-title">${ht('Certificate authority')}</h2>
    <p>${ht('Paste the PEM certificate of the authority that signed your {0} server’s certificate. Leave empty to remove the saved one.', integrations()[provider][0])}</p></div>
    <div class="dialog-body"><textarea name="pem" rows="10" maxlength="8192" spellcheck="false" placeholder="-----BEGIN CERTIFICATE-----"></textarea></div>
    <p class="dialog-error" id="dialog-error" role="alert"></p>
    <div class="dialog-foot"><button type="button" id="dialog-cancel">${ht('Cancel')}</button><button class="primary">${ht('Save')}</button></div></form>`);
  $('#dialog-cancel').onclick = closeDialog;
  $('#ca-form').onsubmit = async event => {
    event.preventDefault();
    try {
      const result = await api('certificate-authority', {provider, pem: event.target.pem.value});
      if (result.error) throw new Error(friendly(result.error));
      closeDialog();
      toast(t('Certificate authority saved.'));
      show();
    } catch (error) { $('#dialog-error').textContent = error.message; }
  };
}
async function homeAssistantPage() {
  const h = await api('home_assistant');
  const pairing = h.enabled && !h.paired && (h.window_s > 0 || h.code);
  const [kind, words] = !h.enabled ? ['', t('Off')] : h.paired ? ['good', t('Paired with {0}', h.client || nt('Home Assistant'))]
    : h.code ? ['warn', t('Compare the codes')] : h.window_s > 0 ? ['info', t('Waiting for Home Assistant')] : ['warn', t('Not paired')];
  const actions = [];
  if (!h.enabled) actions.push(['enable', t('Turn on'), 'primary']);
  if (h.code) actions.push(['accept', t('The codes match: pair'), 'primary'], ['reject', t('They do not match'), 'danger']);
  else if (h.enabled && !h.paired && !h.window_s) actions.push(['pair', t('Start pairing'), 'primary']);
  if (h.window_s > 0 && !h.code) actions.push(['cancel', t('Stop pairing'), '']);
  if (h.paired) actions.push(['unpair', t('Unpair'), 'danger']);
  if (h.enabled) actions.push(['disable', t('Turn off'), '']);
  const confirmText = {unpair: t('Home Assistant loses access until you pair again.'), disable: t('Home Assistant stops receiving the panel.'),
    reject: t('The pairing request is refused.')};
  return {
    html: pageHead(nt('Home Assistant'), t('A direct, encrypted link to your own Home Assistant server. No cloud account.'), '', ['integrations', t('Integrations')]) +
      `<section class="card status-card ${kind}"><span class="state">${esc(words)}</span>
      ${h.code ? `<p>${hx('Home Assistant shows a code. Pair only if it is {0}.', `<strong class="mono">${esc(h.code)}</strong>`)}</p>` : ''}
      ${!h.code && h.window_s > 0 ? `<p>${ht('Add the panel in Home Assistant now. This window closes in {0}.', fmtDuration(h.window_s))}</p>` : ''}
      ${!h.enabled ? `<p>${ht('Turn the link on, then start pairing.')}</p>` : ''}
      ${h.enabled && !h.paired && !pairing ? `<p>${ht('Start pairing here, then add the panel from Home Assistant’s integrations page.')}</p>` : ''}
      <div class="actions gap">${actions.map(([a, label, cls]) => `<button type="button" class="${cls}" data-ha="${a}">${esc(label)}</button>`).join('')}</div></section>`,
    bind() {
      $$('[data-ha]').forEach(button => button.onclick = async () => {
        const action = button.dataset.ha;
        const run = async () => { await api('home_assistant', {action}); };
        if (confirmText[action]) return review({title: t('{0}?', button.textContent), intro: confirmText[action],
          confirm: button.textContent, danger: true, done: t('Done.'), run});
        button.disabled = true;
        try { await run(); show(); } catch (error) { toast(error.message, 'error'); button.disabled = false; }
      });
      if (pairing) poll(async () => { if (!$('#dialog').open) await show(); }, 3000);
    },
  };
}

// ---------------------------------------------------------------------------------------
// Panel settings: display and sound, network, time, storage

const saverTimes = () => [[0, t('Never')], [30000, tn('After {0} second', 'After {0} seconds', 30)],
  [60000, tn('After {0} minute', 'After {0} minutes', 1)], [120000, tn('After {0} minute', 'After {0} minutes', 2)],
  [300000, tn('After {0} minute', 'After {0} minutes', 5)], [600000, tn('After {0} minute', 'After {0} minutes', 10)],
  [900000, tn('After {0} minute', 'After {0} minutes', 15)], [1800000, tn('After {0} minute', 'After {0} minutes', 30)],
  [3600000, tn('After {0} hour', 'After {0} hours', 1)]];
// A time of day as the panel keeps it, minutes after midnight, in this browser's clock: the
// night hours in quarter hours, as the panel's own rows step (docs/features/07 UI-043).
const minuteText = m => timeOfDay(new Date(2026, 0, 1, Math.floor(m / 60), m % 60).getTime());
const dayTimes = () => Array.from({length: 96}, (_, i) => [i * 15, minuteText(i * 15)]);
const displaySpecs = () => [
  // How the screen looks, as on the panel's Screen and sound (docs/features/07 UI-010, BL-132):
  // the choices are the panel's; High contrast works with any of them.
  {name: 'appearance', label: t('Appearance'), type: 'select',
    help: t('Automatic is dark during the night hours and light by day; with the night hours off, it stays light.')},
  {name: 'high_contrast', label: t('High contrast'), type: 'switch', help: t('Stronger colors, every button outlined.')},
  {name: 'brightness', label: t('Screen brightness'), type: 'range', unit: '%'},
  {name: 'saver_after_ms', label: t('Screen saver'), type: 'select', numeric: true, options: saverTimes(),
    unknown: v => t('After {0}', fmtDuration(v / 1000)), help: t('Dims the screen when nobody is using it.')},
  {name: 'saver_brightness', label: t('Screen saver brightness'), type: 'range', unit: '%', help: t('0 turns the screen dark.'),
    showIf: v => v.saver_after_ms > 0},
  {name: 'off_after_ms', label: t('Screen off'), type: 'select', numeric: true, options: saverTimes(),
    unknown: v => t('After {0}', fmtDuration(v / 1000)),
    help: t('The screen goes dark when nobody touches it, counted like the screen saver; a touch wakes it.')},
  {name: 'night_on', label: t('Night hours'), type: 'switch',
    help: t('A dimmer screen that goes dark sooner, for a keypad in a bedroom. Needs the panel to know the time.')},
  {name: 'night_start', label: t('Night starts'), type: 'select', numeric: true, options: dayTimes(),
    unknown: minuteText, showIf: v => v.night_on},
  {name: 'night_end', label: t('Night ends'), type: 'select', numeric: true, options: dayTimes(),
    unknown: minuteText, showIf: v => v.night_on},
  {name: 'night_brightness', label: t('Brightness at night'), type: 'range', unit: '%', showIf: v => v.night_on},
  {name: 'night_off_after_ms', label: t('Screen off at night'), type: 'select', numeric: true, options: saverTimes(),
    unknown: v => t('After {0}', fmtDuration(v / 1000)), showIf: v => v.night_on,
    help: t('An alarm still lights the screen at the normal brightness.')},
  {name: 'chime_volume', label: t('Chime volume'), type: 'range', unit: '%', help: t('The door chime.')},
  {name: 'voice_volume', label: t('Voice volume'), type: 'range', unit: '%', help: t('Announcements and Preview.')},
  {name: 'volume', label: t('Beeps volume'), type: 'range', unit: '%', help: t('Entry and exit, trouble, the walk test and key clicks.')},
  {name: 'key_click', label: t('Key click sounds'), type: 'switch', help: t('A short click for every touch.')},
  {name: 'lang', label: t('Language'), type: 'select', help: t('The words on the panel\'s screen.')},
  {name: 'region', label: t('Region'), type: 'select', help: t('How dates, numbers and the clock are written.')},
  {name: 'clock_mode', label: t('Clock'), type: 'select', help: t('The region\'s, or 14:30 or 2:30 PM whatever the region.')},
];
// Languages on this panel (docs/features/37 language-packs.md, I18N-046): what is installed and
// in what state, a file to install, what the update source offers. Its controls are for those
// who may change the language (the panel decides again); everyone else sees the list.
const LANG_RESULTS = {
  off: tx('Language files are turned off in this firmware.'),
  size: tx('That file is not a language file for this panel (its size is wrong).'),
  signature: tx('That language file was not signed for this panel.'),
  format: tx('That is not a language file, or it is damaged.'),
  draft: tx('That language is still a draft, and this firmware does not use drafts.'),
  english: tx('English is built in and cannot be replaced.'),
  no_space: tx('There is no room for it. Remove a language first.'),
  flash: tx('The panel could not save it. Try again.'),
  not_found: tx('That language is not installed.'),
  busy: tx('The panel is busy with another language. Try again in a moment.'),
  no_source: tx('The panel has no place to download languages from.'),
};
// An answer's result as a sentence: a refusal becomes an Error with it, a success the done words.
function languageWords(result) {
  return result === 'locked' ? friendly('locked') : LANG_RESULTS[result] ? t(LANG_RESULTS[result]) : '';
}
async function languageCall(action, locale) {
  try {
    return await api('languages', {action, locale});
  } catch (error) {
    throw new Error(languageWords(error.detail?.result) || error.message);
  }
}
const languageStateWords = l => l.state === 'ready'
  ? (l.missing > 0 ? tn('Ready; {0} phrase shows in English', 'Ready; {0} phrases show in English', l.missing) : t('Ready'))
  : l.state === 'other' ? t('Made for version {0}; some phrases may show in English', l.version)
  : l.state === 'refused' ? t('Can’t be used here') : t('Damaged');
const languageStateKind = l => l.state === 'ready' ? 'good' : l.state === 'other' ? 'warn' : 'bad';
function languagesCard(d, canChange) {
  const downloading = d.fetch.phase === 'downloading';
  const offered = d.available.filter(a => !d.installed.some(i => i.locale === a.locale && i.version === a.version));
  const installed = d.installed.map(l => `<li><div class="main"><strong>${esc(l.name || l.locale)}</strong>
    <span>${esc(l.locale)} · ${esc(t('Version {0}', l.version))}</span></div>
    ${l.draft ? tag(t('Draft'), 'info') : ''}${l.locale === d.current ? tag(t('In use'), 'good') : ''}${tag(languageStateWords(l), languageStateKind(l))}
    ${canChange ? `<button type="button" class="small" data-lang-remove="${esc(l.locale)}">${ht('Remove')}</button>` : ''}</li>`).join('');
  const more = offered.map(a => `<li><div class="main"><strong>${esc(a.locale)}</strong>
    <span>${esc(t('Version {0}, {1}', a.version, fmtBytes(a.size)))}</span></div>
    ${canChange ? `<button type="button" class="small" data-lang-download="${esc(a.locale)}"${downloading ? ' disabled' : ''}>${ht('Download')}</button>` : ''}</li>`).join('');
  const status = downloading ? `<p class="note" id="lang-status" role="status">${ht('Downloading {0}…', d.fetch.locale)}</p>`
    : d.fetch.phase === 'failed' ? `<p class="note" id="lang-status" role="status">${esc(t('The last download of {0} failed: {1}', d.fetch.locale, languageWords(d.fetch.result) || d.fetch.result))}</p>` : '';
  return `<section class="section" id="languages-card"><div class="card"><div class="card-head"><div><h2>${ht('Languages on this panel')}</h2>
    <p>${ht('English is built in. Free space for more: {0}.', fmtBytes(d.free))}</p></div></div>
    <ul class="list">${installed || `<li class="empty">${ht('No other languages are installed.')}</li>`}</ul>
    ${offered.length ? `<div class="card-head"><div><h2>${ht('Available to download')}</h2></div></div><ul class="list">${more}</ul>` : ''}
    ${status}
    ${canChange ? `<form id="lang-upload-form"><div class="rows"><div class="row"><div><label class="label" for="lang-file">${ht('Language file')}</label></div>
      <div class="control"><input id="lang-file" name="file" type="file" accept=".robolang" required></div></div></div>
      <div class="card-foot"><span class="note" id="lang-upload-note" role="status"></span><button class="primary">${ht('Install')}</button></div></form>` : ''}
  </div></section>`;
}
function bindLanguages(d, canChange) {
  if (d.fetch.phase === 'downloading') poll(async () => {
    const now = await api('languages');
    if (now.fetch.phase === 'downloading') return;
    toast(now.fetch.phase === 'done' ? t('{0} was downloaded.', now.fetch.locale)
      : t('The download of {0} failed: {1}', now.fetch.locale, languageWords(now.fetch.result) || now.fetch.result), now.fetch.phase === 'done' ? '' : 'error');
    await show();
  }, 2000);
  if (!canChange) return;
  $$('[data-lang-remove]').forEach(b => b.onclick = () => {
    const l = d.installed.find(x => x.locale === b.dataset.langRemove);
    review({title: t('Remove {0}?', l.name || l.locale),
      intro: l.locale === d.current ? t('The panel’s screen and these pages then use English.') : t('You can install it again later.'),
      confirm: t('Remove'), danger: true, done: t('The language was removed.'),
      run: () => languageCall('remove', l.locale)});
  });
  $$('[data-lang-download]').forEach(b => b.onclick = async () => {
    b.disabled = true;
    try {
      await languageCall('download', b.dataset.langDownload);
      toast(t('Downloading {0}…', b.dataset.langDownload));
    } catch (error) { toast(error.message, 'error'); }
    await show();
  });
  const form = $('#lang-upload-form');
  if (form) form.onsubmit = async event => {
    event.preventDefault();
    const file = form.file.files[0], note = $('#lang-upload-note'), button = form.querySelector('button');
    if (!file) return toast(t('Choose a file.'), 'error');
    button.disabled = true;
    note.textContent = t('Checking and saving the file…');
    try {
      const response = await fetch('/api/v1/languages', {method: 'PUT', credentials: 'same-origin', cache: 'no-store',
        headers: {'Content-Type': 'application/octet-stream', 'X-Panel-CSRF': csrf}, body: file,
        signal: AbortSignal.timeout(90000)}).catch(error => { throw unreachable(error); });
      const answer = await response.json().catch(() => ({}));
      if (!response.ok || answer.result !== 'ok')
        throw new Error(languageWords(answer.result) || friendly(answer.error) || t('The panel answered {0}.', response.status));
      toast(t('The language was installed.'));
      await show();
    } catch (error) {
      note.textContent = error.message;
      button.disabled = false;
    }
  };
}
// Announcements (docs/features/08 voice-first-build.md D06, D14; the approved proposal
// docs/features/31-Browser-Panel-Manager/mockup/proposals/BL-202-voice.js): the three switches (the
// walk test has none: it always speaks, WALK-016), the voice language, whether the voice is ready, Preview through the Remote keypad, and for the system
// master and installer the signed voice pack's upload. The words are the touchscreen's.
const VOICE_SWITCHES = ['voice_enabled', 'voice_status', 'voice_chime'];
const voiceRoleMay = () => !!state?.access?.system; // the system master and the installer (the panel decides again)
// The language's name in the page's language; another locale as the pack names it (data).
const voiceLocaleName = locale => locale === 'en-US' ? ht('English (United States)') : esc(locale);
// v: GET voices; null when it could not be loaded (readiness unknown: enabling and Preview stay off).
function voiceCard(v, values) {
  const may = voiceRoleMay();
  const gate = !!state?.locked;
  const known = !!v;
  const off = !values.voice_enabled;
  const sw = (name, label, help, locked) => fieldRow({name, label, type: 'switch', help, locked}, !!values[name]);
  const status = !known ? [t('Voice information could not be loaded. Try again.'), 'warn']
    : !v.installed ? [t('Voice pack not installed'), 'warn']
    : !v.ready ? [t('Voice is unavailable.'), 'warn'] : [t('Ready'), 'good'];
  const canPreview = known && v.ready && !gate;
  return `<section class="section" id="voice-card"><div class="card"><div class="card-head"><div><h2>${ht('Announcements')}</h2>
    <p>${ht('The panel says what is happening, in words. Off until you turn it on. Alarms, fire and CO always sound at full volume.')}</p></div></div>
    <div class="rows">
      ${sw('voice_enabled', t('Announcements'), known && v.installed ? t('Uses the Voice volume above.') : known ? t('Needs a voice pack.') : '', gate || !known)}
      <p class="dialog-error" id="voice-error" role="alert" hidden></p>
      ${sw('voice_status', t('Status messages'), t('Says arming, armed, disarmed, entry and not ready messages.'), off || gate)}
      ${sw('voice_chime', t('Zone chime'), t('Says the zone’s spoken name after the chime.'), off || gate)}
      <div class="row"><div><span class="label">${ht('Voice language')}</span><span class="help">${ht('Changing the page language does not change the voice.')}</span></div>
        <div class="control"><span class="what">${known && v.installed ? voiceLocaleName(v.locale) : '-'}</span></div></div>
      <div class="row"><div><span class="label">${ht('Voice pack')}</span><span class="help">${esc(known && v.installed ? t('Version {0}', v.version) : known ? t('No voice pack is installed.') : '')}</span></div>
        <div class="control"><span class="what">${known && v.installed ? esc(v.name) : ''} ${tag(status[0], status[1])}${known && v.installed && v.draft ? ' ' + tag(t('Draft'), 'warn') : ''}
          ${known ? '' : `<button type="button" class="small" id="voice-retry">${ht('Try again')}</button>`}</span></div></div>
      <div class="row"><div><span class="label">${ht('Preview')}</span><span class="help">${ht('Plays on the panel. Use the Remote keypad.')}</span></div>
        <div class="control">${canPreview ? `<a class="button small" href="#/keypad">${ht('Open the Remote keypad')}</a>`
          : `<button type="button" class="small" disabled>${ht('Open the Remote keypad')}</button>`}</div></div>
    </div>
    ${may ? `<form id="voice-upload-form"><fieldset${gate ? ' disabled' : ''}><div class="rows"><div class="row"><div><label class="label" for="voice-file">${ht('Voice pack')}</label>
      <span class="help">${ht('A signed .robovoice file. Installing one never turns Announcements on.')}</span></div>
      <div class="control"><input id="voice-file" name="file" type="file" accept=".robovoice"></div></div></div>
      <div class="card-foot"><span class="note" id="voice-upload-note" role="status">${gate ? ht('The panel is armed or an alarm is sounding. Changes wait until everything is disarmed and quiet.') : ''}</span><button class="primary">${ht('Install')}</button></div></fieldset></form>`
      : `<p class="note gap">${ht('Only the system master or installer can install a voice pack.')}</p>`}
  </div></section>`;
}
// An upload's answer in words: the proposal's, and the language upload's for the same failures.
const VOICE_RESULTS = () => ({
  too_big: t('The voice pack is too large to install.'),
  bad_signature: t('The voice pack is damaged or was not signed for this panel.'),
  armed: t('The panel is armed or an alarm is sounding. Changes wait until everything is disarmed and quiet.'),
  locked: t('The panel is armed or an alarm is sounding. Changes wait until everything is disarmed and quiet.'),
  not_allowed: t('Only the system master or installer can install a voice pack.'),
  busy: t('The panel is busy. Try again in a moment.'),
  no_space: t(LANG_RESULTS.no_space),
  flash: t(LANG_RESULTS.flash),
  off: t('Voice is unavailable.'),
});
// Why Announcements can't be turned on, from the pack's readiness (or the panel's own refusal).
const voiceRefusal = v => !v ? '' : !v.installed ? t('Announcements cannot be turned on because the voice pack is not installed.')
  : !v.ready ? t('Announcements cannot be turned on because voice is unavailable.') : '';
const voiceRefusalWord = code => code === 'no_pack' ? voiceRefusal({installed: false})
  : code === 'unavailable' ? voiceRefusal({installed: true, ready: false}) : code === 'locked' ? VOICE_RESULTS().armed : '';
function bindVoice() {
  $('#voice-retry')?.addEventListener('click', () => show());
  for (const name of VOICE_SWITCHES) {
    const input = $(`#voice-card [name="${name}"]`);
    if (!input) continue;
    input.addEventListener('change', async () => {
      const e = $('#voice-error');
      const refuse = words => { input.checked = !input.checked; e.textContent = words; e.hidden = false; };
      // The panel's own gates, said before asking: nothing changes while armed, and enabling needs ready audio.
      if (state?.locked) return refuse(VOICE_RESULTS().armed);
      if (name === 'voice_enabled' && input.checked) {
        const v = await api('voices').catch(() => null);
        if (!v || !v.ready) return refuse(v ? voiceRefusal(v) : t('Voice information could not be loaded. Try again.'));
      }
      try {
        await api('config/display', {values: {[name]: input.checked}});
      } catch (error) {
        return refuse(voiceRefusalWord(error.code) || error.message);
      }
      await show(); // the card shows what the panel keeps; no toast
    });
  }
  const form = $('#voice-upload-form');
  if (form) form.onsubmit = async event => {
    event.preventDefault();
    const file = form.file.files[0], note = $('#voice-upload-note'), button = form.querySelector('button');
    if (!file) { note.textContent = t('Choose a file.'); return; }
    button.disabled = true;
    note.textContent = t('Checking and saving the file…');
    try {
      const response = await fetch('/api/v1/voices', {method: 'PUT', credentials: 'same-origin', cache: 'no-store',
        headers: {'Content-Type': 'application/octet-stream', 'X-Panel-CSRF': csrf}, body: file,
        signal: AbortSignal.timeout(90000)}).catch(error => { throw unreachable(error); });
      const answer = await response.json().catch(() => ({}));
      if (!response.ok || answer.result !== 'ok')
        throw new Error(VOICE_RESULTS()[answer.result] || friendly(answer.error) || t('The panel answered {0}.', response.status));
      await show(); // the card shows the installed pack; no toast
    } catch (error) {
      note.textContent = error.message;
      button.disabled = false;
    }
  };
}
// A zone's spoken name (zone-status's read-only `spoken`; D01, D07), as the touchscreen's zone row says it.
function spokenState(s) {
  return {ready: [t('Ready'), 'good'], empty: [t('Choose words'), ''], review: [t('Review spoken name'), 'warn'],
    off: [t('Announcements are off'), ''], missing: [t('Voice pack not installed'), 'warn'],
    unavailable: [t('Voice is unavailable.'), 'warn']}[s.state] || [t('Voice is unavailable.'), 'warn'];
}
// The zone's setup card: the row, and for the installer the way to choose the words at the panel.
function spokenRow(s) {
  if (!s) return '';
  const [word, kind] = spokenState(s);
  return `<div class="row spoken-row"><div><span class="label">${ht('Spoken name')}</span>
    <span class="help">${ht('The recorded words the panel says for this zone. Chosen at the keypad.')}</span></div>
    <div class="control"><span class="what"><strong>${esc(s.summary || '-')}</strong>${tag(word, kind)}</span>
    ${isInstaller() ? `<a class="button small" href="#/keypad" data-k="spoken-keypad">${ht('Choose words on the panel')}</a>` : ''}</div></div>`;
}
// The same, read-only, on a Zones > All zones card (every role that sees the zone).
function spokenLine(s) {
  if (!s) return '';
  const [word, kind] = spokenState(s);
  return `<span class="spoken-line"><small>${ht('Spoken name')}</small> <strong>${esc(s.summary || '-')}</strong>${tag(word, kind)}</span>`;
}
async function displayPage() {
  const data = await api('config/display');
  const lang = await api('languages').catch(() => null); // not in every firmware
  // The voice: undefined in a firmware without it (404, no card); null when it could not be loaded.
  const voice = await api('voices').catch(error => error.status === 404 ? undefined : null);
  const canChange = !!state.access?.shared;
  // The Announcements switches are on their own card, saved as they change.
  const specs = withSchema(displaySpecs(), data.fields?.filter(f => !VOICE_SWITCHES.includes(f.name)));
  // These pages' own language, for this browser only (docs/features/37): the panel's languages.
  const languages = (data.fields?.find(f => f.name === 'lang')?.choices || [{value: 'en-US', label: nt('English')}])
    .map(c => [c.value, c.label]);
  const mine = browserLanguage() || 'en-US';
  return {
    html: pageHead(t('Display and sound'), t('How the keypad looks and sounds. Alarm sounds are not affected.')) +
      settingsCard({id: 'config-form', specs, values: data.values}) + (voice !== undefined ? voiceCard(voice, data.values) : '') +
      `<section class="section"><div class="card"><div class="card-head"><div><h2>${esc(t('This browser'))}</h2>
        <p>${esc(t('The language of these pages, on this computer or phone only.'))}</p></div></div>
        <div class="rows">${fieldRow({name: 'page_language', label: t('Language'), type: 'select', options: languages}, mine)}</div>
      </div></section>` + (lang ? languagesCard(lang, canChange) : ''),
    bind: () => {
      bindSettingsCard('config-form', specs, data.values, ({changes, values}) =>
        review({changes, run: () => api('config/display', {values})}));
      if (lang) bindLanguages(lang, canChange);
      if (voice !== undefined) bindVoice();
      const select = $('[name="page_language"]');
      if (select) select.onchange = () => {
        try { localStorage.setItem('panel-language', select.value); } catch { /* this visit only */ }
        location.reload();
      };
    },
  };
}

const ipSpecs = () => [
  {name: 'use_static', label: t('Fixed IP address'), type: 'switch', help: t('Off: the router gives the panel its address (DHCP).')},
  {name: 'ip', label: t('IP address'), type: 'text', placeholder: '192.168.1.50', inputmode: 'decimal', showIf: v => v.use_static},
  {name: 'mask', label: t('Subnet mask'), type: 'text', placeholder: '255.255.255.0', inputmode: 'decimal', showIf: v => v.use_static},
  {name: 'gateway', label: t('Router (gateway)'), type: 'text', placeholder: '192.168.1.1', inputmode: 'decimal', showIf: v => v.use_static},
  {name: 'dns1', label: t('DNS server'), type: 'text', inputmode: 'decimal', showIf: v => v.use_static},
  {name: 'dns2', label: t('Second DNS server'), type: 'text', inputmode: 'decimal', help: t('Optional.'), showIf: v => v.use_static},
];
function checkList(check) {
  const steps = checkSteps(), states = checkStates();
  return check.steps.map((step, i) => {
    const [words, kind] = states[step.state] || [t('Unknown'), ''];
    return `<li><div class="main"><strong>${esc(steps[i] || t('Step {0}', i + 1))}</strong>${step.detail ? `<span>${esc(step.detail)}</span>` : ''}</div>${tag(words, kind)}</li>`;
  }).join('');
}
async function networkPage() {
  const [ip, check] = await Promise.all([api('config/network'), api('network-check').catch(() => null), refreshStatus()]);
  const specs = withSchema(ipSpecs(), ip.fields);
  const w = state.wifi;
  return {
    html: pageHead(t('Network'), t('Changes are tried for two minutes first. If this browser cannot confirm them, the panel goes back to the old settings by itself.')) +
      `<section class="card">${kv([[t('Wi-Fi network'), w.ssid || t('Not connected')], [t('Signal'), w.ssid ? t('{0} ({1} dBm)', signalWords(w.rssi), w.rssi) : ''],
        [t('IP address'), w.ip], [t('Clock'), w.time_synced ? t('Synchronized') : t('Not set yet')]])}</section>
      <section class="section"><form class="card" id="wifi-form" novalidate><div class="card-head"><div><h2>${ht('Wi-Fi network')}</h2>
        <p>${ht('Pick a network from the list or type its name.')}</p></div><button type="button" id="scan">${ht('Find networks')}</button></div>
        <ul class="list clickable" id="networks"></ul>
        <div class="rows">${fieldRow({name: 'ssid', label: t('Network name'), type: 'text', maxLength: 32}, w.ssid)}
        ${fieldRow({name: 'password', label: t('Password'), type: 'secret', maxLength: 63, help: t('Leave blank for an open network.')}, '')}</div>
        <div class="card-foot"><span class="note">${ht('This browser may lose the connection while the panel changes network.')}</span>
        <button class="primary">${ht('Connect')}</button></div></form></section>
      <section class="section">${settingsCard({id: 'ip-form', title: t('IP address'), specs, values: ip.values, button: t('Apply')})}</section>
      ${check ? `<section class="section"><div class="card"><div class="card-head"><div><h2>${ht('Connection check')}</h2>
        <p>${ht('Finds which part of the connection needs attention.')}</p></div><button type="button" id="run-check">${ht('Run the check')}</button></div>
        <ul class="list" id="check-steps">${checkList(check)}</ul></div></section>` : ''}`,
    bind() {
      const form = $('#wifi-form');
      $('#scan').onclick = async () => {
        const button = $('#scan');
        button.disabled = true;
        $('#networks').innerHTML = `<li class="empty">${ht('Looking for networks…')}</li>`;
        try {
          await api('wifi-scan', {});
          let result;
          for (let i = 0; i < 10; i++) {
            await new Promise(r => setTimeout(r, 1500));
            result = await api('wifi-scan');
            if (result.done) break;
          }
          const seen = new Map();
          for (const n of result.networks) if (n.ssid && (!seen.has(n.ssid) || seen.get(n.ssid).rssi < n.rssi)) seen.set(n.ssid, n);
          const networks = [...seen.values()].sort((a, b) => b.rssi - a.rssi);
          $('#networks').innerHTML = networks.map((n, i) => `<li data-i="${i}" tabindex="0"><div class="main"><strong>${esc(n.ssid)}</strong>
            <span>${n.secure ? ht('{0} signal', signalWords(n.rssi)) : ht('{0} signal · open network', signalWords(n.rssi))}</span></div>${n.ssid === w.ssid ? tag(t('Connected'), 'good') : ''}</li>`).join('')
            || `<li class="empty">${ht('No networks found.')}</li>`;
          $$('#networks li[data-i]').forEach(li => li.onclick = li.onkeydown = e => {
            if (e.type === 'keydown' && e.key !== 'Enter') return;
            form.ssid.value = networks[Number(li.dataset.i)].ssid;
            form.password.focus();
          });
        } catch (error) { toast(error.message, 'error'); }
        button.disabled = false;
      };
      form.onsubmit = event => {
        event.preventDefault();
        const ssid = form.ssid.value.trim(), password = form.password.value;
        if (!ssid) return toast(t('Enter the network name.'), 'error');
        review({title: t('Change Wi-Fi network?'), changes: [{label: t('Network'), from: w.ssid, to: ssid},
          ...(password ? [{label: t('Password'), from: '', to: t('New password')}] : [])],
          intro: t('The panel joins the new network. Reconnect to it and confirm within two minutes, or it goes back.'),
          confirm: t('Connect'), done: t('The panel is joining the network.'), run: () => changeNetwork('wifi', {ssid, password})});
      };
      bindSettingsCard('ip-form', specs, ip.values, ({changes, values}) => review({
        title: t('Change the IP address?'), changes, confirm: t('Apply'), done: t('The panel is applying the new address.'),
        intro: t('If the address changes, open the panel at its new address and confirm within two minutes.'),
        run: () => changeNetwork('config/network', {values}, values.use_static ?? ip.values.use_static ? (values.ip || ip.values.ip) : ''),
      }));
      if ($('#run-check')) $('#run-check').onclick = async () => {
        try {
          await api('network-check', {});
          $('#run-check').disabled = true;
          const id = renderId;
          for (let i = 0; i < 40 && id === renderId; i++) {
            await new Promise(r => setTimeout(r, 1500));
            const result = await api('network-check');
            if (id !== renderId) return;
            $('#check-steps').innerHTML = checkList(result);
            if (!result.running) break;
          }
          $('#run-check').disabled = false;
        } catch (error) { toast(error.message, 'error'); }
      };
    },
  };
}

async function timePage() {
  const data = await api('time');
  const zones = timezones();
  const known = zones.some(z => z[2] === data.timezone);
  const specs = [
    {name: 'zone', label: t('Time zone'), type: 'select', local: true,
      options: [...zones.map(([name, where, tz]) => [tz, t('{0} ({1})', name, where)]), ['custom', t('Other: type a POSIX rule')]]},
    {name: 'timezone', label: t('POSIX time-zone rule'), type: 'text', maxLength: 95, placeholder: 'CST6CDT,M3.2.0,M11.1.0',
      showIf: v => v.zone === 'custom'},
    {name: 'ntp', label: t('Time server'), type: 'text', maxLength: 127, help: t('For example pool.ntp.org, or your router.')},
  ];
  const values = {zone: known ? data.timezone : 'custom', timezone: data.timezone, ntp: data.ntp};
  const may = !!state.access?.shared;
  // With hardware that keeps a time set by hand (a clock module, docs/features/35) the date and time
  // can be set here. The hardware itself is never named: a person sees what they can do.
  const settable = may && !!data.clock?.settable;
  return {
    html: pageHead(t('Date and time'), t('The panel sets its clock from a time server. Event times and schedules use this time zone.')) +
      (settable ? clockCard(data.now_ms) : '') +
      settingsCard({id: 'time-form', specs, values, readOnly: !may}) +
      (may ? '' : `<p class="gap muted"><small>${ht('A master or the installer changes the time zone and the time server.')}</small></p>`),
    bind: () => {
      if (settable) bindClockCard(data.now_ms);
      if (may) bindSettingsCard('time-form', specs, values, ({changes}) => {
        const form = $('#time-form');
        const zone = form.zone.value, timezone = zone === 'custom' ? form.timezone.value.trim() : zone;
        review({changes: changes.filter(c => c.name !== 'timezone' || zone === 'custom'),
          run: () => api('time', {timezone, ntp: form.ntp.value.trim()})});
      });
    },
  };
}

// Setting the date and time by hand. Times travel as UTC milliseconds, so this computer's time
// zone never changes the moment; they are shown in this computer's time zone.
function clockWhen(ms) {
  return ms > 0 ? new Date(ms).toLocaleString(wordsLocale(), {dateStyle: 'medium', timeStyle: 'medium'}) : t('Not set');
}
function clockCard(nowMs) {
  return `<section class="card"><form id="clock-form" novalidate><div class="card-head"><div><h2>${ht('Set the date and time')}</h2>
      <p>${ht("The panel's clock says {0}. The time server corrects it at its next sync.", clockWhen(nowMs))}</p></div></div>
    <div class="rows">${fieldRow({name: 'when', label: t('New date and time'), type: 'text', inputType: 'datetime-local',
      help: t('In this computer’s time zone.')}, '')}</div>
    <div class="card-foot"><button type="button" id="clock-computer">${ht('Use this computer’s time')}</button>
    <button class="primary">${ht('Set')}</button></div></form></section>`;
}
function bindClockCard(nowMs) {
  const form = $('#clock-form');
  if (!form) return;
  const pad = n => String(n).padStart(2, '0');
  const d = new Date(nowMs > 0 ? nowMs : Date.now());
  form.when.value = `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}T${pad(d.getHours())}:${pad(d.getMinutes())}`;
  const set = (label, at) => review({title: t('Set the date and time?'), confirm: t('Set'), done: t('The date and time are set.'),
    changes: [{label, from: clockWhen(nowMs), to: clockWhen(at())}],
    run: () => api('time', {epoch_ms: at()})});
  $('#clock-computer').onclick = () => set(t('Date and time (this computer’s)'), () => Date.now());
  form.onsubmit = event => {
    event.preventDefault();
    const at = new Date(form.when.value).getTime();
    if (!form.when.value || !Number.isFinite(at)) return toast(t('Enter a date and time.'), 'error');
    if (at < Date.UTC(2025, 0, 1) || at >= Date.UTC(2100, 0, 1)) return toast(t('Pick a date from 2025 to 2099.'), 'error');
    set(t('Date and time'), () => at);
  };
}

async function storagePage() {
  const d = await api('storage');
  const sd = d.sd, flash = d.flash;
  // sd.state is the API's key (English); sd.words says it in this page's language.
  const present = ![nt('Starting'), nt('No readable card'), nt('Safe to remove')].includes(sd.state);
  return {
    html: pageHead(t('Storage'), t('The optional SD card and the panel’s own flash memory.')) +
      `<div class="grid two"><section class="card"><div class="card-head"><div><h2>${ht('SD card')}</h2>${sd.message ? `<p>${esc(sd.message)}</p>` : ''}</div>
        ${tag(sd.words || sd.state || t('Unknown'), sd.state === 'Ready' ? 'good' : sd.state === 'Card error' ? 'bad' : '')}</div>
        ${present ? kv([[t('Name'), sd.name], [t('File system'), sd.filesystem], [t('Size'), sd.capacity_bytes ? fmtBytes(sd.capacity_bytes) : ''],
          [t('Free'), sd.usage_valid ? fmtBytes(sd.free_bytes) : '']]) : `<p class="empty">${ht('Insert a card to use it.')}</p>`}
        ${sd.usage_valid && sd.capacity_bytes ? `<div class="rows"><meter min="0" max="${sd.capacity_bytes}" value="${sd.capacity_bytes - sd.free_bytes}"></meter></div>` : ''}
        ${state.access?.system ? `<div class="card-foot"><button type="button" data-sd="check">${ht('Check')}</button><button type="button" data-sd="eject">${ht('Eject')}</button>
        <button type="button" class="danger" data-sd="format">${ht('Erase and format…')}</button></div>` : ''}</section>
      <section class="card"><div class="card-head"><div><h2>${ht('Internal flash')}</h2><p>${ht('{0} in total. Running from {1}.', fmtBytes(flash.chip_bytes), flash.running_slot)}</p></div></div>
        <div class="rows"><div class="row wide"><div><span class="label">${ht('Firmware')}</span><span class="help">${ht('{0} of the {1} slot', fmtBytes(flash.image_bytes), fmtBytes(flash.slot_bytes))}</span></div>
        <div class="control"><meter min="0" max="${flash.slot_bytes}" value="${flash.image_bytes}" high="${flash.slot_bytes * 0.9}"></meter></div></div></div>
        <ul class="list">${flash.partitions.map(p => `<li><div class="main"><strong>${esc(p.name)}</strong></div><span class="side">${fmtBytes(p.bytes)}</span></li>`).join('')}</ul></section></div>`,
    bind() {
      $$('[data-sd]').forEach(b => b.onclick = () => {
        const action = b.dataset.sd;
        if (action === 'format') return formatDialog(sd.generation);
        review({title: action === 'check' ? t('Check the SD card?') : t('Eject the SD card?'),
          intro: action === 'eject' ? t('You can then remove it safely.') : t('Looks for file-system errors.'),
          confirm: action === 'check' ? t('Check') : t('Eject'), done: t('Done.'),
          run: async () => { const r = await api('storage', {action, generation: sd.generation}); if (r.error) throw new Error(friendly(r.error)); }});
      });
    },
  };
}
function formatDialog(generation) {
  // The words to type stay English whatever the language: the panel compares them.
  openDialog(`<form id="format-form"><div class="dialog-head"><h2 id="dialog-title">${ht('Erase the SD card?')}</h2>
    <p>${ht('Every file on the card is deleted. The panel’s settings, users and event log are in its own flash and are not touched.')}</p></div>
    <div class="dialog-body"><div class="code-field"><label for="format-confirm">${ht('Type {0} to confirm', nt('ERASE SD CARD'))}</label>
    <input id="format-confirm" name="confirm" autocomplete="off" spellcheck="false"></div></div>
    <p class="dialog-error" id="dialog-error" role="alert"></p>
    <div class="dialog-foot"><button type="button" id="dialog-cancel">${ht('Cancel')}</button><button class="danger solid">${ht('Erase')}</button></div></form>`);
  $('#dialog-cancel').onclick = closeDialog;
  $('#format-form').onsubmit = async event => {
    event.preventDefault();
    try {
      const r = await api('storage', {action: 'format', generation, confirm: event.target.confirm.value});
      if (r.error) throw new Error(friendly(r.error));
      closeDialog(); toast(t('The card is being formatted.')); show();
    } catch (error) { $('#dialog-error').textContent = error.message; }
  };
}

// ---------------------------------------------------------------------------------------
// Installer: zones and their sensors, partitions and devices, system options

let alarmCache = null;
async function loadAlarm() {
  alarmCache = await api('alarm');
  return alarmCache;
}
function applyAlarm(changes) {
  return api('alarm', {changes, revision: alarmCache.revision});
}
function partitionChoices(alarm, withAll = false) {
  const list = alarm.partitions.filter(p => p.enabled).map(p => [p.number, p.name || tc('default partition name', 'Partition {0}', p.number)]);
  return withAll ? [[0, t('All partitions')], ...list] : list;
}
// Zones, for everyone: what needs attention, every zone with filters, Bypass and Unbypass while the
// zone's partition is disarmed, and what the words mean. The installer sets zones up under Installer > Zones
// (#/zone-setup, BL-134).
let zoneFilter = 'all';
function zonesBody() {
  const zones = live.zones || [], means = zoneMeans(), multi = partitionBits().length > 1;
  const keyOf = new Map(zones.map(z => [z.zone, zoneKey(z)]));
  const disarmed = z => partitionOf(z.partition)?.state === 'disarmed';
  // The order the zones tripped in an alarm (its "What happened"), first first.
  const order = new Map();
  for (const p of state.partitions || [])
    alarmSequence(p.number, zones.filter(z => z.partition === p.number)).forEach((x, n) => { if (!order.has(x.zone)) order.set(x.zone, [n, x.event]); });
  const rank = z => keyOf.get(z.zone) === 'alarm' && order.has(z.zone) ? order.get(z.zone)[0] : 99;
  const attention = zones.filter(z => keyOf.get(z.zone) !== 'normal').sort((a, b) => rank(a) - rank(b));
  const sideFor = z => {
    if (rank(z) === 99) return '';
    const [n, e] = order.get(z.zone);
    return tc('the order a sensor tripped in, and when', '#{0} · {1}', n + 1, e.epoch_ms > 0 ? timeOfDay(e.epoch_ms, true) : fmtWhen(e).short);
  };
  const bypassButton = (z, where) => !disarmed(z) || !(z.bypassable || z.flags & ZS.bypassed) ? ''
    : z.flags & ZS.bypassed ? `<button type="button" class="small" data-unbypass="${z.zone}" data-k="${where}-unbypass-${z.zone}"${offIf(live.busy.has(z.partition))}>${esc(tc('zone card button', 'Unbypass'))}</button>`
    : `<button type="button" class="small" data-bypass="${z.zone}" data-k="${where}-bypass-${z.zone}"${offIf(live.busy.has(z.partition))}>${esc(tc('zone card button', 'Bypass'))}</button>`;
  const count = test => zones.filter(test).length;
  const openN = count(isOpen), bypassedN = count(z => z.flags & ZS.bypassed), troubleN = count(z => z.flags & ZS_TROUBLE || z.unreachable);
  const shows = {all: () => true, open: isOpen, bypassed: z => z.flags & ZS.bypassed, trouble: z => z.flags & ZS_TROUBLE || z.unreachable};
  const shown = zones.filter(shows[zoneFilter] || shows.all);
  const filters = [['all', tc('zone filter', 'All')], ['open', tc('zone filter', 'Open')], ['bypassed', tc('zone filter', 'Bypassed')],
    ['trouble', tc('zone filter', 'Trouble')]];
  const summary = [tn('{0} zone', '{0} zones', zones.length), openN ? tn('{0} zone open', '{0} zones open', openN) : '',
    bypassedN ? tn('{0} zone bypassed', '{0} zones bypassed', bypassedN) : '',
    troubleN ? tn('{0} zone with a trouble', '{0} zones with a trouble', troubleN) : ''].filter(Boolean).join(' · ');
  const where = z => multi ? `${tc('default zone name', 'Zone {0}', z.zone)} · ${partitionNames(1 << (z.partition - 1))}` : tc('default zone name', 'Zone {0}', z.zone);
  return `<div class="page-head"><div><h1>${ht('Zones')}</h1><p>${esc(summary)}</p></div></div>
    ${attention.length ? `<section class="section"><div class="section-head"><h2>${ht('Needs attention')}</h2></div>
      <div class="card">${attention.map(z => {
        const key = keyOf.get(z.zone), [word, kind, meaning] = means[key];
        const button = ['open', 'active', 'bypassed'].includes(key) ? bypassButton(z, 'a') : '';
        return `<div class="explain ${kind === 'bad' || kind === 'info' ? kind : ''}"><span class="glyph">${zoneGlyph(z)}</span>
          <div class="main"><h3>${esc(zoneLabel(z))} · ${esc(word)}</h3><p>${esc(meaning)}</p>${button ? `<div class="actions">${button}</div>` : ''}</div>
          <span class="side">${esc(sideFor(z))}</span></div>`;
      }).join('')}</div></section>` : ''}
    <section class="section"><div class="section-head"><h2>${ht('All zones')}</h2>
      <div class="filters" role="group" aria-label="${ht('Show')}">${filters.map(([v, label]) =>
        `<button type="button" data-zfilter="${v}" data-k="filter-${v}" aria-pressed="${v === zoneFilter}">${esc(label)}</button>`).join('')}</div></div>
      ${shown.length ? `<div class="zone-grid">${shown.map(z => {
        const [word, kind] = means[keyOf.get(z.zone)];
        return `<div class="card zone-card ${kind === 'good' ? '' : kind}"><span class="glyph">${zoneGlyph(z)}</span>
          <div class="main"><strong>${esc(zoneLabel(z))}</strong><span class="now">${esc(word)}</span><span>${esc(where(z))}</span>${spokenLine(z.spoken)}</div>${bypassButton(z, 'g')}</div>`;
      }).join('')}</div>` : `<p class="muted">${zones.length ? ht('No zones of this kind.') : ht('No zones.')}</p>`}
      ${zones.every(disarmed) ? '' : `<p class="gap muted"><small>${ht('Bypass while disarmed.')}</small></p>`}</section>
    <section class="section"><div class="card"><details class="more"${live.legend ? ' open' : ''}><summary>${ht('What these words mean')}</summary>
      <dl class="legend">${Object.values(means).map(([w, k, m]) => `<dt>${tag(w, k)}</dt><dd>${esc(m)}</dd>`).join('')}</dl></details></div></section>`;
}
const redrawZones = (force = false) => redraw('zones-live', zonesBody, force);
async function zoneStatusPage() {
  await Promise.all([refreshStatus(), loadZones(), loadHistory().catch(() => {})]);
  renderNav();
  const html = zonesBody();
  return {
    html: `<div id="zones-live">${html}</div>`,
    bind() {
      const root = $('#zones-live');
      if (!root) return;
      drawn['zones-live'] = html;
      live.redraw = redrawZones;
      root.addEventListener('click', async event => {
        const el = event.target.closest('button[data-zfilter], button[data-bypass], button[data-unbypass]');
        if (!el || el.disabled) return;
        if (el.dataset.zfilter) {
          zoneFilter = el.dataset.zfilter;
          return redrawZones();
        }
        const number = Number(el.dataset.bypass || el.dataset.unbypass), z = (live.zones || []).find(x => x.zone === number);
        if (z) await act({number: z.partition, action: el.dataset.bypass ? 'bypass' : 'unbypass', zone: number}, el);
      });
      poll(async () => { await refreshStatus(); redrawZones(); }, 2000);
      poll(async () => { await loadZones(); redrawZones(); }, 3000);
      poll(async () => { await loadHistory(); redrawZones(); }, 5000);
    },
  };
}
// The old addresses (#/zones/setup, #/zones/5) open the installer's page at its new one (BL-134).
async function zonesPage(arg) {
  return isInstaller() && arg ? zpMoved(arg === 'setup' ? '' : arg) : zoneStatusPage();
}

// ---- Installer > Zones (BL-134) ----------------------------------------------------------------------------------
// Laid out as the touchscreen's zone page (docs/features/07 mockup, installer.js "One zone") and worded with its
// words (ui_installer.c, ui_sources.c), so the two say the same (WORD-006).

// ---- words: the touchscreen's (ui_sources.c kind_name / kind_help, ui_installer.c) -------------------------------

function zpKinds() {
  return {
    1: [t('Simulator'), t('No sensor: the service console opens and closes it, for testing'), 'sliders'],
    2: [t('Wired contact'), t('A contact wired to the panel’s terminals'), 'plug'],
    3: [t('Zigbee sensor'), t('Paired straight to the panel’s own radio'), 'radio'],
    4: [t('Home Assistant sensor'), t('Any sensor Home Assistant knows'), 'home'],
    5: [t('433 MHz sensor'), t('Learned by opening and closing it'), 'radio'],
    6: [t('Keypad input'), t('A contact wired to a keypad'), 'grid'],
    7: [t('Z-Wave sensor'), t('Paired to the panel’s Z-Wave radio'), 'radio'],
  };
}
// Where a sensor is, in a few words: "Wired contact · Input 1", "Home Assistant sensor · binary_sensor.back_door"
function zpWhere(a) {
  if (!a || !a.kind) return tc('sensor', 'None');
  const kind = zpKinds()[a.kind]?.[0] || t('Other sensor');
  const id = String(a.id || '0'), hex = v => { try { return BigInt(v).toString(16).toUpperCase(); } catch { return v; } };
  const at = a.kind === 4 ? a.ref
    : [1, 2, 6].includes(a.kind) ? t('Input {0}', a.input)
    : a.kind === 5 ? t('Code {0}', hex(id))
    : a.kind === 7 ? t('Device {0}', id)
    : id !== '0' ? t('Address {0}', hex(id)) : '';
  return at ? `${kind} · ${at}` : kind;
}
// The same on two lines, for a row's value: the kind, then where
function zpWhereHtml(a) {
  const [kind, ...at] = zpWhere(a).split(' · ');
  return `${esc(kind)}${at.length ? `<small>${esc(at.join(' · '))}</small>` : ''}`;
}
const ZP_MAP0 = {mode: 0, invert: false, threshold_x100: 0, hysteresis_x100: 0, states: '', grace_s: 0, response_ms: 0, hold_s: 0};
const ZP_NONE = {kind: 0, input: 0, id: '0', ref: ''};
// The sources that read a hold: 433 MHz (5) and Z-Wave (7, its momentary inputs). A low-battery input newly set
// on a 433 MHz sensor holds 12 h unless another value is set, as at the panel (RF-021, RF-022).
const ZP_HOLD_KINDS = [5, 7], ZP_BATTERY_ROLE = 2, ZP_RF_KIND = 5, ZP_RF_BATTERY_HOLD_S = 43200;
const zpRoleName = role => [t('Source'), t('Tamper input'), t('Low-battery input')][role];
const zpOnOff = on => on ? t('On') : t('Off');
const zpHours = s => s === 0 ? t('Not supervised') : s % 3600 === 0 ? tn('{0} hour', '{0} hours', s / 3600)
  : s % 60 === 0 ? tn('{0} min', '{0} min', s / 60) : tn('{0} s', '{0} s', s);
const zpMinutes = s => s === 0 ? t('Source default') : s % 60 === 0 ? tn('{0} min', '{0} min', s / 60) : tn('{0} s', '{0} s', s);
const zpMs = ms => ms === 0 ? t('Source default') : tn('{0} ms', '{0} ms', ms);
const zpHold = s => s === 0 ? t('Automatic') : zpHours(s);
const zpLimit = z => z.flags & 16 ? tc('alarm limit', 'Never') : z.swinger_trips ? tn('{0} time', '{0} times', z.swinger_trips) : t('System setting');
const zpReport = on => on ? t('Its events go to phones and integrations, not only the event log') : t('Its events stay in the panel’s event log only');
function zpReads(m, kind) {
  if (kind === 2) return m.invert ? t('Normally open') : t('Normally closed');
  const base = [t('Open when on'), t('Open above a value'), t('Open below a value'), t('Open in a named state')][m.mode] || t('Open when on');
  return m.mode === 0 && m.invert ? t('Open when off') : base;
}
// docs/features/22 "Defaults from the device class": a new zone's type from what its sensor says it is
function zpTypeFor(cls) {
  return ({4: 4, 6: 9, 7: 9, 8: 14, 9: 7, 10: 8, 11: 8, 13: 7, 14: 21, 15: 77})[cls] || 3;
}
// ALARM_ZT_*: which rows a type has (ui_installer.c build_zone)
const zpNightTypes = [4, 10, 5], zpChimeTypes = [1, 2, 3, 4, 5, 10, 24, 23, 12], zp24Types = [6, 7, 8];
// How a zone is right now, from zone-status: [words, tone]
function zpNowWords(z) {
  if (!z || !z.bound) return [t('No sensor'), 'warn'];
  if (z.unreachable) return [t('Not reachable'), 'bad'];
  if (z.check) return [t('Check sensor'), 'warn'];
  if (!z.known) return [t('Waiting for the sensor'), ''];
  return [z.words || (z.active ? tc('zone state', 'Open') : t('Normal')), z.active ? 'info' : 'good'];
}

// ---- Installer > Zones: every zone, where its sensor is and how it is now, then the sources ---------------------------

function zpSetupPage(arg) { return arg ? zpPage(arg) : zpListPage(); }
// An old address (#/zones/setup, #/zones/5, #/binding/5, #/sensors): the page at its new one.
async function zpMoved(arg) {
  const out = await zpSetupPage(arg), hash = '#/zone-setup' + (arg ? '/' + arg : '');
  const html = typeof out === 'string' ? out : out.html, bind = typeof out === 'string' ? null : out.bind;
  return {html, bind() { history.replaceState(null, '', hash); page = 'zone-setup'; renderNav(); bind?.(); }};
}
const zpData = {list: null, where: new Map()};
async function zpListPage() {
  const [alarm, status, map, sources] = await Promise.all([loadAlarm(), api('zone-status').catch(() => null),
    api('zone-sources').catch(() => ({zones: []})), api('sources').catch(() => ({sources: []}))]);
  zpData.list = status?.zones || [];
  zpData.where = new Map(map.zones.map(x => [x.zone, x.address]));
  const zones = alarm.zones.filter(z => z.type !== 0);
  return {
    html: pageHead(t('Zones'), tn('{0} zone in use', '{0} zones in use', zones.length),
      !state.program ? `<button type="button" class="primary" data-go="zone-setup" data-arg="new">${icon('plus')}${ht('Add a zone')}</button>` : '') +
      installerNotice() + `<div id="zp-list">${zpListBody(alarm)}</div>` + zpSourcesSection(sources.sources || [], alarm, map),
    bind() {
      drawn['zp-list'] = zpListBody(alarm);
      poll(async () => { zpData.list = (await api('zone-status')).zones || []; redraw('zp-list', () => zpListBody(alarm)); }, 2000);
    },
  };
}
function zpListBody(alarm) {
  const zones = alarm.zones.filter(z => z.type !== 0), live = new Map((zpData.list || []).map(z => [z.zone, z]));
  const multi = partitionChoices(alarm).length > 1;
  const none = zones.filter(z => !live.get(z.number)?.bound);
  const head = none.length ? notice('warn', tn('{0} zone has no sensor', '{0} zones have no sensor', none.length),
    t('{0}: they never open until they have one.', listWords(none.map(z => z.name || tc('default zone name', 'Zone {0}', z.number))))) : '';
  return head + `<div class="card"><ul class="list zp-list">${zones.map(z => {
    const now = live.get(z.number), [word, tone] = zpNowWords(now), where = now?.bound ? zpWhere(zpData.where.get(z.number)) : '';
    const line = [zoneTypeName(z.type), multi ? (alarm.partitions[z.partition - 1]?.name || tc('default partition name', 'Partition {0}', z.partition)) : '', where].filter(Boolean).join(' · ');
    return `<li><a class="zp-zone" href="#/zone-setup/${z.number}" data-k="zone-${z.number}"><span class="zp-chip ${tone}">${zoneGlyph(z)}</span>
      <span class="main"><strong><span class="num">${z.number}</span>${esc(z.name || tc('default zone name', 'Zone {0}', z.number))}</strong><span>${esc(line)}</span></span>
      ${tag(word, tone)}${icon('chev', 'chev')}</a></li>`;
  }).join('') || `<li class="empty">${ht('No zones yet.')}</li>`}</ul></div>`;
}

// ---- One zone ------------------------------------------------------------------------------------------------------

// What the page is editing: the zone and its three inputs, as the panel had them and as changed here.
const zp = {number: 0, isNew: false, alarm: null, sources: [], live: null, zone: null, before: null, inputs: null, inputsBefore: null,
  picking: null, cands: [], query: '', seen: new Map(), moved: new Set(), learning: 0, timer: 0, listening: false};
const zpClone = x => JSON.parse(JSON.stringify(x));

async function zpPage(arg) {
  const isNew = arg === 'new';
  const [alarm, sources, status] = await Promise.all([loadAlarm(), api('sources').catch(() => ({sources: []})), api('zone-status').catch(() => null)]);
  const back = ['zone-setup', t('Zones')];
  let number = Number(arg);
  if (isNew) {
    const free = alarm.zones.find(z => z.type === 0);
    if (!free) return pageHead(t('Add a zone'), t('Every zone is in use.'), '', back);
    number = free.number;
  }
  const zone = alarm.zones[number - 1];
  if (!zone || (!isNew && zone.type === 0))
    return pageHead(tc('default zone name', 'Zone {0}', arg), t('This zone isn’t in use.'),
      `<button type="button" class="primary" data-go="zone-setup" data-arg="new">${icon('plus')}${ht('Add a zone')}</button>`, back);
  const binding = isNew ? {inputs: [0, 1, 2].map(role => ({role, address: {...ZP_NONE}, mapping: {...ZP_MAP0}}))} : await api('binding/' + number);
  Object.assign(zp, {number, isNew, alarm, sources: (sources.sources || []).filter(s => s.usable), cands: [], query: '', seen: new Map(), moved: new Set(),
    live: (status?.zones || []).find(z => z.zone === number) || null,
    zone: isNew ? {number, name: '', type: 3, device_class: 1, partition: partitionChoices(alarm)[0]?.[0] || 1, flags: 1, user: 0,
      supervision_s: 0, cross_zone: 0, swinger_trips: 0} : zpClone(zone),
    before: zpClone(zone), inputs: zpClone(binding.inputs), inputsBefore: zpClone(binding.inputs),
    picking: isNew ? {role: 0, kind: 0} : null});
  const multi = partitionChoices(alarm).length > 1;
  const title = isNew ? t('Add a zone') : zone.name || tc('default zone name', 'Zone {0}', number);
  const sub = isNew ? t('It becomes zone {0}, the next free number.', number)
    : [tc('default zone name', 'Zone {0}', number), multi ? alarm.partitions[zone.partition - 1]?.name || '' : ''].filter(Boolean).join(' · ');
  return {
    html: pageHead(title, sub, '', back) + installerNotice() +
      `<div id="zp-now">${zpNowCard()}</div><form id="zp-body" novalidate>${zpBody()}</form><div id="zp-dock">${zpDock()}</div>`,
    bind() {
      drawn['zp-now'] = zpNowCard();
      const root = $('#zp-body');
      root.addEventListener('input', zpInput);
      root.addEventListener('change', zpInput);
      root.addEventListener('submit', event => event.preventDefault());
      if (!zp.listening) { $('#main').addEventListener('click', zpClick); zp.listening = true; } // #main outlives the page
      if (!isNew) poll(async () => {
        zp.live = ((await api('zone-status')).zones || []).find(z => z.zone === number) || null;
        redraw('zp-now', zpNowCard);
      }, 1000);
      poll(zpSearch, 1000);
      window.addEventListener('hashchange', zpLeave, {once: true});
      if (zp.picking?.kind) zpSearch();
    },
  };
}

// How the zone is right now: the touchscreen's status row, as the Overview's card.
function zpNowCard() {
  const z = zp.zone, a = zp.inputs[0].address;
  if (zp.isNew) {
    return `<section class="card command"><div class="command-top"><span class="state-badge">${zoneGlyph(z)}</span>
      <div class="state-words"><span class="part-name">${ht('New zone {0}', zp.number)}</span>
      <span class="state">${a.kind ? esc(z.name || t('No name yet')) : ht('Where is the sensor?')}</span>
      <p>${a.kind ? esc(zpWhere(a)) : ht('Pick it first: its type and kind follow from it.')}</p></div></div></section>`;
  }
  const [word, tone] = zpNowWords(zp.live), bound = zp.live?.bound;
  const saved = zp.inputsBefore[0].address;
  const line = !bound ? t('It never opens until it has one.')
    : zp.live?.unreachable ? t('{0} · its source has lost it', zpWhere(saved))
    : !zp.live?.known ? t('{0} · nothing heard from it yet', zpWhere(saved)) : zpWhere(saved);
  const action = !bound && !zp.picking && !zp.inputs[0].address.kind
    ? `<div class="choices one">${choice('fill setup', icon('plus'), t('Pick its sensor'), '', 'data-zp="pick" data-role="0" data-k="pick-main"')}</div>` : '';
  return `<section class="card command ${tone}"><div class="command-top"><span class="state-badge">${zoneGlyph(z)}</span>
    <div class="state-words"><span class="part-name">${ht('Right now')}</span><span class="state">${esc(word)}</span><p>${esc(line)}</p></div></div>
    ${action}</section>`;
}

// A row as fieldRow draws one: the label and its help on the left, the control on the right.
function zpRow(id, label, help, control) {
  return `<div class="row"><div><label class="label" for="${id}">${esc(label)}</label>${help ? `<span class="help">${esc(help)}</span>` : ''}</div>
    <div class="control">${control}</div></div>`;
}
function zpSelect(f, options, value) {
  const list = [...options];
  if (!list.some(o => String(o[0]) === String(value))) list.push([value, String(value)]);
  return `<select id="zp-${f.replace('.', '-')}" data-f="${f}" data-k="f-${f}">${list.map(([v, l]) =>
    `<option value="${esc(v)}"${String(v) === String(value) ? ' selected' : ''}>${esc(l)}</option>`).join('')}</select>`;
}
// A value with buttons beside it: "Wired contact · Input 1 [Change]"
function zpValueRow(label, help, value, buttons) {
  return `<div class="row"><div><span class="label">${esc(label)}</span>${help ? `<span class="help">${esc(help)}</span>` : ''}</div>
    <div class="control zp-value"><span class="what">${value}</span>${buttons}</div></div>`;
}
const zpButton = (text, attrs, cls = 'small') => `<button type="button" class="${cls}" ${attrs}>${esc(text)}</button>`;
function zpOpt(f, title, help, on) {
  return `<label class="opt"><span class="main"><strong>${esc(title)}</strong><small>${esc(help)}</small></span>
    <input type="checkbox" class="switch" data-f="${f}" data-k="f-${f}"${on ? ' checked' : ''}></label>`;
}

function zpBody() {
  const cards = [zpZoneCard(), zpSensorCard(), zpDoesCard(), zpAdvancedCard()];
  if (zp.isNew && zp.picking?.role === 0) cards.unshift(cards.splice(1, 1)[0]); // a new zone: its sensor first, then its name
  return cards.join('') + zpApartCard();
}
function zpZoneCard() {
  const z = zp.zone, alarm = zp.alarm, types = zoneTypes();
  const rows = [
    zpRow('zp-zone-name', t('Name'), '', `<input id="zp-zone-name" data-f="zone.name" data-k="f-name" maxlength="24" value="${esc(z.name)}" placeholder="${ht('For example, Front door')}">`),
    spokenRow(zp.isNew ? null : zp.live?.spoken), // read-only: chosen at the keypad (D14)
    zpRow('zp-zone-type', t('Type'), types.find(x => x[0] === z.type)?.[2] || '', zpSelect('zone.type', types.map(x => [x[0], x[1]]), z.type)),
  ];
  if (partitionChoices(alarm).length > 1) rows.push(zpRow('zp-zone-partition', t('Partition'), '', zpSelect('zone.partition', partitionChoices(alarm), z.partition)));
  if (BUTTON_TYPES.includes(z.type)) rows.push(zpRow('zp-zone-user', t('Acts as'), t('The user whose rights this button has'),
    `<input id="zp-zone-user" type="number" inputmode="numeric" min="0" max="64" data-f="zone.user" data-k="f-user" value="${esc(z.user)}">`));
  return `<section class="card gap zp-card"><div class="group-title">${ht('The zone')}</div><div class="rows">${rows.join('')}</div></section>`;
}
function zpSensorCard() {
  const z = zp.zone, main = zp.inputs[0], a = main.address, m = main.mapping;
  const head = `<section class="card gap zp-card" id="zp-sensor"><div class="group-title">${ht('Its sensor')}</div>`;
  if (zp.picking && zp.picking.role === 0) return head + zpPicker() + '</section>';
  const rows = [
    zpRow('zp-zone-device_class', t('Sensor'), t('Its icon here, on the keypad and in integrations'), zpSelect('zone.device_class', deviceClasses(), z.device_class)),
    zpValueRow(t('Source'), a.kind ? '' : t('How this zone’s sensor is connected'), zpWhereHtml(a),
      zpButton(a.kind ? t('Change') : t('Pick'), 'data-zp="pick" data-role="0" data-k="pick-0"', a.kind ? 'small' : 'small primary')),
  ];
  if (a.kind === 2) {
    rows.push(zpRow('zp-contact', t('Contact'), m.invert ? t('Closes when it trips: a cut wire goes unnoticed') : t('Closed while the door is shut: a cut wire shows as open'),
      zpSelect('contact', [[0, t('Normally closed')], [1, t('Normally open')]], m.invert ? 1 : 0)));
  } else if (a.kind) {
    rows.push(zpRow('zp-read', t('Open when'), t('When the zone counts its sensor as open'),
      zpSelect('read', [[0, t('On or open')], [1, t('Above a value')], [2, t('Below a value')], [3, t('In a named state')]], m.mode)));
    if (m.mode === 0) rows.push(`<div class="row"><div><label class="label" for="zp-invert">${ht('Reverse open and closed')}</label>
      <span class="help">${ht('For a sensor that reports the other way round')}</span></div><div class="control">
      <input id="zp-invert" type="checkbox" class="switch" role="switch" data-f="invert" data-k="f-invert"${m.invert ? ' checked' : ''}></div></div>`);
    if (m.mode === 1 || m.mode === 2) {
      rows.push(zpRow('zp-level', t('Value'), '', `<input id="zp-level" type="number" step="0.01" data-f="level" data-k="f-level" value="${esc(m.threshold_x100 / 100)}">`));
      rows.push(zpRow('zp-hyst', t('Hysteresis'), t('How far back the value must move before the zone restores'),
        `<input id="zp-hyst" type="number" step="0.01" min="0" data-f="hyst" data-k="f-hyst" value="${esc(m.hysteresis_x100 / 100)}">`));
    }
    if (m.mode === 3) rows.push(zpRow('zp-states', t('Named states'), t('Comma between them'),
      `<input id="zp-states" data-f="states" data-k="f-states" value="${esc(m.states)}" placeholder="open,unlocked">`));
  }
  if (a.kind) rows.push(zpRow('zp-map-response_ms', t('Response time'), t('How long a change must last before it counts'),
    zpSelect('map.response_ms', [[0, t('Source default')], [10, tn('{0} ms', '{0} ms', 10)], [350, tn('{0} ms', '{0} ms', 350)],
      [700, tn('{0} ms', '{0} ms', 700)], [1200, tn('{0} ms', '{0} ms', 1200)]], m.response_ms)));
  if (ZP_HOLD_KINDS.includes(a.kind)) rows.push(zpRow('zp-map-hold_s', t('Hold'), t('How long it stays open after a sensor that only says “open”'),
    zpSelect('map.hold_s', [[0, t('Automatic')], [30, tn('{0} s', '{0} s', 30)], [120, tn('{0} min', '{0} min', 2)], [43200, tn('{0} hour', '{0} hours', 12)]], m.hold_s)));
  let html = head + `<div class="rows">${rows.join('')}</div>`;
  if (!a.kind) return html + '</section>';
  html += `<div class="zp-sub">${ht('When it goes quiet')}</div><div class="rows">
    ${zpRow('zp-zone-supervision_s', t('Supervision'), t('Shows Check when it misses a check-in'),
      zpSelect('zone.supervision_s', [0, 3600, 14400, 43200].map(s => [s, zpHours(s)]), z.supervision_s))}
    ${zpRow('zp-map-grace_s', t('Unreachable after'), t('Shows Check when its source loses it'),
      zpSelect('map.grace_s', [0, 60, 300, 900, 3600].map(s => [s, zpMinutes(s)]), m.grace_s))}</div>`;
  html += `<div class="zp-sub">${ht('Other inputs')}</div>`;
  if (zp.picking) return html + zpPicker() + '</section>';
  html += `<div class="rows">${[1, 2].map(role => {
    const x = zp.inputs[role].address;
    return zpValueRow(zpRoleName(role), role === 1 ? t('Shows Tamper when its cover comes off') : t('Shows Low battery when it says so'),
      zpWhereHtml(x),
      zpButton(x.kind ? t('Change') : t('Pick'), `data-zp="pick" data-role="${role}" data-k="pick-${role}"`) +
      (x.kind ? zpButton(t('Remove'), `data-zp="clear" data-role="${role}" data-k="clear-${role}"`) : ''));
  }).join('')}</div>`;
  return html + '</section>';
}
function zpDoesCard() {
  const z = zp.zone, opts = [];
  if (zpNightTypes.includes(z.type)) opts.push(zpOpt('flag.2', t('Night zone'), t('Armed in Night too, not only Away'), z.flags & 2));
  if (zpChimeTypes.includes(z.type)) opts.push(zpOpt('flag.1', t('Chime'), t('Ding when it opens while disarmed and chime is on'), z.flags & 1));
  if (zp24Types.includes(z.type)) opts.push(zpOpt('flag.4', t('Can be bypassed'), t('24-hour zones normally can’t be'), z.flags & 4));
  opts.push(zpOpt('report', t('Report this zone'), zpReport(!(z.flags & 8)), !(z.flags & 8)));
  return `<section class="card gap zp-card"><div class="group-title">${ht('What it does')}</div><div class="opts">${opts.join('')}</div></section>`;
}
function zpAdvancedCard() {
  const z = zp.zone;
  const partners = zp.alarm.zones.filter(x => x.type !== 0 && x.number !== zp.number).map(x => [x.number, x.name || tc('default zone name', 'Zone {0}', x.number)]);
  const limit = z.flags & 16 ? 99 : z.swinger_trips;
  return `<section class="card gap zp-card"><details class="advanced"${z.cross_zone || z.flags & 16 || z.swinger_trips ? ' open' : ''}>
    <summary>${ht('Advanced')}</summary><div class="rows">
    ${zpRow('zp-zone-cross_zone', t('Cross-zone partner'), t('Both must trip close together to cause an alarm'),
      zpSelect('zone.cross_zone', [[0, tc('cross-zone partner', 'None')], ...partners], z.cross_zone))}
    ${zpRow('zp-limit', t('Alarm limit'), t('Stops sounding after this many alarms in one arming'),
      zpSelect('limit', [[0, t('System setting')], ...[1, 2, 3, 4, 5, 6].map(n => [n, tn('{0} time', '{0} times', n)]), [99, tc('alarm limit', 'Never')]], limit))}
    </div></details></section>`;
}
function zpApartCard() {
  if (zp.isNew) return '';
  const bound = zp.inputs[0].address.kind !== 0;
  return `<section class="card gap zp-card zp-apart"><div class="rows">
    ${bound ? zpValueRow(t('Remove the sensor, keep the zone'), t('The zone stays, with no sensor: it never opens until you pick one again.'), '',
      zpButton(t('Remove the sensor'), 'data-zp="clear" data-role="0" data-k="clear-0"', 'small danger')) : ''}
    ${zpValueRow(t('Remove this zone'), t('The panel stops watching this sensor. Its history stays in the event log.'), '',
      zpButton(t('Remove zone'), 'data-zp="remove" data-k="remove"', 'small danger'))}</div></section>`;
}

// ---- Picking a sensor: "Where is the sensor?", then what that source sees ------------------------------------------

function zpPicker() {
  const p = zp.picking, role = p.role;
  const what = role === 0 ? (zp.isNew ? t('How the new zone’s sensor is connected') : t('How this zone’s sensor is connected'))
    : role === 1 ? t('Where its tamper input is') : t('Where its low-battery input is');
  const canClose = !zp.isNew || zp.inputs[0].address.kind;
  const close = canClose ? `<button type="button" class="small" data-zp="close" data-k="pick-close">${icon('back')}${ht('Back')}</button>` : '';
  if (!p.kind) {
    const kinds = zpKinds();
    return `<div class="sheet zp-pick"><div class="sheet-head"><div class="main"><h3>${ht('Where is the sensor?')}</h3><p>${esc(what)}</p></div>${close}</div>
      <div class="choices">${zp.sources.map(s => { const [name, help, glyph] = kinds[s.kind] || [sourceName(s.kind), '', 'radio'];
        return choice('', icon(glyph), name, help, `data-zp="kind" data-kind="${s.kind}" data-k="kind-${s.kind}"`); }).join('')}</div>
      ${zp.isNew && !zp.inputs[0].address.kind ? `<p class="zp-hint"><button type="button" class="link" data-zp="later" data-k="later">${ht('Add it without a sensor for now')}</button></p>` : ''}</div>`;
  }
  const kind = p.kind, wired = kind === 2 || kind === 6, learns = [3, 5, 7].includes(kind), search = kind === 4;
  const codes = Boolean(zpCodesQuery()), tamper = role === 1; // a 433 MHz zone's tamper or low-battery code (RF-021)
  const title = codes ? (tamper ? t('Find the tamper code') : t('Find the low-battery code'))
    : wired ? t('Pick the terminal') : learns ? t('Find the sensor') : t('Pick the sensor');
  const note = codes ? (tamper ? t('Open and close the door, then take the cover off') : t('Pick the code it sends'))
    : wired ? t('Open and close the sensor: its terminal shows it') : learns ? t('Open the door, then close it') : zpKinds()[kind]?.[1] || '';
  const hint = codes ? (tamper ? t("Open and close the door once, then take the sensor's cover off (or let go of its tamper switch). Pick the code that appeared as the cover came off.")
    : t('Open and close the door once. A sensor sends its low-battery code only when its battery runs low: power it from a lower voltage to hear it, or pick the code by hand.'))
    : wired ? t('Wire the contact between a terminal and GND. A normally closed contact reads Closed while the door is shut.')
    : learns ? t('Start with the door closed. A remote or a motion sensor: trigger it twice.') : '';
  return `<div class="sheet zp-pick"><div class="sheet-head"><span class="glyph">${icon(zpKinds()[kind]?.[2] || 'radio')}</span>
      <div class="main"><h3>${esc(title)}</h3><p>${esc(note)}</p></div>
      <button type="button" class="small" data-zp="kinds" data-k="pick-kinds">${icon('back')}${ht('Back')}</button></div>
    ${hint ? `<p class="zp-hint">${esc(hint)}</p>` : ''}
    ${search ? `<div class="zp-search"><input type="search" data-f="query" data-k="f-query" value="${esc(zp.query)}" placeholder="${ht('Name, room or entity id. Empty shows everything')}" aria-label="${ht('Find the sensor')}"></div>` : ''}
    <ul class="list zp-cands" id="zp-cands">${zpCandsBody()}</ul>
    <details class="advanced zp-manual"><summary>${ht('Type its address instead')}</summary><div class="rows">
      ${zpRow('zp-m-id', t('Device ID'), t('The number the source reports for this sensor'), `<input id="zp-m-id" inputmode="numeric" maxlength="20" data-k="m-id" value="0">`)}
      ${zpRow('zp-m-input', t('Input or terminal'), '', `<input id="zp-m-input" type="number" min="0" max="255" data-k="m-input" value="0">`)}
      ${zpRow('zp-m-ref', t('Reference'), t('An entity or name, for sources that use one'), `<input id="zp-m-ref" data-k="m-ref" value="">`)}
      <div class="row"><div></div><div class="control">${zpButton(t('Use this address'), 'data-zp="manual" data-k="manual"', 'small primary')}</div></div>
    </div></details></div>`;
}
const zpKey = a => `${a.kind}/${a.input}/${a.id}/${a.ref}`;
const zpZoneName = n => zp.alarm.zones[n - 1]?.name || tc('default zone name', 'Zone {0}', n);
// A 433 MHz source's own words for what it heard, as the touchscreen's picker says them (ui_sources.c rf_status,
// RF-011): the sensor it is sure of, with a button to use it, or how many have answered so far.
function zpRfStatus(cands) {
  const best = cands.find(c => c.best && !c.unusable), heard = cands.filter(c => c.changed);
  if (best && best.zone && best.zone !== zp.number)
    return `<li class="zp-status warn">${ht('{0} answered, but it already feeds {1}.', best.name, zpZoneName(best.zone))}</li>`;
  if (best)
    return `<li class="zp-status good"><span class="main">${ht('Found it: {0} answered each time.', best.name)}</span>
      ${zpButton(t('Use {0}', best.name), `data-zp="cand" data-key="${esc(zpKey(best.address))}" data-k="use-best"`, 'small primary')}</li>`;
  if (!heard.length) return `<li class="empty">${ht('Listening: nothing heard yet.')}</li>`;
  return `<li class="zp-status">${heard.length === 1 ? ht('Heard {0}. Close the door (or press the button again) to be sure.', heard[0].name)
    : esc(tn('Heard {0} sensors. Open and close the door once more: the one that answers is picked.', 'Heard {0} sensors. Open and close the door once more: the one that answers is picked.', heard.length))}</li>`;
}
// A 433 MHz zone's tamper or low-battery input is a code of the zone's own sensor (RF-021, bug B012): the picker asks
// the source for that sensor's codes, as the touchscreen does (ui_sources.c, zsrc_rf_codes_query: "codes <protocol>
// <device in hex>"), never for the sensors around. '' for every other picker.
function zpCodesQuery() {
  const p = zp.picking, a = zp.inputs[0].address;
  if (!p || p.kind !== ZP_RF_KIND || !p.role || a.kind !== ZP_RF_KIND || a.ref || !(Number(a.input) >= 1)) return '';
  let id;
  try { id = BigInt(a.id); } catch { return ''; }
  if (id < 0n || id >> 40n) return '';
  return `codes ${Number(id >> 32n)} ${(id & 0xFFFFFFFFn).toString(16).toUpperCase()}`;
}
// The zone's input a code is already bound to (0, 1, 2), or -1.
const zpCodeRole = c => [0, 1, 2].find(r => zp.inputs[r].address.kind && zpKey(zp.inputs[r].address) === zpKey(c.address)) ?? -1;
// Whether a code can be the input being picked: not the zone's own code or its other input's, not another zone's, not
// one the source advises against (its closing code: ui_sources.c code_pickable).
function zpCodePickable(c) {
  const r = zpCodeRole(c);
  return (r < 0 || r === zp.picking.role) && !(c.zone && c.zone !== zp.number) && !c.unusable;
}
// The codes the zone's sensor sent while the picker was open, as the touchscreen's fill_codes shows them: no "Found it",
// no sensors around, the source's reason on a code it advises against.
function zpCodesBody() {
  const tamper = zp.picking.role === 1;
  const lines = [t("The zone's own code"), t("The zone's tamper code"), t("The zone's low-battery code")];
  let html = '';
  if (!zp.cands.length) html = `<li class="empty">${ht('Listening: nothing heard from this sensor yet.')}</li>`;
  else if (!zp.cands.some(zpCodePickable)) {
    html = `<li class="zp-status warn">${tamper ? ht("Only its door codes so far. If taking the cover off sends one of them, this sensor can't report a tamper apart from the door: leave Tamper input as None.")
      : ht('Only its door codes so far.')}</li>`;
  }
  return html + zp.cands.map(c => {
    const key = zpKey(c.address), r = zpCodeRole(c), taken = c.zone && c.zone !== zp.number, ok = zpCodePickable(c);
    const fact = taken ? `${c.detail || ''} · ${t('In use: {0}', zpZoneName(c.zone))}` : r >= 0 ? `${c.detail || ''} · ${lines[r]}` : c.detail || '';
    const value = c.state ? tag(String(c.state).replace(/^[a-z]/, l => l.toUpperCase())) : '';
    return `<li><button type="button" class="zp-cand${ok ? ' fresh' : ''}" data-zp="cand" data-key="${esc(key)}" data-k="cand-${esc(key)}"${ok ? '' : ' disabled'}>
      <span class="main"><strong>${esc(c.name)}</strong>${fact ? `<span class="fact">${esc(fact)}</span>` : ''}</span>
      ${value}</button></li>`;
  }).join('');
}
function zpCandsBody() {
  if (!zp.picking?.kind) return '';
  if (zpCodesQuery()) return zpCodesBody();
  const role = zp.picking.role, mineKey = zpKey(zp.inputs[role].address), rf = zp.picking.kind === ZP_RF_KIND;
  const cands = zp.cands.map(c => {
    const key = zpKey(c.address);
    // 433 MHz: the source says which one was tested (changed while learning, best); its state is an age, so a
    // change of it says nothing (code review RF24)
    return {c, key, fresh: rf ? Boolean(c.changed || c.best) : zp.moved.has(key), mine: key === mineKey,
      taken: c.zone && c.zone !== zp.number};
  });
  // 433 MHz keeps the source's order (heard while learning first, newest first); elsewhere a just-changed one goes first
  if (!rf) cands.sort((a, b) => (b.fresh - a.fresh) || (a.taken - b.taken));
  const status = rf ? zpRfStatus(zp.cands) : '';
  if (!cands.length) return status || `<li class="empty">${[3, 5, 7].includes(zp.picking.kind) ? ht('Listening: nothing heard yet.') : ht('Nothing found. Try a different search.')}</li>`;
  let before = false;
  return status + cands.map(({c, key, fresh, mine, taken}) => {
    const open = ['open', 'on'].includes(String(c.state).toLowerCase());
    // an unusable one (a sensor's closing code as its tamper code, RF-021) shows the source's reason and can't be picked
    const off = taken || c.unusable;
    const fact = mine ? t('This zone’s sensor') : taken ? t('In use: {0}', zpZoneName(c.zone))
      : fresh && !rf && !c.unusable ? t('Just opened or closed') : c.detail || '';
    const head = rf && !c.changed && !before ? (before = true, `<li class="zp-sect">${ht('Already transmitting before you started')}</li>`) : '';
    // a 433 MHz sensor's state is when it was last heard ("5 s ago"), shown as the source says it, capital first
    const value = !c.state ? '' : rf ? tag(String(c.state).replace(/^[a-z]/, l => l.toUpperCase()))
      : tag(open ? tc('zone state', 'Open') : tc('zone state', 'Closed'), open ? 'info' : '');
    return `${head}<li><button type="button" class="zp-cand${fresh && !c.unusable ? ' fresh' : ''}${mine ? ' mine' : ''}" data-zp="cand" data-key="${esc(key)}" data-k="cand-${esc(key)}"${off ? ' disabled' : ''}>
      <span class="main"><strong>${esc(c.name)}</strong>${fact ? `<span class="fact">${esc(fact)}</span>` : ''}</span>
      ${value}</button></li>`;
  }).join('');
}
// What the chosen source sees, read again once a second while the picker is open (its words change at most that often).
async function zpSearch() {
  const p = zp.picking;
  if (!p?.kind) return;
  const query = zpCodesQuery() || zp.query, asked = `${p.kind}/${p.role}/${query}`;
  const r = await api('sources/search', {kind: p.kind, query});
  if (!zp.picking || `${zp.picking.kind}/${zp.picking.role}/${zpCodesQuery() || zp.query}` !== asked) return;
  zp.cands = r.candidates || [];
  // a sensor that changed while the picker was open stays marked; a 433 MHz source marks it itself (changed), and its
  // state is how long ago it was heard, which changes with no new packet (code review RF24)
  if (p.kind !== ZP_RF_KIND) for (const c of zp.cands) {
    const key = zpKey(c.address);
    if (!zp.seen.has(key)) zp.seen.set(key, c.state); else if (zp.seen.get(key) !== c.state) zp.moved.add(key);
  }
  redraw('zp-cands', zpCandsBody);
}
async function zpLearn(kind) {
  if (zp.learning === kind) return;
  if (zp.learning) await api('sources/learn', {kind: zp.learning, enabled: false}).catch(() => {});
  zp.learning = [3, 5, 7].includes(kind) ? kind : 0;
  if (zp.learning) await api('sources/learn', {kind, enabled: true}).catch(() => {});
}
function zpLeave() {
  if (zp.learning) api('sources/learn', {kind: zp.learning, enabled: false}).catch(() => {});
  zp.learning = 0;
  zp.picking = null;
}
function zpUse(address, cand) {
  const role = zp.picking.role;
  const newBattery = role === ZP_BATTERY_ROLE && address.kind === ZP_RF_KIND && zp.inputs[role].address.kind !== ZP_RF_KIND;
  zp.inputs[role].address = {...address};
  if (newBattery && !zp.inputs[role].mapping.hold_s) zp.inputs[role].mapping.hold_s = ZP_RF_BATTERY_HOLD_S; // RF-022
  if (role === 0) {
    zp.inputs[0].mapping = {...ZP_MAP0, grace_s: zp.inputs[0].mapping.grace_s};
    if (zp.isNew && cand) { // a new zone from a picked sensor starts with what it says it is (docs/features/22)
      const cls = cand.device_class || 1;
      Object.assign(zp.zone, {device_class: cls, type: zpTypeFor(cls)});
      if (!zp.zone.name && cand.address.kind === 4) zp.zone.name = cand.name.slice(0, 24);
      if (!zpChimeTypes.includes(zp.zone.type)) zp.zone.flags &= ~1;
    }
  }
  zp.picking = null;
  zpLearn(0);
  zpDraw(zp.isNew && role === 0 ? 'f-name' : `pick-${role}`);
}

// ---- Editing: every control writes the model; the dock says what changed --------------------------------------------

function zpInput(event) {
  const el = event.target, f = el.dataset?.f;
  if (!f) return;
  const z = zp.zone, m = zp.inputs[0].mapping, n = () => Number(el.value);
  let again = false;
  if (f === 'query') {
    zp.query = el.value;
    clearTimeout(zp.timer);
    zp.timer = setTimeout(zpSearch, 300);
    return;
  }
  if (f.startsWith('zone.')) {
    const k = f.slice(5);
    if (k === 'name') z.name = el.value; else z[k] = n();
    again = event.type === 'change' && ['type', 'device_class'].includes(k);
  } else if (f.startsWith('flag.')) {
    const bit = Number(f.slice(5));
    z.flags = el.checked ? z.flags | bit : z.flags & ~bit;
  } else if (f === 'report') {
    z.flags = el.checked ? z.flags & ~8 : z.flags | 8;
    const small = el.closest('label')?.querySelector('small');
    if (small) small.textContent = zpReport(el.checked);
  } else if (f === 'limit') {
    if (n() === 99) z.flags |= 16; else { z.flags &= ~16; z.swinger_trips = n(); }
  } else if (f === 'contact') {
    m.invert = el.value === '1';
    again = event.type === 'change';
  } else if (f === 'read') {
    m.mode = n();
    again = event.type === 'change';
  } else if (f === 'invert') m.invert = el.checked;
  else if (f === 'level') m.threshold_x100 = Math.round(n() * 100);
  else if (f === 'hyst') m.hysteresis_x100 = Math.round(n() * 100);
  else if (f === 'states') m.states = el.value;
  else if (f.startsWith('map.')) m[f.slice(4)] = n();
  if (again) return zpDraw(el.dataset.k);
  zpDockDraw();
  if (zp.isNew && f === 'zone.name') { const now = $('#zp-now'); if (now) now.innerHTML = drawn['zp-now'] = zpNowCard(); }
}
async function zpClick(event) {
  const el = event.target.closest('[data-zp]');
  if (!el || el.disabled || !$('#zp-body')) return;
  const act = el.dataset.zp, role = Number(el.dataset.role || 0);
  if (act === 'pick') {
    if (role && !zp.inputs[0].address.kind) return toast(t('Pick the zone’s own sensor first'), 'error');
    zp.picking = {role, kind: 0};
    zp.cands = [];
    return zpDraw('pick-kinds', true);
  }
  if (act === 'kind') {
    zp.picking.kind = Number(el.dataset.kind);
    zp.cands = [];
    zp.query = '';
    zp.seen = new Map();
    zp.moved = new Set();
    zpDraw('pick-kinds', true);
    await zpLearn(zp.picking.kind);
    return zpSearch();
  }
  if (act === 'kinds') { zp.picking.kind = 0; zpLearn(0); return zpDraw('kind-2', true); }
  if (act === 'close' || act === 'later') { zp.picking = null; zpLearn(0); return zpDraw(act === 'later' ? 'f-name' : 'pick-0'); }
  if (act === 'cand') {
    const c = zp.cands.find(x => zpKey(x.address) === el.dataset.key);
    // the source advises against it (RF-021), or it is the zone's own code (B012): its row is disabled
    if (c && (zpCodesQuery() ? zpCodePickable(c) : !c.unusable)) zpUse(c.address, c);
    return;
  }
  if (act === 'manual') {
    const id = $('#zp-m-id').value.trim() || '0';
    if (!/^\d{1,20}$/.test(id)) return toast(t('A device ID is digits only.'), 'error');
    return zpUse({kind: zp.picking.kind, id, input: Number($('#zp-m-input').value) || 0, ref: $('#zp-m-ref').value.trim()}, null);
  }
  if (act === 'clear') {
    if (role === 0) zp.inputs = [0, 1, 2].map(r => ({role: r, address: {...ZP_NONE}, mapping: {...ZP_MAP0}}));
    else zp.inputs[role] = {role, address: {...ZP_NONE}, mapping: {...ZP_MAP0}};
    return zpDraw(role ? `pick-${role}` : 'pick-0');
  }
  if (act === 'undo') {
    Object.assign(zp, {zone: zpClone(zp.isNew ? zp.zone : zp.before), inputs: zpClone(zp.inputsBefore), picking: zp.isNew ? {role: 0, kind: 0} : null});
    if (zp.isNew) Object.assign(zp.zone, {name: '', type: 3, device_class: 1, flags: 1});
    return zpDraw('');
  }
  if (act === 'save') return zpSave();
  if (act === 'remove') return zpRemove();
}
// Draws the page's cards again from the model (the picker opened, a type changed), keeping the focus.
function zpDraw(focus, scroll = false) {
  const root = $('#zp-body');
  if (!root) return;
  root.innerHTML = zpBody();
  drawn['zp-cands'] = zpCandsBody();
  if (zp.isNew) { const now = $('#zp-now'); if (now) { drawn['zp-now'] = zpNowCard(); now.innerHTML = drawn['zp-now']; } }
  else redraw('zp-now', zpNowCard);
  zpDockDraw();
  const el = focus ? $(`[data-k="${focus}"]`) : null;
  el?.focus({preventScroll: !scroll});
  if (scroll) $('#zp-sensor')?.scrollIntoView({block: 'nearest'});
}

// ---- Saving: one review of the zone and its sensor --------------------------------------------------------------------

function zpChanges() {
  const out = [], z = zp.zone, b = zp.before, fresh = zp.isNew;
  const add = (label, from, to) => { if (from !== to || fresh) out.push({label, from: fresh ? '' : from, to}); };
  const partName = n => zp.alarm.partitions[n - 1]?.name || tc('default partition name', 'Partition {0}', n);
  const zoneName = n => n ? zp.alarm.zones[n - 1]?.name || tc('default zone name', 'Zone {0}', n) : tc('cross-zone partner', 'None');
  add(t('Name'), b.name || t('(empty)'), z.name || t('(empty)'));
  add(t('Type'), zoneTypeName(b.type), zoneTypeName(z.type));
  if (partitionChoices(zp.alarm).length > 1) add(t('Partition'), partName(b.partition), partName(z.partition));
  add(t('Sensor'), deviceClassName(b.device_class), deviceClassName(z.device_class));
  for (let role = 0; role < 3; role++) {
    const was = zp.inputsBefore[role].address, now = zp.inputs[role].address;
    if (role && !now.kind && !was.kind) continue;
    if (zpKey(was) !== zpKey(now) || (fresh && role === 0)) out.push({label: zpRoleName(role), from: fresh ? '' : zpWhere(was), to: zpWhere(now)});
  }
  const mb = zp.inputsBefore[0].mapping, m = zp.inputs[0].mapping, kind = zp.inputs[0].address.kind;
  if (kind) {
    const was = zpReads(mb, kind), now = zpReads(m, kind);
    if (was !== now && !fresh && zp.inputsBefore[0].address.kind === kind) out.push({label: kind === 2 ? t('Contact') : t('Open when'), from: was, to: now});
    if (m.mode === 1 || m.mode === 2) {
      if (mb.threshold_x100 !== m.threshold_x100) out.push({label: t('Value'), from: String(mb.threshold_x100 / 100), to: String(m.threshold_x100 / 100)});
      if (mb.hysteresis_x100 !== m.hysteresis_x100) out.push({label: t('Hysteresis'), from: String(mb.hysteresis_x100 / 100), to: String(m.hysteresis_x100 / 100)});
    }
    if (m.mode === 3 && mb.states !== m.states) out.push({label: t('Named states'), from: mb.states, to: m.states});
    if (mb.response_ms !== m.response_ms) out.push({label: t('Response time'), from: zpMs(mb.response_ms), to: zpMs(m.response_ms)});
    if (mb.grace_s !== m.grace_s) out.push({label: t('Unreachable after'), from: zpMinutes(mb.grace_s), to: zpMinutes(m.grace_s)});
    if (ZP_HOLD_KINDS.includes(kind) && mb.hold_s !== m.hold_s) out.push({label: t('Hold'), from: zpHold(mb.hold_s), to: zpHold(m.hold_s)});
  }
  if (BUTTON_TYPES.includes(z.type) && b.user !== z.user && !fresh) out.push({label: t('Acts as'), from: String(b.user), to: String(z.user)});
  if (b.supervision_s !== z.supervision_s) out.push({label: t('Supervision'), from: zpHours(b.supervision_s), to: zpHours(z.supervision_s)});
  for (const [bit, label] of [[2, t('Night zone')], [1, t('Chime')], [4, t('Can be bypassed')]])
    if ((b.flags ^ z.flags) & bit && !fresh) out.push({label, from: zpOnOff(b.flags & bit), to: zpOnOff(z.flags & bit)});
  if ((b.flags ^ z.flags) & 8 && !fresh) out.push({label: t('Report this zone'), from: zpOnOff(!(b.flags & 8)), to: zpOnOff(!(z.flags & 8))});
  if (zpLimit(b) !== zpLimit(z) && !fresh) out.push({label: t('Alarm limit'), from: zpLimit(b), to: zpLimit(z)});
  if (b.cross_zone !== z.cross_zone && !fresh) out.push({label: t('Cross-zone partner'), from: zoneName(b.cross_zone), to: zoneName(z.cross_zone)});
  return out;
}
const zpZoneFields = ['name', 'type', 'partition', 'device_class', 'flags', 'user', 'swinger_trips', 'cross_zone', 'supervision_s'];
function zpDock() {
  if (state.program) return '';
  const changes = zpChanges();
  if (!zp.isNew && !changes.length) return '';
  const top = zp.isNew ? t('New zone {0}', zp.number) : tn('{0} change', '{0} changes', changes.length);
  const line = zp.isNew ? [zp.zone.name || t('No name yet'), zoneTypeName(zp.zone.type), zp.inputs[0].address.kind ? zpWhere(zp.inputs[0].address) : t('no sensor')].join(' · ')
    : listWords(changes.map(c => c.label));
  return `<div class="zp-dock"><span class="main"><strong>${esc(top)}</strong><span>${esc(line)}</span></span>
    ${zp.isNew ? '' : zpButton(t('Undo'), 'data-zp="undo" data-k="undo"', '')}
    ${zpButton(zp.isNew ? t('Review and add') : t('Review and save'), 'data-zp="save" data-k="save"', 'primary')}</div>`;
}
function zpDockDraw() { const dock = $('#zp-dock'); if (dock) dock.innerHTML = zpDock(); }
function zpSave() {
  const z = zp.zone, changes = zpChanges();
  if (!z.name.trim() && zp.isNew) return toast(t('Give the zone a name.'), 'error'), $('[data-k="f-name"]')?.focus();
  const patch = {};
  for (const k of zpZoneFields) if (zp.isNew || z[k] !== zp.before[k]) patch[k] = z[k];
  const inputsChanged = zp.inputs.some((x, i) => JSON.stringify(x) !== JSON.stringify(zp.inputsBefore[i]));
  const number = zp.number, isNew = zp.isNew;
  review({
    title: isNew ? t('Add zone {0}', number) : t('Save {0}', z.name || tc('default zone name', 'Zone {0}', number)),
    changes, confirm: isNew ? t('Add zone') : t('Save'), done: '',
    run: async () => {
      // BL-135: the sign-in is enough, no keypad code
      if (Object.keys(patch).length) {
        await applyAlarm({zones: [{number, ...patch}]});
        await loadAlarm(); // a retry after the sensor step failed carries the new revision
      }
      if (inputsChanged) {
        const r = zp.inputs[0].address.kind
          ? await api('binding/' + number, {inputs: zp.inputs.map((x, role) => ({role, address: x.address, mapping: x.mapping}))})
          : zp.inputsBefore[0].address.kind ? await api('binding/' + number, {action: 'unbind'}) : {};
        if (r.error) throw new Error(friendly(r.error));
      }
    },
    after: async () => { if (isNew) go('zone-setup', number); else await show(); },
  });
}
function zpRemove() {
  const name = zp.zone.name || tc('default zone name', 'Zone {0}', zp.number), number = zp.number, bound = zp.inputsBefore[0].address.kind;
  review({
    title: t('Remove {0}?', name), intro: t('The panel stops watching this sensor. Its history stays in the event log.'),
    changes: [{label: t('Type'), from: zoneTypeName(zp.before.type), to: tc('zone type', 'Not used')},
      ...(bound ? [{label: t('Source'), from: zpWhere(zp.inputsBefore[0].address), to: t('Free for another zone')}] : [])],
    confirm: t('Remove'), danger: true, done: '',
    run: async () => {
      // the zone first: a refusal (armed, installer mode at the panel) then changes nothing; once it
      // is gone, its sensor is freed for another zone
      await applyAlarm({zones: [{number, type: 0}]});
      await loadAlarm(); // a retry after the unbind failed carries the new revision
      if (bound) {
        const r = await api('binding/' + number, {action: 'unbind'});
        if (r.error) throw new Error(friendly(r.error));
      }
    },
    after: () => go('zone-setup'),
  });
}

// ---- Sensor sources: how each source is doing, at the end of Installer > Zones --------------------------------------

function zpSourcesSection(sources, alarm, map) {
  const kinds = zpKinds(), usable = sources.filter(s => s.usable), missing = sources.filter(s => !s.usable);
  const zonesOn = kind => map.zones.filter(x => x.address.kind === kind && alarm.zones[x.zone - 1]?.type).length;
  const row = s => {
    const [name, , glyph] = kinds[s.kind] || [sourceName(s.kind), '', 'radio'];
    const [word, tone] = s.link_down ? [s.trouble ? t('Link down: a sensor radio problem is reported') : t('Link down'), 'warn']
      : s.trouble ? [t('Reporting a trouble'), 'warn'] : [t('Working'), 'good'];
    return `<li><span class="zp-chip ${tone}">${icon(glyph)}</span><div class="main"><strong>${esc(name)}</strong>
      <span>${esc(word)} · ${esc(tn('{0} zone', '{0} zones', zonesOn(s.kind)))}</span></div>
      ${tag(tone === 'good' ? t('Ready') : t('Attention'), tone)}</li>`;
  };
  return `<section class="section"><div class="section-head"><h2>${ht('Sensor sources')}</h2></div>
    <div class="card"><ul class="list">${usable.map(row).join('') || `<li class="empty">${ht('No sensor sources in this firmware.')}</li>`}</ul></div>` +
    (missing.length ? `<p class="gap muted"><small>${esc(t('Not on this panel: {0}.', listWords(missing.map(s => kinds[s.kind]?.[0] || sourceName(s.kind)))))}</small></p>` : '') +
    `</section>`;
}

const partitionSpecs = () => [
  {name: 'enabled', label: t('In use'), type: 'switch'},
  {name: 'name', label: t('Name'), type: 'text'},
  {name: 'exit_delay_s', label: t('Exit time'), type: 'number', unit: t('s'), help: t('Time to leave after arming Away.')},
  {name: 'entry_delay1_s', label: t('Entry time 1'), type: 'number', unit: t('s'), help: t('For Entry/exit 1 doors.')},
  {name: 'entry_delay2_s', label: t('Entry time 2'), type: 'number', unit: t('s'), help: t('For Entry/exit 2 doors.')},
];
const deviceSpecs = () => [
  {name: 'enabled', label: t('In use'), type: 'switch'},
  {name: 'name', label: t('Name'), type: 'text'},
  {name: 'partition', label: t('Home partition'), type: 'select', numeric: true, options: []},
  {name: 'supervision_s', label: t('Check-in supervision'), type: 'number', unit: t('s'), help: t('0: not supervised.')},
];
async function partitionsPage() {
  const [alarm, links] = await Promise.all([loadAlarm(), api('keypads')]);
  const parts = alarm.partitions.filter(p => p.enabled), paired = links.keypads;
  const devices = alarm.devices.filter(d => d.enabled && !paired.some(k => k.number === d.number));
  const editable = isInstaller() && !state.program && !state.locked && !links.busy;
  const partName = n => parts.find(p => p.number === n)?.name || t('All partitions');
  const removeButton = disabled => `<button type="button" class="danger" id="remove-entry" style="margin-right:auto"${disabled ? ' disabled' : ''}>${ht('Remove')}</button>`;
  const back = () => go('partitions');
  const summary = k => !k.allowed ? t('Locked to {0}', partName(k.home)) : k.never ? t('Default: {0} · No automatic return', partName(k.home)) : t('Default: {0} · Returns after {1} seconds inactive', partName(k.home), k.seconds);
  const local = links.local ? {...links.local, number: 0, name: links.local.name || t('Panel keypad'), online: true} : null;
  const keypads = [...(local ? [local] : []), ...paired];
  const freeDevice = alarm.devices.find(d => !d.enabled && !paired.some(k => k.number === d.number));
  const freePart = alarm.partitions.find(p => !p.enabled);
  function remove(title, run, password = false) {
    review({title: t('Remove {0}?', title), confirm: t('Remove'), danger: true, password, done: t('Removed.'), run, after: back});
  }
  function partition(p, isNew = false) {
    const assigned = keypads.some(k => k.home === p.number) || devices.some(d => d.partition === p.number) || alarm.zones.some(z => z.type && z.partition === p.number);
    const reason = !isNew && (parts.length === 1 ? t('The last partition cannot be removed.') : assigned ? t('Move assigned devices and zones before removing this partition.') : '');
    editDialog({title: isNew ? t('Add partition') : p.name, intro: reason,
      specs: withSchema(partitionSpecs().filter(s => s.name !== 'enabled'), alarm.schema.partitions, false),
      values: isNew ? {} : p, initial: p, readOnly: !editable, extraButtons: isNew ? '' : removeButton(!editable || !!reason),
      bindExtra(form) { const b = form.querySelector('#remove-entry'); if (b) b.onclick = () => remove(p.name, () => applyAlarm({partitions: [{number: p.number, enabled: false}]})); },
      reviewOptions: {title: t('Save partition'), confirm: t('Apply'), done: t('Partition saved.'), after: back,
        run: values => applyAlarm({partitions: [{...p, ...values, enabled: true}]})}});
  }
  function device(d, isNew = false) {
    const specs = withSchema(deviceSpecs().filter(s => s.name !== 'enabled').map(s => s.name === 'partition' ? {...s, options: partitionChoices(alarm, true)} : s), alarm.schema.devices, false);
    editDialog({title: isNew ? t('Add device') : d.name, specs, values: isNew ? {} : d, initial: d, readOnly: !editable,
      extraButtons: isNew ? '' : removeButton(!editable),
      bindExtra(form) { const b = form.querySelector('#remove-entry'); if (b) b.onclick = () => remove(d.name, () => applyAlarm({devices: [{number: d.number, enabled: false}]})); },
      reviewOptions: {title: t('Save device'), confirm: t('Apply'), done: t('Device saved.'), after: back,
        run: values => applyAlarm({devices: [{...d, ...values, enabled: true}]})}});
  }
  function keypad(k) {
    const canEdit = editable && (k.online || k.waiting);
    const specs = [
      {name: 'name', label: t('Name'), type: 'text', required: true, maxLength: 47},
      {name: 'home', label: t('Default partition'), type: 'select', numeric: true, options: partitionChoices(alarm)},
      {name: 'allowed', label: t('Allow partition switching'), type: 'switch', help: t('Off locks this keypad to its default partition.')},
      {name: 'seconds', label: t('Return after inactivity'), type: 'number', min: 1, max: 3600, step: 1, unit: t('seconds'),
        help: t('Default: 30 seconds. Using any screen restarts the inactivity timer.'), showIf: v => v.allowed},
      {name: 'never', label: t('Never return automatically'), type: 'switch', showIf: v => v.allowed},
    ];
    editDialog({title: k.name, intro: !k.number ? t('The built-in keypad cannot be removed.') : '', specs, values: k, readOnly: !canEdit,
      extraButtons: removeButton(!canEdit || !k.number),
      bindExtra(form) {
        form.querySelector('#remove-entry').onclick = () => remove(k.name, me_password => api('keypads', {op: 'remove', device: k.number, me_password}), true);
        const update = () => { const input = form.querySelector('[name="seconds"]'), never = form.querySelector('[name="never"]').checked;
          input.disabled = !canEdit || never; input.closest('[data-field]').classList.toggle('keypad-return-disabled', never); };
        form.addEventListener('change', update); update();
      },
      reviewOptions: {title: t('Save keypad settings'), password: true, done: t('Device saved.'), after: back,
        run: (values, me_password) => api('keypads', {op: 'save', ...k, ...values, device: k.number, me_password})}});
  }
  function addDevice() {
    openDialog(`<div class="dialog-head"><h2 id="dialog-title">${ht('Add device')}</h2></div><div class="dialog-body"><div class="enroll-choices">
      <button id="add-keypad"${!links.available || paired.length >= links.capacity || !alarm.devices.some(d => d.number <= links.capacity && !d.enabled && !paired.some(k => k.number === d.number)) ? ' disabled' : ''}>${ht('Keypad')}</button>
      <button id="add-expander">${ht('Expander')}</button><button id="add-receiver">${ht('Receiver')}</button></div></div>
      <div class="dialog-foot"><button id="dialog-cancel">${ht('Cancel')}</button></div>`);
    $('#dialog-cancel').onclick = closeDialog;
    $('#add-keypad').onclick = () => keypadEnrollment();
    $('#add-expander').onclick = () => device({...freeDevice, name: t('Expander'), partition: parts[0].number}, true);
    $('#add-receiver').onclick = () => device({...freeDevice, name: t('Receiver'), partition: parts[0].number}, true);
  }
  return {
    html: pageHead(t('Partitions and devices'), t('Areas that arm separately and the devices assigned to them.')) + installerNotice() +
      `<section class="section"><div class="section-head"><h2>${ht('Partitions')}</h2><button id="add-partition"${!editable || !freePart ? ' disabled' : ''}>${ht('Add partition')}</button></div>
      <div class="card"><ul class="list clickable">${parts.map(p => `<li data-partition="${p.number}" tabindex="0"><div class="main"><strong>${esc(p.name)}</strong><span>${ht('Exit {0} · entry {1} / {2}', fmtDuration(p.exit_delay_s), fmtDuration(p.entry_delay1_s), fmtDuration(p.entry_delay2_s))}</span></div><button>${ht('Edit')}</button></li>`).join('')}</ul></div></section>
      <section class="section"><div class="section-head"><h2>${ht('Devices')}</h2><button id="add-device"${!editable || !freeDevice ? ' disabled' : ''}>${ht('Add device')}</button></div>
      <div class="card"><ul class="list clickable">${keypads.map(k => `<li data-keypad="${k.number}" tabindex="0"><div class="main"><strong>${esc(k.name)}</strong><span>${esc(summary(k))}</span></div>${k.number ? tag(k.online ? t('Online') : k.waiting ? t('Waiting for status') : t('Not responding'), k.online ? 'good' : 'warn') : ''}<button>${ht('Edit')}</button></li>`).join('')}
      ${devices.map(d => `<li data-device="${d.number}" tabindex="0"><div class="main"><strong>${esc(d.name || t('Device {0}', d.number))}</strong><span>${esc(partName(d.partition))}</span></div><button>${ht('Edit')}</button></li>`).join('')}
      ${!keypads.length && !devices.length ? `<li class="empty">${ht('No devices added.')}</li>` : ''}</ul></div></section>` +
      (links.enrollment && links.enrollment.state > 0 ? `<button id="resume-pairing">${ht('View keypad enrollment')}</button>` : ''),
    bind() {
      const wire = (kind, fn) => $$(`[data-${kind}]`).forEach(li => { li.onclick = () => fn(Number(li.dataset[kind])); li.onkeydown = e => { if (e.key === 'Enter') fn(Number(li.dataset[kind])); }; });
      wire('partition', n => partition(parts.find(p => p.number === n)));
      wire('device', n => device(devices.find(d => d.number === n)));
      wire('keypad', n => keypad(keypads.find(k => k.number === n)));
      $('#add-partition').onclick = () => partition({...freePart, name: tc('default partition name', 'Partition {0}', freePart.number)}, true);
      $('#add-device').onclick = addDevice;
      if ($('#resume-pairing')) $('#resume-pairing').onclick = () => keypadEnrollment(links.enrollment);
    },
  };
}

// Server owns the enrollment. Polling reads facts; it never manufactures keypad events.
function keypadEnrollment(resume = null) {
  let enrollment = resume, stopped = false, timer = null;
  const id = resume?.attempt || Array.from(crypto.getRandomValues(new Uint8Array(16)), n => n.toString(16).padStart(2, '0')).join('');
  const dialog = $('#dialog');
  function cleanup() { stopped = true; clearTimeout(timer); dialog.removeEventListener('close', cleanup); }
  dialog.addEventListener('close', cleanup);
  async function send(body) {
    try { await api('keypads', {attempt: id, me_password: dialogPassword()?.value || '', ...body}); await poll(); }
    catch (error) { if (!stopped) { $('#dialog-error').textContent = error.message; if (!askPasswordAgain(error)) timer = setTimeout(poll, 1500); } }
  }
  function draw() {
    const s = enrollment?.state || 0, done = s === 5, failed = s === 6;
    const title = done ? t('Keypad enrolled') : failed ? t('Pairing ended') : s === 3 ? t('Compare the codes') : s === 4 ? t('Confirm on the keypad') : s ? t('Waiting for a keypad') : t('Add a keypad');
    const body = done ? `<p>${ht('Choose its name and partition next.')}</p>` : failed ? `<p>${enrollment.fail === 3 ? ht('The codes did not match. Start pairing again.') : enrollment.fail === 7 ? ht('More than one keypad tried to pair. Try one at a time.') : ht('Pairing ended or expired. Check the device list before trying again.')}</p>` : s === 3 || s === 4 ?
      `<p>${s === 3 ? ht('Does this code match your keypad?') : ht('Confirmed here. Tap Confirm on the keypad to finish.')}</p><div class="enroll-code">${esc(enrollment.peer)}<strong>${esc(enrollment.code)}</strong></div>` : s ?
      `<p>${ht('Tap Pair on the new keypad. Keep this window open.')}</p><p class="muted">${ht('Pairing window: 2 minutes')}</p>` :
      `<p>${ht('Prepare the new keypad:')}</p><ol class="enroll-steps"><li>${ht('Choose Keypad in its setup.')}</li><li>${ht('Tap Find panel. If out of range, tap Select Wi-Fi.')}</li></ol>`;
    openDialog(`<div class="dialog-head"><h2 id="dialog-title">${esc(title)}</h2></div><div class="dialog-body enroll-body">${body}${!s && passwordDue() ? passwordField() : ''}</div>
      <p class="dialog-error" id="dialog-error" role="alert"></p><div class="dialog-foot enroll-foot"><button id="enroll-cancel">${done || failed ? ht('Close') : ht('Cancel')}</button>
      ${!s ? `<button class="primary" id="enroll-start">${ht('Start pairing')}</button>` : s === 3 ? `<button id="enroll-no">${ht('No match')}</button><button class="primary" id="enroll-yes">${ht('Matches')}</button>` : done ? `<button class="primary" id="enroll-setup">${ht('Set up keypad')}</button>` : ''}</div>`);
    $('#enroll-cancel').onclick = async () => { if (s && !done && !failed) { try { await api('keypads', {op: 'cancel', attempt: id, me_password: dialogPassword()?.value || ''}); } catch (e) { if (!askPasswordAgain(e)) $('#dialog-error').textContent = e.message; return; } } cleanup(); closeDialog(); go('partitions'); };
    if (!s) $('#enroll-start').onclick = async () => { const me_password = dialogPassword()?.value || ''; await send({op: 'start', me_password}); if (enrollment && me_password) passwordConfirmed(); };
    if (s === 3) { $('#enroll-no').onclick = () => send({op: 'confirm', allow: false, code: enrollment.code, sequence: enrollment.sequence}); $('#enroll-yes').onclick = () => send({op: 'confirm', allow: true, code: enrollment.code, sequence: enrollment.sequence}); }
    if (done) $('#enroll-setup').onclick = async () => { const number = enrollment.device; cleanup(); closeDialog(); await refreshStatus(); await show(); $(`[data-keypad="${number}"]`)?.click(); };
  }
  async function poll() {
    if (stopped || !dialog.open) return;
    try {
      const data = await api('keypads');
      if (data.enrollment?.attempt !== id) throw new Error(t('Pairing status is unknown. Check the device list before starting again.'));
      const next = data.enrollment;
      const changed = !enrollment || ['state', 'fail', 'code', 'peer', 'sequence'].some(k => next[k] !== enrollment[k]);
      enrollment = next;
      if (changed) draw();
      else $('#dialog-error').textContent = '';
      if (next.state < 5) timer = setTimeout(poll, 1000);
    } catch { if (!stopped) { $('#dialog-error').textContent = t('Connection lost. Pairing status is unknown; reconnecting…'); timer = setTimeout(poll, 2000); } }
  }
  draw();
  if (resume) timer = setTimeout(poll, 1000);
}

const optionGroups = () => {
  const s = t('s'), panic = panicKeys();
  return [
    [t('Codes and lockout'), [
      {name: 'code_min_len', label: t('Shortest allowed code'), type: 'number', unit: t('digits')},
      {name: 'lockout_attempts', label: t('Wrong codes before lockout'), type: 'number', help: t('0 turns the lockout off.')},
      {name: 'lockout_s', label: t('Lockout time'), type: 'number', unit: s},
      {name: 'chime_needs_code', label: t('Chime on or off needs a code'), type: 'switch'},
    ]],
    [t('Arming'), [
      {name: 'exit_sounds', label: t('Exit beeps'), type: 'switch'},
      {name: 'auto_stay', label: t('Automatic Stay'), type: 'switch', help: t('Armed Away becomes Stay if nobody leaves through an entry/exit door.')},
      {name: 'quick_arm', label: t('Arm without a code'), type: 'switch', help: t('Anyone at the keypad can arm.')},
      {name: 'quick_exit', label: t('Quick exit'), type: 'switch', help: t('Restart exit time in Stay or Night without a code.')},
      {name: 'exit_restart_limit', label: t('Automatic exit restarts'), type: 'number', help: t('When someone comes back in during exit time.')},
      {name: 'recent_closing_s', label: t('Recent-closing window'), type: 'number', unit: s, help: t('An alarm this soon after arming is reported as a recent closing.')},
      {name: 'remote_arm_without_code', label: t('Integrations may arm without a code'), type: 'switch'},
      // AUT-015, D25: only on a build with automations (optionsPage leaves the field out otherwise).
      {name: 'auto_less_secure', label: t('Allow less secure automations'), type: 'switch', help: t('Automations may disarm, unlock and open')},
    ]],
    [t('Alarms and sirens'), [
      {name: 'bell_timeout_s', label: t('Siren time'), type: 'number', unit: s, help: t('0: sounds until disarmed.')},
      {name: 'fire_bell_timeout', label: t('Fire and CO sirens time out too'), type: 'switch', help: t('Off: they sound until silenced.')},
      {name: 'abort_window_s', label: t('Report delay (abort window)'), type: 'number', unit: s, help: t('Disarming within this time cancels the burglary report.')},
      {name: 'cancel_window_s', label: t('Cancel window'), type: 'number', unit: s, help: t('Disarming within this time after a report sends a cancel.')},
      {name: 'swinger_trips', label: t('Alarms per zone per arming'), type: 'number', help: t('A zone that trips more often is shut down until disarmed.')},
      {name: 'cross_zone_window_s', label: t('Cross-zone window'), type: 'number', unit: s, help: t('Both partner zones must trip within this time.')},
    ]],
    [t('Fire'), [
      {name: 'fire_verify_window_s', label: t('Fire verification window'), type: 'number', unit: s},
      {name: 'smoke_reset_s', label: t('Smoke detector reset'), type: 'number', unit: s, help: t('How long smoke detector power is cut after silencing.')},
    ]],
    [t('Power and start-up'), [
      {name: 'power_up_previous_state', label: t('Restore arming after a power cut'), type: 'switch'},
      {name: 'power_up_settle_s', label: t('Settle time after start-up'), type: 'number', unit: s, help: t('Zone alarms are ignored while sensors settle.')},
      {name: 'power_up_program', label: t('Installer recovery after power-up'), type: 'switch', help: t('For a short time after power-up, installer mode opens without a code.')},
    ]],
    [t('Troubles'), [
      {name: 'trouble_beep_mask', label: t('Troubles that beep at the keypad'), type: 'bits', wide: true, options: troubleBits()},
    ]],
    [t('Keypad panic keys'), [
      {name: 'panic_0', label: t('Panic key A'), type: 'select', numeric: true, options: panic, local: true},
      {name: 'panic_1', label: t('Panic key B'), type: 'select', numeric: true, options: panic, local: true},
      {name: 'panic_2', label: t('Panic key C'), type: 'select', numeric: true, options: panic, local: true},
    ]],
    [t('Service'), [
      {name: 'walk_test_timeout_s', label: t('Walk test ends after'), type: 'number', unit: s},
      {name: 'relaxed_limits', label: t('Allow values outside SIA CP-01'), type: 'switch', help: t('For testing only. Leave off in a home.')},
    ]],
  ];
};
async function optionsPage() {
  const alarm = await loadAlarm();
  // The option is in every build's schema; without automations it decides nothing, so it isn't shown (AUT-015).
  if (!state?.capabilities?.automations) alarm.schema = {...alarm.schema, options: alarm.schema.options.filter(f => f.name !== 'auto_less_secure')};
  const values = {...alarm.options, panic_0: alarm.panic_keys[0], panic_1: alarm.panic_keys[1], panic_2: alarm.panic_keys[2]};
  const optionList = optionGroups();
  const known = optionList.flatMap(g => g[1]);
  const extra = withSchema(known, alarm.schema.options).filter(s => !known.some(k => k.name === s.name));
  const groups = [...optionList.map(([title, specs]) => [title, withSchema(specs, alarm.schema.options, false)]), ...(extra.length ? [[t('Other'), extra]] : [])];
  const specs = groups.flatMap(g => g[1]);
  const editable = isInstaller() && !state.program;
  return {
    html: pageHead(t('System options'), t('How the whole alarm behaves. The panel checks every value against its rules before applying.')) +
      installerNotice() +
      `<form id="options-form" novalidate><fieldset${editable ? '' : ' disabled'}>${groups.map(([title, list]) =>
        `<section class="card gap"><div class="group-title">${esc(title)}</div><div class="rows">${fieldRows(list, values)}</div></section>`).join('')}</fieldset>
      ${editable ? `<div class="card gap"><div class="card-foot"><span class="note">${ht('Applied together after you review them.')}</span><button class="primary">${ht('Review changes')}</button></div></div>` : ''}</form>`,
    bind: () => bindSettingsCard('options-form', specs, values, ({changes, values: changed}) => {
      const options = {}, panic = [...alarm.panic_keys];
      let panicChanged = false;
      for (const [name, value] of Object.entries(changed)) {
        if (name.startsWith('panic_')) { panic[Number(name.slice(6))] = value; panicChanged = true; } else options[name] = value;
      }
      const patch = {};
      if (Object.keys(options).length) patch.options = options;
      if (panicChanged) patch.panic_keys = panic;
      review({title: t('Apply system options'), changes, confirm: t('Apply'), run: () => applyAlarm(patch)});
    }),
  };
}

// Remote access, codes and factory reset

function accessPage() {
  const specs = [
    {name: 'web', label: t('Browser access (this Panel Manager)'), type: 'switch', local: true,
      help: t('Off closes this page for everyone. Turn it back on at the panel, under Settings > About > Installer.')},
    {name: 'developer', label: t('Developer tools'), type: 'switch', local: true, showIf: v => v.web,
      help: t('Signed firmware upload from a file, diagnostic logs and an enrolled development workstation.')},
  ];
  const values = {web: true, developer: !!state.developer};
  return {
    html: pageHead(t('Access and maintenance'), t('Who can reach the panel over the network, the two special codes, and starting over.')) +
      settingsCard({id: 'access-form', title: t('Remote access'), specs, values, button: t('Review')}) +
      `<section class="card gap"><div class="card-head"><div><h2>${ht('Installer and master codes')}</h2>
        <p>${ht('Sets the installer’s or the system master’s code without the old one, as Installer > Installer and master codes does at the panel. Everyone’s code, the installer’s too, also changes on People and codes.')}</p></div></div>
        <div class="card-foot"><button type="button" id="change-code">${ht('Change a code…')}</button></div></section>
      <section class="card gap"><div class="card-head"><div><h2>${ht('Backup')}</h2>
        <p>${ht('One file with everything needed to set this panel up again, or a replacement: zones and their sensors, people and their codes, Wi-Fi, integrations and the Z-Wave network. Encrypted with a password you choose; keep the two apart. It never holds the panel’s own identity or its event log.')}</p></div></div>
        <div class="card-foot"><button type="button" id="backup-download">${ht('Download a backup…')}</button></div></section>
      <section class="card gap"><div class="card-head"><div><h2>${ht('Restore from a backup')}</h2>
        <p>${ht('Replaces this panel’s settings with a backup’s, all or nothing, then restarts. Every partition must be disarmed. A replacement panel keeps its own identity and takes over the backup’s Z-Wave radio.')}</p></div></div>
        <div class="row"><div><label class="label" for="backup-file">${ht('Backup file')}</label>
          <span class="help">${ht('A .rabk file downloaded from a panel of this kind.')}</span></div>
          <div class="control"><input id="backup-file" type="file" accept=".rabk"></div></div>
        <div class="card-foot"><button type="button" class="danger" id="backup-restore">${ht('Restore…')}</button></div></section>
      <section class="card gap"><div class="card-head"><div><h2>${ht('Factory reset')}</h2>
        <p>${ht('Erases settings, users, Wi-Fi and remote access. The panel then needs setting up at its screen.')}</p></div></div>
        <div class="card-foot"><button type="button" class="danger" id="factory-reset">${ht('Factory reset…')}</button></div></section>`,
    bind() {
      bindSettingsCard('access-form', specs, values, ({changes}) => {
        const form = $('#access-form');
        const web = form.web.checked, developer = web && form.developer.checked;
        review({title: t('Change remote access?'), changes, confirm: t('Apply'), danger: !web, password: true,
          intro: web ? '' : t('This page stops working as soon as you apply. Only the panel’s screen can turn it back on.'),
          done: web ? t('Remote access updated.') : '', run: mePassword => api('remote', {web, developer, ...withPassword(mePassword)}),
          after: async () => {
            if (!web) {
              state = null;
              $('#main').innerHTML = pageHead(t('Browser access is off'), t('Turn it back on at the panel: Settings > About > Installer.'));
              $('#nav').replaceChildren();
              return;
            }
            await refreshStatus();
            await show();
          }});
      });
      $('#change-code').onclick = async () => {
        const min = (await api('people').catch(() => null))?.code_min || 4; // the panel's shortest allowed code
        openDialog(`<form id="code-form" novalidate><div class="dialog-head"><h2 id="dialog-title">${ht('Change a code')}</h2></div>
          <div class="dialog-body"><div class="rows">
          ${fieldRow({name: 'number', label: t('Whose code'), type: 'select', numeric: true, options: [[1, t('Installer (user 1)')], [2, t('System master (user 2)')]]}, 1)}
          ${fieldRow({name: 'new_code', label: t('New code'), type: 'secret', inputmode: 'numeric', maxLength: 10, help: t('{0} to {1} digits', min, 10)}, '')}</div>
</div>
          <p class="dialog-error" id="dialog-error" role="alert"></p>
          <div class="dialog-foot"><button type="button" id="dialog-cancel">${ht('Cancel')}</button><button class="primary">${ht('Change code')}</button></div></form>`);
        $('#dialog-cancel').onclick = closeDialog;
        $('#code-form').onsubmit = async event => {
          event.preventDefault();
          const f = event.target;
          try {
            const code = f.new_code.value;
            if (!/^\d+$/.test(code) || code.length < min || code.length > 10) throw new Error(t('A code has {0} to {1} digits.', min, 10));
            const r = await api('installer-code', {number: Number(f.number.value), new_code: f.new_code.value});
            if (r.error) throw new Error(friendly(r.error));
            closeDialog();
            toast(t('Code changed.'));
          } catch (error) { $('#dialog-error').textContent = error.message; }
        };
      };
      $('#backup-download').onclick = () => {
        openDialog(`<form id="backup-form" novalidate><div class="dialog-head"><h2 id="dialog-title">${ht('Download a backup')}</h2>
          <p>${ht('The file opens only with this password. Anyone with both can read the Wi-Fi and integration passwords in it and try to guess codes, so keep them apart.')}</p></div>
          <div class="dialog-body"><div class="rows">
          ${fieldRow({name: 'password', label: t('Backup password'), type: 'secret', maxLength: 64, help: t('8 to 64 characters. You need it to restore.')}, '')}</div>
          ${passwordBox()}</div>
          <p class="dialog-error" id="dialog-error" role="alert"></p>
          <div class="dialog-foot"><button type="button" id="dialog-cancel">${ht('Cancel')}</button><button class="primary">${ht('Download')}</button></div></form>`);
        $('#dialog-cancel').onclick = closeDialog;
        $('#backup-form').onsubmit = async event => {
          event.preventDefault();
          const f = event.target, button = f.querySelector('button.primary');
          try {
            if (!/^[\x20-\x7e]{8,64}$/.test(f.password.value)) throw new Error(friendly('backup_password'));
            const mePassword = passwordOf(f);
            button.disabled = true;
            $('#dialog-error').textContent = t('Making the backup: the panel takes a few seconds to encrypt it…');
            const response = await fetch('/api/v1/backup', {method: 'POST', credentials: 'same-origin', cache: 'no-store',
              headers: {'Content-Type': 'application/json', 'X-Panel-CSRF': csrf},
              body: JSON.stringify({password: f.password.value, ...withPassword(mePassword)}), signal: AbortSignal.timeout(90000)})
              .catch(error => { throw unreachable(error); });
            if (!response.ok) {
              throw answerError(await response.json().catch(() => ({})), response.status);
            }
            if (mePassword) passwordConfirmed();
            const blob = await response.blob();
            const name = /filename="([^"]+)"/.exec(response.headers.get('Content-Disposition') || '')?.[1] || 'panel-backup.rabk';
            const url = URL.createObjectURL(blob);
            const link = document.createElement('a');
            link.href = url;
            link.download = name;
            document.body.append(link);
            link.click();
            link.remove();
            setTimeout(() => URL.revokeObjectURL(url), 10000);
            closeDialog();
            toast(t('Backup downloaded: {0}.', name));
          } catch (error) {
            if (!askPasswordAgain(error)) $('#dialog-error').textContent = error.message;
          } finally {
            button.disabled = false;
          }
        };
      };
      $('#backup-restore').onclick = () => {
        const file = $('#backup-file').files[0];
        if (!file) return toast(t('Choose the backup file first.'), 'error');
        // The word to type stays English whatever the language, like the panel's own confirmations.
        openDialog(`<form id="restore-form" novalidate><div class="dialog-head"><h2 id="dialog-title">${ht('Restore {0}?', file.name)}</h2>
          <p>${ht('Everything the backup holds replaces this panel’s: zones, people and codes, Wi-Fi, integrations, Z-Wave. The panel checks the whole file first and changes nothing if any of it is wrong, then restarts.')}</p></div>
          <div class="dialog-body"><div class="rows">
          ${fieldRow({name: 'password', label: t('Backup password'), type: 'secret', maxLength: 64}, '')}</div>
          <div class="code-field"><label for="restore-confirm">${ht('Type {0} to confirm', nt('RESTORE'))}</label><input id="restore-confirm" name="confirm" autocomplete="off" spellcheck="false"></div>
          ${passwordBox()}</div>
          <p class="dialog-error" id="dialog-error" role="alert"></p>
          <div class="dialog-foot"><button type="button" id="dialog-cancel">${ht('Cancel')}</button><button class="danger solid">${ht('Restore')}</button></div></form>`);
        $('#dialog-cancel').onclick = closeDialog;
        $('#restore-form').onsubmit = async event => {
          event.preventDefault();
          const f = event.target, button = f.querySelector('button.danger');
          try {
            if (f.confirm.value.trim().toUpperCase() !== 'RESTORE') throw new Error(t('Type {0} to confirm.', nt('RESTORE')));
            const mePassword = passwordOf(f);
            const hexOf = text => Array.from(new TextEncoder().encode(text), b => b.toString(16).padStart(2, '0')).join('');
            const hex = hexOf(f.password.value);
            button.disabled = true;
            $('#dialog-error').textContent = t('Checking the backup: the panel takes a few seconds to open it…');
            const response = await fetch('/api/v1/backup', {method: 'PUT', credentials: 'same-origin', cache: 'no-store',
              headers: {'Content-Type': 'application/octet-stream', 'X-Panel-CSRF': csrf, 'X-Backup-Password': hex, ...(mePassword ? {'X-Panel-Password': hexOf(mePassword)} : {})},
              body: file, signal: AbortSignal.timeout(90000)}).catch(error => { throw unreachable(error); });
            const answer = await response.json().catch(() => ({}));
            if (!response.ok) throw answerError(answer, response.status);
            closeDialog();
            state = null;
            $('#nav').replaceChildren();
            const what = t('{0}, {1}', tn('{0} zone', '{0} zones', answer.zones), tn('{0} person', '{0} people', answer.users));
            $('#main').innerHTML = pageHead(t('The panel is restarting'), answer.error ? friendly(answer.error)
              : answer.created ? t('Restored the backup made {0} (firmware {1}): {2}. Sign in again in a minute.',
                new Date(answer.created * 1000).toLocaleDateString(wordsLocale()), answer.firmware, what)
              : t('Restored the backup (firmware {0}): {1}. Sign in again in a minute.', answer.firmware, what));
          } catch (error) {
            if (!askPasswordAgain(error)) $('#dialog-error').textContent = error.message;
          } finally {
            button.disabled = false;
          }
        };
      };
      $('#factory-reset').onclick = () => {
        // The panel compares the words to type: they stay English whatever the language.
        openDialog(`<form id="reset-form" novalidate><div class="dialog-head"><h2 id="dialog-title">${ht('Factory reset?')}</h2>
          <p>${ht('Everything is erased: settings, users and codes, Wi-Fi and remote access. The panel restarts and must be set up at its screen.')}</p></div>
          <div class="dialog-body"><div class="rows">${fieldRow({name: 'erase_log', label: t('Erase the event log too'), type: 'switch'}, false)}</div>
          <div class="code-field"><label for="reset-confirm">${ht('Type {0} to confirm', nt('ERASE THIS PANEL'))}</label><input id="reset-confirm" name="confirm" autocomplete="off" spellcheck="false"></div>
          ${passwordBox()}</div>
          <p class="dialog-error" id="dialog-error" role="alert"></p>
          <div class="dialog-foot"><button type="button" id="dialog-cancel">${ht('Cancel')}</button><button class="danger solid">${ht('Erase the panel')}</button></div></form>`);
        $('#dialog-cancel').onclick = closeDialog;
        $('#reset-form').onsubmit = async event => {
          event.preventDefault();
          const f = event.target;
          try {
            const r = await api('factory-reset', {erase_log: f.erase_log.checked, confirm: f.confirm.value, ...withPassword(passwordOf(f))});
            if (r.error) throw new Error(friendly(r.error));
            closeDialog();
            state = null;
            $('#nav').replaceChildren();
            $('#main').innerHTML = pageHead(t('The panel is resetting'), t('Set it up again at its screen.'));
          } catch (error) { if (!askPasswordAgain(error)) $('#dialog-error').textContent = error.message; }
        };
      };
    },
  };
}

// ---------------------------------------------------------------------------------------
// System: health, updates, developer

async function healthPage() {
  await refreshStatus();
  const h = state.health;
  const internalKind = h.internal_kb < 48 ? 'bad' : h.internal_kb < 96 ? 'warn' : 'good';
  const displayWords = {frames: t('Frames drawn'), late: t('Late frames'), guard_trips: t('Flicker-guard trips'),
    forced_off: t('Backlight forced off (dark build)'), backlight: t('Backlight (%)')};
  const yes = v => v ? t('Yes') : t('No');
  const display = h.display && typeof h.display === 'object'
    ? Object.entries(h.display).map(([k, v]) => [displayWords[k] || titleCase(k),
      typeof v === 'boolean' ? yes(v) : typeof v === 'number' ? v.toLocaleString(wordsLocale()) : String(v)]) : [];
  const power = h.power;
  const battery = {present: t('Present'), absent: t('Not fitted')}[power?.battery] || t('Unknown');
  return {
    html: pageHead(t('System health'), t('Live readings from the panel. Refresh the page for new values.'), `<button type="button" id="refresh">${ht('Refresh')}</button>`) +
      `<div class="grid three">
      <section class="card tile"><span class="tile-label">${icon('heart')}${ht('Internal memory free')}</span><span class="tile-value">${ht('{0} KB', h.internal_kb)}</span>
        <span class="tile-detail">${tag(internalKind === 'good' ? t('Healthy') : internalKind === 'warn' ? t('Getting low') : t('Low'), internalKind)}${h.internal_largest_kb !== undefined ? ' ' + ht('Largest block {0} KB', h.internal_largest_kb) : ''}</span></section>
      <section class="card tile"><span class="tile-label">${icon('layers')}${ht('PSRAM free')}</span><span class="tile-value">${fmtBytes(h.psram_kb * 1024)}</span>
        <span class="tile-detail">${ht('Pictures, the web service and the screen use this.')}</span></section>
      <section class="card tile"><span class="tile-label">${icon('clock')}${ht('Running for')}</span><span class="tile-value">${fmtDuration(h.uptime_s)}</span>
        <span class="tile-detail">${h.stale ? esc(tn('{0} health source not reporting', '{0} health sources not reporting', h.stale)) : ht('Every health source is reporting')}</span></section></div>
      <div class="grid two gap">
      <section class="card"><div class="card-head"><h2>${ht('Panel')}</h2></div>${kv([[t('Board'), state.board], [t('Firmware'), state.firmware], [t('Chip'), h.chip],
        [nt('ESP-IDF'), h.idf], [t('Panel ID'), state.panel_id], [t('Web service stack unused'), h.web_stack_free !== undefined ? tn('{0} byte', '{0} bytes', h.web_stack_free) : '']])}</section>
      <section class="card"><div class="card-head"><h2>${ht('Display')}</h2></div>${display.length ? kv(display) : `<p class="empty">${ht('Not reported.')}</p>`}</section>
      <section class="card"><div class="card-head"><h2>${ht('Network')}</h2></div>${kv([[t('Wi-Fi'), state.wifi.ssid], [t('Signal'), t('{0} dBm ({1})', state.wifi.rssi, signalWords(state.wifi.rssi))],
        [t('IP address'), state.wifi.ip], [t('Clock'), state.wifi.time_synced ? t('Synchronized') : t('Not set')]])}</section>
      <section class="card"><div class="card-head"><h2>${ht('Power')}</h2></div>${power ? kv([[t('Battery'), battery],
        [t('Charge'), power.battery === 'present' ? t('{0}%', power.percent) : ''], [t('Charging'), yes(power.charging)], [t('Low'), yes(power.low)]])
        : `<p class="empty">${ht('This board does not report power.')}</p>`}</section></div>`,
    bind: () => { $('#refresh').onclick = show; },
  };
}

const fwsChannelWords = () => ({stable: [t('Stable'), t('Tested the longest. Right for most homes')],
  beta: [t('Beta'), t('Newer releases, tried by a few people first')], alpha: [t('Alpha'), t('The newest releases, still being tested')],
  dev: [t('Dev'), t('Special builds for developers')]});
const fwsRefusals = () => ({
  unreachable: d => [t('{0} can’t be reached', d), t('Check the domain and that the panel is online.')],
  not_a_server: d => [t('{0} isn’t an update server', d), t('It didn’t answer like one. Check the domain.')],
  no_firmware: d => [t('{0} has no firmware for this panel', d), t('It doesn’t serve this model of panel.')],
});
const FWS_CHANNELS = ['stable', 'beta', 'alpha', 'dev'];
function fwsDomain(text) {
  const d = String(text || '').trim().toLowerCase().replace(/^https?:\/\//, '').replace(/\/+$/, '');
  return /^(?=.{4,253}$)([a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z]{2,63}$/.test(d) ? d : '';
}
// A panel that trusts no extra key may not name one: then the key this version is signed with.
const fwsKeyHtml = key => !key ? ht('The key this version is signed with')
  : `<span title="${esc(key.sha256)}">${esc(key.name || t('The key this version is signed with'))} · <span class="fws-key">${esc(key.fingerprint)}</span></span>`;

// ---- BL-188 (OTA-026..OTA-030): an update's progress, Stop the update, and the restart followed --------------------------
const UPD_RUNNING = ['downloading', 'verifying', 'waiting', 'installing', 'stopping', 'rolling_back', 'wifi_module', 'file'];
const updRunning = d => UPD_RUNNING.includes(d.stage) || !!d.confirming;
// Lost while it restarts, during its self-test, or at a step where it can't be restarting.
function updLostKind(d) {
  if (d.confirming || d.stage === 'rolling_back') return 'self-test';
  const due = d.stage === 'installing' || d.stage === 'verifying' || (d.stage === 'waiting' && !d.reason) || (d.stage === 'downloading' && d.percent >= 100);
  return due ? 'restart' : 'other';
}
// What holds the update, and what ends the wait. The panel's own reasons are marked here and read with t().
function updHold(reason) {
  const holds = {
    [tx('Installer mode is on')]: [t('It installs once installer mode is off'), t('Leave installer mode and the panel restarts to install it.')],
    [tx('The system is armed')]: [t('It installs once everything is disarmed'), t('Disarm everything and the panel restarts to install it.')],
    [tx('An alarm is sounding')]: [t('It installs once the alarm is silenced'), t('Silence the alarm and the panel restarts to install it.')],
    [tx('A walk test is running')]: [t('It installs once the walk test ends'), t('End the walk test and the panel restarts to install it.')],
    [tx('Trying again shortly')]: [t('It installs within 30 seconds'), t('The system got busy as it was about to restart. It tries again within 30 seconds.')],
  };
  return holds[reason] || [t('It installs once everything is disarmed'), t('{0}.', t(reason))];
}
// A time today, or the day.
function updWhen(at) {
  const when = new Date(at * 1000), today = when.toDateString() === new Date().toDateString();
  return [today, today ? when.toLocaleTimeString(wordsLocale(), {hour: 'numeric', minute: '2-digit'})
    : when.toLocaleDateString(wordsLocale(), {month: 'short', day: 'numeric'})];
}
const updStepWords = () => [t('Download'), t('Check the download'), t('Wait until everything is disarmed'), t('Restart with the new version'), t('Self-test')];
function updSteps(now) {
  return `<ol class="upd-steps" aria-label="${ht('Steps')}">${updStepWords().map((w, i) => {
    const kind = i < now ? 'done' : i === now ? 'now' : 'next';
    const said = kind === 'done' ? t('Done') : kind === 'now' ? t('Now') : t('Next');
    return `<li class="${kind}"${kind === 'now' ? ' aria-current="step"' : ''}><span class="dot">${kind === 'done' ? icon('check') : ''}</span>
      <span class="upd-hidden">${esc(said)}: </span>${esc(w)}</li>`;
  }).join('')}</ol>`;
}
// The bar is the exit time's timebar: steps, no animation.
const updBar = d => `<div class="timebar upd-bar"><progress max="100" value="${Math.max(0, d.percent)}" aria-label="${ht('Download')}"></progress></div>`;
const updBytes = d => t('{0} of {1} · {2} %', fmtNumber(d.bytes / 1048576, 1), fmtBytes(d.size), d.percent);
// The bar and the size in a card's html: all that changes within a step.
function updLive(html) {
  const value = /<progress[^>]* value="(\d+)"/.exec(html), bytes = /id="upd-bytes">([^<]*)</.exec(html);
  return value && {value: Number(value[1]), bytes: bytes && bytes[1].replace(/&(amp|lt|gt|quot|#39);/g, (m, c) => ({amp: '&', lt: '<', gt: '>', quot: '"', '#39': "'"})[c])};
}

// The status card, every state of an update. lost: '' | 'restart' | 'self-test' | 'other' (the panel isn't answering).
function updCard(d, lost = '') {
  const can = state?.access?.system, last = d.last, frozen = lost === 'other';
  const locked = !!state?.locked && !updRunning(d);
  const card = (kind, title, lines, steps = '', buttons = '', bar = '') => {
    if (frozen) [lines, buttons] = [[t('The panel isn’t answering. The update goes on without this page; this page keeps trying.')], ''];
    return `<section class="card status-card ${kind}" id="upd-card">
    <span class="muted">${ht('Installed: version {0}', d.running)}</span><span class="state" id="upd-title" tabindex="-1">${esc(title)}</span>${bar}
    ${lines.filter(Boolean).map((l, i) => `<p${i === 0 && bar && !frozen ? ' id="upd-bytes"' : ''}>${esc(l)}</p>`).join('')}${steps}${buttons ? `<div class="actions gap">${buttons}</div>` : ''}</section>`;
  };
  const stop = can ? `<button type="button" class="danger" data-update="stop">${ht('Stop the update')}</button>` : '';
  const check = can ? `<button type="button" data-update="check"${d.phase === 1 ? ' disabled' : ''}>${ht('Check now')}</button>` : '';
  const offer = !!d.latest && d.latest !== d.running;
  const install = can && offer ? `<button type="button" class="primary" data-update="install"${locked ? ' disabled' : ''}>${ht('Install version {0}', d.latest)}</button>` : '';
  const later = install && locked ? t('You can install it once everything is disarmed.') : '';
  const hold = d.reason ? updHold(d.reason) : null;
  const until = hold ? t('{0}. {1}; until then you can stop the update.', t(d.reason), hold[0]) : '';
  if (lost === 'restart') return card('info', t('Restarting with version {0}', d.target),
    [t('The panel isn’t answering. It’s probably restarting to install version {0}; this page reconnects by itself, and you’ll sign in again.', d.target)], updSteps(3));
  if (lost === 'self-test') return card('warn', d.stage === 'rolling_back' ? t('Going back to version {0}', d.previous) : t('The panel isn’t answering'),
    [t('If its self-test failed, the panel is restarting with version {0}. This page reconnects by itself; you’ll sign in again.', d.previous)], updSteps(4));
  switch (d.stage) {
    case 'downloading': return card('info', t('Downloading version {0}', d.target),
      [d.bytes > 0 ? updBytes(d) : t('Starting the download.'), until || t('The alarm keeps working while it downloads. You can stop the update until the panel restarts.')],
      updSteps(0), stop, updBar(d));
    case 'verifying': return card('info', t('Checking the download'),
      [t('Its signature and checksum: a few seconds.'), until || t('The alarm keeps working. You can stop the update until the panel restarts.')], updSteps(1), stop);
    case 'waiting': return card('warn', t('Waiting to install version {0}', d.target),
      [hold ? (d.reason === 'Trying again shortly' ? hold[1] : t('{0}. {1}', t(d.reason), hold[1])) : t('It installs in a moment.'),
        t('It takes about a minute. You can stop the update until the panel restarts.')], updSteps(2), stop);
    case 'installing': return card('info', t('Installing version {0}', d.target),
      [t('The panel restarts with it now. This page waits and reconnects by itself: about a minute.')], updSteps(3));
    case 'stopping': return card('', t('Stopping the update'),
      [t('The panel keeps version {0}. It stops within a few seconds, or up to a minute on a slow connection.', d.running)], updSteps(d.percent >= 100 ? 1 : 0));
    case 'rolling_back': return card('warn', t('Going back to version {0}', d.previous),
      [t('Version {0} failed its self-test. The panel restarts with version {1}; it answers again in about a minute.', d.running, d.previous)], updSteps(4));
    case 'wifi_module': return card('info', t('Updating the Wi-Fi module'),
      [d.percent >= 0 ? t('{0} %', d.percent) : '', t('The panel restarts when it’s done, once everything is disarmed. This update can’t be stopped.')], '', '', updBar(d));
    case 'file': return card('info', t('Installing a file from developer tools'),
      [d.percent >= 0 ? t('{0} %', d.percent) : '', t('Whoever sent it follows it there. The panel restarts with the new version.')], '', '', updBar(d));
  }
  if (d.confirming) return card('info', t('Self-test of version {0}', d.running),
    [t('Usually a minute, at most three. If it fails, the panel restarts with version {0} by itself, armed or not.', d.previous),
      d.reason ? t('{0}. The self-test goes on; the alarm keeps working.', t(d.reason)) : ''], updSteps(4));
  if (d.interrupted) return card('warn', t('Update didn’t finish'),
    [t('The panel restarted before installing version {0}. Nothing was changed.', d.interrupted), later], '', install + check);
  if (d.phase === 1) return card('', t('Checking for updates'), [t('Asking the update server.')], '', check);
  if (d.phase === 5 && d.failed === 'check') return card('bad', t('Couldn’t check for updates'), [t('{0}.', t(d.reason)), later], '', (d.available ? install : '') + check);
  if (last?.result === 'stopped') {
    const [today, when] = last.at ? updWhen(last.at) : [true, ''];
    return card('', t('Update stopped'), [last.by && when ? (today ? t('{0} stopped it at {1}. Nothing was changed.', last.by, when)
      : t('{0} stopped it on {1}. Nothing was changed.', last.by, when)) : t('Nothing was changed.'), later], '', install + check);
  }
  if (last?.result === 'failed') return card('bad', t('Update failed'), [t('{0}. Nothing was changed.', t(last.reason)), later], '', install + check);
  if (last?.result === 'rolled_back') return card('warn', t('Version {0} failed its self-test', last.version),
    [t('The panel went back to version {0} by itself.', last.from), later], '', install + check);
  if (last?.result === 'updated') {
    const [today, when] = last.at ? updWhen(last.at) : [true, ''];
    return card('good', t('Updated to version {0}', last.version), [!when ? t('It passed its self-test.')
      : today ? t('It passed its self-test at {0}.', when) : t('It passed its self-test on {0}.', when)], '', check);
  }
  const offered = d.available ? (d.critical ? t('Version {0} is available · an important fix', d.latest) : t('Version {0} is available', d.latest)) : d.latest ? t('Up to date') : t('Not checked yet');
  return card(d.available ? 'info' : d.latest ? 'good' : '', offered, [d.available ? later : ''], '', (d.available ? install : '') + check);
}
// What makes the card a new step (replaced, said once) rather than the same one (bar and size in place).
const updKey = (d, lost) => `${lost}:${d.stage}:${d.confirming}:${d.last?.result || ''}:${d.reason}:${d.phase}:${!!state?.locked}:${!!d.interrupted}`;

// While an update runs the page's armed notice gives way only to a card that names what holds it.
function updNotice(d) {
  const box = $$('#main > .notice').find(n => n.textContent.includes(t('The panel is armed or an alarm is sounding')));
  if (!box || !updRunning(d)) return;
  if (d.reason) return box.remove();
  const p = box.querySelector('p');
  if (p) p.textContent = t('Other changes wait until everything is disarmed and quiet. You can still stop the update.');
}
// The question before a stop. Keep updating is the safe default.
function updStopDialog(d, after) {
  openDialog(`<div class="dialog-head"><h2 id="dialog-title">${ht('Stop the update?')}</h2>
    <p>${ht('The panel keeps version {0}. You can install {1} again later; it downloads it again.', d.running, d.target)}</p></div>
    <p class="dialog-error" id="dialog-error" role="alert"></p>
    <div class="dialog-foot"><button type="button" id="dialog-cancel" autofocus>${ht('Keep updating')}</button>
      <button type="button" class="danger solid" id="upd-stop">${ht('Stop it')}</button></div>`);
  $('#dialog-cancel').onclick = closeDialog;
  $('#dialog-cancel').focus();
  $('#upd-stop').onclick = async () => {
    try {
      await api('updates', {action: 'cancel'});
      closeDialog();
      await after();
    } catch (error) {
      if (error.code === 'no_update') { closeDialog(); toast(t('It isn’t running any more.')); return after(); }
      $('#dialog-error').textContent = error.status ? error.message : t('The panel isn’t answering. It may be restarting to install the update.');
    }
  };
}

function keypadUpdates(rows) {
  if (!rows.length) return '';
  const words = [t('Waiting for status'), t('Up to date'), t('Downloading'), t('Receiving from this panel'), t('Ready · restarts with this panel'), t('Restarting'), t('Not updated yet · tries again by itself'), t('Newer than this panel')];
  return `<section class="section"><div class="section-head"><h2>${ht('Keypad updates')}</h2></div><div class="card"><ul class="list">${rows.map(k => `<li><div class="main"><strong>${esc(k.name)}</strong><span>${esc(k.firmware || t('Waiting for status'))}</span></div><span>${esc(words[k.update_state] || words[0])}${[2, 3].includes(k.update_state) ? ' · ' + esc(k.update_percent) + ' %' : ''}</span></li>`).join('')}</ul></div></section>`;
}
async function updatesPage() {
  const first = await api('updates');
  const keypadRows = isInstaller() ? (await api('keypads')).keypads : [];
  // Back from a restart this page lost the panel in: did the version it was installing come up?
  const saved = restartSaved();
  const interrupted = saved?.version && !updRunning(first) && !first.last && first.running !== saved.version ? saved.version : '';
  if (saved && !updRunning(first)) sessionStorage.removeItem('panel-restart');
  const decorate = x => interrupted && !updRunning(x) && !x.last ? {...x, interrupted} : x;
  let d = decorate(first);
  const s = d.server, installer = isInstaller(), held = updRunning(d);
  const channels = fwsChannelWords();
  const channelSpecs = [{name: 'channel', label: t('Channel'), type: 'select', local: true,
    options: FWS_CHANNELS.map(c => [c, channels[c][0], channels[c][1]]), help: t('Which releases this panel is offered. A channel also gets the releases of the steadier ones; Dev stands apart.')}];
  const channelCard = settingsCard({id: 'channel-form', title: t('Update channel'), specs: channelSpecs, values: {channel: d.channel}, button: t('Save')});
  return {
    html: pageHead(t('Software updates'), t('Every update is signed. The panel checks the signature and installs only while disarmed; if the new version fails its self-test it goes back by itself.')) +
      (d.withdrawn ? notice('warn', t('Version {0} was withdrawn', d.running), d.available ? t('Its publisher took it back. Install version {0} when you can.', d.latest) : t('Its publisher took it back.')) : '') +
      `<div id="keypad-updates">${keypadUpdates(keypadRows)}</div>` + updCard(d) + `<p class="upd-hidden" role="status" id="upd-said"></p>
      ${s ? `<section class="section"><div class="card"><div class="card-head"><div><h2>${ht('Where updates come from')}</h2>
          <p>${ht('Every update must be signed with this key or with the key of the version it runs. The panel refuses anything else, wherever it comes from.')}</p></div>
          ${tag(s.official ? t('RoboAlarms') : t('Not the RoboAlarms server'), s.official ? 'good' : 'warn')}</div>
        <dl class="kv"><dt>${ht('Update server')}</dt><dd>${esc(s.domain)}</dd>
          <dt>${ht('Signing key')}</dt><dd>${fwsKeyHtml(s.key)}</dd></dl>
        <div class="card-foot">${held ? `<span class="fws-held">${icon('alert')}${ht('Not while an update runs.')}</span>`
          : d.factory_code ? `<span class="fws-held">${icon('alert')}${ht('Change the installer code first: the factory one is public.')}</span>
            <button type="button" data-go="access">${ht('Change a code…')}</button>` : ''}
          ${!s.official ? `<button type="button" id="fws-back"${held ? ' disabled' : ''}>${ht('Go back to roboalarms.com')}</button>` : ''}
          <button type="button" id="fws-change"${d.factory_code || held ? ' disabled' : ''}>${ht('Change the update server…')}</button></div></div></section>
        <section class="section">${held ? `<fieldset class="fws-hold" disabled>${channelCard}</fieldset>` : channelCard}</section>` : ''}
      ${state.developer && installer ? `<section class="section"><form class="card" id="upload-form"><div class="card-head"><div><h2>${ht('Install a signed file')}</h2>
        <p>${ht('Developer tools are on. Choose a signed .bin built for this board and profile. The panel must be disarmed.')}</p></div></div>
        <div class="rows"><div class="row"><div><label class="label" for="image">${ht('Firmware file')}</label></div><div class="control"><input id="image" name="image" type="file" accept=".bin" required></div></div>
        ${fieldRow({name: 'allow_older', label: t('Allow the same or an older version'), type: 'switch'}, false)}</div>
        <div class="card-foot"><span class="note" id="upload-progress" role="status"></span><button class="primary">${ht('Upload and install')}</button></div></form></section>` : ''}`,
    bind() {
      updNotice(d);
      let lost = '', shownHtml = updCard(d), shownKey = updKey(d, lost), misses = 0, firstMiss = 0, n = 0;
      const wasRunning = updRunning(d);
      // Within a step only the bar and the size change, in place; a new step replaces the card, keeps the
      // focus on the same button (or the title) and is said once.
      const redraw = () => {
        const old = $('#upd-card'), html = updCard(d, lost), key = updKey(d, lost);
        if (!old || html === shownHtml) return;
        shownHtml = html;
        const bar = $('#upd-card progress'), live = updLive(html);
        if (key === shownKey && bar && live) {
          bar.value = live.value;
          const bytes = $('#upd-bytes');
          if (bytes && live.bytes != null) bytes.textContent = live.bytes;
          return;
        }
        const at = document.activeElement?.closest?.('#upd-card') ? document.activeElement.dataset.update || 'title' : '';
        old.outerHTML = html;
        bindCard();
        if (at) ($(`#upd-card [data-update="${at}"]`) || $('#upd-title'))?.focus();
        if (key !== shownKey && $('#upd-said')) $('#upd-said').textContent = $('#upd-title')?.textContent || '';
        shownKey = key;
        connectionShows(lost);
      };
      const bindCard = () => $$('[data-update]').forEach(b => b.onclick = async () => {
        const action = b.dataset.update;
        if (action === 'stop') return updStopDialog(d, async () => { d = decorate(await api('updates')); redraw(); });
        if (action === 'install') return review({title: t('Install version {0}?', d.latest),
          intro: t('The panel downloads it now and restarts once everything is disarmed. It keeps protecting while it downloads and waits, and you can stop the update until it restarts.'),
          confirm: t('Install'), done: '', after: show,
          run: () => api('updates', {action: 'install'}).catch(error => {
            if (error.code === 'locked') throw new Error(t('The panel is armed or an alarm is sounding, so nothing was installed. Install it once everything is disarmed.'));
            throw error;
          })});
        b.disabled = true;
        try { await api('updates', {action}); } catch (error) { toast(error.message, 'error'); }
        show();
      });
      // While a run goes: once a second; otherwise every 3 s. A run that begins or ends draws the page again.
      updFollow.tick = async () => {
        if ($('#dialog')?.open) return;
        if ((!updRunning(d) || lost) && ++n % 3) return;
        try {
          const was = d;
          d = decorate(await api('updates', undefined, {signal: AbortSignal.timeout(4000)}));
          if (!updRunning(d) && !lost) { // idle: the lock follows the panel too
            const s = await api('status', undefined, {signal: AbortSignal.timeout(4000)});
            if (state) state.locked = !!s.locked;
          }
          if (lost) sessionStorage.removeItem('panel-restart'); // it answered without a sign-in: it hadn't restarted
          misses = 0;
          lost = '';
          if (updRunning(d) !== wasRunning || (was.running !== d.running)) return show();
          redraw();
          if (installer) { const html = keypadUpdates((await api('keypads')).keypads); const box = $('#keypad-updates'); if (box && box.innerHTML !== html) box.innerHTML = html; }
        } catch (error) {
          if (error.status || !updRunning(d) || lost) return; // a 401 was said by api(), with the note when this page lost the panel
          if (!misses++) { firstMiss = Date.now(); return; }
          if (Date.now() - firstMiss < 2000) return;
          lost = updLostKind(d);
          restartLost(lost, d.target);
          redraw();
        }
      };
      bindCard();
      poll(updFollow.tick, 1000);
      if (s) {
        bindSettingsCard('channel-form', channelSpecs, {channel: d.channel}, ({changes, values}) =>
          review({changes, intro: t('The panel checks for updates on the new channel at once.'), run: () => api('updates', {action: 'channel', channel: values.channel})}));
        const change = $('#fws-change');
        if (change) change.onclick = () => fwsTypeDialog(s);
        const back = $('#fws-back');
        if (back) back.onclick = () => fwsRunCheck(s, 'roboalarms.com');
      }
      const form = $('#upload-form');
      if (form) form.onsubmit = event => {
        event.preventDefault();
        const file = form.image.files[0], older = form.allow_older.checked;
        if (!file) return toast(t('Choose a file.'), 'error');
        review({title: t('Install this file?'), changes: [{label: t('File'), to: file.name}, {label: t('Size'), to: fmtBytes(file.size)},
          {label: t('Same or older version'), to: older ? t('Allowed') : t('Not allowed')}],
          intro: t('The panel checks the signature before anything changes, then restarts to install it.'),
          confirm: t('Upload and install'), done: '', after: () => {},
          run: async () => {
            closeDialog();
            await uploadFirmware(file, older, percent => { const p = $('#upload-progress'); if (p) p.textContent = t('Uploading… {0}%', percent); })
              .catch(error => toast(error.message, 'error'));
          }});
      };
    },
  };
}
// The page's follow-up, reachable to the tests.
const updFollow = {tick: null};

// Step 1: the domain. Nothing is kept until the last step says so.
function fwsTypeDialog(s, typed = '', error = '') {
  openDialog(`<form id="fws-form" novalidate><div class="dialog-head"><h2 id="dialog-title">${ht('Change the update server')}</h2>
    <p>${ht('The domain only, like roboalarms.com. The panel checks it first; nothing changes until you confirm.')}</p></div>
    <div class="dialog-body"><div class="rows"><div class="row"><div><label class="label" for="fws-domain">${ht('Update server')}</label>
      <span class="help">${ht('Now {0}', s.domain)}</span></div>
      <div class="control"><input id="fws-domain" name="domain" type="text" inputmode="url" autocapitalize="none" spellcheck="false" maxlength="253" value="${esc(typed)}" placeholder="example.net"></div></div></div></div>
    <p class="dialog-error" id="dialog-error" role="alert">${esc(error)}</p>
    <div class="dialog-foot"><button type="button" id="dialog-cancel">${ht('Cancel')}</button><button type="submit" class="primary">${ht('Check')}</button></div></form>`);
  $('#dialog-cancel').onclick = closeDialog;
  $('#fws-form').onsubmit = event => {
    event.preventDefault();
    const domain = fwsDomain(event.target.domain.value);
    if (!event.target.domain.value.trim()) return void ($('#dialog-error').textContent = t('Type a domain, like roboalarms.com.'));
    if (!domain) return void ($('#dialog-error').textContent = t('That isn’t a domain. Type one like example.net.'));
    if (domain === s.domain) return void ($('#dialog-error').textContent = t('Updates come from {0} already.', domain));
    fwsRunCheck(s, domain);
  };
}

// Step 2: the panel asks the server for its signing key and for firmware for this panel.
async function fwsRunCheck(s, domain) {
  openDialog(`<div class="dialog-head"><h2 id="dialog-title">${ht('Checking {0}', domain)}</h2>
    <p>${ht('Asking for its signing key and for firmware for this panel.')}</p></div>
    <div class="dialog-foot"><button type="button" id="dialog-cancel">${ht('Cancel')}</button></div>`);
  let cancelled = false;
  $('#dialog-cancel').onclick = () => { cancelled = true; closeDialog(); };
  try {
    await api('updates', {action: 'server_check', domain});
    let c = null;
    for (let i = 0; i < 40 && !cancelled; i++) {
      await new Promise(r => setTimeout(r, 500));
      c = (await api('updates')).check;
      if (c?.domain === domain && c.state === 'done') break;
    }
    if (cancelled || !$('#dialog').open) return;
    if (!c || c.state !== 'done') return fwsTypeDialog(s, domain, t('The panel didn’t finish the check. Try again.'));
    if (c.result !== 'ok') {
      const [title, line] = (fwsRefusals()[c.result] || fwsRefusals().unreachable)(domain);
      return fwsTypeDialog(s, domain, `${title}. ${line}`);
    }
    if (c.trusted) {
      return review({title: t('Use {0} for updates?', domain), intro: t('The panel trusts its signing key already.'),
        changes: [{label: t('Update server'), from: s.domain, to: domain}, {label: t('Signing key'), to: `${c.key.name || t('The key this version is signed with')} · ${c.key.fingerprint}`}],
        confirm: t('Use this server'), done: '', run: () => api('updates', {action: 'server', domain, key_sha256: c.key.sha256})});
    }
    fwsWarnDialog(domain, c);
  } catch (error) {
    if ($('#dialog').open) fwsTypeDialog(s, domain, error.message);
  }
}

// Step 3: a key the panel doesn't trust yet. The warning, the key to compare, a tick and the password.
function fwsWarnDialog(domain, c) {
  openDialog(`<form id="fws-warn" novalidate><div class="dialog-head"><h2 id="dialog-title">${ht('Use {0} for updates?', domain)}</h2>
    <p>${ht('{0} answered and has firmware for this panel.', domain)}</p></div>
    <div class="dialog-body">
      ${domain === 'roboalarms.com' ? notice('info', t('The RoboAlarms update server'), t('The version this panel runs came from another project, so it doesn’t trust RoboAlarms’ key yet.'))
        : notice('warn', t('Not the RoboAlarms update server'), t('Whoever runs this server can install firmware that changes everything this panel does, its alarms too. Continue only if you trust them.'))}
      <dl class="kv"><dt>${ht('Its signing key')}</dt><dd>${fwsKeyHtml(c.key)}</dd></dl>
      <p class="help">${ht('Check that this matches the key its owner publishes. From now on the panel also accepts updates signed with it.')}</p>
      <label class="fws-check"><input type="checkbox" name="trust" required><span>${ht('I trust {0} with this panel’s firmware', domain)}</span></label>
      ${passwordDue() ? passwordField() : ''}</div>
    <p class="dialog-error" id="dialog-error" role="alert"></p>
    <div class="dialog-foot"><button type="button" id="dialog-cancel">${ht('Cancel')}</button>
      <button type="submit" class="danger solid">${ht('Use this server')}</button></div></form>`);
  $('#dialog-cancel').onclick = closeDialog;
  $('#fws-warn').onsubmit = async event => {
    event.preventDefault();
    const f = event.target, input = dialogPassword();
    if (!f.trust.checked) return void ($('#dialog-error').textContent = t('Tick the box to say you trust this server.'));
    if (input && !input.value) { $('#dialog-error').textContent = t('Enter your password.'); return input.focus(); }
    const mePassword = input ? input.value : '';
    try {
      await api('updates', {action: 'server', domain, key_sha256: c.key.sha256, ...withPassword(mePassword)});
      if (mePassword) passwordConfirmed();
      closeDialog();
      await show();
    } catch (error) {
      if (!askPasswordAgain(error)) $('#dialog-error').textContent = error.code === 'factory_code'
        ? t('Change the installer code first: the factory one is public.') : error.message;
    }
  };
}

function developerPage() {
  return {
    html: pageHead(t('Developer'), t('Tools for building and testing firmware on this panel.')) +
      `<section class="card">${kv([[t('Panel ID'), state.panel_id], [t('Address'), state.wifi.ip], [t('Board'), state.board],
        [t('Development service'), t('Port 6055, mutual TLS, for an enrolled workstation')]])}
        <div class="card-foot"><button type="button" data-go="updates">${ht('Install a signed file')}</button>
        <button type="button" id="enroll">${ht('Enroll a workstation…')}</button></div></section>
      <section class="section"><div class="card"><div class="card-head"><div><h2>${ht('Development log')}</h2>
        <p>${ht('Service and health events. Credentials and raw console output are never included.')}</p></div><button type="button" id="load-log">${ht('Load')}</button></div>
        <pre class="log mono" id="log" hidden></pre></div></section>`,
    bind() {
      $('#load-log').onclick = async () => {
        try { const d = await api('developer/logs'); $('#log').hidden = false; $('#log').textContent = d.text || t('(empty)'); }
        catch (error) { toast(error.message, 'error'); }
      };
      $('#enroll').onclick = () => {
        openDialog(`<form id="enroll-form"><div class="dialog-head"><h2 id="dialog-title">${ht('Enroll a workstation')}</h2>
          <p>${ht('Paste the public certificate made by panelctl on the workstation. Never paste a private key. This replaces the enrolled workstation.')}</p></div>
          <div class="dialog-body"><textarea name="certificate" rows="9" maxlength="2048" required spellcheck="false" placeholder="-----BEGIN CERTIFICATE-----"></textarea>
          ${passwordBox()}</div>
          <p class="dialog-error" id="dialog-error" role="alert"></p>
          <div class="dialog-foot"><button type="button" id="dialog-cancel">${ht('Cancel')}</button><button class="primary">${ht('Enroll')}</button></div></form>`);
        $('#dialog-cancel').onclick = closeDialog;
        $('#enroll-form').onsubmit = async event => {
          event.preventDefault();
          try {
            const mePassword = passwordOf(event.target);
            const d = await api('developer/enroll', {certificate: event.target.certificate.value, host: location.hostname, ...withPassword(mePassword)});
            if (mePassword) passwordConfirmed();
            const url = URL.createObjectURL(new Blob([JSON.stringify(d, null, 2)], {type: 'application/json'}));
            const link = document.createElement('a');
            link.href = url;
            link.download = `panel-enrollment-${d.id}.json`;
            link.click();
            setTimeout(() => URL.revokeObjectURL(url), 10000);
            closeDialog();
            toast(t('Enrolled. Import the downloaded file with panelctl.'));
          } catch (error) { if (!askPasswordAgain(error)) $('#dialog-error').textContent = error.message; }
        };
      };
    },
  };
}

// ---------------------------------------------------------------------------------------
// Z-Wave (docs/features/36 screens.md §11): the radio, the devices, and SmartStart's list -
// sensors registered by their QR code (scanned with this phone's camera, or pasted) or their
// 40-digit key, which join by themselves when powered. The panel checks every code again.

// The panel's own words only (never a name), so marked with tx() here and read with t() where shown.
const ZW_RADIO = {working: ['good', tx('Working')], starting: ['info', tx('Starting')], not_answering: ['warn', tx('Not answering')],
  not_connected: ['', tx('Not connected')], unusable: ['warn', tx('Can’t be used')], foreign: ['warn', tx('Belongs to another system')],
  replaced: ['warn', tx('Replaced or reset')], updating: ['info', tx('Updating')]};
const ZW_REGIONS = {0: tx('Europe'), 1: tx('United States'), 2: tx('Australia and New Zealand'), 3: tx('Hong Kong'), 5: tx('India'),
  6: tx('Israel'), 7: tx('Russia'), 8: tx('China'), 9: tx('United States with Long Range'), 11: tx('Europe with Long Range'),
  32: tx('Japan'), 33: tx('Korea')};
const ZW_KINDS = [tx('Device'), tx('Sensor'), tx('Keypad'), tx('Switch'), tx('Dimmer'), tx('Relays'), tx('Siren'), tx('Range extender')];
const ZW_SECURITY = [tx('Not asked yet'), tx('Not encrypted'), tx('Older security (S0)'), tx('Without a PIN'), tx('With its PIN'), tx('Access control'),
  tx('Not confirmed: its setup didn’t finish'), tx('Not encrypted (it supports encryption)')];
const ZW_KEYS = [[4, tx('Access control')], [2, tx('With its PIN')], [1, tx('Without a PIN')]];
const ZW_PROV = {waiting: ['info', tx('Waiting')], joined: ['good', tx('Joined')], stopped: ['warn', tx('Stopped')], off: ['', tx('Off')]};
const ZW_RESULTS = {
  added: tx('Registered: it joins by itself once it is powered.'),
  updated: tx('It was registered already: turned on again.'),
  joined_already: tx('Registered. That device is in the network already.'),
  full: tx('32 sensors are registered: forget one first.'),
  bad_code: tx('That isn’t a Z-Wave QR code or 40-digit key.'),
  checksum: tx('That code is damaged or mistyped: scan or type it again.'),
  s2_only: tx('That code can’t join by itself: add the device at the panel with its PIN.'),
  not_allowed: tx('Only the installer can do that.'),
  not_found: tx('It is no longer registered.'),
};
// A device row's status word and its tag, from its setup state and whether it is heard (the touchscreen's device_look).
// Device states: 0 ready, 1 setting up, 2 waits for a wake-up, 3 can't report, 4 another controller, 5 setup stopped, 6 joining.
function zwSpan(s) {
  if (s < 3600) return tn('{0} min', '{0} min', Math.max(1, Math.floor(s / 60)));
  if (s < 172800) return tn('{0} h', '{0} h', Math.floor(s / 3600));
  return tn('{0} day', '{0} days', Math.floor(s / 86400));
}
// BL-152: a finished device read again - the panel keeps every setup step done while it reads it (the touchscreen's rereading()).
const zwRereading = d => [1, 2, 5].includes(d.state) && d.setup_total > 0 && d.setup_done >= d.setup_total;
function zwStatus(d) {
  const reread = zwRereading(d);
  // a re-read is calm (the device works as before), unless it stopped answering or its battery is low: those come first
  if (reread && !d.unreachable && !d.battery_low) {
    return [d.state === 2 ? t('Read again at its next wake-up') : d.state === 5 ? t('Read again stopped: works as before') : t('Being read again'), ''];
  }
  switch (reread ? -1 : d.state) {
    case 1: return [t('Being set up'), ''];
    case 2: return [t('Wake it to finish setting it up'), 'warn'];
    case 3: return [t('It can’t report to the panel'), 'warn'];
    case 4: return [t('Another controller: remove it'), 'warn'];
    case 5: return [t('Setup stopped'), 'warn'];
    case 6: return [t('Being added: its question waits on Add a device'), 'warn'];
    default: break;
  }
  if (d.unreachable) return [d.heard_s === undefined ? t('Not heard since the panel started') : t('Not heard for {0}', zwSpan(d.heard_s)), 'warn'];
  return [zwAgo(d.heard_s), null];
}
const zwWord = (table, key, fallback = '') => table[key] !== undefined ? t(table[key]) : fallback;

// A network operation (screens.md §4, §7, §11), as the touchscreen says it.
const ZW_OP_TITLES = {add: tx('Adding a device'), remove: tx('Removing a device'), remove_failed: tx('Removing a failed device'),
  replace: tx('Replacing a device'), ping: tx('Checking a device'), identify: tx('Blinking its light'), routes: tx('Rebuilding routes'),
  check_in: tx('Changing a check-in'), region: tx('Changing the region'), reset: tx('Resetting the network'), restart: tx('Restarting the radio'),
  firmware: tx('Updating a device’s software'), radio_update: tx('Updating the Z-Wave radio')};
// Why the radio's own update ended without it (ui_zw_fw_failure_t), its bootloader's code shown.
function zwRadioFailure(g) {
  const code = g.fw_error ? `0x${g.fw_error.toString(16).toUpperCase().padStart(2, '0')}` : '';
  switch (g.fw_why) {
    case 4: return g.fw_error === 0x18 ? t('Its bootloader refused the file (error 0x18): from 1.50 or 2.00, install 2.20 first.')
      : code ? t('Its bootloader refused the file (error {0}). It keeps its old software.', code)
        : t('Its bootloader refused the file. It keeps its old software.');
    case 5: return t('The file kept arriving damaged. Check the radio’s wires, then try again.');
    case 7: return t('The radio stopped answering during the update. If it isn’t back in a few minutes, restart the panel, then try again.');
    case 1: return t('Its bootloader doesn’t take updates over the wire.');
    default: return t('The new software didn’t start: the radio waits in its bootloader. Try the update again.');
  }
}
const ZW_S2_WHY = {wrong_pin: tx('The PIN didn’t match its label.'), timeout: tx('It, or the answer, took too long.'),
  cancelled: tx('You chose to add it without encryption.'), device_stopped: tx('The device stopped the secure setup.'),
  failed: tx('The secure setup failed.'), no_info: tx('It didn’t say what it supports.')};
// Whether a device can be a zone: a switch, a dimmer, a relay kit and a keypad have no inputs (ZW-019, screens.md §3 and §11).
const ZW_NO_INPUTS = [2, 3, 4, 5];
// BL-152 (ZW-024): the devices a route rebuild names, by the name the table shows
function zwRouteWho(node, devices) {
  const d = devices.find(x => x.node === node);
  return (d && d.model) || t('device {0}', node);
}
function zwRouteList(g, devices, what) {
  return (g.routes_list || []).filter(x => x.what === what).map(x => zwRouteWho(x.node, devices));
}
function zwDone(g, devices = []) {
  const who = g.model || (g.node ? t('device {0}', g.node) : t('It'));
  const zones = g.zones?.length ? tn('Zone {1} now needs a sensor.', 'Zones {1} now need a sensor.', g.zones.length, g.zones.join(', ')) : '';
  switch (g.result) {
    case 'added': {
      const dev = devices.find(d => d.node === g.node && (g.generation === undefined || d.generation === g.generation));
      const first = [3, 4, 5].includes(g.security) ? t('Encrypted with Security 2.') : t('{0}.', zwWord(ZW_SECURITY, g.security, t('Added')));
      if (dev && ZW_NO_INPUTS.includes(dev.kind)) {
        return ['good', t('Added · {0}', who), dev.kind === 2 ? t('{0} Open its page at the panel to set it up.', first)
          : t('{0} Switch it on its page at the panel.', first)];
      }
      return ['good', t('Added · {0}', who), t('{0} Make it a zone under Installer > Zones: open a zone, then Its sensor.', first), 'zone-setup'];
    }
    case 'added_insecure': return ['warn', t('Added without encryption'), zwWord(ZW_S2_WHY, g.s2_failure)];
    case 'downgraded': return ['warn', t('Added with weaker security'), t('It supports better security than it got.')];
    case 'no_reports': return ['warn', t('Added, but it doesn’t report'), t('It can’t tell the panel anything, so it can’t be a zone.')];
    case 'nothing_joined': return ['', t('Nothing joined'), t('No device answered in time. Put it in pairing mode while the countdown runs. If it was used with another system, remove it first: put it in pairing mode on the Remove page, then add it.')];
    case 'join_failed': return ['warn', t('Didn’t join'), t('It didn’t finish joining. Remove it (press its button while removing), then add it again.')];
    case 'already_added': return ['', t('Already added'), t('{0} is in the network already.', who)];
    case 'controller': return ['warn', t('Not a sensor'), t('That is another controller: the panel doesn’t add it.')];
    case 'stopped': return ['', t('Stopped'), t('Nothing changed.')];
    case 'removed': return ['good', t('Removed · {0}', who), zones];
    case 'removed_foreign': return ['good', t('Removed'), t('A device from another system was reset: it can be added now.')];
    case 'nothing_removed': return ['', t('Nothing removed'), t('No device answered in time. Press its button while the countdown runs.')];
    case 'still_answers': return ['warn', t('Kept'), t('It answered, so it wasn’t removed. Use Remove a device and press its button.')];
    case 'answered': return ['good', t('It answered'), ''];
    case 'no_answer': return ['warn', t('No answer'), t('Check its power, or bring it closer to the panel.')];
    case 'done': {
      if (g.op !== 'routes') return ['good', t('Done'), ''];
      const bad = (g.routes_failed || 0) + (g.routes_skipped || 0);
      const good = g.routes_done - bad;
      const parts = [g.routes_stopped ? t('Stopped: {0} of {1} done.', good, g.routes_total) : t('Routes rebuilt for {0} of {1}.', good, g.routes_total)];
      const failed = zwRouteList(g, devices, 'failed'), skipped = zwRouteList(g, devices, 'skipped'), noRoute = zwRouteList(g, devices, 'no_route');
      if (g.routes_failed) parts.push(t('Didn’t answer: {0}.', failed.join(', ') || g.routes_failed));
      if (g.routes_skipped) parts.push(t('Skipped (dead, Long Range or stopped): {0}.', skipped.join(', ') || g.routes_skipped));
      if (noRoute.length) parts.push(t('Its way to the panel wasn’t renewed: {0}.', noRoute.join(', ')));
      return [bad ? 'warn' : 'good', t('Done'), parts.join(' ')];
    }
    case 'setup_paused': return ['info', t('Setup not finished'), t('It joined; the panel finishes reading it later. Wake it if it sleeps.')];
    case 'timed_out': return ['warn', t('Not confirmed'), t('It didn’t finish in time. Try again.')];
    case 'radio_restarted': return ['warn', t('It couldn’t be done'), t('The Z-Wave radio restarted, so it stopped listening. Try again.')];
    case 'refused': return ['warn', t('Not removed'), t('The Z-Wave radio doesn’t count it as failed yet. Wait a minute, then try again.')];
    case 'fw_updated': return ['good', g.op === 'radio_update' ? t('Radio updated') : t('Updated'), g.op === 'radio_update' ? t('Its sensors are heard again once it is back.') : ''];
    case 'fw_failed': return ['warn', g.op === 'radio_update' ? t('Radio update failed') : t('Update failed'), g.op === 'radio_update' ? zwRadioFailure(g) : ''];
    default: return ['warn', t('It couldn’t be done'), t('Try again.')];
  }
}
function zwTime(s) { return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`; }
function zwAgo(s) {
  if (s === undefined) return t('Not heard yet');
  if (s < 60) return t('Just now');
  if (s < 3600) return tn('{0} min ago', '{0} min ago', Math.floor(s / 60));
  if (s < 172800) return t('{0} h ago', Math.floor(s / 3600));
  return tn('{0} day ago', '{0} days ago', Math.floor(s / 86400));
}
// The card that follows the operation running. Its key names what is drawn, so a poll redraws it
// only when that changed (never under a PIN being typed).
function zwOpCard(g, devices = []) {
  if (!g || g.op === 'none') return {key: '', html: ''};
  const title = zwWord(ZW_OP_TITLES, g.op, t('Working'));
  const buttons = (...list) => `<div class="card-foot">${list.join('')}</div>`;
  const b = (action, words, cls = '') => `<button type="button" class="${cls}" data-zwop="${action}">${esc(words)}</button>`;
  let body = '', key = `${g.op}:${g.step}:${g.result}`;
  switch (g.step) {
    case 'starting': body = `<p>${ht('Starting…')}</p>`; break;
    case 'listening':
      body = `<p>${g.op === 'remove' ? ht('Press the device’s Z-Wave button now (or its reset hole).') : ht('Put the device in pairing mode now, as its manual says.')}</p>
        <p class="muted-note">${ht('Listening · {0} left', zwTime(g.seconds_left))}</p>` + buttons(b('keep_listening', t('Keep listening')), b('stop', t('Stop')));
      key += `:${g.seconds_left}`;
      break;
    case 'connecting': body = `<p>${g.op === 'remove' ? ht('A device answered: it is leaving…') : ht('A device answered: it is joining…')}</p>`; break;
    case 'ask_allow': {
      const boxes = ZW_KEYS.filter(([bit]) => g.requested & bit).map(([bit, w]) =>
        `<label class="check"><input type="checkbox" name="zw-key" value="${bit}" checked> ${ht(w)}</label>`).join('');
      body = `<p>${ht('Device {0} asks for this security. Allow it quickly, before the time left runs out.', g.node)}</p>
        <p class="muted-note">${ht('Time left')} <strong id="zw-left">${zwTime(g.seconds_left)}</strong></p>
        <div class="rows">${boxes}</div>` + buttons(b('insecure', t('Add it without encryption')), b('allow', t('Allow'), 'primary'));
      break;
    }
    case 'ask_pin':
      body = `<p>${ht('Type the first five digits of its key, printed on its label or box.')}</p>
        <p class="mono">${esc(g.dsk)}</p>
        <p class="muted-note">${ht('Time left')} <strong id="zw-left">${zwTime(g.seconds_left)}</strong></p>
        <div class="row"><div><label class="label" for="zw-pin">${ht('PIN')}</label></div>
          <div class="control"><input id="zw-pin" inputmode="numeric" maxlength="5" autocomplete="off" pattern="[0-9]{5}"></div></div>
        <p class="dialog-error" id="zw-pin-error" role="alert"></p>` + buttons(b('insecure', t('Add it without encryption')), b('pin', t('Continue'), 'primary'));
      break;
    case 'securing': body = `<p>${ht('Securing its connection…')}</p>`; break;
    case 'reading':
      body = `<p>${g.asleep ? ht('Reading what it can do · Step {0} of {1} · Wake it to finish', g.setup_done, g.setup_total || 16)
        : ht('Reading what it can do · Step {0} of {1}', g.setup_done, g.setup_total || 16)}</p>` +
        buttons(b('stop', t('Stop and finish later')));
      key += `:${g.setup_done}:${g.asleep}`;
      break;
    case 'working':
      body = g.op === 'routes' ? `<p>${ht('{0} of {1} done', g.routes_done, g.routes_total)}</p>` +
          (g.routes_waiting ? `<p class="muted-note">${ht('Wake these: {0}', zwRouteList(g, devices, 'waiting').join(', ') || g.routes_waiting)}</p>` : '')
        : g.op === 'radio_update' ? `<p>${ht('{0} % sent. Z-Wave sensors aren’t heard, and arming waits, until the radio is back. Keep the panel powered.', g.fw_total ? Math.floor(g.fw_done * 100 / g.fw_total) : 0)}</p>`
          : `<p>${ht('Working…')}</p>`;
      key += `:${g.routes_done}:${g.routes_waiting || 0}:${g.fw_done}`;
      break;
    case 'done': {
      const [kind, head, words, link] = zwDone(g, devices);
      let open = link ? `<button type="button" data-zwgo="${link}">${ht('Open Zones')}</button>` : '';
      // ZW-006: a device that still belongs to another system must be removed first. ZW-005: a restart closes the window.
      if (g.result === 'nothing_joined' && g.op === 'add') open += b('remove', t('Remove a device'));
      if (g.result === 'radio_restarted' && (g.op === 'add' || g.op === 'remove')) open += b(g.op, t('Try again'));
      return {key, html: `<section class="card status-card ${kind}" id="zw-op"><span class="state">${esc(head)}</span>
        ${words ? `<p>${esc(words)}</p>` : ''}${buttons(open + b('dismiss', t('OK'), 'primary'))}</section>`};
    }
    default: body = `<p>${ht('Working…')}</p>`;
  }
  return {key, html: `<section class="card pad" id="zw-op"><h2>${esc(title)}</h2>${body}</section>`};
}

function zwName(e) {
  if (e.name) return e.name;
  if (e.model) return e.model; // ZW-024: the model its code names, when the panel knows it
  const kind = e.generic === 0x10 ? t('Switch') : e.generic === 0x11 ? t('Dimmer') : e.generic === 0x40 ? t('Keypad') : t('Sensor');
  return t('{0} {1}', kind, e.key.slice(6, 11));
}
function zwKeys(mask) {
  return ZW_KEYS.filter(([bit]) => mask & bit).map(([, w]) => t(w)).join(', ') || t('None');
}

// What a pasted or scanned code says, before the panel is asked (network.md §4.2): the checksum,
// the keys it asks for, the product. A code of 40 digits is a typed key. The panel checks again.
async function zwReadCode(text) {
  const plain = text.replace(/[\s]/g, '');
  const digits = plain.replace(/-/g, '');
  if (/^[\d-]+$/.test(plain) && digits.length === 40) {
    const groups = digits.match(/\d{5}/g).map(Number);
    return groups.some(g => g > 65535) ? {error: 'bad_code'} : {key: true};
  }
  if (!/^\d+$/.test(plain) || !plain.startsWith('90')) return {error: 'bad_code'};
  if (plain.length < 52) return {error: 'bad_code'};
  const version = Number(plain.slice(2, 4));
  if (version > 1) return {error: 'bad_code'};
  if (crypto.subtle) {
    const hash = new Uint8Array(await crypto.subtle.digest('SHA-1', new TextEncoder().encode(plain.slice(9))));
    if (Number(plain.slice(4, 9)) !== hash[0] * 256 + hash[1]) return {error: 'checksum'};
  }
  const out = {version, requested: Number(plain.slice(9, 12)), smartstart: version === 1};
  for (let at = 52; at + 4 <= plain.length;) {
    const type = Number(plain.slice(at, at + 2)) >> 1, length = Number(plain.slice(at + 2, at + 4));
    const data = plain.slice(at + 4, at + 4 + length);
    if (type === 1 && data.length === 20) {
      out.product = [0, 5, 10].map(i => Number(data.slice(i, i + 5)).toString(16).toUpperCase().padStart(4, '0')).join(':');
    }
    at += 4 + length;
  }
  return out;
}

// The camera, where the browser can read QR codes itself (no library is sent to the phone).
async function zwScan(onCode) {
  if (typeof window.BarcodeDetector !== 'function' || !navigator.mediaDevices?.getUserMedia) {
    return toast(t('This browser can’t read QR codes: paste or type the code instead.'), 'error');
  }
  let stream;
  try {
    stream = await navigator.mediaDevices.getUserMedia({video: {facingMode: 'environment'}, audio: false});
  } catch {
    return toast(t('The camera is not available. Allow it for this page, or type the code.'), 'error');
  }
  openDialog(`<div class="dialog-head"><h2 id="dialog-title">${ht('Scan its QR code')}</h2>
    <p>${ht('Hold the code on its label or box inside the frame.')}</p></div>
    <div class="dialog-body"><video id="zw-video" class="scan-video" playsinline muted></video></div>
    <div class="dialog-foot"><button type="button" id="zw-scan-cancel">${ht('Cancel')}</button></div>`);
  let stopped = false;
  const stop = () => { stopped = true; stream.getTracks().forEach(t => t.stop()); };
  $('#zw-scan-cancel').onclick = () => { stop(); closeDialog(); };
  $('#dialog').addEventListener('close', stop, {once: true});
  const video = $('#zw-video');
  video.srcObject = stream;
  try { await video.play(); } catch { /* shown once it can */ }
  const detector = new BarcodeDetector({formats: ['qr_code']});
  const look = async () => {
    if (stopped) return;
    try {
      const found = (await detector.detect(video)).find(c => /^\s*90\d{50,}/.test(c.rawValue));
      if (found) { stop(); closeDialog(); return onCode(found.rawValue.trim()); }
    } catch { /* the next frame */ }
    setTimeout(look, 300);
  };
  look();
}

async function zwavePage() {
  const z = await api('zwave');
  if (!z.available) {
    return {html: pageHead(nt('Z-Wave')) + notice('info', t('Z-Wave isn’t running on this panel'),
      t('Choose where its radio is wired in the Flasher or at the panel.'))};
  }
  const [kind, words] = z.radio.state === 'working' && z.radio.jammed ? ['warn', t('Interference')]
    : ZW_RADIO[z.radio.state] ? [ZW_RADIO[z.radio.state][0], t(ZW_RADIO[z.radio.state][1])] : ['', z.radio.state];
  const name = d => d.model || zwWord(ZW_KINDS, d.kind, t('Device'));
  const devices = z.devices.map(d => {
    const at = `data-node="${d.node}" data-gen="${d.generation}" data-name="${esc(name(d))}"`;
    const acts = [d.listening ? `<button type="button" data-zwdev="ping" ${at}>${ht('Check it now')}</button>` : '',
      d.state === 5 ? `<button type="button" data-zwdev="setup" ${at}>${ht('Try its setup again')}</button>` : '',
      // BL-152: a finished device read again from the start (Z-Wave JS's re-interview), its zones kept
      isInstaller() && (d.state === 0 || d.state === 3)
        ? `<button type="button" data-zwdev="reread" data-sleeps="${d.listening ? 0 : 1}" ${at}>${ht('Set it up again')}</button>` : '',
      `<button type="button" data-zwdev="routes" ${at}>${ht('Rebuild its routes')}</button>`,
      `<button type="button" class="danger" data-zwdev="remove_failed" ${at}>${ht('Remove as failed')}</button>`].join('');
    return `<tr id="zwdev-${d.node}"><td>${esc(name(d))}</td>
    <td>${ht('device {0}', d.node)}</td><td>${esc(zwWord(ZW_SECURITY, d.security))}</td>
    <td>${d.battery !== undefined ? ht('{0} %', d.battery) : ''}${d.battery_low ? ' ' + tag(t('Low'), 'warn') : ''}</td>
    <td>${(([words, kind]) => kind === null ? esc(words) : tag(words, kind))(zwStatus(d))}</td>
    <td>${d.signal ? esc(zgSignal(d)) : ''}</td>
    <td>${d.zones.length ? esc(tn('Zone {1}', 'Zones {1}', d.zones.length, d.zones.join(', '))) : ht('Not in a zone')}</td>
    <td class="actions">${acts}</td></tr>`;
  }).join('');
  const op = zwOpCard(z.progress, z.devices);
  const running = z.progress && z.progress.op !== 'none' && z.progress.step !== 'done';
  const entries = z.smartstart.map(e => {
    const [k, w] = ZW_PROV[e.state] ? [ZW_PROV[e.state][0], t(ZW_PROV[e.state][1])] : ['', e.state];
    const toggle = e.state === 'waiting' ? ['off', t('Turn off')] : e.state === 'off' || e.state === 'stopped' ? ['on', t('Turn on')] : null;
    return `<tr><td><strong>${esc(zwName(e))}</strong>${e.name && e.model ? `<br><span class="sub">${esc(e.model)}</span>` : ''}<br><span class="mono sub">${esc(e.key)}</span></td>
      <td>${tag(w, k)}${e.state === 'stopped' ? `<br><span class="sub">${esc(tn('Could not secure it {0} time: check its code', 'Could not secure it {0} times: check its code', e.failures))}</span>` : ''}
        ${e.state === 'joined' ? `<br><button type="button" class="link" data-zwfocus="${e.node}">${ht('device {0}', e.node)}</button>` : ''}</td>
      <td>${esc(zwKeys(e.grant))}${e.long_range ? `<br><span class="sub">${ht('Long Range')}</span>` : ''}</td>
      <td>${esc(e.product || '')}</td>
      <td class="actions">${toggle ? `<button type="button" data-zw="${toggle[0]}" data-id="${esc(e.id)}">${esc(toggle[1])}</button>` : ''}
        <button type="button" class="danger" data-zw="forget" data-id="${esc(e.id)}" data-name="${esc(zwName(e))}">${ht('Forget')}</button></td></tr>`;
  }).join('');
  const region = z.radio.region_known ? zwWord(ZW_REGIONS, z.radio.region, t('Other')) : t('Unknown');
  return {
    html: pageHead(nt('Z-Wave'), t('Sensors paired with the panel’s Z-Wave radio.')) +
      `<section class="card status-card ${kind}"><span class="state">${ht('Radio · {0}', words)}</span>
        ${kv([[t('Region'), region],
          [t('Devices'), String(z.radio.devices)], [t('Not heard'), z.radio.not_heard ? String(z.radio.not_heard) : ''],
          [t('Radio software'), z.radio.firmware], [t('Z-Wave version'), z.radio.sdk],
          [t('Background noise'), z.radio.noise_dbm !== undefined ? t('{0} dBm (quietest channel)', z.radio.noise_dbm) : '']])}
        ${z.radio.jammed ? `<p>${ht('Something drowns out its signals: sensors may not be heard. The panel shows a sensor radio trouble until the air clears.')}</p>` : ''}
        ${z.radio.update_advised ? `<p>${ht('Its Z-Wave software is older than the panel wants.')}</p>` : ''}
        ${z.radio.storage_full ? `<p>${ht('The panel’s storage is full: no new device can be added until space is freed. Devices on the SmartStart list join once there is room.')}</p>` : ''}
        <div class="card-foot"><button type="button" data-zwgo="zwaveradio">${ht('Radio details')}</button>
          <button type="button" data-zwop="restart" ${running ? 'disabled' : ''}>${ht('Restart the radio')}</button></div></section>
      <div id="zw-op-slot">${op.html}</div>
      <section class="section"><h2>${ht('Devices')}</h2>
        <div class="card-foot left"><button type="button" class="primary" data-zwop="add" ${running ? 'disabled' : ''}>${ht('Add a device')}</button>
          <button type="button" data-zwop="remove" ${running ? 'disabled' : ''}>${ht('Remove a device')}</button>
          <button type="button" data-zwop="routes" ${running || !z.devices.length ? 'disabled' : ''}>${ht('Rebuild all routes')}</button></div>
        ${devices ? `<div class="card table-wrap"><table>
        <thead><tr><th>${ht('Device')}</th><th></th><th>${ht('Security')}</th><th>${ht('Battery')}</th><th>${ht('Last heard')}</th><th>${ht('Signal')}</th><th>${ht('Zones')}</th><th></th></tr></thead>
        <tbody>${devices}</tbody></table></div>` : `<p class="muted-note">${ht('No devices yet: Add a device, then put it in pairing mode.')}</p>`}</section>
      <section class="section"><h2>${ht('Waiting to join')}</h2>
        <p class="muted-note">${ht('A registered sensor joins by itself when it is powered, secured with S2: no buttons to press. It joins only while every partition is disarmed.')}</p>
        ${entries ? `<div class="card table-wrap"><table><thead><tr><th>${ht('Sensor')}</th><th>${ht('Now')}</th><th>${ht('Security it may have')}</th><th>${ht('Product')}</th><th></th></tr></thead>
          <tbody>${entries}</tbody></table></div>` : `<p class="muted-note">${ht('No sensor is registered.')}</p>`}
        <form class="card pad narrow" id="zw-register" novalidate><h3>${ht('Register a sensor')}</h3><div class="rows">
          <div class="row"><div><label class="label" for="zw-code">${ht('Its QR code or 40-digit key')}</label>
            <span class="help">${ht('Scan it with this phone’s camera, paste what a scanner app read, or type the key printed under the code.')}</span></div>
            <div class="control"><textarea id="zw-code" rows="3" maxlength="600" autocomplete="off" spellcheck="false"></textarea></div></div>
          <div class="row"><div><label class="label" for="zw-name">${ht('Name')}</label><span class="help">${ht('Optional: where it is.')}</span></div>
            <div class="control"><input id="zw-name" type="text" maxlength="23" autocomplete="off"></div></div></div>
          <p class="muted-note" id="zw-check" role="status"></p><p class="dialog-error" id="zw-error" role="alert"></p>
          <div class="card-foot"><button type="button" id="zw-scan">${ht('Scan with the camera')}</button>
            <button class="primary" id="zw-submit">${ht('Register')}</button></div></form></section>`,
    bind() {
      const check = async () => {
        const text = $('#zw-code').value.trim();
        $('#zw-error').textContent = '';
        if (!text) { $('#zw-check').textContent = ''; return null; }
        const c = await zwReadCode(text);
        $('#zw-check').textContent = c.error ? '' : c.key ? t('A 40-digit key: every S2 security it asks for may be granted.')
          : !c.smartstart ? t('This code is for adding it at the panel with its PIN; it can’t join by itself.')
            : c.product ? t('Checksum right · asks for {0} · product {1}.', zwKeys(c.requested), c.product)
              : t('Checksum right · asks for {0}.', zwKeys(c.requested));
        if (c.error) $('#zw-error').textContent = zwWord(ZW_RESULTS, c.error);
        return c;
      };
      $('#zw-code').oninput = check;
      $('#zw-scan').onclick = () => zwScan(code => { $('#zw-code').value = code; check(); });
      $('#zw-register').onsubmit = async event => {
        event.preventDefault();
        const c = await check();
        if (!c || c.error) return;
        const button = $('#zw-submit');
        button.disabled = true;
        try {
          const r = await api('zwave', {action: 'register', code: $('#zw-code').value.trim(), name: $('#zw-name').value.trim()});
          const ok = ['added', 'updated', 'joined_already'].includes(r.result);
          if (!ok) { $('#zw-error').textContent = zwWord(ZW_RESULTS, r.result, r.result); return; }
          toast(zwWord(ZW_RESULTS, r.result));
          await show();
        } catch (error) {
          $('#zw-error').textContent = error.message;
        } finally {
          button.disabled = false;
        }
      };
      // Network operations (screens.md §11): the same steps as the touchscreen, the PIN typed here.
      const send = async body => {
        const r = await api('zwave', body);
        if (r.result !== 'ok') throw new Error(zwRefusal(r.result));
      };
      let drawn = op.key;
      const bindOp = () => $$('[data-zwop]').forEach(button => button.onclick = async () => {
        const action = button.dataset.zwop;
        const body = {action};
        if (action === 'allow') {
          body.keys = $$('input[name="zw-key"]:checked').reduce((m, i) => m | Number(i.value), 0);
          if (!body.keys) return toast(t('Tick at least one, or add it without encryption.'), 'error');
        }
        if (action === 'pin') {
          const pin = $('#zw-pin').value.trim();
          if (!/^\d{5}$/.test(pin) || Number(pin) > 65535) return void ($('#zw-pin-error').textContent = t('Five digits, as on its label.'));
          body.pin = Number(pin);
        }
        if (action === 'routes') body.node = 0;
        const go = async () => { await send(body); if (action === 'pin') $('#zw-pin').value = ''; await show(); };
        if (action === 'restart') {
          return review({title: t('Restart the Z-Wave radio?'), intro: t('Takes a few seconds; zones keep their state.'), confirm: t('Restart'), done: t('Restarting.'), run: () => send(body)});
        }
        if (action === 'routes') {
          return review({title: t('Rebuild routes?'), intro: t('Refreshes how every device’s signals reach the panel. Sleeping devices are done when they next wake up.'),
            confirm: t('Rebuild'), done: t('Rebuilding.'), run: () => send(body)});
        }
        button.disabled = true;
        try { await go(); } catch (error) { toast(error.message, 'error'); button.disabled = false; }
      });
      bindOp();
      $$('[data-zwgo]').forEach(button => button.onclick = () => go(button.dataset.zwgo));
      // ZW-024: a joined sensor's "device N" takes the eye to that device's row (steady, no blinking)
      $$('[data-zwfocus]').forEach(button => button.onclick = () => {
        const row = $(`#zwdev-${button.dataset.zwfocus}`);
        if (!row) return;
        $$('tr[id^="zwdev-"]').forEach(r => { r.style.outline = ''; });
        row.style.outline = '2px solid var(--info)';
        row.scrollIntoView({block: 'center'});
      });
      $$('[data-zwdev]').forEach(button => button.onclick = async () => {
        const action = button.dataset.zwdev, who = button.dataset.name;
        const body = {action, node: Number(button.dataset.node), generation: Number(button.dataset.gen)};
        if (action === 'remove_failed') {
          return review({title: t('Remove {0} as failed?', who), intro: t('For a device that is broken, reset or gone: the radio checks it no longer answers first. Its zones then need another sensor.'),
            confirm: t('Remove it'), danger: true, done: t('Checking it no longer answers.'), run: () => send(body)});
        }
        if (action === 'reread') {
          const sleeps = button.dataset.sleeps === '1';
          return review({title: t('Set {0} up again?', who),
            intro: sleeps ? t('The panel reads what it can do again, at its next wake-up: press its button to start now. Its zones keep working, and it keeps its security.')
              : t('The panel reads what it can do again. Its zones keep working, and it keeps its security.'),
            confirm: t('Set it up again'), done: sleeps ? t('It’s read again at its next wake-up.') : t('Reading it again.'),
            run: () => send({...body, action: 'setup'})});
        }
        if (action === 'routes') {
          return review({title: t('Rebuild {0}’s routes?', who), intro: t('Refreshes how its signals reach the panel. A sleeping device is done when it next wakes up.'),
            confirm: t('Rebuild'), done: t('Rebuilding.'), run: () => send(body)});
        }
        button.disabled = true;
        try { await send(body); await show(); } catch (error) { toast(error.message, 'error'); button.disabled = false; }
      });
      // While an operation runs, its card follows it every second; the rest of the page only when
      // the operation ends.
      if (running) poll(async () => {
        if ($('#dialog').open) return;
        const now = await api('zwave');
        const card = zwOpCard(now.progress, now.devices);
        if (!now.progress || now.progress.op === 'none' || now.progress.step === 'done') { await show(); return; }
        const left = $('#zw-left');
        if (left && now.progress.seconds_left !== undefined) left.textContent = zwTime(now.progress.seconds_left);
        if (card.key === drawn || document.activeElement === $('#zw-pin') || $('#zw-pin')?.value) return;
        drawn = card.key;
        $('#zw-op-slot').innerHTML = card.html;
        bindOp();
      }, 1000);
      $$('[data-zw]').forEach(button => button.onclick = async () => {
        const action = button.dataset.zw, id = button.dataset.id;
        const run = async () => {
          const r = await api('zwave', {action, id});
          if (r.result !== 'updated') throw new Error(zwWord(ZW_RESULTS, r.result, r.result));
        };
        if (action === 'forget') {
          return review({title: t('Forget {0}?', button.dataset.name), intro: t('It won’t join by itself any more. A device already in the network stays there.'),
            confirm: t('Forget it'), danger: true, done: t('Forgotten.'), run});
        }
        button.disabled = true;
        try { await run(); await show(); } catch (error) { toast(error.message, 'error'); button.disabled = false; }
      });
      // A sensor waiting to join: the page is drawn again only when something changed (never a
      // blank "Loading" every few seconds), and not while a code is being typed or a dialog is open.
      const seen = JSON.stringify([z.radio, z.devices, z.smartstart]);
      // The radio's own check or download is out (network.md §14): its operation starts from it.
      const asking = ['checking', 'downloading'].includes(z.radio.software?.check) || z.radio.state === 'updating';
      if (!running && asking) poll(async () => {
        if ($('#dialog').open) return;
        const now = await api('zwave');
        if (JSON.stringify([now.radio, now.progress.op, now.progress.step]) !== JSON.stringify([z.radio, z.progress.op, z.progress.step])) await show();
      }, 1000);
      else if (!running && z.smartstart.some(e => e.state === 'waiting')) poll(async () => {
        if ($('#dialog').open || $('#zw-code').value) return;
        const now = await api('zwave');
        if (JSON.stringify([now.radio, now.devices, now.smartstart]) !== seen) await show();
      }, 5000);
    },
  };
}

// The radio's diagnostics (screens.md §11 "Radio"): what the touchscreen leaves out.
async function zwaveRadioPage() {
  const z = await api('zwave');
  const d = z.diag;
  if (!z.available || !d) {
    return {html: pageHead(t('Z-Wave radio'), '', '', ['zwave', nt('Z-Wave')]) + notice('info', t('Z-Wave isn’t running on this panel'), '')};
  }
  const dbm = v => v === undefined ? '' : t('{0} dBm', v);
  const noise = d.noise_dbm ? t('{0} dBm', d.noise_dbm.map(v => v === 127 ? '–' : v === 126 ? t('saturated') : v === 125 ? t('quiet') : `${v}`).join(' · ')) : t('Not read yet');
  const nodes = d.nodes.map(n => `<tr><td>${ht('device {0}', n.node)}</td><td>${n.wrong_level}</td><td>${n.decrypt_failures}</td><td>${n.duplicates}</td></tr>`).join('');
  // Its own software (network.md §14): the update service asked as Z-Wave JS asks about a
  // controller; or its maker's own .gbl uploaded (the ZAC93 isn't on the service yet).
  const sw = z.radio.software || {check: 'none'};
  const running = z.progress && z.progress.op !== 'none' && z.progress.step !== 'done';
  const swWords = {checking: t('Asking the update service…'), downloading: t('Downloading its new software and checking it…'),
    up_to_date: t('Up to date.'), unknown: t('The update service doesn’t list this radio yet: upload its maker’s file below.'),
    check_failed: sw.words, file_failed: sw.words}[sw.check] || '';
  const software = `<section class="card pad" id="zw-radio-sw"><h2>${ht('Software')}</h2>
    ${kv([[t('Radio software'), t('{0} · Z-Wave {1}', z.radio.firmware, z.radio.sdk)]])}
    ${swWords ? `<p class="muted-note">${esc(swWords)}</p>` : ''}
    ${sw.check === 'available' ? `<p>${sw.words ? ht('Version {0} is available: {1}.', sw.version, sw.words.replace(/[.\s]+$/, '')) : ht('Version {0} is available.', sw.version)}</p>` : ''}
    <div class="card-foot">
      <button type="button" data-zwr="check" ${running || sw.check === 'checking' || sw.check === 'downloading' ? 'disabled' : ''}>${sw.check === 'none' ? ht('Check for an update') : ht('Check again')}</button>
      ${sw.check === 'available' ? `<button type="button" class="primary" data-zwr="install" ${running ? 'disabled' : ''}>${ht('Install version {0}', sw.version)}</button>` : ''}
    </div>
    <div class="row"><div><label class="label" for="zw-gbl">${ht('Its maker’s file')}</label>
      <span class="help">${ht('A .gbl from the radio’s maker (Zooz, for the ZAC93). The panel checks it before a byte is sent.')}</span></div>
      <div class="control"><input id="zw-gbl" type="file" accept=".gbl"></div></div>
    <div class="card-foot"><button type="button" data-zwr="upload" ${running ? 'disabled' : ''}>${ht('Install this file')}</button></div>
    <p class="muted-note">${ht('Checked only when you ask; nothing installs by itself. While it installs, Z-Wave sensors aren’t heard and arming waits until the radio is back (15 minutes at most).')}</p></section>`;
  return {
    bind() {
      const refused = {not_allowed: t('Only the installer can do that, with every partition disarmed.'), busy: t('Something else is running: try when it’s done.'),
        no_radio: t('The Z-Wave radio isn’t working.'), invalid: t('That isn’t a whole Gecko bootloader (.gbl) file.'), unsupported: t('This panel can’t do that yet.')};
      const done = async r => {
        if (r.result !== 'ok') throw new Error(refused[r.result] || r.result);
      };
      $$('[data-zwr]').forEach(button => button.onclick = async () => {
        const what = button.dataset.zwr;
        if (what === 'check') {
          button.disabled = true;
          try { await done(await api('zwave', {action: 'radio_check'})); await show(); } catch (error) { toast(error.message, 'error'); button.disabled = false; }
          return;
        }
        const run = what === 'install' ? async () => done(await api('zwave', {action: 'radio_install'}))
          : async () => {
            const file = $('#zw-gbl').files[0];
            if (!file) throw new Error(t('Choose the file first.'));
            const response = await fetch('/api/v1/zwave-radio', {method: 'PUT', credentials: 'same-origin',
              headers: {'Content-Type': 'application/octet-stream', 'X-Panel-CSRF': csrf}, body: file});
            const answer = await response.json().catch(() => ({}));
            if (!response.ok) throw new Error(answer.error || t('The panel answered {0}.', response.status));
            await done(answer);
          };
        review({title: what === 'install' ? t('Update the radio to {0}?', sw.version) : t('Install this file on the radio?'),
          intro: t('Z-Wave sensors aren’t heard for up to 15 minutes, and arming waits until the radio is back. Keep the panel powered.'),
          confirm: t('Update'), done: t('Updating the radio.'), run, after: () => go('zwave')});
      });
      if (sw.check === 'checking' || sw.check === 'downloading') poll(async () => {
        const now = await api('zwave');
        if ((now.radio.software || {}).check !== sw.check) await show();
      }, 1000);
    },
    html: pageHead(t('Z-Wave radio'), t('Diagnostics for the installer.'), '', ['zwave', nt('Z-Wave')]) + software +
      `<section class="card pad">${kv([[t('Home ID'), d.home_id], [t('The panel'), t('device {0}', d.node_id)],
        [t('Network manager (SUC)'), d.suc_node_id ? t('device {0}', d.suc_node_id) : t('None')], [t('Radio software'), t('{0} · {1} (type {2})', z.radio.firmware, d.library, d.library_type)],
        [t('Z-Wave version'), z.radio.sdk], [t('Chip'), d.chip], [t('Product'), d.product],
        [t('Region'), z.radio.region_known ? zwWord(ZW_REGIONS, z.radio.region, t('Other')) : t('Unknown')],
        [t('Background noise per channel'), noise], [t('Interference above'), dbm(d.jam_threshold_dbm)],
        [t('Transmit power'), d.power_dbm !== undefined ? (d.power_came_with_dbm !== undefined ? t('{0} (came with {1})', dbm(d.power_dbm), dbm(d.power_came_with_dbm)) : dbm(d.power_dbm)) : t('Not read')],
        [t('Long Range maximum'), dbm(d.lr_power_dbm)],
        [t('Link'), t('up {0} · down {1} · restarted {2} · recovered {3}', d.link_ups, d.link_downs, d.restarts, d.recoveries)],
        [t('Frames from other networks'), String(d.foreign)], [t('Frames that didn’t parse'), String(d.malformed)]])}</section>
      <section class="section"><h2>${ht('Per device')}</h2>
        <p class="muted-note">${ht('Frames at the wrong security level are dropped: a device sending them may be mis-set or not what it seems.')}</p>
        ${nodes ? `<div class="card table-wrap"><table><thead><tr><th>${ht('Device')}</th><th>${ht('Wrong security level')}</th><th>${ht('Didn’t decrypt')}</th><th>${ht('Repeated')}</th></tr></thead>
        <tbody>${nodes}</tbody></table></div>` : `<p class="muted-note">${ht('No devices.')}</p>`}</section>`,
  };
}

// ---------------------------------------------------------------------------------------
// Zigbee (docs/features/20 devices.md §14, ZB-025; the pages BL-145 draws): Installer > Zigbee,
// right after Z-Wave, with its inside pages: adding (zigbee-add), an install code (zigbee-code),
// a device's page (zigbee-device/<id>), radio details (zigbee-radio) and the channel
// (zigbee-channel). Nothing on them blinks or moves: the countdowns change once a second, in words.

// The radio's states, as the touchscreen says them: [look, word, what it means, what to do].
function zgRadioWords(r) {
  const n = r.devices;
  const count = n ? tn('Channel {1} · {0} device', 'Channel {1} · {0} devices', n, r.channel) : t('Channel {0} · No devices', r.channel);
  if (r.state === 'working' && r.jammed) return ['warn', t('Interference'), t('Something nearby drowns channel {0}: sensors may not be heard', r.channel),
    t('Move Wi-Fi routers and other radios away from the panel, or choose a quieter channel below.')];
  const paused = {firmware: t('Its software doesn’t match what the panel expects'), reset: t('It didn’t come back from a restart'),
    no_answer: t('It stopped answering while the panel used it')}[r.paused] || t('It restarted the panel twice, so the panel stopped using it');
  return {
    working: ['good', t('Working'), count, ''],
    starting: ['info', t('Starting'), t('A few seconds'), ''],
    not_connected: ['', t('Not connected'), t('Nothing answers in the wireless socket'), t('Devices added before are kept. They are heard again once the Zigbee radio answers.')],
    not_answering: ['warn', t('Not answering'), t('Check that it sits firmly in its socket; the panel keeps trying'), t('Zigbee sensors aren’t heard until it answers. Their zones keep their last state; a Sensor radio problem follows if it lasts.')],
    paused: ['warn', t('Paused after restarts'), paused, t('Zigbee sensors aren’t heard while it is paused. Their zones keep their last state; a Sensor radio problem follows if it lasts.')],
    socket_zwave: ['warn', t('Socket used by the Z-Wave radio'), t('The wireless socket holds the Z-Wave radio'), t('Zigbee needs the wireless socket. The manual says where else the Z-Wave radio can go.')],
    unusable: ['warn', t('Not usable'), t('It answers, but its software isn’t one the panel can use'), t('Put the panel’s own Zigbee radio software on it with the Flasher.')],
  }[r.state] || ['', r.state, '', ''];
}
const zgUsable = r => r.state === 'working';
// What a device is, by the class of its first input (alarm_device_class_t) or its role.
function zgKindWord(d) {
  const role = {siren: t('Siren and strobe'), keypad: t('Keypad'), keyfob: t('Key fob'), repeater: t('Extends the network'), other: t('A device the panel doesn’t use')}[d.kind];
  if (role) return role;
  return {1: t('Door contact'), 2: t('Window contact'), 3: t('Garage door contact'), 4: t('Motion sensor'), 5: t('Glass break sensor'), 6: t('Smoke alarm'),
    7: t('Heat sensor'), 8: t('Carbon monoxide alarm'), 9: t('Gas sensor'), 10: t('Water leak sensor'), 11: t('Temperature sensor'), 12: t('Vibration sensor'),
    13: t('Panic button'), 14: t('Key fob'), 15: t('Key switch')}[d.device_class] || (d.kind === 'sensor' ? t('Sensor') : t('Device'));
}
function zgGlyph(d) {
  const role = {siren: 'siren', keypad: 'key', keyfob: 'keyfob', repeater: 'plug', other: 'plug'}[d.kind];
  return role || {1: 'door', 2: 'window', 3: 'garage', 4: 'motion', 5: 'glass', 6: 'smoke', 7: 'smoke', 8: 'co', 9: 'smoke', 10: 'water',
    12: 'zap', 13: 'alert', 14: 'keyfob', 15: 'key'}[d.device_class] || 'radio';
}
// Its name: its own if renamed, else its zone's, else its model (zone names from zone-status).
function zgName(d, names = {}) {
  return d.name || (d.zones?.length && names[d.zones[0]]) || d.model || zgKindWord(d);
}
async function zgZoneNames() {
  const z = await api('zone-status').catch(() => ({zones: []}));
  return Object.fromEntries((z.zones || []).map(x => [x.number, x.name]));
}
function zgSignal(d) {
  return {good: t('Good'), fair: t('Fair'), weak: t('Weak')}[d.signal] || t('Not measured yet');
}
function zgPower(d) {
  if (d.power === 'mains') return d.router ? t('Mains · Extends the network') : t('Mains');
  if (d.power === 'battery') return d.battery !== undefined ? t('Battery {0} %', d.battery) : t('Battery');
  return t('Not known yet');
}
const zgJoinWords = j => j === 'install_code' ? t('With its install code') : j === 'standard_key' ? t('With the standard key') : t('Not known');
const zgAddr = hex => String(hex || '').replace(/(.{4})(?=.)/g, '$1 ');
function zgUses(d) {
  if (d.kind === 'siren') return t('Siren');
  if (d.kind === 'keypad') return t('Keypad');
  if (d.kind === 'keyfob') return t('Key fob');
  return d.zones.length ? tn('Zone {1}', 'Zones {1}', d.zones.length, d.zones.join(', ')) : t('Not in a zone');
}
// The one thing to do about a device, in amber under its name ('' when nothing).
function zgTodo(d) {
  if (d.unreachable) return d.heard_s !== undefined ? t('Not heard for {0}', fmtDuration(d.heard_s)) : t('Not heard since the panel started');
  if (d.state === 'wake_needed') return t('Wake it to finish setting it up');
  if (d.state === 'setup_stopped') return t('Its setup stopped: set it up again');
  if (d.state === 'setting_up' || d.state === 'joining') return t('Still setting up');
  if (d.state === 'not_usable') return t('Nothing on it the panel can use');
  if (d.low) return t('Battery low: change it soon');
  if (d.signal === 'weak') return t('Signal weak: move it closer or add a mains device between');
  return '';
}
// What the panel answered a Z-Wave network request (ui_zw_req_result_t; web_zwave.c's words), in
// words. full (ZW-039): the panel keeps the room its alarm settings need - Zigbee's words.
function zwRefusal(code) {
  return {not_allowed: t('Only the installer can do that, with every partition disarmed.'), busy: t('Another Z-Wave operation is running.'),
    no_radio: t('The Z-Wave radio isn’t working.'), invalid: t('Not now, or not that device.'), unsupported: t('This panel can’t do that yet.'),
    low_battery: t('Its battery is too low for an update: replace it first.'),
    full: t('No room for another device: the panel’s storage is full.')}[code] || code;
}
// What the panel answered a request (devices.md §14 results; ui_zb_req_result_t), in words.
function zgRefusal(code) {
  return {not_allowed: t('Only the installer can do that, with every partition disarmed.'), busy: t('Another Zigbee operation is running.'),
    no_radio: t('The Zigbee radio isn’t working.'), invalid: t('Not now, or not that device.'), unsupported: t('This panel can’t do that yet.'),
    bad_code: t('That isn’t an install code: 16, 20, 28 or 36 characters, 0 to 9 and A to F.'), checksum: t('It doesn’t check out: compare it with the label.'),
    bad_address: t('That isn’t a device address: 16 characters, 0 to 9 and A to F.'),
    full: t('No room: the panel has as many devices or install codes as it keeps, or its storage is full.'),
    not_found: t('That device isn’t in the network any more.'), in_use: t('Another key fob belongs to that person already.'),
    wrong_code: t('That isn’t the installer’s code.'), locked_out: t('Too many wrong codes: try again in a few minutes.')}[code] || code;
}
// How an operation ended (ui_zb_result_t), in words: [look, title, what it means].
function zgResultWords(g) {
  const zones = g.zones?.length ? tn('Zone {1} needs a new sensor: it shows CHECK until it has one.', 'Zones {1} need a new sensor: they show CHECK until they have one.',
    g.zones.length, g.zones.join(', ')) : '';
  switch (g.result) {
    case 'added': return ['good', t('Added'), t('Use it for a zone from its page.')];
    case 'nothing_joined': return ['warn', t('Nothing joined'), t('The 3 minutes ran out. Check the device’s pairing steps in its manual: some need a reset first. Then try again.')];
    case 'join_failed': return ['warn', t('Didn’t join'), t('It started joining but didn’t finish. Reset the device, keep it close to the panel and try again.')];
    case 'already_added': return ['info', t('Already added'), t('This device is already in the network.')];
    case 'not_a_sensor': return ['warn', t('Not a sensor'), t('It has nothing the panel can use as a zone. Kept, it still extends the network for battery sensors.')];
    case 'setup_not_finished': return ['warn', t('Setup not finished'), t('It joined, but its setup stopped before it finished. Open it and set it up again (wake it first if it sleeps).')];
    case 'stopped': return ['info', t('Stopped'), t('Devices still setting up go on.')];
    case 'removed': return ['good', t('Removed'), zones];
    case 'no_answer': return ['warn', t('It didn’t answer'), g.op === 'remove' ? t('It is still in the network. If it is gone or broken it can’t answer: forget it anyway?')
      : t('Check its power, or bring it closer to the panel.')];
    case 'forgotten': return ['good', t('Forgotten'), zones];
    case 'replaced': return ['good', t('Replaced'), g.zones?.length ? tn('The new device took Zone {1}.', 'The new device took Zones {1}.', g.zones.length, g.zones.join(', ')) : ''];
    case 'answered': return ['good', t('It answered'), ''];
    case 'done': return ['good', t('Done'), ''];
    case 'failed': return ['warn', t('It couldn’t be done'), t('The radio went away or the device refused. Try again.')];
    case 'full': return ['warn', t('No room for another device'), t('It was asked to leave. Remove a device the panel no longer needs first.')];
    default: return ['', t('Finished'), ''];
  }
}
function zgOpTitle(g, name) {
  return {add: t('Adding devices'), add_code: t('Adding with an install code'), replace: t('Replacing {0}', name), remove: t('Removing {0}', name),
    forget: t('Forgetting {0}', name), ping: t('Checking {0}', name), identify: t('Blinking {0}’s light', name), setup: t('Setting up {0} again', name),
    channel: t('Moving to another channel'), survey: t('Checking for noise'), reset: t('Resetting the Zigbee network'), restart: t('Restarting the radio'),
    siren_test: t('Testing {0}', name)}[g.op] || t('Working');
}
async function zgSend(body, accept = ['ok']) {
  const r = await api('zigbee', body);
  if (!accept.includes(r.result)) throw new Error(zgRefusal(r.result));
  return r;
}
// Why nothing can be changed just now, as a notice at the top of each page ('' when changes are possible).
function zgLockNotice(z) {
  if (z.may_change) return '';
  return notice('warn', t('Looking only while a partition is armed'), t('Adding, removing and changing Zigbee devices wait until every partition is disarmed.'));
}
const zgBack = () => ['zigbee', nt('Zigbee')];
const zgOff = on => on ? '' : 'disabled';
const zgRunning = g => !!g && g.op !== 'none' && g.step !== 'done';
const zgListening = g => ['add', 'add_code', 'replace'].includes(g.op);
function zgOff2(text) { return `<p class="muted-note">${esc(text)}</p>`; }
async function zgNotRunning(title) {
  return {html: pageHead(title, '', '', title === nt('Zigbee') ? null : zgBack()) + notice('info', t('Zigbee isn’t running on this panel'), '')};
}

// ---- Installer > Zigbee ---------------------------------------------------------------------
async function zgMainPage() {
  const z = await api('zigbee');
  if (!z.available) return zgNotRunning(nt('Zigbee'));
  const names = await zgZoneNames();
  const r = z.radio, [kind, word, detail, todo] = zgRadioWords(r);
  const g = z.progress, running = zgRunning(g), may = z.may_change, usable = zgUsable(r);
  const nameOf = id => { const d = z.devices.find(x => x.id === id); return d ? zgName(d, names) : g.model || t('The device'); };
  const radioCard = `<section class="card status-card ${kind}"><span class="state">${ht('Radio · {0}', word)}</span>
    <p>${esc(detail)}</p>${todo ? `<p>${esc(todo)}</p>` : ''}
    ${kv([[t('Devices'), String(r.devices)], [t('Not heard'), r.not_heard ? String(r.not_heard) : '']])}
    ${r.storage_full ? `<p>${ht('The panel’s storage is full: no new device or install code can be added until space is freed.')}</p>` : ''}
    ${!usable && r.state !== 'starting' ? zgOff2(t('Adding and removing wait until the radio works.')) : ''}
    <div class="card-foot"><button type="button" data-go="zigbee-radio">${ht('Radio details')}</button>
      ${r.state === 'paused' ? `<button type="button" class="primary" data-zb="retry" ${zgOff(may)}>${ht('Try again')}</button>`
        : `<button type="button" data-zb="restart" ${zgOff(may && !running && !['not_connected', 'socket_zwave'].includes(r.state))}>${ht('Restart the radio')}</button>`}</div></section>`;
  const op = zgOpCard(g, nameOf);
  const order = (a, b) => (b.unreachable - a.unreachable) || zgName(a, names).localeCompare(zgName(b, names));
  const rows = [...z.devices].sort(order).map(d => {
    const todo2 = zgTodo(d);
    return `<tr><td><div class="zb-dev"><span class="zb-glyph ${todo2 ? 'warn' : ''}">${icon(zgGlyph(d))}</span>
        <div><a href="#/zigbee-device/${d.id}">${esc(zgName(d, names))}</a><span class="sub">${esc(d.model || zgKindWord(d))}</span>
        ${todo2 ? `<span class="zb-warn">${esc(todo2)}</span>` : ''}</div></div></td>
      <td class="zb-wide">${esc(zgPower(d))}</td><td>${d.unreachable ? tag(t('Not heard'), 'warn') : esc(zgSignal(d))}</td>
      <td class="zb-wide">${esc(zwAgo(d.heard_s))}</td>
      <td class="zb-wide">${d.join === 'install_code' ? ht('Install code') : d.join === 'standard_key' ? ht('Standard key') : ''}</td>
      <td>${esc(zgUses(d))}</td></tr>`;
  }).join('');
  const can = may && usable && !running;
  const codes = z.codes.map(c => `<tr><td><span class="mono">${esc(zgAddr(c.ieee))}</span></td>
    <td>${c.added ? esc(fmtAgo(c.added * 1000)) : ''}</td>
    <td class="actions"><button type="button" class="danger" data-zbcode="${esc(c.ieee)}" ${zgOff(may)}>${ht('Forget')}</button></td></tr>`).join('');
  return {
    html: pageHead(nt('Zigbee'), t('Sensors, sirens and keypads on the panel’s Zigbee radio.')) + zgLockNotice(z) + radioCard +
      `<div id="zb-op-slot">${op.html}</div>
      <section class="section"><h2>${ht('Devices')}</h2>
        <div class="card-foot left"><button type="button" class="primary" data-zb="add" ${zgOff(can)}>${ht('Add a device')}</button>
          <button type="button" data-go="zigbee-code" ${zgOff(may)}>${ht('Add with an install code')}</button>
          <button type="button" data-zb="remove-pick" ${zgOff(can && z.devices.length)}>${ht('Remove a device')}</button></div>
        ${rows ? `<div class="card table-wrap"><table class="zb-table"><thead><tr><th>${ht('Device')}</th><th class="zb-wide">${ht('Power')}</th>
          <th>${ht('Signal')}</th><th class="zb-wide">${ht('Last heard')}</th><th class="zb-wide">${ht('How it joined')}</th><th>${ht('Used by')}</th></tr></thead>
          <tbody>${rows}</tbody></table></div>`
          : `<p class="muted-note">${ht('No devices yet. Add one: most sensors join when you hold their button until their light blinks.')}</p>`}</section>
      <section class="section"><h2>${ht('Install codes waiting')}</h2>
        <p class="muted-note">${ht('A device with its code here joins with its own key when the panel listens for new devices. Joined, it leaves this list.')}</p>
        ${codes ? `<div class="card table-wrap"><table><thead><tr><th>${ht('Address')}</th><th>${ht('Added')}</th><th></th></tr></thead>
          <tbody>${codes}</tbody></table></div>` : `<p class="muted-note">${ht('No install codes are waiting.')}</p>`}</section>
      <section class="section"><h2>${ht('Network')}</h2>
        <div class="card"><div class="rows">
          <div class="row"><div><span class="label">${ht('Channel')}</span><span class="help">${r.channel ? ht('Where the Zigbee network talks: now {0}.', r.channel) : ht('No network yet.')}</span></div>
            <div class="control"><button type="button" data-go="zigbee-channel">${ht('Change the channel')}</button></div></div>
          <div class="row"><div><span class="label">${ht('Reset the Zigbee network')}</span>
            <span class="help">${z.devices.length ? ht('Every device must be added again') : ht('A new network, with new keys')}</span></div>
            <div class="control"><button type="button" class="danger" data-zb="reset" ${zgOff(may && !running && usable)}>${ht('Reset the network')}</button></div></div>
        </div></div></section>`,
    bind() {
      let drawn = op.key;
      const bindOp = () => $$('#zb-op-slot [data-zbop]').forEach(button => button.onclick = async () => {
        const action = button.dataset.zbop, id = Number(button.dataset.id);
        if (action === 'open') return go('zigbee-add');
        if (action === 'forget') return zgForget({id, name: button.dataset.name, zones: (button.dataset.zones || '').split(',').filter(Boolean)});
        if (action === 'again') return zgRemoveAsk({id, name: button.dataset.name, zones: (button.dataset.zones || '').split(',').filter(Boolean)});
        button.disabled = true;
        try { await zgSend({op: action}); await show(); } catch (error) { toast(error.message, 'error'); button.disabled = false; }
      });
      bindOp();
      $$('[data-zb]').forEach(button => button.onclick = async () => {
        const action = button.dataset.zb;
        if (action === 'remove-pick') return zgRemovePick(z.devices, names);
        if (action === 'restart') return review({title: t('Restart the Zigbee radio?'), intro: t('Takes a few seconds; zones keep their state.'),
          confirm: t('Restart'), done: t('Restarting.'), run: () => zgSend({op: action})});
        // ZB-017: a reset asks twice, and for the installer's password again (never a keypad code in a browser).
        if (action === 'reset') return zgResetAsk(z.devices.length);
        button.disabled = true;
        try {
          await zgSend({op: action});
          if (action === 'add') return go('zigbee-add');
          if (action === 'retry') toast(t('Starting the radio again.'));
          await show();
        } catch (error) { toast(error.message, 'error'); button.disabled = false; }
      });
      $$('[data-zbcode]').forEach(button => button.onclick = () => review({title: t('Forget this install code?'),
        intro: t('{0} can then join only with the standard key.', zgAddr(button.dataset.zbcode)), confirm: t('Forget'), danger: true, done: t('Forgotten.'),
        run: () => zgSend({op: 'code_forget', ieee: button.dataset.zbcode})}));
      // While an operation runs, its card follows it every second; the rest of the page when it ends.
      if (running) poll(async () => {
        if ($('#dialog').open) return;
        const now = await api('zigbee');
        if (!zgRunning(now.progress)) { await show(); return; }
        const card = zgOpCard(now.progress, nameOf);
        if (card.key === drawn) {
          const left = $('#zb-op .zb-left');
          if (left && zgListening(now.progress)) left.textContent = t('Listening · {0} left', fmtDuration(now.progress.seconds));
          return;
        }
        drawn = card.key;
        $('#zb-op-slot').innerHTML = card.html;
        bindOp();
      }, 1000);
    },
  };
}

// The card that follows the operation running or just ended (adding has its own page too).
function zgOpCard(g, nameOf) {
  if (!g || g.op === 'none') return {key: '', html: ''};
  const name = g.op === 'replace' ? nameOf(g.old_id) : nameOf(g.id);
  const b = (action, words, cls = '', data = '') => `<button type="button" class="${cls}" data-zbop="${action}" ${data}>${esc(words)}</button>`;
  const foot = (...list) => `<div class="card-foot">${list.join('')}</div>`;
  if (zgListening(g) && g.step !== 'done') {
    const n = g.joined.length;
    return {key: `add:${g.op}:${g.step}:${n}`, html: `<section class="card pad" id="zb-op"><h2>${esc(zgOpTitle(g, name))}</h2>
      <p class="zb-left">${ht('Listening · {0} left', fmtDuration(g.seconds))}</p>${n ? `<p>${esc(tn('{0} device joined so far.', '{0} devices joined so far.', n))}</p>` : ''}
      ${foot(b('open', t('Open'), 'primary'), b('stop', t('Stop')))}</section>`};
  }
  if (g.step !== 'done') return {key: `op:${g.op}:${g.step}`, html: `<section class="card pad" id="zb-op"><h2>${esc(zgOpTitle(g, name))}</h2>
    <p>${g.op === 'remove' ? ht('Asked it to leave. A sleeping sensor answers when it wakes: press its button now.') : ht('Working…')}</p></section>`};
  const [look, title, words] = zgResultWords(g);
  const data = `data-id="${g.id}" data-name="${esc(name)}" data-zones="${esc(g.zones.join(','))}"`;
  const acts = g.op === 'remove' && g.result === 'no_answer'
    ? foot(b('dismiss', t('Keep it')), b('again', t('Try again'), '', data), b('forget', t('Forget it anyway'), 'danger', data))
    : zgListening(g) ? foot(b('open', t('See the results'), 'primary'), b('dismiss', t('OK'))) : foot(b('dismiss', t('OK'), 'primary'));
  return {key: `done:${g.op}:${g.result}`, html: `<section class="card status-card ${look}" id="zb-op"><span class="state">${esc(t('{0}: {1}', zgOpTitle(g, name), title))}</span>
    ${words ? `<p>${esc(words)}</p>` : ''}${acts}</section>`};
}
function zgRemovePick(devices, names) {
  editDialog({title: t('Remove a device'), intro: t('It is asked to leave the network.'),
    specs: [{name: 'id', label: t('Device'), type: 'select', options: [['', t('Choose a device')], ...devices.map(d => [String(d.id), `${zgName(d, names)} · ${zgUses(d)}`])]}],
    values: {id: ''},
    submit: result => {
      const d = devices.find(x => String(x.id) === result.values.id);
      if (!d) throw new Error(t('Choose a device.'));
      zgRemoveAsk({id: d.id, name: zgName(d, names), zones: d.zones});
    }});
}
function zgRemoveAsk(d) {
  const zones = d.zones || [];
  review({title: t('Remove {0}?', d.name), confirm: t('Remove'), danger: true, done: t('Asked it to leave.'),
    intro: zones.length ? tn('It is asked to leave the network. Zone {1} keeps its settings and needs a new sensor.',
      'It is asked to leave the network. Zones {1} keep their settings and need a new sensor.', zones.length, zones.join(', '))
      : t('It is asked to leave the network.'),
    run: () => zgSend({op: 'remove', id: d.id}), after: () => go('zigbee')});
}
function zgForget(d) {
  const list = d.zones || [];
  review({title: t('Forget {0}?', d.name), confirm: t('Forget it'), danger: true, done: t('Forgotten.'),
    intro: list.length ? tn('Only when it’s gone or broken: it isn’t asked to leave. Zone {1} keeps its settings and needs a new sensor.',
      'Only when it’s gone or broken: it isn’t asked to leave. Zones {1} keep their settings and need a new sensor.', list.length, list.join(', '))
      : t('Only when it’s gone or broken: it isn’t asked to leave. If it ever comes back it has to be added again.'),
    run: () => zgSend({op: 'forget', id: d.id}), after: () => go('zigbee')});
}
// The network reset (ZB-017): asked twice, then the installer's password (when not typed in the last 10 minutes).
function zgResetAsk(count) {
  review({title: t('Reset the Zigbee network?'), danger: true, confirm: t('Continue'), done: '',
    intro: count ? tn('The {0} device loses the panel. It must be reset and added again, and its zones show CHECK until then.',
      'All {0} devices lose the panel. Each must be reset and added again, and their zones show CHECK until then.', count)
      : t('A new network, with new keys.'),
    run: () => {},
    after: () => review({title: t('Reset it now?'), intro: t('This can’t be undone. Install codes waiting are kept.'), password: true, danger: true,
      confirm: t('Reset the network'), done: t('Resetting the Zigbee network.'),
      run: pw => zgSend({op: 'reset', ...withPassword(pw)}), after: () => go('zigbee')})});
}

// ---- Add a Zigbee device (also with an install code, and replacing one) -----------------------
function zgJoinedRow(j, done) {
  const steps = {joining: t('Joining'), reading: t('Reading what it can do'), setting_up: t('Setting it up'), working: t('Setting it up'), starting: t('Joining')};
  const [look, words] = j.step === 'done' ? zgResultWords({...j, op: 'add', zones: []}) : ['info', steps[j.step] || t('Joining')];
  const title = j.model || t('A device');
  let more = '', act = '';
  if (j.asleep && j.step !== 'done') more = `<span class="zb-warn">${ht('It went to sleep before it finished')}</span>
    <p class="zb-wake">${ht('Press its button to wake it, then again every few seconds until it says Ready.')}</p>`;
  if (j.step === 'done' && ['added', 'already_added', 'setup_not_finished', 'not_a_sensor'].includes(j.result)) {
    act = `<button type="button" data-go="zigbee-device" data-arg="${j.id}">${ht('Open it')}</button>`;
    if (j.result === 'added') more = `<span class="sub">${ht('Use it for a zone from its page: tamper and low battery come with the zone.')}</span>`;
  }
  if (done && j.step !== 'done') more = `<span class="sub">${ht('It joined, but the panel hasn’t finished setting it up. It finishes when it next wakes.')}</span>`;
  return `<div class="zb-joined"><div class="zb-dev"><span class="zb-glyph ${look === 'good' ? '' : look || 'muted'}">${icon(j.step === 'done' && look === 'good' ? 'check' : look === 'warn' ? 'alert' : 'radio')}</span>
    <div><strong>${esc(title)}</strong><span class="sub">${esc(words)}</span>${more}</div></div>
    <div class="zb-do">${act}</div></div>`;
}
async function zgAddPage() {
  const z = await api('zigbee');
  if (!z.available) return zgNotRunning(t('Add a Zigbee device'));
  const g = z.progress, may = z.may_change, adding = zgListening(g), done = adding && g.step === 'done';
  const old = g.op === 'replace' ? z.devices.find(d => d.id === g.old_id) : null;
  const title = g.op === 'replace' ? t('Replace {0}', old ? zgName(old) : g.model || t('a device')) : g.op === 'add_code' ? t('Add with an install code') : t('Add a Zigbee device');
  const head = pageHead(title, g.op === 'replace' ? t('The new device takes its zones; the old one is removed once the new one is ready.')
    : g.op === 'add_code' ? t('Only the device with that install code may join.') : t('Any Zigbee 3.0 device, for 3 minutes.'), '', zgBack());
  if (!adding) {
    const usable = zgUsable(z.radio);
    return {html: head + zgLockNotice(z) + `<section class="card pad"><h2>${ht('Not listening')}</h2>
      <p>${ht('The panel listens for 3 minutes. Any Zigbee 3.0 device may join; those with a code under Install codes waiting join with their own key.')}</p>
      ${usable ? '' : zgOff2(t('The Zigbee radio isn’t working: see the Zigbee page.'))}
      <div class="card-foot"><button type="button" class="primary" id="zb-start" ${zgOff(may && usable && !zgRunning(g))}>${ht('Start listening')}</button></div></section>`,
      bind() {
        $('#zb-start').onclick = async () => { try { await zgSend({op: 'add'}); await show(); } catch (error) { toast(error.message, 'error'); } };
      }};
  }
  const rows = g.joined.map(j => zgJoinedRow(j, done)).join('');
  const n = g.joined.length;
  let body;
  if (done) {
    const [look, word, words] = zgResultWords(g);
    body = `<section class="card status-card ${look}"><span class="state">${esc(word)}</span>
      <p>${n ? esc(tn('{0} device joined.', '{0} devices joined.', n)) : esc(words)}</p>
      <div class="card-foot"><button type="button" data-zbadd="dismiss">${ht('Done')}</button>
        <button type="button" class="primary" data-zbadd="add" ${zgOff(may && zgUsable(z.radio))}>${ht('Add another')}</button></div></section>`;
  } else {
    body = `<section class="card pad"><p>${g.op === 'add_code' ? ht('Power it up or put it in pairing mode now.')
      : ht('Put the device in pairing mode now: most sensors, hold their button until their light blinks. Keep it close to the panel.')}</p>
      <p class="zb-left" id="zb-left" role="status">${ht('Listening · {0} left', fmtDuration(g.seconds))}</p>
      ${n ? `<p>${esc(tn('{0} device joined. More can join until the time runs out.', '{0} devices joined. More can join until the time runs out.', n))}</p>` : ''}
      <div class="card-foot"><button type="button" data-zbadd="stop">${ht('Stop')}</button>
        <button type="button" data-zbadd="keep" ${zgOff(may)}>${ht('Keep listening')}</button></div>
      <p class="muted-note">${ht('The panel keeps listening if you leave this page, until the time runs out or you stop it.')}</p></section>`;
  }
  if (rows) body += `<section class="section"><h2>${ht('Joined')}</h2><div class="card">${rows}</div></section>`;
  const key = p => JSON.stringify([p.op, p.step, p.joined.map(j => [j.id, j.step, j.result, j.asleep])]);
  const seen = key(g);
  return {
    html: head + zgLockNotice(z) + body,
    bind() {
      $$('[data-zbadd]').forEach(button => button.onclick = async () => {
        const action = button.dataset.zbadd;
        button.disabled = true;
        try {
          if (action === 'dismiss') { await zgSend({op: action}); return go('zigbee'); }
          if (action === 'add') await zgSend({op: 'dismiss'});
          await zgSend({op: action});
          await show();
        } catch (error) { toast(error.message, 'error'); button.disabled = false; }
      });
      // Once a second while listening: the time left changes in place; the rest only when a device moved on.
      if (!done) poll(async () => {
        if ($('#dialog').open) return;
        const now = (await api('zigbee')).progress;
        if (key(now) !== seen) { await show(); return; }
        $('#zb-left').textContent = t('Listening · {0} left', fmtDuration(now.seconds));
      }, 1000);
    },
  };
}

// ---- Add with an install code --------------------------------------------------------------------
// The CRC an install code ends with: CRC-16/X.25 of the rest, low byte first (the panel checks it again).
function zgCrcOk(hex) {
  const bytes = hex.match(/../g).map(h => parseInt(h, 16)), body = bytes.slice(0, -2);
  let c = 0xFFFF;
  for (const b of body) { c ^= b; for (let i = 0; i < 8; i++) c = (c & 1) ? (c >>> 1) ^ 0x8408 : c >>> 1; }
  return ((c ^ 0xFFFF) & 0xFFFF) === (bytes[bytes.length - 2] | (bytes[bytes.length - 1] << 8));
}
function zgCheckAddress(text) {
  const hex = text.replace(/[\s:-]/g, '').toUpperCase();
  if (!hex) return ['', ''];
  if (!/^[0-9A-F]+$/.test(hex)) return ['warn', t('Only 0 to 9 and A to F')];
  if (hex.length === 16) return /^0+$/.test(hex) ? ['warn', t('That isn’t a device address')] : ['good', t('All 16 characters')];
  return hex.length > 16 ? ['warn', t('Too long: an address has 16 characters')] : ['', t('{0} of 16 characters', hex.length)];
}
function zgCheckCode(text) {
  const hex = text.replace(/[\s:-]/g, '').toUpperCase();
  if (!hex) return ['', ''];
  if (!/^[0-9A-F]+$/.test(hex)) return ['warn', t('Only 0 to 9 and A to F')];
  if (hex.length > 36) return ['warn', t('Too long: an install code has at most 36 characters')];
  const next = [16, 20, 28, 36].find(n => n >= hex.length);
  if (hex.length !== next) return ['', t('{0} of {1} characters', hex.length, next)];
  if (zgCrcOk(hex)) return ['good', t('Its check matches: the code is right')];
  return ['warn', hex.length === 36 ? t('It doesn’t check out: compare it with the label')
    : t('{0} characters: its check doesn’t match yet. Keep typing, or compare it with the label.', hex.length)];
}
// A QR code's address and install code ("Z:<address>$I:<code>", "...$A:<address>$I:<code>", "<address> <code>"):
// the browser only fills the boxes; the panel reads the code again.
function zgReadQr(raw) {
  const s = String(raw || '').trim();
  const m = /(?:^|\$)A:([0-9A-Fa-f]{16})\$I:([0-9A-Fa-f]+)/.exec(s) || /^Z:([0-9A-Fa-f]{16})\$I:([0-9A-Fa-f]+)/.exec(s)
    || /^([0-9A-Fa-f]{16})[\s|]+([0-9A-Fa-f]+)$/.exec(s);
  return m ? {address: m[1].toUpperCase(), code: m[2].toUpperCase()} : null;
}
async function zgScan(onCode) {
  if (typeof window.BarcodeDetector !== 'function' || !navigator.mediaDevices?.getUserMedia)
    return toast(t('This browser can’t read QR codes: type the address and code instead.'), 'error');
  let stream;
  try { stream = await navigator.mediaDevices.getUserMedia({video: {facingMode: 'environment'}, audio: false}); } catch {
    return toast(t('The camera is not available. Allow it for this page, or type the code.'), 'error');
  }
  openDialog(`<div class="dialog-head"><h2 id="dialog-title">${ht('Scan its QR code')}</h2>
    <p>${ht('Hold the code on its label or box inside the frame.')}</p></div>
    <div class="dialog-body"><video id="zb-video" class="scan-video" playsinline muted></video><p class="dialog-error" id="zb-scan-error" role="alert"></p></div>
    <div class="dialog-foot"><button type="button" id="zb-scan-cancel">${ht('Cancel')}</button></div>`);
  let stopped = false;
  const stop = () => { stopped = true; stream.getTracks().forEach(x => x.stop()); };
  $('#zb-scan-cancel').onclick = () => { stop(); closeDialog(); };
  $('#dialog').addEventListener('close', stop, {once: true});
  const video = $('#zb-video');
  video.srcObject = stream;
  try { await video.play(); } catch { /* shown once it can */ }
  const detector = new BarcodeDetector({formats: ['qr_code']});
  const look = async () => {
    if (stopped) return;
    try {
      const codes = await detector.detect(video);
      const found = codes.map(c => zgReadQr(c.rawValue)).find(Boolean);
      if (found) { stop(); closeDialog(); return onCode(found); }
      if (codes.length) $('#zb-scan-error').textContent = t('That QR code has no Zigbee install code: type it from the label instead.');
    } catch { /* the next frame */ }
    setTimeout(look, 300);
  };
  look();
}
async function zgCodePage() {
  const z = await api('zigbee');
  if (!z.available) return zgNotRunning(t('Add with an install code'));
  const may = z.may_change, usable = zgUsable(z.radio), running = zgRunning(z.progress);
  const line = id => `<p class="zb-check" id="${id}" role="status"></p>`;
  return {
    html: pageHead(t('Add with an install code'), t('For a device whose label has one.'), '', zgBack()) + zgLockNotice(z) +
      `<form class="card" id="zb-code-form" novalidate><fieldset ${zgOff(may)}><div class="rows">
        <div class="row"><div><label class="label" for="zb-addr">${ht('Its address')}</label>
          <span class="help">${ht('16 characters on its label, often after MAC or IEEE.')}</span></div>
          <div class="control stack"><input id="zb-addr" class="mono" maxlength="23" autocomplete="off" spellcheck="false" autocapitalize="characters">
            ${line('zb-addr-check')}</div></div>
        <div class="row"><div><label class="label" for="zb-ic">${ht('Its install code')}</label>
          <span class="help">${ht('On its label or box, often beside a QR code: 16, 20, 28 or 36 characters.')}</span></div>
          <div class="control stack"><textarea id="zb-ic" class="mono" rows="2" maxlength="60" autocomplete="off" spellcheck="false" autocapitalize="characters"></textarea>
            ${line('zb-ic-check')}</div></div>
      </div></fieldset>
      <p class="dialog-error" id="zb-code-error" role="alert"></p>
      <div class="card-foot"><span class="note">${ht('Only this device may join while the panel listens. It joins with its own key, not the standard one.')}</span>
        <button type="button" id="zb-scan" ${zgOff(may)}>${ht('Scan with the camera')}</button>
        <button type="button" id="zb-keep" ${zgOff(may)}>${ht('Keep it waiting')}</button>
        <button type="submit" class="primary" id="zb-listen" ${zgOff(may && usable && !running)}>${ht('Listen for this device')}</button></div></form>
      <p class="muted-note">${ht('Kept waiting, it joins with its own key whenever the panel listens for new devices. The code is never shown again.')}</p>`,
    bind() {
      const check = () => {
        const a = zgCheckAddress($('#zb-addr').value), c = zgCheckCode($('#zb-ic').value);
        $('#zb-addr-check').className = `zb-check ${a[0]}`; $('#zb-addr-check').textContent = a[1];
        $('#zb-ic-check').className = `zb-check ${c[0]}`; $('#zb-ic-check').textContent = c[1];
        $('#zb-code-error').textContent = '';
        return a[0] === 'good' && c[0] === 'good';
      };
      $('#zb-addr').oninput = check;
      $('#zb-ic').oninput = check;
      $('#zb-scan').onclick = () => zgScan(found => { $('#zb-addr').value = zgAddr(found.address); $('#zb-ic').value = zgAddr(found.code); check(); });
      const send = async op => {
        if (!check()) { $('#zb-code-error').textContent = t('Type the address and the install code from the label first.'); return; }
        try {
          await zgSend({op, ieee: $('#zb-addr').value, code: $('#zb-ic').value});
          $('#zb-ic').value = '';
          if (op === 'add_code') return go('zigbee-add');
          toast(t('Kept: it joins with its own key when the panel listens.'));
          go('zigbee');
        } catch (error) { $('#zb-code-error').textContent = error.message; }
      };
      $('#zb-keep').onclick = () => send('code_add');
      $('#zb-code-form').onsubmit = event => { event.preventDefault(); send('add_code'); };
    },
  };
}

// ---- A device's page --------------------------------------------------------------------------
const zgVolumes = () => [[0, t('Low')], [1, t('Medium')], [2, t('High')], [3, t('Very high')]];
// A siren's supervision is never off (zbz_wd.h): the default, or a longer silence before its trouble.
function zgSupervision(now) {
  const list = [[0, t('The default: 90 minutes on mains, 3 hours on battery')], [60, t('After 1 hour')], [240, t('After 4 hours')], [1440, t('After 24 hours')]];
  if (!list.some(o => o[0] === now)) list.push([now, tn('After {0} minute', 'After {0} minutes', now)]);
  return list;
}
function zgParts(off) {
  return [...(off ? [[0, off]] : []), ...(state.partitions || []).map(p => [p.number, p.name || tc('default partition name', 'Partition {0}', p.number)])];
}
// Who may own a key fob (ZB-024, alarm-devices.md §3): a master, partition master, standard, arm-only
// or guest user (authority 2..6) - never the installer, a duress code or an empty slot.
const zgFobOwner = u => Number.isInteger(u.authority) && u.authority >= 2 && u.authority <= 6;
// The model table's words for waking a device (zbz_models.c, context "wake up", as the touchscreen
// looks them up): English as they come; another language takes their translation once the table
// has one, else the page's own sentence - never an English clause in a translated page.
// The model table's wake words are a clause ('press its button once'): on their own they read
// as a sentence, with a capital and a full stop.
const zgSentence = s => {
  const w = String(s).trim();
  if (!w) return w;
  const c = w.charAt(0).toUpperCase() + w.slice(1);
  return /[.!?]$/.test(c) ? c : c + '.';
};
function zgWakeWords(d) {
  const own = t('Press its button to wake it.');
  if (!d.wake_words) return own;
  if (!words.entries) return zgSentence(d.wake_words);
  const found = lookup('wake up', d.wake_words);
  return typeof found === 'string' ? zgSentence(found) : own;
}
// A channel's noise in words, as the touchscreen says it (channel_noise in ui_zigbee.c).
const zgNoiseWords = n => n === undefined ? tc('channel noise', 'Not checked yet') : n >= -75 ? tc('channel noise', 'Busy')
  : n >= -85 ? tc('channel noise', 'Some noise') : tc('channel noise', 'Quiet');
function zgRoleSpecs(d, users) {
  if (d.siren) return ['siren', [
    {name: 'partitions', label: t('Partitions'), help: t('The alarms of these partitions sound it. None: the panel doesn’t use it.'), type: 'bits', options: partitionBits()},
    {name: 'burglary', label: t('Burglary and audible panic'), type: 'switch'},
    {name: 'fire', label: t('Fire and carbon monoxide'), help: t('With its fire sound for fire, its emergency sound for CO'), type: 'switch'},
    {name: 'strobe', label: t('Strobe with it'), help: t('Its own light, while it sounds'), type: 'switch'},
    {name: 'squawk', label: t('Chirp when arming and disarming'), help: t('One chirp to arm, two to disarm'), type: 'switch'},
    {name: 'volume', label: t('Volume'), type: 'select', numeric: true, options: zgVolumes()},
    {name: 'supervision_min', label: t('Supervision'), help: t('A siren trouble when it isn’t heard this long'), type: 'select', numeric: true, options: zgSupervision(d.siren.supervision_min)},
  ]];
  if (d.keypad) return ['keypad', [
    {name: 'partition', label: t('Partition'), help: t('The one it arms and disarms'), type: 'select', numeric: true, options: zgParts(t('Off: the panel doesn’t use it'))},
    {name: 'show_state', label: t('Show the panel’s state'), help: t('Its lights and sounds follow the panel'), type: 'switch'},
    {name: 'panic_mask', label: t('Its emergency buttons'), help: t('The alarms its buttons may raise'), type: 'bits',
      options: [[1, t('Panic alarm, audible')], [4, t('Fire alarm')], [8, t('Medical alarm')]]},
  ]];
  if (d.fob) return ['fob', [
    {name: 'user', label: t('Belongs to'), help: t('Its presses are logged with this name'), type: 'select', numeric: true,
      options: [[0, t('Nobody: the panel doesn’t use it')], ...users.filter(zgFobOwner).map(u => [u.number, u.name])]},
    {name: 'partition', label: t('Arms'), type: 'select', numeric: true, options: zgParts('')},
    {name: 'may_disarm', label: t('May disarm'), help: t('Off: it only arms, so a lost fob can’t open the house'), type: 'switch'},
    {name: 'panic', label: t('Panic button'), help: t('Sounds a panic alarm'), type: 'switch'},
  ]];
  return ['', []];
}
async function zgDevicePage(arg) {
  const id = Number(arg);
  let z;
  try {
    if (!Number.isInteger(id) || id < 1 || id > 65535) throw Object.assign(new Error(''), {status: 404});
    z = await api('zigbee?device=' + id);
  } catch (error) {
    if (error.status !== 404) throw error;
    return {html: pageHead(t('Zigbee device'), '', '', zgBack()) + notice('info', t('That device isn’t in the network any more.'),
      t('It may have been removed or forgotten. The Zigbee page lists the devices the panel has.'))};
  }
  if (!z.available) return zgNotRunning(t('Zigbee device'));
  const names = await zgZoneNames();
  const d = z.device, may = z.may_change, usable = zgUsable(z.radio), name = zgName(d, names);
  const ready = d.state === 'ready';
  const users = d.fob ? ((await api('people').catch(() => ({users: []}))).users || []).filter(zgFobOwner) : [];
  const notices = [
    d.unreachable ? notice('warn', d.heard_s !== undefined ? t('Not heard for {0}', fmtDuration(d.heard_s)) : t('Not heard since the panel started'),
      t('Change its battery or bring it closer, then press its button to wake it.')) : '',
    !ready ? notice('warn', t('Setup not finished · step {0} of {1}', d.setup_done, d.setup_total), zgWakeWords(d)) : '',
    d.low && !d.unreachable ? notice('warn', t('Battery low'), t('Change it soon.')) : '',
    d.signal === 'weak' && !d.unreachable ? notice('warn', t('Signal weak'), t('Move it closer, or add a mains device between it and the panel.')) : '',
    !usable ? notice('info', t('The Zigbee radio isn’t working'), t('Its facts are as last heard. Checking, setting up and removing wait until the radio works.')) : '',
  ].join('');
  const [role, specs] = zgRoleSpecs(d, users);
  const settings = role ? d[role] : null;
  const fit = settings?.fit;
  const roleTitle = {siren: t('What it sounds for, and how'), keypad: t('Arming'), fob: t('Who uses it, and what it may do')}[role] || '';
  const roleNote = {keypad: d.keypad?.device ? t('Codes typed on it are checked by the panel as device {0}. Five wrong codes lock it out for a while.', d.keypad.device)
      : t('Codes typed on it are checked by the panel. Five wrong codes lock it out for a while.'),
    fob: t('Disarming from a fob takes no code: turn it on only for a fob its owner keeps with their keys.'),
    siren: t('Its troubles, its silence included, are the panel’s siren trouble.')}[role] || '';
  const roleWhy = {not_ready: t('Finish its setup first.'), no_device_slot: t('Every device number a Zigbee keypad may have is taken: free one in the device table first.')}[fit];
  const roleCard = role ? `<section class="section">${roleWhy ? notice('info', roleTitle, roleWhy) : settingsCard({id: 'zb-settings', title: roleTitle, specs, values: settings,
      readOnly: !may, description: roleNote})}
    ${role === 'siren' ? `<div class="card-foot left"><button type="button" data-zbd="siren_test" ${zgOff(may && usable && !d.unreachable && fit === 'available')}>${ht('Test it')}</button>
      <span class="muted-note">${d.siren.last_test !== 'none' ? esc(t('Last test: {0}', zgResultWords({result: d.siren.last_test})[1])) : ht('A short sound at low volume. Its strobe is its own light: nothing on the panel’s screen flashes.')}</span></div>` : ''}</section>` : '';
  const facts = kv([[t('Name'), name], [t('Model'), d.model], [t('Maker and model as it reports them'), t('{0} · {1}', d.manufacturer || '–', d.model_id || '–')],
    [t('What it is'), zgKindWord(d)], [t('Power'), zgPower(d)], [t('Signal'), d.unreachable ? t('{0}, when last heard', zgSignal(d)) : zgSignal(d)],
    [t('Last heard'), zwAgo(d.heard_s)], [t('How it joined'), zgJoinWords(d.join)], [t('Joined'), d.joined_at ? fmtAgo(d.joined_at * 1000) : ''],
    [t('Software'), d.sw_build], [t('Address'), zgAddr(d.ieee)], [t('Note'), d.note]]);
  const inputs = d.inputs.map(x => `<div class="row"><div><span class="label">${esc(x.words)}</span>
    <span class="help">${x.zone ? (names[x.zone] ? ht('Zone {0} · {1}', x.zone, names[x.zone]) : esc(tn('Zone {1}', 'Zones {1}', 1, x.zone))) : ht('Not in a zone')}${x.state ? ' · ' + esc(x.state) : ''}</span></div>
    <div class="control">${x.zone ? `<button type="button" data-go="zone-setup" data-arg="${x.zone}">${ht('Open the zone')}</button>`
      : `<button type="button" data-go="zone-setup" data-arg="new" ${zgOff(may && ready)}>${ht('Make it a zone')}</button>`}</div></div>`).join('');
  const usedBy = role || !d.inputs.length ? '' : `<section class="section"><h2>${ht('Used by')}</h2><div class="card"><div class="rows">${inputs}</div></div>
    ${ready ? '' : zgOff2(t('Finish its setup first: a zone of it then hears all it can tell.'))}</section>`;
  const row = (label, help, button) => `<div class="row"><div><span class="label">${esc(label)}</span><span class="help">${esc(help)}</span></div><div class="control">${button}</div></div>`;
  const live = usable && !zgRunning(z.progress);
  const checks = `<section class="section"><h2>${ht('Check it')}</h2><div class="card"><div class="rows">
    ${d.can_identify ? row(t('Blink its light'), t('Find which device this is: its own light blinks for a few seconds'), `<button type="button" data-zbd="identify" ${zgOff(live)}>${ht('Blink')}</button>`) : ''}
    ${row(t('Check it now'), d.sleepy ? t('A sleeping device answers when it next wakes') : t('Asks it to answer'), `<button type="button" data-zbd="ping" ${zgOff(live)}>${ht('Check')}</button>`)}
    ${row(t('Set it up again'), t('Reads what it can do and how it reports'), `<button type="button" data-zbd="setup" ${zgOff(live && may)}>${ht('Set it up')}</button>`)}
  </div></div></section>`;
  const removing = `<section class="section"><h2>${ht('Removing it')}</h2><div class="card"><div class="rows">
    ${row(t('Replace it'), d.zones.length ? t('A new device takes its zones') : t('A new device takes its place'), `<button type="button" data-zbd="replace" ${zgOff(live && may)}>${ht('Replace')}</button>`)}
    ${row(t('Remove this device'), t('Asks it to leave the network'), `<button type="button" class="danger" data-zbd="remove" ${zgOff(live && may)}>${ht('Remove')}</button>`)}
    ${row(t('Forget it: it’s gone or broken'), t('Removed without its answer'), `<button type="button" class="danger" data-zbd="forget" ${zgOff(may && !zgRunning(z.progress))}>${ht('Forget')}</button>`)}
  </div></div></section>`;
  // A device not heard leads with what can still be done about it.
  const sections = d.unreachable ? removing + usedBy + roleCard + checks : roleCard + usedBy + checks + removing;
  return {
    html: pageHead(name, t('{0} · {1}', d.model || zgKindWord(d), zgUses(d)), '', zgBack()) + zgLockNotice(z) + notices + sections +
      `<section class="section"><h2>${ht('This device')}</h2><div class="card pad">${facts}
        <div class="card-foot"><button type="button" data-zbd="rename" ${zgOff(may)}>${ht('Rename')}</button></div></div></section>`,
    bind() {
      if (role && !roleWhy) bindSettingsCard('zb-settings', specs, settings, result => review({title: t('Save {0}’s settings?', name), changes: result.changes,
        run: () => {
          const all = Object.fromEntries(specs.map(s => [s.name, result.values[s.name] ?? settings[s.name]]));
          return zgSend({op: role, id: d.id, [role]: all});
        }}));
      $$('[data-zbd]').forEach(button => button.onclick = async () => {
        const action = button.dataset.zbd;
        if (action === 'remove') return zgRemoveAsk({id: d.id, name, zones: d.zones});
        if (action === 'forget') return zgForget({id: d.id, name, zones: d.zones});
        if (action === 'rename') return editDialog({title: t('Rename {0}', name), intro: t('Empty: named after its zone, or its model.'),
          specs: [{name: 'name', label: t('Name'), type: 'text', maxLength: 23}], values: {name: d.name},
          reviewOptions: {title: t('Rename it?'), run: values => zgSend({op: 'rename', id: d.id, name: values.name})}});
        if (action === 'replace') return review({title: t('Replace {0}?', name), confirm: t('Replace'), done: t('Listening for the new device.'),
          intro: t('Add the new device next: it takes {0}. {1} is removed once the new one is ready.', zgUses(d), name),
          run: () => zgSend({op: action, id: d.id}), after: () => go('zigbee-add')});
        button.disabled = true;
        try {
          await zgSend({op: action, id: d.id});
          toast({identify: t('Its light blinks for a few seconds.'), ping: t('Asked it to answer: the Zigbee page shows what it said.'),
            siren_test: t('It sounds for a moment.'), setup: t('Setting it up again.')}[action]);
        } catch (error) { toast(error.message, 'error'); }
        button.disabled = false;
      });
    },
  };
}

// ---- Radio details and Channel ----------------------------------------------------------------------
async function zgRadioPage() {
  const z = await api('zigbee');
  if (!z.available) return zgNotRunning(t('Radio details'));
  const names = await zgZoneNames();
  const r = z.radio, [, word] = zgRadioWords(r), may = z.may_change, paused = r.state === 'paused';
  const nodes = z.devices.map(d => `<tr><td><a href="#/zigbee-device/${d.id}">${esc(zgName(d, names))}</a></td>
    <td>${d.lqi ? esc(String(d.lqi)) : '–'}</td><td>${d.rssi ? esc(t('{0} dBm', d.rssi)) : '–'}</td><td>${esc(zwAgo(d.heard_s))}</td><td>${d.router ? ht('Yes') : ht('No')}</td></tr>`).join('');
  return {
    html: pageHead(t('Radio details'), t('The panel’s Zigbee radio, for the installer.'), '', zgBack()) + zgLockNotice(z) +
      `<section class="card pad">${kv([[t('State'), word], [t('Radio software'), r.version || t('Not known')], [t('Channel'), r.channel ? String(r.channel) : ''],
        [t('Network id (PAN)'), r.pan], [t('Extended id'), zgAddr(r.ext_pan)], [t('The panel’s Zigbee address'), zgAddr(r.ieee)],
        [t('Background noise'), r.noise !== undefined ? t('{0} dBm', r.noise) : t('Not measured yet')],
        [t('Panel restarts it caused'), paused ? t('{0}: paused', r.restarts) : t('{0} (it pauses at {1})', r.restarts, 2)]])}
        <div class="card-foot">${paused ? `<button type="button" class="primary" data-zbr="retry" ${zgOff(may)}>${ht('Try again')}</button>`
          : `<button type="button" data-zbr="restart" ${zgOff(may && !zgRunning(z.progress) && !['not_connected', 'socket_zwave'].includes(r.state))}>${ht('Restart the radio')}</button>`}</div></section>
      <section class="section"><h2>${ht('Per device')}</h2>
        <p class="muted-note">${ht('Link quality (0 to 255) and signal strength as the radio heard each device’s last message. The touchscreen says them in words: Good, Fair, Weak.')}</p>
        ${nodes ? `<div class="card table-wrap"><table><thead><tr><th>${ht('Device')}</th><th>${ht('Link quality')}</th><th>${ht('Signal strength')}</th>
          <th>${ht('Last heard')}</th><th>${ht('Extends the network')}</th></tr></thead><tbody>${nodes}</tbody></table></div>` : `<p class="muted-note">${ht('No devices.')}</p>`}</section>`,
    bind() {
      $$('[data-zbr]').forEach(button => button.onclick = async () => {
        const action = button.dataset.zbr;
        if (action === 'restart') return review({title: t('Restart the Zigbee radio?'), intro: t('Takes a few seconds; zones keep their state.'),
          confirm: t('Restart'), done: t('Restarting.'), run: () => zgSend({op: action})});
        button.disabled = true;
        try { await zgSend({op: action}); toast(t('Starting the radio again.')); await show(); } catch (error) { toast(error.message, 'error'); button.disabled = false; }
      });
    },
  };
}
async function zgChannelPage() {
  const z = await api('zigbee');
  if (!z.available) return zgNotRunning(t('Channel'));
  const r = z.radio, may = z.may_change && zgUsable(r) && !zgRunning(z.progress);
  const words = zgNoiseWords;
  const choices = (r.survey || []).map(c => `<label class="zb-chan"><input type="radio" name="zb-chan" value="${c.channel}" ${c.channel === r.channel ? 'checked' : ''} ${zgOff(may)}>
    <span><strong>${ht('Channel {0}', c.channel)}</strong><small>${esc(c.dbm !== undefined ? t('{0} · {1} dBm', words(c.dbm), c.dbm) : words(c.dbm))}</small></span>
    <span class="tags">${c.channel === r.channel ? tag(t('Now'), 'info') : ''}${c.channel === r.quietest ? tag(t('Quietest'), 'good') : ''}</span></label>`).join('');
  return {
    html: pageHead(t('Channel'), t('Where the Zigbee network talks.'), '', zgBack()) + zgLockNotice(z) +
      notice('warn', t('Sleeping devices move when they next check in, within 2 hours.'), t('Some may need to be added again.')) +
      `<form class="card" id="zb-chan-form" novalidate>${choices}
        <div class="card-foot"><span class="note">${ht('Checking for noise: sensors aren’t heard for a few seconds.')}</span>
          <button type="button" id="zb-scan-again" ${zgOff(may)}>${ht('Check for noise again')}</button>
          <button type="submit" class="primary" ${zgOff(may)}>${ht('Move to this channel')}</button></div></form>`,
    bind() {
      $('#zb-scan-again').onclick = async () => { try { await zgSend({op: 'survey'}); toast(t('Checking for noise.')); } catch (error) { toast(error.message, 'error'); } };
      $('#zb-chan-form').onsubmit = event => {
        event.preventDefault();
        const channel = Number($('#zb-chan-form input[name="zb-chan"]:checked')?.value || r.channel);
        if (channel === r.channel) return toast(t('It is on channel {0} already.', channel));
        review({title: t('Move to channel {0}?', channel), confirm: t('Move'), done: t('Moving to channel {0}.', channel),
          intro: t('Every device follows. Sleeping ones move when they next check in; some may need to be added again.'),
          run: () => zgSend({op: 'channel', channel}), after: () => go('zigbee')});
      };
    },
  };
}

// ---------------------------------------------------------------------------------------
// Walk test (docs/features/40): the phone's page, the Overview's card and the nav's count.
// The page follows the panel's list, never its own: it reads walk-test?key=<key> every second
// (WALK-017) and re-draws when the key moved. Nothing flashes or animates (WALK-013): a zone moves
// in the list at most once a second, because that is how often it is read; the time left shows in whole
// minutes, so it changes once a minute.

const WALK_AWAY_MS = 4000; // longer than this since the last answer: the phone was away (WALK-015)
const WALK_FB = {beep: true, vibrate: true, names: true, awake: true};
const walk = {data: [], parts: {}, part: 0, key: null, fullAt: 0, lastOk: 0, offline: false, busy: false, filter: 'untested',
  panic: {}, view: {}, order: {}, seen: {}, ends: {}, fb: {...WALK_FB}, lock: null, locking: false, speaker: null, left: false};

// Each switch is remembered in this browser (WALK-014).
function walkPrefs() {
  try { return {...WALK_FB, ...JSON.parse(localStorage.getItem('panel-walk-feedback') || '{}')}; } catch { return {...WALK_FB}; }
}
function walkSavePrefs() {
  try { localStorage.setItem('panel-walk-feedback', JSON.stringify(walk.fb)); } catch { /* not remembered */ }
}

// Speech never piles up (WALK-015): one name is said, at most one more waits, the rest become "3 more
// zones tested". speak(text, done) says a text and calls done() when it has been said.
// Only zone names count towards "more zones tested"; a status phrase (the test ended, while you were away)
// takes the waiting place, the latest one wins, and a name it displaces is counted. stop() silences it for good.
function makeSpeaker(speak) {
  const s = {busy: false, stopped: false, next: '', nextIsStatus: false, more: 0};
  const run = text => {
    s.busy = true;
    speak(text, () => { if (s.stopped) return; s.busy = false; follow(); });
  };
  const follow = () => {
    if (s.stopped) return;
    if (s.next) { const text = s.next; s.next = ''; s.nextIsStatus = false; run(text); }
    else if (s.more) { const n = s.more; s.more = 0; run(tn('{0} more zone tested', '{0} more zones tested', n)); }
  };
  s.say = (text, status = false) => {
    if (s.stopped) return;
    if (!s.busy) return run(text);
    if (status) { if (s.next && !s.nextIsStatus) s.more++; s.next = text; s.nextIsStatus = true; }
    else if (!s.next) { s.next = text; s.nextIsStatus = false; }
    else s.more++;
  };
  s.stop = () => { s.stopped = true; s.busy = false; s.next = ''; s.more = 0; };
  return s;
}
// What a new answer holds that the phone hasn't heard: the zones tested since, a repeat trip (a tick) or,
// after the page was away, only how many were tested meanwhile - never a backlog of names (WALK-015).
// seen: {tested: Set of zone numbers, trips}, null on the first answer (nothing is announced then).
function walkDiff(seen, zones, trips, gapMs) {
  if (!seen) return {fresh: [], repeat: false, away: 0};
  const fresh = zones.filter(z => z.tested && !seen.tested.has(z.number)).sort((a, b) => a.first_s - b.first_s || a.number - b.number);
  if (gapMs > WALK_AWAY_MS && fresh.length) return {fresh: [], repeat: false, away: fresh.length};
  return {fresh, repeat: !fresh.length && trips > seen.trips, away: 0};
}
// The tested zones, the newest first: by the seconds after the start, then by the order this page saw them.
function walkNewestFirst(zones, order) {
  return zones.filter(z => z.tested).sort((a, b) => b.first_s - a.first_s || order.indexOf(b.number) - order.indexOf(a.number));
}
// The report's file: one cell in a spreadsheet's own quoting; a name that starts like a formula is made text.
function csvCell(value) {
  let s = String(value ?? '');
  if (/^[=+\-@\t\r]/.test(s)) s = "'" + s;
  return /[",\r\n]/.test(s) ? '"' + s.replaceAll('"', '""') + '"' : s;
}
const walkCsv = rows => rows.map(row => row.map(csvCell).join(',')).join('\r\n') + '\r\n';

const walkWho = (id, name) => name || (id ? tc('default user name', 'User {0}', id) : t('Someone'));
function walkStartedWords(from, name) {
  switch (from) {
    case 'panel': return t('Started at the panel by {0}', name);
    case 'panel_manager': return t('Started in the Panel Manager by {0}', name);
    case 'keypad': return t('Started at a keypad by {0}', name);
    case 'console': return t('Started from the service console by {0}', name);
    case 'remote': return t('Started remotely by {0}', name);
    default: return t('Started by {0}', name);
  }
}
function walkEndedWords(p) {
  const name = walkWho(p.ended_by, p.ended_by_name);
  return p.end_how === 'time_limit' ? t('The time ran out') : p.end_how === 'disarm' ? t('Ended by {0} with a disarm', name) : t('Ended by {0}', name);
}
// The nav's count while a test runs, wherever it was started (a steady number, never a blinking dot).
function walkBadge() {
  const running = (state?.partitions || []).filter(p => p.walk_test);
  if (!running.length) return '';
  return `${running.reduce((n, p) => n + (p.walk_tested || 0), 0)}/${running.reduce((n, p) => n + (p.walk_total || 0), 0)}`;
}
const walkLeftMs = number => Math.max(0, (walk.ends[number] || 0) - Date.now());
const walkMinutes = ms => Math.max(1, Math.ceil(ms / 60000));
// The same words as the touchscreen's walk test (ui_walk.c): one translation serves both
const walkCountWords = p => tn('{0} of {1} tested', '{0} of {1} tested', p.tested, p.total);
const walkEndsWords = number => tn('Ends in {0} min', 'Ends in {0} min', walkMinutes(walkLeftMs(number)));

// ---- Reading the panel ---------------------------------------------------------------------------
const walkViewOf = p => p.running ? 'running' : walk.view[p.number] === 'summary' && p.end_how ? 'summary' : 'ready';
const walkAny = () => walk.data.some(p => p.running);
function walkSetEnd(number, remaining) {
  const end = Date.now() + remaining * 1000;
  // kept steady unless the panel's differs by more than a second and a half (a poll's own delay shouldn't move it)
  if (!walk.ends[number] || Math.abs(walk.ends[number] - end) > 1500) walk.ends[number] = end;
}
function walkApply(answer, gap) {
  const now = Date.now();
  walk.lastOk = now;
  if (answer.unchanged) {
    for (const q of answer.partitions) {
      walkSetEnd(q.number, q.remaining_s);
      const p = walk.parts[q.number], sp = partitionOf(q.number);
      if (p) Object.assign(p, {running: q.running, remaining_s: q.remaining_s});
      if (sp) Object.assign(sp, {walk_test: q.running, walk_remaining_s: q.remaining_s});
    }
    return;
  }
  walk.key = answer.key;
  walk.fullAt = now;
  const all = {fresh: [], repeat: false, away: 0}, ended = [];
  for (const p of answer.partitions) {
    const was = walk.parts[p.number], order = walk.order[p.number] || [];
    const diff = p.running && was?.running ? walkDiff(walk.seen[p.number] || null, p.zones, p.trips, gap) : {fresh: [], repeat: false, away: 0};
    all.fresh.push(...diff.fresh);
    all.repeat ||= diff.repeat;
    all.away += diff.away;
    if (was?.running && !p.running) { walk.view[p.number] = 'summary'; ended.push(p); }
    walk.seen[p.number] = p.running ? {tested: new Set(p.zones.filter(z => z.tested).map(z => z.number)), trips: p.trips} : null;
    // the order this page saw the zones tested in (the seconds only say it to the second)
    const tested = new Set(p.zones.filter(z => z.tested).map(z => z.number));
    walk.order[p.number] = order.filter(n => tested.has(n)).concat(walkNewestFirst(p.zones, []).reverse().map(z => z.number).filter(n => !order.includes(n)));
    walk.parts[p.number] = p;
    walkSetEnd(p.number, p.remaining_s);
    const sp = partitionOf(p.number);
    if (sp) Object.assign(sp, {walk_test: p.running, walk_tested: p.tested, walk_total: p.total, walk_remaining_s: p.remaining_s,
      walk_started_by: p.started_by, walk_started_by_name: p.started_by_name, walk_started_from: p.started_from});
  }
  walk.data = answer.partitions;
  walkFeedback(all, ended);
}

// ---- The phone's own feedback (WALK-014, WALK-015) -------------------------------------------------
const walkVibrate = pattern => { try { navigator.vibrate?.(pattern); } catch { /* not on this phone */ } };
const walkCanVibrate = () => typeof navigator !== 'undefined' && 'vibrate' in navigator;
function walkSay(text, status = false) {
  if (!walk.fb.names || !canSpeak()) return;
  walk.speaker = walk.speaker || makeSpeaker(speakNow);
  walk.speaker.say(text, status);
}
function walkFeedback({fresh, repeat, away}, ended) {
  if (walk.left) return; // the page was left: nothing more is heard
  const fb = walk.fb;
  if (fresh.length || away) {
    if (fb.beep) beep(false);
    if (fb.vibrate) walkVibrate([70, 60, 70, 60, 70]);
  } else if (repeat) { // a repeat trip: a short tick only
    if (fb.beep) beep(true);
    if (fb.vibrate) walkVibrate(30);
  }
  if (away) walkSay(tn('{0} zone tested while you were away', '{0} zones tested while you were away', away), true);
  for (const z of fresh) walkSay(zoneLabel({name: z.name, zone: z.number}));
  const last = fresh[fresh.length - 1];
  const say = $('#walk-say');
  if (say && last) {
    const p = walk.data.find(x => x.zones.some(z => z === last)) || walk.parts[walk.part] || {};
    say.textContent = t('{0} tested. {1} of {2}.', zoneLabel({name: last.name, zone: last.number}), p.tested ?? '', p.total ?? '');
  }
  for (const p of ended) walkSay(t('Walk test ended. {0} of {1} tested.', p.tested, p.total), true);
}
function trySound() { // a muted phone is found before the walk
  unlockAudio();
  beep(false);
  walkVibrate([70, 60, 70, 60, 70]);
  walkSay(t('Sound check'));
}
// Screen Wake Lock while a test runs on this page (WALK-014)
async function walkSyncLock() {
  const can = typeof navigator !== 'undefined' && !!navigator.wakeLock;
  const want = can && page === 'walk' && walkAny() && walk.fb.awake;
  if (walk.locking) return;
  walk.locking = true;
  try {
    if (want && !walk.lock) {
      const lock = await navigator.wakeLock.request('screen');
      if (walk.left || page !== 'walk' || !walkAny()) { try { await lock.release(); } catch { /* already let go */ } return; } // left meanwhile
      walk.lock = lock;
      lock.addEventListener('release', () => { if (walk.lock === lock) walk.lock = null; });
    } else if (!want && walk.lock) {
      const lock = walk.lock;
      walk.lock = null;
      await lock.release();
    }
  } catch { walk.lock = null; } finally { walk.locking = false; }
}
function walkLeave() { // called by show() when another page opens: the screen may sleep again, nothing more is said
  const lock = walk.lock;
  walk.lock = null;
  try { lock?.release().catch(() => {}); } catch { /* already let go */ }
  try { if (canSpeak()) speechSynthesis.cancel(); } catch { /* nothing was speaking */ }
  walk.speaker?.stop();
  walk.speaker = null;
  walk.left = true;
}

// ---- The page --------------------------------------------------------------------------------------
const walkKind = z => deviceClassName((live.zones || []).find(x => x.zone === z.number)?.device_class);
const walkGlyph = number => { const z = (live.zones || []).find(x => x.zone === number); return z ? zoneGlyph(z) : icon('shield'); };
const walkAt = (p, z) => timeOfDay(walk.fullAt - (p.started_s_ago - z.first_s) * 1000);
function walkZoneState(p, z) {
  const at = z.tested ? walkAt(p, z) : '';
  if (z.check) return ['nocontact', t('No contact'), t('Check its battery or range')];
  if (z.tested && z.open) return ['open tested', tc('zone state', 'Open'), t('Tested {0} · close it', at)];
  if (z.tested) return ['tested', t('Tested'), [at, z.trips > 1 ? tn('{0} time', '{0} times', z.trips) : '', z.cover ? t('cover too') : ''].filter(Boolean).join(' · ')];
  if (z.cover) return ['untested', t('Not tested'), t('Cover tested')];
  return ['untested', t('Not tested'), walkKind(z)];
}
function walkRow(p, z, fresh) {
  const [cls, word, line] = walkZoneState(p, z);
  const glyph = cls === 'tested' ? icon('check') : cls === 'nocontact' ? icon('alert') : walkGlyph(z.number);
  return `<li class="${cls}${fresh ? ' fresh' : ''}"><div class="wt-row"><span class="wt-chip">${glyph}</span>
    <span class="main"><strong><span class="num">${z.number}</span>${esc(zoneLabel({name: z.name, zone: z.number}))}</strong>
    <span><span class="now">${esc(word)}</span>${line ? ' · ' + esc(line) : ''}</span></span></div></li>`;
}
const walkList = (p, list, newest) => `<div class="card"><ul class="list wt-list">${list.map(z => walkRow(p, z, newest === z.number)).join('')}</ul></div>`;
function walkLive(p) {
  if (!p.live?.length) return '';
  return `<section class="section"><div class="section-head"><h2>${ht('Stay on during the test')}</h2></div><div class="card"><ul class="list wt-list">${p.live.map(z =>
    `<li class="live"><div class="wt-row"><span class="wt-chip">${walkGlyph(z.number)}</span><span class="main"><strong><span class="num">${z.number}</span>${esc(zoneLabel({name: z.name, zone: z.number}))}</strong>
    <span><span class="now">${ht('Stays on')}</span>${walkKind({number: z.number}) ? ' · ' + esc(walkKind({number: z.number})) : ''}</span></span></div></li>`).join('')}</ul></div></section>`;
}
function walkSwitches() {
  const iphone = typeof navigator !== 'undefined' && (/iPhone|iPad|iPod/.test(navigator.userAgent || '') || (/Macintosh/.test(navigator.userAgent || '') && navigator.maxTouchPoints > 1));
  const can = {beep: true, vibrate: walkCanVibrate(), names: canSpeak(), awake: typeof navigator !== 'undefined' && !!navigator.wakeLock};
  const pill = (k, glyph, label) => can[k]
    ? `<button type="button" data-wfb="${k}" aria-pressed="${walk.fb[k]}">${icon(glyph)}<span class="name">${esc(label)}</span><span class="on">${walk.fb[k] ? ht('On') : ht('Off')}</span></button>`
    : `<button type="button" disabled>${icon(glyph)}<span class="name">${esc(label)}</span><span class="on">${ht('Not on this phone')}</span></button>`;
  return `<section class="wt-phone" aria-labelledby="wt-phone-h"><div class="head"><h2 id="wt-phone-h">${ht('On this phone')}</h2>
      <button type="button" class="small" data-wact="try">${icon('volume')}${ht('Test sound')}</button></div>
    <div class="wt-toggles">${pill('beep', 'volume', t('Beep'))}${pill('vibrate', 'vibrate', t('Vibrate'))}${pill('names', 'speech', t('Say zone names'))}${pill('awake', 'sun', t('Keep the screen on'))}</div>
    ${iphone ? `<p class="note">${icon('info')}${ht('No sound? The ring switch on the side mutes the beeps and names.')}</p>` : ''}</section>`;
}
const walkOffTop = () => walk.offline ? `<div class="wt-off-top">${notice('warn', t('Not connected to the panel'),
  t('The test goes on at the panel. Zones tested meanwhile show when you’re back.'))}</div>` : '';
const walkOff = () => walk.offline || walk.busy ? ' disabled' : '';
const walkCount = p => `<div class="wt-count"><span class="count">${p.tested}<small>/${p.total}</small></span><span class="summary">${ht('tested')}</span></div>`;

function walkReady(p) {
  // everyone who may start a test, as the engine and the touchscreen do (partition masters too)
  const options = `<div class="opts"><label class="opt"><span class="main"><strong>${ht('Include panic and 24-hour zones')}</strong>
      <small>${ht('They beep instead of alarming · off again after the test')}</small></span>
      <input type="checkbox" class="switch" data-wpanic data-k="panic"${walk.panic[p.number] ? ' checked' : ''}></label></div>`;
  const last = p.end_how ? `<div class="wt-lasttest"><span>${ht('Last test: {0} of {1}', p.tested, p.total)}</span>
      <button type="button" class="small" data-wact="view">${ht('View')}</button></div>` : '';
  return `${walkOffTop()}<section class="card command">
    <div class="command-top"><span class="state-badge">${icon('walk')}</span>
      <div class="state-words"><span class="part-name">${esc(partName(p))}</span><span class="state">${ht('Ready to test')}</span>
        <p>${esc(tn('{0} zone · smoke and CO stay on', '{0} zones · smoke and CO stay on', p.zones.length))}</p></div></div>
    ${options}${last}
    <p class="summary">${ht('The siren sounds for 1 s at the start.')}</p>
    <div class="choices one">${choice('fill test', icon('walk'), t('Start walk test'), '', `data-wact="start" data-k="start"${walkOff()}`)}</div>
  </section>
  ${walkSwitches()}
  <section class="section"><div class="section-head"><h2>${ht('Zones to test')}</h2><p>${p.zones.length}</p></div>${p.zones.length ? walkList(p, p.zones, 0)
    : `<div class="card"><p class="empty">${ht('No zones to test.')}</p></div>`}</section>
  ${walkLive(p)}`;
}

function walkStrip(p) {
  const newest = walkNewestFirst(p.zones, walk.order[p.number] || []);
  const last = newest[0];
  if (!last) return `<div class="wt-last none"><span class="wt-chip">${icon('untested')}</span><span class="main"><strong>${ht('Nothing tested yet')}</strong>
    <span>${ht('Open a door or walk past a sensor')}</span></span></div>`;
  const [, word, line] = walkZoneState(p, last), before = newest.slice(1, 3).map(z => zoneLabel({name: z.name, zone: z.number}));
  return `<div class="wt-last"><span class="wt-chip">${icon('check')}</span><span class="main"><strong>${esc(zoneLabel({name: last.name, zone: last.number}))}</strong>
    <span>${esc(word)}${line ? ' · ' + esc(line) : ''}</span>${before.length ? `<span class="before">${esc(t('Before: {0}', listWords(before)))}</span>` : ''}</span></div>`;
}
function walkRunning(p) {
  const left = walkLeftMs(p.number), ending = left <= 300000;
  const untested = p.zones.filter(z => !z.tested), tested = walkNewestFirst(p.zones, walk.order[p.number] || []);
  const shown = walk.filter === 'untested' ? untested : walk.filter === 'tested' ? tested : untested.concat(tested);
  const filters = [['untested', t('Not tested {0}', untested.length)], ['tested', t('Tested {0}', tested.length)], ['all', t('All {0}', p.zones.length)]];
  const newest = walkNewestFirst(p.zones, walk.order[p.number] || [])[0];
  const parts = walk.data.length > 1 ? esc(partName(p)) + ' · ' : '';
  const dockTop = walk.offline ? ht('Not connected') : parts + esc(walkCountWords(p));
  const dockLine = walk.offline ? ht('Last update {0} · the panel still beeps', timeOfDay(walk.lastOk))
    : (newest && !ending ? esc(zoneLabel({name: newest.name, zone: newest.number})) + ' · ' : '') + esc(walkEndsWords(p.number));
  const extend = (cls, label) => choice(cls, icon('plus'), label, '', `data-wact="extend" data-k="extend"${walkOff()}`);
  return `${walkOffTop()}
  <section class="card command ${ending ? 'warn' : 'info'}">
    <div class="command-top"><span class="state-badge">${icon('walk')}</span>
      <div class="state-words"><span class="part-name">${esc(partName(p))}</span><span class="state">${ht('Testing zones')}</span>
        <p>${esc(walkStartedWords(p.started_from, walkWho(p.started_by, p.started_by_name)))}</p></div>${walkCount(p)}</div>
    <div class="timebar"><progress max="${Math.max(1, p.total)}" value="${p.tested}" aria-label="${ht('Zones tested')}"></progress>
      <div class="wt-ends"><span><strong>${esc(walkEndsWords(p.number))}</strong> · ${esc(timeOfDay(walk.ends[p.number] || Date.now()))}</span>${ending ? ''
        : `<button type="button" class="small" data-wact="extend" data-k="extend-small"${walkOff()}>${icon('plus')}${ht('Extend')}</button>`}</div></div>
    ${p.life24 ? notice('info', t('Panic and 24-hour zones only beep')) : ''}
    ${walkStrip(p)}
    ${ending ? `<div class="choices one wt-wide-only">${extend('fill test', t('Extend the walk test'))}</div>` : ''}
    <div class="choices one wt-wide-only">${choice('neutral', icon('stop'), t('End walk test'), '', `data-wact="end" data-k="end"${walkOff()}`)}</div>
  </section>
  <section class="section"><div class="section-head"><h2>${ht('Zones')}</h2>
    <div class="filters" role="group" aria-label="${ht('Show')}">${filters.map(([v, label]) =>
      `<button type="button" data-wfilter="${v}" aria-pressed="${walk.filter === v}">${esc(label)}</button>`).join('')}</div></div>
    ${shown.length ? walkList(p, shown, newest?.number) : `<div class="card"><p class="empty">${walk.filter === 'untested' ? ht('Every zone is tested.') : ht('None yet.')}</p></div>`}</section>
  ${walkLive(p)}
  ${walkSwitches()}
  <div class="wt-dock${walk.offline ? ' off' : ending ? ' ending' : ''}"><span class="main"><strong>${dockTop}</strong><span>${dockLine}</span></span>
    <span class="acts">${ending && !walk.offline ? `<button type="button" class="small confirm test" data-wact="extend" data-k="extend-dock"${walkOff()}>${icon('plus')}${ht('Extend')}</button>` : ''}
    <button type="button" class="small" data-wact="end" data-k="end-dock"${walkOff()}>${icon('stop')}${ht('End')}</button></span></div>`;
}

function walkEnded(p) {
  const untested = p.zones.filter(z => !z.tested), open = p.zones.filter(z => z.open), all = untested.length === 0;
  const group = (title, list, help) => list.length ? `<section class="section"><div class="section-head"><div><h2>${esc(title)} · ${list.length}</h2>${help ? `<p>${esc(help)}</p>` : ''}</div></div>${walkList(p, list, 0)}</section>` : '';
  const tested = walkNewestFirst(p.zones, walk.order[p.number] || []);
  return `${walkOffTop()}<section class="card command ${all ? 'good' : 'warn'}">
    <div class="command-top"><span class="state-badge">${icon(all ? 'check' : 'alert')}</span>
      <div class="state-words"><span class="part-name">${esc(partName(p))}</span><span class="state">${esc(walkCountWords(p))}</span><p>${esc(walkEndedWords(p))}</p></div></div>
    ${all ? '' : `<div class="choices one">${choice('fill test', icon('again'), t('Test the untested again'), t('Starts a new test: the list starts again'), `data-wact="again" data-k="again"${walkOff()}`)}</div>`}
    <div class="actions wt-end-acts"><button type="button" data-wact="report" data-k="report">${icon('report')}${ht('Save report')}</button><button type="button" data-wact="done" data-k="done">${ht('Done')}</button></div>
  </section>
  ${group(t('Still open'), open, t('Close before arming'))}
  ${group(t('Not tested'), untested, '')}
  <section class="section"><div class="card"><details class="more"><summary>${ht('Tested')} · ${tested.length}</summary>
    <ul class="list wt-list">${tested.map(z => walkRow(p, z, false)).join('')}</ul></details></div></section>`;
}
function walkBody() {
  if (!walk.data.length) return notice('info', t('No partitions'), t('There is no partition this sign-in may test.'));
  if (!walk.parts[walk.part]) walk.part = (walk.data.find(p => p.running) || walk.data[0]).number;
  const p = walk.parts[walk.part], view = walkViewOf(p);
  const pills = walk.data.length > 1 ? `<div class="filters" role="group" aria-label="${ht('Partition')}">${walk.data.map(q =>
    `<button type="button" data-wpart="${q.number}" aria-pressed="${q.number === p.number}">${esc(partName(q))}${q.running ? ` · ${q.tested}/${q.total}` : ''}</button>`).join('')}</div>` : '';
  return pills + (view === 'running' ? walkRunning(p) : view === 'summary' ? walkEnded(p) : walkReady(p));
}
const redrawWalk = (force = false) => redraw('walk-live', walkBody, force);

async function walkRefresh(full) {
  const gap = walk.lastOk ? Date.now() - walk.lastOk : 0, shown = renderId;
  const answer = await api('walk-test' + (!full && walk.key !== null ? '?key=' + walk.key : ''), undefined, {signal: AbortSignal.timeout(4000)});
  if (walk.left || shown !== renderId) return; // the page was left while this was on its way: nothing is said or drawn
  walk.offline = false;
  walkApply(answer, gap);
  walkSyncLock();
}
async function walkPoll() {
  const shown = renderId;
  try { await walkRefresh(false); } catch { walk.offline = true; } // the test goes on at the panel; the page catches up when back
  if (!walk.left && shown === renderId) redrawWalk();
}
// The panel's refusal in words (a workstation's grant needs a password sign-in, WEB-024)
const walkRefusal = error => error.code === 'locked' || error.code === 'password_sign_in_required' ? error.message : refusalWords(error);
async function walkSend(action, extra = {}) {
  if (walk.busy) return false;
  walk.busy = true;
  redrawWalk();
  let done = false;
  try {
    await api('partition', {number: walk.part, action, ...extra});
    done = true;
    await walkRefresh(true);
  } catch (error) {
    if (!done) toast(walkRefusal(error), 'error');
  } finally {
    walk.busy = false;
  }
  redrawWalk(true);
  return done;
}
// The report, made here and saved as a file (WALK-010): the date, the result and each zone's.
function walkReportRows(p) {
  const word = z => z.check ? t('No contact') : z.tested && z.open ? tc('zone state', 'Open') : z.tested ? t('Tested') : t('Not tested');
  return [[t('Walk test report')], [t('Partition'), partName(p)], [t('Date'), new Date().toLocaleString(wordsLocale())],
    [t('Tested'), walkCountWords(p)], [t('Result'), walkEndedWords(p)], [],
    [t('Zone'), t('Name'), t('Result'), t('Times tripped'), t('Cover tested'), t('Seconds after the start')],
    ...p.zones.slice().sort((a, b) => a.number - b.number).map(z => [z.number, zoneLabel({name: z.name, zone: z.number}), word(z), z.trips,
      z.cover ? t('Yes') : t('No'), z.tested ? z.first_s : ''])];
}
function walkSaveReport(p) {
  const url = URL.createObjectURL(new Blob(['﻿' + walkCsv(walkReportRows(p))], {type: 'text/csv;charset=utf-8'}));
  const link = document.createElement('a');
  link.href = url;
  link.download = `walk-test-${p.number}-${new Date().toISOString().slice(0, 10)}.csv`;
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10000);
}
async function walkClick(event) {
  const el = event.target.closest('[data-wact], [data-wfb], [data-wfilter], [data-wpart]');
  if (!el || el.disabled) return;
  const d = el.dataset, p = walk.parts[walk.part];
  if (d.wfb) {
    walk.fb[d.wfb] = !walk.fb[d.wfb];
    walkSavePrefs();
    if (d.wfb === 'awake') walkSyncLock();
    redrawWalk();
  } else if (d.wfilter) {
    walk.filter = d.wfilter;
    redrawWalk();
  } else if (d.wpart) {
    walk.part = Number(d.wpart);
    redrawWalk(true);
  } else if (!p) {
    return;
  } else if (d.wact === 'start') {
    const life24 = !!walk.panic[p.number];
    if (await walkSend('walk_test_start', {life24})) walk.panic[p.number] = false; // the option is per start
  } else if (d.wact === 'again') {
    await walkSend('walk_test_start', {life24: !!p.life24});
  } else if (d.wact === 'extend') {
    await walkSend('walk_test_extend');
  } else if (d.wact === 'end') {
    await walkSend('walk_test_end');
  } else if (d.wact === 'report') {
    walkSaveReport(p);
  } else if (d.wact === 'done' || d.wact === 'view') {
    walk.view[p.number] = d.wact === 'done' ? 'ready' : 'summary';
    redrawWalk(true);
    window.scrollTo(0, 0);
  } else if (d.wact === 'try') {
    trySound();
  }
}
async function walkPage() {
  Object.assign(walk, {fb: walkPrefs(), key: null, lastOk: 0, offline: false, busy: false, view: {}, seen: {}, order: {}, ends: {}, parts: {}, data: [], speaker: null, left: false});
  await Promise.all([refreshStatus(), loadZones().catch(() => { live.zones = live.zones || []; })]);
  await walkRefresh(true);
  renderNav();
  const html = walkBody();
  return {
    html: `<div class="wt-page">${pageHead(t('Walk test'), t('Trip each sensor once. The panel keeps the list.'))}
      <div class="wt-sr" id="walk-say" role="status" aria-live="polite"></div><div id="walk-live">${html}</div></div>`,
    bind() {
      const root = $('#walk-live');
      if (!root) return;
      drawn['walk-live'] = html;
      live.redraw = redrawWalk;
      root.addEventListener('click', walkClick);
      root.addEventListener('change', event => {
        if (event.target.dataset?.wpanic !== undefined) { walk.panic[walk.part] = event.target.checked; redrawWalk(); }
      });
      walkSyncLock();
      poll(walkPoll, 1000);
    },
  };
}

// ---------------------------------------------------------------------------------------
// The page list: order and groups are the navigation.

// ---------------------------------------------------------------------------------------
// Your password (the browser sign-in; set first on the panel, or a temporary one)

function passwordPage() {
  const required = !!state.password_change_required;
  return {
    html: pageHead(required ? t('Choose your password') : t('Change your password'),
      required ? t('You signed in with a temporary password. Choose your own before anything else.')
        : t('The password you sign in to the Panel Manager with. Your keypad code does not change.')) +
      `<form class="card pad narrow" id="password-form" novalidate><div class="rows">
      <div class="row"><div><label class="label" for="pw-current">${required ? ht('Temporary password') : ht('Current password')}</label></div>
        <div class="control"><input id="pw-current" type="password" autocomplete="current-password" maxlength="64" required></div></div>
      <div class="row"><div><label class="label" for="pw-new">${ht('New password')}</label><span class="help">${ht('At least 12 characters. A few unrelated words are easy to remember.')}</span></div>
        <div class="control"><input id="pw-new" type="password" autocomplete="new-password" maxlength="64" required></div></div>
      </div><p class="dialog-error" id="pw-error" role="alert"></p>
      <div class="card-foot"><span class="note">${ht('Your other browsers are signed out.')}</span><button class="primary">${ht('Change password')}</button></div></form>`,
    bind() {
      $('#password-form').onsubmit = async event => {
        event.preventDefault();
        const current = $('#pw-current').value, next = $('#pw-new').value;
        const button = event.target.querySelector('button.primary');
        $('#pw-error').textContent = '';
        if (next.length < 12) return void ($('#pw-error').textContent = t('Use at least 12 characters.'));
        button.disabled = true;
        try {
          await api('password', {current, password: next});
          $$('#password-form input').forEach(i => { i.value = ''; });
          toast(t('Password changed.'));
          await refreshStatus();
          go('overview');
        } catch (error) {
          $('#pw-error').textContent = error.status === 429 ? t('Too many wrong passwords. Wait a minute and try again.') : error.message;
        } finally {
          button.disabled = false;
        }
      };
    },
  };
}

// access: which part of the panel's table the page belongs to (docs/features/14 "Who can change
// what"): users (the installer, a system or partition master), shared (the same people: Wi-Fi,
// time), system (the system master or the installer: integrations), installer. None: anyone
// signed in, as with no code at the panel. The panel checks every request itself. The names
// are marked (tx) and shown translated (t).
// ---- Household > Automations (docs/features/41 DESIGN.md section 10; the touchscreen mockup screens/automations.js) -------------
// The panel's own automations: one trigger and one or two actions, up to 16. The list says each one in the
// touchscreen's sentences (section 9 "Words used across screens"), a page per automation changes it, and every
// refusal web_automations.c can give is said in words. The panel checks everything again (AUT-001..AUT-016).

const AU_DAYS = ['mon', 'tue', 'wed', 'thu', 'fri', 'sat', 'sun'];
const AU_OUTSIDE = ['zone_opened', 'zone_closed', 'alarm', 'fire']; // D24: triggers someone outside may cause
const AU_ALARMS = ['alarm', 'fire']; // D22: an automation never silences an alarm, so no Disarm after these
const AU_POLL_MS = 2000; // the state lines: read again every 2 s, written only when they changed (UI-020, UI-023)
// Phase C (AUT-011): "Arm the panel" and "Disarm", the group "The alarm" in Then.
const auIsAlarm = a => a?.kind === 'arm' || a?.kind === 'disarm';
// Without devices (BL-224, DESIGN.md section 15): Remind to arm, "still armed" and the door chime in "The alarm", Send a
// notification, and the wired outputs set to "For automations". Remind and "still armed" look at the panel at their time:
// only after a time of day, and alone.
const AU_PANEL = ['remind', 'still_armed', 'chime', 'notify', 'output'];
const AU_TIME_ONLY = ['remind', 'still_armed'];
const auOwn = a => AU_PANEL.includes(a?.kind);
// The installer code never arms or disarms itself (USR-001): an Arm the installer sets up uses the system master's
// permission (BL-224, approved 2026-10-21); a Disarm stays the master's.
const auInstaller = () => state?.access?.arm === false;
// The less secure actions (auto_action_less_secure): a disarm, or a device pair hc_auto_less_secure names - an
// unlock, an open, a garage door's or a cover's stop (D25).
const auLessSecure = (a, kind = auKindOf(a)) => a?.kind === 'disarm' ||
  (a?.kind === 'device' && (['unlock', 'open'].includes(a.action) || (kind === 'cover' && a.action === 'stop')));
// auto_action_needs_trigger_confirm (D24, D31, AUT-016): a less secure action after a door, window, motion or alarm
// trigger, and an unlock, an open or a stop after an armed trigger, is saved only once confirmed.
// A Disarm of another partition after an armed trigger asks the plain confirm too (decided 2026-10-05; both editors).
const auNeedsConfirm = (when, a) => AU_OUTSIDE.includes(when?.kind) ? auLessSecure(a) : when?.kind === 'armed' && auLessSecure(a);
// A Disarm after an armed trigger of its own partition, or of any, could only undo the arming: refused (AUT-016, D31).
const auUndoes = (when, a) => a?.kind === 'disarm' && when?.kind === 'armed' && (!when.partitions?.length || when.partitions.includes(a.partition));
const auActs = d => (d?.actions || []).map(x => x.action);
// A switch turned on or toggled, or a scene, is asked about once when picked (D8, D9): the server's own flag.
const auAsksOnce = (d, action) => !!d?.actions?.find(x => x.action === action)?.confirm;
function auKindOf(a) {
  const d = au.devices.find(x => x.source === a?.source && x.ref === a?.ref);
  return d?.kind || '';
}

// What the pages are showing: the list as the panel sent it, the devices, and the automation being edited.
const au = {slot: 0, isNew: false, data: null, devices: [], rec: null, before: null, now: null, locked: false, said: ''};

// ---- Words: the sentences of the touchscreen ----------------------------------------------------------------

const auMany = () => (au.data?.partitions || []).length > 1;
const auPartNumbers = () => (au.data?.partitions || []).map(p => p.number);
const auPartName = n => au.data?.partitions.find(p => p.number === n)?.name || tc('default partition name', 'Partition {0}', n);
const auLevelWord = l => ({away: t('Away'), stay: t('Stay'), night: t('Night')})[l] || '';
const auKinds = () => [
  ['armed', t('When the panel is armed'), t('Once exit time is over')],
  ['disarmed', t('When the panel is disarmed'), ''],
  ['alarm', t('When an alarm sounds'), t('Never for a silent alarm')],
  ['fire', t('When a fire or CO alarm sounds'), ''],
  ['zone_opened', t('When a door or window opens'), t('Or a motion or water sensor')],
  ['zone_closed', t('When a door or window closes'), ''],
  ['time', t('At a time of day'), t('On the days you pick')],
  ['trouble', t('When there’s a trouble'), ''],
];
// "When there's a trouble" (BL-224): which one starts it.
const auTroubles = () => [['any', t('Any trouble')], ['ac', t('The power goes off')], ['battery', t('The panel’s battery is low')],
  ['sensor', t('A sensor stops reporting')], ['siren', t('A siren problem')]];
const auOutput = n => (au.data?.outputs || []).find(o => o.number === n);
const auOutputName = n => auOutput(n)?.name || t('Output {0}', n);
// Where a notification goes: every phone service set up in Integrations, even with that category off (BL-224).
const auWhere = () => listWords((au.data?.notify || []).map(x => ({ntfy: nt('ntfy'), pushover: nt('Pushover')})[x] || x));
const auTimes = () => Array.from({length: 288}, (_, i) => [i * 5, minuteText(i * 5)]); // 5-minute steps
const auShortDay = d => new Date(2026, 0, 5 + AU_DAYS.indexOf(d)).toLocaleDateString(wordsLocale(), {weekday: 'short'});
const auDayNames = days => listWords(AU_DAYS.filter(d => days.includes(d)).map(auShortDay));
function auDaysKind(days) {
  const has = d => days.includes(d);
  if (AU_DAYS.every(has)) return 'all';
  if (days.length === 5 && AU_DAYS.slice(0, 5).every(has)) return 'weekdays';
  if (days.length === 2 && has('sat') && has('sun')) return 'weekends';
  return 'some';
}
const auZone = w => au.data?.zones.find(z => z.number === w?.zone);
function auWhenText(w) {
  if (!w?.kind) return t('New automation');
  const part = auMany() && w.partitions?.length === 1 ? auPartName(w.partitions[0]) : '';
  const lvl = w.levels?.length === 1 ? auLevelWord(w.levels[0]) : '';
  const zone = auZone(w)?.name || w.zone_name || '', cls = auZone(w)?.class || 'door';
  switch (w.kind) {
    case 'armed':
      if (part) return lvl ? t('When {0} is armed {1}', part, lvl) : t('When {0} is armed', part);
      return lvl ? t('When the panel is armed {0}', lvl) : t('When the panel is armed');
    case 'disarmed': return part ? t('When {0} is disarmed', part) : t('When the panel is disarmed');
    case 'alarm': return t('When an alarm sounds');
    case 'fire': return t('When a fire or CO alarm sounds');
    case 'zone_opened':
      if (cls === 'motion') return t('When {0} sees motion', zone);
      if (cls === 'water') return t('When {0} detects water', zone);
      if (['door', 'window', 'garage_door'].includes(cls)) return t('When {0} opens', zone);
      return t('When {0} is triggered', zone);
    case 'zone_closed': return t('When {0} closes', zone);
    case 'trouble': return ({any: t('When there’s a trouble'), ac: t('When the power goes off'), battery: t('When the panel’s battery is low'),
      sensor: t('When a sensor stops reporting'), siren: t('When there’s a siren problem')})[w.trouble || 'any'] || t('When there’s a trouble');
    case 'time': {
      const at = minuteText(w.minute), k = auDaysKind(w.days || []);
      if (k === 'all') return t('At {0} every day', at);
      if (k === 'weekdays') return t('At {0} on weekdays', at);
      if (k === 'weekends') return t('At {0} on weekends', at);
      return t('At {0} on {1}', at, auDayNames(w.days || []));
    }
  }
  return '';
}
function auRunsText(w) {
  if (!w.days.length) return t('Pick at least one day');
  const at = minuteText(w.minute), k = auDaysKind(w.days);
  if (k === 'all') return t('Runs every day at {0}', at);
  if (k === 'weekdays') return t('Runs on weekdays at {0}', at);
  if (k === 'weekends') return t('Runs on weekends at {0}', at);
  return t('Runs on {0} at {1}', auDayNames(w.days), at);
}
const auMinutes = n => n === 60 ? t('1 hour') : tn('{0} minute', '{0} minutes', n);
// What the phone gets: from the trigger, never typed (AUT-001, BL-224).
function auNotice(w) {
  const zone = auZone(w)?.name || w?.zone_name || '';
  switch (w?.kind) {
    case 'zone_opened': return t('{0} opened', zone);
    case 'zone_closed': return t('{0} closed', zone);
    case 'armed': return t('The panel was armed');
    case 'disarmed': return t('The panel was disarmed');
    case 'alarm': return t('An alarm is sounding');
    case 'fire': return t('A fire or CO alarm is sounding');
    case 'trouble': return ({ac: t('The power went off'), battery: t('The panel’s battery is low'), sensor: t('A sensor stopped reporting'),
      siren: t('A siren has a problem')})[w.trouble] || t('There’s a trouble');
    case 'time': return t('It’s {0}', minuteText(w.minute));
  }
  return '';
}
function auActionText(a) {
  if (a.kind === 'arm') { // the touchscreen's arm_text: the partition's name only when there is more than one
    const p = auMany() ? auPartName(a.partition) : '';
    if (p) return ({away: t('Arm {0} Away', p), stay: t('Arm {0} Stay', p), night: t('Arm {0} Night', p)})[a.level] || '';
    return ({away: t('Arm Away'), stay: t('Arm Stay'), night: t('Arm Night')})[a.level] || '';
  }
  if (a.kind === 'disarm') return auMany() ? t('Disarm {0}', auPartName(a.partition)) : t('Disarm');
  switch (a.kind) { // BL-224: without a device
    case 'remind': return t('Remind to arm');
    case 'still_armed': return t('Tell me if it’s still armed');
    case 'chime': return a.on ? t('Turn the door chime on') : t('Turn the door chime off');
    case 'notify': return t('Send a notification');
    case 'output': {
      const name = auOutputName(a.output);
      if (a.action === 'off') return t('Turn off {0}', name);
      return a.off_after ? t('Turn on {0} for {1}', name, auMinutes(a.off_after)) : t('Turn on {0}', name);
    }
  }
  switch (a.action) {
    case 'on': return a.off_after ? t('Turn on {0} for {1}', a.device, auMinutes(a.off_after)) : t('Turn on {0}', a.device);
    case 'off': return t('Turn off {0}', a.device);
    case 'toggle': return t('Toggle {0}', a.device);
    case 'lock': return t('Lock {0}', a.device);
    case 'unlock': return t('Unlock {0}', a.device);
    case 'close': return t('Close {0}', a.device);
    case 'open': return t('Open {0}', a.device);
    case 'stop': return t('Stop {0}', a.device);
    case 'run': return t('Run {0}', a.device);
  }
  return '';
}
// The installer has turned the option off (D25): the touchscreen's sentence.
const auOffNote = () => t('Your installer has turned off automations that unlock, open or disarm.');
const auBlocked = a => au.data?.less_secure === false && auLessSecure(a);
// Who set it up (D30: never "arms as" a person; it acts as them, checked each time).
// Only a draft not yet saved falls back to the viewer: a stored one with no maker (deleted, never stored) says nothing, as the touchscreen does.
const auMadeBy = a => a.made_by || (au.isNew ? state?.name || '' : '');
// Whoever saves an Arm sets it up; the installer's uses the system master's permission (BL-224), as the panel stores it.
const auArmMaker = () => auInstaller() ? {made_by: au.data?.master || '', saved_by: state?.name || ''} : {made_by: state?.name || '', saved_by: null};
// The partitions a Disarm may name after this trigger (the touchscreen's disarm_partitions): the ones the person saving
// it may disarm, and never one an armed trigger watches (it could only undo the arming).
const auDisarmParts = when => {
  const mine = (state?.partitions || []).map(p => p.number);
  return auPartNumbers().filter(n => (!mine.length || mine.includes(n)) && !auUndoes(when, {kind: 'disarm', partition: n}));
};
function auActionDetail(a) {
  if (auBlocked(a)) return auOffNote();
  if (auUndoes(au.now?.when, a)) return t('It would undo the arming at once.');
  if (a.kind === 'arm') {
    const who = auMadeBy(a);
    if (!who) return '';
    if (a.saved_by) // the installer's (BL-224): it uses the system master's permission
      return tn('Set up by Installer · uses {1}’s permission · warns {0} minute before', 'Set up by Installer · uses {1}’s permission · warns {0} minutes before', a.warn_min, who);
    return a.bypass && au.data?.less_secure !== false
      ? tn('Set up by {1} · warns {0} minute before · bypasses open zones', 'Set up by {1} · warns {0} minutes before · bypasses open zones', a.warn_min, who)
      : tn('Set up by {1} · warns {0} minute before', 'Set up by {1} · warns {0} minutes before', a.warn_min, who);
  }
  if (a.kind === 'disarm') return ''; // its own row says who set it up, with what the panel checks
  switch (a.kind) { // BL-224
    case 'remind': return a.notify ? t('A chime and a note on the panel · also a notification') : t('A chime and a note on the panel');
    case 'still_armed': case 'notify': {
      const where = auWhere();
      if (!where) return t('Can’t send: no phone notifications are set up');
      const said = a.kind === 'notify' ? auNotice(au.now?.when) : t('{0} is still armed at {1}', auPartName(a.partition), minuteText(au.now?.when?.minute || 0));
      return said ? t('“{0}” · {1}', said, where) : where;
    }
    case 'chime': return auMany() ? auPartName(a.partition) : '';
    case 'output': return auOutput(a.output) ? t('Output on GPIO {0}', auOutput(a.output).gpio) : t('Not an output for automations now · pick another');
  }
  if (a.found === false) return t('Missing: it was called {0} · pick another device', a.device);
  return [a.area, a.online === false ? t('Not reachable now') : ''].filter(Boolean).join(' · ');
}
// The chip's glyph, by the first action (section 9).
function auActionIcon(a) {
  if (a.kind === 'remind' || a.kind === 'chime') return icon('bell');
  if (a.kind === 'notify' || a.kind === 'still_armed') return icon('send');
  if (a.kind === 'output') return icon('toggle');
  if (a.kind === 'arm') return icon(({away: 'away', stay: 'house', night: 'moon'})[a.level] || 'shield');
  if (a.kind === 'disarm' || a.action === 'unlock') return icon('lockOpen');
  return icon(({on: 'bulb', off: 'bulb', toggle: 'toggle', lock: 'lock', close: 'garage', open: 'garage', stop: 'garage', run: 'sparkles'})[a.action] || 'zap');
}
const auVerb = a => ({on: t('Turn on'), off: t('Turn off'), toggle: t('Toggle'), lock: t('Lock'), unlock: t('Unlock'), close: t('Close'),
  open: t('Open'), stop: t('Stop'), run: t('Run')})[a] || a;
// The confirm's sentence for one less secure action after a door, window, motion or alarm trigger (D24, AUT-016):
// whole sentences, one per trigger and action, never glued from pieces (components/ui/AGENTS.md).
function auConfirmText(w, a) {
  const dis = a.kind === 'disarm', many = auMany();
  const name = dis ? auPartName(a.partition) : a.device, zone = auZone(w)?.name || w.zone_name || '', cls = auZone(w)?.class || 'door';
  const form = w.kind === 'alarm' ? 'alarm' : w.kind === 'fire' ? 'fire' : w.kind === 'armed' ? 'armed' : w.kind === 'zone_closed' ? 'closes'
    : cls === 'motion' ? 'motion' : cls === 'water' ? 'water' : ['door', 'window', 'garage_door'].includes(cls) ? 'opens' : 'sensor';
  // A Disarm with one partition says "the panel" (the touchscreen's DISARM_PANEL_NOTE); with several, the partition's name.
  const D = {
    opens: () => many ? t('Anyone who opens {0} will disarm {1}.', zone, name) : t('Anyone who opens {0} will disarm the panel.', zone),
    closes: () => many ? t('Anyone who closes {0} will disarm {1}.', zone, name) : t('Anyone who closes {0} will disarm the panel.', zone),
    motion: () => many ? t('Anyone who walks past {0} will disarm {1}.', zone, name) : t('Anyone who walks past {0} will disarm the panel.', zone),
    water: () => many ? t('Water at {0} will disarm {1}.', zone, name) : t('Water at {0} will disarm the panel.', zone),
    sensor: () => many ? t('Anything that triggers {0} will disarm {1}.', zone, name) : t('Anything that triggers {0} will disarm the panel.', zone),
  };
  if (dis && D[form]) return D[form]();
  // After an armed trigger (D31): the touchscreen's sentences, by the level it watches and the partition it names.
  const lev = w.levels?.length === 1 ? w.levels[0] : 'any', part = w.partitions?.length ? auPartName(w.partitions[0]) : '';
  const A = {
    panel: {
      away: {unlock: () => t('Every time the panel is armed Away, {1} will unlock.', '', name), open: () => t('Every time the panel is armed Away, {1} will open.', '', name), stop: () => t('Every time the panel is armed Away, {1} will stop.', '', name)},
      stay: {unlock: () => t('Every time the panel is armed Stay, {1} will unlock.', '', name), open: () => t('Every time the panel is armed Stay, {1} will open.', '', name), stop: () => t('Every time the panel is armed Stay, {1} will stop.', '', name)},
      night: {unlock: () => t('Every time the panel is armed Night, {1} will unlock.', '', name), open: () => t('Every time the panel is armed Night, {1} will open.', '', name), stop: () => t('Every time the panel is armed Night, {1} will stop.', '', name)},
      any: {unlock: () => t('Every time the panel is armed, {1} will unlock.', '', name), open: () => t('Every time the panel is armed, {1} will open.', '', name), stop: () => t('Every time the panel is armed, {1} will stop.', '', name)},
    },
    named: {
      away: {unlock: () => t('Every time {0} is armed Away, {1} will unlock.', part, name), open: () => t('Every time {0} is armed Away, {1} will open.', part, name), stop: () => t('Every time {0} is armed Away, {1} will stop.', part, name), disarm: () => t('Every time {0} is armed Away, it will disarm {1}.', part, name)},
      stay: {unlock: () => t('Every time {0} is armed Stay, {1} will unlock.', part, name), open: () => t('Every time {0} is armed Stay, {1} will open.', part, name), stop: () => t('Every time {0} is armed Stay, {1} will stop.', part, name), disarm: () => t('Every time {0} is armed Stay, it will disarm {1}.', part, name)},
      night: {unlock: () => t('Every time {0} is armed Night, {1} will unlock.', part, name), open: () => t('Every time {0} is armed Night, {1} will open.', part, name), stop: () => t('Every time {0} is armed Night, {1} will stop.', part, name), disarm: () => t('Every time {0} is armed Night, it will disarm {1}.', part, name)},
      any: {unlock: () => t('Every time {0} is armed, {1} will unlock.', part, name), open: () => t('Every time {0} is armed, {1} will open.', part, name), stop: () => t('Every time {0} is armed, {1} will stop.', part, name), disarm: () => t('Every time {0} is armed, it will disarm {1}.', part, name)},
    },
  };
  const S = {
    armed: (part && many ? A.named : A.panel)[lev] || {},
    opens: {unlock: () => t('Anyone who opens {0} will unlock {1}, even when the alarm is on.', zone, name),
      open: () => t('Anyone who opens {0} will open {1}, even when the alarm is on.', zone, name),
      stop: () => t('Anyone who opens {0} will stop {1}, even when the alarm is on.', zone, name)},
    closes: {unlock: () => t('Anyone who closes {0} will unlock {1}, even when the alarm is on.', zone, name),
      open: () => t('Anyone who closes {0} will open {1}, even when the alarm is on.', zone, name),
      stop: () => t('Anyone who closes {0} will stop {1}, even when the alarm is on.', zone, name)},
    motion: {unlock: () => t('Anyone who walks past {0} will unlock {1}, even when the alarm is on.', zone, name),
      open: () => t('Anyone who walks past {0} will open {1}, even when the alarm is on.', zone, name),
      stop: () => t('Anyone who walks past {0} will stop {1}, even when the alarm is on.', zone, name)},
    water: {unlock: () => t('Water at {0} will unlock {1}, even when the alarm is on.', zone, name),
      open: () => t('Water at {0} will open {1}, even when the alarm is on.', zone, name),
      stop: () => t('Water at {0} will stop {1}, even when the alarm is on.', zone, name)},
    sensor: {unlock: () => t('Anything that triggers {0} will unlock {1}, even when the alarm is on.', zone, name),
      open: () => t('Anything that triggers {0} will open {1}, even when the alarm is on.', zone, name),
      stop: () => t('Anything that triggers {0} will stop {1}, even when the alarm is on.', zone, name)},
    alarm: {unlock: () => t('Whenever an alarm sounds, it will unlock {1}, even a false alarm.', '', name), open: () => t('Whenever an alarm sounds, it will open {1}, even a false alarm.', '', name),
      stop: () => t('Whenever an alarm sounds, it will stop {1}, even a false alarm.', '', name)},
    fire: {unlock: () => t('Whenever a fire or CO alarm sounds, it will unlock {1}.', '', name), open: () => t('Whenever a fire or CO alarm sounds, it will open {1}.', '', name),
      stop: () => t('Whenever a fire or CO alarm sounds, it will stop {1}.', '', name)},
  };
  return S[form][dis ? 'disarm' : a.action]?.() || '';
}
// Why it didn't work: the touchscreen's reasons (section 9).
function auReason(st) {
  const d = st.device || '';
  return ({no_answer: t('{0} didn’t answer', d), unreachable: t('{0} wasn’t reachable', d), refused: t('{0} said no', d),
    missing: t('{0} isn’t on the panel now', d), busy: t('{0} was busy', d), held_back: t('Too many ran at once'),
    not_allowed: t('That action isn’t allowed in an automation.')})[st.reason] || '';
}
// {same, tm, dt}: whether a moment is today, its time and its date.
function auParts(ms) {
  const d = new Date(ms);
  return {same: d.toDateString() === new Date().toDateString(), tm: timeOfDay(ms), dt: d.toLocaleDateString(wordsLocale(), {month: 'short', day: 'numeric'})};
}
// What an automation last did, as {text, tone}; the list passes list: true to leave out "hasn't run".
function auStatus(a, list = false) {
  const data = au.data;
  if (a.record === 'newer') return {text: t('Can’t run: made by newer software'), tone: 'warn'};
  if (a.record !== 'ok') return {text: t('Can’t run: open it and pick the device again'), tone: 'warn'};
  if (data?.less_secure === false && a.then.some(x => auLessSecure(x))) return {text: auOffNote(), tone: 'warn'}; // kept as it is, never sent (AUT-015)
  const gone = a.then.find(x => x.found === false);
  if (gone) return {text: t('{0} isn’t on the panel now', gone.device), tone: 'warn'};
  const st = a.status || {result: 'none'}, p = st.at ? auParts(st.at) : null;
  const alarmText = auAlarmStatus(a, st, p) || auPanelStatus(a, st, p);
  if (alarmText) return alarmText;
  switch (st.result) {
    case 'ran': if (p) return {text: p.same ? t('Ran at {0}', p.tm) : t('Ran on {0}', p.dt), tone: ''}; break;
    case 'tried': if (p) return {text: p.same ? t('Tried at {0}', p.tm) : t('Tried on {0}', p.dt), tone: ''}; break;
    case 'failed': {
      const head = !p ? t('Didn’t work') : p.same ? t('Didn’t work at {0}', p.tm) : t('Didn’t work on {0}', p.dt), why = auReason(st);
      return {text: why ? t('{0}: {1}', head, why) : head, tone: 'warn'};
    }
    case 'skipped':
      if (st.reason === 'no_clock') return {text: t('Waits until the clock is set'), tone: ''};
      if (st.reason === 'less_secure_off') return {text: auOffNote(), tone: 'warn'};
      if (st.reason === 'maker_gone') return {text: t('Can’t run: the person who saved it can no longer change automations'), tone: 'warn'};
      if (st.reason === 'held_back') {
        const head = !p ? t('Didn’t run') : p.same ? t('Didn’t run at {0}', p.tm) : t('Didn’t run on {0}', p.dt);
        return {text: t('{0}: {1}', head, t('Too many ran at once')), tone: 'warn'};
      }
      return {text: t('Can’t run: open it to fix it'), tone: 'warn'};
  }
  if (list) return null;
  return {text: data && !data.clock_set && a.when?.kind === 'time' ? t('Waits until the clock is set') : t('Hasn’t run since the panel started'), tone: ''};
}
// The statuses of an arming or disarming automation (DESIGN.md section 3; the touchscreen mockup screens/automations.js), as
// {text, tone}; null: the device automations' words apply (ran, held back, the option off, the maker gone).
function auAlarmStatus(a, st, p) {
  const x = a.then.find(auIsAlarm);
  if (!x) return null;
  const arm = x.kind === 'arm', at = (same, on) => p ? (p.same ? same(p.tm) : on(p.dt)) : '';
  switch (st.result) {
    case 'already':
      return {text: arm ? at(tm => t('Already armed at {0}', tm), dt => t('Already armed on {0}', dt)) || t('Already armed')
        : at(tm => t('Already disarmed at {0}', tm), dt => t('Already disarmed on {0}', dt)) || t('Already disarmed'), tone: ''};
    case 'put_off': // D20, D27: who, and until when
      if (st.choice === 'night') return {text: t('Won’t arm tonight · put off by {0}', st.by), tone: 'info'};
      if (st.choice === 'day') return {text: t('Won’t arm today · put off by {0}', st.by), tone: 'info'};
      return {text: st.until ? t('Put off by {0} until {1}', st.by, timeOfDay(st.until)) : t('Put off by {0}', st.by), tone: 'info'};
    case 'skipped':
      if (st.reason === 'not_ready') return {text: st.zone ? at(tm => t('Didn’t arm at {0}: {1} was open', tm, st.zone), dt => t('Didn’t arm on {0}: {1} was open', dt, st.zone))
        : at(tm => t('Didn’t arm at {0}: a door or window was open', tm), dt => t('Didn’t arm on {0}: a door or window was open', dt)), tone: 'warn'};
      if (st.reason === 'no_warning') return {text: at(tm => t('Didn’t arm at {0}: no time to warn first', tm), dt => t('Didn’t arm on {0}: no time to warn first', dt)), tone: 'warn'};
      return null;
    case 'failed':
      if (st.reason !== 'arm_refused') return null;
      if (arm && st.refused === 'not_authorized' && x.made_by) return {text: t('Can’t run: {0} can no longer arm this partition', x.made_by), tone: 'warn'};
      return {text: arm ? at(tm => t('Didn’t arm at {0}: the panel refused it', tm), dt => t('Didn’t arm on {0}: the panel refused it', dt))
        : at(tm => t('Didn’t disarm at {0}: the panel refused it', tm), dt => t('Didn’t disarm on {0}: the panel refused it', dt)), tone: 'warn'};
  }
  return null;
}
// BL-224: the statuses of a reminder, "still armed", the door chime and a notification, as {text, tone}; null: the
// common words apply (ran, tried, didn't work).
function auPanelStatus(a, st, p) {
  const at = (same, on) => p ? (p.same ? same(p.tm) : on(p.dt)) : '';
  if (st.result === 'skipped' && st.reason === 'no_service') return {text: t('Not sent: no phone notifications are set up'), tone: 'warn'};
  const x = a.then.find(auOwn);
  if (!x) return null;
  if (st.result === 'already') {
    if (x.kind === 'remind') return {text: at(tm => t('Already armed at {0}', tm), dt => t('Already armed on {0}', dt)) || t('Already armed'), tone: ''};
    if (x.kind === 'still_armed') return {text: at(tm => t('Not armed at {0}: nothing sent', tm), dt => t('Not armed on {0}: nothing sent', dt)) || t('Not armed: nothing sent'), tone: ''};
    if (p) return {text: p.same ? t('Ran at {0}', p.tm) : t('Ran on {0}', p.dt), tone: ''}; // the chime was so already
  }
  if (st.result === 'failed' && st.reason === 'arm_refused' && x.kind === 'chime')
    return {text: at(tm => t('Didn’t change the door chime at {0}: the panel refused it', tm), dt => t('Didn’t change the door chime on {0}: the panel refused it', dt))
      || t('Didn’t change the door chime: the panel refused it'), tone: 'warn'};
  return null;
}
function auTags(a) {
  if (a.record === 'newer') return [[t('Made by newer software'), 'warn']];
  if (a.record !== 'ok') return [[t('Can’t run'), 'warn']];
  const out = [];
  if (!a.on) out.push([t('Off'), '']);
  if (au.data?.less_secure === false && a.then.some(x => auLessSecure(x))) out.push([t('Can’t run'), 'warn']);
  const armSkip = a.then.some(x => x.kind === 'arm') && (a.status?.result === 'skipped' && ['not_ready', 'no_warning'].includes(a.status.reason));
  const armGone = a.then.some(x => x.kind === 'arm') && a.status?.reason === 'arm_refused' && a.status.refused === 'not_authorized';
  if (armSkip) out.push([t('Didn’t arm'), 'warn']);
  else if (armGone) out.push([t('Can’t run'), 'warn']);
  else if (a.status?.result === 'failed') out.push([t('Didn’t work'), 'warn']);
  if (a.then.some(x => x.found === false)) out.push([t('Device missing'), 'warn']);
  return out;
}
// The server's refusals in words (DESIGN.md section 10's table); the access ones are panel.js's ERRORS.
function auMessage(error) {
  const words = {
    changed: t('Someone changed automations meanwhile. Reload to see it.'),
    full: t('16 is the most. Delete one to add another.'),
    action_not_allowed: t('That action isn’t allowed in an automation.'),
    same_device: t('That device is already in this automation.'),
    needs_confirm: t('It has to be confirmed first.'),
    undoes_arming: t('It would undo the arming at once.'),
    arm_alone: t('Arming has to be its only action.'),
    bad_output: t('That output isn’t one for automations now.'),
    needs_time: t('A reminder and “still armed” run only at a time of day.'),
    less_secure_off: auOffNote(),
    incomplete: t('Choose when it runs and what it does.'),
    bad_zone: t('That sensor can’t start an automation.'),
    missing_device: t('That device isn’t on the panel now.'),
    storage_full: t('There’s no room left to store it.'),
    newer: t('Made by newer software: it can only be deleted.'),
    read_only: t('Automations can’t be changed right now.'),
    locked: t('Armed: automations can be changed once every partition is disarmed and quiet.'),
    busy: t('The panel was busy. Try again.'),
    failed: t('The panel couldn’t do that just now. Try again in a moment.'),
    bad_request: t('The panel did not accept one of the values.'),
  };
  return words[error.code] || error.message;
}

// ---- The list -------------------------------------------------------------------------------------------------

function auPage(arg) {
  const [first, second] = String(arg || '').split('/');
  if (first === 'new') return second ? auRecordPage('new', second) : auNewPage();
  if (/^\d+$/.test(first)) return auRecordPage(Number(first));
  return auListPage();
}
const auBack = () => ['automations', t('Automations')];
async function auLoad() {
  const [data, devices] = await Promise.all([api('automations'), api('automations/devices')]);
  au.data = data;
  au.devices = devices.devices;
  return data;
}
// Opened by hand without the right (a partition master, a standard user): the panel refuses, the page says who may.
function auRefusedPage() {
  return pageHead(t('Automations'), t('Things the panel does by itself.')) +
    notice('warn', t('Only the system master or the installer changes automations.'), t('Ask them, or sign in as one of them.'));
}
async function auGuard(load) {
  try { return await load(); } catch (error) { if (error.status === 403) return null; throw error; }
}
const auReadOnly = data => !!state.locked || !!data.read_only;
async function auListPage() {
  const data = await auGuard(() => api('automations'));
  if (!data) return auRefusedPage();
  au.data = data;
  const full = data.used >= data.maximum;
  let html = pageHead(t('Automations'), t('Things the panel does by itself.'),
    `<button type="button" class="primary" data-go="automations" data-arg="new"${full || auReadOnly(data) ? ' disabled' : ''}>${icon('plus')}${ht('Add an automation')}</button>`);
  if (state.locked) html += notice('info', t('Automations keep running while the panel is armed.'), t('Open one to see it; changes wait until every partition is disarmed and quiet.'));
  else if (data.read_only) html += notice('warn', t('Automations can’t be changed right now.'));
  if (!data.clock_set) html += notice('warn', t('The clock isn’t set, so times wait until it is.'));
  if (full) html += notice('info', t('16 is the most. Delete one to add another.'));
  if (!data.automations.length)
    return html + `<div class="card status-card"><p>${ht('The panel can do things by itself, like arming at bedtime or telling your phone when a door opens at night.')}</p></div>`;
  return {
    html: html + `<div id="au-rows">${auRows(data)}</div>`,
    bind() {
      let last = auRows(data);
      poll(async () => { // the state lines, at most every AU_POLL_MS, written only when they changed
        const fresh = await api('automations');
        au.data = fresh;
        const rows = auRows(fresh), box = $('#au-rows');
        if (rows !== last && box) { box.innerHTML = rows; last = rows; }
      }, AU_POLL_MS);
    },
  };
}
const auRows = data => `<div class="card"><ul class="list pc-list">${data.automations.map(auRow).join('')}</ul></div>
  <p class="au-count">${esc(t('{0} of 16 automations', data.used))}</p>`;
function auRow(a) {
  const ok = a.record === 'ok';
  const title = ok ? auWhenText(a.when) : t('Automation {0}', a.slot);
  const detail = ok ? a.then.map(auActionText).join(' · ') : '';
  const tags = auTags(a), st = auStatus(a, true);
  const tone = !ok || tags.some(x => x[1] === 'warn') ? 'warn' : a.on ? 'accent' : '';
  const glyph = ok && a.then.length ? auActionIcon(a.then[0]) : icon('zap');
  return `<li><a class="pc-person" href="#/automations/${a.slot}"><span class="pc-chip ${tone}">${glyph}</span>
    <span class="main"><strong>${esc(title)}${tags.map(([text, kind]) => ' ' + tag(text, kind)).join('')}</strong>
    ${detail ? `<span class="au-detail">${esc(detail)}</span>` : ''}${st ? `<span class="au-line ${st.tone}">${esc(st.text)}</span>` : ''}</span>${icon('chev', 'chev')}</a></li>`;
}

// ---- Add an automation: the touchscreen's ideas; one shows only when its parts exist --------------------------------

const auBlank = () => ({kind: 'device', source: '', ref: '', device: '', area: '', found: true, online: true, action: '', off_after: 0, confirmed: false});
// A new arm or disarm: the person saving it sets it up (the panel makes them the maker; the installer's Arm uses the system
// master's permission, BL-224); 5 minutes' warning (AUT-012).
const auNewArm = (level = 'night') => ({kind: 'arm', partition: auPartNumbers()[0] || 1, level, warn_min: 5, bypass: false, ...auArmMaker()});
function auNewDisarm(when) { // a partition its trigger doesn't arm, if the trigger is an arming (D31)
  const free = auDisarmParts(when);
  return {kind: 'disarm', partition: free[0] || auPartNumbers()[0] || 1, confirmed: false, made_by: state?.name || ''};
}
// BL-224: the actions without a device, as the editor holds them.
const auNewRemind = () => ({kind: 'remind', partition: auPartNumbers()[0] || 1, notify: (au.data?.notify || []).length > 0});
const auNewStillArmed = () => ({kind: 'still_armed', partition: auPartNumbers()[0] || 1});
const auNewChime = on => ({kind: 'chime', partition: auPartNumbers()[0] || 1, on});
const auNewNotify = () => ({kind: 'notify'});
const auNewOutput = (output, action = 'on', offAfter = 0) => ({kind: 'output', output, action, off_after: offAfter});
const auPicked = a => auIsAlarm(a) || auOwn(a) || !!a.ref;
function auThenOf(d, action, offAfter = 0, confirmed = false) {
  return {kind: 'device', source: d.source, ref: d.ref, device: d.name, area: d.area, found: true, online: d.online, action, off_after: offAfter, confirmed};
}
const auModel = (when, then) => ({on: true, when: {kind: '', partitions: [], levels: [], zone: 0, zone_name: '', minute: 1380, days: [...AU_DAYS], window: null, ...when},
  then: then || [auBlank()]});
// The ideas a new panel can use first (BL-224), each only when its parts exist; then the device ideas, then Start from scratch.
// The installer is offered the arming ones (it uses the system master's permission), never Disarm.
function auIdeas(data, devs) {
  const door = data.zones.find(z => ['door', 'window'].includes(z.class) && z.usable_open);
  const water = data.zones.find(z => z.class === 'water' && z.usable_open);
  const light = devs.find(d => d.kind === 'light' && auActs(d).includes('on'));
  const lock = devs.find(d => d.kind === 'lock' && auActs(d).includes('lock'));
  const valve = devs.find(d => d.kind === 'valve' && auActs(d).includes('close'));
  const output = data.outputs?.[0], weekdays = AU_DAYS.slice(0, 5);
  const out = [
    {id: 'bedtime', glyph: icon('moon'), title: t('Arm at bedtime'), detail: t('Every night at a time you choose, after a warning'),
      make: () => auModel({kind: 'time', minute: 1350}, [auNewArm('night')])},
    {id: 'morning', glyph: icon('away'), title: t('Arm Away on weekday mornings'), detail: t('After everyone has left, with a warning'),
      make: () => auModel({kind: 'time', minute: 510, days: weekdays}, [auNewArm('away')])},
    {id: 'remind', glyph: icon('bell'), title: t('Remind me to arm'), detail: t('A chime and a note if it’s still disarmed at bedtime'),
      make: () => auModel({kind: 'time', minute: 1380}, [auNewRemind()])},
    {id: 'still-armed', glyph: icon('send'), title: t('Tell me if it’s still armed'), detail: t('Like nobody home from school by 4:00 PM'),
      make: () => auModel({kind: 'time', minute: 960, days: weekdays}, [auNewStillArmed()])},
  ];
  if (!auInstaller() && data.less_secure !== false) out.push({id: 'open', glyph: icon('lockOpen'), title: t('Disarm at opening time'), detail: t('For a shop or an office, on weekdays'),
    make: () => auModel({kind: 'time', minute: 480, days: weekdays}, [auNewDisarm({kind: 'time'})])});
  if (door) out.push({id: 'door', glyph: icon('door'), title: t('Tell me when a door opens at night'), detail: t('A notification to your phone, 10:00 PM to 6:00 AM'),
    make: () => auModel({kind: 'zone_opened', zone: door.number, zone_name: door.name, window: {from: 1320, to: 360}}, [auNewNotify()])});
  out.push({id: 'chime', glyph: icon('bell'), title: t('Quiet the door chime at night'), detail: t('Off at bedtime; another can turn it on in the morning'),
    make: () => auModel({kind: 'time', minute: 1320}, [auNewChime(false)])});
  if (output) out.push({id: 'output', glyph: icon('toggle'), title: t('Switch an output when an alarm sounds'), detail: t('Like the garden lights, for 30 minutes'),
    make: () => auModel({kind: 'alarm'}, [auNewOutput(output.number, 'on', 30)])});
  if (door && light) out.push({id: 'door-light', glyph: icon('bulb'), title: t('Turn on a light when a door opens'), detail: t('Like the porch light, in the evening'),
    make: () => auModel({kind: 'zone_opened', zone: door.number, zone_name: door.name, window: {from: 1080, to: 360}}, [auThenOf(light, 'on', 10)])});
  if (lock) out.push({id: 'lock-away', glyph: icon('lock'), title: t('Lock up when you arm Away'), detail: t('Lock a door when everyone has left'),
    make: () => auModel({kind: 'armed', levels: ['away']}, [auThenOf(lock, 'lock')])});
  if (light) out.push({id: 'alarm-lights', glyph: icon('siren'), title: t('Turn on lights when an alarm sounds'), detail: t('So you can see, and be seen'),
    make: () => auModel({kind: 'alarm'}, [auThenOf(light, 'on')])});
  if (water && valve) out.push({id: 'leak-water', glyph: icon('drops'), title: t('Close the water when a leak is found'), detail: t('Shut the water main'),
    make: () => auModel({kind: 'zone_opened', zone: water.number, zone_name: water.name}, [auThenOf(valve, 'close')])});
  out.push({id: 'scratch', glyph: icon('zap'), title: t('Start from scratch'), detail: t('Choose when it runs and what it does'), make: () => auModel({})});
  return out;
}
async function auNewPage() {
  const data = await auGuard(auLoad);
  if (!data) return auRefusedPage();
  if (state.locked) return pageHead(t('Add an automation'), t('Armed: automations can be changed once every partition is disarmed and quiet.'), '', auBack());
  if (data.read_only) return pageHead(t('Add an automation'), t('Automations can’t be changed right now.'), '', auBack());
  if (data.used >= data.maximum) return pageHead(t('Add an automation'), t('16 is the most. Delete one to add another.'), '', auBack());
  return pageHead(t('Add an automation'), t('Pick an idea to start from. Nothing is saved until you press Save.'), '', auBack()) +
    `<div class="card"><ul class="list pc-list">${auIdeas(data, au.devices).map(i => `<li><a class="pc-person" href="#/automations/new/${i.id}">
      <span class="pc-chip accent">${i.glyph}</span><span class="main"><strong>${esc(i.title)}</strong><span class="au-detail">${esc(i.detail)}</span></span>${icon('chev', 'chev')}</a></li>`).join('')}</ul></div>`;
}

// ---- One automation -------------------------------------------------------------------------------------------

const auClone = x => JSON.parse(JSON.stringify(x));
async function auRecordPage(which, idea = '') {
  const data = await auGuard(auLoad);
  if (!data) return auRefusedPage();
  const isNew = which === 'new';
  let rec = null, model;
  if (isNew) {
    if (auReadOnly(data) || data.used >= data.maximum) return auNewPage();
    const made = auIdeas(data, au.devices).find(i => i.id === idea);
    if (!made) return auNewPage();
    model = made.make();
  } else {
    rec = data.automations.find(a => a.slot === which);
    if (!rec) return pageHead(t('Automation'), t('There is no such automation.'), '', auBack());
    if (rec.record !== 'ok') return auBrokenPage(rec);
    model = {on: rec.on, when: auClone(rec.when), then: auClone(rec.then)};
  }
  const said = au.said;
  Object.assign(au, {slot: isNew ? 0 : which, isNew, rec, before: auClone(model), now: auClone(model), locked: auReadOnly(data), said: ''});
  const title = isNew ? t('New automation') : auWhenText(rec.when);
  const sub = isNew ? t('Nothing is saved until you press Save.') : rec.then.map(auActionText).join(' · ');
  // Armed: this page's own notice, the only one (panel.js lockedNotice leaves Automations to it).
  const why = state.locked ? notice('warn', t('Armed: automations can be changed once every partition is disarmed and quiet.'))
    : data.read_only ? notice('warn', t('Automations can’t be changed right now.')) : '';
  return {
    html: pageHead(title, sub, '', auBack()) + why + `<p class="au-say" id="au-say" role="alert">${esc(said)}</p>
      <div id="au-root"><form id="au-body" novalidate>${auBody()}</form><div id="pc-dock"><span id="au-dock">${auDock()}</span></div></div>`,
    bind() {
      const root = $('#au-root');
      root.addEventListener('change', auInput);
      root.addEventListener('click', auClick);
      root.addEventListener('submit', event => event.preventDefault());
      if (!isNew) poll(auRefresh, AU_POLL_MS);
    },
  };
}
// The state line of a saved automation (Last run), read again; written only when its words changed. The gen the
// page saves with stays the one it was opened with, so another person's edit meanwhile is refused as changed.
// The string last written is kept (as the list's poll keeps its own): the browser reads innerHTML back changed (esc()'s
// &#39; comes back as '), so comparing with it would rewrite a name with an apostrophe every time.
async function auRefresh() {
  const fresh = await api('automations');
  const mine = fresh.automations.find(a => a.slot === au.slot && a.uid === au.rec?.uid);
  if (!mine || !au.rec) return;
  au.rec.status = mine.status;
  const box = $('#au-last'), html = au.isNew ? '' : auLastLine();
  if (box && html !== au.lastLine) { box.innerHTML = html; au.lastLine = html; }
}
// A saved automation the panel cannot run (made by newer software, or broken): only Delete works (AUT-008).
function auBrokenPage(rec) {
  const newer = rec.record === 'newer';
  Object.assign(au, {slot: rec.slot, isNew: false, rec, locked: auReadOnly(au.data)});
  return {
    html: pageHead(t('Automation {0}', rec.slot), '', '', auBack()) +
      notice('warn', newer ? t('Made by newer software: it can only be deleted.') : t('Can’t run: open it and pick the device again')) +
      `<p class="au-say" id="au-say" role="alert"></p><div id="au-root">
      <section class="card gap pc-card"><div class="rows">${pcRowHtml('au-on', t('Use this automation'), auStatus(rec).text, '<input id="au-on" type="checkbox" class="switch" disabled>')}</div></section>` +
      (au.locked ? '' : pcCard('', `<div class="rows">${auDeleteRow()}</div>`, 'pc-apart')) + '</div>',
    bind() {
      // On the page's own root, which goes with the page: never #main's, which would outlive it.
      $('#au-root')?.addEventListener('click', event => { if (event.target.closest('[data-au="delete"]')) auDelete(); });
    },
  };
}
const auDeleteRow = () => pcRowHtml('', t('Delete this automation'), t('It stops at once. This can’t be undone.'),
  `<button type="button" class="danger" data-au="delete" data-k="delete">${ht('Delete')}</button>`);

function auBody() {
  return `<fieldset class="au-set"${au.locked ? ' disabled' : ''}>${auOnCard()}${auWhenCard()}${auWindowCard()}${auThenCard()}</fieldset>` +
    (au.isNew || au.locked ? '' : pcCard('', `<div class="rows">${auDeleteRow()}</div>`, 'pc-apart'));
}
function auLastHtml() {
  if (au.isNew) return '';
  return (au.lastLine = auLastLine());
}
function auLastLine() {
  const tried = ['tried', 'failed'].includes(au.rec?.status?.result);
  const last = au.now.on || tried ? auStatus({...au.rec, on: true}) : {text: t('Off: it won’t run'), tone: ''};
  return `<dl class="kv"><dt>${ht('Last run')}</dt><dd class="au-line ${last.tone}">${esc(last.text)}</dd></dl>`;
}
function auOnCard() {
  return pcCard('', `<div class="rows">${pcRowHtml('au-on', t('Use this automation'), au.isNew ? t('It starts working when you save it.') : '',
    `<input id="au-on" type="checkbox" class="switch" data-f="on" data-k="on"${au.now.on ? ' checked' : ''}>`)}</div>${au.isNew ? '' : `<div class="au-kv" id="au-last">${auLastHtml()}</div>`}`);
}
const auOptions = (list, value) => list.map(([v, l]) => `<option value="${esc(v)}"${String(v) === String(value) ? ' selected' : ''}>${esc(l)}</option>`).join('');
function auWhenCard() {
  const w = au.now.when, rows = [], kinds = auKinds();
  const hint = kinds.find(k => k[0] === w.kind)?.[2];
  rows.push(pcRowHtml('au-kind', t('Starts when'), hint || '', `<select id="au-kind" data-f="kind" data-k="kind">${w.kind ? '' :
    `<option value="" selected disabled>${ht('Choose when it runs')}</option>`}${auOptions(kinds.map(k => [k[0], k[1]]), w.kind)}</select>`));
  if (auMany() && ['armed', 'disarmed'].includes(w.kind))
    rows.push(pcRowHtml('au-part', t('Which partition?'), '', `<select id="au-part" data-f="part" data-k="part">${auOptions([['', t('Any of them')],
      ...au.data.partitions.map(p => [p.number, auPartName(p.number)])], w.partitions[0] || '')}</select>`));
  if (w.kind === 'armed')
    rows.push(pcRowHtml('au-level', t('Armed how?'), '', `<select id="au-level" data-f="level" data-k="level">${auOptions([['', t('Any of them')],
      ['away', t('Away')], ['stay', t('Stay')], ['night', t('Night')]], w.levels[0] || '')}</select>`));
  if (w.kind === 'zone_opened' || w.kind === 'zone_closed') {
    const zones = au.data.zones.filter(z => w.kind === 'zone_opened' ? z.usable_open : z.usable_close);
    rows.push(pcRowHtml('au-zone', w.kind === 'zone_opened' ? t('Which sensor?') : t('Which door or window?'), t('Silent, fire and button zones aren’t listed.'),
      zones.length ? `<select id="au-zone" data-f="zone" data-k="zone">${auOptions(zones.map(z => [z.number, z.name || tc('default zone name', 'Zone {0}', z.number)]), w.zone)}</select>`
        : `<select id="au-zone" disabled><option>${ht('No doors or windows to choose')}</option></select>`));
  }
  if (w.kind === 'trouble')
    rows.push(pcRowHtml('au-trouble', t('Which trouble?'), t('It runs once when the trouble starts.'),
      `<select id="au-trouble" data-f="trouble" data-k="trouble">${auOptions(auTroubles(), w.trouble || 'any')}</select>`));
  if (w.kind === 'time') {
    rows.push(pcRowHtml('au-time', t('Run at'), '', `<select id="au-time" data-f="minute" data-k="minute">${auOptions(auTimes(), w.minute)}</select>`));
    rows.push(pcRowHtml('', t('On these days'), [auRunsText(w), t('It runs once a day at most, and waits while the clock isn’t set.')],
      `<div class="checks">${AU_DAYS.map(d => `<label><input type="checkbox" data-f="day" data-d="${d}" data-k="day-${d}"${w.days.includes(d) ? ' checked' : ''}>${esc(auShortDay(d))}</label>`).join('')}</div>`));
  }
  return pcCard(t('When'), `<div class="rows">${rows.join('')}</div>`) + `<p class="help au-count">${ht('Silent alarms and duress never start an automation.')}</p>`;
}
function auWindowCard() {
  const w = au.now.when;
  if (!w.kind || w.kind === 'time') return '';
  const win = w.window, times = auTimes();
  const rows = [pcRowHtml('au-from', t('From'), '', `<select id="au-from" data-f="from" data-k="from">${auOptions([[-1, t('Any time')], ...times], win ? win.from : -1)}</select>`)];
  if (win) rows.push(pcRowHtml('au-to', t('To'), t('From {0} to {1}', minuteText(win.from), minuteText(win.to)),
    `<select id="au-to" data-f="to" data-k="to">${auOptions(times, win.to)}</select>`));
  return pcCard(t('Only between'), `<div class="rows">${rows.join('')}</div>`);
}
// The device list, one group per area, scenes apart, after "The alarm", "Tell someone" and "Outputs" (BL-224).
function auThenGroups() {
  const groups = new Map();
  for (const d of au.devices) {
    const g = d.kind === 'scene' ? t('Scenes') : d.area || t('Other');
    if (!groups.has(g)) groups.set(g, []);
    groups.get(g).push(d);
  }
  return groups;
}
// "The alarm", Then's first group (AUT-011, BL-224): "Arm the panel" after any trigger while it is the only action, the
// installer's too; "Disarm" as the first action, with at most one other action that is no arming, never after an alarm
// trigger (D22), with the option on (D25), nor where it could only undo an arming (D31), never the installer's; Remind to
// arm and "still armed" after a time, alone; the door chime on or off. A stored one is always listed, so it shows as it is.
function auAlarmOptions(i) {
  const n = au.now, cur = n.then[i], w = n.when, out = [];
  const alone = i === 0 && n.then.length === 1, other = n.then.find((x, k) => k !== i);
  if (cur?.kind === 'arm' || alone) out.push(['alarm:arm', t('Arm the panel')]);
  const room = !other || (!auIsAlarm(other) && !AU_TIME_ONLY.includes(other.kind));
  const free = auDisarmParts(w).length > 0;
  if (cur?.kind === 'disarm' || (!auInstaller() && i === 0 && room && au.data?.less_secure !== false && !AU_ALARMS.includes(w.kind) && free))
    out.push(['alarm:disarm', t('Disarm')]);
  if (AU_TIME_ONLY.includes(cur?.kind) || (w.kind === 'time' && alone))
    out.push(['panel:remind', t('Remind to arm')], ['panel:still_armed', t('Tell me if it’s still armed')]);
  if (cur?.kind === 'chime' || other?.kind !== 'chime') out.push(['panel:chime-on', t('Turn the door chime on')], ['panel:chime-off', t('Turn the door chime off')]);
  return out;
}
// The select's value of an action: a device's source:ref, or what it is in "The alarm", "Tell someone" or "Outputs".
const auPickValue = a => a.kind === 'chime' ? (a.on ? 'panel:chime-on' : 'panel:chime-off') : a.kind === 'output' ? 'panel:output:' + a.output
  : auOwn(a) ? 'panel:' + a.kind : auIsAlarm(a) ? 'alarm:' + a.kind : '';
function auDeviceSelect(a, i) {
  const gone = a.kind === 'device' && a.ref && a.found === false, mine = auPickValue(a), other = au.now.then.find((x, k) => k !== i);
  let html = `<option value=""${(!a.ref && !mine) || gone ? ' selected' : ''}>${gone ? ht('Pick another device') : ht('Choose what it does')}</option>`;
  const alarm = auAlarmOptions(i);
  if (alarm.length) html += `<optgroup label="${esc(t('The alarm'))}">${auOptions(alarm, mine)}</optgroup>`;
  if (a.kind === 'notify' || other?.kind !== 'notify')
    html += `<optgroup label="${esc(t('Tell someone'))}">${auOptions([['panel:notify', t('Send a notification')]], mine)}</optgroup>`;
  // The wired outputs "For automations"; a stored one that no longer is stays listed, so it shows as it is
  const outs = (au.data?.outputs || []).filter(o => !(other?.kind === 'output' && other.output === o.number));
  if (a.kind === 'output' && !auOutput(a.output)) outs.push({number: a.output, name: auOutputName(a.output)});
  if (outs.length) html += `<optgroup label="${esc(t('Outputs'))}">${auOptions(outs.map(o => ['panel:output:' + o.number, o.name || t('Output {0}', o.number)]), mine)}</optgroup>`;
  for (const [group, list] of auThenGroups())
    html += `<optgroup label="${esc(group)}">${list.map(d => `<option value="${esc(d.source + ':' + d.ref)}"${a.kind === 'device' && a.source === d.source && a.ref === d.ref && !gone ? ' selected' : ''}>${
      esc(d.online ? d.name : t('{0} · not reachable now', d.name))}</option>`).join('')}</optgroup>`;
  return `<select id="au-dev-${i}" data-f="dev" data-i="${i}" data-k="dev-${i}">${html}</select>`;
}
// Try it now: devices, outputs, the door chime and a notification; never an arm, a disarm, a reminder or "still armed",
// which mean something only at their trigger.
const auTriable = a => (a.kind === 'device' && a.ref && a.found !== false && !auBlocked(a)) || ['output', 'chime', 'notify'].includes(a.kind);
function auThenCard() {
  const then = au.now.then, rows = [];
  if (!au.now.when.kind) return pcCard(t('Then'), `<div class="rows">${pcRowHtml('', t('Choose when it runs'), t('Then pick what it does.'), '')}</div>`);
  then.forEach((a, i) => {
    const dev = a.kind === 'device' ? au.devices.find(d => d.source === a.source && d.ref === a.ref) : null;
    const remove = then.length > 1 ? `<button type="button" class="small" data-au="rm" data-i="${i}" data-k="rm-${i}">${ht('Remove this action')}</button>` : '';
    let block = pcRowHtml('', auPicked(a) ? auActionText(a) : t('Choose what it does'), auPicked(a) ? auActionDetail(a) : '', remove);
    block += pcRowHtml(`au-dev-${i}`, t('What it does'), '', auDeviceSelect(a, i));
    if (auIsAlarm(a)) { rows.push(`<div class="au-then">${block}${auAlarmRows(a, i)}</div>`); return; }
    if (auOwn(a)) { rows.push(`<div class="au-then">${block}${auPanelRows(a, i)}</div>`); return; }
    // A stored action the installer option now hides stays as it is, marked (AUT-015): never changed for the person.
    const acts = dev ? auActs(dev).map(x => [x, auVerb(x)]) : [];
    if (dev && a.action && !auActs(dev).includes(a.action)) acts.push([a.action, t('{0} · can’t run', auVerb(a.action))]);
    if (acts.length > 1) block += pcRowHtml(`au-act-${i}`, t('Action'), '', `<select id="au-act-${i}" data-f="act" data-i="${i}" data-k="act-${i}">${auOptions(acts, a.action)}</select>`);
    if (a.action === 'on') block += pcRowHtml(`au-off-${i}`, t('Turn it off after'), '', `<select id="au-off-${i}" data-f="off" data-i="${i}" data-k="off-${i}">${auOptions(
      [[0, t('Leave it on')], [5, auMinutes(5)], [10, auMinutes(10)], [15, auMinutes(15)], [30, auMinutes(30)], [60, auMinutes(60)]], a.off_after)}</select>`);
    rows.push(`<div class="au-then">${block}</div>`);
  });
  // A second action: beside a device, an output, the door chime, a notification or a Disarm; never with Arm, Remind to
  // arm or "still armed" (AUT-011, BL-224)
  const one = then[0];
  if (then.length === 1 && (one.kind === 'disarm' || (one.kind === 'device' && one.ref) || ['output', 'chime', 'notify'].includes(one.kind)))
    rows.push(pcRowHtml('', t('Add a second action'), '', `<button type="button" class="small" data-au="add2" data-k="add2">${icon('plus')}${ht('Add a second action')}</button>`));
  if (!au.isNew && !au.locked && then.every(auTriable)) {
    const dirty = auChanges().length > 0;
    rows.push(pcRowHtml('', t('Try it now'), dirty ? t('Save your changes first.') : t('Runs the actions once, now.'),
      `<button type="button" class="small" data-au="try" data-k="try"${dirty ? ' disabled' : ''}>${icon('play')}${ht('Try it now')}</button>`));
  }
  // No device yet: a quiet line where they will be (BL-224). The installer option is off (D25): Unlock, Open and Stop
  // aren't offered, and the page says why.
  const none = au.devices.length ? '' : `<p class="help au-count">${ht('Lights, locks and other devices show here once one is connected.')}</p>`;
  const off = au.data?.less_secure === false ? `<p class="help au-count">${esc(auOffNote())}</p>` : '';
  return pcCard(t('Then'), `<div class="rows">${rows.join('')}</div>`) + none + off;
}
// The rows of a reminder, "still armed", the door chime, a notification or an output (BL-224).
function auPanelRows(a, i) {
  const rows = [], w = au.now.when;
  if (auMany() && ['remind', 'still_armed', 'chime'].includes(a.kind))
    rows.push(pcRowHtml(`au-pn-part-${i}`, t('Which partition?'), '',
      `<select id="au-pn-part-${i}" data-f="pn-part" data-i="${i}" data-k="pn-part-${i}">${auOptions(auPartNumbers().map(n => [n, auPartName(n)]), a.partition)}</select>`));
  if (a.kind === 'remind') {
    rows.push(pcRowHtml(`au-rn-${i}`, t('Also send a notification'), auWhere() ? t('To {0}', auWhere()) : t('No phone notifications are set up yet.'),
      `<input id="au-rn-${i}" type="checkbox" class="switch" data-f="remind-notify" data-i="${i}" data-k="rn-${i}"${a.notify ? ' checked' : ''}>`));
    rows.push(pcRowHtml('', t('At {0}, if the panel isn’t armed: a chime and a note on the panel.', minuteText(w.minute)),
      t('It never arms by itself. The note goes once the panel is armed, or at 6:00 AM.'), ''));
  }
  if (a.kind === 'chime' && !a.on && w.kind === 'time') rows.push(pcRowHtml('', t('Nothing turns it on again. Add one for the morning.'), '', ''));
  if (a.kind === 'output') {
    rows.push(pcRowHtml(`au-oa-${i}`, t('Action'), '', `<select id="au-oa-${i}" data-f="out-act" data-i="${i}" data-k="oa-${i}">${
      auOptions([['on', t('Turn on')], ['off', t('Turn off')]], a.action)}</select>`));
    if (a.action === 'on') rows.push(pcRowHtml(`au-of-${i}`, t('Turn it off after'), '', `<select id="au-of-${i}" data-f="out-off" data-i="${i}" data-k="of-${i}">${auOptions(
      [[0, t('Leave it on')], [5, auMinutes(5)], [10, auMinutes(10)], [15, auMinutes(15)], [30, auMinutes(30)], [60, auMinutes(60)]], a.off_after)}</select>`));
  }
  return rows.join('');
}

// The rows of an arm or a disarm (AUT-011..AUT-014; the touchscreen mockup screens/automations.js, D29-D31).
function auAlarmRows(a, i) {
  const rows = [], w = au.now.when, partRow = f => {
    if (!auMany()) return '';
    // Disarm offers only what the saver may disarm (the touchscreen's disarm_partitions); a stored one stays shown.
    const offer = f === 'disarm-part' ? auPartNumbers().filter(n => n === a.partition || auDisarmParts(w).includes(n)) : auPartNumbers();
    return pcRowHtml(`au-${f}-${i}`, t('Which partition?'), '',
      `<select id="au-${f}-${i}" data-f="${f}" data-i="${i}" data-k="${f}-${i}">${auOptions(offer.map(n => [n, auPartName(n)]), a.partition)}</select>`);
  };
  if (a.kind === 'disarm') {
    rows.push(partRow('disarm-part'));
    const part = auPartName(a.partition);
    if (auMadeBy(a)) rows.push(pcRowHtml('', t('Set up by {0}', auMadeBy(a)), t('It never disarms while an alarm sounds, and stops if {0} can no longer disarm {1}.', auMadeBy(a), part), ''));
    return rows.join('');
  }
  rows.push(pcRowHtml(`au-arm-level-${i}`, t('Arm how?'), '', `<select id="au-arm-level-${i}" data-f="arm-level" data-i="${i}" data-k="arm-level-${i}">${
    auOptions([['away', t('Away')], ['stay', t('Stay')], ['night', t('Night')]], a.level)}</select>`));
  rows.push(partRow('arm-part'));
  rows.push(pcRowHtml(`au-arm-warn-${i}`, t('Warn first'), '', `<select id="au-arm-warn-${i}" data-f="arm-warn" data-i="${i}" data-k="arm-warn-${i}">${
    auOptions(Array.from({length: 15}, (_, n) => [n + 1, auMinutes(n + 1)]), a.warn_min)}</select>`));
  // "Bypass open zones" (D29, off by default): only while the installer allows less secure automations (D31)
  const less = au.data?.less_secure !== false;
  if (less) rows.push(pcRowHtml(`au-arm-bypass-${i}`, t('Bypass open zones'), [a.bypass ? t('Arms anyway; each one is watched again once it closes')
    : t('Off: it won’t arm while a door or window is open'), t('Fire, CO and 24-hour zones are never bypassed. It does nothing if the panel is already armed. Anyone can put it off with their code.')],
    `<input id="au-arm-bypass-${i}" type="checkbox" class="switch" data-f="arm-bypass" data-i="${i}" data-k="arm-bypass-${i}"${a.bypass ? ' checked' : ''}>`));
  const level = auLevelWord(a.level);
  const result = w.kind === 'time' ? t('Warns at {0}, arms {1} at {2}.', minuteText(((w.minute - a.warn_min) % 1440 + 1440) % 1440), level, minuteText(w.minute))
    : t('Warns for {0}, then arms {1}.', auMinutes(a.warn_min), level);
  rows.push(pcRowHtml('', result, [less ? '' : t('It arms only if every door and window is closed and nobody armed it already. Anyone can put it off with their code.'),
    a.level === 'away' ? t('Inside sensors will be on.') : ''], ''));
  return rows.join('');
}

// ---- Editing ---------------------------------------------------------------------------------------------------

function auChanges() {
  const b = au.before, n = au.now, same = (x, y) => JSON.stringify(x) === JSON.stringify(y), out = [];
  if (!same({...b.when, window: null}, {...n.when, window: null})) out.push(t('When'));
  if (!same(b.when.window, n.when.window)) out.push(t('Only between'));
  if (!same(b.then, n.then)) out.push(t('Then'));
  if (au.isNew && b.on !== n.on) out.push(t('Use this automation'));
  return out;
}
function auDock() {
  if (au.locked) return '';
  const changes = auChanges();
  if (!au.isNew && !changes.length) return '<span class="pc-error" id="au-error" role="alert"></span>';
  const top = au.isNew ? t('New automation') : tn('{0} change', '{0} changes', changes.length);
  const line = au.isNew ? auWhenText(au.now.when) : listWords(changes);
  return `<div class="pc-dock"><span class="main"><strong>${esc(top)}</strong><span>${esc(line)}</span><span class="pc-error" id="au-error" role="alert"></span></span>
    ${au.isNew ? '' : `<button type="button" data-au="undo" data-k="undo">${ht('Undo')}</button>`}
    <button type="button" class="primary" data-au="save" data-k="save">${ht('Save')}</button></div>`;
}
function auRedraw(focus = '') {
  const body = $('#au-body');
  if (!body) return;
  body.innerHTML = auBody();
  $('#au-dock').innerHTML = auDock();
  if (focus) $(`[data-k="${focus}"]`)?.focus();
}
function auSay(message) {
  const line = $('#au-error') || $('#au-say');
  if (line) line.textContent = message;
  const top = $('#au-say');
  if (message && top && line !== top) top.textContent = '';
}
// Someone else's edit came first: the page reads the panel again and says so (the gen, DESIGN.md section 10).
function auReload(error) {
  au.said = error.code === 'changed' ? t('Someone changed automations meanwhile. The page now shows them as they are.') : auMessage(error);
  return show();
}
function auRefused(error) {
  if (error.code === 'changed') return auReload(error);
  if (error.code === 'locked' || error.code === 'read_only') { au.locked = true; auRedraw(); auSay(auMessage(error)); return; }
  auRedraw();
  auSay(auMessage(error));
}
function auSetKind(kind) {
  const w = au.now.when;
  Object.assign(w, {kind, partitions: [], levels: [], zone: 0, zone_name: ''});
  if (kind === 'trouble') w.trouble = 'any'; else delete w.trouble;
  if (kind === 'zone_opened' || kind === 'zone_closed') {
    const z = au.data.zones.find(x => kind === 'zone_opened' ? x.usable_open : x.usable_close);
    w.zone = z?.number || 0;
    w.zone_name = z?.name || '';
  }
  if (kind === 'time') { w.window = null; if (!w.days.length) w.days = [...AU_DAYS]; }
}
// A scene, or a switch turned on or toggled, is asked about once (D8, D9): the pick waits for "Use it".
function auAsk(i, next, dev) {
  const old = au.now.then[i];
  const asked = auAsksOnce(dev, next.action) && !(old.ref === next.ref && old.confirmed && old.action === next.action);
  if (!asked) { au.now.then[i] = next; return auRedraw(); }
  auRedraw(); // the old pick stays on screen until it is confirmed
  review({
    title: t('Use {0} in an automation?', dev.name),
    intro: dev.kind === 'scene' ? t('The panel can’t see what a scene does. If it unlocks or opens anything, it will do so every time this runs.')
      : t('Use it only for a light or a plug, never for a switch that opens a door, a gate or a garage.'),
    confirm: t('Use it'), done: '',
    run: async () => { next.confirmed = true; au.now.then[i] = next; },
    after: () => auRedraw(`dev-${i}`),
  });
}
function auInput(event) {
  const el = event.target, f = el.dataset?.f;
  if (!f) return;
  const n = au.now, w = n.when, i = Number(el.dataset.i || 0);
  if (f === 'on') {
    if (au.isNew) { n.on = el.checked; return auRedraw('on'); }
    return auSwitch(el.checked);
  }
  // A changed trigger is a new pairing: each less secure action is confirmed again at Save (D24).
  if (['kind', 'zone', 'part'].includes(f)) n.then.forEach(a => { if (auLessSecure(a)) a.confirmed = false; });
  if (f === 'kind') {
    auSetKind(el.value);
    // An alarm trigger drops a Disarm (D22: an automation never silences an alarm); any but a time drops a reminder
    // and "still armed", which look at the panel at their time (BL-224)
    const drop = a => (AU_ALARMS.includes(el.value) && a.kind === 'disarm') || (el.value !== 'time' && AU_TIME_ONLY.includes(a.kind));
    if (n.then.some(drop)) {
      n.then = n.then.filter(a => !drop(a));
      if (!n.then.length) n.then.push(auBlank());
    }
    return auRedraw('kind');
  }
  const x = n.then[i];
  if (x && auIsAlarm(x) && f.startsWith(x.kind === 'arm' ? 'arm-' : 'disarm-')) { // the rows of an arm or a disarm
    if (f === 'arm-level') x.level = el.value;
    else if (f === 'arm-warn') x.warn_min = Number(el.value);
    else if (f === 'arm-bypass') x.bypass = !!el.checked;
    else if (f === 'arm-part' || f === 'disarm-part') { x.partition = Number(el.value); x.confirmed = false; } // a new pairing (D24)
    if (x.kind === 'arm') Object.assign(x, auArmMaker()); // whoever saves it sets it up (the installer's: as the master)
    else x.made_by = state?.name || x.made_by;
    return auRedraw(el.dataset.k);
  }
  if (f === 'part') w.partitions = el.value ? [Number(el.value)] : [];
  else if (f === 'level') w.levels = el.value ? [el.value] : [];
  else if (f === 'zone') { w.zone = Number(el.value); w.zone_name = auZone(w)?.name || ''; }
  else if (f === 'minute') w.minute = Number(el.value);
  else if (f === 'day') w.days = AU_DAYS.filter(d => d === el.dataset.d ? el.checked : w.days.includes(d));
  else if (f === 'from') w.window = Number(el.value) < 0 ? null : {from: Number(el.value), to: w.window ? w.window.to : (Number(el.value) + 720) % 1440};
  else if (f === 'to') w.window = {from: w.window.from, to: Number(el.value)};
  else if (f === 'trouble') w.trouble = el.value;
  else if (f === 'remind-notify') n.then[i].notify = !!el.checked;
  else if (f === 'pn-part') n.then[i].partition = Number(el.value);
  else if (f === 'out-act') { n.then[i].action = el.value; if (el.value !== 'on') n.then[i].off_after = 0; }
  else if (f === 'out-off') n.then[i].off_after = Number(el.value);
  else if (f === 'dev' && el.value.startsWith('panel:')) { // BL-224: the alarm's own, a notification, an output
    const v = el.value.slice(6), old = n.then[i];
    const next = v === 'remind' ? auNewRemind() : v === 'still_armed' ? auNewStillArmed() : v === 'chime-on' ? auNewChime(true)
      : v === 'chime-off' ? auNewChime(false) : v === 'notify' ? auNewNotify() : auNewOutput(Number(v.split(':')[1]));
    if (old?.kind === next.kind && 'partition' in old) next.partition = old.partition;
    if (AU_TIME_ONLY.includes(next.kind)) n.then = [next]; else n.then[i] = next; // alone
  } else if (f === 'dev') {
    const d = au.devices.find(x => x.source + ':' + x.ref === el.value), old = n.then[i];
    if (el.value === 'alarm:arm') { n.then = [old.kind === 'arm' ? old : auNewArm()]; return auRedraw(el.dataset.k); }
    if (el.value === 'alarm:disarm') { n.then[i] = old.kind === 'disarm' ? old : auNewDisarm(w); return auRedraw(el.dataset.k); }
    if (!d) n.then[i] = auBlank();
    else {
      const action = auActs(d).includes(old.action) ? old.action : auActs(d)[0];
      const same = old.source === d.source && old.ref === d.ref && old.action === action;
      return auAsk(i, auThenOf(d, action, action === 'on' ? old.off_after || 0 : 0, same && old.confirmed), d);
    }
  } else if (f === 'act') {
    const a = n.then[i], d = au.devices.find(x => x.source === a.source && x.ref === a.ref);
    return auAsk(i, {...a, action: el.value, off_after: el.value === 'on' ? a.off_after : 0, confirmed: false}, d);
  } else if (f === 'off') n.then[i].off_after = Number(el.value);
  else return;
  auRedraw(el.dataset.k);
}
async function auClick(event) {
  const el = event.target.closest('[data-au]');
  if (!el || el.disabled) return;
  const act = el.dataset.au, i = Number(el.dataset.i || 0);
  if (act === 'undo') { au.now = auClone(au.before); auSay(''); return auRedraw(); }
  if (act === 'rm') { au.now.then.splice(i, 1); return auRedraw(); }
  if (act === 'add2') { au.now.then.push(auBlank()); return auRedraw('dev-1'); }
  if (act === 'save') return auSave();
  if (act === 'try') return auTry();
  if (act === 'delete') return auDelete();
}
// What the server takes (DESIGN.md section 10): the page's words; it maps them to the model itself.
// An arm's "Bypass open zones" is sent only while the option is on (D31): off, the switch isn't offered and the
// automation arms without bypassing, which is what saving it then says.
const auWireAction = a => a.kind === 'arm' ? {kind: 'arm', partition: a.partition, level: a.level, warn_min: a.warn_min, bypass: au.data?.less_secure !== false && !!a.bypass}
  : a.kind === 'disarm' ? {kind: 'disarm', partition: a.partition, confirmed: !!a.confirmed}
  // BL-224: without a device
  : a.kind === 'output' ? {kind: 'output', output: a.output, action: a.action, off_after: a.action === 'on' ? a.off_after || 0 : 0}
  : a.kind === 'remind' ? {kind: 'remind', partition: a.partition, notify: !!a.notify}
  : a.kind === 'still_armed' ? {kind: 'still_armed', partition: a.partition}
  : a.kind === 'chime' ? {kind: 'chime', partition: a.partition, on: !!a.on}
  : a.kind === 'notify' ? {kind: 'notify'}
  : {kind: 'device', source: a.source, ref: a.ref, action: a.action, off_after: a.action === 'on' ? a.off_after || 0 : 0, confirmed: !!a.confirmed};
const auWire = n => ({on: n.on, when: {kind: n.when.kind, partitions: n.when.partitions, levels: n.when.levels, zone: n.when.zone,
  minute: n.when.minute, days: n.when.kind === 'time' ? n.when.days : [], window: n.when.kind === 'time' ? null : n.when.window,
  ...(n.when.kind === 'trouble' ? {trouble: n.when.trouble || 'any'} : {})},
  then: n.then.map(auWireAction)});
const auSend = wire => api('automations', {action: 'save', slot: au.slot, gen: au.data.gen, automation: wire});
const auSaved = r => { if (au.isNew || r.slot === null) go('automations'); else go('automations', String(r.slot)); }; // no toast: the page shows it
// "Save this automation?" (D24, D31, AUT-016): one whole sentence for each less secure action after a door, window,
// motion, alarm or armed trigger; Save sends them confirmed. Also what a needs_confirm refusal opens: when the panel
// then asks for another action's, the page asks again for that one, as often as it is refused (each index once).
function auConfirmSave(ask) {
  const w = au.now.when;
  au.again = null;
  review({
    title: t('Save this automation?'), intro: ask.map(a => auConfirmText(w, a) || t('It will do this by itself every time it runs.')).join(' '), confirm: t('Save'), done: '',
    run: async () => {
      const wire = auWire(au.now);
      wire.then.forEach((a, i) => { if (ask.includes(au.now.then[i])) a.confirmed = true; });
      try { au.saved = await auSend(wire); } catch (error) {
        if (error.code === 'changed') { closeDialog(); return auReload(error); }
        const next = au.now.then[error.detail?.index];
        if (error.code === 'needs_confirm' && next && !ask.includes(next)) { ask.forEach(a => { a.confirmed = true; }); au.again = next; return; }
        throw new Error(auMessage(error));
      }
      ask.forEach(a => { a.confirmed = true; });
    },
    after: () => {
      const next = au.again;
      au.again = null;
      if (next) { next.confirmed = false; auRedraw(); return auConfirmSave([next]); }
      if (au.saved) auSaved(au.saved);
    },
  });
}
async function auSave() {
  const n = au.now, w = n.when;
  au.saved = null;
  if (!w.kind || n.then.some(a => !auPicked(a))) return auSay(t('Choose when it runs and what it does.'));
  if (n.then.some(a => auUndoes(w, a))) return auSay(t('It would undo the arming at once.')); // D31: refused at save
  if (w.kind === 'time' && !w.days.length) return auSay(t('Pick at least one day'));
  if ((w.kind === 'zone_opened' || w.kind === 'zone_closed') && !w.zone) return auSay(t('That sensor can’t start an automation.'));
  auSay('');
  const ask = n.then.filter(a => auNeedsConfirm(w, a) && !a.confirmed);
  if (ask.length && au.data.less_secure !== false) return auConfirmSave(ask);
  const button = $('[data-au="save"]');
  if (button) button.disabled = true;
  try {
    auSaved(await auSend(auWire(n)));
  } catch (error) {
    // The panel asks for the confirm the page didn't know was due: ask it for that action, then send again.
    const a = n.then[error.detail?.index];
    if (error.code === 'needs_confirm' && a) { a.confirmed = false; auRedraw(); return auConfirmSave([a]); }
    auRefused(error);
  }
}
async function auSwitch(on) {
  try {
    const r = await api('automations', {action: 'switch', slot: au.slot, gen: au.data.gen, on});
    au.data.gen = r.gen;
    au.now.on = on;
    au.before.on = on;
    au.rec.on = on;
    auRedraw('on');
    auSay('');
  } catch (error) {
    auRefused(error);
  }
}
async function auTry() {
  try {
    const r = await api('automations', {action: 'try', slot: au.slot, gen: au.data.gen});
    au.data.gen = r.gen;
    au.rec.status = r.status; // the outcome goes into Last run: no toast
    auRedraw('try');
    auSay('');
  } catch (error) {
    auRefused(error);
  }
}
function auDelete() {
  const slot = au.slot, gen = au.data?.gen;
  review({
    title: t('Delete this automation?'), intro: t('It stops at once. This can’t be undone.'),
    confirm: t('Delete'), danger: true, done: '',
    run: async () => {
      try { await api('automations', {action: 'delete', slot, gen}); } catch (error) {
        if (error.code === 'changed') { closeDialog(); au.saved = 'changed'; return auReload(error); }
        throw new Error(auMessage(error));
      }
    },
    after: () => { if (au.saved !== 'changed') go('automations'); au.saved = null; },
  });
}

const PAGES = {
  overview: {title: tx('Overview'), icon: 'home', group: '', render: overviewPage},
  keypad: {title: tx('Remote keypad'), icon: 'monitor', group: '', render: keypadPage, when: () => !!state?.capabilities?.video?.length},
  activity: {title: tx('Activity'), icon: 'activity', group: '', render: activityPage},
  zones: {title: tx('Zones'), icon: 'grid', group: '', render: zonesPage},
  troubles: {title: tx('Troubles'), icon: 'alert', group: '', render: troublesPage, badge: () => troubleBadge()},
  people: {title: tx('People and codes'), icon: 'users', group: tx('Household'), access: 'users', render: peoplePage},
  integrations: {title: tx('Integrations'), icon: 'plug', group: tx('Household'), access: 'system', render: integrationsPage},
  integration: {title: tx('Integration'), icon: 'plug', nav: false, parent: 'integrations', access: 'system', render: integrationPage},
  // docs/features/41 DESIGN.md section 10: the system master's and the installer's, on a build with automations.
  automations: {title: tx('Automations'), icon: 'zap', group: tx('Household'), access: 'system', render: auPage, when: () => !!state?.capabilities?.automations},
  display: {title: tx('Display and sound'), icon: 'sun', group: tx('Panel settings'), render: displayPage},
  network: {title: tx('Network'), icon: 'wifi', group: tx('Panel settings'), access: 'shared', render: networkPage},
  time: {title: tx('Date and time'), icon: 'clock', group: tx('Panel settings'), render: timePage},
  storage: {title: tx('Storage'), icon: 'drive', group: tx('Panel settings'), render: storagePage},
  // Installer > Zones first, then Z-Wave, as on the touchscreen (BL-134). Sensor sources and the sensor link live on
  // the zone pages now: their old addresses (#/sensors, #/binding/5) open those.
  'zone-setup': {title: tx('Zones'), icon: 'door', group: tx('Installer'), access: 'installer', render: zpSetupPage},
  sensors: {title: tx('Zones'), icon: 'door', nav: false, parent: 'zone-setup', access: 'installer', render: () => zpMoved('')},
  binding: {title: tx('Zones'), icon: 'door', nav: false, parent: 'zone-setup', access: 'installer', render: arg => zpMoved(arg)},
  zwave: {title: tx('Z-Wave'), icon: 'radio', group: tx('Installer'), access: 'installer', render: zwavePage, when: () => !!state?.capabilities?.zwave},
  zwaveradio: {title: tx('Z-Wave radio'), icon: 'radio', nav: false, parent: 'zwave', access: 'installer', render: zwaveRadioPage, when: () => !!state?.capabilities?.zwave},
  // Zigbee (devices.md §14, BL-145): right after Z-Wave, as on the touchscreen; on every panel that runs it, an empty socket included.
  zigbee: {title: nt('Zigbee'), icon: 'radio', group: tx('Installer'), access: 'installer', render: zgMainPage, when: () => !!state?.capabilities?.zigbee},
  'zigbee-add': {title: tx('Add a Zigbee device'), icon: 'radio', nav: false, parent: 'zigbee', access: 'installer', render: zgAddPage, when: () => !!state?.capabilities?.zigbee},
  'zigbee-code': {title: tx('Add with an install code'), icon: 'radio', nav: false, parent: 'zigbee', access: 'installer', render: zgCodePage, when: () => !!state?.capabilities?.zigbee},
  'zigbee-device': {title: tx('Zigbee device'), icon: 'radio', nav: false, parent: 'zigbee', access: 'installer', render: zgDevicePage, when: () => !!state?.capabilities?.zigbee},
  'zigbee-radio': {title: tx('Zigbee radio'), icon: 'radio', nav: false, parent: 'zigbee', access: 'installer', render: zgRadioPage, when: () => !!state?.capabilities?.zigbee},
  'zigbee-channel': {title: tx('Zigbee channel'), icon: 'radio', nav: false, parent: 'zigbee', access: 'installer', render: zgChannelPage, when: () => !!state?.capabilities?.zigbee},
  partitions: {title: tx('Partitions and devices'), icon: 'layers', group: tx('Installer'), access: 'installer', render: partitionsPage},
  options: {title: tx('System options'), icon: 'sliders', group: tx('Installer'), access: 'installer', render: optionsPage},
  access: {title: tx('Access and maintenance'), icon: 'key', group: tx('Installer'), access: 'installer', render: accessPage},
  health: {title: tx('System health'), icon: 'heart', group: tx('System'), render: healthPage},
  walk: {title: tx('Walk test'), icon: 'walk', group: tx('System'), access: 'shared', render: walkPage, badge: walkBadge, badgeKind: 'info'},
  updates: {title: tx('Software updates'), icon: 'download', group: tx('System'), access: 'system', render: updatesPage},
  developer: {title: tx('Developer'), icon: 'code', group: tx('System'), access: 'installer', render: developerPage, when: () => state?.developer},
  password: {title: tx('Your password'), icon: 'lock', nav: false, render: passwordPage, when: () => state?.has_password},
};
