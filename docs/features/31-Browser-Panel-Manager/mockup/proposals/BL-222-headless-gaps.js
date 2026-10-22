// BL-222: what a panel without a screen (the headless ESP32-S3 board, firmware/esp32s3_devkitc, docs/features/42)
// still needs from the Panel Manager: everything a person does at the touchscreen today but can't do in the browser.
// Drawn as Panel Manager pages, with panel-pages.js's helpers and panel.css's parts (WEB-026); nothing here is built.
//   - Installer > Sirens and outputs: wired outputs and 433 MHz sirens (ui_sirens.c), with a review and Test it
//   - Overview: Emergency (police, fire, medical), each behind a confirmation (screen_main.xml's Emergency)
//   - Smart Home: the devices' tiles by room, their controls, what asks for a master code (ui_devices.c)
//   - Z-Wave: a device's page (blink, wake-up, software, replace, switch, dimmer, relays, siren), region and reset
//   - Installer > Zones: the source "Keypad input" (ZSRC_KEYPAD, ui_sources.c), and Partitions and devices' keypad inputs
//   - Network: Forget this network
proposal({
  bl: 'BL-222',
  title: 'Everything a panel without a screen needs, in the Panel Manager',
  status: 'draft',
  decided: '',
  role: 'installer',
  why: [
    'A panel without a screen is set up and run only from the Panel Manager, but sirens and outputs, the emergency buttons, Smart Home, several Z-Wave actions, a zone on a keypad\'s terminal and forgetting the Wi-Fi are still only on the touchscreen. On a headless panel nobody can do them at all.',
    'Each gets its Panel Manager page or card, in the touchscreen\'s words: Installer > Sirens and outputs, Emergency on the Overview, a Smart Home page, a page for each Z-Wave device with the region and the network reset on Z-Wave radio, Keypad input in a zone\'s sensor with the keypads\' inputs under Partitions and devices, and Forget this network.',
  ],
  look: [
    ['outputs', 'Installer > Sirens and outputs: the wired outputs, the 433 MHz sirens and the radios\' sirens, each with its state; Test it beside each siren', {role: 'installer', scenario: 'ready', parts: '1'}],
    ['output/w1', 'A wired siren: what it sounds for, its partitions, how it is wired, its supervision input; change something, then Review', {role: 'installer', scenario: 'ready'}],
    ['output/w3', 'An armed light: on while its partitions are armed (no siren rows)', {role: 'installer', scenario: 'ready'}],
    ['output/new-wired', 'Add a wired output: what it is and a free terminal first; the review lists everything it will save', {role: 'installer', scenario: 'ready'}],
    ['output/r1', 'A 433 MHz siren: not supervised, what it sounds for, Test it, Pair it again, Remove it', {role: 'installer', scenario: 'ready'}],
    ['output/pair-r1', 'Pairing a 433 MHz siren: its learning mode, Send the pairing signal, Test it (Add a siren on the list opens the same for a new one)', {role: 'installer', scenario: 'ready'}],
    ['outputs/trouble', 'A problem: the wired siren\'s supervision input is open, and nothing can transmit on 433 MHz', {role: 'installer', scenario: 'ready'}],
    ['outputs/empty', 'Empty: no output or siren yet, and the ways to add one', {role: 'installer', scenario: 'ready'}],
    ['outputs', 'Armed Away: tests and changes wait; the armed light says On', {role: 'installer', scenario: 'armed'}],
    ['outputs', 'An alarm sounding: the sirens in Home say Sounding, in steady words', {role: 'installer', scenario: 'alarm'}],
    ['overview', 'Overview as the system master: Emergency under the partition, Police, Fire and Medical; each asks to confirm', {role: 'master', scenario: 'ready', parts: '1'}],
    ['overview', 'Two partitions: the confirmation asks which partition', {role: 'master', scenario: 'ready', parts: '2'}],
    ['overview', 'Armed Away as Sam, a standard user: Emergency stays (a panic works in any state)', {role: 'standard', scenario: 'armed', parts: '1'}],
    ['overview', 'As the installer: no Emergency (the installer\'s browser login doesn\'t arm or disarm)', {role: 'installer', scenario: 'ready', parts: '1'}],
    ['devices', 'Smart Home as the system master: Favorites, then the rooms; each tile\'s state and controls, a failed change, an offline device', {role: 'master', scenario: 'ready'}],
    ['devices/all', 'All devices, and below them What asks for a master code and Rooms (system master and installer)', {role: 'master', scenario: 'ready'}],
    ['devices', 'As Sam, a standard user: the same tiles; Front Door\'s Unlock and Garage Door\'s Open are for masters only', {role: 'standard', scenario: 'ready'}],
    ['devices', 'Armed Away: the tiles work as before; unlocking and opening still ask for a master code', {role: 'master', scenario: 'armed'}],
    ['devices/none', 'Empty: no device yet, and where devices come from', {role: 'master', scenario: 'ready'}],
    ['devices/error', 'The devices could not be loaded: Try again', {role: 'master', scenario: 'ready'}],
    ['zwave', 'Z-Wave > Devices: each row gains Open its page', {role: 'installer', scenario: 'ready', zwave: '1'}],
    ['zwave-device/14', 'A Z-Wave siren: what it sounds for, partitions, chirps, volume, supervision, Test it', {role: 'installer', scenario: 'ready', zwave: '1'}],
    ['zwave-device/13', 'A dimmer: brightness, on and off, Blink its light, Check for an update', {role: 'installer', scenario: 'ready', zwave: '1'}],
    ['zwave-device/15', 'A relay kit: its relays, and one as the garage door opener with the door\'s sensor', {role: 'installer', scenario: 'ready', zwave: '1'}],
    ['zwave-device/2', 'A battery sensor: Checks in (its wake-up interval), software that waits for a wake-up, Replace it', {role: 'installer', scenario: 'ready', zwave: '1'}],
    ['zwave-device/14', 'Armed Away: the device\'s page reads, its changes and tests wait', {role: 'installer', scenario: 'armed', zwave: '1'}],
    ['zwaveradio', 'Z-Wave radio: Network, with Region and Reset the network, each with a review and your password', {role: 'installer', scenario: 'ready', zwave: '1'}],
    ['zone-setup/new', 'Installer > Zones, a new zone: Where is the sensor? offers Keypad input; pick it: the keypad\'s terminals, IO5 opens and closes and is marked', {role: 'installer', scenario: 'ready'}],
    ['zone-setup/11', 'Zone 11, Workshop door: its sensor is Keypad input · Keypad 1 · IO4', {role: 'installer', scenario: 'ready'}],
    ['partitions', 'Partitions and devices: Hall keypad says 4 inputs, 1 feeds a zone; Keypad inputs lists IO4 to IO7 and what each feeds', {role: 'installer', scenario: 'ready'}],
    ['partitions/keypad-offline', 'The keypad isn\'t answering: its inputs aren\'t known, its zone shows Check', {role: 'installer', scenario: 'ready'}],
    ['network', 'Network: Forget this network, with a review and your password (a panel with a screen)', {role: 'master', scenario: 'ready'}],
    ['network/headless', 'The same on a panel without a screen: it says the setup Wi-Fi opens, and how to get back', {role: 'master', scenario: 'ready'}],
    ['network/headless', 'Armed Away: forgetting is refused until everything is disarmed', {role: 'master', scenario: 'armed'}],
  ],
  replaces: ['overview', 'zwave', 'zwaveradio', 'zone-setup', 'partitions', 'network'],
  today: {outputs: 'partitions', output: 'partitions', devices: 'automations', 'zwave-device': 'zwave'},
  notes: [
    'Why: a headless panel (docs/features/42) has no touchscreen, so whatever only the touchscreen does can\'t be done on it. These are the touchscreen-only things a headless owner meets; the words are the touchscreen\'s (ui_sirens.c, ui_devices.c, ui_zwave.c, ui_sources.c, screen_main.xml), so the two say the same (WORD-006).',
    'Sirens and outputs (new page, Installer, installer only): three lists. Wired outputs: Siren, Strobe, Armed light, Smoke detector power, each "Terminal GPIO 47 · what it follows", with its state (Ready, Sounding, Siren problem; an armed light On or Off), Test it for a siren or strobe, and Edit. 433 MHz sirens: the transmitter (Ready or None), each siren "Address 9E3B5 · Not supervised", Test it, Edit, Add a siren. Zigbee and Z-Wave sirens: listed, opened on their device pages. Changes and tests wait while a partition is armed ("Not while one of its partitions is armed").',
    'A wired output\'s page: It sounds for (Burglary and panic, Fire and CO, Partitions, Also sounds for) or It follows (an armed light\'s Partitions), How it is wired (What it is, Terminal from the free ones, Relay board, Chirp when arming and disarming, Supervision input), Check it (Test it, Remove it). Review lists the changes; a siren that sounds for nothing, or in no partition, is refused as on the panel: "Not saved: it must sound for something, in a partition". Also sounds for shows only with two or more partitions.',
    'A 433 MHz siren\'s page: the amber note "Not supervised: the panel can\'t hear this siren. Test it now and then.", the same It sounds for rows ("Its one sound"), Chirp ("Its own arm and disarm sounds"), Test it, Pair it again, Remove it ("Sends it the stop signal first"). Add a siren makes one (burglary and fire in its partitions, for now) and opens the pairing page: its three steps and Send the pairing signal.',
    'API (new): GET /api/v1/outputs {transmitter, free: [terminals], wired: [{slot, function: siren|strobe|armed|smoke, gpio, partitions, also, burglary, fire, squawk, active_low, sense, trouble}], rf: [{slot, address, partitions, also, burglary, fire, squawk}], radio: [{source, id, name}]}; POST /api/v1/outputs {op: add|save|test|remove|pair, kind: wired|rf, slot, values} answers {result: ok|armed|in_use|incomplete|full|sounding|not_allowed, slot}, installer only, refused while armed. The same calls as the console\'s "out wired" and "out rf" (outputs_service). The pretend panel answers them in this file.',
    'Emergency (Overview): a card under the partitions with Police, Fire and Medical, for whoever may arm and disarm in the browser (not the installer, whose browser login has no arming). Each asks first ("Set off a police alarm?"); with two partitions the question asks which. The line under the buttons is the touchscreen\'s (online, offline or nothing connected; here "online"). It stays in every state, armed or in an alarm, as the panel\'s Emergency screen does. Police follows the panel\'s panic setting (System options > Panic keys: audible or silent).',
    'API (changed): POST /api/v1/partition has no panic action today. Proposed: {action: "panic", panic: "police"|"fire"|"medical", partition} for whoever may arm or disarm that partition (the access table\'s "anyone" with the partition), never refused while armed; the event names the browser user as a keypad\'s panic names its keypad. The pretend panel answers it in this file.',
    'Smart Home (new page, Household, everyone signed in): tabs Favorites, the rooms, Other, All devices (the touchscreen\'s rooms, BL-220); tiles for a switch, a dimmer (brightness), a thermostat (minus, set point, plus, mode), a cover (Open, Stop where it can, Close), a fan (Off, Low, Medium, High), a lock, a scene (Run). A failed change says so in amber (a press tries again); an offline device says since when and its controls are off. A device that asks for a master code shows a key: a system or partition master confirms with their password (the browser\'s proof of a master, as for other protected changes); anyone else sees "Only a master code can change it". Unlocking a lock and opening a garage door always ask (the touchscreen\'s "Unlocking asks for a code anyway").',
    'Smart Home for the system master and installer: What asks for a master code (a switch per device, Arranging asks for a master code) and Rooms (New room, Rename, Remove room: "Its devices go to Other"), each through a review with the password. A tile\'s Change opens its room and Favorites. Dragging tiles and their sizes stay at the panel: in a browser the order is the panel\'s.',
    'API (new): GET /api/v1/devices {rooms, arrange_code, devices: [{id, name, kind: switch|dimmer|thermostat|cover|fan|lock|scene, room, favorite, online, offline_since, failed, code, always_code, on, level, mode, target, now, doing, position, can_stop, speed, locked, ran}]} and POST /api/v1/devices {op: act|set|codes|room_add|room_rename|room_remove, ...} answering {result: ok|offline|busy|not_allowed}, the same as the touchscreen\'s dev_* backend (home_control). Open question for the owner: should a guest see Smart Home? Here everyone signed in does, as at the panel.',
    'Z-Wave (web_zwave.c says these stay at the panel): each row of Z-Wave > Devices gains Open its page. A device\'s page: Blink its light, Replace it, Check for an update and Update (a sleeping one waits for a wake-up), for a battery device Checks in (its wake-up interval: "It changes when it next wakes up"), for a switch On and Off, for a dimmer Brightness, for a relay kit its relays and the garage door opener (relay and sensor zone), for a siren the touchscreen\'s siren rows (sounds for, partitions, chirps, volume, supervision, Test it). Z-Wave radio gains Network: Region (the review says how many devices stop working) and Reset the network (all devices lose the panel), each with the installer\'s password, as the touchscreen asks for the installer code once more.',
    'API (changed): POST /api/v1/zwave gains the actions identify, wakeup {seconds}, fw_check, fw_install, fw_stop, replace, switch {on}, level {level}, relay {relay, on}, garage {relay, zone}, siren {values}, siren_test, region {region, me_password}, reset {me_password}, each the touchscreen\'s UI_ZW_REQ_* request; GET /api/v1/zwave gains, per device, its wake-up interval, software state and siren or relay settings. Here the pretend panel gains a dimmer (13), a siren (14) and a relay kit (15), and this file answers the new actions.',
    'Keypad input (Installer > Zones): the source picker offers "Keypad input · A contact wired to a keypad" (it is already in panel-pages.js\'s words; the panel\'s GET /api/v1/sources lists kind 6 once a keypad offers inputs). Its terminals are named as on the touchscreen, "IO5" with "Keypad 1", Open or Closed; one that changes while the picker is open is marked "Just opened or closed" and goes first. A zone on one reads "Keypad input · Keypad 1 · IO4" (today panel-pages.js would say "Input 0": the address is input 0, id = keypad << 8 | terminal). A terminal another zone uses says "In use: Workshop door".',
    'Partitions and devices: a keypad that offers inputs says how many and how many feed a zone; a new Keypad inputs section lists each terminal (IO4 to IO7), what it feeds (Open its zone) or Free (Make it a zone), and Not read yet; while the keypad isn\'t answering its inputs aren\'t known and its zones show Check. The panel\'s own keypad has no inputs of its own (its terminals are the wired zones) and isn\'t listed there.',
    'API (new): POST /api/v1/sources/search {kind: 6} answers the keypads\' terminals (zsrc_keypad) as candidates; GET /api/v1/keypads gains per keypad inputs: [{gpio, known, open, zone}]. The mockup asks keypads/inputs for that, since a proposal can\'t extend the pretend panel\'s keypads answer; built, it is one field of GET /api/v1/keypads. Mockup only: zone 11, Workshop door, is put on IO4 of the Hall keypad.',
    'Network: Forget this network ("The panel leaves Harbor House and forgets its password"), for whoever may change the network, after a review with the password; refused while armed (the panel\'s own lock). On a panel without a screen the panel then opens its setup Wi-Fi, as when it was new (BL-048); the review says so and how to get back. API (changed): POST /api/v1/wifi {action: "forget", me_password}.',
    'Left out: choosing which partition the Overview acts on (the Overview already shows a card for every partition the person may see); a zone\'s spoken-name words (a headless board has no speaker, and a word editor in the browser is its own row, as BL-202 decided); Display\'s Start screen (start_view: a panel without a screen has none to start, and each keypad keeps its own; for panels with screens it is a gap of its own in web_config.c\'s preferences table).',
    'New icons (Lucide, added by this proposal): flame (Fire) and fan. Nothing flashes or moves: Sounding, Offline and failed changes are steady words and colour (UI-020).',
    'Mockup only: the arguments after a page\'s address pick what the pretend panel answers (outputs/empty, outputs/trouble, devices/none, devices/error, partitions/keypad-offline, network/headless); they are not part of the design.',
  ],
  css: `
    .hg-emergency .choices { grid-template-columns: repeat(3, minmax(0, 1fr)); }
    .choice.hg-police .glyph { color: var(--bad); background: var(--bad-soft); }
    .choice.hg-fire .glyph { color: var(--warn); background: var(--warn-soft); }
    .choice.hg-medical .glyph { color: var(--tone-away); background: color-mix(in srgb, var(--tone-away) var(--tint), transparent); }
    .hg-emergency .note { display: block; margin-top: 12px; font-size: 12.5px; color: var(--muted); }
    .hg-list > li > .icon { flex-shrink: 0; color: var(--muted); }
    .hg-list > li .hg-acts { display: flex; gap: 8px; flex-wrap: wrap; justify-content: flex-end; }
    .hg-tiles { margin-bottom: 18px; }
    .hg-tile { padding: 16px 18px; display: flex; flex-direction: column; gap: 12px; min-width: 0; }
    .hg-tile-head { display: flex; gap: 12px; align-items: center; min-width: 0; }
    .hg-tile-head .main { flex: 1; min-width: 0; }
    .hg-tile-head .main strong { display: block; overflow-wrap: anywhere; }
    .hg-tile-head .main span { display: block; font-size: 12.5px; color: var(--muted); }
    .hg-chip { width: 40px; height: 40px; border-radius: 50%; display: grid; place-items: center; flex-shrink: 0; background: var(--surface-2); color: var(--muted); }
    .hg-tile.on .hg-chip { background: var(--accent-soft); color: var(--accent); }
    .hg-tile.off .hg-chip, .hg-tile.off .hg-acts { opacity: 0.5; }
    .hg-acts { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
    .hg-acts .filters { margin: 0; }
    .hg-acts input[type=range] { flex: 1; min-width: 120px; }
    .hg-acts .hg-set { min-width: 3em; text-align: center; font-weight: 650; }
    .hg-tile .hg-key { color: var(--muted); }
    @media (max-width: 640px) { .hg-emergency .choices { grid-template-columns: minmax(0, 1fr); } }
  `,
  routes(route) {
    route('GET', /^outputs$/, () => [200, hgOutAnswer()]);
    route('POST', /^outputs$/, (match, body) => { const result = hgOutDo(body); return [200, {result, slot: hgOut.last}]; });
    route('POST', /^partition$/, (match, body, mock) => body.action === 'panic' ? hgPanicDo(body, mock) : null);
    route('GET', /^devices$/, () => hgHome.mode === 'error' ? [503, {error: 'unavailable'}] : [200, hgHomeAnswer()]);
    route('POST', /^devices$/, (match, body) => [200, {result: hgHomeDo(body)}]);
    route('POST', /^zwave$/, (match, body, mock) => HG_ZW_ACTIONS.includes(body.action) ? [200, {result: hgZwDo(body, mock)}] : null);
    route('GET', /^sources$/, () => [200, {sources: HG_SOURCES}]);
    route('POST', /^sources\/search$/, (match, body, mock) => body.kind === 6 ? [200, {candidates: hgKeypadCands(mock)}] : null);
    route('GET', /^keypads\/inputs$/, (match, body, mock) => [200, {keypads: [hgKeypadInputs(mock)]}]);
  },
  apply(mock) {
    // ---- the pretend panel: what this proposal needs that it doesn't have (mockup only) ----
    hgOut.sets = {full: hgOutSeed(), empty: {transmitter: true, free: [47, 46, 45, 44, 43, 42], wired: [], rf: [], radio: []}};
    Object.assign(hgHome, hgHomeSeed());
    for (const d of HG_ZW_DEVICES) if (!mock.zwave.devices.some(x => x.node === d.node)) mock.zwave.devices.push({...d});
    Object.assign(mock.alarm.zones[10], {name: 'Workshop door', type: 3, device_class: 1, flags: 1, partition: 1});
    mock.bindings.set(11, {kind: 6, input: 0, id: String(1 * 256 + 4), ref: ''});
    ICONS.flame = '<path d="M8.5 14.5A2.5 2.5 0 0 0 11 12c0-1.38-.5-2-1-3-1.072-2.143-.224-4.054 2-6 .5 2.5 2 4.9 4 6.5 2 1.6 3 3.5 3 5.5a7 7 0 1 1-14 0c0-1.153.433-2.294 1-3a2.5 2.5 0 0 0 2.5 2.5z"/>';
    ICONS.fan = '<path d="M10.827 16.379a6.082 6.082 0 0 1-8.618-7.002l5.412 1.45a6.082 6.082 0 0 1 7.002-8.618l-1.45 5.412a6.082 6.082 0 0 1 8.618 7.002l-5.412-1.45a6.082 6.082 0 0 1-7.002 8.618l1.45-5.412Z"/><path d="M12 12v.01"/>';

    // ---- new pages, each in its group ----
    hgPlace('outputs', {title: tx('Sirens and outputs'), icon: 'siren', group: tx('Installer'), access: 'installer', render: hgOutputsPage}, 'partitions');
    hgPlace('output', {title: tx('Sirens and outputs'), icon: 'siren', nav: false, parent: 'outputs', access: 'installer', render: hgOutputPage}, 'outputs');
    hgPlace('devices', {title: tx('Smart Home'), icon: 'bulb', group: tx('Household'), render: hgDevicesPage}, 'people');
    hgPlace('zwave-device', {title: tx('Z-Wave device'), icon: 'radio', nav: false, parent: 'zwave', access: 'installer', render: hgZwDevicePage,
      when: () => !!state?.capabilities?.zwave}, 'zwaveradio');

    // ---- Overview: Emergency under the partitions ----
    const body = overviewBody;
    overviewBody = () => {
      const html = body(), at = html.indexOf('<div class="grid four gap">');
      return !hgMayPanic() || at < 0 ? html : html.slice(0, at) + hgEmergencyCard() + html.slice(at);
    };
    const overview = PAGES.overview.render;
    PAGES.overview.render = async arg => {
      const page = await overview(arg);
      return {html: page.html, bind() {
        page.bind?.();
        $('#overview')?.addEventListener('click', event => { const b = event.target.closest?.('[data-hg-panic]'); if (b) hgPanicAsk(b.dataset.hgPanic); });
      }};
    };

    // ---- Z-Wave: Open its page on each row; Network on the radio page ----
    const zw = PAGES.zwave.render;
    PAGES.zwave.render = async arg => {
      const page = await zw(arg);
      const html = page.html.replace(/(<tr id="zwdev-(\d+)">[\s\S]*?<td class="actions">)/g,
        (all, head, node) => `${head}<button type="button" data-hg-zw="${node}">${ht('Open its page')}</button>`);
      return {html, bind() { page.bind?.(); $$('[data-hg-zw]').forEach(b => { b.onclick = () => go('zwave-device', b.dataset.hgZw); }); }};
    };
    const radio = PAGES.zwaveradio.render;
    PAGES.zwaveradio.render = async arg => {
      const page = await radio(arg);
      const z = await api('zwave').catch(() => null);
      if (!z?.available) return page;
      return {html: page.html + hgZwNetworkCard(z), bind() { page.bind?.(); hgZwNetworkBind(z); }};
    };

    // ---- Zones: where a keypad input is ----
    const where = zpWhere;
    zpWhere = a => {
      if (a?.kind !== 6) return where(a);
      const id = Number(a.id) || 0;
      return `${zpKinds()[6][0]} · ${t('Keypad {0}', id >> 8)} · IO${id & 255}`;
    };

    // ---- Partitions and devices: the keypads' inputs ----
    const parts = PAGES.partitions.render;
    PAGES.partitions.render = async arg => {
      hgKp.offline = arg === 'keypad-offline';
      const page = await parts(arg);
      const kp = await api('keypads/inputs').catch(() => null);
      return {html: hgKeypadLines(page.html, kp) + hgKeypadSection(kp), bind() {
        page.bind?.();
        $$('[data-hg-kp-zone]').forEach(b => { b.onclick = () => go('zone-setup', b.dataset.hgKpZone); });
        $$('[data-hg-kp-new]').forEach(b => { b.onclick = () => go('zone-setup', 'new'); });
      }};
    };

    // ---- Network: Forget this network ----
    const network = PAGES.network.render;
    PAGES.network.render = async arg => {
      const page = await network(arg);
      const card = hgForgetCard(arg === 'headless');
      const at = page.html.indexOf('<form class="card" id="ip-form"');
      const start = at < 0 ? page.html.length : page.html.lastIndexOf('<section', at);
      return {html: page.html.slice(0, start) + card + page.html.slice(start), bind() { page.bind?.(); hgForgetBind(arg === 'headless'); }};
    };
  },
});

