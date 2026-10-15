// Settings > Installer (ui_installer.c): locked, then installer mode with arming off. Who may open
// what follows docs/features/14 "Who can change what".

const INST_RAIL = {troubles: 1, tone: 'blue'};
const INST_BAR = {banner: 'installer', time: '10:52', troubles: 1};
const inst = (meta, page, extra = {}) => M.screen({
  ...meta, area: 'Installer', ...extra,
  render: () => M.main({view: 'settings', tone: 'blue', rail: INST_RAIL, bar: INST_BAR,
    content: M.settings({section: 'installer', ...page()}), ...(extra.main ? extra.main() : {})}),
});

M.screen({
  id: 'installer-lost-code', area: 'Installer', title: 'Installer, before the code: a lost code', shot: 'installer_lost_code',
  notes: 'BL-131 small fix: the lost-code paragraph became a row that opens this page, before the code.',
  render: () => M.main({view: 'settings', rail: {troubles: 1}, bar: {time: '10:52', troubles: 1}, content: M.settings({
    section: 'installer', back: 'installer-locked', title: 'Lost the installer code?', note: '',
    body: [
      M.paragraph('Unplug the panel for ten seconds. For two minutes after it starts, Installer opens without the code (unless the installer turned that off).'),
    ].join('')})}),
});

inst({id: 'installer-options', title: 'System options', shot: 'installer_options'}, () => ({
  title: 'System options', note: 'Saved when you go back · SIA CP-01 limits apply', back: 'installer-menu',
  body: [
    M.paragraph('No zone is a night zone yet, so Night arms like Stay.'),
    M.section('Delays'),
    M.row_number({title: 'Exit time', detail: 'Time to leave after arming', value: '60 s'}),
    M.row_number({title: 'Entry time 1', detail: 'Time to disarm after the main door opens', value: '30 s'}),
    M.row_number({title: 'Entry time 2', detail: 'For a second door, like the garage', value: '45 s'}),
    M.row_number({title: 'Report delay', detail: 'Disarm within this time and no burglary alarm is sent', value: '30 s'}),
    M.row_number({title: 'Siren time', detail: 'How long a burglary alarm sounds', value: '4 min'}),
    M.row_toggle({title: 'Fire and CO sirens stop by themselves', detail: 'Off: they sound until silenced (recommended)'}),
    M.row_number({title: 'Cancel window', detail: 'Disarming this soon after an alarm was reported sends a cancel', value: '5 min'}),
    M.section('Alarms'),
    M.row_link({glyph: 'siren', color: 'red', title: 'Panic keys', detail: 'What the keypad\'s A, B and C keys send'}),
    M.row_link({glyph: 'bell', color: 'amber', title: 'Trouble beeps', detail: 'Which troubles make the keypad beep'}),
    M.section('Arming'),
    M.row_toggle({title: 'Auto-stay', detail: 'Away becomes Stay when nobody leaves through a door', on: true}),
    M.row_toggle({title: 'Exit beeps', detail: 'Beep through Away exit time (faster in the last 10 seconds)', on: true}),
    M.row_toggle({title: 'Arm without a code', detail: 'Quick arm. Disarming always needs a code'}),
    M.row_toggle({title: 'Allow less secure automations', detail: 'Automations may disarm, unlock and open', on: true}),
    M.section('Codes'),
    M.row_number({title: 'Shortest new code', detail: 'Existing shorter codes keep working', value: '4'}),
    M.row_number({title: 'Wrong codes before lockout', detail: 'The keypad locks for a while', value: '5'}),
  ].join(''),
}), {notes: 'Abridged: the full list is ui_installer.c build_options(). "Allow less secure automations" (AUT-015, D25) shows only on a build with automations, on by default; the installer alone sees it. Off, the automation pickers don\'t offer Unlock, Open, Stop or Disarm, a save holding one is refused, and a stored one is kept but shows "Can\'t run" and is never sent.'});

const ZONE_ROWS = [
  ['door-closed', 'Front Door', 'Zone 1 · Entry/exit 1 · Simulator'],
  ['warehouse', 'Garage Door', 'Zone 2 · Entry/exit 2 · Simulator'],
  ['app-window', 'Living Room Window', 'Zone 3 · Perimeter · Simulator'],
  ['door-closed', 'Back Door', 'Zone 4 · Perimeter · Simulator'],
  ['footprints', 'Hallway Motion', 'Zone 5 · Interior follower · Simulator'],
  ['alarm-smoke', 'Kitchen Smoke', 'Zone 6 · Fire · Simulator'],
];
inst({id: 'installer-zones', title: 'Zones (installer)', shot: 'installer_zones'}, () => ({
  title: 'Zones', note: '8 of 128 zones in use', back: 'installer-menu',
  body: M.row_action({glyph: 'plus', text: 'Add a zone', color: 'blue'}) +
    ZONE_ROWS.map(([g, t, d]) => M.row_link({glyph: g, color: 'green', title: t, detail: d, goTo: t === 'Hallway Motion' ? 'zone-edit' : ''})).join(''),
}));

