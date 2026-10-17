// BL-048 proposal P2, redrawn simple: keypad updates through the panel link (docs/features/11
// "Updates through the link" and its "What people see"; Decisions, "Keypad updates" 1-8).
//
// The 31 screens approved on 2026-10-17 were replaced the same day. The owner: they are "too
// complicated"; "the new key seems unnecessary, changes from the master get pushed to the slaves";
// "updates should not be paused on the keypad: it should still receive the update when armed but not
// apply it until the system is disarmed, for a quick reboot"; "99% of the time the remote keypad
// should update at the same time as the master" (chosen: keypads restart together with the master);
// "you are doing this the complicated way". Later the same day: the panel is for someone's grandma,
// and nobody (not even an installer, gone after installation) acts on what it says, so no screen asks
// anyone to do anything.
//
// So the master tells its keypads which version to have; each gets it (downloads it, or receives it
// from the master) while it keeps protecting, armed or not, and restarts together with the master once
// everything is disarmed. A keypad that misses an update tries again by itself.
//
// Drawn, 8 screens, one cast (the master Harbor House; its keypads Hall keypad 2, Bedroom keypad 3,
// Kitchen 4):
//   the master  bl-048-fw-master-keypads           Software update while it downloads, with its keypads
//               bl-048-fw-master-keypads-done      after the update: two up to date, one not yet
//               bl-048-fw-master-install-question  the Install question: its keypads update with it
//               bl-048-fw-installer-keypads        Installer > Keypads and devices: "Updating" while one restarts
//               bl-048-fw-master-activity          Activity's keypad rows
//   a keypad    bl-048-fw-kp-about                 About: Software update "From Harbor House"
//   (Hall)      bl-048-fw-kp-update                Software update: where updates come from, and the run
//               bl-048-fw-kp-home                  Home with today's steady update banner
//
// Removed from the approved drawing, and why: the key screens (a keypad takes its master's server and
// key from its orders, decision 1); Stop updating keypads and Try again now (failures are tried again
// by themselves, decision 8); the turn-taking rows and the first-keypad-per-model (canary) rows
// (keypads restart together with the master, decisions 7 and 8); the keypad's paused, ready, stopped,
// failed and rolled-back pages and its Stop question (downloading goes on while armed, decision 6, and
// a keypad has no Install and no Stop); every row that told someone what to do, and About's amber
// "keypads behind" (nobody acts on them).
//
// Nothing blinks: words, steady tones and today's stepped bar; a percent changes at most once a second
// (UI-020, UI-023).
//
// Open questions, with what this proposal recommends:
//  Q1 The master's own Stop the update (today's) stays while its keypads follow it, and stopping it
//     ends their download too. Recommended: keep it (decision 8 takes away the keypads' Stop and Try
//     again only); otherwise the master's page loses its Stop as well.
//  Q2 A keypad of another model without internet (Kitchen, a 10.1" panel): the master fetches that
//     model's image and sends it (the owner, 2026-10-17), where docs/features/11 still says "no image
//     fetched for another model". Recommended: change the spec's rules and "Not done" to match.
//  Q3 The keypad's banner names its master, "Updating · restarts with Harbor House"; a banner is cut
//     with "..." and a name can be 47 bytes. Recommended: keep the name (the page says it whole); else
//     "Updating · restarts with the alarm panel".

const P2 = (replaces, why) => ({bl: 'BL-048', status: 'approved', decided: '2026-10-17: Approve, recommendations OK', ...(replaces ? {replaces} : {}), why});
// A banner with new words: outline it as changed.
const newBanner = (html) => html.replace('class="bar_banner', 'class="changed bar_banner');
// A confirm dialog whose note has new words: outline the note (its only body text).
const changedNote = (html) => html.replace('<span class="font_body"', '<span class="changed font_body"');

// ================================ The master ================================
// Software update as today (screens/updates-run.js), the Settings list as BL-189 has it.
const MS_SECTIONS = M.settings_sections({drop: ['installer']}).filter((s) => !s.divider);
const msUpd = ({id, title, notes, proposal, body, bar, tags = {}, over}) => M.screen({
  id, area: 'Settings', title, shot: null, proposal, notes,
  render: () => M.main({view: 'settings', rail: {}, bar, over,
    content: M.settings({section: 'about', sections: MS_SECTIONS, title: 'Software update', back: 'settings-about', tags, body: body.join('')})}),
});