// ---- shared ------------------------------------------------------------------------------------------------------------

// A page in PAGES after another, so the menu keeps its groups together.
function hgPlace(id, page, after) {
  const entries = Object.entries(PAGES).filter(([k]) => k !== id);
  for (const [k] of entries) delete PAGES[k];
  let placed = false;
  for (const [k, v] of entries) {
    PAGES[k] = v;
    if (k === after) { PAGES[id] = page; placed = true; }
  }
  if (!placed) PAGES[id] = page;
}
const hgBit = n => 1 << (n - 1);
const hgFirstBit = () => partitionBits()[0]?.[0] || 1;
const hgLockedNotice = () => state?.locked ? notice('warn', t('Not while one of its partitions is armed'),
  t('The panel is armed or an alarm is sounding. Changes wait until everything is disarmed and quiet.')) : '';
const hgRow = (label, help, control) => `<div class="row"><div><span class="label">${esc(label)}</span>${help ? `<span class="help">${esc(help)}</span>` : ''}</div>
  <div class="control">${control}</div></div>`;
const hgButton = (text, attrs, cls = 'small') => `<button type="button" class="${cls}" ${attrs}>${esc(text)}</button>`;
const hgOff = on => on ? '' : ' disabled';
const hgNow = () => new Date().toLocaleTimeString(wordsLocale(), {hour: '2-digit', minute: '2-digit'});

