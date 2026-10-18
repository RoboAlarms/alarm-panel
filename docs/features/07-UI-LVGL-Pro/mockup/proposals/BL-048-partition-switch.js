// KPD-006: temporary partition switching; saved home and audio preferences do not change.
const gotoProposal = {bl: 'BL-048', status: 'draft', replaces: 'home-ready',
  why: 'Control another enabled partition with an authorized code, then return home after two minutes idle.'};
M.screen({id: 'bl-048-partition-home', area: 'Home', title: 'Choose another partition', shot: null,
  proposal: gotoProposal,
  notes: 'This is the normal Home screen when multiple partitions are enabled. Tap House above the arm buttons to open the chooser; it never opens automatically. With one enabled partition, Home stays unchanged. No saved audio settings change.',
  render: () => M.main({content: M.home(
    {tone: 'green', glyph: 'shield-check', headline: 'Disarmed', detail: 'Ready to arm', note: 'All zones closed'},
    M.changed(M.row_link({glyph: 'layers', title: 'House', detail: 'Home partition', trail: 'chevron-right', goTo: 'bl-048-partition-chooser'})) + M.panel_arm())}),
});
M.screen({id: 'bl-048-partition-chooser', area: 'Home', title: 'Choose a partition', shot: null,
  proposal: gotoProposal,
  notes: 'Opened only by tapping the partition row on Home. Choose Workshop to request access with a code. House returns to the current Home screen. Disabled partitions are omitted.',
  render: () => M.main({content: M.home(
    {tone: 'blue', glyph: 'layers', headline: 'Choose a partition', detail: 'Currently House', note: 'Your code must allow access'},
    M.changed(M.row_link({glyph: 'home', title: 'House', detail: 'Home partition · Current', trail: 'chevron-right', goTo: 'bl-048-partition-home'}) +
      M.row_link({glyph: 'layers', title: 'Workshop', detail: 'Enter code to switch', trail: 'chevron-right', goTo: 'bl-048-partition-code'})))}),
});
M.screen({id: 'bl-048-partition-single', area: 'Home', title: 'One partition: unchanged Home', shot: null,
  proposal: gotoProposal,
  notes: 'With only one enabled partition there is no partition row or chooser. The existing Home screen stays unchanged.',
  render: () => M.main({content: M.home(
    {tone: 'green', glyph: 'shield-check', headline: 'Disarmed', detail: 'Ready to arm', note: 'All zones closed'},
    M.panel_arm())}),
});
M.screen({id: 'bl-048-partition-code', area: 'Home', title: 'Authorize the selected partition', shot: null,
  proposal: {...gotoProposal, replaces: 'pad-arm-away'},
  notes: 'Only enabled partitions appear in the chooser. This is the code prompt after choosing Workshop; authorization is checked by the master.',
  render: () => M.main({content: M.home(
    {tone: 'green', glyph: 'shield-check', headline: 'Disarmed', detail: 'House', note: 'Home partition'},
    M.changed(M.code_pad({title: 'Switch to Workshop', tone: 'blue', glyph: 'layers', digits: 3})))}),
});
M.screen({id: 'bl-048-partition-away', area: 'Home', title: 'Temporarily controlling Workshop', shot: null,
  proposal: gotoProposal,
  notes: 'The existing arm/disarm controls act on Workshop. Return home is explicit; two minutes without interaction also returns home. Restart/reconnect and reassignment clear the selection.',
  render: () => M.main({content: M.home(
    {tone: 'green', glyph: 'shield-check', headline: 'Disarmed', detail: 'Workshop', note: 'Returns to House after 2 minutes idle'},
    M.changed(M.row_link({glyph: 'home', title: 'Return to House', detail: 'Home partition', trail: 'chevron-right', goTo: 'bl-048-partition-home'})) + M.panel_arm())}),
});
M.screen({id: 'bl-048-partition-denied', area: 'Home', title: 'Partition access refused', shot: null,
  proposal: gotoProposal,
  notes: 'A refused code leaves the current partition unchanged. Normal source lockout applies. A lost link refuses switching and retains the existing No system communication screen.',
  render: () => M.main({content: M.home(
    {tone: 'green', glyph: 'shield-check', headline: 'Disarmed', detail: 'House', note: 'Home partition'},
    M.changed(M.paragraph('This code cannot control Workshop.')) + M.panel_arm())}),
});