const zoneEditBody = () => [
  M.row_status({glyph: 'footprints', color: 'green', title: 'No motion', detail: 'Simulator · For testing: nothing is wired, the service console opens and closes it.'}),
  M.section('The zone'),
  M.row_link({glyph: 'tag', title: 'Name', value: 'Hallway Motion'}),
  M.row_link({glyph: 'mic', title: 'Spoken name', value: 'Choose words', goTo: 'zone-spoken-empty'}),
  M.row_link({glyph: 'shield', title: 'Type', value: 'Interior follower (04)', goTo: 'zone-type'}),
  M.section('Its sensor'),
  M.row_link({glyph: 'footprints', color: 'green', title: 'Sensor', value: 'Motion'}),
  M.row_link({glyph: 'wrench', title: 'Source', value: 'Simulator'}),
  M.row_link({glyph: 'sliders-horizontal', title: 'Sensor settings', detail: 'Open when on · 350 ms · not supervised', goTo: 'zone-sensor'}),
  M.section('What it does'),
  M.row_toggle({title: 'Night zone', detail: 'Armed in Night too, not only Away', on: true}),
  M.row_toggle({title: 'Chime', detail: 'Ding when it opens while disarmed and chime is on'}),
  M.row_toggle({title: 'Report this zone', detail: 'Its events go to phones and integrations, not only the event log', on: true}),
  M.section('Advanced'),
  M.row_link({glyph: 'shield', title: 'Cross-zone partner', detail: 'Both must trip close together to cause an alarm', value: 'None'}),
  M.row_link({glyph: 'siren', title: 'Alarm limit', detail: 'Stops sounding after this many alarms in one arming', value: 'System setting'}),
  M.section(''),
  M.row_action({glyph: 'trash-2', text: 'Remove this zone', color: 'red'}),
].join('');
inst({id: 'zone-edit', title: 'One zone', shot: 'zone_edit'}, () => ({
  title: 'Hallway Motion', note: 'Zone 5 · saved when you go back', back: 'installer-zones', body: zoneEditBody(),
}), {notes: 'How the zone is right now on top: not a button; its words change at most once a second, after the input settles (UI-023); its detail is the source and what it promises (amber when a warning). Then four groups and Remove apart (BL-131 finding 23). Partition shows with more than one partition, "Acts as" for a button, "Can be bypassed" for a 24-hour zone. Spoken name, right after Name, is the words the panel says for the zone (BL-202; its states are the zone-spoken-* screens), only in a build with a voice. Contact and Response time are on Sensor settings only; Alarm limit is one picker (System setting, 1 to 6 times, Never) over the two stored fields the stepper and the "Never shut down" switch set. The preview also shoots it scrolled: zone_edit_sensor, zone_edit_end.'});

// ---- A zone's spoken name (BL-202; docs/features/08 voice-first-build.md D01-D03, D07, D09) ----
// Front Door on the real en-US voice, as the preview walks it (walk_voice). Words from the pack are
// data, shown as recorded; everything else is the interface's words.
const SN_WHY = 'A zone needs the words the panel will say for it. The display name is free text and can\'t be spoken; the installer picks recorded words, and nothing is said until they are approved.';
const snZoneBody = (spoken, name = 'Front Door') => [
  M.row_status({glyph: 'door-closed', color: 'green', title: 'Closed', detail: 'Simulator · For testing: nothing is wired, the service console opens and closes it.'}),
  M.section('The zone'),
  M.row_link({glyph: 'tag', title: 'Name', value: name}),
  spoken,
  M.row_link({glyph: 'shield', title: 'Type', value: 'Entry/exit 1 (01)'}),
  M.section('Its sensor'),
  M.row_link({glyph: 'door-open', color: 'green', title: 'Sensor', value: 'Door or window'}),
  M.row_link({glyph: 'wrench', title: 'Source', value: 'Simulator'}),
  M.row_link({glyph: 'sliders-horizontal', title: 'Sensor settings', detail: 'Open when on · 350 ms · not supervised'}),
  M.section('What it does'),
  M.row_toggle({title: 'Chime', detail: 'Ding when it opens while disarmed and chime is on', on: true}),
  M.row_toggle({title: 'Report this zone', detail: 'Its events go to phones and integrations, not only the event log', on: true}),
  M.section(''),
  M.row_action({glyph: 'trash-2', text: 'Remove this zone', color: 'red'}),
].join('');
const snRow = (value, detail) => M.row_link({glyph: 'mic', title: 'Spoken name', value, detail, goTo: 'spoken-editor-words'});
const snPage = (spoken, name = 'Front Door') => ({title: name, note: 'Zone 1 · saved when you go back', back: 'installer-zones', body: snZoneBody(spoken, name)});
const snZone = (id, title, shot, row, notes, name) => inst({id, title, shot}, () => snPage(row, name), {notes: notes + ' ' + SN_WHY});

