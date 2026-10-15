// BL-152 (ZW-024): the Z-Wave operation card for "Rebuild all routes" names the sleepers to wake while it
// runs and, when it ends, the devices that didn't answer or were skipped (docs/features/36 screens.md section 3).
// Drawn straight in panel-pages.js (zwOpCard, zwDone); this file records the proposal and where to look.
proposal({
  bl: 'BL-152',
  title: 'Rebuilding routes says who to wake and who failed',
  status: 'approved',
  decided: '2026-10-03: the Z-Wave TLC changes were to be drawn, then built without waiting for review',
  role: 'installer',
  why: [
    'The card showed only "3 of 7 done" and, at the end, "Routes rebuilt for 5 of 7": a sleeper to wake, a failed device and a skipped one were all invisible.',
    'While it runs the card adds "Wake these: Front Door, Garage tilt"; at its end it says how many were rebuilt and names the devices that didn\'t answer and the ones skipped (dead, Long Range or stopped), in a warning card until OK.',
  ],
  look: [['zwave', 'Rebuild all routes: the running card, then the result']],
  replaces: ['zwave'],
  notes: ['The API\'s progress gains routes_failed, routes_skipped, routes_waiting, routes_stopped and routes_list ([{node, what}] with what: waiting, failed, skipped, no_route). Names come from the devices table.'],
  css: ``,
  apply() {},
});