// ---- Installer > Sirens and outputs ------------------------------------------------------------------------------------

// What the pretend panel says (mockup only). mode: full, empty, trouble (the siren's supervision input open, no transmitter).
const hgOut = {mode: 'full', sets: null, last: 0};
function hgOutSeed() {
  return {
    transmitter: true, free: [43, 42, 41, 40],
    wired: [
      {slot: 1, function: 'siren', gpio: 47, partitions: 1, also: 0, burglary: true, fire: true, squawk: true, active_low: true, sense: 48},
      {slot: 2, function: 'strobe', gpio: 46, partitions: 1, also: 0, burglary: true, fire: true, squawk: false, active_low: true, sense: 0},
      {slot: 3, function: 'armed', gpio: 45, partitions: 1, also: 0, burglary: false, fire: false, squawk: false, active_low: false, sense: 0},
      {slot: 4, function: 'smoke', gpio: 44, partitions: 0, also: 0, burglary: false, fire: false, squawk: false, active_low: true, sense: 0},
    ],
    rf: [{slot: 1, address: '9E3B5', partitions: 1, also: 0, burglary: true, fire: true, squawk: false}],
    radio: [{source: 'zigbee', id: '6', name: 'Hall siren'}, {source: 'zwave', id: '14', name: 'Upstairs siren'}],
  };
}
const HG_RF_ADDRESSES = ['9E3B5', '4C1A7', 'B2F90', '71D3E'];
const hgOutSet = () => hgOut.mode === 'empty' ? hgOut.sets.empty : hgOut.sets.full;
function hgOutAnswer() {
  const s = hgOutSet(), trouble = hgOut.mode === 'trouble';
  return {transmitter: s.transmitter && !trouble, free: [...s.free], radio: s.radio,
    wired: s.wired.map(w => ({...w, trouble: trouble && w.slot === 1})), rf: s.rf.map(r => ({...r}))};
}
// The panel's own answers to a change: who, while armed, a free terminal, something to sound for, room.
function hgOutDo(b) {
  if (!isInstaller()) return 'not_allowed';
  if (state?.locked) return 'armed';
  const s = hgOutSet(), v = b.values || {};
  const list = b.kind === 'rf' ? s.rf : s.wired, item = list.find(x => x.slot === b.slot);
  if (['test', 'pair'].includes(b.op)) return item ? 'ok' : 'failed';
  if (b.op === 'remove') {
    if (!item) return 'failed';
    list.splice(list.indexOf(item), 1);
    if (b.kind === 'wired') s.free.push(...[item.gpio, item.sense].filter(Boolean));
    return 'ok';
  }
  if (b.kind === 'rf' && b.op === 'add') {
    if (s.rf.length >= 4) return 'full';
    hgOut.last = [1, 2, 3, 4].find(n => !s.rf.some(r => r.slot === n));
    s.rf.push({slot: hgOut.last, address: HG_RF_ADDRESSES[hgOut.last - 1], partitions: hgFirstBit(), also: 0, burglary: true, fire: true, squawk: false});
    return 'ok';
  }
  const sounds = ['siren', 'strobe'].includes(v.function ?? 'siren');
  if ((sounds || b.kind === 'rf') && (!(v.burglary || v.fire) || !v.partitions)) return 'incomplete';
  if (v.function === 'armed' && !v.partitions) return 'incomplete';
  if (b.kind === 'rf') { if (!item) return 'failed'; Object.assign(item, v); return 'ok'; }
  const mine = item ? [item.gpio, item.sense] : [];
  for (const pin of [v.gpio, v.sense].filter(Boolean)) if (!s.free.includes(pin) && !mine.includes(pin)) return 'in_use';
  if (v.gpio && v.gpio === v.sense) return 'in_use';
  s.free = [...s.free, ...mine.filter(Boolean)].filter(p => p !== v.gpio && p !== v.sense).sort((x, y) => y - x);
  if (item) { Object.assign(item, v); return 'ok'; }
  if (s.wired.length >= 8) return 'full';
  hgOut.last = [1, 2, 3, 4, 5, 6, 7, 8].find(n => !s.wired.some(w => w.slot === n));
  s.wired.push({slot: hgOut.last, ...v});
  return 'ok';
}
const hgOutWords = () => ({armed: t('Not while one of its partitions is armed'), in_use: t('That terminal is in use: pick a free one'),
  incomplete: t('Not saved: it must sound for something, in a partition'), full: t('No room for another one'),
  sounding: t('Not while it sounds'), failed: t('Not saved: try again'), not_allowed: t('Changes are made in installer mode.')});
async function hgOutSend(body) {
  const r = await api('outputs', body);
  if (r.result !== 'ok') throw new Error(hgOutWords()[r.result] || t('The panel answered {0}.', r.result));
  return r;
}
const hgFns = () => ({siren: [tc('wired output', 'Siren'), t('Sounds with the alarm'), 'siren'],
  strobe: [tc('wired output', 'Strobe'), t('On from an audible alarm until disarmed'), 'bulb'],
  armed: [tc('wired output', 'Armed light'), t('On while its partitions are armed'), 'lock'],
  smoke: [tc('wired output', 'Smoke detector power'), t('Cut briefly to reset 4-wire smoke detectors'), 'smoke']});