snZone('zone-spoken-chosen', 'Zone page: spoken name chosen', 'voice_zone_chosen', snRow('Front Door'),
  'D03. Spoken name sits right after Name. The row shows the chosen words, cut with "..." only in this summary (40 bytes). Tap opens the editor. Zone editing is installer-only (the zone page\'s gate).');
snZone('zone-spoken-empty', 'Zone page: no spoken name yet', 'voice_zone_empty', snRow('Choose words'),
  'A new zone starts here. The editor\'s Use zone name makes a draft from the name.');
snZone('zone-spoken-review', 'Zone page: spoken name needs review', 'voice_zone_review',
  snRow('Review spoken name', 'The name, partition or sensor changed'),
  'Changing the name, partition or sensor keeps the words but clears the approval; nothing is said until the installer has looked and pressed Done. The row says so as soon as the draft differs from what was approved (here the name), before the save. A draft saved without approval (too long to say, or Done without a ready voice) shows the same.',
  'Front Door 2nd floor');
snZone('zone-spoken-off', 'Zone page: announcements are off', 'voice_zone_off', snRow('Announcements are off', 'Front Door'),
  'The words stay saved. The value says why nothing is spoken, the detail keeps the phrase.');
snZone('zone-spoken-nopack', 'Zone page: voice pack not installed', 'voice_zone_nopack', snRow('Voice pack not installed'),
  'The words stay saved. Without a pack there are no recorded words to name them by, so the row has no phrase under it. The editor still opens, with Add word, Use zone name and Preview dimmed.');
snZone('zone-spoken-unavailable', 'Zone page: voice unavailable', 'voice_zone_unavailable', snRow('Voice is unavailable'),
  'A pack is installed but can\'t be used (or is still being checked at start-up): the words stay saved, the editor dims what needs the voice.');

// The editor: a form sheet (ui_form_sheet_open) with Cancel and Done over the zone page.
const snEditor = ({words = [], voice = 'ready', unmatched = '', long = false, name = 'Front Door'}) => {
  const full = words.length >= 8;
  const ok = voice === 'ready';
  return M.sheet({title: 'Spoken name', note: name, width: 760, body: `<div class="list scroll">
    ${M.section('Words, in the order they are said')}
    ${words.length ? words.map((w, i) => M.row_link({glyph: 'audio-lines', title: w, value: String(i + 1)})).join('')
      : M.paragraph('No words yet. Add words, or use the zone name.')}
    ${M.row_action({glyph: 'plus', text: 'Add word', color: 'blue', disabled: full || !ok})}
    ${full ? M.paragraph('Eight words is the most.') : ''}
    ${M.section('Or')}
    ${M.row_action({glyph: 'text-cursor-input', text: 'Use zone name', color: 'blue', disabled: !ok})}
    ${unmatched}
    ${M.row_action({glyph: 'volume-2', text: 'Preview', color: 'blue', disabled: !words.length || !ok || long})}
    ${voice === 'none' ? M.paragraph('Voice pack not installed.', 'amber') : ''}
    ${voice === 'unavailable' ? M.paragraph('Voice is unavailable.', 'amber') : ''}
    ${long ? M.paragraph('This name is too long to say. Remove a word.', 'amber') : ''}
    ${M.paragraph('Voice language: English (United States)')}</div>`});
};
const snOver = (id, title, shot, over, notes, name = 'Front Door', row = snRow('Choose words')) =>
  inst({id, title, shot}, () => snPage(row, name), {notes, main: () => ({over: over()})});

snOver('spoken-editor-empty', 'Spoken name editor: empty', 'voice_editor_empty', () => snEditor({}),
  'D03. Form sheet with Cancel and Done. Done with no words saves nothing spoken; Cancel discards the draft. Preview is dimmed with no words. Only the installer reaches this editor. The editor holds a lease on the voice pack while it is open and gives it back however it closes.');
