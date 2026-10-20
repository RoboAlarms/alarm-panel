// BL-220: Smart Home rooms made on the panel (docs/features/24 HCTL-003, HCTL-008, HCTL-013;
// ui_devices.c). Asked for by the owner on 2026-10-19, after trying BL-219 on both panels with a bench
// stand-in for Home Assistant whose devices all sat in one area: "a way to add sections beside
// Favorites". Today the tabs beside Favorites are only the device source's areas (Home Assistant's),
// and a device without one goes to "Other"; a home without Home Assistant (devices added straight to
// the panel: Z-Wave today, Zigbee later, BL-169, BL-149) has no way to group its devices at all.
//
// Proposed:
//   R1  Rooms belong to the panel. Every tab after Favorites is a panel room; a source's area only
//       seeds one. A picked tile in Arrange gets Room: a room, New room (the keyboard), No room, or
//       (for a device whose source has an area) the room from its source.
//   R2  Arrange gets Rooms: a sheet of the rooms, where a room is renamed, moved earlier or later,
//       or removed (its devices go to Other, nothing is deleted), and a room is added empty. On a
//       room's page Arrange's Add puts devices in that room, as Favorites' Add does.
//   R3  A device from a source with areas starts in the panel room of that area's name (made when
//       missing) and follows the source while nobody has moved it on the panel. Once someone picks
//       a room for it on the panel, the panel's choice wins; "Room from its source" goes back.
//       A room made from a source's area keeps its link when renamed; it goes away by itself only
//       when it is empty and nobody renamed it.
//   R4  Rooms are the house's, not a screen's: one list, kept by the alarm panel and shown by every
//       link keypad, which can change it too (sent to the alarm panel, judged there). Each keypad's
//       Arrange - the order, sizes and Favorites - stays its own (HCTL-013 decision 2).
//   R5  Who may change: the household chooses (revised by the owner, 2026-10-19: "I might not want
//       the kids changing my thermostat ... someone else might not want to require a master code").
//       A new Settings > Smart Home page (a master's or the installer's code, as Integrations) has:
//       "Arranging asks for a master code" (Arrange: order, sizes, Favorites and rooms; off from the
//       factory, as Arrange is today, HCTL-008), and a switch per device, "asks for a master code"
//       (off from the factory). A device switched on shows a key on its tile, and any change to it -
//       a tap, its control, its sheet - first opens the code pad; after a master's code its changes
//       go through until the screen has been left alone for 90 s (HOME_AFTER_MS) or leaves Smart
//       Home. A lock, garage door, cover or valve keeps HCTL-005 (a user code to unlock or open);
//       switched on, every change to it asks for a master's code instead. Judged on the alarm panel
//       for a keypad too (LINK-048), so the setting is the house's.
//   R6  The tab for devices without a room is "Other" while there are rooms, and "All devices"
//       while there are none, so a house without rooms doesn't show a lonely "Other".
//   R7  No "Smart Home" title and no note over the page (the owner, 2026-10-19: "just using up the
//       screen"): the tabs start the row, the pencil ends it. The row stays - the tabs and the pencil
//       need it - so the tiles get no more height; the tabs get the whole width, about 400 px more,
//       so far more rooms show before they scroll. The note's summary (HCTL-006: jammed, unlocked,
//       offline, failed, lights on) goes; each tile keeps its own words, and Arrange keeps its title.
//   R8  Start screen (the owner, 2026-10-19): Settings > Screen and sound gets "Start screen",
//       Security or Smart Home, this screen's own (a keypad chooses its own), anyone, as the rest of
//       the page. With Smart Home the panel starts there, goes back there after 90 s left alone, and
//       wakes there from the screen saver. Security still takes over by itself where it does today -
//       entry time, an alarm, fire or CO, a walk test, a code entry started on Security - and the
//       panel stays there until it is calm again. Shown only while Smart Home is in the rail; if
//       the rail loses it (no devices), the panel rests on Security.
//
// Decisions (recommended first; approved 2026-10-19). D1, D2, D4 and D5 taken as recommended;
// D3 (no code) replaced by the owner's R5 the same day; D6 to D8 taken as recommended the same day:
//   D1  Rooms are the panel's, a source's area only seeding one (R1, R3).
//   D2  Keypads share the alarm panel's rooms and can change them (R4).
//   D4  "All devices" while there are no rooms (R6).
//   D5  Add on a room's page moves devices into it (bl-220-room-add).
//   D6  Whose code is "a master's": the system master's, a partition master's for this panel's
//       partition, or the installer's - or the system master's and the installer's only.
//   D7  After a master's code, a protected device works until 90 s left alone - or each change asks.
//   D8  With Smart Home as the start screen, while armed the panel rests on Security (the state and
//       Disarm in view) - or on Smart Home whatever the state.
//
// Storage (for the build, not the screen; components/core/AGENTS.md): because rooms are shared with
// the keypads and a keypad's layout is its own (R4), the rooms are NOT in the layout blob
// (ui_dev_model.h, unchanged) but a stored format of their own on the alarm panel: a compat byte,
// then the rooms in tab order - each a stable id (1-based, never reused while stored, 0 = no room),
// its name (at most 47 bytes, DEV_AREA_LEN 48), the source area it was made from (empty for a room a
// person made) and whether a person renamed it - then each device a person placed: its ref, its room
// id (0 = no room). A device not listed follows its source. Up to 24 rooms (the tabs scroll). A
// frozen v1 fixture as for the layout. Over the link a keypad already receives each device's area
// (plink_home.c): the alarm panel sends the device's panel room there, plus the rooms' order (a new
// field, appended), and a keypad's room change is a new request (LINK protocol, docs/features/11).
//
// Not here, for later rows: Settings > Devices (BL-045, hide and rename) can show each device's room
// when built; the automations' device picker, which lists devices by area (automations.js), should
// list them by these rooms; the Panel Manager has no Smart Home page yet.
//
// Each mockup file has its own scope, so the Smart Home pieces of views.js are repeated here.