const hgPin = n => t('GPIO {0}', n);
const hgSounding = mask => (state.partitions || []).some(p => (mask & hgBit(p.number)) && p.alarm);
const hgArmedIn = mask => (state.partitions || []).some(p => (mask & hgBit(p.number)) && p.state !== 'disarmed');
function hgPartsWords(parts, also) {
  const own = partitionNames(parts), more = also & ~parts;
  return more ? t('{0}, also {1}', own, partitionNames(more)) : own;
}
function hgSoundsWords(x) {
  return x.burglary && x.fire ? t('Burglary and fire') : x.burglary ? tc('siren sounds for', 'Burglary and panic')
    : x.fire ? tc('siren sounds for', 'Fire and CO') : tc('sirens', 'None');
}
function hgWiredState(w) {
  if (w.function === 'smoke') return [tc('output state', 'On'), 'good'];
  if (w.function === 'armed') return hgArmedIn(w.partitions) ? [tc('output state', 'On'), 'info'] : [tc('output state', 'Off'), ''];
  if (w.trouble) return [t('Siren problem'), 'warn'];
  if (hgSounding(w.partitions | w.also)) return [tc('siren state', 'Sounding'), 'bad'];
  return [tc('siren state', 'Ready'), 'good'];
}
function hgWiredLine(w) {
  const what = w.function === 'smoke' ? hgFns().smoke[1] : w.function === 'armed' ? partitionNames(w.partitions)
    : `${hgSoundsWords(w)} · ${hgPartsWords(w.partitions, w.also)}`;
  return t('Terminal {0} · {1}', hgPin(w.gpio), what);
}
async function hgOutputsPage(arg) {
  hgOut.mode = ['empty', 'trouble'].includes(arg) ? arg : 'full';
  const o = await api('outputs');
  const may = isInstaller() && !state.locked;
  const fns = hgFns();
  const wired = o.wired.map(w => {
    const [state_, kind] = hgWiredState(w), [title, , glyph] = fns[w.function];
    return `<li>${icon(glyph)}<div class="main"><strong>${esc(title)}</strong><span>${esc(hgWiredLine(w))}</span></div>${tag(state_, kind)}
      <span class="hg-acts">${['siren', 'strobe'].includes(w.function) ? hgButton(t('Test it'), `data-hg-test="wired/${w.slot}"${hgOff(may)}`) : ''}
      ${hgButton(may ? t('Edit') : t('Open'), `data-hg-open="w${w.slot}"`)}</span></li>`;
  }).join('') || `<li class="empty">${ht('No wired output yet.')}</li>`;
  const sends = o.transmitter;
  const rf = `<li>${icon('radio')}<div class="main"><strong>${ht('Transmitter')}</strong><span>${sends ? ht('433 MHz sirens can be sounded') : ht('433 MHz sirens can\'t be sounded')}</span></div>
      ${tag(sends ? tc('transmitter state', 'Ready') : tc('transmitter state', 'None'), sends ? 'good' : (o.rf.length ? 'warn' : ''))}</li>` +
    (o.rf.map(r => {
      const sounding = hgSounding(r.partitions | r.also);
      return `<li>${icon('siren')}<div class="main"><strong>${ht('433 MHz siren {0}', r.slot)}</strong><span>${esc(t('Address {0} · Not supervised', r.address))} · ${esc(hgSoundsWords(r))} · ${esc(hgPartsWords(r.partitions, r.also))}</span></div>
        ${!sends ? tag(t('Siren problem'), 'warn') : sounding ? tag(tc('siren state', 'Sounding'), 'bad') : tag(tc('siren state', 'Ready'), 'good')}
        <span class="hg-acts">${hgButton(t('Test it'), `data-hg-test="rf/${r.slot}"${hgOff(may && sends)}`)}${hgButton(may ? t('Edit') : t('Open'), `data-hg-open="r${r.slot}"`)}</span></li>`;
    }).join('') || `<li class="empty">${ht('No 433 MHz siren yet.')}</li>`);
  const radio = o.radio.map(r => `<li>${icon('siren')}<div class="main"><strong>${esc(r.name)}</strong><span>${r.source === 'zigbee' ? nt('Zigbee') : nt('Z-Wave')}</span></div>
      ${hgButton(t('Open its page'), `data-hg-radio="${r.source === 'zigbee' ? 'zigbee-device' : 'zwave-device'}/${esc(r.id)}"`)}</li>`).join('')
    || `<li class="empty">${ht('No Zigbee or Z-Wave siren.')}</li>`;
  const trouble = o.wired.some(w => w.trouble) || (!sends && o.rf.length);
  return {
    html: pageHead(t('Sirens and outputs'), t('What sounds and what switches when the alarm does')) + installerNotice() + hgLockedNotice() +
      (trouble ? notice('warn', t('Siren problem'), [o.wired.some(w => w.trouble) ? t('Its supervision input is open: check the siren and its wiring.') : '',
        !sends && o.rf.length ? t('Nothing can transmit on 433 MHz now: these sirens won\'t sound. It is a siren trouble.') : ''].filter(Boolean).join(' ')) : '') +
      `<section class="section"><div class="section-head"><div><h2>${ht('Wired outputs')}</h2><p>${ht('On the panel\'s terminals')}</p></div>
        ${hgButton(t('Add a wired output'), `data-hg-add="wired"${hgOff(may && o.free.length)}`, '')}</div>
        <div class="card"><ul class="list hg-list">${wired}</ul></div>
        ${o.free.length ? '' : `<p class="muted-note">${ht('No free terminal: every one is a zone\'s or an output\'s')}</p>`}</section>
      <section class="section"><div class="section-head"><div><h2>${ht('433 MHz sirens')}</h2><p>${ht('One that learns its panel like a key fob')}</p></div>
        ${hgButton(t('Add a siren'), `data-hg-add="rf"${hgOff(may && sends && o.rf.length < 4)}`, '')}</div>
        <div class="card"><ul class="list hg-list">${rf}</ul></div></section>
      <section class="section"><div class="section-head"><div><h2>${ht('Zigbee and Z-Wave sirens')}</h2>
        <p>${ht('Zigbee and Z-Wave sirens are set on their device pages, under Zigbee and Z-Wave.')}</p></div></div>
        <div class="card"><ul class="list hg-list">${radio}</ul></div></section>`,
    bind() {
      $$('[data-hg-open]').forEach(b => { b.onclick = () => go('output', b.dataset.hgOpen); });
      $$('[data-hg-radio]').forEach(b => { b.onclick = () => { const [id, arg] = b.dataset.hgRadio.split('/'); go(id, arg); }; });
      $$('[data-hg-test]').forEach(b => { b.onclick = () => { const [kind, slot] = b.dataset.hgTest.split('/'); hgTest(kind, Number(slot)); }; });
      $$('[data-hg-add]').forEach(b => {
        b.onclick = async () => {
          if (b.dataset.hgAdd === 'wired') return go('output', 'new-wired');
          try { const r = await hgOutSend({op: 'add', kind: 'rf'}); go('output', 'pair-r' + r.slot); } catch (error) { toast(error.message, 'error'); }
        };
      });
    },
  };
}
async function hgTest(kind, slot) {
  try { await hgOutSend({op: 'test', kind, slot}); toast(t('Sounding for 2 seconds')); } catch (error) { toast(error.message, 'error'); }
}
// A wired output, a 433 MHz siren or its pairing: #/output/w1, r1, pair-r1, new-wired.
async function hgOutputPage(arg) {
  const o = await api('outputs');
  const back = ['outputs', t('Sirens and outputs')];
  if (arg === 'new-wired') return hgWiredPage(o, null, back);
  let m = /^w(\d+)$/.exec(arg);
  const w = m && o.wired.find(x => x.slot === Number(m[1]));
  if (w) return hgWiredPage(o, w, back);
  m = /^(pair-)?r(\d+)$/.exec(arg);
  const r = m && o.rf.find(x => x.slot === Number(m[2]));
  if (r) return m[1] ? hgPairPage(o, r, back) : hgRfPage(o, r, back);
  return {html: pageHead(t('Wired output'), '', '', back) + notice('info', t('It was removed.'))};
}
function hgScopeSpecs(armedLight, sounds, help = {}) {
  const bits = partitionBits(), many = bits.length > 1;
  return [
    {name: 'burglary', label: tc('siren sounds for', 'Burglary and panic'), type: 'switch', help: t('A silent panic stays silent'), showIf: sounds},
    {name: 'fire', label: tc('siren sounds for', 'Fire and CO'), type: 'switch', help: help.fire || t('The fire pattern for fire, the CO pattern for CO'), showIf: sounds},
    {name: 'partitions', label: tc('siren', 'Partitions'), type: 'bits', options: bits,
      help: t('Their alarms and its chirps'), showIf: v => sounds(v) || armedLight(v)},
    {name: 'also', label: t('Also sounds for'), type: 'bits', options: bits, help: t('Other partitions\' alarms, never their chirps'), showIf: v => many && sounds(v)},
  ];
}
function hgWiredSpecs(o, w, fnNow) {
  const mine = w ? [w.gpio, w.sense].filter(Boolean) : [];
  const pins = [...new Set([...mine, ...o.free])].sort((a, b) => b - a).map(p => [p, hgPin(p)]);
  const fns = hgFns();
  const sounds = v => ['siren', 'strobe'].includes(v.function), armed = v => v.function === 'armed';
  const scope = hgScopeSpecs(armed, sounds);
  scope[2].describe = () => (($('#hg-out-form [name="function"]')?.value || fnNow) === 'armed' ? t('On while any of them is armed') : t('Their alarms and its chirps'));
  return {
    scope,
    wiring: [
      {name: 'function', label: t('What it is'), type: 'select', options: Object.entries(fns).map(([k, [title]]) => [k, title]),
        describe: v => fns[v]?.[1] || ''},
      {name: 'gpio', label: t('Terminal'), type: 'select', numeric: true, options: pins, help: t('A terminal no zone uses')},
      {name: 'active_low', label: t('Relay board'), type: 'select', numeric: true, options: [[1, t('Switches on low')], [0, t('Switches on high')]], help: t('Most switch on low')},
      {name: 'squawk', label: t('Chirp when arming and disarming'), type: 'switch', help: t('One chirp to arm, two to disarm'), showIf: v => v.function === 'siren'},
      {name: 'sense', label: t('Supervision input'), type: 'select', numeric: true, options: [[0, tc('supervision input', 'None')], ...pins],
        help: t('Its fault contact: open is a siren trouble'), showIf: v => v.function === 'siren'},
    ],
  };
}
function hgCheckCard(rows) {
  return `<section class="card gap"><div class="card-head"><div><h2>${esc(tc('siren section', 'Check it'))}</h2></div></div><div class="rows">${rows.join('')}</div></section>`;
}
const hgFoot = label => `<div class="card-foot"><span class="note">${ht('Changes apply after you review them.')}</span><button class="primary">${esc(label)}</button></div>`;
function hgWiredPage(o, w, back) {
  const isNew = !w, fns = hgFns(), may = isInstaller() && !state.locked;
  const values = isNew ? {function: 'siren', gpio: o.free[0] || 0, active_low: 1, burglary: true, fire: true, partitions: hgFirstBit(), also: 0, squawk: false, sense: 0}
    : {...w, active_low: w.active_low ? 1 : 0};
  const {scope, wiring} = hgWiredSpecs(o, w, values.function), specs = [...wiring, ...scope];
  const followsTitle = f => ['siren', 'strobe'].includes(f) ? tc('siren section', 'It sounds for') : tc('output section', 'It follows');
  const scopeCard = `<section class="card gap" id="hg-scope"${values.function === 'smoke' ? ' hidden' : ''}><div class="card-head"><div><h2 id="hg-scope-title">${esc(followsTitle(values.function))}</h2></div></div>
    <div class="rows">${fieldRows(scope, values)}</div></section>`;
  const wiringCard = `<section class="card gap"><div class="card-head"><div><h2>${ht('How it is wired')}</h2></div></div><div class="rows">${fieldRows(wiring, values)}</div></section>`;
  const check = isNew ? '' : hgCheckCard([
    ['siren', 'strobe'].includes(w.function) ? hgRow(t('Test it'), t('Sounds it for 2 seconds'), hgButton(t('Test it'), 'id="hg-test"')) : '',
    hgRow(t('Remove it'), t('Its terminal goes back to the zones'), hgButton(t('Remove it'), 'id="hg-remove"', 'small danger'))]);
  if (isNew && !o.free.length) {
    return {html: pageHead(t('Add a wired output'), '', '', back) + notice('warn', t('No free terminal: every one is a zone\'s or an output\'s'))};
  }
  return {
    html: pageHead(isNew ? t('Add a wired output') : fns[w.function][0], isNew ? t('A relay or MOSFET module on a free terminal') : t('On terminal {0}', hgPin(w.gpio)), '', back) +
      installerNotice() + hgLockedNotice() +
      (w?.trouble ? notice('warn', t('Siren problem'), t('Its supervision input is open: check the siren and its wiring.')) : '') +
      `<form id="hg-out-form" novalidate><fieldset${may ? '' : ' disabled'} style="border:0;margin:0;padding:0">
      ${isNew ? wiringCard + scopeCard : scopeCard + wiringCard}${check}
      <section class="card gap">${hgFoot(isNew ? t('Review') : t('Review changes'))}</section></fieldset></form>`,
    bind() {
      const form = $('#hg-out-form');
      if (!form) return;
      liveFields(form, specs);
      form.addEventListener('change', () => {
        const f = form.querySelector('[name="function"]').value;
        $('#hg-scope').hidden = f === 'smoke';
        $('#hg-scope-title').textContent = followsTitle(f);
      });
      const title = isNew ? '' : fns[w.function][0];
      if ($('#hg-test')) $('#hg-test').onclick = () => hgTest('wired', w.slot);
      if ($('#hg-remove')) $('#hg-remove').onclick = () => review({title: t('Remove it?'), intro: t('It stops switching, and its terminal goes back to the zones.'),
        confirm: t('Remove'), danger: true, done: t('Removed.'), run: () => hgOutSend({op: 'remove', kind: 'wired', slot: w.slot}), after: () => go('outputs')});
      form.onsubmit = event => {
        event.preventDefault();
        try {
          const {values: changed, changes} = collect(form, specs, isNew ? {} : values);
          if (!changes.length) return toast(t('Nothing changed.'));
          const all = {...values, ...changed};
          const sounds = ['siren', 'strobe'].includes(all.function);
          if ((sounds && (!(all.burglary || all.fire) || !all.partitions)) || (all.function === 'armed' && !all.partitions)) throw new Error(hgOutWords().incomplete);
          const v = {...all, active_low: !!all.active_low, sense: all.function === 'siren' ? all.sense : 0, partitions: all.function === 'smoke' ? 0 : all.partitions};
          review({title: isNew ? t('Add a wired output?') : t('Save {0}?', title), changes, confirm: isNew ? t('Add') : t('Save changes'),
            done: isNew ? t('Added.') : t('Saved on the panel.'), run: () => hgOutSend({op: isNew ? 'add' : 'save', kind: 'wired', slot: w?.slot || 0, values: v}),
            after: () => go('outputs')});
        } catch (error) {
          toast(error.message, 'error');
        }
      };
    },
  };
}
function hgRfPage(o, r, back) {
  const may = isInstaller() && !state.locked;
  const scope = hgScopeSpecs(() => false, () => true, {fire: t('Its one sound')});
  const specs = [...scope, {name: 'squawk', label: t('Chirp when arming and disarming'), type: 'switch', help: t('Its own arm and disarm sounds')}];
  const title = t('433 MHz siren {0}', r.slot);
  return {
    html: pageHead(title, t('Address {0} · Not supervised', r.address), '', back) + installerNotice() + hgLockedNotice() +
      notice('warn', o.transmitter ? t('Not supervised: the panel can\'t hear this siren. Test it now and then.')
        : t('Nothing can transmit on 433 MHz now: this siren won\'t sound. It is a siren trouble.')) +
      `<form id="hg-rf-form" novalidate><fieldset${may ? '' : ' disabled'} style="border:0;margin:0;padding:0">
      <section class="card gap"><div class="card-head"><div><h2>${esc(tc('siren section', 'It sounds for'))}</h2></div></div><div class="rows">${fieldRows(specs, r)}</div>
      ${hgFoot(t('Review changes'))}</section>
      ${hgCheckCard([hgRow(t('Test it'), t('Sounds it for 2 seconds'), hgButton(t('Test it'), `id="hg-test"${hgOff(o.transmitter)}`)),
        hgRow(t('Pair it again'), t('Its learning mode first'), hgButton(t('Pair it again'), 'id="hg-pair"')),
        hgRow(t('Remove it'), t('Sends it the stop signal first'), hgButton(t('Remove it'), 'id="hg-remove"', 'small danger'))])}
      </fieldset></form>`,
    bind() {
      const form = $('#hg-rf-form');
      if (!form) return;
      liveFields(form, specs);
      $('#hg-test').onclick = () => hgTest('rf', r.slot);
      $('#hg-pair').onclick = () => go('output', 'pair-r' + r.slot);
      $('#hg-remove').onclick = () => review({title: t('Remove it?'), intro: t('It gets the stop signal, and the panel no longer sounds it.'),
        confirm: t('Remove'), danger: true, done: t('Removed.'), run: () => hgOutSend({op: 'remove', kind: 'rf', slot: r.slot}), after: () => go('outputs')});
      form.onsubmit = event => {
        event.preventDefault();
        try {
          const {values, changes} = collect(form, specs, r);
          if (!changes.length) return toast(t('Nothing changed.'));
          const all = {...r, ...values};
          if (!(all.burglary || all.fire) || !all.partitions) throw new Error(hgOutWords().incomplete);
          review({title: t('Save {0}?', title), changes, run: () => hgOutSend({op: 'save', kind: 'rf', slot: r.slot, values: all}), after: () => go('outputs')});
        } catch (error) {
          toast(error.message, 'error');
        }
      };
    },
  };
}
function hgPairPage(o, r, back) {
  const may = isInstaller() && !state.locked;
  return {
    html: pageHead(t('Pair a 433 MHz siren'), t('433 MHz siren {0}', r.slot), '', back) + installerNotice() + hgLockedNotice() +
      (o.transmitter ? '' : notice('warn', t('Nothing can transmit on 433 MHz'))) +
      `<section class="card gap"><div class="rows">
        ${hgRow(t('1. Put the siren in its learning mode: its manual says how, usually by holding its button until it beeps.'), '', '')}
        ${hgRow(t('2. Send the pairing signal. Most sirens beep to say they learned it.'), '', hgButton(t('Send the pairing signal'), `id="hg-send"${hgOff(may && o.transmitter)}`, 'small primary'))}
        ${hgRow(t('3. Test it. If it stays quiet, send the pairing signal again.'), t('Sounds it for 2 seconds'), hgButton(t('Test it'), `id="hg-test"${hgOff(may && o.transmitter)}`))}
        ${hgRow(t('What it sounds for'), t('Burglary and fire in its partitions, for now'), hgButton(t('Open'), 'id="hg-settings"'))}
      </div></section>`,
    bind() {
      $('#hg-send').onclick = async () => {
        try { await hgOutSend({op: 'pair', kind: 'rf', slot: r.slot}); toast(t('Sending the pairing signal three times')); } catch (error) { toast(error.message, 'error'); }
      };
      $('#hg-test').onclick = () => hgTest('rf', r.slot);
      $('#hg-settings').onclick = () => go('output', 'r' + r.slot);
    },
  };
}

