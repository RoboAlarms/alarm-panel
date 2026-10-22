// Settings > Installer > Sirens and outputs, a wired output, a 433 MHz siren and its pairing,
// Installer > 433 MHz, a Z-Wave siren's rows, the Z-Wave radio's software from an SD card, a Zigbee
// smoke detector's own sounder as a siren, and a wired zone's end-of-line Contact choices
// (ui_sirens.c, ui_zwave.c, ui_zigbee.c, ui_sources.c; docs/features/16, 25, 36, 20, 02).
// Drawn as the BL-037 proposal (with BL-215 and BL-063) and approved by the owner on 2026-10-20
// ("It's approved"); built and walked in the PC preview. Nothing on these pages blinks; live words
// change at most once a second (UI-020, UI-023). All data is fictional (Harbor House: partitions
// Home and Shop).

const RAIL = {tone: 'blue'};
const BAR = {banner: 'installer', time: '22:41'};
const AREA = 'Sirens and 433 MHz';
// The preview's picture of each screen it walks (host/ui_preview/main.c walk_sirens); null where
// it has no such device (a Z-Wave siren, a Zigbee smoke detector) or shows another state.
const SHOTS = {
  'bl-037-installer-menu': 'bl-037-installer-menu', 'bl-037-sirens': 'bl-037-sirens',
  'bl-037-wired': 'bl-037-wired', 'bl-037-wired-end': 'bl-037-wired-end',
  'bl-037-wired-add': 'bl-037-wired-add', 'bl-037-rf-siren': 'bl-037-rf-siren',
  'bl-037-rf-siren-pair': 'bl-037-rf-siren-pair', 'bl-037-rf': 'bl-037-rf',
  'bl-224-output': 'bl-224-output', 'bl-224-sirens': 'bl-224-sirens',
};
const prop = (id, title, replaces, why, notes, body, extra = {}) => M.screen({
  id, area: AREA, title, shot: SHOTS[id] || null,
  notes: why + ' ' + notes,
  render: () => M.main({view: 'settings', tone: 'blue', rail: RAIL, bar: BAR,
    content: M.settings({section: 'installer', ...body}), over: extra.over ? extra.over() : '', toast: extra.toast}),
});

// ---- 1: the installer menu ----
prop('bl-037-installer-menu', 'Installer menu: Sirens and outputs, 433 MHz', 'zigbee-installer-menu',
  'Wired sirens, 433 MHz sirens and Z-Wave sirens had no place on the screen; 433 MHz sensors could only be found zone by zone.',
  '"433 MHz" follows Zigbee and shows only while a 433 MHz receiver or transmitter answers, or a 433 MHz zone or siren exists; its value: "3 sensors, 1 siren", "Not connected" (gray), "Interference" (amber). "Sirens and outputs" follows Keypads and devices on every panel that has terminals or a radio; its value counts every siren the panel sounds (wired, 433 MHz, Zigbee, Z-Wave), amber "Siren problem" while any is in trouble.',
  {title: 'Installer', body: [
    M.section('Sensors and devices'),
    M.row_link({glyph: 'door-closed', color: 'green', title: 'Zones', detail: 'Sensors, types, names', value: '12 in use'}),
    M.row_link({glyph: 'radio-tower', title: 'Z-Wave', detail: 'Sensors on the panel\'s Z-Wave radio', value: '5 devices'}),
    M.row_link({glyph: 'radio', title: 'Zigbee', detail: 'Sensors on the panel\'s Zigbee radio', value: '8 devices'}),
    M.row_link({glyph: 'radio', title: '433 MHz', detail: 'Sensors and sirens on 433 MHz', value: '3 sensors, 1 siren', goTo: 'bl-037-rf'}),
    M.row_link({glyph: 'warehouse', color: 'violet', title: 'Partitions', detail: 'Areas that arm separately', value: '2 in use'}),
    M.row_link({glyph: 'cpu', title: 'Keypads and devices', detail: 'Supervised keypads and expanders', value: 'None'}),
    M.row_link({glyph: 'siren', color: 'red', title: 'Sirens and outputs', detail: 'What sounds and what switches when the alarm does', value: '4 sirens', goTo: 'bl-037-sirens'}),
  ].join('')});

