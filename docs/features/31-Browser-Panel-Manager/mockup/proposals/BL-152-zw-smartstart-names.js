// BL-152: the Z-Wave page's SmartStart table names each sensor by its model and links a joined one to its device row
// (docs/features/31 mockup/README.md; docs/features/36 screens.md §9, §11; ZW-024).
// Drawn straight in panel-pages.js (zwavePage), the same words as the touchscreen's.
proposal({
  bl: 'BL-152',
  title: 'The SmartStart list shows each sensor\'s model and links a joined one to its device',
  status: 'approved',
  decided: '2026-10-03: the Z-Wave TLC changes were to be drawn, then built without waiting for review',
  role: 'installer',
  why: [
    'A registered sensor without a name was "Sensor 64138", and its product column showed three hex numbers. A joined one said "device 7" as plain words.',
    'The name column falls back to the model the panel knows from its code (the same name the device table uses), then to today\'s words. The product numbers stay in their column for a model the panel doesn\'t know. A joined row\'s "device 7" is a button that scrolls to that device\'s row and marks it for a moment (steady, no blinking); its security column shows the class it was given.',
  ],
  look: [
    ['zwave', 'Register a sensor by a code of a known model: its model is the name; a joined one links to its device row'],
  ],
  replaces: ['zwave'],
  notes: [
    'API: each smartstart entry gains an optional "model" string, sent only when the product is in the panel\'s table.',
  ],
  css: ``,
  apply() {},
});