// ---- Overview: Emergency -----------------------------------------------------------------------------------------------

// Whoever may arm and disarm in the browser (the installer's login doesn't): a panic needs no code at the panel either.
const hgMayPanic = () => state?.access?.arm !== false && (state?.partitions || []).length > 0;
const hgPanics = () => ({
  police: {name: tc('panic key', 'Police'), glyph: 'siren', title: t('Set off a police alarm?'), done: t('Police alarm set off'),
    intro: t('The panel sounds its police alarm, as its Emergency screen does when a button is held.')},
  fire: {name: tc('panic key', 'Fire'), glyph: 'flame', title: t('Set off a fire alarm?'), done: t('Fire alarm set off'),
    intro: t('The panel sounds its fire alarm, as its Emergency screen does when a button is held.')},
  medical: {name: tc('panic key', 'Medical'), glyph: 'heart', title: t('Set off a medical alarm?'), done: t('Medical alarm set off'),
    intro: t('The panel sounds its medical alarm, as its Emergency screen does when a button is held.')},
});
function hgEmergencyCard() {
  const p = hgPanics();
  return `<section class="section hg-emergency"><div class="section-head"><div><h2>${ht('Emergency')}</h2>
    <p>${ht('Sets off the alarm on the panel. It asks before it does.')}</p></div></div>
    <div class="card pad"><div class="choices">${['police', 'fire', 'medical'].map(k =>
      choice('hg-' + k, icon(p[k].glyph), p[k].name, '', `data-hg-panic="${k}" data-k="hg-panic-${k}"`)).join('')}</div>
    <span class="note">${ht('A connected service is online and is sent the alarm · The panel can\'t call emergency services')}</span></div></section>`;
}
function hgPanicAsk(kind) {
  const w = hgPanics()[kind], parts = state.partitions || [];
  openDialog(`<form id="hg-panic-form" novalidate>
    <div class="dialog-head"><h2 id="dialog-title">${esc(w.title)}</h2><p>${esc(w.intro)}</p></div>
    <div class="dialog-body">${parts.length > 1 ? `<div class="rows">${fieldRow({name: 'partition', label: t('Partition'), type: 'select',
      options: parts.map(p => [p.number, partName(p)])}, parts[0].number)}</div>` : ''}
      <p class="muted-note">${ht('Disarm with a code to stop it. The event log says it came from this browser, with your name.')}</p></div>
    <p class="dialog-error" id="dialog-error" role="alert"></p>
    <div class="dialog-foot"><button type="button" id="dialog-cancel">${ht('Cancel')}</button>
    <button type="submit" class="danger solid">${ht('Set off the alarm')}</button></div></form>`);
  $('#dialog-cancel').onclick = closeDialog;
  $('#hg-panic-form').onsubmit = async event => {
    event.preventDefault();
    const button = event.submitter || $('#hg-panic-form [type=submit]');
    button.disabled = true;
    try {
      const partition = Number($('#hg-panic-form [name="partition"]')?.value || parts[0]?.number || 1);
      await api('partition', {action: 'panic', panic: kind, partition});
      closeDialog();
      toast(w.done);
      await show();
    } catch (error) {
      $('#dialog-error').textContent = error.message;
      button.disabled = false;
    }
  };
}
// The pretend panel's answer: the partition goes into alarm as a keypad's panic would put it.
function hgPanicDo(b, mock) {
  if (!hgMayPanic()) return [403, {error: 'not_allowed'}];
  const p = mock.parts[(b.partition || 1) - 1];
  if (!p || !['police', 'fire', 'medical'].includes(b.panic)) return [400, {error: 'invalid'}];
  if (!p.alarm) p.alarm_at = Date.now();
  Object.assign(p, {alarm: true, siren: true, alarm_kind: {police: 'panic', fire: 'fire', medical: 'aux'}[b.panic]});
  mock.addEvent('alarm', {police: 'Panic alarm', fire: 'Fire alarm', medical: 'Medical alarm'}[b.panic], 'From the Panel Manager', 0, 7, p, 0, state?.user || 0);
  return [200, {}];
}

// ---- Smart Home --------------------------------------------------------------------------------------------------------

// What the pretend panel says (mockup only). mode: full, none (no device), error (could not be loaded).
const hgHome = {mode: 'full', rooms: [], devices: [], arrange_code: false};
function hgHomeSeed() {
  return {
    rooms: ['Outside', 'Downstairs', 'Living Room', 'Entry', 'Garage'], arrange_code: false,
    devices: [
      {id: 'light.porch', name: 'Porch Light', kind: 'switch', room: 'Outside', favorite: true, on: true},
      {id: 'light.hallway', name: 'Hallway Light', kind: 'dimmer', room: 'Downstairs', favorite: true, on: true, level: 40},
      {id: 'climate.hallway', name: 'Hallway', kind: 'thermostat', room: 'Downstairs', favorite: true, mode: 'heat', target: 20, now: 19, doing: 'heating'},
      {id: 'lock.front_door', name: 'Front Door', kind: 'lock', room: 'Entry', favorite: true, locked: true, code: true, always_code: true},
      {id: 'scene.good_night', name: 'Good Night', kind: 'scene', room: '', favorite: true, ran: '22:30'},
      {id: 'switch.living_lamp', name: 'Living Room Lamp', kind: 'switch', room: 'Living Room', favorite: false, on: false},
      {id: 'cover.living_blinds', name: 'Living Room Blinds', kind: 'cover', room: 'Living Room', favorite: false, position: 'open', can_stop: true},
      {id: 'switch.christmas', name: 'Christmas Lights', kind: 'switch', room: 'Outside', favorite: false, on: false, failed: 'on'},
      {id: 'cover.garage_door', name: 'Garage Door', kind: 'cover', room: 'Garage', favorite: false, position: 'closed', can_stop: false,
        online: false, offline_since: '14:05', code: true, always_code: true},
      {id: 'fan.bedroom', name: 'Bedroom Fan', kind: 'fan', room: '', favorite: false, speed: 'low'},
    ].map(d => ({online: true, failed: '', code: false, always_code: false, ...d})),
  };
}
function hgHomeAnswer() {
  return hgHome.mode === 'none' ? {rooms: [], devices: [], arrange_code: false}
    : {rooms: [...hgHome.rooms], devices: hgHome.devices.map(d => ({...d})), arrange_code: hgHome.arrange_code};
}
const hgIsMaster = () => ['master', 'partition_master'].includes(state?.authority);
const hgMayArrange = () => !hgHome.arrange_code || hgIsMaster() || isInstaller();
const hgNeedsCode = (d, action) => d.code && ((d.kind === 'lock' && action === 'unlock') || (d.kind === 'cover' && action === 'open' && d.always_code) || !d.always_code);
function hgHomeDo(b) {
  const d = hgHome.devices.find(x => x.id === b.id);
  if (b.op === 'act') {
    if (!d) return 'not_found';
    if (!d.online) return 'offline';
    if (hgNeedsCode(d, b.action) && !hgIsMaster()) return 'not_allowed';
    d.failed = '';
    const a = b.action;
    if (a === 'on' || a === 'off') d.on = a === 'on';
    if (a === 'level') { d.level = b.value; d.on = b.value > 0; }
    if (a === 'target') d.target = Math.max(5, Math.min(30, b.value));
    if (a === 'mode') { d.mode = b.value; d.doing = b.value === 'off' ? 'off' : b.value === 'cool' ? 'idle' : 'heating'; }
    if (a === 'open' || a === 'close') d.position = a === 'open' ? 'open' : 'closed';
    if (a === 'speed') d.speed = b.value;
    if (a === 'lock' || a === 'unlock') d.locked = a === 'lock';
    if (a === 'run') d.ran = hgNow();
    return 'ok';
  }
  if (!hgMayArrange() && ['set', 'room_add', 'room_rename', 'room_remove'].includes(b.op)) return 'not_allowed';
  if (b.op === 'set' && d) { Object.assign(d, {room: b.room, favorite: !!b.favorite}); return 'ok'; }
  if (b.op === 'codes') {
    if (!state?.access?.system) return 'not_allowed';
    for (const x of hgHome.devices) if (!x.always_code && b.codes && x.id in b.codes) x.code = !!b.codes[x.id];
    if ('arrange' in b) hgHome.arrange_code = !!b.arrange;
    return 'ok';
  }
  if (b.op === 'room_add') { if (hgHome.rooms.includes(b.name)) return 'exists'; hgHome.rooms.push(b.name); return 'ok'; }
  if (b.op === 'room_rename') {
    const i = hgHome.rooms.indexOf(b.room);
    if (i < 0) return 'not_found';
    hgHome.rooms[i] = b.name;
    hgHome.devices.forEach(x => { if (x.room === b.room) x.room = b.name; });
    return 'ok';
  }
  if (b.op === 'room_remove') {
    hgHome.rooms = hgHome.rooms.filter(r => r !== b.room);
    hgHome.devices.forEach(x => { if (x.room === b.room) x.room = ''; });
    return 'ok';
  }
  return 'invalid';
}
const hgHomeWords = () => ({offline: t('It can\'t be reached right now'), busy: t('It is still busy with the last change'),
  not_allowed: t('Only a master code can change it'), exists: t('There is already a room called {0}', ''), not_found: t('That can\'t be done right now')});
async function hgHomeSend(body) {
  const r = await api('devices', body);
  if (r.result !== 'ok') throw new Error(hgHomeWords()[r.result] || t('That can\'t be done right now'));
  return r;
}
const hgDevGlyph = d => ({switch: 'bulb', dimmer: 'bulb', thermostat: 'thermo', cover: d.id.includes('garage') ? 'garage' : 'window',
  fan: 'fan', lock: d.locked ? 'lock' : 'lockOpen', scene: 'sparkles'})[d.kind] || 'toggle';
const hgSpeeds = () => [['off', tc('fan speed', 'Off')], ['low', tc('fan speed', 'Low')], ['medium', tc('fan speed', 'Medium')], ['high', tc('fan speed', 'High')]];
const hgModes = () => [['off', tc('thermostat mode', 'Off')], ['heat', tc('thermostat mode', 'Heat')], ['cool', tc('thermostat mode', 'Cool')], ['auto', tc('thermostat mode', 'Auto')]];
const hgDeg = n => tc('temperature', '{0}°', n);
function hgDevState(d) {
  if (!d.online) return tc('since a date', 'Offline since {0}', d.offline_since || '');
  switch (d.kind) {
    case 'switch': return d.on ? tc('device state', 'On') : tc('device state', 'Off');
    case 'dimmer': return d.on ? tc('percent', '{0}%', d.level) : tc('device state', 'Off');
    case 'thermostat': return {heating: tc('thermostat doing', 'Heating · {0} now', hgDeg(d.now)), idle: tc('thermostat doing', 'Idle · {0} now', hgDeg(d.now)),
      off: tc('thermostat doing', 'Off · {0} now', hgDeg(d.now))}[d.doing] || hgDeg(d.now);
    case 'cover': return d.position === 'open' ? tc('device state', 'Open') : tc('device state', 'Closed');
    case 'fan': return hgSpeeds().find(([v]) => v === d.speed)?.[1] || tc('device state', 'Unknown');
    case 'lock': return d.locked ? tc('device state', 'Locked') : tc('device state', 'Unlocked');
    case 'scene': return d.ran ? t('Ran at {0}', d.ran) : '';
    default: return tc('device state', 'Unknown');
  }
}
const hgFailedWords = d => ({on: t('Didn\'t turn on'), off: t('Didn\'t turn off'), lock: t('Didn\'t lock'), unlock: t('Didn\'t unlock'),
  open: t('Didn\'t open'), close: t('Didn\'t close'), run: t('Didn\'t run')})[d.failed] || t('Didn\'t change');
const hgIsOn = d => d.online && ((['switch', 'dimmer'].includes(d.kind) && d.on) || (d.kind === 'thermostat' && d.doing === 'heating')
  || (d.kind === 'cover' && d.position === 'open') || (d.kind === 'fan' && d.speed !== 'off') || (d.kind === 'lock' && !d.locked));
