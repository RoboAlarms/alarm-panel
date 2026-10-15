// The panel's appearances as built (BL-132, docs/features/07 UI-010): the main screens in Light,
// High contrast and High contrast, light, each drawn in its own theme (the layout's themes.xml).
// The PC preview takes the same screens at the end of its walk (walk_themes in
// host/ui_preview/main.c), switching at run time as the panel does.

const look = (id) => () => M.screens.find((s) => s.id === id).render();
const themed = (id, theme, of, title, shot, notes) =>
  M.screen({id, area: 'Appearance', title, shot, theme, render: look(of), ...(notes ? {notes} : {})});

themed('theme-light-home', 'light', 'home-ready', 'Home in Light', 'theme_light_home',
  'Dark words on a light screen; the cards white on a pale grey, the tones deeper so words in them read.');
themed('theme-light-armed', 'light', 'armed-away', 'Armed Away in Light: the code pad', 'theme_light_armed');
themed('theme-light-alarm', 'light', 'alarm-burglary', 'An alarm in Light', 'theme_light_alarm',
  'The screen washes pale red and the state card is pink with a red edge; the same slow glow (UI-020).');
themed('theme-light-saver', 'light', 'screen-saver-armed', 'The screen saver in Light', 'theme_light_saver',
  'The screen saver keeps the dark colours in every appearance (a theme\'s saver=).');
themed('theme-light-settings', 'light', 'settings-display', 'Settings in Light', 'theme_light_settings');
themed('theme-contrast-home', 'contrast', 'home-ready', 'Home in High contrast', 'theme_contrast_home',
  'White on black, the tones brighter; every card, row and control outlined 2 px, what is chosen ringed in white. Nothing moves.');
themed('theme-contrast-alarm', 'contrast', 'alarm-burglary', 'An alarm in High contrast', 'theme_contrast_alarm');
themed('theme-contrast-keys', 'contrast', 'wifi-password', 'Typing in High contrast', 'theme_contrast_keys',
  'Every key outlined; the special keys keep a shade of their own (color_key_special).');
themed('theme-contrast-light-home', 'contrast_light', 'home-ready', 'Home in High contrast, light', 'theme_contrast_light_home');
themed('theme-contrast-light-keys', 'contrast_light', 'wifi-password', 'Typing in High contrast, light', 'theme_contrast_light_keys');
themed('theme-light-devices', 'light', 'devices', 'Smart Home in Light', 'theme_light_devices');
themed('theme-contrast-devices', 'contrast', 'devices', 'Smart Home in High contrast', 'theme_contrast_devices');
themed('theme-contrast-light-devices', 'contrast_light', 'devices', 'Smart Home in High contrast, light',
  'theme_contrast_light_devices');