const DEVICE_PORCH = {glyph: 'lightbulb', tone: 'blue', name: 'Porch Light', state: 'On · 80%', wide: true, control: M.device_level({value: 80})};
const DEVICE_FAVORITES = [
  DEVICE_PORCH,
  {glyph: 'lock', tone: 'green', name: 'Front Door', state: 'Locked'},
  {glyph: 'lock-open', tone: 'amber', name: 'Back Door', state: 'Unlocked'},
  {glyph: 'warehouse', tone: 'green', name: 'Garage Door', state: 'Closed'},
  {glyph: 'thermometer', tone: 'orange', name: 'Hallway', state: 'Heating · 71° now', wide: true, control: M.device_stepper({label: 'Heat to', value: '72°'})},
  {glyph: 'sparkles', tone: 'violet', name: 'Good Night', state: 'Ran at 8:30 PM', more: false},
  {glyph: 'lightbulb', tone: 'blue', name: 'Living Room Lamp', state: 'On · 40%', wide: true, control: M.device_level({value: 40})},
  {glyph: 'lightbulb', name: 'Kitchen Lights', state: 'Off'},
];
// A house without Home Assistant: its devices added to the panel itself, none in a room yet.
const R_ALL = [
  DEVICE_PORCH,
  {glyph: 'lock', tone: 'green', name: 'Front Door', state: 'Locked'},
  {glyph: 'warehouse', tone: 'green', name: 'Garage Door', state: 'Closed'},
  {glyph: 'lightbulb', name: 'Kitchen Lights', state: 'Off'},
  {glyph: 'lightbulb', tone: 'blue', name: 'Living Room Lamp', state: 'On · 40%', wide: true, control: M.device_level({value: 40})},
  {glyph: 'plug', name: 'Pool Pump', state: 'Off'},
  {glyph: 'droplet', name: 'Garden Water', state: 'Closed'},
  {glyph: 'lightbulb', name: 'Patio Lights', state: 'Off'},
];
const R_ROOMS = ['Favorites', 'Living room', 'Kitchen', 'Outside'];
const R_BAR = {time: '10:48', troubles: 1};
const rMain = (o) => M.main({view: 'devices', tone: 'amber', rail: {zones: 1, troubles: 1}, bar: R_BAR, ...o});

