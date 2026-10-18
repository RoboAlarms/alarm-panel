// BL-048 / LINK-053. Approved 2026-10-18: implement automatic Wi-Fi and Select Wi-Fi fallback.
// Current firmware screens, verified against the real LVGL enrollment walk.
(() => {
  const row = (title, detail, glyph = 'house', color = 'blue') => M.row_status({title, detail, glyph, color});
  const select = M.row_link({glyph:'wifi', title:'Select Wi-Fi', goTo:'setup-keypad-wifi'});
  const screen = (name, shot, options) => M.screen({id:'bl-048-radio-' + name, area:'Setup',
    title:'Keypad enrollment: ' + options.title, shot, notes:'LINK-053: language, display and audio stay local.',
    render:() => M.wizard({step:4, steps:4, glyph:'house', back:false, ...options})});
  screen('start', 'bl-048-radio-start', {step:2, title:'Find your alarm panel',
    note:'Pair nearby to get Wi-Fi settings automatically.', back:true, next:'Find panel',
    body:row('Find panel','Open Add keypad in Panel Manager or on the alarm panel.','search') +
      M.row_link({glyph:'wifi',title:'Select Wi-Fi',detail:'If the alarm panel is out of range',goTo:'setup-keypad-wifi'})});
  for (const [name, shot, online, waiting, connected] of [
    ['sharing','bl-048-radio-sharing',true,true,false],
    ['lost','bl-048-radio-lost',false,false,false],
    ['done-radio','bl-048-radio-done-radio',true,false,false],
    ['done','bl-048-setup-pair-done',true,false,true]
  ]) screen(name, shot, {title:'Keypad paired',
    note:'Your pairing is saved. Language, display and sound stay on this keypad.',
    next:connected ? 'Done' : 'Continue without Wi-Fi', nextDisabled:!online || waiting,
    body:row(online ? 'Connected to the alarm panel' : 'Reconnecting to the alarm panel','', 'house',online ? 'green' : 'amber') +
      row(connected ? 'Wi-Fi connected' : waiting ? 'Setting up Wi-Fi' : 'Wi-Fi is not connected',
        waiting ? 'Getting settings from the alarm panel and joining its network.' : '', 'wifi',connected ? 'green' : 'blue') +
      (connected ? '' : select + (waiting ? '' : M.row_link({glyph:'refresh-cw',title:'Try again',goTo:'bl-048-radio-sharing'})))});
  screen('search','bl-048-setup-pair-looking',{step:3,title:'Pair with the alarm panel',
    note:'Find it nearby, or select Wi-Fi if it is out of range.',back:true,next:'Next',nextDisabled:true,
    body:row('Looking for the alarm panel','Nearby or on your Wi-Fi','search') +
      M.paragraph('Open Add keypad in Panel Manager or on the alarm panel.') + select});
})();