// One keypad's row: its name; its number and version; one status line in its tone. Not tappable.
const kpRow = (name, n, version, state, tone) => M.row_link_alert({glyph: 'keyboard', color: tone, title: name,
  detail: `Keypad ${n} · version ${version}`, sub: state, subColor: tone === 'gray' ? 'text_2' : tone, trail: ''});
// The Keypads section: its heading, one line, a row per keypad; no buttons.
const keypads = (...rows) => M.changed([M.section('Keypads'),
  M.paragraph('Keypads update with this panel. They restart together once everything is disarmed.'), ...rows].join(''));
const UP_TO_DATE = (name, n, v) => kpRow(name, n, v, 'Up to date', 'green');

// Today's steps (screens/updates-run.js), the first running.
const STEPS = [M.section('Steps'), ...[
  ['Download', '2.7 MB'],
  ['Check the download', 'Its signature and checksum'],
  ['Wait until everything is disarmed', 'Right away when it already is'],
  ['Restart with the new version', 'About a minute; the self-test starts at once'],
  ['Self-test', 'Usually a minute, at most three · restarts with 0.6.1 if it fails'],
].map(([title, detail], i) => M.row_link({glyph: i === 0 ? 'circle-dot' : 'circle', color: i === 0 ? 'blue' : 'gray', title, detail, trail: ''}))].join('');

msUpd({id: 'bl-048-fw-master-keypads', title: 'Master: downloading its update, its keypads with it',
  proposal: P2('settings-update-downloading', 'Whoever looks sees that the keypads update with this panel, and how far each one is; nobody has to do anything.'),
  notes: 'Today\'s page with the Keypads section under the run row, only on a panel with keypads; today\'s Stop, its paragraph and the steps follow below it (Q1). The status line, in its tone: "Downloading · 45 %" and "Receiving from this panel · 30 %" (blue), "Ready · restarts with this panel" (blue), "Restarting" (blue), "Up to date" (green), "Not updated yet · tries again by itself" (gray). The percent at most once a second. Armed, downloading goes on and the rows say the same.',
  bar: {banner: {glyph: 'download', tone: 'blue', text: 'Downloading an update'}, time: '6:52', ampm: 'AM'},
  tags: {about: {text: 'Updating', tone: 'blue'}},
  body: [
    M.row_status({glyph: 'download', color: 'blue', title: 'Downloading version 0.6.2', detail: '1.1 of 2.7 MB · 42 %', level: 42}),
    keypads(
      kpRow('Hall keypad', 2, '0.6.1', 'Downloading · 45 %', 'blue'),
      kpRow('Bedroom keypad', 3, '0.6.1', 'Receiving from this panel · 30 %', 'blue'),
      kpRow('Kitchen', 4, '0.6.1', 'Ready · restarts with this panel', 'blue')),
    M.section(''),
    M.row_action({glyph: 'circle-x', text: 'Stop the update', color: 'red', goTo: 'settings-update-stop-question'}),
    M.paragraph('The alarm keeps working while it downloads. You can stop the update until the panel restarts.'),
    STEPS,
  ]});

msUpd({id: 'bl-048-fw-master-keypads-done', title: 'Master: updated, one keypad not yet',
  proposal: P2('settings-update-updated', 'After the update the page says which keypads have it; one that missed it tries again by itself.'),
  notes: 'One gray line for every keypad that missed the version, whatever the reason: "Not updated yet · tries again by itself". It does: a failed download after an hour and at this panel\'s next version; one that went back, at the next version. The reason is in Activity and the console only. No amber, no trouble, no banner.',
  bar: {banner: null, time: '7:31', ampm: 'AM'},
  body: [
    M.row_status({glyph: 'circle-check', color: 'green', title: 'Updated to version 0.6.2', detail: '7:14 AM · it passed its self-test'}),
    keypads(
      UP_TO_DATE('Hall keypad', 2, '0.6.2'),
      UP_TO_DATE('Bedroom keypad', 3, '0.6.2'),
      kpRow('Kitchen', 4, '0.6.1', 'Not updated yet · tries again by itself', 'gray')),
    M.section(''),
    M.row_link({glyph: 'refresh-cw', title: 'Check again', detail: 'Checked 7:17 AM', trail: ''}),
  ]});

