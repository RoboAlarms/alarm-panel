// BL-152: the Z-Wave devices table says each device's setup state, and the added card says what comes next
// (docs/features/31 mockup/README.md; docs/features/36 screens.md §3 and §11, ZW-019).
// The pages are drawn straight in panel-pages.js (zwStatus, zwDone) and the pretend panel (mock-panel.cjs gives
// its Z-Wave devices every state); this file records the proposal and where to look.
proposal({
  bl: 'BL-152',
  title: 'Z-Wave devices say their state, and the added card says what comes next',
  status: 'approved',
  decided: '2026-10-03: the Z-Wave TLC changes were to be drawn, then built without waiting for review',
  role: 'installer',
  why: [
    'A device that is setting up, waiting for a wake-up, can\'t report, is another controller or is still joining looks like a healthy row in Z-Wave > Devices: only "Not heard" is said. After adding a device the card says "Make it a zone at the panel or under Sensor sources", though Sensor sources is no longer a menu entry, and the same sentence follows a switch or a keypad, which can never be a zone.',
    'Each row says its state in the touchscreen\'s words, problems with a warning tag. The added card of a sensor sends the installer to Installer > Zones with a button; that of a switch, a dimmer, a relay kit or a keypad says to use its page at the panel.',
  ],
  look: [
    ['zwave', 'Devices in every state: being set up, wake it, can\'t report, another controller, setup stopped, joining, not heard for 2 h'],
    ['zwave', 'The added card of a sensor: add a device, listen, allow, PIN 12345 (a switch\'s card says to switch it on its page)'],
  ],
  replaces: ['zwave'],
  notes: [
    'Status word per row (the touchscreen\'s device_look): "Being set up" (plain), then in the warn tag "Wake it to finish setting it up", "It can\'t report to the panel", "Another controller: remove it", "Setup stopped" (the row keeps its "Try its setup again" button), "Being added: its question waits on Add a device", and for a device that is ready but silent "Not heard for 2 h" (or "Not heard since the panel started"). A healthy row keeps "Just now", "5 min ago".',
    'Added card: "Make it a zone under Installer > Zones: open a zone, then Its sensor" with an Open Zones button (#/zone-setup) for a sensor; "Switch it on its page at the panel" for a switch, dimmer or relay kit and "Open its page at the panel to set it up" for a keypad, with no button. Whether a device has inputs comes from the "kind" the API already sends (keypad, switch, dimmer, relays have none).',
    'No API change. The pretend panel gains devices in each state (nodes 7 to 12).',
  ],
  css: ``,
  apply() {},
});