// The tabs; scrolled: more than fit, scrolled so the open one is in view (HCTL-006), the first ones
// cut off at the left as LVGL's scrolled row shows them. mark: the tabs a proposal changes.
const rTabs = (items, active = 0, {scrolled = false, mark = false} = {}) => {
  const seg = M.segments(items, active);
  if (scrolled) {
    return `<div class="${mark ? 'changed' : ''}" style="flex:1 1 0;min-width:0;overflow:hidden;display:flex;justify-content:flex-end">${seg}</div>`;
  }
  return mark ? M.changed(seg) : seg;
};
const rMark = (html) => html.replace('class="device_tile', 'class="changed device_tile');
// R7: no "Smart Home" title and no note over the page: the tabs start the row, the pencil ends it.
const rView = ({tabs, tiles, marked = [], empty, markRow = false}) => {
  const row = `<div class="row" style="gap:var(--space_md)">${tabs}<div class="grow"></div>
    <div class="round_button">${M.icon('pencil', 20)}</div></div>`;
  return `<div class="view column">${markRow ? M.changed(row) : row}
  ${empty || `<div class="device_grid scroll grow">${tiles.map((t) => (marked.includes(t.name) ? rMark(M.device_tile(t)) : M.device_tile(t))).join('')}</div>`}</div>`;
};

// Arrange (views.js arrangeHead), with Room on a picked tile and Rooms on the page.
const R_ROOM_BTN = M.changed(M.button('Room', '', 'door-open'));
const R_ROOMS_BTN = M.changed(M.button('Rooms', '', 'house'));
const rArrangeHead = (pick, page) => (pick
  ? `${M.view_header({title: pick.name, note: 'Hold to drag, or use arrows'})}
    <div class="round_button">${M.icon('arrow-left', 20)}</div><div class="round_button">${M.icon('arrow-right', 20)}</div>
    ${M.segments(['Small', 'Wide'], pick.wide ? 1 : 0)}${R_ROOM_BTN}${page === 'Favorites' ? M.button('Remove', '', 'star-off') : ''}${M.button('Done', 'primary')}`
  : `${M.view_header({title: `Arrange ${page}`, note: 'Hold a tile to drag it, or tap it to change it'})}
    ${R_ROOMS_BTN}${page === 'Other' || page === 'All devices' ? '' : (page === 'Favorites' ? M.button('Add', '', 'plus') : M.changed(M.button('Add', '', 'plus')))}${M.button('Done', 'primary')}`);
const rArrange = (pick, tiles, page = 'Favorites') => `<div class="view column">
  <div class="row" style="gap:var(--space_md)">${rArrangeHead(pick, page)}</div>
  <div class="device_grid scroll grow">${tiles.map((t) => M.device_tile({...t, arrange: true, selected: pick === t})).join('')}</div></div>`;

const R_DECIDED = '2026-10-19: the owner took D1, D2, D4 and D5 as recommended, replaced D3 with a master code by setting (R5), asked for no title (R7) and a start screen (R8), took D6-D8 as recommended, and chose layout L1 (tabs only, no summary)';
const R_WHY ='A home without Home Assistant has no way to group its devices, and a home with it can\'t group them differently on the panel: the tabs beside Favorites become rooms the panel keeps, made and changed on the screen.';
const r = (id, title, replaces, notes, main) => M.screen({id, area: 'Smart Home', title, shot: null,
  proposal: {bl: 'BL-220', status: 'approved', replaces, why: R_WHY, decided: R_DECIDED}, notes, render: () => rMain(main())});

// ---- R6: no rooms yet ----
r('bl-220-rooms-none', 'Smart Home rooms: none yet', 'devices',
  'A house whose devices were added to the panel itself (Z-Wave today, Zigbee later): no source gives them a room. Today the second tab reads "Other", which says nothing when there is nothing else; proposed (R6): "All devices" while there is no room, "Other" once there is one. Rooms start from the pencil: Arrange, then a tile\'s Room or the page\'s Rooms.',
  () => ({content: rView({note: '2 lights on', tabs: rTabs(['Favorites', 'All devices'], 1, {mark: true}), tiles: R_ALL})}));

r('bl-220-arrange-rooms-button', 'Arranging: Rooms beside Add', 'devices-arrange',
  'Arrange gets Rooms (R2), on every page: it opens the rooms\' sheet (bl-220-rooms-sheet). The note now names a tile\'s room among what a tap offers. Add stays Favorites\' own here; on a room\'s page it adds to that room (bl-220-room-add). Without a code unless Settings > Smart Home asks for a master\'s (R5, bl-220-arrange-code).',
  () => ({content: rArrange(null, DEVICE_FAVORITES)}));