// ---- 2: Sirens and outputs ----
prop('bl-037-sirens', 'Sirens and outputs: every siren the panel sounds', null,
  'One place to see what sounds in an alarm, whichever way it is connected.',
  'Wired outputs first (the terminals on the J7 header: a relay or MOSFET module drives the siren, never the pin itself), then 433 MHz sirens, then the Zigbee and Z-Wave sirens, which open their own device pages. Each row: what it is, its partitions, and its state ("Sounding", amber "Siren problem: its supervision input is open"). Add rows are left out once four of a kind exist. A paragraph at the top says that the panel\'s own speaker always sounds as well.',
  {title: 'Sirens and outputs', note: 'What sounds and what switches when the alarm does', back: 'bl-037-installer-menu', body: [
    M.paragraph('The panel\'s own speaker always sounds too. Sirens follow it: steady for burglary, the fire pattern for fire, the CO pattern for CO.'),
    M.section('On the panel\'s terminals'),
    M.row_link({glyph: 'siren', color: 'red', title: 'Outdoor siren', detail: 'Terminal GPIO 49 · Home, also Shop · Chirps', value: 'Ready', goTo: 'bl-037-wired'}),
    M.row_link({glyph: 'lightbulb', color: 'amber', title: 'Armed light', detail: 'Terminal GPIO 50 · Home', value: 'Off'}),
    M.row_link({glyph: 'plus', color: 'green', title: 'Add a wired siren or output', detail: 'A relay or MOSFET module on a free terminal', goTo: 'bl-037-wired-add'}),
    M.section('433 MHz'),
    M.row_link({glyph: 'siren', color: 'red', title: '433 MHz siren 1', detail: 'Home · Not supervised', value: 'Ready', goTo: 'bl-037-rf-siren'}),
    M.row_link({glyph: 'plus', color: 'green', title: 'Add a 433 MHz siren', detail: 'One that learns its panel like a key fob', goTo: 'bl-037-rf-siren-pair'}),
    M.section('Zigbee and Z-Wave'),
    M.row_link({glyph: 'siren', color: 'red', title: 'Hall Siren', detail: 'Zigbee · Home', value: 'Ready'}),
    M.row_link({glyph: 'siren', color: 'red', title: 'Garage Siren', detail: 'Z-Wave · Shop, also Home', value: 'Ready', goTo: 'bl-037-zw-siren'}),
  ].join('')});

// ---- 3: a wired output ----
const WIRED = {title: 'Outdoor siren', note: 'Siren on terminal GPIO 49 · J7 header', back: 'bl-037-sirens'};
prop('bl-037-wired', 'A wired siren\'s page', null,
  'A siren on a terminal: what it sounds for, for whom, and its check.',
  'The same two switches as a Zigbee siren (Burglary and panic; Fire and CO). Partitions: its own - their alarms, its chirps, and Test it waits until they are disarmed. Also sounds for: other partitions\' alarms only (here the Shop\'s); never their chirps. A partition can\'t be in both lists. The line pattern is the panel\'s (steady, temporal-3 for fire, temporal-4 for CO), timed to the millisecond by the panel.',
  {...WIRED, body: [
    M.section('It sounds for'),
    M.row_toggle({glyph: 'siren', color: 'red', title: 'Burglary and panic', detail: 'A silent panic stays silent', on: true}),
    M.row_toggle({glyph: 'flame', color: 'red', title: 'Fire and CO', detail: 'The fire pattern for fire, the CO pattern for CO', on: true}),
    M.row_link({glyph: 'warehouse', color: 'violet', title: 'Partitions', detail: 'Their alarms and its chirps', value: 'Home'}),
    M.row_link({glyph: 'layers', color: 'violet', title: 'Also sounds for', detail: 'Other partitions\' alarms, never their chirps', value: 'Shop'}),
  ].join('')});
prop('bl-037-wired-end', 'A wired siren\'s page, scrolled', null,
  'How it is wired, checked and removed.',
  'What it is: Siren, Strobe, Smoke detector power, Armed light (a choice sheet). Terminal: the free terminals only (a zone\'s terminal is never offered; an output\'s terminal is never offered to a zone). Relay board: "Switches on high" or "Switches on low" (most relay boards switch on low). Supervision input: None, or a free terminal wired to the siren\'s fault contact (open 10 s = a siren trouble). Test it: 2 seconds, refused while one of its partitions is armed. Remove: its terminal goes back to the zones.',
  {...WIRED, body: [
    M.section('How it is wired'),
    M.row_link({glyph: 'siren', title: 'What it is', value: 'Siren'}),
    M.row_link({glyph: 'plug', title: 'Terminal', value: 'GPIO 49'}),
    M.row_link({glyph: 'zap', title: 'Relay board', detail: 'Most switch on low', value: 'Switches on low'}),
    M.row_toggle({glyph: 'bell', title: 'Chirp when arming and disarming', detail: 'One chirp to arm, two to disarm'}),
    M.row_link({glyph: 'clock-alert', color: 'amber', title: 'Supervision input', detail: 'Its fault contact: open is a siren trouble', value: 'GPIO 50'}),
    M.section('Check it'),
    M.row_link({glyph: 'bell-ring', title: 'Test it', detail: 'Sounds it for 2 seconds', trail: ''}),
    M.row_link({glyph: 'trash-2', color: 'red', title: 'Remove it', detail: 'Its terminal goes back to the zones', trail: ''}),
  ].join('')});