function hgControls(d) {
  const off = d.online ? '' : ' disabled';
  const act = (label, action, extra = '', cls = 'small') => `<button type="button" class="${cls}" data-hg-act="${action}" data-hg-dev="${esc(d.id)}"${extra}${off}>${esc(label)}</button>`;
  const asks = action => hgNeedsCode(d, action) ? ' ' + icon('key', 'hg-key') : '';
  switch (d.kind) {
    case 'switch': return act(d.on ? t('Turn off') : t('Turn on'), d.on ? 'off' : 'on');
    case 'dimmer': return `${act(d.on ? t('Turn off') : t('Turn on'), d.on ? 'off' : 'on')}
      <input type="range" min="0" max="100" step="5" value="${esc(d.level)}" data-hg-level="${esc(d.id)}" aria-label="${ht('Brightness')}"${off}>`;
    case 'thermostat': return `${act('−', 'down', ` aria-label="${ht('Lower')}"`)}<span class="hg-set">${esc(hgDeg(d.target))}</span>${act('+', 'up', ` aria-label="${ht('Higher')}"`)}
      <select data-hg-mode="${esc(d.id)}" aria-label="${esc(tc('thermostat', 'Mode'))}"${off}>${hgModes().map(([v, l]) => `<option value="${v}"${v === d.mode ? ' selected' : ''}>${esc(l)}</option>`).join('')}</select>`;
    case 'cover': return act(tc('cover', 'Open'), 'open') + asks('open') + (d.can_stop ? act(tc('cover', 'Stop'), 'stop') : '') + act(tc('cover', 'Close'), 'close');
    case 'fan': return `<div class="filters">${hgSpeeds().map(([v, l]) => `<button type="button" data-hg-act="speed" data-hg-value="${v}" data-hg-dev="${esc(d.id)}" aria-pressed="${v === d.speed}"${off}>${esc(l)}</button>`).join('')}</div>`;
    case 'lock': return act(tc('lock', 'Lock'), 'lock') + act(tc('lock', 'Unlock'), 'unlock') + asks('unlock');
    case 'scene': return act(tc('scene', 'Run'), 'run', '', 'small primary');
    default: return '';
  }
}
function hgTile(d) {
  const sub = hgDevState(d);
  return `<div class="card hg-tile${hgIsOn(d) ? ' on' : ''}${d.online ? '' : ' off'}">
    <div class="hg-tile-head"><span class="hg-chip">${icon(hgDevGlyph(d))}</span><div class="main"><strong>${esc(d.name)}</strong>${sub ? `<span>${esc(sub)}</span>` : ''}</div>
      ${d.code ? `<span title="${esc(t('{0} asks for a master code', d.name))}">${icon('key', 'hg-key')}</span>` : ''}
      ${hgButton(t('Change'), `data-hg-change="${esc(d.id)}" aria-label="${esc(t('Change {0}', d.name))}"`)}</div>
    ${d.failed ? `<div>${tag(hgFailedWords(d), 'warn')}</div>` : !d.online ? `<div>${tag(tc('device state', 'Offline'), 'warn')}</div>` : ''}
    <div class="hg-acts">${hgControls(d)}</div></div>`;
}
// #/devices: Favorites; #/devices/room-N, other, all (mockup only: none, error).
async function hgDevicesPage(arg) {
  hgHome.mode = ['none', 'error'].includes(arg) ? arg : 'full';
  let data;
  try {
    data = await api('devices');
  } catch (error) {
    return {html: pageHead(t('Smart Home'), t('The lights, locks and other devices the panel controls.')) +
      notice('bad', t('The devices could not be loaded'), t('The panel didn\'t answer. Try again.'), `<div class="actions">${hgButton(t('Try again'), 'id="hg-retry"', '')}</div>`),
      bind() { $('#hg-retry').onclick = () => go('devices'); }};
  }
  const head = pageHead(t('Smart Home'), t('The lights, locks and other devices the panel controls.'));
  if (!data.devices.length) {
    return {html: head + `<section class="card pad"><h2>${ht('No devices yet')}</h2>
      <p class="muted-note">${ht('Devices come from Home Assistant, Zigbee and Z-Wave. Once one is added there, it shows here and on the keypads.')}</p></section>`};
  }
  const other = data.devices.filter(d => !d.room || !data.rooms.includes(d.room));
  const tabs = [['', tc('smart home', 'Favorites')], ...data.rooms.map((r, i) => ['room-' + i, r]), ...(other.length ? [['other', tc('smart home', 'Other')]] : []),
    ['all', tc('smart home', 'All devices')]];
  const tab = tabs.some(([id]) => id === arg) ? arg : '';
  const room = /^room-(\d+)$/.exec(tab);
  const shown = tab === '' ? data.devices.filter(d => d.favorite) : tab === 'other' ? other : tab === 'all' ? data.devices
    : data.devices.filter(d => d.room === data.rooms[Number(room[1])]);
  const system = !!state.access?.system, mayArrange = !data.arrange_code || hgIsMaster() || isInstaller();
  const codeSpecs = data.devices.map(d => ({name: 'code-' + d.id, label: d.name, type: 'switch', locked: d.always_code,
    help: d.always_code ? (d.kind === 'lock' ? t('Unlocking asks for a code anyway') : t('Opening asks for a code anyway')) : ''}));
  const codeValues = Object.fromEntries(data.devices.map(d => ['code-' + d.id, d.code]));
  const arrangeSpec = [{name: 'arrange', label: t('Arranging asks for a master code'), type: 'switch', help: t('Moving tiles, their sizes, Favorites and rooms')}];
  const codesCard = system ? `<section class="section"><form class="card" id="hg-codes" novalidate><div class="card-head"><div><h2>${ht('What asks for a master code')}</h2>
      <p>${ht('Devices that ask for a master code before a change')}</p></div></div>
      <div class="rows">${fieldRows(codeSpecs, codeValues)}${fieldRows(arrangeSpec, {arrange: data.arrange_code})}</div>${hgFoot(t('Review changes'))}</form></section>` : '';
  const roomsCard = system || mayArrange ? `<section class="section"><div class="section-head"><div><h2>${ht('Rooms')}</h2><p>${ht('The tabs beside Favorites, in their order')}</p></div>
      ${hgButton(t('New room'), `id="hg-room-add"${hgOff(mayArrange)}`, '')}</div>
      <div class="card"><ul class="list hg-list">${data.rooms.map((r, i) => {
        const n = data.devices.filter(d => d.room === r).length;
        return `<li><div class="main"><strong>${esc(r)}</strong><span>${esc(tn('{0} device', '{0} devices', n))}</span></div>
          <span class="hg-acts">${hgButton(t('Rename'), `data-hg-room-rename="${i}"${hgOff(mayArrange)}`)}${hgButton(tc('room', 'Remove'), `data-hg-room-remove="${i}"${hgOff(mayArrange)}`, 'small danger')}</span></li>`;
      }).join('') || `<li class="empty">${ht('No rooms yet')}</li>`}</ul></div></section>` : '';
  return {
    html: head + `<div id="hg-devices">
      <div class="filters" role="group" aria-label="${ht('Rooms')}">${tabs.map(([id, label]) => `<button type="button" data-hg-tab="${id}" aria-pressed="${id === tab}">${esc(label)}</button>`).join('')}</div>
      ${shown.length ? `<div class="grid three hg-tiles">${shown.map(hgTile).join('')}</div>`
        : `<p class="muted-note">${tab === '' ? ht('Nothing on Favorites yet: a device\'s Change puts it there.') : ht('An empty room, to add devices to')}</p>`}
      ${tab === 'all' ? codesCard + roomsCard : ''}</div>`,
    bind() {
      const root = $('#hg-devices');
      if (!root) return;
      root.addEventListener('click', event => {
        const b = event.target.closest?.('button');
        if (!b || b.disabled) return;
        if (b.dataset.hgTab !== undefined) return go('devices', b.dataset.hgTab);
        if (b.dataset.hgChange) return hgDevChange(data, data.devices.find(d => d.id === b.dataset.hgChange), tab);
        if (b.dataset.hgAct) {
          const d = data.devices.find(x => x.id === b.dataset.hgDev);
          if (b.dataset.hgAct === 'up' || b.dataset.hgAct === 'down') return hgDevAct(d, 'target', d.target + (b.dataset.hgAct === 'up' ? 0.5 : -0.5));
          return hgDevAct(d, b.dataset.hgAct, b.dataset.hgValue);
        }
        if (b.dataset.hgRoomRename) return hgRoomEdit(data.rooms[Number(b.dataset.hgRoomRename)]);
        if (b.dataset.hgRoomRemove) {
          const r = data.rooms[Number(b.dataset.hgRoomRemove)];
          return review({title: t('Remove {0}?', r), intro: t('Its devices go to Other'), confirm: tc('room', 'Remove'), danger: true, password: !!data.arrange_code,
            done: '', run: () => hgHomeSend({op: 'room_remove', room: r}), after: () => go('devices', 'all')});
        }
        if (b.id === 'hg-room-add') return hgRoomEdit('');
      });
      root.addEventListener('change', event => {
        const el = event.target;
        if (el.dataset?.hgLevel) hgDevAct(data.devices.find(d => d.id === el.dataset.hgLevel), 'level', Number(el.value));
        if (el.dataset?.hgMode) hgDevAct(data.devices.find(d => d.id === el.dataset.hgMode), 'mode', el.value);
      });
      const form = $('#hg-codes');
      if (form) form.onsubmit = event => {
        event.preventDefault();
        const specs = [...codeSpecs, ...arrangeSpec], before = {...codeValues, arrange: data.arrange_code};
        const {values, changes} = collect(form, specs, before);
        if (!changes.length) return toast(t('Nothing changed.'));
        const codes = Object.fromEntries(Object.entries(values).filter(([k]) => k.startsWith('code-')).map(([k, v]) => [k.slice(5), v]));
        review({title: t('What asks for a master code'), changes, password: true,
          run: me => hgHomeSend({op: 'codes', codes, ...('arrange' in values ? {arrange: values.arrange} : {}), me_password: me})});
      };
    },
  };
}
async function hgDevAct(d, action, value) {
  if (!d) return;
  if (hgNeedsCode(d, action)) {
    if (!hgIsMaster()) return toast(t('Only a master code can change it'), 'error');
    const title = action === 'unlock' ? t('Unlock {0}', d.name) : action === 'open' ? t('Open {0}', d.name) : t('Change {0}', d.name);
    return review({title, intro: t('{0} asks for a master code', d.name), password: true, confirm: title, done: '',
      run: me => hgHomeSend({op: 'act', id: d.id, action, value, me_password: me})});
  }
  try { await hgHomeSend({op: 'act', id: d.id, action, value}); } catch (error) { toast(error.message, 'error'); }
  await show();
}
function hgDevChange(data, d, tab) {
  if (!d) return;
  const specs = [
    {name: 'room', label: t('Rooms'), type: 'select', options: [['', t('No room')], ...data.rooms.map(r => [r, r])], help: t('Where it shows besides Favorites')},
    {name: 'favorite', label: tc('smart home', 'Favorites'), type: 'switch', help: t('Shown first, on its own tab')},
  ];
  const values = {room: data.rooms.includes(d.room) ? d.room : '', favorite: d.favorite};
  editDialog({title: t('Change {0}', d.name), intro: data.arrange_code ? t('Arranging asks for a master code') : '', specs, values,
    readOnly: !(!data.arrange_code || hgIsMaster() || isInstaller()),
    reviewOptions: {title: t('Change {0}', d.name), password: !!data.arrange_code, done: '', after: () => go('devices', tab),
      run: v => hgHomeSend({op: 'set', id: d.id, room: v.room ?? values.room, favorite: v.favorite ?? values.favorite})}});
}
function hgRoomEdit(room) {
  editDialog({title: room ? t('Rename {0}', room) : t('New room'), intro: room ? '' : t('Type a name for the room'),
    specs: [{name: 'name', label: t('Name'), type: 'text', maxLength: 23, required: true}], values: {name: room},
    reviewOptions: {title: room ? t('Rename {0}', room) : t('New room'), password: !!hgHome.arrange_code, done: '', after: () => go('devices', 'all'),
      run: v => hgHomeSend(room ? {op: 'room_rename', room, name: v.name.trim()} : {op: 'room_add', name: v.name.trim()})}});
}

// ---- Z-Wave: a device's page, and the network -------------------------------------------------------------------------

