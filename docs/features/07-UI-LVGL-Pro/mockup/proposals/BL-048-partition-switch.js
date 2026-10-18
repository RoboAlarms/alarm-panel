// KPD-006: per-panel installer policy; selecting a view does not authorize alarm actions.
const gotoProposal = {bl: 'BL-048', status: 'draft', replaces: 'home-ready',
  why: 'Show the partition selector under the left status card; installer controls switching and return delay.',
  decided: '2026-10-18: owner requested no code to switch views, per-panel switching lock, and installer-set return delay defaulting to 30 seconds. Revised layout awaits review.'};
const partitionHero = {tone: 'green', glyph: 'shield-check', headline: 'Disarmed', detail: 'Ready to arm', note: 'All zones closed'};
M.screen({id: 'bl-048-partition-home', area: 'Home', title: 'Home: switcher below the status card', shot: null,
  proposal: gotoProposal,
  notes: 'Multiple enabled partitions and switching allowed on this panel: House appears below the left card. Tap it to choose another partition. Away, Stay and Night remain available on the right. No code is needed to change the view; alarm actions retain their normal authorization.',
  render: () => M.main({content: M.home(partitionHero, M.panel_arm(),
    M.changed(M.row_link({glyph: 'layers', title: 'House', detail: 'Default partition · Tap to switch', trail: 'chevron-right', goTo: 'bl-048-partition-chooser'})))}),
});
M.screen({id: 'bl-048-partition-chooser', area: 'Home', title: 'Choose a partition: no code prompt', shot: null,
  proposal: gotoProposal,
  notes: 'Opened only by tapping the switcher. Tap Workshop to view it immediately, without a code prompt. House returns to the original view. Only enabled partitions appear; selecting an armed partition does not disarm it.',
  render: () => M.main({content: M.home(
    {tone: 'blue', glyph: 'layers', headline: 'Choose a partition', detail: 'Currently House', note: 'Choose which partition to view'},
    M.changed(M.row_link({glyph: 'home', title: 'House', detail: 'Default partition · Current', trail: 'chevron-right', goTo: 'bl-048-partition-home'}) +
      M.row_link({glyph: 'layers', title: 'Workshop', detail: 'View this partition', trail: 'chevron-right', goTo: 'bl-048-partition-away'})))}),
});
M.screen({id: 'bl-048-partition-single', area: 'Home', title: 'One partition: unchanged Home', shot: null,
  proposal: gotoProposal,
  notes: 'With only one enabled partition there is no switcher. The existing Home screen stays unchanged.',
  render: () => M.main({content: M.home(partitionHero, M.panel_arm())}),
});
M.screen({id: 'bl-048-partition-locked', area: 'Home', title: 'Panel locked to its default partition', shot: null,
  proposal: gotoProposal,
  notes: 'The installer has turned switching off for this panel. Home looks exactly as it does today: no switcher, partition label, or locked notice below the left card. All controls remain scoped to the assigned default partition; a different panel may allow switching.',
  render: () => M.main({content: M.home(partitionHero, M.panel_arm())}),
});
M.screen({id: 'bl-048-partition-away', area: 'Home', title: 'Workshop: return after 30 seconds idle', shot: null,
  proposal: gotoProposal,
  notes: 'The same arm/disarm controls now target Workshop with their normal code requirements. After 30 seconds without interaction this panel returns to its saved default, House. The installer can change that delay per panel. Touch activity restarts the idle timer. Switching never copies audio preferences.',
  render: () => M.main({content: M.home({...partitionHero, detail: 'Workshop', note: 'Returns to House after 30 seconds idle'},
    M.panel_arm(), M.changed(M.row_link({glyph: 'layers', title: 'Workshop', detail: 'Temporary view · Tap to switch', trail: 'chevron-right', goTo: 'bl-048-partition-chooser'})))}),
});
M.screen({id: 'bl-048-partition-policy', area: 'Settings', title: 'Installer: partition settings for this panel', shot: null,
  proposal: {...gotoProposal, replaces: null},
  notes: 'Installer-only settings, stored separately for each panel. Default partition selects its normal view. Allow partition switching off locks it there. Return delay is editable in seconds, default 30, and applies only while switching is allowed. These are proposed controls; firmware persistence and enforcement are still pending.',
  render: () => M.main({view: 'settings', content: M.settings({section: 'keypads', title: 'Hall keypad', note: 'Installer · Partition settings',
    body: M.changed(M.row_link({glyph: 'home', title: 'Default partition', detail: 'House', trail: 'chevron-right'}) +
      M.row_toggle({title: 'Allow partition switching', detail: 'Off locks this panel to its default partition', on: true}) +
      M.row_number({title: 'Return after inactivity', detail: 'Seconds · Default 30', value: '30'}))})}),
});