snOver('spoken-editor-words', 'Spoken name editor: words chosen', 'voice_editor_words', () => snEditor({words: ['Front', 'Door']}),
  'Tap a word to move or remove it. "Voice language: English (United States)" is always shown, in the interface\'s language; the recorded words and the audio stay in the pack\'s language. Preview plays the draft once (with Announcements off too) and changes nothing. Done puts the words into the zone\'s draft; when the zone page is saved (going back) they are approved for the zone as it is then (D07). No outcome toast: the row shows it.');
snOver('spoken-editor-full', 'Spoken name editor: eight words', 'voice_editor_full',
  () => snEditor({words: ['Front', 'Door', 'Upstairs', 'Back', 'Bedroom', 'Window', 'One', 'Hundred']}),
  'At eight words Add word is dimmed and a line says why. The preview shoots the sheet scrolled to its end.');
snOver('spoken-editor-long', 'Spoken name editor: too long to say', 'voice_editor_long',
  () => snEditor({words: Array(8).fill('Sump pump'), long: true}),
  'A draft over either limit (eight words, 10 seconds with its joins) shows its reason inline and is never approved, truncated or previewed: Preview is dimmed and Done saves it without approval (the row then says Review spoken name). Use zone name that would exceed either limit leaves the draft unchanged and says the same line.');
snOver('spoken-editor-nopack', 'Spoken name editor: no voice pack', 'voice_editor_nopack',
  () => snEditor({words: ['Word 1', 'Word 2'], voice: 'none'}),
  'Without a ready voice, Add word, Use zone name and Preview are dimmed and the editor says "Voice pack not installed." ("Voice is unavailable." for a pack that can\'t be used). The saved words stay, with their move and remove actions, named by their place: their recorded names come from the pack. Done saves an unapproved draft.',
  'Front Door', snRow('Voice pack not installed'));
snOver('spoken-editor-unmatched', 'Use zone name: a part can\'t be spoken', 'voice_editor_unmatched',
  () => snEditor({words: ['Front', 'Door'], name: 'Front Door 2nd floor', unmatched:
    M.paragraph('The panel can’t say "2nd". The words stay as they are. Choose words yourself, or rename the zone.', 'amber')}),
  'D03. The draft is unchanged; the unmatched part is named, never dropped silently (several are joined: "2nd, B2"). The message is one translated sentence with the part as a value.',
  'Front Door 2nd floor', snRow('Review spoken name', 'The name, partition or sensor changed'));

snOver('spoken-word-actions', 'A word\'s actions (second of two)', 'voice_word_actions', () => M.pick_sheet({fit: true, title: 'Door', note: 'Word 2 of 2', rows: [
  M.row_link({glyph: 'arrow-up', title: 'Move earlier', trail: ''}),
  M.row_link({glyph: 'arrow-down', title: 'Move later', trail: '', disabled: true}),
  M.row_link({glyph: 'trash-2', color: 'red', title: 'Remove', trail: ''}),
].join('')}), 'D03. The last word can\'t move later, the first can\'t move earlier: dimmed (ui_pick_sheet_dim). Each action, and Cancel, comes back to the editor.');
snOver('spoken-add-word', 'Add word: find or choose a group', 'voice_add_word', () => M.pick_sheet({fit: true, title: 'Add word', note: 'Voice language: English (United States)', rows: [
  M.row_link({glyph: 'search', title: 'Find a word', trail: ''}),
  M.row_link({glyph: 'compass', title: 'Position', detail: 'Front, Back, Upstairs...', trail: ''}),
  M.row_link({glyph: 'house', title: 'Rooms and places', detail: 'Kitchen, Garage, Basement...', trail: ''}),
  M.row_link({glyph: 'package', title: 'Things', detail: 'Door, Window, Motion...', trail: ''}),
  M.row_link({glyph: 'hash', title: 'Numbers', detail: 'One to Nine, Ten, Twenty, Hundred...', trail: ''}),
].join('')}), 'D03/D09. Four groups, each under 84 words. The group names and the disclosure are interface words; the examples under each are words of the pack (another voice shows its own first three).');
snOver('spoken-add-group', 'Add word: one group, in order', 'voice_add_group', () => M.pick_sheet({title: 'Rooms and places', note: 'Voice language: English (United States)', rows: [
  M.section('A'), M.row_link({title: 'Apartment', trail: ''}), M.row_link({title: 'Area', trail: ''}), M.row_link({title: 'Attic', trail: ''}),
  M.section('B'), M.row_link({title: 'Baby', trail: ''}), M.row_link({title: 'Balcony', trail: ''}), M.row_link({title: 'Barn', trail: ''}),
].join('')}), 'Alphabetical, a heading for each letter. A tap adds the word and returns to the editor. The words are the pack\'s, shown as recorded.');
snOver('spoken-find-word', 'Find a word: the search', 'voice_find_word', () => M.text_sheet({title: 'Find a word', note: 'Enter the start of a word, then tap Search.', value: 'gar', ok: 'Search'}),
  'D03. The same submitted search as Sources: type, press Search. Cancel goes back to the editor. An empty search lists all words.');