r('bl-220-arrange-pick', 'Arranging: a picked tile has Room', 'devices-arrange-tile',
  'A picked tile\'s actions gain Room, between its size and Remove (R1), on Favorites and on every room\'s page (where Remove isn\'t shown, as today). Room opens the room picker (bl-220-room-pick-none, bl-220-room-pick). The name keeps what is left of the row and ends in "..." when long, as today.',
  () => ({content: rArrange(DEVICE_PORCH, DEVICE_FAVORITES)}));

// ---- R1: a tile's room ----
const rPick = ({title = 'Room for Porch Light', note = 'Where it shows besides Favorites', rows}) => M.pick_sheet({title, note, fit: true, rows: rows.join('')});
const R_NEW = M.row_link({glyph: 'plus', color: 'blue', title: 'New room', detail: 'Name it, and Porch Light goes in it', trail: ''});
const rNone = (on) => M.row_link({glyph: 'circle-slash', color: 'gray', title: 'No room', detail: 'Shown in Other', trail: on ? 'check' : '', trailColor: 'blue'});
const rRoom = (name, n, on) => M.row_link({glyph: 'door-open', color: on ? 'blue' : 'gray', title: name, detail: `${n} ${n === 1 ? 'device' : 'devices'}`, trail: on ? 'check' : '', trailColor: 'blue'});

r('bl-220-room-pick-none', 'A tile\'s Room, no rooms yet', 'devices-arrange-tile',
  'The room picker in a house with no room: New room and No room, the device\'s current choice ticked. A tap on a row applies it and closes the sheet; the tile stays picked. "Shown in Other": the words of R6 once a room exists.',
  () => ({content: rArrange(DEVICE_PORCH, DEVICE_FAVORITES), over: M.changed(rPick({rows: [R_NEW, rNone(true)]}))}));

r('bl-220-room-new', 'New room: naming it', 'devices-arrange-tile',
  'The panel\'s keyboard (text_sheet), as for a zone\'s name. Up to 47 bytes, as a device\'s name; the keyboard stops taking letters at the limit. Add stays dimmed while the name is empty or only spaces, and while another room has the same name (case aside), when the note says "There is already a room called Outside" in amber instead. Add makes the room, puts Porch Light in it and returns to Arrange with the tile still picked; the new tab is last.',
  () => ({content: rArrange(DEVICE_PORCH, DEVICE_FAVORITES),
    over: M.changed(M.text_sheet({title: 'New room', note: 'Porch Light goes in it', value: 'Outside', ok: 'Add'}))}));

r('bl-220-room-made', 'Smart Home with its first room', 'devices',
  'After Done: Favorites, the new room and Other (the devices still without one; R6). Nothing else changes: Favorites keeps its tiles and order. A room\'s page shows its devices in the page\'s own order, arranged as any page.',
  () => ({content: rView({note: '2 lights on', tabs: rTabs(['Favorites', 'Outside', 'Other'], 1, {mark: true}),
    tiles: [DEVICE_PORCH, R_ALL[5], R_ALL[6], R_ALL[7]]})}));

// R3: a device from a source with areas
const R_SOURCE = (on) => M.row_link({glyph: 'house-wifi', color: on ? 'blue' : 'gray', title: 'Room from its source', detail: 'Outside now · Moves when its source moves it', trail: on ? 'check' : '', trailColor: 'blue'});
r('bl-220-room-pick', 'A tile\'s Room, with rooms', 'devices-arrange-tile',
  'Where there are rooms: each in the tabs\' order with how many devices it has, then New room and No room. For a device whose source has an area (Home Assistant\'s), the first row is the room from its source (R3), ticked while the device follows it; here someone moved Porch Light to Living room on the panel, which wins from then on, and the first row takes it back. The words say "its source", not the integration\'s name: general screens never name one (UI-018). Many rooms: the list scrolls, the card at most 520 high.',
  () => ({content: rArrange(DEVICE_PORCH, DEVICE_FAVORITES), over: M.changed(rPick({rows: [
    R_SOURCE(false), rRoom('Living room', 3, true), rRoom('Kitchen', 2), rRoom('Outside', 5), R_NEW, rNone(false)]}))}));