prop('bl-037-wired-add', 'Adding a wired siren or output: what it is', null,
  'The first question when adding one.',
  'A choice sheet over Sirens and outputs. Then the terminal (the free ones), then its page. A new siren sounds for burglary and fire in partition 1, no chirps, switching on high. "For automations" (BL-224, docs/features/41 AUT-021) is last: a relay that automations switch, never with the alarm.',
  {title: 'Sirens and outputs', note: 'What sounds and what switches when the alarm does', back: 'bl-037-installer-menu', body: [
    M.section('On the panel\'s terminals'),
    M.row_link({glyph: 'siren', color: 'red', title: 'Outdoor siren', detail: 'Terminal GPIO 49 · Home, also Shop · Chirps', value: 'Ready'}),
  ].join('')},
  {over: () => M.pick_sheet({title: 'Add a wired output', note: 'What it switches', fit: true, rows: [
    M.row_link({glyph: 'siren', color: 'red', title: 'Siren', detail: 'Sounds with the alarm', trail: ''}),
    M.row_link({glyph: 'sun', title: 'Strobe', detail: 'On from an audible alarm until disarmed', trail: ''}),
    M.row_link({glyph: 'flame', color: 'red', title: 'Smoke detector power', detail: 'Cut briefly to reset 4-wire smoke detectors', trail: ''}),
    M.row_link({glyph: 'lightbulb', color: 'amber', title: 'Armed light', detail: 'On while its partitions are armed', trail: ''}),
    M.row_link({glyph: 'power', title: 'For automations', detail: 'Switched on and off by Settings > Automations', trail: ''}),
  ].join('')})});

// ---- 3b: an output for automations (BL-224, approved 2026-10-21) ----
prop('bl-224-output', 'A wired output for automations', null,
  'A relay that automations switch: garden or porch lights, a buzzer.',
  'Its name is typed like a zone\'s (at most 23 bytes); no partitions, no chirps, no supervision. Test it switches it on for 2 seconds, armed or not; afterwards it is back to what automations set. It starts off at every start-up and never follows the alarm (OUT-023, OUT-024). Older firmware doesn\'t know it: it is stored apart (outputs/auto), so a downgrade never costs the sirens.',
  {title: 'Garden Lights', note: 'On terminal GPIO 2 · J7 header', back: 'bl-224-sirens', body: [
    M.section('How it is wired'),
    M.row_link({glyph: 'tag', title: 'Name', value: 'Garden Lights'}),
    M.row_link({glyph: 'plug', title: 'Terminal', value: 'GPIO 2'}),
    M.row_link({glyph: 'zap', title: 'Relay board', detail: 'Most switch on low', value: 'Switches on high'}),
    M.section('Check it'),
    M.row_link({glyph: 'power', title: 'Test it', detail: 'Switches it on for 2 seconds', trail: ''}),
    M.row_link({glyph: 'trash-2', color: 'red', title: 'Remove it', detail: 'Its terminal goes back to the zones', trail: ''}),
  ].join('')});
prop('bl-224-sirens', 'Sirens and outputs with an output for automations', null,
  'An output for automations in the list.',
  'Its title is its name, its line the terminal; its value "Not used yet" while no automation switches it, then On or Off.',
  {title: 'Sirens and outputs', note: 'What sounds and what switches when the alarm does', back: 'bl-037-installer-menu', body: [
    M.section('On the panel\'s terminals'),
    M.row_link({glyph: 'siren', color: 'red', title: 'Siren', detail: 'Terminal GPIO 49 · Home', value: 'Ready'}),
    M.row_link({glyph: 'lightbulb', color: 'amber', title: 'Armed light', detail: 'Terminal GPIO 51 · Home', value: 'Off'}),
    M.row_link({glyph: 'sun', title: 'Strobe', detail: 'Terminal GPIO 52 · Home', value: 'Ready'}),
    M.row_link({glyph: 'power', title: 'Garden Lights', detail: 'GPIO 2 on the J7 header', value: 'Not used yet', goTo: 'bl-224-output'}),
  ].join('')});