snOver('spoken-find-results', 'Find a word: the matches', 'voice_find_results', () => M.pick_sheet({fit: true, title: 'Find a word', note: 'gar: 2 matches', rows: [
  M.row_link({title: 'Garage', detail: 'Rooms and places', trail: ''}),
  M.row_link({title: 'Garden', detail: 'Rooms and places', trail: ''}),
].join('')}), 'A word matches by the start of its text or of any word in it. Matches come from the pack\'s selectable words only; status and walk clips never appear.');
const SN_WORDS = [['Air conditioner', 'Things'], ['Apartment', 'Rooms and places'], ['Aquarium', 'Things'], ['Area', 'Rooms and places'], ['Attic', 'Rooms and places'], ['Baby', 'Rooms and places']];
snOver('spoken-find-paged', 'Find a word: all words, more than 64', 'voice_find_paged', () => M.pick_sheet({title: 'Find a word', note: 'All words: 64 of 221, page 1 of 4', rows: SN_WORDS.map(([w, g]) => M.row_link({title: w, detail: g, trail: ''})).concat([
  M.row_link({glyph: 'chevron-left', title: 'Previous page', trail: '', disabled: true}),
  M.row_link({glyph: 'chevron-right', title: 'Next page', trail: ''}),
]).join('')}), 'An empty search lists all 221 selectable words, 64 to a page, Previous page and Next page as the last rows (dimmed where there is none). A search over 64 matches pages the same way: "door: 64 of 70 matches, page 1 of 2".');
snOver('spoken-find-none', 'Find a word: no match', 'voice_find_none', () => M.pick_sheet({fit: true, title: 'Find a word', note: 'No words match "zz".', rows: ''}),
  'A search that finds nothing says so in the note; Cancel goes back to the editor.');

inst({id: 'zone-type', title: 'Choosing a zone type', shot: 'zone_type'}, () => ({
  title: 'Hallway Motion', note: 'Zone 5 · saved when you go back', back: true, body: zoneEditBody(),
}), {main: () => ({over: M.pick_sheet({fit: true, title: 'Zone type', note: 'How the panel reacts when this sensor trips', rows: [
  M.row_link({title: 'Entry/exit 1', detail: 'The door you come in by: starts entry time 1', trail: ''}),
  M.row_link({title: 'Entry/exit 2', detail: 'A second door, like the garage: entry time 2', trail: ''}),
  M.row_link({title: 'Perimeter', detail: 'Doors and windows: alarm right away', trail: ''}),
  M.row_link({title: 'Interior follower', detail: 'Motion: waits while entry time runs', trail: 'check', trailColor: 'blue'}),
  M.row_link({title: 'Interior with delay', detail: 'Motion that starts entry time', trail: ''}),
  M.row_link({title: '24-hour', detail: 'Alarms armed or not', trail: ''}),
].join('')})})});

inst({id: 'zone-sensor', title: 'A zone\'s sensor settings', shot: 'installer_zone_sensor'}, () => ({
  title: 'Sensor settings', note: 'Front Door · changes apply at once', back: 'zone-edit',
  body: [
    M.section('How it reads'),
    M.row_link({glyph: 'sliders-horizontal', title: 'Open when', value: 'On or open'}),
    M.row_toggle({glyph: 'arrow-left-right', title: 'Reverse open and closed', detail: 'For a normally open contact, or a sensor that reports the other way round'}),
    M.row_link({glyph: 'timer', title: 'Response time', value: '350 ms'}),
    M.section('When it goes quiet'),
    M.row_link({glyph: 'clock-alert', title: 'Supervision', detail: 'Shows Check when it misses a check-in', value: 'Not supervised'}),
    M.row_link({glyph: 'unlink', title: 'Unreachable after', detail: 'Shows Check when its source loses it', value: 'Source default'}),
    M.section('Other inputs'),
    M.row_link({glyph: 'shield-alert', color: 'amber', title: 'Tamper input', value: 'None'}),
    M.row_link({glyph: 'battery-warning', color: 'amber', title: 'Low-battery input', value: 'None'}),
    M.section(''),
    M.row_action({glyph: 'trash-2', text: 'Remove the sensor, keep the zone', color: 'red'}),
  ].join(''),
}), {notes: 'Pickers, not steppers holding words (BL-131 findings 19, 24). Supervision is how long the sensor itself may stay silent (Not supervised, 1, 4 or 12 hours: the stored field holds at most 18 h); Unreachable after how long its source may say it is gone (Source default, 1, 5, 15 or 60 min); a value set another way stays offered as it is. For a wired sensor the reverse switch is "Contact: Normally closed / Normally open". The preview shoots its end: installer_zone_sensor_end.'});