// ---- R2: the rooms ----
const R_ROOM_ROWS = [['Living room', 3], ['Kitchen', 2], ['Outside', 5]].map(([name, n]) =>
  M.row_link({glyph: 'door-open', color: 'blue', title: name, detail: `${n} devices`}));
r('bl-220-rooms-sheet', 'Arranging: the rooms', 'devices-arrange',
  'Rooms, from Arrange: the rooms in the tabs\' order, a tap opens one (bl-220-room-sheet); New room makes an empty one (the keyboard, then its own page: bl-220-room-empty). Favorites and Other aren\'t rooms, so they aren\'t listed. With no rooms the sheet has New room alone and says "No rooms yet".',
  () => ({content: rArrange(null, DEVICE_FAVORITES), over: M.changed(M.pick_sheet({title: 'Rooms', note: 'The tabs beside Favorites, in their order', fit: true,
    rows: [...R_ROOM_ROWS, M.row_link({glyph: 'plus', color: 'blue', title: 'New room', detail: 'An empty room, to add devices to', trail: ''})].join('')}))}));

r('bl-220-room-sheet', 'One room: rename, move, remove', 'devices-arrange',
  'A room\'s sheet. Rename opens the keyboard with its name (bl-220-room-rename). Earlier and Later move its tab one place and keep the sheet open, the note saying where it is now; the one that can\'t is dimmed. Remove asks first (bl-220-room-remove). X goes back to the rooms. Everything here applies at once, as the rest of Arrange does on Done.',
  () => ({content: rArrange(null, DEVICE_FAVORITES), over: M.changed(M.pick_sheet({title: 'Kitchen', note: '2 devices · Second of 3 rooms', fit: true, rows: [
    M.row_link({glyph: 'pencil', color: 'blue', title: 'Rename', trail: ''}),
    M.row_link({glyph: 'arrow-left', color: 'blue', title: 'Earlier', detail: 'One tab to the left', trail: ''}),
    M.row_link({glyph: 'arrow-right', color: 'blue', title: 'Later', detail: 'One tab to the right', trail: ''}),
    M.row_link({glyph: 'trash-2', color: 'red', title: 'Remove room', detail: 'Its devices go to Other', trail: ''})].join('')}))}));

r('bl-220-room-rename', 'Renaming a room', 'devices-arrange',
  'The keyboard with the room\'s name. The same rules as a new room: 47 bytes, not empty, not another room\'s name. A room made from a source\'s area keeps its link when renamed (R3): its devices stay in it, and the source can\'t bring the old name back as a second room.',
  () => ({content: rArrange(null, DEVICE_FAVORITES), over: M.changed(M.text_sheet({title: 'Rename Kitchen', value: 'Kitchen and dining', ok: 'Done'}))}));

r('bl-220-room-remove', 'Removing a room with devices', 'devices-arrange',
  'Red, as removing is (confirm_dialog). Its devices stay on the panel and in Favorites; they show in Other. A device that followed its source\'s area now has no room on purpose: the source doesn\'t put it back. A device new to the panel later, in that area, brings a room of that name back - a new device, so nobody\'s choice is undone.',
  () => ({content: rArrange(null, DEVICE_FAVORITES), over: M.changed(M.confirm_dialog({title: 'Remove Kitchen?',
    note: 'Its 2 devices stay on the panel and show in Other. Favorites doesn\'t change.', ok: 'Remove'}))}));

r('bl-220-room-empty', 'An empty room', 'devices',
  'A room made from Rooms, or one whose devices all moved: its page says so and how to fill it. It stays until someone removes it, except a room made from a source\'s area that nobody renamed, which goes when its last device leaves (R3).',
  () => ({content: rView({note: 'No devices in Garage', tabs: rTabs(['Favorites', 'Living room', 'Kitchen', 'Outside', 'Garage'], 4), empty: M.changed(`<div class="col center grow" style="gap:var(--space_md)">${M.icon_chip({glyph: 'door-open', size: 112, font: 40, color: 'gray'})}
      ${M.text('No devices in Garage', 'title')}${M.text('Tap the pencil, then Add, to put devices here', 'body', 'text_2')}</div>`)})}));