msUpd({id: 'bl-048-fw-master-install-question', title: 'Master: Install, its keypads update with it',
  proposal: P2('settings-update-install-question', 'Whoever installs knows the keypads restart too.'),
  notes: 'Today\'s question; only the note\'s end is new, and only on a panel with keypads. Behind it, the page with its Keypads section, all up to date on 0.6.1.',
  bar: {banner: null, time: '6:50', ampm: 'AM'},
  tags: {about: {text: 'Update', tone: 'violet'}},
  body: [
    M.row_link({glyph: 'sparkles', color: 'violet', title: 'Version 0.6.2 is ready to install', detail: 'You have 0.6.1 · the alarm keeps working while it downloads', trail: ''}),
    M.row_action({glyph: 'download', text: 'Install version 0.6.2', color: 'blue'}),
    M.paragraph('The panel restarts once everything is disarmed. It takes about a minute.'),
    keypads(UP_TO_DATE('Hall keypad', 2, '0.6.1'), UP_TO_DATE('Bedroom keypad', 3, '0.6.1'), UP_TO_DATE('Kitchen', 4, '0.6.1')),
  ],
  over: changedNote(M.confirm_dialog({title: 'Install version 0.6.2?', tone: 'primary', ok: 'Install',
    note: 'The panel downloads it now and keeps protecting meanwhile; you can stop the update until it restarts. It restarts once everything is disarmed, then its keypads update with it.'}))});

// Installer > Keypads and devices as built (screens/panel-link.js installer-devices-keypads).
const instTable = (skip) => Array.from({length: 16}, (_, i) => i + 1).filter((d) => !skip.includes(d))
  .map((d) => M.row_link({glyph: 'cpu', color: 'gray', title: `Device ${d}`, detail: 'Not in use', goTo: d === 1 ? 'installer-device' : ''})).join('');
M.screen({id: 'bl-048-fw-installer-keypads', area: 'Installer', title: 'Installer: Keypads and devices, a keypad restarting for its update', shot: null,
  proposal: P2('installer-devices-keypads', 'A keypad restarting for its update isn\'t lost: its row says so.'),
  notes: 'While a keypad restarts with the update (up to 10 minutes, its trouble held meanwhile), its row says "Updating" (blue) instead of "Not responding". After 10 minutes without it, today\'s row and trouble.',
  render: () => M.main({view: 'settings', tone: 'blue', rail: {tone: 'blue'}, bar: {banner: 'installer', time: '10:58'},
    content: M.settings({section: 'about', sections: MS_SECTIONS, title: 'Keypads and devices', note: 'A supervised device that stops answering shows a trouble', back: 'installer-menu', body: [
      M.section('Keypads'),
      M.row_action({glyph: 'plus', text: 'Add a keypad', color: 'blue', goTo: 'keypad-add-open'}),
      M.row_link({glyph: 'keyboard', color: 'green', title: 'Hall keypad', detail: 'Keypad 2 · Home', value: 'Connected', goTo: 'keypad-page'}),
      M.changed(M.row_link_alert({glyph: 'keyboard', color: 'blue', title: 'Bedroom keypad', detail: 'Keypad 3 · Home', value: 'Updating', valueColor: 'blue', goTo: 'keypad-page'})),
      M.section('Supervised devices'),
      instTable([2, 3]),
    ].join('')})}),
});

M.screen({id: 'bl-048-fw-master-activity', area: 'Views', title: 'Activity on the master: keypad updates', shot: null,
  proposal: P2('activity-log', 'What each keypad\'s update did stays in the master\'s event log, as its own updates do.'),
  notes: 'Another morning than bl-048-fw-master-keypads-done: the keypads restarted with this panel at 7:12; Kitchen, off then, caught up later and its download broke off. The keypad by its number, as "Keypad 3 not responding". A failed update says why on its line; nothing to do: it tries again by itself. "Keypad 3 installing an update" is logged at the restart too (not drawn).',
  render: () => M.main({view: 'activity', rail: {}, bar: {banner: null, time: '7:31', ampm: 'AM'}, content: `<div class="view column">
    ${M.view_header({title: 'Activity', note: 'Everything the panel did, newest first'})}
    <div class="row" style="flex:none">${M.segments(['All', 'Alarms', 'Troubles', 'Arming', 'Bypass', 'System'], 5)}</div>
    <div class="list scroll">${[
      M.changed(M.event_row({glyph: 'refresh-cw', title: 'Keypad 4 update failed', detail: 'The download broke off', time: '7:25 AM', tone: 'amber'})),
      M.changed(M.event_row({glyph: 'refresh-cw', title: 'Keypad 2 update rolled back', detail: 'The new version failed; the previous one runs', time: '7:14 AM', tone: 'amber'})),
      M.changed(M.event_row({glyph: 'refresh-cw', title: 'Keypad 3 updated', detail: 'The new version passed its self-test', time: '7:14 AM', tone: 'blue'})),
      M.event_row({glyph: 'refresh-cw', title: 'Firmware updated', detail: 'The new version passed its self-test', time: '7:14 AM', tone: 'blue'}),
      M.event_row({glyph: 'refresh-cw', title: 'Installing an update', detail: 'The panel restarts with the new version', time: '7:12 AM', tone: 'blue'}),
    ].join('')}</div></div>`}),
});

