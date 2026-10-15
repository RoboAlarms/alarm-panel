// BL-152: the SmartStart list names each sensor by its model, and a joined sensor opens its device page
// (docs/features/36 screens.md §9; ZW-024). Drawn with what changes wrapped in M.changed.
M.screen({
  id: 'bl-152-zw-smartstart-names', area: 'Installer', title: 'Z-Wave Waiting to join: model names', shot: null,
  proposal: {bl: 'BL-152', status: 'approved', replaces: 'zwave-region',
    decided: '2026-10-03: the Z-Wave TLC changes were to be drawn, then built without waiting for review',
    why: 'Each row said "Sensor 64138" (what it is and five digits of its key) though the code carries its maker and model. A joined row said "In the network: device 7" as plain words, and its page still offered the security it "may have".'},
  notes: 'ZW-024. A registered sensor with a name keeps it as the row\'s title and the model leads the detail line. With no name the model is the title; a model the panel doesn\'t know keeps "Sensor 64138". A joined row opens that device\'s page (the entry page of a joined sensor says "Security it got" and lists only the classes it was given).',
  render: () => M.main({view: 'settings', tone: 'blue', rail: {troubles: 1, tone: 'blue'}, bar: {banner: 'installer', time: '10:52', troubles: 1},
    content: M.settings({section: 'installer', title: 'Waiting to join', note: 'Sensors registered by their QR code', back: true, body: [
      M.changed(M.row_link({glyph: 'plug', color: 'blue', title: 'Porch', detail: 'Zooz ZSE41 open/close sensor · Long Range · Power it up', value: 'Waiting'})),
      M.changed(M.row_link({glyph: 'plug', color: 'blue', title: 'Zooz ZSE42 water leak sensor', detail: 'Power it up: it joins by itself', value: 'Waiting'})),
      M.row_link({glyph: 'plug', color: 'blue', title: 'Sensor 64138', detail: 'Power it up: it joins by itself', value: 'Waiting'}),
      M.changed(M.row_link({glyph: 'plug', color: 'green', title: 'Zooz ZSE41 open/close sensor', detail: 'In the network: device 7 · Open its page', value: 'Joined'})),
      M.row_link({glyph: 'key-round', color: 'blue', title: 'Register a sensor', detail: 'Type its 40-digit key, or scan it with a phone'}),
    ].join('')})}),
});
