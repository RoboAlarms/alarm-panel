// BL-132: the panel's appearance (decided 2026-10-01: a light mode as well as the dark one, and a
// high-contrast mode for people who have a hard time seeing). The palettes are the layout's themes.xml, held
// to WCAG contrast by tools/ui_mockup.py's check; the viewer's Appearance switch shows any screen
// in any of them. Here: where a person picks it (Screen and sound), and the main screens drawn in
// each appearance (a screen's own theme), beside today's.

const P = (replaces, why) => ({bl: 'BL-132', status: 'approved', decided: '2026-10-01: approved as drawn: "start working on getting all of the screen changes in place"', ...(replaces ? {replaces} : {}), why});
const look = (id) => () => M.screens.find((s) => s.id === id).render();

// Screen and sound (BL-131's page) with the appearance on top.
const GO = {display: 'a-display', users: 'settings-users', 'wifi': 'n-wifi', time: 'n-time', integrations: 'n-integrations',
  walk: 'settings-walk', about: 'n-about', installer: 'n-installer-menu'};
const display = ({appearance = 'Automatic', contrast = false, mark = true}) => M.main({next: true, view: 'settings', rail: {troubles: 1},
  bar: {time: '10:50', troubles: 1}, content: M.settings({next: true, go: GO, section: 'display', title: 'Screen and sound', body: [
    M.section('Screen'),
    (mark ? M.changed : (h) => h)([
      M.row_link({glyph: 'sun-moon', title: 'Appearance', value: appearance, goTo: 'a-appearance',
        detail: appearance === 'Automatic' ? 'Dark during the night hours, light by day' : ''}),
      M.row_toggle({glyph: 'contrast', title: 'High contrast', detail: 'Stronger colours, every button outlined', on: contrast}),
    ].join('')),
    M.row_level({glyph: 'sun', title: 'Brightness', value: 80}),
    M.row_link({glyph: 'image', title: 'Screen saver', detail: 'A dimmed photo with the time and the alarm state', value: 'After 1 minute'}),
    M.row_link({glyph: 'monitor-off', title: 'Screen off', detail: 'Dark until a touch wakes it', value: 'Never'}),
    M.row_link({glyph: 'moon', color: 'violet', title: 'Night hours', detail: '10:00 PM to 6:00 AM · dimmer, and dark sooner', value: 'On', goTo: 'n-night'}),
    M.section('Sound'),
    M.row_level({glyph: 'volume-2', title: 'Volume', value: 70}),
  ].join('')})});

M.screen({
  id: 'a-display', area: 'Settings', title: 'Screen and sound: Appearance', shot: null,
  proposal: P('n-display', 'The panel has one look today, dark (UI-010: "a light theme is future work"). Appearance picks Dark, Light or Automatic, and High contrast is a switch beside it that works with either. Both sit at the top of Screen, where people look for how the screen looks; like brightness, they need no code (UI-015).'),
  notes: 'The change applies at once, in one step: nothing fades or slides (UI-020). High contrast keeps the layout and the fonts: it changes colours and outlines, so nothing moves or gets cut off. Larger text would be a feature of its own (a layout with room for it).',
  render: () => display({}),
});

M.screen({
  id: 'a-appearance', area: 'Settings', title: 'Choosing the appearance', shot: null,
  proposal: P(null, 'Three choices, each saying when it suits. Automatic follows the night hours the panel already keeps (UI-043), so a hallway panel is dark at night, when it is looked at in the dark, and light by day.'),
  notes: 'With the night hours off, Automatic says so under its name and stays light. The switch happens at the start and the end of the night hours, once each, never back and forth (UI-020). The screen saver stays a dimmed photo in every appearance.',
  render: () => M.main({next: true, view: 'settings', rail: {troubles: 1}, bar: {time: '10:50', troubles: 1},
    content: M.settings({next: true, go: GO, section: 'display', title: 'Screen and sound'}),
    over: M.pick_sheet({title: 'Appearance', note: 'How the screen looks', fit: true, rows: [
      M.row_link({glyph: 'moon', color: 'violet', title: 'Dark', detail: 'Light words on a dark screen: easy on the eyes at night', trail: ''}),
      M.row_link({glyph: 'sun', color: 'amber', title: 'Light', detail: 'Dark words on a light screen, for a bright room', trail: ''}),
      M.row_link({glyph: 'sun-moon', color: 'blue', title: 'Automatic', detail: 'Dark during the night hours (10:00 PM to 6:00 AM), light by day', trail: 'check', trailColor: 'blue'}),
    ].join('')})}),
});