r('bl-220-room-add', 'Arranging a room: Add puts devices in it', 'devices-arrange-add',
  'On a room\'s page Add lists the devices in other rooms or none, each with where it is now; a tap moves one into this room, at the end, and the list stays open for the next (as Favorites\' Add). The quick way to fill a new room in a house without Home Assistant. Its title says where they go.',
  () => ({content: rArrange(null, [], 'Garage'), over: M.changed(M.pick_sheet({title: 'Add to Garage', note: 'Tap a device to move it here', fit: true,
    rows: [['warehouse', 'Garage Door', 'Outside · Closed'], ['plug', 'Pool Pump', 'Outside · Off'], ['lightbulb', 'Kitchen Lights', 'Kitchen · Off'], ['droplet', 'Garden Water', 'Other · Closed']]
      .map(([glyph, title, detail]) => M.row_link({glyph, color: 'gray', title, detail, trail: 'plus', trailColor: 'blue'})).join('')}))}));

// ---- many rooms, a long name ----
const R_MANY = ['Favorites', 'Living room', 'Kitchen', 'Dining', 'Hall', 'Office', 'Bedroom', 'Kids', 'Garage', 'Outside'];
r('bl-220-rooms-many', 'Many rooms: the tabs scroll', 'devices-outside',
  'Nine rooms: the tabs scroll sideways (HCTL-006 already lets them), the open one kept in view. Up to 24 rooms. Nothing moves by itself; a swipe on the tabs scrolls them.',
  () => ({content: rView({note: 'Garage Door closed', tabs: rTabs(R_MANY, 8, {scrolled: true, mark: true}),
    tiles: [{glyph: 'warehouse', tone: 'green', name: 'Garage Door', state: 'Closed'}, {glyph: 'lightbulb', name: 'Garage Lights', state: 'Off'}, {glyph: 'plug', name: 'Freezer', state: 'On', tone: 'blue'}]})}));

const R_LONG = 'Downstairs guest bedroom by the laundry';
r('bl-220-room-long', 'A long room name', 'devices',
  'A room\'s name may be 47 bytes. Its tab ends in "..." where the open tab must give way (HCTL-006: never narrower than Favorites), as a long Home Assistant area does today. A hold on a shortened tab does nothing new; Rename shows the whole name (bl-220-room-long-sheet).',
  () => ({content: rView({note: '1 light on', tabs: rTabs(['Favorites', 'Living room', 'Downstairs guest bed…'], 2, {mark: true}),
    tiles: [{glyph: 'lightbulb', tone: 'blue', name: 'Bedside Lamp', state: 'On · 30%', wide: true, control: M.device_level({value: 30})}, {glyph: 'blinds', name: 'Guest Blinds', state: 'Closed'}]}),
    over: ''}));
r('bl-220-room-long-sheet', 'A long room name in its sheet', 'devices-arrange',
  'The same room\'s sheet: its title ends in "..." as every sheet\'s does; Rename shows the whole name in the keyboard\'s field. The Rooms list and the picker show it in a row, also ending in "..." past their width. Later is dimmed: it is the last room.',
  () => ({content: rArrange(null, DEVICE_FAVORITES), over: M.changed(M.pick_sheet({title: R_LONG, note: '2 devices · Second of 2 rooms', fit: true, rows: [
    M.row_link({glyph: 'pencil', color: 'blue', title: 'Rename', trail: ''}),
    M.row_link({glyph: 'arrow-left', color: 'blue', title: 'Earlier', detail: 'One tab to the left', trail: ''}),
    M.row_link({glyph: 'arrow-right', color: 'blue', title: 'Later', detail: 'One tab to the right', trail: '', disabled: true}),
    M.row_link({glyph: 'trash-2', color: 'red', title: 'Remove room', detail: 'Its devices go to Other', trail: ''})].join('')}))}));