// ---- 4: a 433 MHz siren ----
prop('bl-037-rf-siren-pair', 'Adding a 433 MHz siren: pairing', null,
  'Cheap 433 MHz sirens learn their panel the way they learn a key fob; the panel needs one button that makes it do so.',
  'The panel gives the new siren an address of its own (no sensor or factory remote uses it). Steps: put the siren in its learning mode (its manual says how: usually its button, until it beeps), then Send the pairing signal - the panel sends it three times. Most sirens beep or flash to say they learned it. Test it then sounds it. Nothing is supervised: these sirens never answer, and their page says so.',
  {title: 'Add a 433 MHz siren', note: '433 MHz siren 2', back: 'bl-037-sirens', body: [
    M.paragraph('1. Put the siren in its learning mode: its manual says how, usually by holding its button until it beeps.'),
    M.paragraph('2. Send the pairing signal. Most sirens beep to say they learned it.'),
    M.row_action({glyph: 'send', text: 'Send the pairing signal', color: 'blue'}),
    M.paragraph('3. Test it. If it stays quiet, send the pairing signal again.'),
    M.row_link({glyph: 'bell-ring', title: 'Test it', detail: 'Sounds it for 2 seconds', trail: ''}),
  ].join('')});
const RFS = {title: '433 MHz siren 1', note: 'Address 9E3B5 · Not supervised', back: 'bl-037-sirens'};
prop('bl-037-rf-siren', 'A 433 MHz siren\'s page', null,
  'What a 433 MHz siren sounds for, and the honest word that nobody can hear it.',
  'Same rows as a wired siren. A one-way siren has one sound: fire and CO sound it like burglary (its row says so). It gets its alarm signal again every 20 seconds while the alarm sounds, and the stop signal three times when it ends. The amber paragraph appears while nothing can transmit (the siren trouble is on).',
  {...RFS, body: [
    M.paragraph('Not supervised: the panel can\'t hear this siren. Test it now and then.', 'amber'),
    M.section('It sounds for'),
    M.row_toggle({glyph: 'siren', color: 'red', title: 'Burglary and panic', detail: 'A silent panic stays silent', on: true}),
    M.row_toggle({glyph: 'flame', color: 'red', title: 'Fire and CO', detail: 'Its one sound', on: true}),
    M.row_link({glyph: 'warehouse', color: 'violet', title: 'Partitions', detail: 'Their alarms and its chirps', value: 'Home'}),
    M.row_link({glyph: 'layers', color: 'violet', title: 'Also sounds for', detail: 'Other partitions\' alarms, never their chirps', value: 'None'}),
    M.row_toggle({glyph: 'bell', title: 'Chirp when arming and disarming', detail: 'Its own arm and disarm sounds'}),
    M.section('Check it'),
    M.row_link({glyph: 'bell-ring', title: 'Test it', detail: 'Sounds it for 2 seconds', trail: ''}),
    M.row_link({glyph: 'send', title: 'Pair it again', detail: 'Its learning mode first', trail: ''}),
    M.row_link({glyph: 'trash-2', color: 'red', title: 'Remove it', detail: 'Sends it the stop signal first', trail: ''}),
  ].join('')});

