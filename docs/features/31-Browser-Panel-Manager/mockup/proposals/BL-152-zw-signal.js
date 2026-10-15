// BL-152 (ZW-040): the Z-Wave devices table gains a Signal column (Good, Fair, Weak; empty until measured),
// as the Zigbee table has (docs/features/36 screens.md §11). Drawn straight in panel-pages.js and the pretend
// panel (mock-panel.cjs sends `signal` for its devices); this file records the proposal and where to look.
proposal({
  bl: 'BL-152',
  title: 'The Z-Wave devices table says how well each device is heard',
  status: 'approved',
  decided: '2026-10-03: the Z-Wave TLC changes were to be drawn, then built without waiting for review',
  role: 'installer',
  why: [
    'The table has Last heard but not how strong the signal was, so an installer can\'t tell a device that is barely heard from one that is fine.',
    'A Signal column after Last heard, in the touchscreen\'s words: Good, Fair, Weak; empty until a frame was measured.',
  ],
  look: [['zwave', 'The devices table: Signal beside Last heard']],
  replaces: ['zwave'],
  notes: ['The API gains `signal` per device ("good", "fair", "weak"; left out when not measured). The pretend panel sends it for its devices.'],
  css: ``,
  apply() {},
});