inst({id: 'factory-reset', title: 'Factory reset, held', shot: 'factory_reset'}, () => ({
  title: 'Factory reset', note: 'Start again from the box', back: 'installer-menu',
  body: [
    M.paragraph('Erases the zones, users and codes, Wi-Fi and every setting, then restarts with the factory codes (installer 69420, master 123456) and the setup.'),
    M.row_toggle({title: 'Also erase the event log', detail: 'Off keeps the history, with the reset in it'}),
    M.row_hold({text: 'Hold to erase everything', held: 0.53}),
  ].join(''),
}));

// Z-Wave > Region (ui_zwave.c build_region; docs/features/36 screens.md §8, ZW-045): each other
// region's row says what choosing it does to the devices; a change that stops some names how many,
// then asks for the installer code once more, as the network reset does.
const zwRegionBody = ({current = 'United States', others = [['United States with Long Range', 'Devices keep working']]} = {}) => {
  const stops = others.some(([, d]) => d.includes('stop'));
  return [
    M.row_link({glyph: 'globe', color: 'green', title: current, value: 'Selected', trail: ''}),
    ...others.map(([t, d]) => M.row_link({glyph: 'globe', color: 'gray', title: t, detail: d})),
    stops ? M.paragraph('Devices that stop working must be added again. Until then their zones show CHECK.', 'amber') : '',
    M.paragraph('Changing it restarts the Z-Wave radio (a few seconds).'),
  ].join('');
};
const ZW_REGION_PAGE = {title: 'Region', note: 'Must match where the radio was bought', back: true};
const ZW_BAND_OTHERS = [['United States', 'Devices keep working'], ['Europe', '5 devices stop working']];

inst({id: 'zwave-region', title: 'Z-Wave region: the same band', shot: null}, () => ({
  ...ZW_REGION_PAGE, body: zwRegionBody(),
}), {notes: 'Preview: zwave_region (the -ZWave walk). A US radio offers United States and United States with Long Range: the same band, so no amber warning; with no devices the rows have no detail.'});

inst({id: 'zwave-region-same-band', title: 'Z-Wave region: a same-band change', shot: null}, () => ({
  ...ZW_REGION_PAGE, body: zwRegionBody(),
}), {notes: 'Preview: zwave_region_same_band. "Change it" changes the region at once: nothing stops, so no code.',
  main: () => ({over: M.confirm_dialog({title: 'Change the region to United States with Long Range?', note: 'All 5 devices keep working. The Z-Wave radio restarts (a few seconds).', ok: 'Change it'})})});

inst({id: 'zwave-region-band', title: 'Z-Wave region: another band offered', shot: null}, () => ({
  ...ZW_REGION_PAGE, body: zwRegionBody({current: 'United States with Long Range', others: ZW_BAND_OTHERS}),
}), {notes: 'Preview: zwave_region_band (the walk makes its radio offer Europe too). Leaving a Long Range region for its classic one says "2 devices stop working" when 2 Long Range devices joined, else "Devices keep working".'});

inst({id: 'zwave-region-drop-confirm', title: 'Z-Wave region: a change that stops devices', shot: null}, () => ({
  ...ZW_REGION_PAGE, body: zwRegionBody({current: 'United States with Long Range', others: ZW_BAND_OTHERS}),
}), {notes: 'Preview: zwave_region_drop_confirm. Leaving Long Range says "2 Long Range devices stop working. Each must be added again, and their zones show CHECK until then."',
  main: () => ({over: M.confirm_dialog({title: 'Change the region to Europe?', note: 'All 5 devices stop working. Each must be added again, and their zones show CHECK until then.', ok: 'Change it'})})});

inst({id: 'zwave-region-code', title: 'Z-Wave region: the installer code once more', shot: null}, () => ({
  pad: M.code_pad({title: 'Confirm the region', tone: 'red', glyph: 'triangle-alert', hint: 'Enter the installer code once more', width: 428}),
}), {notes: 'Preview: zwave_region_code. The network reset\'s check: only the installer code; a master code says "Wrong code, try again"; the X closes it and nothing changes.'});

