// The panel's components as functions that return HTML, one per LVGL component of the 7" layout
// (components/ui/alarm_ui/layouts/1024x600/alarm_ui), with the XML's names and props:
//   <arm_tile glyph="&#xE10E;" title="Away" subtitle="Everyone is leaving" color="#color_rose"/>
//   M.arm_tile({glyph: 'log-out', title: 'Away', subtitle: 'Everyone is leaving', color: 'rose'})
// Glyphs are Lucide names (kit/lucide-codepoints.json), colours are tone names (green, amber,
// blue, violet, rose, orange, red) or a token (text_2). Screens compose these; a screen that
// needs something no component offers adds a component here, named as the XML one will be.
// Rows the C code builds (ui_forms.c) have the helper's name: M.row_link = ui_row_link.
//
// Nothing here may move or blink (AGENTS.md "Display safety"): the mockup is still pictures.

const M = (() => {
  const ICONS = window.MOCKUP_ICONS || {};          // Lucide name -> hex codepoint (all of Lucide)
  const PANEL_ICONS = new Set(window.MOCKUP_PANEL_ICONS || []); // the ones in the panel's fonts
  const screens = [];
  const missing = new Set();

  const esc = (s) => String(s ?? '').replace(/[&<>"]/g, (c) => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));
  const TONES = ['green', 'amber', 'blue', 'violet', 'rose', 'orange', 'red'];
  // A colour prop: a tone ('rose'), a token ('text_2', 'on_tone') or 'gray' (text_2, ui_tone_gray()).
  const color = (c) => (!c ? 'inherit' : c === 'gray' ? 'var(--color_text_2)' : `var(--color_${c})`);
  const go = (id) => (id ? ` data-go="${esc(id)}"` : '');
  const cls = (...c) => c.filter(Boolean).join(' ');

  /** A Lucide glyph in one of the panel's icon fonts (20, 28, 40, 96). */
  function icon(name, size = 20, c) {
    const cp = ICONS[name];
    if (!cp) { missing.add(name); return `<span class="ic icons_${size} unknown" title="unknown icon ${esc(name)}"></span>`; }
    if (!PANEL_ICONS.has(name)) missing.add(`${name} (not in the panel's icon fonts yet)`);
    return `<span class="ic icons_${size}" style="color:${c ? color(c) : 'inherit'}">&#x${cp};</span>`;
  }
  /** Text in one of the panel's fonts: caption 15, body 19, label 20, title 26, headline 42, keypad 34, digits 88. */
  function text(s, font = 'body', c, extra = '') {
    return `<span class="font_${font}${extra ? ' ' + extra : ''}"${c ? ` style="color:${color(c)}"` : ''}>${esc(s)}</span>`;
  }
  /** Same, but the words may hold markup the screen wrote itself (a <b>, an M.icon()). */
  const raw = (html, font = 'body', c) => `<span class="font_${font}"${c ? ` style="color:${color(c)}"` : ''}>${html}</span>`;

  // ---- common ----
  const icon_chip = ({glyph, size = 56, font = 28, color: c = 'green'}) =>
    `<div class="icon_chip" style="width:${size}px;height:${size}px;color:${color(c)}">${icon(glyph, font)}</div>`;
  const badge = (n, c = 'amber') => (n ? `<div class="badge font_caption" style="background:${color(c)}">${esc(n)}</div>` : '');
  const view_header = ({title, note}) =>
    `<div class="view_header">${text(title, 'title')}${note ? text(note, 'body', 'text_2') : ''}</div>`;
  const segments = (items, active = 0) =>
    `<div class="segments">${items.map((s, i) => `<div class="segment font_label${i === active ? ' active' : ''}">${esc(s)}</div>`).join('')}</div>`;

  // ---- rail ----
  const NAV = [
    {key: 'home', glyph: 'shield', text: 'Security', go: 'home-ready'},
    {key: 'zones', glyph: 'layout-grid', text: 'Zones', go: 'zones'},
    {key: 'devices', glyph: 'house-wifi', text: 'Smart\nHome', go: 'devices'},
    {key: 'activity', glyph: 'history', text: 'Activity', go: 'activity'},
  ];
  /**
   * The navigation rail: places only (BL-131). view: home|zones|devices|activity|panic; tone colours
   * the logo. Smart Home (BL-045, HCTL-003) shows only while there is a device to control
   * (devices: false hides it); its name is on two lines and its button a line taller (.two). panic:
   * false (BL-048) leaves Panic out, as a keypad the alarm panel removed shows it.
   */
  function rail({view = 'home', tone = 'green', zones = 0, devices = true, panic = true} = {}) {
    const counts = {zones};
    return `<div class="rail">
      <div class="rail_logo" style="color:${color(tone)}"><i></i></div>
      ${NAV.filter((n) => devices || n.key !== 'devices').map((n) => `<div class="nav_item${n.text.includes('\n') ? ' two' : ''}${view === n.key ? ' active' : ''}"${go(n.go)}>
        ${icon(n.glyph, 28)}${raw(n.text.split('\n').map(esc).join('<br>'), 'caption')}${badge(counts[n.key])}</div>`).join('')}
      <div class="rail_fill"></div>
      ${panic ? `<div class="nav_item panic${view === 'panic' ? ' active' : ''}"${go('panic')}>${icon('siren', 28)}${text('Panic', 'caption')}</div>` : ''}
    </div>`;
  }

  // ---- status bar ----
  const BANNERS = {
    codes: {glyph: 'key-round', tone: 'amber', text: 'Factory codes · Tap to change'},
    installer: {glyph: 'wrench', tone: 'blue', text: 'Installer · Arming off · Tap to leave'},
    walk: {glyph: 'footprints', tone: 'blue', text: 'Walk test'},
    remote: {glyph: 'globe', tone: 'red', text: 'Alex is using this keypad · Tap to pause', solid: true},
    // arm_banner: an automation's arming warning, steady, no countdown; a tap opens Home
    arm: {glyph: 'clock-alert', tone: 'amber', text: 'Arming Night at 10:30 PM', goTo: 'home-arm-warning'},
  };
  /**
   * The status bar. banner: null (the date) | 'codes' | 'installer' | 'walk' | 'remote' | 'arm', or
   * {glyph, tone, text, goTo}; bannerText replaces a named banner's words. battery: null (none shown),
   * 'none' (the red struck-out battery: no battery fitted), or {glyph, text, level: ''|'warn'|'critical'}.
   * chimeOff (BL-048): Chime dimmed alone, as a keypad without the alarm panel shows it (Settings stays).
   */
  function bar({banner = 'codes', bannerText, date = 'Saturday, September 19', time = '10:41', ampm = 'PM',
    wifi = 'wifi', wifiOk = true, battery = 'none', troubles = 0, chime = false, settings = false, locked = false, chimeOff = false, view} = {}) {
    let left = text(date, 'label', 'text_2');
    if (banner) {
      const b = typeof banner === 'string' ? BANNERS[banner] : banner;
      left = `<div class="bar_banner${b.solid ? ' solid' : ''}" style="color:${b.solid ? 'var(--color_on_tone)' : color(b.tone)}"${go(b.goTo)}>
        ${icon(b.glyph, 20)}<span class="font_label dots">${esc(bannerText || b.text)}</span></div>`;
    }
    let bat = '';
    if (battery === 'none') {
      bat = `<div class="bar_battery err"><div style="position:relative;width:24px;height:24px;display:flex;align-items:center;justify-content:center">${icon('battery', 20)}<i class="strike"></i></div></div>`;
    } else if (battery) {
      bat = `<div class="bar_battery ${battery.level === 'critical' ? 'err' : battery.level === 'warn' ? 'warn' : 't2'}">
        <div style="width:24px;height:24px;display:flex;align-items:center;justify-content:center">${icon(battery.glyph || 'battery-medium', 20)}</div>
        ${battery.text ? text(battery.text, 'caption') : ''}</div>`;
    }
    return `<div class="bar">${left}<div class="bar_fill"></div>
      ${troubles ? `<div class="bar_trouble${view === 'troubles' ? ' active' : ''}"${go('troubles')}>${icon('triangle-alert', 20)}${text(troubles, 'label')}</div>` : ''}
      <span class="${wifiOk ? 't2' : 'warn'}">${icon(wifi, 20)}</span>
      ${bat}
      <div class="bar_clock">${text(time, 'title')}${ampm ? text(ampm, 'caption', 'text_2', 'ampm') : ''}</div>
      <div class="bar_round${chime ? ' on' : ''}${locked || chimeOff ? ' disabled' : ''}"${go(locked || chimeOff ? '' : 'chime-pad')}>${icon(chime ? 'bell' : 'bell-off', 20)}</div>
      <div class="bar_settings${settings ? ' active' : ''}${locked ? ' disabled' : ''}"${go(locked ? '' : 'settings-display')}>${icon('settings', 20)}</div>
    </div>`;
  }

  // ---- home ----
  /** A ring: full in the state's tone, or a countdown (fill 0..1) during exit and entry time. */
  function ring(size, width, tone, fill = 1, track = true) {
    const r = (size - width) / 2, c = 2 * Math.PI * r, len = Math.max(0, Math.min(1, fill)) * c;
    return `<svg width="${size}" height="${size}" viewBox="0 0 ${size} ${size}">
      ${track ? `<circle cx="${size / 2}" cy="${size / 2}" r="${r}" fill="none" stroke="var(--color_text)" stroke-opacity=".08" stroke-width="${width}"/>` : ''}
      ${fill > 0 ? `<circle cx="${size / 2}" cy="${size / 2}" r="${r}" fill="none" stroke="${color(tone)}" stroke-width="${width}"
        stroke-linecap="round" stroke-dasharray="${len} ${c}" transform="rotate(-90 ${size / 2} ${size / 2})"/>` : ''}</svg>`;
  }
  /** The state card: ring, icon or seconds left, headline, detail (a "\n" starts a line of its own), note. */
  function hero({tone = 'green', glyph = 'shield-check', countdown = 0, fill = 1, headline, detail, note, alarm = false}) {
    return `<div class="hero${alarm ? ' alarm' : ''}">
      <div class="ring_box">${ring(236, 12, tone, fill)}
        <div class="disk" style="color:${color(tone)}">${countdown ? text(countdown, 'digits', null, 'count') : icon(glyph, 96)}</div></div>
      ${text(headline, 'headline')}
      ${String(detail).includes('\n') ? `<span class="font_body detail" style="color:${color('text_2')}">${String(detail).split('\n').map(esc).join('<br>')}</span>`
        : text(detail, 'body', 'text_2', 'detail')}
      ${note ? text(note, 'caption', 'text_3', 'note dots') : ''}
    </div>`;
  }
  /** disabled (BL-048 proposal): dimmed and not tappable (LV_STATE_DISABLED), as a keypad shows them without the alarm panel. */
  const arm_tile = ({glyph, title, subtitle, color: c, goTo, disabled = false}) => `<div class="arm_tile${disabled ? ' disabled' : ''}"${disabled ? '' : go(goTo)}>
    ${icon_chip({glyph, size: 76, font: 40, color: c})}
    <div class="words">${text(title, 'title')}${text(subtitle, 'body', 'text_2')}</div>${icon('chevron-right', 28, 'text_3')}</div>`;
  /**
   * The three arming tiles of Home (panel_arm). above (D19): something drawn over
   * the tiles in the same pane (the arming warning's strip); the tiles share what height is left and
   * are otherwise exactly as they are. disabled (BL-048 proposal): the three tiles dimmed, not what is above them.
   */
  const panel_arm = (above, {disabled = false} = {}) => (above ? `<div class="pane_stack">${above}` : '') + `<div class="panel_arm">
    ${arm_tile({glyph: 'log-out', title: 'Away', subtitle: 'Everyone is leaving', color: 'rose', goTo: 'pad-arm-away', disabled})}
    ${arm_tile({glyph: 'house', title: 'Stay', subtitle: 'Someone is home', color: 'blue', goTo: 'pad-arm-away', disabled})}
    ${arm_tile({glyph: 'moon', title: 'Night', subtitle: 'Going to bed', color: 'violet', goTo: 'pad-arm-away', disabled})}</div>` + (above ? '</div>' : '');
  /**
   * arm_levels (components/forms/arm_levels): Away, Stay and Night as one 64 px row of three keys, each with the
   * arming tile's glyph; the chosen one tinted in its level's tone. active: 0..2
   */
  const ARM_LEVELS = [{glyph: 'log-out', text: 'Away', tone: 'rose'}, {glyph: 'house', text: 'Stay', tone: 'blue'}, {glyph: 'moon', text: 'Night', tone: 'violet'}];
  const arm_levels = ({active = 2} = {}) => `<div class="arm_levels">${ARM_LEVELS.map((l, i) => `<div class="arm_level font_label${i === active ? ' active' : ''}"${i === active ? ` style="color:${color(l.tone)}"` : ''}>
    ${icon(l.glyph, 20)}<span>${esc(l.text)}</span></div>`).join('')}</div>`;
  /**
   * arm_strip (components/home/arm_strip; D19: it never takes the tiles' place): the same warning as a short
   * card for panel_arm(above): the chip, when it arms, who set it, an optional note, and the put-off
   * button (action: {text, glyph, goTo}, or none once it has been put off).
   */
  const arm_strip = ({glyph = 'moon', tone = 'violet', title, detail, note, noteTone = 'amber', action}) => `<div class="arm_strip">
    <div class="strip_top">${icon_chip({glyph, size: 48, font: 28, color: tone})}
      <div class="words">${text(title, 'title')}${text(detail, 'body', 'text_2')}${note ? text(note, 'body', noteTone) : ''}</div></div>
    ${action ? `<div${go(action.goTo)}>${button(action.text, '', action.glyph)}</div>` : ''}</div>`;
  const action_tile = ({glyph, title, subtitle, color: c = 'green', goTo}) => `<div class="action_tile"${go(goTo)}>
    ${icon_chip({glyph, size: 168, font: 96, color: c})}${text(title, 'headline')}${text(subtitle, 'body', 'text_2')}</div>`;

  /**
   * The code pad (components/pad/code_pad). title in the action's tone; hint, or digits typed (dots);
   * error turns the hint red; options: [{text, checked}]; silence: the fire/CO Silence button;
   * close: the X; note: plain words under the title (over the screen); fit: sized by its content.
   */
  function code_pad({title = 'Arm Away', tone = 'rose', glyph = 'log-out', hint = 'Enter your code', digits = 0, error = false,
    options = null, silence = false, close = true, note, fit = false, width, enter = 'check'}) {
    const dotsOn = digits > 0 && !error;
    return `<div class="code_pad${fit ? ' fit' : ''}" style="${width ? `width:${width}px;` : ''}">
      ${close ? `<div class="pad_close">${icon('x', 20)}</div>` : ''}
      <div class="pad_top"><div class="pad_header"><div class="pad_lead">
        ${glyph ? `<div class="pad_icon" style="color:${color(tone)}">${icon(glyph, 28)}</div>` : ''}
        <div class="pad_words${glyph ? '' : ' alone'}" style="max-width:${fit ? 352 : 300}px">
          ${text(title, 'title', tone)}
          ${text(hint, 'body', dotsOn ? null : error ? 'red' : 'text_2', 'hint' + (dotsOn ? ' under_dots' : ''))}
          ${dotsOn ? `<div class="pad_dots">${'<i></i>'.repeat(digits)}</div>` : ''}
        </div></div></div>
        ${note ? text(note, 'caption', 'text_3', 'pad_note') : ''}</div>
      ${options ? `<div class="pad_options">${options.map((o) => `<div class="chip font_label${o.checked ? ' checked' : ''}">${esc(o.text)}</div>`).join('')}</div>` : ''}
      ${silence ? `<div class="pad_silence font_label">${icon('bell-off', 20, 'red')}Silence</div>` : ''}
      <div class="pad_keys">${['1', '2', '3', '4', '5', '6', '7', '8', '9'].map((k) => `<div class="pad_key">${text(k, 'keypad')}</div>`).join('')}
        <div class="pad_key">${icon('delete', 28)}</div><div class="pad_key">${text('0', 'keypad')}</div>
        <div class="pad_key enter" style="background:${color(tone)}">${icon(enter, 28)}</div></div>
    </div>`;
  }

  // ---- lists ----
  const zone_card = ({glyph = 'door-closed', title, detail, tone = 'green', bypass = 'Bypass'}) => `<div class="zone_card">
    ${icon_chip({glyph, size: 56, font: 28, color: tone})}
    <div class="words">${text(title, 'label', null, 'dots')}${text(detail, 'body', 'text_2')}</div>
    ${bypass ? `<div class="zone_bypass font_label">${esc(bypass)}</div>` : ''}</div>`;
  const event_row = ({glyph, title, detail, time = 'Just now', tone = 'gray'}) => `<div class="event_row">
    ${icon_chip({glyph, size: 48, font: 20, color: tone})}
    <div class="row_words" style="min-height:56px">${text(title, 'label')}${detail ? text(detail, 'caption', 'text_2') : ''}</div>
    ${text(time, 'caption', 'text_2')}</div>`;
  const trouble_row = ({glyph = 'zap', title, detail, tone = 'amber'}) => `<div class="trouble_row">
    ${icon_chip({glyph, size: 52, font: 28, color: tone})}
    <div class="row_words" style="min-height:62px">${text(title, 'label')}${text(detail, 'body', 'text_2')}</div></div>`;
  const panic_button = ({glyph, text: t, color: c, held = 0}) => `<div class="panic_button">
    <div class="panic_ring">${held ? ring(212, 10, c, held, false) : ''}
      <div class="panic_disk${held ? ' pressed' : ''}" style="color:${color(c)}">${icon(glyph, 96)}</div></div>
    ${text(t, 'title')}</div>`;

  // ---- settings ----
  // The sections, one line each in the rail's style (BL-131 finding 1): an icon and the name,
  // Installer apart under a line. Event log stays a section until Activity holds the whole log
  // (finding 7). Integrations is a puzzle piece: the house with Wi-Fi is the rail's Smart Home.
  const SECTIONS = [
    {key: 'display', glyph: 'sun', title: 'Screen and sound'},
    {key: 'users', glyph: 'users', title: 'Users'},
    {key: 'wifi', glyph: 'wifi', title: 'Wi-Fi and network'},
    {key: 'time', glyph: 'calendar-clock', title: 'Time and language'},
    {key: 'integrations', glyph: 'puzzle', title: 'Integrations'},
    // Only on a build with home control (HCTL-010). The Event log is Activity's now (BL-131 finding 7).
    {key: 'automations', glyph: 'zap', title: 'Automations', goTo: 'automations'},
    {key: 'walk', glyph: 'footprints', title: 'Walk test'},
    {key: 'about', glyph: 'info', title: 'About'},
    {divider: true},
    {key: 'installer', glyph: 'wrench', title: 'Installer'},
  ];
  /** settings_item: one line, the icon and the name; tag {text, tone}: About's "Update" while one waits. */
  const settings_item = ({glyph, title, active, goTo, tag}) => `<div class="settings_item${active ? ' active' : ''}"${go(goTo)}>
    <span class="nav_glyph">${icon(glyph, 20)}</span>${text(title, 'label', null, 'grow dots')}
    ${tag ? `<span class="nav_tag font_caption" style="color:${color(tag.tone || 'amber')}"><span>${esc(tag.text)}</span></span>` : ''}</div>`;
  /**
   * Settings: the section list beside the open page. section: a SECTIONS key; tags: a section's tag
   * ({text, tone}) in this state; title/note/back: the page's head; body: its rows; pad: a
   * code_pad({...}) in the page's place.
   * sections: the list to draw instead of SECTIONS, for a proposal that adds,
   * moves or drops a section; an item may carry goTo (where it leads) and changed (outlined).
   * M.settings_sections({add: [{after, key, glyph, title, goTo}], drop: ['log']}) makes one.
   */
  function settings({section = 'display', title, note, back = false, busy = false, body = '', pad, tags = {}, sections = SECTIONS}) {
    const nav = `<div class="settings_nav scroll">
      ${sections.map((s) => {
        if (s.divider) return '<div class="settings_divider"></div>';
        const item = settings_item({...s, tag: tags[s.key], active: s.key === section, goTo: s.goTo || `settings-${s.key}`});
        return s.changed ? changed(item) : item;
      }).join('')}</div>`;
    const page = pad
      ? `<div class="settings_page" style="align-items:center">${pad}</div>`
      : `<div class="settings_page">
        <div class="page_head">${back ? `<div class="round_button"${go(typeof back === 'string' ? back : '')}>${icon('chevron-left', 20)}</div>` : ''}
          <div class="col grow" style="gap:2px">${text(title, 'title', null, 'dots')}${note ? text(note, 'body', 'text_2') : ''}</div>
          ${busy ? '<div class="spinner"></div>' : ''}</div>
        <div class="page_body scroll">${body}</div></div>`;
    return `<div class="view" style="gap:var(--space_xl)">${nav}${page}</div>`;
  }
  /** The section list with sections added (each {after: key, key, glyph, title, goTo}; outlined as changed unless mark is false) and dropped. */
  function settings_sections({add = [], drop = [], mark = true} = {}) {
    const out = [];
    for (const s of SECTIONS) {
      if (!s.divider && drop.includes(s.key)) continue;
      out.push(s);
      for (const a of add) if (!s.divider && a.after === s.key) out.push({...a, changed: mark});
    }
    return out;
  }

  // Rows the C code builds (ui_forms.c): the same names as the helpers.
  /** ui_row_link: chip, title, detail, value, trail (a chevron by default). disabled: dimmed, not tappable (LV_STATE_DISABLED). */
  const row_link = ({glyph, color: c = 'blue', title, detail, value, trail = 'chevron-right', goTo, trailColor, disabled = false}) => `<div class="list_row${disabled ? ' disabled' : ''}"${disabled ? '' : go(goTo)}>
    ${glyph ? icon_chip({glyph, size: 44, font: 20, color: c}) : ''}
    <div class="words">${text(title, 'label')}${detail ? text(detail, 'caption', 'text_2') : ''}</div>
    ${value ? text(value, 'label', 'text_2') : ''}${trail ? icon(trail, 20, trailColor || 'text_3') : ''}</div>`;
  /**
   * ui_row_toggle; glyph (mockup-next): a chip, so it lines up with the rows around it that have one.
   * detailColor: a tone for a detail that carries a problem (ui_row_set_detail's
   * tone); disabled: dimmed and not switchable (a record this software can't run).
   */
  const row_toggle = ({glyph, color: c = 'blue', title, detail, on = false, detailColor, disabled = false}) => `<div class="row_switch${glyph ? ' with_chip' : ''}${disabled ? ' disabled' : ''}">
    ${glyph ? icon_chip({glyph, size: 44, font: 20, color: c}) : ''}<div class="words">${text(title, 'label')}${detail ? text(detail, 'caption', detailColor || 'text_2') : ''}</div><div class="switch${on ? ' on' : ''}"></div></div>`;
  /** ui_row_number: minus, value, plus */
  const row_number = ({title, detail, value}) => `<div class="row_stepper">
    <div class="words">${text(title, 'label')}${detail ? text(detail, 'caption', 'text_2') : ''}</div>
    <div class="step_button">${icon('minus', 20)}</div>${text(value, 'label', null, 'value')}<div class="step_button">${icon('plus', 20)}</div></div>`;
  /** ui_row_level: a slider, value 0..100 */
  const row_level = ({glyph, title, value = 50, label}) => `<div class="row_slider">
    <div class="row" style="gap:var(--space_md)">${icon_chip({glyph, size: 44, font: 20, color: 'blue'})}${text(title, 'label', null, 'grow')}${text(label ?? `${value}%`, 'body', 'text_2')}</div>
    <div class="slider"><div class="fill" style="width:${value}%"></div><div class="knob" style="left:${value}%"></div></div></div>`;
  /** ui_row_action: a full-width button tinted in its tone; disabled: dimmed, not tappable */
  const row_action = ({glyph, text: t, color: c = 'blue', goTo, disabled = false}) =>
    `<div class="row_button font_label${disabled ? ' disabled' : ''}" style="color:${color(c)}"${disabled ? '' : go(goTo)}>${icon(glyph, 20)}<span class="text">${esc(t)}</span></div>`;
  /** ui_row_value: a read-only line */
  const row_value = ({title, value}) => `<div class="row_value">${text(title, 'body', 'text_2')}${text(value, 'label', null, 'value')}</div>`;
  /** ui_row_hold: press and hold (held 0..1 shows the fill) */
  const row_hold = ({text: t, held = 0, color: c = 'red'}) => `<div class="hold_button font_label" style="color:${color(c)}">
    ${held ? `<div class="fill" style="width:${held * 100}%"></div>` : ''}<span class="text">${esc(t)}</span></div>`;
  /** ui_section */
  const section = (t) => `<div class="section_title">${text(t, 'caption', 'text_3')}</div>`;
  /** ui_paragraph: gray (ui_tone_gray) or a tone */
  const paragraph = (t, c = 'gray') => `<div class="paragraph">${text(t, 'body', c === 'gray' ? 'text_2' : c)}</div>`;

  // ---- sheets over the screen ----
  const button = (t, kind = '', glyph, glyphAfter) =>
    `<div class="style_button font_label ${kind}">${glyph ? icon(glyph, 20) : ''}${esc(t)}${glyphAfter ? icon(glyphAfter, 20) : ''}</div>`;
  /**
   * confirm_dialog: title, explanation, Cancel and the red confirm. tone: 'primary'
   * for a confirm that destroys nothing (an attestation: "Use it"), blue instead of red.
   */
  const confirm_dialog = ({title, note, ok = 'Done', cancel = 'Cancel', tone = 'danger'}) => `<div class="style_backdrop">
    <div class="sheet_card centered" style="width:560px">${text(title, 'title')}${note ? text(note, 'body', 'text_2') : ''}
      <div class="row" style="justify-content:flex-end;gap:var(--space_md);margin-top:var(--space_md)">${button(cancel)}${button(ok, tone === 'primary' ? 'primary' : 'danger')}</div></div></div>`;
  /** pick_sheet: a list of choices; rows are row_link()s (they sit on the card's own colour).
   * fit (mockup-next): the card is as tall as its choices, up to 520 px. */
  const pick_sheet = ({title, note, rows, fit = false}) => `<div class="style_backdrop">
    <div class="sheet_card centered" style="width:600px;${fit ? 'max-height:520px' : 'height:520px'}">
      <div class="row" style="gap:var(--space_md)"><div class="col grow" style="gap:2px">${text(title, 'title', null, 'dots')}${note ? text(note, 'body', 'text_2') : ''}</div>
        <div class="round_button t2">${icon('x', 20)}</div></div>
      <div class="list scroll"${fit ? ' style="flex:0 1 auto"' : ''}>${rows}</div></div></div>`;
  const KB = {
    letters: [['q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'], ['a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l'],
      [{k: 'arrow-big-up', w: 1.4, s: 1}, 'z', 'x', 'c', 'v', 'b', 'n', 'm', {k: 'delete', w: 1.4, s: 1}],
      [{t: '123', w: 1.4, s: 1}, {t: 'àé', w: 1, s: 1}, {t: 'space', w: 3.9}, '-', '.', {k: 'corner-down-left', w: 1.4, s: 1}]],
    // the number page (ui_forms.c MAP_NUMBERS): a text sheet of kind number opens on it
    numbers: [['1', '2', '3', '4', '5', '6', '7', '8', '9', '0'], ['-', '/', ':', ';', '(', ')', '$', '&', '@', '"'],
      [{t: '#+=', w: 1.4, s: 1}, '.', ',', '?', '!', "'", '_', '*', {k: 'delete', w: 1.4, s: 1}],
      [{t: 'abc', w: 1.4, s: 1}, '-', {t: 'space', w: 5}, '.', {k: 'corner-down-left', w: 1.4, s: 1}]],
    // the accent layer's first page (ui_forms.c MAP_ACCENT1_LOWER, I18N-019), from the letters' àé key
    accent: [['à', 'á', 'â', 'ä', 'ã', 'å', 'æ', 'ç', 'è', 'é'], ['ê', 'ë', 'ì', 'í', 'î', 'ï', 'ñ', 'ò', 'ó', 'ô'],
      [{k: 'arrow-big-up', w: 1.4, s: 1}, 'õ', 'ö', 'ø', 'ù', 'ú', 'ü', 'ß', {k: 'delete', w: 1.4, s: 1}],
      [{t: 'abc', w: 1.4, s: 1}, {t: 'čł', w: 1, s: 1}, {t: 'space', w: 3.9}, '-', '.', {k: 'corner-down-left', w: 1.4, s: 1}]],
  };
  // the letters without the àé key: the space bar takes its place (a panel without the letters pack)
  KB.plain = [...KB.letters.slice(0, 3),
    [{t: '123', w: 1.4, s: 1}, {t: 'space', w: 4.9}, '-', '.', {k: 'corner-down-left', w: 1.4, s: 1}]];
  /**
   * text_sheet: title, note, the field and the keyboard. password shows dots, and the field ends
   * in an eye that shows what was typed (BL-131); shown: the password in the clear; numbers: opens on
   * the number page (a field of kind number: addresses, install codes, ports). page: the keyboard's
   * page when not the letters ('numbers', 'accent', 'plain': the letters without the àé key);
   * changedRow: the keyboard row a proposal changes, outlined.
   */
  function text_sheet({title, note, value = '', ok = 'Done', password = false, shown = false, numbers = false,
    page = numbers ? 'numbers' : 'letters', changedRow = -1}) {
    const keys = KB[page].map((r, i) => {
      const row = `<div class="kb_row" style="${i === 1 && (page === 'letters' || page === 'plain') ? 'padding:0 5%' : ''}">${r.map((k) => {
        const o = typeof k === 'string' ? {t: k} : k;
        return `<div class="kb_key font_label${o.s ? ' special' : ''}" style="flex-grow:${o.w || 1}">${o.k ? icon(o.k, 20) : esc(o.t)}</div>`;
      }).join('')}</div>`;
      return i === changedRow ? `<div class="changed kb_row" style="display:flex">${row}</div>` : row;
    }).join('');
    return `<div class="style_backdrop"></div><div class="text_sheet"><div class="text_card">
      <div class="row" style="gap:var(--space_md)"><div class="col grow" style="gap:2px">${text(title, 'title', null, 'dots')}${note ? text(note, 'body', 'text_2', 'dots') : ''}</div>
        ${button('Cancel')}${button(ok, 'primary')}</div>
      <div class="text_field">${text(password && !shown ? '•'.repeat(value.length) : value, 'title')}<i class="cursor"></i>
        ${password ? `<div class="field_eye">${icon(shown ? 'eye-off' : 'eye', 20)}</div>` : ''}</div>
      <div class="keyboard">${keys}</div></div></div>`;
  }
  /** modal_pad: the code pad over the screen for chime, bypass, acknowledging (UI-025) */
  const modal_pad = (pad) => `<div class="style_backdrop"><div class="centered">${code_pad({fit: true, width: 480, ...pad})}</div></div>`;
  /** A toast; kb: the keyboard is open, so it sits at the top; wiz: on the setup's left side */
  const toast = (t, where = '') => `<div class="toast font_label ${where}">${esc(t)}</div>`;

  // ---- pickers (BL-131 finding 3) ----
  /**
   * roller: LVGL's lv_roller, one column of choices that scrolls under a fixed band; the band
   * holds the choice. items: the five rows on show, the middle one chosen; label above it.
   */
  const roller = ({label, items, width = 96}) => `<div class="roller_col" style="width:${width}px">
    ${label ? text(label, 'caption', 'text_2') : ''}
    <div class="roller"><div class="roller_band"></div>${items.map((t, i) => `<div class="roller_item ${['far', 'near', 'sel', 'near', 'far'][i] || 'far'}">${text(t, i === 2 ? 'title' : 'label')}</div>`).join('')}</div></div>`;
  /**
   * calendar: LVGL's lv_calendar with its arrow header. month: 'September 2026'; first: the column
   * (0 = Sunday) of the 1st; days: how many; sel: the chosen day; today: the panel's day.
   */
  function calendar({month = 'September 2026', first = 2, days = 30, sel = 30, today = 0, week = ['S', 'M', 'T', 'W', 'T', 'F', 'S'], width = 392}) {
    const cells = [];
    for (let i = 0; i < first; i++) cells.push('<div class="cal_day"></div>');
    for (let d = 1; d <= days; d++) cells.push(`<div class="cal_day${d === sel ? ' sel' : ''}${d === today ? ' today' : ''}">${text(d, 'label')}</div>`);
    while (cells.length % 7) cells.push('<div class="cal_day"></div>');
    return `<div class="calendar" style="width:${width}px">
      <div class="row cal_head"><div class="round_button">${icon('chevron-left', 20)}</div>
        <div class="grow center row" style="gap:var(--space_xs)">${text(month, 'label')}${icon('chevron-down', 20, 'text_3')}</div>
        <div class="round_button">${icon('chevron-right', 20)}</div></div>
      <div class="cal_grid">${week.map((w) => `<div class="cal_week">${text(w, 'caption', 'text_3')}</div>`).join('')}${cells.join('')}</div></div>`;
  }
  /** A sheet over the screen with a title, its content and Cancel / OK (pick_sheet's card). */
  const sheet = ({title, note, body, ok = 'Done', cancel = 'Cancel', width = 880, height}) => `<div class="style_backdrop">
    <div class="sheet_card centered" style="width:${width}px;${height ? `height:${height}px;` : ''}">
      <div class="row" style="gap:var(--space_md)"><div class="col grow" style="gap:2px">${text(title, 'title', null, 'dots')}${note ? text(note, 'body', 'text_2') : ''}</div>
        ${button(cancel)}${button(ok, 'primary')}</div>
      ${body}</div></div>`;
  /**
   * time_rollers (components/pickers/time_rollers): Hour, Minute and AM/PM. Each is the five rows on
   * show, the middle one chosen; ampm: null on a 24-hour clock.
   */
  const time_rollers = ({hour, minute, ampm, width = 88, ampmWidth = 80}) => `<div class="time_rollers">${roller({label: 'Hour', items: hour, width})}${roller({label: 'Minute', items: minute, width})}${ampm ? roller({items: ampm, width: ampmWidth}) : ''}</div>`;
  /** day_toggle (components/pickers/day_toggle): one day of the week, 64 px round; on: filled blue. */
  const day_toggle = ({text: t, on = false}) => `<div class="day_toggle font_label${on ? ' on' : ''}">${esc(t)}</div>`;
  /**
   * time_sheet (components/sheets/time_sheet, modelled on night_sheet): one time of
   * day (time_rollers) beside seven day_toggles in the region's week order, the result line under
   * them, Cancel / OK. days: [{text, on}]; result: {label, value, tone} (no label: value is the whole
   * sentence, "Runs on weekdays at 11:00 PM"); okDisabled: no day picked.
   */
  const time_sheet = ({title, note, rollers, days, result, ok = 'Done', cancel = 'Cancel', okDisabled = false}) => `<div class="style_backdrop">
    <div class="sheet_card centered" style="width:880px">
      <div class="row" style="gap:var(--space_md)"><div class="col grow" style="gap:2px">${text(title, 'title', null, 'dots')}${note ? text(note, 'body', 'text_2') : ''}</div>
        ${button(cancel)}${button(ok, okDisabled ? 'primary disabled' : 'primary')}</div>
      <div class="time_sheet_body">${time_rollers(rollers)}
        <div class="col" style="gap:var(--space_sm)">${text('Days', 'caption', 'text_2')}<div class="day_row">${days.map(day_toggle).join('')}</div></div></div>
      <div class="result_line">${result.label ? text(result.label, 'body', 'text_2') : ''}${text(result.value, 'label', result.tone)}</div></div></div>`;

  // ---- whole screens ----
  /**
   * The main screen (screen_main.xml): rail, status bar, the content of one view, and anything
   * over it (a sheet, a modal pad, a toast). rail/bar take the props of rail() and bar().
   */
  function main({view = 'home', tone = 'green', alarm = false, rail: r = {}, bar: b = {}, content = '', over = '', toast: t}) {
    return `<div class="screen${alarm ? ' alarm' : ''}">
      ${rail({view, tone, ...r})}${bar({settings: view === 'settings', view, locked: !!r.locked, ...b})}
      <div class="content">${content}</div>${over}${t ? toast(t) : ''}</div>`;
  }
  /** Home: the state card beside the arming tiles, a pad or an action tile. */
  const home = (heroProps, pane) => `<div class="view" style="gap:var(--space_xl)">${hero(heroProps)}<div class="pane">${pane}</div></div>`;
  /** big_value: one value to read out or compare, large (a pairing code) */
  const big_value = ({label, value, note}) => `<div class="big_value">${label ? text(label, 'body', 'text_2') : ''}
    ${text(value, 'headline')}${note ? text(note, 'caption', 'text_2') : ''}</div>`;
  /** Two row_actions side by side, each half the width. */
  const action_pair = (a, b) => `<div class="action_pair"><div class="grow">${row_action(a)}</div><div class="grow">${row_action(b)}</div></div>`;
  /**
   * Smart Home (docs/features/24, BL-045; copied from mockup-next's kit): a device's tile, its icon
   * tinted by state (tone; gray when off). A tap does its main action; more: the arrow to its detail
   * sheet. wide: two cells, with a control under the name (device_level, device_stepper,
   * device_keys). pending: an action is on its way, so the chip dims (nothing moves); offline: the
   * tile stays readable, the words say since when, the chip dims to 70 % and there is no arrow
   * (BL-181; a control passed in rests at 40 % on its own, class "disabled"); stateTone: the words'
   * colour (amber when an action failed).
   * arrange: the screen is being arranged, so the tile shows a grip, a dashed edge and its control
   * dimmed (a tap picks it, it doesn't switch anything); selected: the one picked, edged in blue.
   */
  const device_tile = ({glyph, tone = 'gray', name, state, stateTone, more = true, wide = false, control = '', pending = false, offline = false,
    arrange = false, selected = false}) => {
    const chip = icon_chip({glyph, size: 48, font: 28, color: tone});
    // BL-184: the name one line and dots; two on a wide tile without a control under it
    const words = `<div class="words">${text(name, 'label', null, wide && !control ? 'two_lines' : 'dots')}${text(state, 'caption', stateTone || (tone === 'amber' && !pending ? 'amber' : 'text_2'), 'dots')}</div>`;
    const corner = arrange ? `<div class="grip">${icon('grip-vertical', 20)}</div>`
      : more && !offline ? `<div class="more">${icon('chevron-right', 20)}</div>` : '';
    return `<div class="${cls('device_tile', wide && 'wide', pending && 'pending', offline && 'offline', arrange && 'arrange', selected && 'selected')}">
      ${wide ? `<div class="top">${chip}${words}</div>${control}` : chip + words}${corner}</div>`;
  };
  /** A wide tile's level (a thick lv_slider): drag it to set the brightness. value 0..100 */
  const device_level = ({value = 0, tone = 'blue', glyph = 'sun'}) => `<div class="device_level" style="color:${color(tone)}">
    <div class="fill" style="width:${value}%"></div>${icon(glyph, 20, 'text_2')}${value ? `<i class="edge" style="left:${value}%"></i>` : ''}</div>`;
  /** A wide tile's stepper: minus, what the set point does and its value, plus */
  const device_stepper = ({label, value}) => `<div class="device_stepper">
    <div class="step_button">${icon('minus', 20)}</div>
    <div class="grow row center" style="gap:var(--space_sm)">${text(label, 'body', 'text_2')}${text(value, 'title')}</div>
    <div class="step_button">${icon('plus', 20)}</div></div>`;
  /** A row of keys: a cover's Open, Stop and Close, a fan's speeds. keys: 'Text' or {glyph, text}; active: the lit one */
  const device_keys = ({keys, active = -1, tone = 'blue'}) => `<div class="device_keys">${keys.map((k, i) => {
    const o = typeof k === 'string' ? {text: k} : k;
    return `<div class="device_key font_label${i === active ? ' active' : ''}"${i === active ? ` style="color:${color(tone)}"` : ''}>
      ${o.glyph ? icon(o.glyph, 20) : ''}${o.text ? `<span>${esc(o.text)}</span>` : ''}</div>`;
  }).join('')}</div>`;
  /** A thermostat's set point in its sheet: large, between a big minus and plus (device_setpoint.xml) */
  const device_setpoint = ({label, value, tone}) => `<div class="device_setpoint">
    <div class="step_button big">${icon('minus', 28)}</div>
    <div class="col center grow">${text(label, 'body', 'text_2')}${text(value, 'headline', tone)}</div>
    <div class="step_button big">${icon('plus', 28)}</div></div>`;
  /**
   * device_sheet: a device's detail sheet (HCTL-004; device_sheet.xml), its chip, name and state at
   * the top and an X to close it. A change applies at once, so there is no OK. on: a switch beside the name.
   * offline (BL-181): the chip at 70 % and the switch shown off, resting at 40 %; the rest as it is.
   */
  const device_sheet = ({glyph, tone = 'gray', title, note, on, body, width = 600, offline = false}) => `<div class="style_backdrop">
    <div class="${cls('sheet_card centered', offline && 'offline')}" style="width:${width}px">
      <div class="row" style="gap:var(--space_md);align-items:flex-start">${icon_chip({glyph, size: 48, font: 28, color: tone})}
        <div class="col grow" style="gap:0">${text(title, 'title', null, 'wrap_all')}${note ? text(note, 'body', 'text_2', 'wrap_all') : ''}</div>
        ${on != null ? `<div class="switch${on && !offline ? ' on' : ''}" style="margin-top:6px"></div>` : ''}<div class="round_button t2" style="margin-top:2px">${icon('x', 20)}</div></div>
      ${body}</div></div>`;
  /** The screen saver (screen_saver.xml). */
  const saver = ({tone = 'green', glyph = 'shield-check', headline = 'Disarmed', detail = 'Ready to arm', note = 'All zones closed',
    time = '10:50', ampm = 'PM', date = 'Saturday, September 19', trouble}) => `<div class="screen saver">
    <div class="saver_bg"></div><div class="saver_shade"></div>
    <div class="saver_clock"><div class="row" style="align-items:flex-end;gap:var(--space_sm)">${text(time, 'digits')}<span class="font_title soft" style="padding-bottom:14px">${esc(ampm)}</span></div>
      <span class="font_title soft">${esc(date)}</span></div>
    <div class="saver_state"><div class="saver_disk" style="color:${color(tone)}">${icon(glyph, 40)}</div>
      <div class="col" style="gap:2px;max-width:400px">${text(headline, 'headline', tone)}${text(detail, 'label')}<span class="font_body soft">${esc(note)}</span>
        ${trouble ? `<div class="row warn" style="gap:var(--space_sm);margin-top:var(--space_sm)">${icon('triangle-alert', 20)}${text(trouble, 'label')}</div>` : ''}</div></div></div>`;
  /**
   * The first-boot setup (screen_wizard.xml). step 0..steps; body: rows; pad: a code_pad in its place.
   * steps (BL-048 proposal): how many progress dots, 5 today; a branch of the setup (the alarm panel's,
   * a keypad's) has its own count. nextDisabled (BL-048 proposal): Next dimmed until the step is done.
   */
  function wizard({step = 0, steps = 5, glyph = 'sparkles', title, note, body = '', pad, next = 'Start', nextDisabled = false,
    back = false, exit = false, toast: t, leaveSide = false}) {
    // leaveSide (mockup-next): "Leave setup" is a quiet link under the step's words, away from Back and Next.
    const footer = pad ? '' : `<div class="wiz_footer">${exit && !leaveSide ? button('Leave setup', '', 'log-out') : ''}${back ? button('Back', '', 'chevron-left') : ''}
      <div class="grow"></div>${button(next, nextDisabled ? 'primary disabled' : 'primary', null, 'chevron-right')}</div>`;
    const leave = exit && leaveSide ? `<div class="grow"></div><div class="row wiz_leave">${icon('log-out', 20)}${text('Leave setup', 'label')}</div>` : '';
    return `<div class="screen">
      <div class="wiz_side"><div class="wiz_steps">${Array.from({length: steps}, (_, k) => k + 1).map((i) => `<i class="${step >= i ? 'on' : ''}"></i>`).join('')}</div>
        ${icon_chip({glyph, size: 168, font: 96, color: 'blue'}).replace('class="icon_chip"', 'class="icon_chip wiz_disk"')}
        ${text(title, 'headline', null, '')}${text(note, 'body', 'text_2')}${leave}</div>
      <div class="wiz_main">${pad ? `<div class="col grow" style="align-items:center">${pad}</div>` : `<div class="page_body scroll">${body}</div>`}${footer}</div>
      ${t ? toast(t, 'wiz') : ''}</div>`;
  }

  /**
   * Register a screen. id: unique, kebab-case; area: the gallery's group; title: what it shows;
   * shot: the PC preview's picture of the same state, without its number ('home_ready' for
   * build/ui_preview/shots/01_home_ready.png); notes: what to know. A proposal adds
   * proposal: {bl: 'BL-NNN', status: 'draft'|'approved'|'rejected'|'built', replaces: '<id>', why: '...'}.
   */
  function screen(s) { screens.push(s); }

  /** lv_bar: a level to read, not a control (no knob). value 0..100 */
  const level_bar = ({value = 50, tone = 'blue'}) =>
    `<div class="level_bar"><div class="fill" style="width:${value}%;background:${color(tone)}"></div></div>`;
  /**
   * row_status: the state of a page's subject at its top (a card, an integration, an update):
   * chip, the state in words, a detail, and optionally a bar under them. Not tappable.
   */
  const row_status = ({glyph, color: c = 'green', title, detail, level, levelTone}) => `<div class="list_row row_status">
    ${icon_chip({glyph, size: 44, font: 20, color: c})}
    <div class="words">${text(title, 'label')}${detail ? text(detail, 'caption', 'text_2') : ''}
      ${level != null ? level_bar({value: level, tone: levelTone || c}) : ''}</div></div>`;
  /** Wrap what a proposal changes, so the reviewer sees it at a glance. */
  const changed = (html) => `<div class="changed">${html}</div>`;

  /**
   * row_link_alert (BL-144): ui_row_link whose words carry a state or a problem, as the C code
   * colours them after building the row: the detail through ui_row_set_detail(row, text, true,
   * tone), the value through its label's colour (ui_zwave.c set_words). detailColor, valueColor: a
   * tone; sub: a second line under the detail, in subColor (amber), for a device's warning
   * ("Not heard for 2 h 30 min", "Battery low").
   */
  const row_link_alert = ({glyph, color: c = 'blue', title, detail, detailColor, sub, subColor = 'amber', value, valueColor,
    trail = 'chevron-right', goTo, trailColor}) => `<div class="list_row"${go(goTo)}>
    ${glyph ? icon_chip({glyph, size: 44, font: 20, color: c}) : ''}
    <div class="words">${text(title, 'label')}${detail ? text(detail, 'caption', detailColor || 'text_2') : ''}${sub ? text(sub, 'caption', subColor) : ''}</div>
    ${value ? text(value, 'label', valueColor || 'text_2') : ''}${trail ? icon(trail, 20, trailColor || 'text_3') : ''}</div>`;
  return {
    screen, screens, missing, esc, icon, text, raw, color, changed, TONES, SECTIONS,
    icon_chip, badge, view_header, segments, rail, bar, ring, hero, arm_tile, panel_arm, action_tile, code_pad,
    zone_card, event_row, trouble_row, panic_button, settings_item, settings, settings_sections,
    time_rollers, day_toggle, time_sheet, arm_levels, arm_strip,
    row_link, row_toggle, row_number, row_level, row_action, row_value, row_hold, section, paragraph, big_value, action_pair, roller, calendar, sheet, level_bar, row_status, device_tile, device_level, device_stepper, device_keys, device_setpoint, device_sheet,
    button, confirm_dialog, pick_sheet, text_sheet, modal_pad, toast, main, home, saver, wizard,
    row_link_alert,
  };
})();