// ---- 5: the 433 MHz page ----
prop('bl-037-rf', '433 MHz: the receiver, sensors and sirens', null,
  'The 433 MHz sensors were only visible one zone at a time, and the receiver\'s state only on the console.',
  'The receiver row: Working (blue, "Heard 2 min ago"), Not connected (gray), Interference (amber, the sensor radio trouble), Starting. The transmitter row says whether 433 MHz sirens can be sounded. Sensors: one row per zone whose sensor is on 433 MHz, with its code and when it was last heard (minutes, then hours), amber when its zone shows a problem; a row opens the zone. Sirens: as on Sirens and outputs. Add a sensor opens the zone editor\'s Add a zone, with 433 MHz sensor picked. Words change at most once a second.',
  {title: '433 MHz', note: 'Sensors and sirens on 433 MHz', back: 'bl-037-installer-menu', body: [
    M.row_link({glyph: 'radio', title: 'Receiver', detail: 'Heard a sensor 2 min ago', value: 'Working', trail: ''}),
    M.row_link({glyph: 'send', title: 'Transmitter', detail: '433 MHz sirens can be sounded', value: 'Ready', trail: ''}),
    M.row_link({glyph: 'plus', color: 'green', title: 'Add a sensor', detail: 'In Zones: add a zone, pick 433 MHz sensor, trigger it'}),
    M.row_link({glyph: 'plus', color: 'green', title: 'Add a siren', detail: 'One that learns its panel like a key fob', goTo: 'bl-037-rf-siren-pair'}),
    M.section('Sensors'),
    M.row_link({glyph: 'door-closed', color: 'green', title: 'Back Door', detail: 'Zone 3 · Sensor A2B3C, code 6 · Heard 2 min ago'}),
    M.row_link_alert({glyph: 'footprints', color: 'amber', title: 'Garage Motion', detail: 'Zone 4 · Sensor 51F07, code 1', sub: 'Not heard for 3 h: this kind of sensor only sends when it is triggered'}),
    M.row_link({glyph: 'droplets', color: 'green', title: 'Laundry Water', detail: 'Zone 11 · Sensor 0C4D2, code 8 · Heard yesterday'}),
    M.section('Sirens'),
    M.row_link({glyph: 'siren', color: 'red', title: '433 MHz siren 1', detail: 'Home · Not supervised', value: 'Ready', goTo: 'bl-037-rf-siren'}),
  ].join('')});

// ---- 6: a Z-Wave siren (on its device page) ----
prop('bl-037-zw-siren', 'A Z-Wave siren\'s page: its siren rows', null,
  'Z-Wave sirens (Zooz, Aeotec Siren 6) were listed as sirens but never sounded with the alarm.',
  'The rows a Zigbee siren has, on the Z-Wave device page of a device with Sound Switch or a siren\'s Binary Switch. Tones: which of its tones sounds for burglary, fire, CO and its chirps (a list read from it, "Its default tone" first). Volume 10-100 % or its own. Supervision: a siren trouble when it isn\'t heard, after 4 hours by default (the common Z-Wave siren default).',
  {title: 'Garage Siren', note: 'Zooz siren · Mains · Node 12', back: 'bl-037-sirens', body: [
    M.section('It sounds for'),
    M.row_toggle({glyph: 'siren', color: 'red', title: 'Burglary and panic', detail: 'A silent panic stays silent', on: true}),
    M.row_toggle({glyph: 'flame', color: 'red', title: 'Fire and CO', detail: 'Its fire tone for fire, its CO tone for CO', on: true}),
    M.row_link({glyph: 'warehouse', color: 'violet', title: 'Partitions', detail: 'Their alarms and its chirps', value: 'Shop'}),
    M.row_link({glyph: 'layers', color: 'violet', title: 'Also sounds for', detail: 'Other partitions\' alarms, never their chirps', value: 'Home'}),
    M.section('How it sounds'),
        M.row_link({glyph: 'volume-2', title: 'Volume', value: 'Its own'}),
    M.row_toggle({glyph: 'bell', title: 'Chirp when arming and disarming', detail: 'Its chirp tone'}),
    M.section('Check it'),
    M.row_link({glyph: 'bell-ring', title: 'Test it', detail: 'Sounds it for 2 seconds', trail: ''}),
    M.row_link({glyph: 'clock-alert', color: 'amber', title: 'Supervision', detail: 'A siren trouble when it isn\'t heard', value: 'After 4 h'}),
  ].join('')});

// ---- 7: the Z-Wave radio's software from an SD card (the "Z-Wave updates" follow-up) ----
prop('bl-037-zw-radio-sd', 'Z-Wave radio: its software, from an SD card', 'zwave-radio-details',
  'The Z-Wave page said "Update its software" for a Zooz radio older than 2.20, but the update service doesn\'t list the ZAC93, and the only way was a file uploaded in the Panel Manager from a computer.',
  'When its software is older than its maker advises, an amber paragraph points to Check for an update (approved 2026-10-20): the panel finds Zooz\'s file by itself (network.md §14). "Install from an SD card" reads the one .gbl file at the card\'s top or in a folder named zwave, asks first (the same question as an update from the service), then the radio\'s update runs exactly as from the Panel Manager (the file is checked as this radio\'s before anything is written; arming waits). No card, no file, two files, or a file for another radio each say so in one sentence, in place.',
  {title: 'Z-Wave radio', note: 'What the panel knows about its radio', back: 'zwave-radio-details', body: [
    M.section('Software'),
    M.paragraph('Its software is older than its maker advises. Tap Check for an update below to install the newer one.', 'amber'),
    M.paragraph('The update service doesn\'t list this radio yet: install its maker\'s own file from an SD card or the Panel Manager.'),
    M.row_link({glyph: 'hard-drive', title: 'Install from an SD card', detail: 'Its maker\'s .gbl file for this radio, copied to the card'}),
    M.row_action({glyph: 'refresh-cw', text: 'Check again', color: 'blue'}),
  ].join('')});