// Pretend devices for the kinds the mock lacks (mockup only): a dimmer, a siren and a relay kit.
const HG_ZW_DEVICES = [
  {node: 13, generation: 12, model: 'Hallway dimmer', kind: 4, security: 4, state: 0, unreachable: false, listening: true, signal: 'good', heard_s: 8, battery_low: false, zones: []},
  {node: 14, generation: 13, model: 'Upstairs siren', kind: 6, security: 4, state: 0, unreachable: false, listening: true, signal: 'good', heard_s: 30, battery_low: false, zones: []},
  {node: 15, generation: 14, model: 'Garage relays', kind: 5, security: 4, state: 0, unreachable: false, listening: true, signal: 'fair', heard_s: 15, battery_low: false, zones: []},
];
const HG_ZW_ACTIONS = ['identify', 'wakeup', 'fw_check', 'fw_install', 'fw_stop', 'replace', 'switch', 'level', 'relay', 'garage', 'siren', 'siren_test', 'region', 'reset'];
// What the pretend panel keeps for them (mockup only).
const hgZw = {wake: {2: 3600, 7: 14400, 8: 3600, 9: 43200, 11: 3600, 12: 3600}, fw: {}, on: {3: false, 13: true}, level: {13: 60}, relays: {15: [false, false]},
  garage: {15: {relay: 1, zone: 2}}, siren: {14: {burglary: true, fire: true, partitions: 1, also: 0, squawk: true, volume: 0, supervision_min: 240}}};
function hgZwDo(b, mock) {
  if (!isInstaller()) return 'not_allowed';
  if (state?.locked) return 'armed';
  const n = b.node;
  switch (b.action) {
    case 'region': mock.zwave.radio.region = b.region; return 'ok';
    case 'reset': mock.zwave.devices.length = 0; mock.zwave.radio.devices = 0; mock.zwave.radio.not_heard = 0; return 'ok';
    case 'wakeup': hgZw.wake[n] = b.seconds; return 'ok';
    case 'fw_check': hgZw.fw[n] = n === 2 ? {check: 'available', version: '2.1'} : n === 13 ? {check: 'available', version: '1.6'} : {check: 'up_to_date'}; return 'ok';
    case 'fw_install': hgZw.fw[n] = {...hgZw.fw[n], check: 'installing', percent: n === 2 ? 0 : 40}; return 'ok';
    case 'fw_stop': hgZw.fw[n] = {check: 'stopped'}; return 'ok';
    case 'switch': hgZw.on[n] = !!b.on; return 'ok';
    case 'level': hgZw.level[n] = b.level; hgZw.on[n] = b.level > 0; return 'ok';
    case 'relay': hgZw.relays[n][b.relay - 1] = !!b.on; return 'ok';
    case 'garage': hgZw.garage[n] = {relay: b.relay, zone: b.zone}; return 'ok';
    case 'siren': Object.assign(hgZw.siren[n], b.values); return 'ok';
    default: return 'ok';
  }
}
const hgZwWords = () => ({armed: t('Not while one of its partitions is armed'), not_allowed: t('Changes are made in installer mode.')});
async function hgZwSend(body) {
  const r = await api('zwave', body);
  if (r.result !== 'ok') throw new Error(hgZwWords()[r.result] || zwWord(ZW_RESULTS, r.result, r.result));
  return r;
}
const hgWakeChoices = () => [[1800, t('Every 30 min')], [3600, t('Every hour')], [14400, t('Every 4 hours')], [43200, t('Every 12 hours')], [86400, t('Every day')]];
async function hgZwDevicePage(arg) {
  const z = await api('zwave');
  const back = ['zwave', nt('Z-Wave')];
  const d = z.devices?.find(x => String(x.node) === String(arg)) || (arg === '' ? z.devices?.[0] : null);
  if (!d) return {html: pageHead(t('Z-Wave device'), '', '', back) + notice('info', t('It was removed.'))};
  const name = d.model || zwWord(ZW_KINDS, d.kind, t('Device'));
  const may = isInstaller() && !state.locked, off = hgOff(may);
  const at = {node: d.node, generation: d.generation};
  const [stateWords, stateKind] = zwStatus(d);
  const cards = [];
  // What it switches, what it sounds for
  if (d.kind === 3) cards.push(`<section class="card gap"><div class="card-head"><div><h2>${ht('Switch it')}</h2></div></div><div class="rows">
    ${hgRow(t('Now'), '', `${tag(hgZw.on[d.node] ? tc('output state', 'On') : tc('output state', 'Off'), hgZw.on[d.node] ? 'info' : '')}
      ${hgButton(hgZw.on[d.node] ? t('Turn off') : t('Turn on'), `data-hg-zwa="switch" data-on="${hgZw.on[d.node] ? 0 : 1}"${off}`)}`)}</div></section>`);
  if (d.kind === 4) cards.push(`<section class="card gap"><div class="card-head"><div><h2>${ht('Brightness')}</h2></div></div><div class="rows">
    ${hgRow(t('Now'), '', `${tag(hgZw.on[d.node] ? tc('percent', '{0}%', hgZw.level[d.node]) : tc('output state', 'Off'), hgZw.on[d.node] ? 'info' : '')}
      ${hgButton(hgZw.on[d.node] ? t('Turn off') : t('Turn on'), `data-hg-zwa="level" data-level="${hgZw.on[d.node] ? 0 : 100}"${off}`)}`)}
    ${hgRow(t('Brightness'), '', `<input type="range" min="0" max="100" step="5" value="${esc(hgZw.level[d.node])}" id="hg-zw-level" aria-label="${ht('Brightness')}"${off}>`)}</div></section>`);
  if (d.kind === 5) {
    const g = hgZw.garage[d.node];
    const zones = (await loadAlarm()).zones.filter(x => x.type).map(x => [x.number, x.name || tc('default zone name', 'Zone {0}', x.number)]);
    cards.push(`<section class="card gap"><div class="card-head"><div><h2>${ht('Its relays')}</h2>
      <p>${g.relay ? ht('Relay {0} opens the garage door', g.relay) : ht('Switch its relays, or make one a garage door opener, on its page.')}</p></div></div><div class="rows">
      ${hgZw.relays[d.node].map((on, i) => hgRow(t('Relay {0}', i + 1), g.relay === i + 1 ? t('Garage door opener') : '',
        `${tag(on ? tc('output state', 'On') : tc('output state', 'Off'), on ? 'info' : '')}${hgButton(on ? t('Turn off') : t('Turn on'), `data-hg-zwa="relay" data-relay="${i + 1}" data-on="${on ? 0 : 1}"${off}`)}`)).join('')}</div></section>
      <form class="card gap" id="hg-zw-garage" novalidate><div class="card-head"><div><h2>${ht('Garage door')}</h2><p>${ht('For locks, keypads and garage doors')}</p></div></div>
      <fieldset${may ? '' : ' disabled'} style="border:0;margin:0;padding:0"><div class="rows">${fieldRows(hgGarageSpecs(zones), g)}</div>${hgFoot(t('Review changes'))}</fieldset></form>`);
  }
  if (d.kind === 6) {
    const s = hgZw.siren[d.node];
    cards.push(`<form class="card gap" id="hg-zw-siren" novalidate><div class="card-head"><div><h2>${esc(tc('Z-Wave siren section', 'Siren'))}</h2></div></div>
      <fieldset${may ? '' : ' disabled'} style="border:0;margin:0;padding:0"><div class="rows">${fieldRows(hgZwSirenSpecs(), s)}
      ${hgRow(t('Test it'), t('Sounds it for 2 seconds'), hgButton(t('Test it'), 'data-hg-zwa="siren_test"'))}</div>${hgFoot(t('Review changes'))}</fieldset></form>`);
  }
  // A battery device: how often it wakes up
  if (!d.listening) cards.push(`<form class="card gap" id="hg-zw-wake" novalidate><div class="card-head"><div><h2>${ht('Checks in')}</h2>
      <p>${ht('How often it reports in. It changes when it next wakes up.')}</p></div></div>
    <fieldset${may ? '' : ' disabled'} style="border:0;margin:0;padding:0"><div class="rows">${fieldRows(hgWakeSpecs(), {seconds: hgZw.wake[d.node] || 3600})}</div>
    ${hgFoot(t('Review changes'))}</fieldset></form>`);
  // Its software
  const fw = hgZw.fw[d.node] || {check: 'none'};
  const fwWords = {up_to_date: t('Up to date.'), stopped: t('Stopped. It keeps its old software.'),
    available: t('Version {0} is available.', fw.version || ''),
    installing: d.listening ? t('Getting new software: {0} %', fw.percent || 0) : t('Getting new software: wake it')}[fw.check] || '';
  cards.push(`<section class="card gap"><div class="card-head"><div><h2>${ht('Software')}</h2><p>${ht('Its own software; sensors are heard again after')}</p></div></div><div class="rows">
    ${hgRow(t('Software'), fwWords, fw.check === 'available' ? hgButton(t('Update'), `data-hg-zwa="fw_install"${off}`, 'small primary')
      : fw.check === 'installing' ? hgButton(t('Stop the update'), `data-hg-zwa="fw_stop"${off}`)
      : hgButton(t('Check for an update'), `data-hg-zwa="fw_check"${off}`))}</div></section>`);
  // Find it, replace it
  cards.push(`<section class="card gap"><div class="card-head"><div><h2>${ht('Find it or replace it')}</h2></div></div><div class="rows">
    ${hgRow(t('Blink its light'), d.listening ? t('Its light blinks 3 times') : t('Tap, then press its button to wake it'), hgButton(t('Blink its light'), `data-hg-zwa="identify"${off}`))}
    ${hgRow(t('Replace it'), t('Its current sensor is replaced'), hgButton(t('Replace it'), `data-hg-zwa="replace"${off}`, 'small danger'))}</div></section>`);
  return {
    html: pageHead(name, t('device {0}', d.node), '', back) + hgLockedNotice() +
      `<section class="card">${kv([[t('Now'), stateWords], [t('Security'), zwWord(ZW_SECURITY, d.security)],
        [t('Battery'), d.battery !== undefined ? t('{0} %', d.battery) : ''], [t('Signal'), d.signal ? zgSignal(d) : ''],
        [t('Zones'), d.zones.length ? tn('Zone {1}', 'Zones {1}', d.zones.length, d.zones.join(', ')) : t('Not in a zone')]])}
        ${stateKind === 'warn' || stateKind === 'bad' ? `<div class="rows"><p class="muted-note">${esc(stateWords)}</p></div>` : ''}</section>` + cards.join(''),
    bind() {
      $$('[data-hg-zwa]').forEach(b => { b.onclick = () => hgZwAction(d, name, b.dataset); });
      const level = $('#hg-zw-level');
      if (level) level.onchange = () => hgZwSimple({action: 'level', ...at, level: Number(level.value)});
      hgZwForm('#hg-zw-wake', hgWakeSpecs(), {seconds: hgZw.wake[d.node] || 3600}, name, v => ({action: 'wakeup', ...at, seconds: v.seconds}),
        () => t('Saved. It changes when it next wakes up.'));
      hgZwForm('#hg-zw-siren', hgZwSirenSpecs(), hgZw.siren[d.node] || {}, name, v => ({action: 'siren', ...at, values: v}));
      if (d.kind === 5) loadAlarm().then(alarm => {
        const zones = alarm.zones.filter(x => x.type).map(x => [x.number, x.name || tc('default zone name', 'Zone {0}', x.number)]);
        hgZwForm('#hg-zw-garage', hgGarageSpecs(zones), hgZw.garage[d.node], name, v => ({action: 'garage', ...at, ...hgZw.garage[d.node], ...v}));
      });
    },
  };
}
const hgWakeSpecs = () => [{name: 'seconds', label: t('Checks in'), type: 'select', numeric: true, options: hgWakeChoices(), help: t('Read again at its next wake-up')}];
const hgGarageSpecs = zones => [
  {name: 'relay', label: t('Garage door opener'), type: 'select', numeric: true, options: [[0, tc('sensor', 'None')], [1, t('Relay {0}', 1)], [2, t('Relay {0}', 2)]]},
  {name: 'zone', label: t('Garage door\'s sensor'), type: 'select', numeric: true, options: [[0, tc('sensor', 'None')], ...zones], showIf: v => v.relay > 0},
];
function hgZwSirenSpecs() {
  const bits = partitionBits();
  return [
    {name: 'burglary', label: tc('siren sounds for', 'Burglary and panic'), type: 'switch', help: t('A silent panic stays silent')},
    {name: 'fire', label: tc('siren sounds for', 'Fire and CO'), type: 'switch', help: t('Its fire tone for fire, its CO tone for CO')},
    {name: 'partitions', label: tc('siren', 'Partitions'), type: 'bits', options: bits, help: t('Their alarms and its chirps')},
    ...(bits.length > 1 ? [{name: 'also', label: t('Also sounds for'), type: 'bits', options: bits, help: t('Other partitions\' alarms, never their chirps')}] : []),
    {name: 'squawk', label: t('Chirp when arming and disarming'), type: 'switch', help: t('Its arm and disarm tones')},
    {name: 'volume', label: t('Volume'), type: 'select', numeric: true, options: [[0, t('Its own')], ...[25, 50, 75, 100].map(v => [v, t('{0} %', v)])], help: t('For its alarms, chirps and tests')},
    {name: 'supervision_min', label: t('Supervision'), type: 'select', numeric: true, options: [1, 4, 12, 24].map(h => [h * 60, t('After {0} h', h)]),
      help: t('A siren trouble when it isn\'t heard')},
  ];
}
function hgZwForm(selector, specs, values, name, body, done = () => t('Saved on the panel.')) {
  const form = $(selector);
  if (!form) return;
  liveFields(form, specs);
  form.onsubmit = event => {
    event.preventDefault();
    try {
      const {values: v, changes} = collect(form, specs, values);
      if (!changes.length) return toast(t('Nothing changed.'));
      review({title: t('Save {0}?', name), changes, done: done({...values, ...v}), run: () => hgZwSend(body({...values, ...v}))});
    } catch (error) {
      toast(error.message, 'error');
    }
  };
}
async function hgZwSimple(body, done = '') {
  try { await hgZwSend(body); if (done) toast(done); } catch (error) { toast(error.message, 'error'); }
  await show();
}
function hgZwAction(d, name, data) {
  const at = {node: d.node, generation: d.generation}, a = data.hgZwa;
  if (a === 'replace') return review({title: t('Replace it with a new device?'), danger: true, confirm: t('Replace it'), done: '',
    intro: t('The new device takes its place: its zones and settings stay. Put the new one in pairing mode once the panel listens.'),
    run: () => hgZwSend({action: 'replace', ...at}), after: () => go('zwave')});
  if (a === 'fw_install') return review({title: t('Update it to {0}?', hgZw.fw[d.node]?.version || ''), confirm: t('Update'), done: '',
    intro: d.listening ? t('Its own software; sensors are heard again after') : t('Tap, then press its button to wake it'),
    run: () => hgZwSend({action: 'fw_install', ...at})});
  if (a === 'fw_stop') return review({title: t('Stop the update?'), intro: t('It keeps its old software.'), confirm: t('Stop the update'), done: '',
    run: () => hgZwSend({action: 'fw_stop', ...at})});
  if (a === 'identify') return hgZwSimple({action: 'identify', ...at}, d.listening ? t('Its light blinks 3 times') : t('Read again at its next wake-up'));
  if (a === 'siren_test') return hgZwSimple({action: 'siren_test', ...at}, t('Sounding for 2 seconds'));
  if (a === 'switch') return hgZwSimple({action: 'switch', ...at, on: data.on === '1'});
  if (a === 'level') return hgZwSimple({action: 'level', ...at, level: Number(data.level)});
  if (a === 'relay') return hgZwSimple({action: 'relay', ...at, relay: Number(data.relay), on: data.on === '1'});
  return hgZwSimple({action: a, ...at});
}
// Z-Wave radio > Network: the region and the reset, as the touchscreen's Z-Wave page has them.
const HG_ZW_BANDS = {1: 'us', 9: 'us', 0: 'eu', 11: 'eu'};
function hgZwNetworkCard(z) {
  const may = isInstaller() && !state.locked;
  const regions = Object.entries(ZW_REGIONS).map(([v, l]) => [Number(v), t(l)]);
  return `<section class="section" id="hg-zw-net"><div class="card"><div class="card-head"><div><h2>${ht('Network')}</h2>
    <p>${ht('Changing the region is done in installer mode.')}</p></div></div><div class="rows">
    ${fieldRow({name: 'hg_region', label: t('Region'), type: 'select', numeric: true, options: regions, locked: !may,
      help: t('The radio\'s frequencies. Devices on another band stop working.')}, z.radio.region)}
    ${hgRow(t('Reset the Z-Wave network'), z.devices.length ? tn('{0} device loses the panel', 'All {0} devices lose the panel', z.devices.length) : t('No devices have joined'),
      hgButton(t('Reset the network'), `id="hg-zw-reset"${hgOff(may)}`, 'small danger'))}</div>
    <div class="card-foot"><span class="note">${ht('Each asks for your password.')}</span>${hgButton(t('Change the region'), `id="hg-zw-region"${hgOff(may)}`, '')}</div></div></section>`;
}
function hgZwNetworkBind(z) {
  const region = $('#hg-zw-region'), reset = $('#hg-zw-reset');
  if (region) region.onclick = () => {
    const to = Number($('[name="hg_region"]').value);
    if (to === z.radio.region) return toast(t('Nothing changed.'));
    const name = t(ZW_REGIONS[to] || 'Other'), n = z.devices.length;
    const same = HG_ZW_BANDS[to] && HG_ZW_BANDS[to] === HG_ZW_BANDS[z.radio.region];
    review({title: t('Change the region to {0}?', name), confirm: t('Confirm the region'), danger: !same && n > 0, password: true, done: '',
      intro: same || !n ? t('Its devices keep working.') : tn('{0} device stops working. It must be added again, and its zones show CHECK until then.',
        '{0} devices stop working. Each must be added again, and their zones show CHECK until then.', n),
      changes: [{label: t('Region'), from: t(ZW_REGIONS[z.radio.region] || 'Other'), to: name}],
      run: me => hgZwSend({action: 'region', region: to, me_password: me})});
  };
  if (reset) reset.onclick = () => review({title: t('Reset the Z-Wave network?'), confirm: t('Reset the network'), danger: true, password: true, done: '',
    intro: z.devices.length ? tn('{0} device loses the panel. It must be reset and added again, and its zones show CHECK until then.',
      'All {0} devices lose the panel. Each one must be reset and added again, and their zones show CHECK until then.', z.devices.length)
      : t('The panel starts a new Z-Wave network. No devices have joined, so nothing else changes.'),
    run: me => hgZwSend({action: 'reset', me_password: me})});
}