inst({id: 'zwave-radio-details', title: 'Z-Wave radio details', shot: null}, () => ({
  title: 'Z-Wave radio', note: 'What the panel knows about its radio', back: true,
  body: [
    M.row_value({title: 'State', value: 'Working'}),
    M.row_value({title: 'Radio software', value: '2.30 · Z-Wave 7.24.2'}),
    M.row_value({title: 'Region', value: 'United States'}),
    M.row_value({title: 'Background noise', value: '-94 dBm (quietest channel)'}),
    M.row_link({glyph: 'rotate-ccw', color: 'blue', title: 'Restart the radio', detail: 'Takes a few seconds; zones keep their state'}),
  ].join(''),
}), {notes: 'Preview: zwave_radio_details (the -ZWave walk). BL-152: Background noise (the quietest channel; "Not measured yet" before a reading). While the radio updates, or another change runs, Restart is gray: "Not while its software installs" / "Not while another change runs".'});

// Settings > Installer > Z-Wave > Add a device (ui_zwave.c build_add), refused for want of storage
// (BL-152, ZW-039; docs/features/20 devices.md "The NVS budget"). Drawn and built
// without waiting for review (docs/features/36 ZD-3 (k)). Fictional data.
inst({id: 'zwave-add-full', title: 'Z-Wave: Add refused, the storage is full', shot: null}, () => ({
  title: 'Add a Z-Wave device', note: '', back: 'installer-menu',
  body: [
    M.paragraph('Not started: there is no room for another device. Remove one you no longer use first.', 'amber'),
    M.row_action({glyph: 'plus', text: 'Try again', color: 'blue'}),
  ].join(''),
}), {notes: 'BL-152 (ZW-039): while NVS has fewer free entries than a new device needs (the alarm configuration\'s rewrite, the node cache\'s growth, and its own rewrite once it is large), Add and Replace answer FULL. The page says why in Zigbee\'s words, as the Zigbee Add page does; a Replace refused the same way shows this page too, and the device page\'s Replace it shows the toast "No room for another device: the panel\'s storage is full". SmartStart devices waiting under SmartStart codes simply don\'t join until there is room (the radio stops listening for them). Changing what is there still works. The Panel Manager (which adds but does not replace) says the same when its Add is refused: "No room for another device: the panel’s storage is full."'});

// Settings > Installer > Z-Wave (ui_zwave.c radio_look), while the storage is full (BL-152, ZW-039):
// the radio's row says why nothing new can be added; SmartStart devices on the list wait. Drawn
// and built without waiting for review (docs/features/36 ZD-3 (k)). Fictional data.
inst({id: 'zwave-storage-full', title: 'Z-Wave: the storage is full', shot: null}, () => ({
  title: 'Z-Wave', note: 'Sensors on the panel\'s Z-Wave radio', back: 'installer-menu',
  body: [
    M.row_link_alert({glyph: 'radio-tower', color: 'amber', title: 'Radio · Working',
      detail: 'The panel\'s storage is full: no new device can be added', detailColor: 'amber'}),
    M.row_link({glyph: 'plus', color: 'blue', title: 'Add a device', detail: 'Pair a sensor with the panel'}),
    M.row_link({glyph: 'trash-2', title: 'Remove a device', detail: 'Also resets a sensor from another system'}),
    M.section('Devices'),
    M.row_link({glyph: 'door-open', title: 'Back door', detail: 'Zone 3 · all heard', value: 'Secure'}),
  ].join(''),
}), {notes: 'BL-152 (ZW-039): ui_zw_status_t.storage_full (appended) is set while NVS has fewer free entries than a new device needs (zwz_panel_nvs_need). The radio row keeps its title and turns amber with one line in Zigbee\'s words; Interference, when present, is shown instead (it is the more urgent). Add stays tappable and answers with the page above (zwave-add-full); SmartStart devices on the list don\'t join until there is room, and the line clears by itself within about 10 seconds once space is freed. The Panel Manager\'s radio card says: "The panel’s storage is full: no new device can be added until space is freed. Devices on the SmartStart list join once there is room."'});

