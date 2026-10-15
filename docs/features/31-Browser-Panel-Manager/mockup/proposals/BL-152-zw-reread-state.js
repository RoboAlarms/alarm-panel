// BL-152: a finished Z-Wave device read again says so in the Panel Manager, in the touchscreen's words
// (docs/features/36 screens.md §3, ZW-037). Drawn straight in panel-pages.js (zwRereading, zwStatus); the
// API gains setup_done and setup_total per device, and this file adds three re-read devices to the pretend panel.
proposal({
  bl: 'BL-152',
  title: 'A Z-Wave device read again says so, calmly',
  status: 'approved',
  decided: '2026-10-03: the Z-Wave TLC changes were to be drawn, then built without waiting for review',
  role: 'installer',
  why: [
    'Set it up again reads a working device from the start while its zones keep working. The Devices table showed it as "Being set up", or with a warning tag as "Wake it to finish setting it up" or "Setup stopped", as if the device were unusable.',
  ],
  look: [
    ['zwave', 'Devices 20 to 22: "Being read again", "Read again at its next wake-up" and "Read again stopped: works as before", none with a warning tag'],
  ],
  replaces: ['zwave'],
  notes: [
    'A device whose state is setting up, waiting for a wake-up or stopped with every setup step done is a re-read (the touchscreen\'s rereading()). Its row says "Being read again", "Read again at its next wake-up" or "Read again stopped: works as before", plain.',
    'A re-read that stopped because the device stopped answering says "Not heard for ..."; a low battery is shown too: the calm words never hide a problem.',
    'API: each Z-Wave device gains setup_done and setup_total (additive; the Zigbee devices already send them).',
  ],
  css: ``,
  apply(mock) {
    const devices = mock?.zwave?.devices;
    if (!devices || devices.some(d => d.node === 20)) return;
    const base = {kind: 1, security: 4, unreachable: false, listening: true, battery_low: false, zones: [], setup_done: 16, setup_total: 16, heard_s: 30};
    devices.push({...base, node: 20, generation: 20, model: 'Hall sensor', state: 1},
      {...base, node: 21, generation: 21, model: 'Cellar sensor', state: 2, listening: false},
      {...base, node: 22, generation: 22, model: 'Porch sensor', state: 5});
  },
});