// ---- Keypad input: a zone on a keypad's terminal (ZSRC_KEYPAD) -------------------------------------------------------

// The panel's sources, as GET /api/v1/sources gives them once a keypad offers inputs: the pretend panel's five, and kind 6.
const HG_SOURCES = [{kind: 1, capabilities: 3, usable: true, link_down: false, trouble: false}, {kind: 2, capabilities: 3, usable: true, link_down: false, trouble: false},
  {kind: 6, capabilities: 3, usable: true, link_down: false, trouble: false},
  {kind: 3, capabilities: 7, usable: false, link_down: true, trouble: false}, {kind: 4, capabilities: 3, usable: true, link_down: false, trouble: false},
  {kind: 5, capabilities: 7, usable: false, link_down: false, trouble: false}];
// The Hall keypad (keypad 1 of the panel link) reports four terminals (mockup only). IO5 opens and closes every 4 s, as a
// door would while the installer tries it; IO7 hasn't been read yet.
const hgKp = {offline: false, terms: [4, 5, 6, 7]};
function hgKpZone(mock, gpio) {
  const id = String(1 * 256 + gpio);
  for (const [zone, a] of mock.bindings) if (a.kind === 6 && String(a.id) === id) return zone;
  return 0;
}
function hgKpTerm(mock, gpio) {
  const zone = hgKpZone(mock, gpio), known = !hgKp.offline && gpio !== 7;
  const open = !known ? false : zone ? mock.live.open.has(zone) : gpio === 5 && Math.floor(Date.now() / 4000) % 2 === 1;
  return {gpio, known, open, zone};
}
function hgKeypadCands(mock) {
  return hgKp.terms.map(g => {
    const x = hgKpTerm(mock, g);
    return {address: {kind: 6, input: 0, id: String(1 * 256 + g), ref: ''}, name: 'IO' + g, detail: hgKp.offline ? 'Keypad 1 isn\'t answering' : 'Keypad 1',
      state: x.known ? (x.open ? 'open' : 'closed') : '', zone: x.zone, device_class: 0, changed: false, best: false, unusable: false};
  });
}
function hgKeypadInputs(mock) {
  return {number: 1, name: 'Hall keypad', online: !hgKp.offline, terminals: hgKp.terms.map(g => hgKpTerm(mock, g))};
}
// The keypad's line in Partitions and devices: how many inputs, how many feed a zone.
function hgKeypadLines(html, kp) {
  for (const k of kp?.keypads || []) {
    const used = k.terminals.filter(x => x.zone).length;
    const line = `<span>${esc(t('{0} · {1}', tn('{0} input', '{0} inputs', k.terminals.length), tn('{0} feeds a zone', '{0} feed zones', used)))}</span>`;
    html = html.replace(new RegExp(`(<li data-keypad="${k.number}"[^>]*><div class="main"><strong>[^<]*</strong><span>[^<]*</span>)`), `$1${line}`);
    if (!k.online) html = html.replace(new RegExp(`(<li data-keypad="${k.number}"[\\s\\S]*?)<span class="tag good">[^<]*</span>`), `$1${tag(t('Not responding'), 'warn')}`);
  }
  return html;
}
function hgKeypadSection(kp) {
  const zoneName = n => alarmCache?.zones?.[n - 1]?.name || tc('default zone name', 'Zone {0}', n);
  const cards = (kp?.keypads || []).filter(k => k.terminals.length).map(k => `<div class="card gap"><div class="card-head"><div><h2>${esc(k.name)}</h2>
      <p>${esc(t('{0} · {1}', t('Keypad {0}', k.number), tn('{0} input', '{0} inputs', k.terminals.length)))}</p></div></div>
    ${k.online ? '' : `<div class="rows">${notice('warn', t('Keypad {0} not responding', k.number), t('Its inputs aren\'t known until it answers. Their zones show Check.'))}</div>`}
    <ul class="list hg-list">${k.terminals.map(x => `<li>${icon('plug')}<div class="main"><strong>IO${x.gpio}</strong>
      <span>${x.zone ? esc(t('Feeds {0} (zone {1})', zoneName(x.zone), x.zone)) : ht('Free')}</span></div>
      ${!x.known ? tag(t('Not read yet')) : tag(x.open ? tc('zone state', 'Open') : tc('zone state', 'Closed'), x.open ? 'info' : '')}
      ${x.zone ? hgButton(t('Open its zone'), `data-hg-kp-zone="${x.zone}"`) : hgButton(t('Make it a zone'), 'data-hg-kp-new="1"')}</li>`).join('')}</ul></div>`).join('');
  return `<section class="section"><div class="section-head"><div><h2>${ht('Keypad inputs')}</h2>
    <p>${ht('Contacts wired to a keypad\'s terminals. Each can feed a zone: pick Keypad input as the zone\'s sensor.')}</p></div></div>
    ${cards || `<p class="muted-note">${ht('No keypad offers inputs.')}</p>`}</section>`;
}

// ---- Network: Forget this network -------------------------------------------------------------------------------------

function hgForgetCard(headless) {
  const ssid = state.wifi?.ssid;
  if (!ssid) return '';
  return `<section class="section"><div class="card"><div class="card-head"><div><h2>${ht('Forget this network')}</h2>
    <p>${esc(t('The panel leaves {0} and forgets its password.', ssid))}</p></div>
    ${hgButton(t('Forget this network'), `id="hg-forget"${hgOff(!!state.access?.shared)}`, 'danger')}</div>
    <div class="rows"><p class="muted-note">${headless ? ht('This panel has no screen: it then opens its setup Wi-Fi, as when it was new. Join it with the setup label to choose a network again. Its zones, people and keypads stay.')
      : ht('Choose a network again at the panel. Its zones, people and keypads stay.')}</p></div></div></section>`;
}
function hgForgetBind(headless) {
  const b = $('#hg-forget');
  if (!b) return;
  b.onclick = () => review({title: t('Forget this network?'), danger: true, password: true, confirm: t('Forget this network'),
    intro: headless ? t('This browser loses the panel. Join the panel\'s setup Wi-Fi, from its setup label, to choose a network again.')
      : t('This browser loses the panel until it joins a network again, chosen at the panel.'),
    changes: [{label: t('Wi-Fi network'), from: state.wifi.ssid, to: t('None')}],
    done: t('The panel is leaving the network.'), run: me => api('wifi', {action: 'forget', me_password: me})});
}