// ---- 8: a Zigbee smoke detector's own sounder as a siren (BL-215, here because the request
// covered Zigbee sirens) ----
M.screen({
  id: 'bl-215-zb-smoke-sounder', area: AREA, title: 'A Zigbee smoke detector with a sounder: its siren rows', shot: null,
  notes: 'The panel accepted a siren assignment for a smoke detector whose sounder is an IAS WD (Heiman SmokeSensor-EM), but no screen offered it. A sensor\'s page as today (Used by, its facts), then a paragraph and the siren page\'s rows (BL-147): It sounds for, How it sounds, Check it. It stays a sensor: its zones, its row in the list and its "Replace it" are unchanged. A detector whose sounder speaks only Tuya\'s 0xEF00 has no IAS WD and gets none of this.',
  render: () => M.main({view: 'settings', tone: 'blue', rail: RAIL, bar: BAR,
    content: M.settings({section: 'installer', title: 'Kitchen Smoke', note: 'Heiman smoke and CO alarm · Zones 6, 9', back: 'zigbee', body: [
      M.row_link({glyph: 'tag', title: 'Used by', value: 'Zones 6, 9'}),
      M.paragraph('Its own sounder can also sound with the panel\'s alarms, like a siren.'),
      M.section('It sounds for'),
      M.paragraph('The panel doesn\'t sound it yet: turn on what it sounds for.', 'amber'),
      M.row_toggle({glyph: 'siren', color: 'red', title: 'Burglary and panic', detail: 'Its partitions\' alarms; a silent panic stays silent'}),
      M.row_toggle({glyph: 'flame', color: 'red', title: 'Fire and CO', detail: 'Its fire sound for fire, its emergency sound for CO'}),
    ].join('')})}),
});

// ---- 9: a wired zone's Contact with end-of-line loops (BL-063, ZONE-005; here because the request
// covered wired zones) ----
M.screen({
  id: 'bl-063-eol-contact', area: AREA, title: 'A wired zone\'s Contact: end-of-line loops', shot: 'zone_eol_contact',
  notes: 'Wired zones could only be plain contacts: a cut wire on a normally open loop, or a shorted loop, went unnoticed, and a fire loop had no trouble for a cut wire. Zone > Sensor settings > Contact, as a pick sheet. The three end-of-line choices are offered only on a terminal that measures (the 7"\'s J7 GPIO 49-52); on the others the sheet is today\'s two choices. The row then says "Closed, end-of-line resistor" and so on. What each reading does: a tamper and a trouble show on the zone as they do for any sensor (the zone list\'s chip, Troubles).',
  render: () => M.main({view: 'settings', tone: 'blue', rail: RAIL, bar: BAR,
    content: M.settings({section: 'installer', title: 'Sensor settings', note: 'Zone 10 · Wired · GPIO 50', back: 'installer-zones', body: [
      M.row_link({glyph: 'arrow-left-right', title: 'Contact', value: 'Normally closed'}),
      M.row_link({glyph: 'timer', title: 'Response time', value: '350 ms'}),
    ].join('')}),
    over: () => M.pick_sheet({title: 'Contact', note: 'How the sensor\'s contact works', rows: [
      M.row_link({title: 'Normally closed', detail: 'Closed while the door is shut: a cut wire shows as open', trail: ''}),
      M.row_link({title: 'Normally open', detail: 'Closes when it trips: a cut wire goes unnoticed', trail: ''}),
      M.row_link({title: 'Closed, end-of-line resistor', detail: 'Doors, windows, motion: 2 kOhm at the sensor, 4.7 kOhm pull-up. A short is a tamper', trail: ''}),
      M.row_link({title: 'Open, end-of-line resistor', detail: 'Smoke detectors, panic buttons: a cut wire is a trouble', trail: ''}),
      M.row_link({title: 'Double end-of-line', detail: 'Two 2 kOhm resistors: a cut or shorted wire is a tamper', trail: ''}),
    ].join('')})}),
});