// The main screens in each appearance, beside today's dark ones.
const shown = ({id, theme, replaces, title, why, render}) => M.screen({id, area: 'Appearance', title, shot: null, theme,
  proposal: P(replaces, why), render: render || look(replaces)});

shown({id: 'a-home-light', theme: 'light', replaces: 'n-home', title: 'Security in Light',
  why: 'Light: dark words on a light grey screen, white cards, the tones deepened so their words still read (4.5:1 or more everywhere, WCAG AA).'});
shown({id: 'a-armed-light', theme: 'light', replaces: 'n-armed', title: 'Armed Away in Light: the code pad',
  why: 'The keys are grey on the white card, the Enter key in the action\'s tone with white words on it.'});
shown({id: 'a-devices-light', theme: 'light', replaces: 'n-devices', title: 'Smart Home in Light',
  why: 'The tiles are white cards on the grey screen; the state colours are the same family, deeper.'});
shown({id: 'a-settings-light', theme: 'light', replaces: 'a-display', title: 'Settings in Light', render: () => display({mark: false}),
  why: 'The section list on the screen\'s grey, the page\'s rows as white cards.'});
shown({id: 'a-home-contrast', theme: 'contrast', replaces: 'n-home', title: 'Security in High contrast',
  why: 'High contrast: white words on black, the tones brighter, and every card and button outlined, so edges show without telling dark greys apart. Every word is 7:1 or more (WCAG AAA), outlines 3:1 or more.'});
shown({id: 'a-devices-contrast', theme: 'contrast', replaces: 'n-devices', title: 'Smart Home in High contrast',
  why: 'Each tile and control has an outline; what is chosen (the room, a speed) is ringed in white.'});
shown({id: 'a-home-contrast-light', theme: 'contrast_light', replaces: 'n-home', title: 'Security in High contrast, light',
  why: 'High contrast with Light: black on white, deep tones, the same outlines, for people who read better on white.'});
shown({id: 'a-devices-contrast-light', theme: 'contrast_light', replaces: 'n-devices', title: 'Smart Home in High contrast, light',
  why: 'The tiles are white with dark outlines; the words are black or a deep tone.'});
shown({id: 'a-keys-contrast', theme: 'contrast', replaces: 'n-wifi-password', title: 'Typing in High contrast',
  why: 'Every key outlined, the letters white. The special keys (shift, delete, 123, accents, return) keep a shade of their own, their symbols 11:1: filled with the outline colour, as today\'s style would, they were under 2:1 and all but vanished.'});
shown({id: 'a-keys-contrast-light', theme: 'contrast_light', replaces: 'n-wifi-password', title: 'Typing in High contrast, light',
  why: 'Black letters on pale keys with dark outlines; the special keys a shade darker, their symbols near black, 11.6:1.'});

M.screen({
  id: 'a-display-contrast', area: 'Appearance', title: 'Screen and sound with High contrast on', shot: null, theme: 'contrast',
  proposal: P('a-display', 'The same page with the switch on: rows outlined, the hints as bright as the words around them, the section list\'s choice ringed.'),
  render: () => display({appearance: 'Dark', contrast: true, mark: false}),
});

const alarm = () => M.main({next: true, tone: 'rose', alarm: true, rail: {locked: true}, bar: {banner: null, time: '10:52'}, content: M.home(
  {tone: 'red', glyph: 'siren', headline: 'Burglary alarm', detail: 'Front Door', note: 'Enter your code to disarm', alarm: true},
  M.code_pad({title: 'Disarm', tone: 'green', glyph: 'lock-open', close: false}))});
M.screen({
  id: 'a-alarm-light', area: 'Appearance', title: 'An alarm in Light', shot: null, theme: 'light', render: alarm,
  proposal: P('alarm-burglary', 'An alarm stays unmistakable in Light: the screen washes pale red, and the state card is pink with a red edge, ring and icon, its headline in the darkest text. Its slow glow is unchanged (UI-020).'),
});
M.screen({
  id: 'a-alarm-contrast', area: 'Appearance', title: 'An alarm in High contrast', shot: null, theme: 'contrast', render: alarm,
  proposal: P('alarm-burglary', 'In High contrast the alarm\'s red is brighter and the card darker red, the keys outlined.'),
});
M.screen({
  id: 'a-saver-light', area: 'Appearance', title: 'The screen saver in Light', shot: null, theme: 'light', render: look('screen-saver'),
  proposal: P('screen-saver', 'The screen saver stays a dimmed photo with the time in every appearance (its words are on the photo\'s dark side); it keeps the dark colours.'),
});