// ---- R4: a link keypad ----
const rKp = (main) => {
  const m = main();
  return M.main({view: 'devices', tone: 'amber', rail: {devices: true}, bar: {time: '10:42', banner: 'codes', bannerText: 'Factory codes · Change them on the alarm panel'}, ...m})
    .replace('data-go="devices"', 'data-go="kp-devices"').replace('data-go="home-ready"', 'data-go="kp-home-ready"');
};
M.screen({id: 'bl-220-kp-room-pick', area: 'Keypad', title: 'Keypad: a tile\'s Room', shot: null,
  proposal: {bl: 'BL-220', status: 'approved', replaces: 'kp-devices', why: R_WHY, decided: R_DECIDED},
  notes: 'A link keypad shows Harbor House\'s rooms and can change them (R4): the same picker, the choice sent to Harbor House, which keeps the rooms for the house; every screen shows it within a second (LINK-045). The keypad\'s order, sizes and Favorites stay its own (HCTL-013 decision 2). While the link is lost, Room and Rooms are dimmed and the note under the picked tile\'s name says "Rooms change while Harbor House answers". The other way (decision D2): rooms set on the alarm panel only, the keypad showing them.',
  render: () => rKp(() => ({content: rArrange(DEVICE_PORCH, DEVICE_FAVORITES), over: M.changed(rPick({rows: [
    rRoom('Living room', 3, false), rRoom('Kitchen', 2), rRoom('Outside', 5, true), R_NEW, rNone(false)]}))}))});

// ---- R7: no title or note over the page ----
const R_HALLWAY = {...DEVICE_FAVORITES[4], locked: true};
const R_FAV_LOCKED = DEVICE_FAVORITES.map((t) => (t.name === 'Hallway' ? R_HALLWAY : t));
r('bl-220-no-header', 'Smart Home without a title', 'devices',
  'R7: the "Smart Home" title and its note are gone; the tabs start the row and the pencil ends it. The rail already says where you are. The row keeps its height (the tabs and the pencil need it), so the tiles don\'t move up; the tabs gain the width the title took, about 400 px, room for four or five more before they scroll. Gone with it: the note\'s summary of the page (HCTL-006, "Back Door unlocked · 2 lights on"); each tile still says its own state, a failure in amber, an offline time. Hallway shows the key of a device that asks for a master code (R5).',
  () => ({content: rView({tabs: rTabs(R_ROOMS, 0), tiles: R_FAV_LOCKED, markRow: true, marked: ['Hallway']})}));

// ---- R5: a master code, by setting ----
r('bl-220-device-code', 'A protected thermostat asks for a master code', 'device-unlock',
  'Hallway is switched on in Settings > Smart Home (R5). A tap on its minus or plus, on its tile or in its sheet, opens the code pad first; the step is made once a master\'s code is right. After that, Hallway and any other protected device take changes without asking until the screen is left alone for 90 s or leaves Smart Home (D7). A wrong code counts toward the lockout, as every code does. Other codes are refused with "Only a master code can change Hallway". Nothing about it on a tile but the key: no locked look, it still reads as usual.',
  () => ({content: rView({tabs: rTabs(R_ROOMS, 0), tiles: R_FAV_LOCKED}),
    over: M.changed(M.modal_pad({title: 'Change Hallway', tone: 'orange', glyph: 'thermometer', note: 'Hallway asks for a master code'}))}));

r('bl-220-arrange-code', 'Arranging asks for a master code', 'devices-arrange',
  'Only while "Arranging asks for a master code" is on (R5; off from the factory, when the pencil opens Arrange at once as today). The pencil opens this pad; the right code opens Arrange, which stays open until Done or 90 s left alone. Rooms, Room and Add are all inside Arrange, so the one code covers them.',
  () => ({content: rView({tabs: rTabs(R_ROOMS, 0), tiles: R_FAV_LOCKED}),
    over: M.changed(M.modal_pad({title: 'Arrange Smart Home', tone: 'blue', glyph: 'house-wifi', note: 'Arranging asks for a master code'}))}));