// ================================ A keypad (Hall keypad) ================================
// The keypad's frame, as screens/panel-link.js draws it (kp, KP_SECTIONS).
const KP_SECTIONS = M.settings_sections({drop: ['users', 'integrations', 'automations', 'walk', 'installer']}).filter((s) => !s.divider);
const KP_BANNER = {glyph: 'download', tone: 'blue', text: 'Updating · restarts with Harbor House', goTo: 'bl-048-fw-kp-update'};
const kpFrame = (m) => newBanner(M.main({...m, rail: {devices: true}, bar: {time: '6:52', ampm: 'AM', ...m.bar}})
  .replace('data-go="devices"', 'data-go="kp-devices"').replace('data-go="home-ready"', 'data-go="bl-048-fw-kp-home"'));

M.screen({id: 'bl-048-fw-kp-about', area: 'Keypad', title: 'Keypad: About, its updates from Harbor House', shot: null,
  proposal: P2('kp-about', 'A keypad follows its master: About says where its updates come from.'),
  notes: 'kp-about with only the Software update row changed: "From Harbor House" on its line (a long name wraps there). While an update runs the row\'s value says "Updating" (blue), as on any panel.',
  render: () => kpFrame({view: 'settings', bar: {banner: null},
    content: M.settings({sections: KP_SECTIONS, section: 'about', title: 'About this panel', body: [
      M.row_status({glyph: 'shield-check', color: 'green', title: 'RoboAlarms Panel', detail: 'Version 0.6.1 · Panel ID b7c8d9e0f1a2'}),
      M.row_status({glyph: 'keyboard', color: 'green', title: 'Keypad 2 of Harbor House', detail: 'Connected · shows and arms Home'}),
      M.changed(M.row_link({glyph: 'refresh-cw', title: 'Software update', detail: 'From Harbor House', goTo: 'bl-048-fw-kp-update'})),
      M.row_link({glyph: 'hard-drive', color: 'violet', title: 'SD card', detail: '44.7 GB free of 59.6 GB', value: 'Ready'}),
      M.section('This panel'),
      M.row_value({title: 'Built', value: 'Sep 19, 2026'}),
      M.row_value({title: 'Running for', value: '3 days'}),
    ].join('')})}),
});

M.screen({id: 'bl-048-fw-kp-update', area: 'Keypad', title: 'Keypad: Software update, updating with Harbor House', shot: null,
  proposal: P2('settings-update', 'A keypad runs its master\'s version, by itself: the page only says where updates come from and how far one is.'),
  notes: 'No Install, no Check again, no Stop, no steps. With no update running, the first row alone. While one runs, today\'s run row with its stepped bar and one line: "Downloading · 45 %" (or "Receiving from Harbor House · 30 %"), then "Ready · restarts with Harbor House once everything is disarmed". The percent at most once a second.',
  render: () => kpFrame({view: 'settings', bar: {banner: KP_BANNER},
    content: M.settings({sections: KP_SECTIONS, section: 'about', title: 'Software update', back: 'bl-048-fw-kp-about', tags: {about: {text: 'Updating', tone: 'blue'}}, body: M.changed([
      M.row_status({glyph: 'house', color: 'blue', title: 'Updates come from Harbor House', detail: 'Version 0.6.1'}),
      M.row_status({glyph: 'download', color: 'blue', title: 'Updating to version 0.6.2', detail: 'Downloading · 45 %', level: 45}),
    ].join(''))})}),
});

M.screen({id: 'bl-048-fw-kp-home', area: 'Keypad', title: 'Keypad: Home, the update banner', shot: null,
  proposal: P2('kp-home-ready', 'Whoever passes a keypad that is updating knows it will restart with the alarm panel.'),
  notes: 'Today\'s steady blue update banner, on a keypad: "Updating · restarts with Harbor House", from the first byte to the restart, armed or not; the same words throughout. It gives way to an alarm as today. A tap opens Software update (with the code). Nothing covers Home, the pad or Panic.',
  render: () => kpFrame({bar: {banner: KP_BANNER},
    content: M.home({tone: 'green', glyph: 'shield-check', headline: 'Disarmed', detail: 'Ready to arm', note: 'All zones closed'}, M.panel_arm())}),
});
