// KPD-006: per-panel installer policy; selecting a view does not authorize alarm actions.
// Approved by the owner 2026-10-18; implemented and checked in the PC preview.
const partitionHero = {tone: 'green', glyph: 'shield-check', headline: 'Disarmed', detail: 'Ready to arm', note: 'All zones closed'};
M.screen({id: 'bl-048-partition-home', area: 'Home', title: 'Home: switcher below the status card', shot: 'bl-048-partition-home',
  notes: 'Multiple enabled partitions and switching allowed on this panel: Home appears below the left card. Tap it to choose another partition. Away, Stay and Night remain available on the right. No code is needed to change the view; alarm actions retain their normal authorization.',
  render: () => M.main({content: M.home(partitionHero, M.panel_arm(),
    M.row_link({glyph: 'warehouse', title: 'Home', detail: 'Switch partition', trail: 'chevron-right', goTo: 'bl-048-partition-chooser'}))}),
});
M.screen({id: 'bl-048-partition-chooser', area: 'Home', title: 'Choose a partition: no code prompt', shot: 'bl-048-partition-chooser',
  notes: 'Opened only by tapping the switcher. Tap Studio to view it immediately, without a code prompt. Home returns to the original view. Only enabled partitions appear; selecting an armed partition does not disarm it.',
  render: () => M.main({content: M.home(partitionHero, M.panel_arm(),
    M.row_link({glyph: 'warehouse', title: 'Home', detail: 'Switch partition'})),
    over: M.pick_sheet({title: 'Choose a partition', fit: true, rows: [
      M.row_link({glyph: 'warehouse', title: 'Home', trail: 'check', trailColor: 'blue', goTo: 'bl-048-partition-home'}),
      M.row_link({glyph: 'warehouse', title: 'Studio', trail: '', goTo: 'bl-048-partition-away'}),
    ].join('')})}),
});
M.screen({id: 'bl-048-partition-single', area: 'Home', title: 'One partition: unchanged Home', shot: null,
  notes: 'With only one enabled partition there is no switcher. The existing Home screen stays unchanged.',
  render: () => M.main({content: M.home(partitionHero, M.panel_arm())}),
});
M.screen({id: 'bl-048-partition-locked', area: 'Home', title: 'Panel locked to its default partition', shot: 'bl-048-partition-locked',
  notes: 'The installer has turned switching off for this panel. Home looks exactly as it does today: no switcher, partition label, or locked notice below the left card. All controls remain scoped to the assigned default partition; a different panel may allow switching.',
  render: () => M.main({content: M.home(partitionHero, M.panel_arm())}),
});
M.screen({id: 'bl-048-partition-away', area: 'Home', title: 'Studio: return after 30 seconds idle', shot: 'bl-048-partition-away',
  notes: 'The same arm/disarm controls now target Studio with their normal code requirements. After 30 seconds without interaction this panel returns to its saved default, Home. The installer can change that delay per panel. Touch activity restarts the idle timer. Switching never copies audio preferences.',
  render: () => M.main({content: M.home(partitionHero,
    M.panel_arm(), M.row_link({glyph: 'warehouse', title: 'Studio', detail: 'Switch partition', trail: 'chevron-right', goTo: 'bl-048-partition-chooser'}))}),
});
M.screen({id: 'bl-048-partition-policy', area: 'Settings', title: 'Installer: partition settings for this panel', shot: 'bl-048-partition-policy',
  notes: 'Installer-only settings, stored separately for each panel. Default partition selects its normal view. Allow partition switching off locks it there. The Never return automatically switch sits directly below the seconds control. On disables that control; off makes it editable again, preserving its value (default 30). No separate delay screen. Never disables only the inactivity return. Firmware persistence and enforcement are verified on the paired bench panels.',
  render: () => M.main({view: 'settings', content: M.settings({section: 'about', title: 'This panel', note: 'Installer · Partition settings',
    body: M.row_link({glyph: 'warehouse', title: 'Default partition', value: 'Home', trail: 'chevron-right', trail: 'chevron-right'}) +
      M.row_toggle({title: 'Allow partition switching', detail: 'Off locks this panel to its default partition', on: true}) +
      M.row_number({title: 'Return after inactivity', detail: 'Seconds · Default 30', value: '30'}) +
      M.row_toggle({title: 'Never return automatically', on: false, goTo: 'bl-048-partition-policy-never'})})}),
});

M.screen({id: 'bl-048-partition-policy-never', area: 'Settings', title: 'Installer: return delay set to Never', shot: 'bl-048-partition-policy-never',
  notes: 'Never disables inactivity-based return and dims/disables the seconds control immediately above it. Tap the switch to see the off state in this mockup. The selected partition remains until the user switches it, or a restart/reconnection or installer reassignment resets the selection. This is not a permanent change to the default partition.',
  render: () => M.main({view: 'settings', content: M.settings({section: 'about', title: 'This panel', note: 'Installer · Partition settings',
    body: M.row_link({glyph: 'warehouse', title: 'Default partition', value: 'Home', trail: 'chevron-right'}) +
      M.row_toggle({title: 'Allow partition switching', detail: 'Off locks this panel to its default partition', on: true}) +
      M.row_number({title: 'Return after inactivity', detail: 'Seconds · Default 30', value: '30', disabled: true}) +
      M.row_toggle({title: 'Never return automatically', on: true, goTo: 'bl-048-partition-policy'})})}),
});
M.screen({id: 'bl-048-partition-away-never', area: 'Home', title: 'Studio: no inactivity return', shot: null,
  notes: 'With return delay set to Never, being idle does not change the selected partition. No return countdown or promise is displayed.',
  render: () => M.main({content: M.home(partitionHero, M.panel_arm(),
    M.row_link({glyph: 'warehouse', title: 'Studio', detail: 'Switch partition', goTo: 'bl-048-partition-chooser'}))}),
});