const R_SECTIONS = M.settings_sections({add: [{after: 'integrations', key: 'smarthome', glyph: 'house-wifi', title: 'Smart Home', goTo: 'bl-220-settings-smart-home'}]});
const rSet = (page) => ({view: 'settings', content: M.settings({sections: R_SECTIONS, ...page})});
r('bl-220-settings-smart-home', 'Settings > Smart Home', null,
  'A new Settings page, after Integrations, only on a build with Smart Home and on the alarm panel (a keypad follows it: its devices are judged there). Opened with a master\'s or the installer\'s code, as Integrations. Both kinds of switch are off from the factory, so nothing asks for a code until the household chooses (R5). Every device is listed, by room; a lock\'s detail says it asks for a code to unlock anyway (HCTL-005). Saved as each switch moves.',
  () => rSet({section: 'smarthome', title: 'Smart Home', note: 'What asks for a master code', body: M.changed([
    M.row_toggle({glyph: 'pencil', title: 'Arranging asks for a master code', detail: 'Moving tiles, their sizes, Favorites and rooms', on: false}),
    M.section('Devices that ask for a master code before a change'),
    M.row_toggle({glyph: 'thermometer', color: 'orange', title: 'Hallway', detail: 'Living room', on: true}),
    M.row_toggle({glyph: 'lightbulb', title: 'Living Room Lamp', detail: 'Living room', on: false}),
    M.row_toggle({glyph: 'lightbulb', title: 'Kitchen Lights', detail: 'Kitchen', on: false}),
    M.row_toggle({glyph: 'lock', color: 'green', title: 'Front Door', detail: 'Outside · Unlocking asks for a code anyway', on: false}),
    M.row_toggle({glyph: 'warehouse', color: 'green', title: 'Garage Door', detail: 'Outside · Opening asks for a code anyway', on: false}),
    M.row_toggle({glyph: 'lightbulb', title: 'Porch Light', detail: 'Outside', on: false}),
  ].join(''))}));

// ---- R8: the start screen ----
const rDisplay = (start, over) => ({...rSet({section: 'display', title: 'Screen and sound', note: '', body: [
  M.section('Screen'),
  M.changed(M.row_link({glyph: 'house-wifi', title: 'Start screen', detail: 'Where this screen rests; an alarm always shows Security', value: start})),
  M.row_level({glyph: 'sun', title: 'Brightness', value: 80}),
  M.row_link({glyph: 'image', title: 'Screen saver', detail: 'A dimmed photo with the time and the alarm state', value: 'After 1 minute'}),
  M.row_link({glyph: 'monitor-off', title: 'Screen off', detail: 'Dark until a touch wakes it', value: 'Never'}),
].join('')}), over});
r('bl-220-start-screen', 'Screen and sound: Start screen', 'settings-display',
  'A row at the top of Screen (under Sound, not drawn here), anyone may change it, as brightness. Shown only while Smart Home is in the rail. It is this screen\'s own: each keypad chooses its own (Screen and sound is per screen).',
  () => rDisplay('Smart Home'));
r('bl-220-start-screen-pick', 'Choosing the start screen', 'settings-display',
  'Security is the factory\'s. With Smart Home, the panel shows Smart Home after a start, after 90 s left alone and when it wakes from the screen saver. Security takes over by itself, as today, for entry time, an alarm, fire or CO, and a walk test, and stays until the panel is calm and left alone 90 s. While armed it rests on Security, so the state and Disarm are in view (D8); disarmed, back to Smart Home.',
  () => rDisplay('Smart Home', M.changed(M.pick_sheet({title: 'Start screen', note: 'An alarm or entry time always shows Security', fit: true, rows: [
    M.row_link({glyph: 'shield', color: 'gray', title: 'Security', detail: 'The alarm state and arming', trail: ''}),
    M.row_link({glyph: 'house-wifi', color: 'blue', title: 'Smart Home', detail: 'Your devices; Security while armed', trail: 'check', trailColor: 'blue'})].join('')}))));

// ---- R7 again: the page's layout, compared (the owner, 2026-10-19: "research the layout ... how
// other home automation systems do its layout"). What the others do, as of 2025-2026:
//   Apple Home: rooms on the main page, one after another as you scroll, under a row of category
//     buttons (Climate, Lights, Security, Water, Speakers) that each summarise ("3 on", "68-72°",
//     "2 unlocked"); a tap on one shows that kind in every room.
//   Home Assistant: the new Home dashboard has Summaries (Lights, Climate, Security, Media), then
//     Favorites, then Areas; its Areas dashboard has a tab per area with an overview first.
//   Google Home: Favorites first; on a tablet in landscape, Devices is one long page with a bar of
//     rooms beside it that jumps to each room's group.
//   Wall panels (Brilliant, Aqara Panel Hub): the most relevant devices first, the rest a tap away;
//     room views switch rather than scroll.
// So the panel's tabs are close to what the others do. Compared on 2026-10-19: L1 tabs only, L2 tabs
// with a summary by kind (Lights, Climate, Locks, Covers) over Favorites, L3 one long page with room
// headings. The owner chose L1 (bl-220-no-header): the most tiles, no summary.