// A Z-Wave device's page (ui_zwave.c build_device; docs/features/36 screens.md §6). BL-152: Check it
// now on every listening device, and Set it up again for one whose setup is done.
const zwDeviceBody = ({setup = null, sleeper = false, ready = true} = {}) => [
  setup ? M.paragraph(setup, 'blue') : '',
  M.row_value({title: 'Security', value: 'With its PIN'}),
  M.row_value({title: 'Battery', value: sleeper ? '90 %' : 'Mains powered'}),
  M.row_value({title: 'Last heard', value: '2 min ago'}),
  M.row_value({title: 'Checks in', value: sleeper ? 'Every 4 hours' : 'Not needed'}),
  M.row_link({glyph: 'tag', title: 'Used by', value: 'Zone 3 · Front door'}),
  sleeper ? '' : M.row_action({glyph: 'refresh-cw', text: 'Check it now'}),
  ready ? M.row_link({glyph: 'refresh-cw', title: 'Set it up again', detail: 'Read what it can do again: its zones keep working', trail: null}) : '',
  ready ? M.row_link({glyph: 'waves', title: 'Rebuild its routes', detail: sleeper ? 'Tap, then press its button to wake it' : 'Refresh how its signals reach the panel', trail: null}) : '',
].join('');
const ZW_DEVICE_PAGE = {title: 'Door sensor', note: 'Z-Wave sensor · device 7', back: true};

inst({id: 'zwave-device-health', title: 'Z-Wave device: Check it now and Set it up again', shot: null}, () => ({
  ...ZW_DEVICE_PAGE, body: zwDeviceBody(),
}), {notes: 'Preview: zwave_device_health_actions. BL-152. A listening device that answers has Check it now too (its answer a toast: "It answered" or "No answer"). Set it up again: installer mode, a device whose setup is done (Ready, or can\'t report).'});

inst({id: 'zwave-device-setup-again-confirm', title: 'Z-Wave device: Set it up again, confirmed first', shot: null}, () => ({
  ...ZW_DEVICE_PAGE, body: zwDeviceBody(),
}), {notes: 'Preview: zwave_device_setup_again_confirm. BL-152. A sleeper\'s text (at most 120 bytes: the confirmation holds 127): "It\'s read again at its next wake-up. Its zones keep working, and it keeps its security. Wake it to start now."',
  main: () => ({over: M.confirm_dialog({title: 'Set it up again?', note: 'The panel reads what it can do again. Its zones keep working, and it keeps its security.', ok: 'Set it up again'})})});

inst({id: 'zwave-device-setup-again-running', title: 'Z-Wave device: read again', shot: null}, () => ({
  ...ZW_DEVICE_PAGE, body: zwDeviceBody({setup: 'Reading it again. Its zones keep working.', ready: false}),
}), {notes: 'Preview: zwave_device_setup_again_running. BL-152. The device keeps working on its old facts while it is read (its stored setup stays done until the new reading is complete), so no step count is shown. Try its setup again is not offered while it runs, also when the page is left and opened again (the panel says it is a re-read); the zone keeps its state and its reports. Its list row: "Being read again". When it ends the page is the Ready one again; if it stops: "Reading it again stopped: it didn\'t answer. It works as before. Bring it closer or check its power, then try again." with Try its setup again.'});

inst({id: 'zwave-device-setup-again-sleeper', title: 'Z-Wave device: a sleeper read again at its wake-up', shot: null}, () => ({
  ...ZW_DEVICE_PAGE, note: 'Z-Wave sensor · device 9', body: zwDeviceBody({sleeper: true, ready: false,
    setup: 'It\'s read again at its next wake-up. Its zones keep working. To start now, wake it (press its button).'}),
}), {notes: 'Preview: zwave_device_setup_again_sleeper. BL-152. A sleeping device: no Check it now (it answers only when it wakes); its reading starts at its next wake-up. Calm (blue), never the amber "Wake it to finish setting it up": it works on as before meanwhile. Its list row: "Read again at its next wake-up", not a warning.'});

inst({id: 'zwave-device-setup-again-stopped', title: 'Z-Wave device: a read-again that stopped', shot: null}, () => ({
  ...ZW_DEVICE_PAGE, note: 'Z-Wave keypad · device 3', body: [
    M.paragraph('Reading it again stopped: it didn\'t answer. It works as before. Bring it closer or check its power, then try again.', 'blue'),
    M.row_action({glyph: 'refresh-cw', text: 'Try its setup again'}),
    zwDeviceBody({ready: false})].join(''),
}), {notes: 'Preview: zwave_device_setup_again_stopped. BL-152. Calm (blue): the device works as before. A sleeper says "Reading it again stopped. It works as before. Wake it (press its button), then try again." Its list row: "Read again stopped: works as before". If the device then stops answering (or its battery is low) the page shows the amber "It stopped answering. Check its power or batteries, or bring it closer. If it was reset, remove it below." and its row "Not heard for ...": those come before the calm words.'});
