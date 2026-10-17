// BL-048 proposal P2: keypad updates through the panel link (docs/features/11 "Updates through the
// link", LINK-029..042, OTA-026, OTA-027, OTA-030, OTA-032; the four choices decided 2026-10-16 under
// Decisions, "Keypad updates"). The master decides when its keypads update and to which version (the
// one it runs, once its own update passed its self-test); a keypad that reaches the update server
// downloads its own image, one that can't gets it from the master over the link; each installs only at
// the master's order, one at a time, lowest number first, and only when everything is disarmed. A
// keypad found behind later (newly paired, or back on line) waits for Update keypads (decision 2).
//
// Drawn: A. the master's Software update with its Keypads block (one row per keypad: name, number,
// version and the state in words, in its tone), Update keypads and Stop (each with its paragraph under
// it, as today's Install and Stop), the master's own install put off while a keypad self-tests, the
// install question, the keypads list in Installer and Activity's rows; B. a keypad's About and
// Software update ("Updates come from Harbor House", the run, paused, waiting for its turn, the Stop
// question, stopped, failed, rolled back) and its Home banner; C. a new signing key accepted at the
// keypad with the installer's code (LINK-036).
//
// One cast, as the built screens (screens/panel-link.js) and the README's fictional data: the master is
// Harbor House; its keypads are Hall keypad (2, a 7" panel without internet: it receives from Harbor
// House; the keypad screens in B and C are its), Bedroom keypad (3, a 7" panel that reaches the update
// server: it downloads its own), Kitchen (4, a 10.1" panel: another model, the first of its kind;
// without internet) and Garage (5, on a link that can't carry updates, LINK-037; drawn only on the two
// screens about it, behind and ended: it is the last row of every list). Activity and the ended page
// are two different mornings. A keypad screen's replaces names the page a keypad shows today (any
// panel's); the master keeps that page.
//
// Words: a name (the master's or a keypad's, up to 47 bytes) never goes in a status-bar banner or a
// step's title, which are cut short; it goes in a detail line or a paragraph, whose TR comment counts
// 47 bytes for it. Nothing moves or blinks: words, steady tones and the stepped bar of today's run;
// every percent changes at most once a second (UI-020, UI-023). The Panel Manager's side is its own
// proposal (docs/features/31 mockup).
//
// Questions for the owner, each also in the notes of the screen it concerns, with what this proposal
// recommends:
//  Q1  Keypad rows: three lines and no bar of their own (recommended) or a bar under the one transferring.
//  Q2  Update keypads asks no question (recommended), unlike Install.
//  Q3  The master's Install question has a note of its own when it has keypads: "... then its 3 keypads
//      update." (recommended).
//  Q4  No banner on the master while only its keypads update; About's row stays (recommended).
//  Q5  Installer > Keypads and devices says "Updating" for a keypad installing (recommended; not in the spec).
//  Q6  Activity names the keypad by number, "Keypad 3 updated" (recommended until a shared name lookup).
//  Q7  The installer's code at the key hold only when Software update was opened with another code (recommended).
//  Q8  A key accepted at a keypad is logged on the master, "Keypad 2 accepted a new signing key"
//      (recommended; a new event stage, so a spec change).
//  Q9  A keypad's banner while it waits for the key (recommended), so the installer finds the page there.
//  Q10 The master by its name in a keypad's words, "Updates come from Harbor House" (recommended), not "the
//      alarm panel" (each such sentence says "the alarm panel" otherwise); approving it also changes
//      docs/features/11's READY line and OTA-026's "Waiting for the alarm panel" to "Waiting for <master>".
//  Q11 A keypad that can't be updated through the link keeps its row (the spec has one row per keypad, so
//      it is never left out), saying "Update it at the keypad: Settings > About" (recommended).
//  Q12 One Stop for all keypads on the screen (recommended); one keypad alone only at the console or at that keypad.
//  Q13 Settled by LINK-030, no longer a question: after the first keypad of a model went back, Update keypads
//      starts with it again (the lowest number first).
//  Q14 The two waiting banners, decided together: the master's "Update waiting · installs once the keypad is
//      back" and a keypad's "Update paused · goes on by itself" (both recommended: neither says "once
//      disarmed" while everything is disarmed).

const P2 = (replaces, why) => ({bl: 'BL-048', status: 'draft', ...(replaces ? {replaces} : {}), why});
// A page drawn scrolled down by y pixels (the viewer and the bare view honour data-scroll).
const scrolledTo = (html, y) => (y ? html.replace('class="page_body scroll"', `class="page_body scroll" data-scroll="${y}"`) : html);
// Every update banner this proposal draws has new words: outline it as changed.
const newBanner = (html) => html.replace('class="bar_banner', 'class="changed bar_banner');
// A confirm dialog whose note has new words: outline the note (its only body text).
const changedNote = (html) => html.replace('<span class="font_body"', '<span class="changed font_body"');

// The cast (see above).
const HALL = 'Hall keypad';
const BEDROOM = 'Bedroom keypad';
const KITCHEN = 'Kitchen';

// ================================ A. The master: Software update ================================
// The Settings list as BL-189 has it (Installer opens from About), as screens/updates-run.js.
const MS_SECTIONS = M.settings_sections({drop: ['installer']}).filter((s) => !s.divider);
const MS_BAR = {banner: null, time: '7:31', ampm: 'AM'};
const msUpd = ({id, title, notes, proposal, body, bar = MS_BAR, tags = {}, over, scroll = 0}) => M.screen({
  id, area: 'Settings', title, shot: null, proposal, notes,
  render: () => newBanner(scrolledTo(M.main({view: 'settings', rail: {}, bar, over,
    content: M.settings({section: 'about', sections: MS_SECTIONS, title: 'Software update', back: 'settings-about', tags, body: body.join('')})}), scroll)),
});

// One keypad's row: its name; its number and the version it runs (none while not known); its state
// in words, in the state's tone (green done, blue running, amber waiting on someone or held, red
// failed, gray idle, violet can be updated). Not tappable: Update keypads and Stop act on all of them.
const kpRow = (name, n, version, state, tone = 'gray') => M.row_link_alert({glyph: 'keyboard', color: tone, title: name,
  detail: version ? `Keypad ${n} · version ${version}` : `Keypad ${n}`, sub: state, subColor: tone === 'gray' ? 'text_2' : tone, trail: ''});
const GARAGE_ROW = kpRow('Garage', 5, '0.6.0', 'Update it at the keypad: Settings > About', 'amber');
const KP_SECTION = M.section('Keypads');
const UPDATE_KEYPADS = M.row_action({glyph: 'download', text: 'Update keypads', color: 'blue'});
const STOP_KEYPADS = M.row_action({glyph: 'circle-x', text: 'Stop updating keypads', color: 'red', goTo: 'bl-048-fw-master-stop-question'});
const ONE_AT_A_TIME = M.paragraph('Keypads restart one at a time, once everything is disarmed. The alarm keeps working.');
const MS_UPDATED = (v = '0.6.2') => M.row_status({glyph: 'circle-check', color: 'green', title: `Updated to version ${v}`, detail: '7:14 AM · it passed its self-test'});
const MS_UP_TO_DATE = M.row_status({glyph: 'circle-check', color: 'green', title: 'Up to date', detail: 'Version 0.6.2'});
const MS_CHECK = [M.section(''), M.row_link({glyph: 'refresh-cw', title: 'Check again', detail: 'Checked 7:14 AM', trail: ''})].join('');
const block = (...rows) => M.changed(rows.join(''));

msUpd({id: 'bl-048-fw-master-behind', title: 'Master: Software update, a keypad behind and Update keypads',
  proposal: P2('settings-update-updated', 'The installer or system master sees, on the master, which keypads run which version and brings the ones behind to the master\'s version in one tap.'),
  notes: `The Keypads block, under this panel's own state (and its Install, when one is offered), above Check again; only on a master with keypads (link_keypads), one row per keypad in number order. The rows follow the keypads once a second (ui_link_keypad_t gains fw_state, fw_why, fw_path, fw_percent, fw_behind). "Can be updated to 0.6.2" (violet, as an offered version): a keypad found behind later, newly paired or back on line, waits for Update keypads (decision 2, until BL-190 decides automatic updates). "Version not known yet" (gray, the UNKNOWN state, no version on the line above): a keypad not heard since this panel started; once heard, its version and state. Update keypads shows while a keypad is behind, stopped or not updated, no rollout runs and this panel runs no update of its own; the paragraph names the keypad, or counts them (TRN: "Brings 2 keypads to 0.6.2, the version this panel runs."). It acts at once (Q2: no question, recommended: nothing restarts before each keypad's turn, and Stop undoes it, WORD-005). Garage is on a link that can't carry updates (433 MHz, LINK-037, later); until such a link exists, this row is seen only in development, with a keypad whose firmware is from before keypad updates (its session lacks PLINK_CAP_FW). Such a keypad updates itself as a stand-alone panel does, with its own Install (LINK-029), and Update keypads doesn't count it. Q11: its row stays, as the spec has one row per keypad (leaving it out would contradict that), saying "Update it at the keypad: Settings > About" in amber (recommended). Q1: three lines per row (name; number and version; state), the percent in words and no bar per row (recommended: rows of one height; the bar stays this panel's own). Drawn scrolled to the Keypads block; Garage is the next row down (bl-048-fw-master-ended shows it).`,
  body: [MS_UP_TO_DATE,
    block(KP_SECTION, UPDATE_KEYPADS, M.paragraph('Brings Hall keypad to 0.6.2, the version this panel runs. It restarts by itself once everything is disarmed.'),
      kpRow(HALL, 2, '0.6.1', 'Can be updated to 0.6.2', 'violet'),
      kpRow(BEDROOM, 3, '0.6.2', 'Up to date', 'green'),
      kpRow(KITCHEN, 4, null, 'Version not known yet', 'gray'),
      GARAGE_ROW),
    MS_CHECK], scroll: 88});

msUpd({id: 'bl-048-fw-master-running', title: 'Master: keypads updating, one receiving from this panel, one downloading',
  proposal: P2('settings-update-updated', 'Right after the master\'s own update passed its self-test, its keypads follow without another tap; the page says how each one gets it.'),
  notes: `The rollout an Install on the master started, once its own update was confirmed (that Install covers its keypads). Hall keypad can't reach the update server: this panel sends it its own image (RELAY, one keypad at a time): "Receiving from this panel · 45 %". Bedroom keypad reaches it and downloads its own image (FETCH, all such keypads at once): "Downloading · 72 %". Kitchen, also without internet, waits: "Waiting for its turn to receive it" (another model: this panel fetches a copy for it). After a download: "Checking the download". Before an order is answered: "Asking the update server" (PROBE). A keypad silent for 30 s: "Not responding · updates when it's back" (the rollout waits up to 24 h for it). Stop updating keypads replaces Update keypads while the rollout runs (next screens). The percent at most once a second; the words only when the state changes. Q4: no status-bar banner on the master while only its keypads update, and About's row stays "Up to date" (recommended: the master doesn't restart; the keypad that installs shows its own banner). The order (lowest number first) is the rows' order. Drawn scrolled to the Keypads block.`,
  body: [MS_UPDATED(),
    block(KP_SECTION, STOP_KEYPADS, ONE_AT_A_TIME,
      kpRow(HALL, 2, '0.6.1', 'Receiving from this panel · 45 %', 'blue'),
      kpRow(BEDROOM, 3, '0.6.1', 'Downloading · 72 %', 'blue'),
      kpRow(KITCHEN, 4, '0.6.1', 'Waiting for its turn to receive it', 'gray')),
    MS_CHECK], scroll: 88});

msUpd({id: 'bl-048-fw-master-installing', title: 'Master: one keypad on its self-test, the next ready',
  proposal: P2('settings-update-updated', 'One keypad installs or self-tests at a time, so a bad version never takes every keypad at once.'),
  notes: `One keypad installs or self-tests at a time, the lowest number first (LINK-030). From the order until the keypad answers with the new version: "Installing · back in about a minute"; then "Self-test · usually a minute, at most three" (its version is the new one, on probation); then "Updated · it passed its self-test" and the next ready keypad goes. Meanwhile its supervision trouble isn't raised (up to 10 min) and this panel puts off its own restart (bl-048-fw-master-own-waits). Stop here stops the keypads not installing yet (Kitchen); Bedroom keypad's self-test decides by itself (OTA-027). Drawn scrolled to the Keypads block.`,
  body: [MS_UPDATED(),
    block(KP_SECTION, STOP_KEYPADS, ONE_AT_A_TIME,
      kpRow(HALL, 2, '0.6.2', 'Updated · it passed its self-test', 'green'),
      kpRow(BEDROOM, 3, '0.6.2', 'Self-test · usually a minute, at most three', 'blue'),
      kpRow(KITCHEN, 4, '0.6.1', 'Ready · waiting for its turn', 'blue')),
    MS_CHECK], scroll: 88});

msUpd({id: 'bl-048-fw-master-waits', title: 'Master: the keypads wait, another partition is armed',
  proposal: P2('settings-update-updated', 'Keypads never restart, and the master never sends an image, while anything is armed or busy: the row says what they wait for.'),
  notes: `Arming this panel's own partition closes Settings, as today, so this page shows it while another partition is armed (or the master is busy otherwise). A keypad ready to install: "Ready · waits: the system is armed"; a keypad receiving from this panel pauses (the link stays for the alarm): "Receiving from this panel · paused at 60 %". The reasons (the why's 50s, LINK-034), as the master's own: "the system is armed", "an alarm is sounding", "installer mode is on", "a walk test is running", "this panel is busy" (MASTER_BUSY 50); the keypad's own: "a panic is being sent from it", "its fire or CO alarm still sounds", "it can't hear this panel · check its Wi-Fi" (NO_STATUS), "it is busy" (BUSY 55: another update runs on it); a value this panel doesn't know: "Ready · waits". A keypad downloading its own image goes on. A relay paused 30 min ends as "Not updated: it waited too long". Amber: it waits on something, nothing is wrong. Drawn scrolled to the Keypads block.`,
  body: [MS_UPDATED(),
    block(KP_SECTION, STOP_KEYPADS, ONE_AT_A_TIME,
      kpRow(HALL, 2, '0.6.2', 'Updated · it passed its self-test', 'green'),
      kpRow(BEDROOM, 3, '0.6.1', 'Ready · waits: the system is armed', 'amber'),
      kpRow(KITCHEN, 4, '0.6.1', 'Receiving from this panel · paused at 60 %', 'amber')),
    MS_CHECK], scroll: 88});

msUpd({id: 'bl-048-fw-master-ended', title: 'Master: the keypads\' update ended, some not updated',
  proposal: P2('settings-update-updated', 'Each keypad left behind says why in one line, and what to do where there is something to do.'),
  notes: `The rollout ended: Update keypads again. Each row by its why (docs/features/11, the table of why's; LINK-035); a value this panel doesn't know, by its decade (LINK-039). Transfer (20-29), red: tried again after 2 min, 3 failed runs per keypad in 24 h; "Not updated: <reason> · trying again shortly" while tries remain, "· 3 tries failed today, try again tomorrow" after the third (the keypad then answers BUDGET 38, so Update keypads doesn't count it). The reasons: "the download broke off" (20), "the download was too slow" (21), "it got the wrong size" (22), "it arrived damaged" (23), "no data for a minute" (24), "it waited too long" (25), "not enough memory" (26), "it couldn't store it" (27); another: "the transfer failed". Refused (30-49), red, no retry, each with what to do: "Not updated: it isn't for this keypad's chip · update it with a cable" (31), and so "it isn't from a project it accepts" (32), "the signature check failed" (34), "it's too big for this keypad" (35), "it can't take this kind of update" (36), "it doesn't know this order" (37), "it refused the order" (40), "it got another version" (41); another: "the keypad refused it · update it with a cable". KEY 30 is "Accept the new key at keypad n" (amber, bl-048-fw-master-key); NOT_NEWER 33 is no failure: gray "Newer than this panel". The update server (10-19), amber, never relayed past its rules (LINK-031): "Not updated: the update server has nothing for it" (10), "the update server holds it back for now" (11; "held back" is the server's word only), "it needs another version first · update it with a cable" (12); another: "the update server won't offer it"; 13 and 14 take the relay. Not reached (1-9) takes the relay; with nothing to relay, amber "Not updated: connect keypad 4 to the internet, or update it with a cable" (NO_IMAGE: Kitchen is another model and this panel can't reach the server for a copy); another value: "Not updated: it can't reach the update server". Waiting (50-59): bl-048-fw-master-waits. Stopped (60-69), gray: "Stopped at the keypad" (STOPPED 60: its installer or system master stopped it there, bl-048-fw-kp-stopped), "Stopped" (CANCELLED 61: Stop here, or another value). 70 and up, gray: "Not updated". Garage keeps its own words (bl-048-fw-master-behind). The paragraph counts the keypads Update keypads can try (TRN): Bedroom keypad and Kitchen, not Hall keypad (its tries are used up) nor Garage. Another morning than Activity's (bl-048-fw-master-activity). Drawn scrolled to the rows, under Update keypads, so all four show.`,
  body: [MS_UPDATED(),
    block(KP_SECTION, UPDATE_KEYPADS, M.paragraph('Tries 2 keypads again.'),
      kpRow(HALL, 2, '0.6.1', 'Not updated: no data for a minute · 3 tries failed today, try again tomorrow', 'red'),
      kpRow(BEDROOM, 3, '0.6.1', 'Stopped at the keypad', 'gray'),
      kpRow(KITCHEN, 4, '0.6.1', 'Not updated: connect keypad 4 to the internet, or update it with a cable', 'amber'),
      GARAGE_ROW),
    MS_CHECK], scroll: 191});

msUpd({id: 'bl-048-fw-master-canary', title: 'Master: the first keypad went back, the rest of its model wait',
  proposal: P2('settings-update-updated', 'A version that fails on the first keypad of a model stops there, so the other keypads of that model keep working on the old one.'),
  notes: `The first keypad of each model (device and chip revision) goes first (LINK-030). Its rollback, or no return within 10 minutes ("Not updated: it didn't come back within 10 minutes"), holds every keypad of that model: "Waiting: Hall keypad failed on 0.6.2" (amber; "held back" stays the update server's word), until Update keypads. Kitchen is a 10.1" panel, another model, so it goes on. Activity logs "Keypad 2 update rolled back". Q13, settled by LINK-030: Update keypads then starts with Hall keypad again (the lowest number first), its row keeping the reason until then. The amber line stays under Stop, steady, until Update keypads. Drawn scrolled to the Keypads block.`,
  body: [MS_UPDATED(),
    block(KP_SECTION, STOP_KEYPADS,
      M.paragraph('Hall keypad failed its self-test on 0.6.2. Keypads of the same model wait; Update keypads tries again.', 'amber'),
      kpRow(HALL, 2, '0.6.1', 'Not updated: it failed its self-test, back on 0.6.1', 'red'),
      kpRow(BEDROOM, 3, '0.6.1', 'Waiting: Hall keypad failed on 0.6.2', 'amber'),
      kpRow(KITCHEN, 4, '0.6.1', 'Ready · installs next', 'blue')),
    MS_CHECK], scroll: 88});

msUpd({id: 'bl-048-fw-master-key', title: 'Master: its keypads must accept a new signing key',
  proposal: P2('settings-update-updated', 'A master moved to another signing key leaves no keypad needing a cable: each accepts the key at its own screen.'),
  notes: `The installer moved Harbor House to example.net (installer-update-server-warning), and 0.7.0 is signed with that server's key. A keypad that doesn't trust the key stops before downloading (KEY_WAIT, why KEY 30) and shows it (bl-048-fw-kp-key, Hall keypad's): "Accept the new key at keypad 2" in amber, until it is accepted there or Stop. A keypad that already accepted it goes on (Kitchen's was accepted at Kitchen a moment ago). The rest of the rollout doesn't wait for it: the next keypad's turn comes as usual. Drawn scrolled to the Keypads block, under "Updated to version 0.7.0".`,
  body: [MS_UPDATED('0.7.0'),
    block(KP_SECTION, STOP_KEYPADS,
      M.paragraph('Version 0.7.0 is signed with a new key. Each keypad accepts it on its own screen, with the installer code.', 'amber'),
      kpRow(HALL, 2, '0.6.2', 'Accept the new key at keypad 2', 'amber'),
      kpRow(BEDROOM, 3, '0.6.2', 'Accept the new key at keypad 3', 'amber'),
      kpRow(KITCHEN, 4, '0.6.2', 'Receiving from this panel · 12 %', 'blue')),
    MS_CHECK], scroll: 88});

// This panel's own run, from screens/updates-run.js (prog, STEPS): its own Stop, paragraph and steps.
const MS_STOP = M.row_action({glyph: 'circle-x', text: 'Stop the update', color: 'red', goTo: 'settings-update-stop-question'});
msUpd({id: 'bl-048-fw-master-own-waits', title: 'Master: its own update waits while a keypad self-tests',
  proposal: P2('settings-update-waiting', 'A master restart in the middle of a keypad\'s self-test would send the keypad back to its old version (its self-test needs the link): the master waits for it.'),
  notes: `OTA-032: while a keypad installs or self-tests, this panel puts off its own restart, with the reason "A keypad is checking its update" (panel_ota_set_restart_hold), at most 10 minutes; the line under the title: "Installs once the keypad is back". Starting this panel's update (0.6.3 here) stopped the keypads that weren't installing yet (CANCEL). This Install covers them, so they follow by themselves once 0.6.3 has passed its self-test: their rows say "Updates after this panel" (gray), as on the Install question (bl-048-fw-master-install-question). The steps follow under the keypads (screens/updates-run.js). Q14: the status-bar banner says "Update waiting · installs once the keypad is back" (50 bytes) while that is the reason (recommended, today's "Update waiting · installs once ..." pattern; "installs once disarmed" would be wrong while disarmed), as the other reasons keep today's banner; decided with the keypad's paused banner (bl-048-fw-kp-paused).`,
  bar: {...MS_BAR, banner: {glyph: 'clock', tone: 'blue', text: 'Update waiting · installs once the keypad is back'}},
  tags: {about: {text: 'Updating', tone: 'blue'}},
  body: [
    M.changed(M.row_status({glyph: 'clock', color: 'amber', title: 'Waiting to install version 0.6.3', detail: 'Installs once the keypad is back · A keypad is checking its update'})),
    MS_STOP,
    M.changed(M.paragraph('A keypad is checking its update. This panel restarts to install 0.6.3 once it is back, within 10 minutes.')),
    block(KP_SECTION,
      kpRow(HALL, 2, '0.6.2', 'Self-test · usually a minute, at most three', 'blue'),
      kpRow(BEDROOM, 3, '0.6.1', 'Updates after this panel', 'gray'),
      kpRow(KITCHEN, 4, '0.6.1', 'Updates after this panel', 'gray')),
  ]});

msUpd({id: 'bl-048-fw-master-stop-question', title: 'Master: Stop updating keypads?',
  proposal: P2(null, 'Stopping the keypads\' update is a choice someone may regret, as Stop the update is: the same question, for the keypads.'),
  notes: `Today's Stop question (settings-update-stop-question) for the keypads, titled as its button, "Stop updating keypads" (WORD-005). Stop it sends CANCEL to every keypad not installing yet; their rows say "Stopped". A keypad installing or on its self-test finishes (OTA-027: never after its restart begins). Keep updating closes the question. Q12: one Stop for all keypads (recommended); stopping one keypad alone stays at the console (plink fw cancel n) and at that keypad (bl-048-fw-kp-stop-question).`,
  body: [MS_UPDATED(), KP_SECTION, STOP_KEYPADS,
    kpRow(HALL, 2, '0.6.1', 'Receiving from this panel · 45 %', 'blue'),
    kpRow(BEDROOM, 3, '0.6.1', 'Downloading · 72 %', 'blue'),
    kpRow(KITCHEN, 4, '0.6.1', 'Waiting for its turn to receive it', 'gray')],
  over: M.confirm_dialog({title: 'Stop updating keypads?', ok: 'Stop it', cancel: 'Keep updating',
    note: 'Keypads that haven\'t started installing keep their version. Update keypads starts it again later.'})});

msUpd({id: 'bl-048-fw-master-install-question', title: 'Master: Install, the question saying its keypads update after it',
  proposal: P2('settings-update-install-question', 'An Install on the master also updates its keypads afterwards: the question says so, and the page shows which keypads will follow.'),
  notes: `Q3: on a master with keypads the question has a note of its own (TRN, by the keypads that update through the link: "... then its keypad updates."): "The panel downloads it now and restarts once everything is disarmed, then its 3 keypads update." (95 bytes) and "It keeps protecting meanwhile, and you can stop it until it restarts." (69 bytes), the sheet's two sentences of at most 95 bytes (191 in all); recommended, so whoever installs knows the keypads restart afterwards too. Without keypads, today's note. Before the Install the rows say "Updates after this panel"; a keypad that can't be updated through the link keeps its own words and isn't counted. Starting this panel's update while its keypads update stops the ones not installing yet; they follow once this panel's update passed its self-test (bl-048-fw-master-own-waits).`,
  tags: {about: {text: 'Update', tone: 'violet'}},
  body: [
    M.row_link({glyph: 'sparkles', color: 'violet', title: 'Version 0.6.2 is ready to install', detail: 'You have 0.6.1 · the alarm keeps working while it downloads', trail: ''}),
    M.row_action({glyph: 'download', text: 'Install version 0.6.2', color: 'blue'}),
    M.paragraph('The panel restarts once everything is disarmed. It takes about a minute.'),
    block(KP_SECTION,
      kpRow(HALL, 2, '0.6.1', 'Updates after this panel', 'gray'),
      kpRow(BEDROOM, 3, '0.6.1', 'Updates after this panel', 'gray'),
      kpRow(KITCHEN, 4, '0.6.1', 'Updates after this panel', 'gray')),
  ],
  over: changedNote(M.confirm_dialog({title: 'Install version 0.6.2?', tone: 'primary', ok: 'Install',
    note: 'The panel downloads it now and restarts once everything is disarmed, then its 3 keypads update. It keeps protecting meanwhile, and you can stop it until it restarts.'}))});

// ---- the master, elsewhere ----
// Installer > Keypads and devices as built (screens/panel-link.js installer-devices-keypads: linkInst,
// DEVICES_PAGE, ADD_ROW, HALL, tableRows), copied here: each screen file has its own scope.
const LINK_INST_SECTIONS = M.settings_sections({drop: ['installer']}).filter((s) => !s.divider);
const INST_PAGE = {title: 'Keypads and devices', note: 'A supervised device that stops answering shows a trouble', back: 'installer-menu'};
const instTable = (skip = []) => Array.from({length: 16}, (_, i) => i + 1).filter((d) => !skip.includes(d))
  .map((d) => M.row_link({glyph: 'cpu', color: 'gray', title: `Device ${d}`, detail: 'Not in use', goTo: d === 1 ? 'installer-device' : ''})).join('');
const INST_ADD = M.row_action({glyph: 'plus', text: 'Add a keypad', color: 'blue', goTo: 'keypad-add-open'});
const INST_HALL = M.row_link({glyph: 'keyboard', color: 'green', title: 'Hall keypad', detail: 'Keypad 2 · Home', value: 'Connected', goTo: 'keypad-page'});
M.screen({id: 'bl-048-fw-installer-keypads', area: 'Installer', title: 'Installer: Keypads and devices, a keypad updating', shot: null,
  proposal: P2('installer-devices-keypads', 'A keypad restarting for its update isn\'t lost: its row says so instead of "Not responding".'),
  notes: `Q5 (not in the spec): while a keypad installs or is on its self-test, its row's value says "Updating" in blue, with "Installing version 0.6.2" or "Self-test after its update" under it, instead of "Connecting" or a gray "Not responding" while it restarts (recommended: its supervision trouble is held then, up to 10 min, LINK-030, and "Not responding" would say otherwise). After 10 minutes without it, today's amber "Not responding" and the trouble. The keypad's own page says the same on its top row. Drawn on the built page with only Bedroom keypad's row changed (there "Not responding" since 10:25 PM); Kitchen (4) would be a third row.`,
  render: () => M.main({view: 'settings', tone: 'blue', rail: {tone: 'blue'}, bar: {banner: 'installer', time: '10:58'},
    content: M.settings({section: 'about', sections: LINK_INST_SECTIONS, ...INST_PAGE, body: [
      M.section('Keypads'), INST_ADD, INST_HALL,
      M.changed(M.row_link_alert({glyph: 'keyboard', color: 'blue', title: 'Bedroom keypad', detail: 'Keypad 3 · Home', sub: 'Self-test after its update', subColor: 'blue', value: 'Updating', valueColor: 'blue'})),
      M.section('Supervised devices'),
      instTable([2, 3]),
    ].join('')})}),
});

M.screen({id: 'bl-048-fw-master-activity', area: 'Views', title: 'Activity on the master: keypad updates', shot: null,
  proposal: P2('activity-log', 'What each keypad\'s update did stays in the master\'s event log, as its own updates do.'),
  notes: `The firmware_update stages appended for keypads (keypad_installing 9, keypad_updated 10, keypad_rolled_back 11, keypad_failed 12 with the reason in arg2; source {keypad, n}; docs/features/05), in notify_words.c, so a phone with System events on gets the same words. Another morning than bl-048-fw-master-ended's, in an order the rules allow (LINK-030): Hall keypad's transfer failed for the third time ("No data for a minute"), so Bedroom keypad, the first 7" panel ready, installed first and passed; then Kitchen, the first (and only) 10.1" panel, installed and went back, which holds only its own model. Q6: the keypad by its number, as "Keypad 3 not responding" (recommended until a name lookup shared with Zigbee and Z-Wave exists, docs/features/11 Open questions). Q8: a key accepted at a keypad logged here too, "Keypad 2 accepted a new signing key" (recommended, as a server change is, OTA-023; a new stage after 12, so a spec change). Amber as the master's own rolled-back and failed rows.`,
  render: () => M.main({view: 'activity', rail: {}, bar: {banner: null, time: '7:54', ampm: 'AM'}, content: `<div class="view column">
    ${M.view_header({title: 'Activity', note: 'Everything the panel did, newest first'})}
    <div class="row" style="flex:none">${M.segments(['All', 'Alarms', 'Troubles', 'Arming', 'Bypass', 'System'], 5)}</div>
    <div class="list scroll">${[
      M.changed(M.event_row({glyph: 'refresh-cw', title: 'Keypad 4 update rolled back', detail: 'The new version failed; the previous one runs', time: '7:53 AM', tone: 'amber'})),
      M.changed(M.event_row({glyph: 'refresh-cw', title: 'Keypad 4 installing an update', detail: 'It restarts with the new version', time: '7:49 AM', tone: 'blue'})),
      M.changed(M.event_row({glyph: 'refresh-cw', title: 'Keypad 2 update failed', detail: 'No data for a minute', time: '7:44 AM', tone: 'amber'})),
      M.changed(M.event_row({glyph: 'refresh-cw', title: 'Keypad 3 updated', detail: 'The new version passed its self-test', time: '7:33 AM', tone: 'blue'})),
      M.changed(M.event_row({glyph: 'refresh-cw', title: 'Keypad 3 installing an update', detail: 'It restarts with the new version', time: '7:30 AM', tone: 'blue'})),
      M.event_row({glyph: 'refresh-cw', title: 'Firmware updated', detail: 'The new version passed its self-test', time: '7:14 AM', tone: 'blue'}),
    ].join('')}</div></div>`}),
});

// ================================ B. A keypad: About and Software update ================================
// The keypad's frame, as screens/panel-link.js draws it (kp, KP_SECTIONS): its rail opens its own
// screens, its Settings keep Screen and sound, Wi-Fi and network, Time and language, About.
const KP_SECTIONS = M.settings_sections({drop: ['users', 'integrations', 'automations', 'walk', 'installer']}).filter((s) => !s.divider);
const kpLinks = (html) => html.replace('data-go="devices"', 'data-go="kp-devices"').replace('data-go="home-ready"', 'data-go="kp-home-ready"');

// kp-about as built (kpSet: the factory-codes reminder, 10:42), with only the Software update row changed.
const CODES_KP = 'Factory codes · Change them on the alarm panel';
M.screen({id: 'bl-048-fw-kp-about', area: 'Keypad', title: 'Keypad: About, its updates from Harbor House', shot: null,
  proposal: P2('kp-about', 'A keypad that updates at its master\'s order runs no check of its own: About says where its updates come from instead of an "Up to date" with nothing behind it.'),
  notes: `kp-about with only the Software update row changed. Paired with a master that offers keypad updates (LINK-029), the keypad runs no check of its own, so "Up to date" would have nothing behind it: the row says where updates come from, "From Harbor House" on its line (the name wraps there), with no value while nothing runs. While a run exists (from Harbor House's order to the end of the self-test) the value is "Updating" (blue) and the Settings list's About tag "Updating"; after a rollback "Rolled back" (amber), as on the master, until the next order. A PROBE answer that offers a version never shows on a keypad as "Update" (violet) or as an offered version, here, in the tag or on Software update: no Install there would answer it. A keypad paired with a master without keypad updates, in LINK-025's maintenance, or needing an update to read its master's settings keeps today's row (its own check and Install). When built, kp-about's note "Software update works as on any panel (a keypad updates itself, LINK-020)" becomes "Software update says where its updates come from: Harbor House orders them (LINK-029)".`,
  render: () => kpLinks(M.main({view: 'settings', rail: {devices: true}, bar: {time: '10:42', banner: 'codes', bannerText: CODES_KP},
    content: M.settings({sections: KP_SECTIONS, section: 'about', title: 'About this panel', body: [
      M.row_status({glyph: 'shield-check', color: 'green', title: 'RoboAlarms Panel', detail: 'Version 0.6.0 · Panel ID b7c8d9e0f1a2'}),
      M.row_status({glyph: 'keyboard', color: 'green', title: 'Keypad 2 of Harbor House', detail: 'Connected · shows and arms Home'}),
      M.changed(M.row_link({glyph: 'refresh-cw', title: 'Software update', detail: 'From Harbor House', goTo: 'bl-048-fw-kp-from-master'})),
      M.row_link({glyph: 'hard-drive', color: 'violet', title: 'SD card', detail: '44.7 GB free of 59.6 GB', value: 'Ready'}),
      M.section('This panel'),
      M.row_value({title: 'Built', value: 'Sep 19, 2026'}),
      M.row_value({title: 'Running for', value: '3 days'}),
    ].join('')})})),
});

const KP_BAR = {banner: null, time: '7:42', ampm: 'AM', chimeOff: false};
const kpFrame = (m) => newBanner(kpLinks(M.main({...m, rail: {devices: true, ...(m.rail || {})}, bar: {...KP_BAR, ...(m.bar || {})}})));
const KP_UPDATING = {about: {text: 'Updating', tone: 'blue'}};
const kpUpd = ({id, title, notes, proposal, body = [], bar, tags = KP_UPDATING, pad, over, toast, scroll = 0}) => M.screen({
  id, area: 'Keypad', title, shot: null, proposal, notes,
  render: () => scrolledTo(kpFrame({view: 'settings', bar, over, toast,
    content: M.settings({section: 'about', sections: KP_SECTIONS, title: 'Software update', back: 'bl-048-fw-kp-about', tags, body: body.join(''), pad})}), scroll),
});
const kpBanner = (text, glyph = 'download', tone = 'blue') => ({banner: {glyph, tone, text}});

// The steps of an ordered run: today's five (screens/updates-run.js), the first (on the relay) and the
// third the keypad's own. No name in a step's title (47 bytes); the name goes on its line (79 bytes).
const kpSteps = (path, now, previous = '0.6.1') => [M.section('Steps'), ...[
  path === 'relay' ? ['Receive it', 'From Harbor House · 2.7 MB', true] : ['Download', '2.7 MB'],
  ['Check the download', 'Its signature and checksum'],
  ['Wait for its turn', 'One keypad at a time, once everything is disarmed', true],
  ['Restart with the new version', 'About a minute; the self-test starts at once'],
  ['Self-test', `Usually a minute, at most three · restarts with ${previous} if it fails`],
].map(([title, detail, mark], i) => {
  const row = M.row_link({glyph: i < now ? 'circle-check' : i === now ? 'circle-dot' : 'circle', color: i < now ? 'green' : i === now ? 'blue' : 'gray', title, detail, trail: ''});
  return mark ? M.changed(row) : row;
})].join('');
const KP_STOP = M.row_action({glyph: 'circle-x', text: 'Stop the update', color: 'red', goTo: 'bl-048-fw-kp-stop-question'});
const KP_TRY_AGAIN = M.changed(M.paragraph('Harbor House\'s installer or system master can try again there, with Update keypads.'));

kpUpd({id: 'bl-048-fw-kp-from-master', title: 'Keypad: Software update, updates come from Harbor House',
  proposal: P2('settings-update', 'A keypad runs the version its master runs, installed when its master says: it offers no Install and checks nothing itself.'),
  notes: `Today a keypad's Software update is any panel's (settings-update), with Install and Check again; besides the change, only the frame differs from it (the keypad's rail and Settings list). Paired with a master that offers keypad updates (LINK-029, OTA-030): no Install and no Check again (it runs no check of its own; its master's order is its check), and a PROBE answer is never offered here (bl-048-fw-kp-about). Today's page stays for a keypad paired with a master without keypad updates, during LINK-025's maintenance (10 minutes without system communication) and while it needs an update to read its master's settings (kp-home-update). Q10: the master by its name, as Home's "To Harbor House" and the spec's "Updates come from <master>" (recommended); "the alarm panel" only where no name is known. The title holds the name: its TR comment gives at least 65 bytes (18 and a 47-byte name), more than the status title's 64-byte buffer. The paragraph says how an update starts, after Harbor House's own update or with Update keypads there (decision 2: a keypad found behind later, newly paired or back on line, waits for Update keypads). For 24 hours after an update the row says "Updated to version 0.6.2 · 7:44 AM · it passed its self-test" first, as on any panel; a stopped, failed or rolled-back run shows its result here until the next order (bl-048-fw-kp-stopped, -failed, -rolled-back). Opens for the installer and the system master, their code checked by Harbor House, as today.`,
  tags: {},
  body: [
    M.changed(M.row_status({glyph: 'house', color: 'blue', title: 'Updates come from Harbor House', detail: 'Version 0.6.1'})),
    M.changed(M.paragraph('Harbor House updates its keypads after its own update, or when its installer or system master chooses Update keypads there. This keypad restarts by itself when its turn comes, once everything is disarmed.')),
  ]});

const RECEIVING = [
  M.changed(M.row_status({glyph: 'download', color: 'blue', title: 'Receiving version 0.6.2', detail: 'From Harbor House · 1.1 of 2.7 MB · 42 %', level: 42})),
  KP_STOP,
  M.changed(M.paragraph('The alarm keeps working while it receives. You can stop the update until the keypad restarts.')),
  kpSteps('relay', 0),
];
kpUpd({id: 'bl-048-fw-kp-receiving', title: 'Keypad: receiving the update from Harbor House',
  proposal: P2('settings-update-downloading', 'The keypad shows the same run as any panel, saying where the update comes from.'),
  notes: `A keypad that can't reach the update server gets the image from Harbor House (RELAY). One that can downloads it itself, exactly as today's settings-update-downloading ("Downloading version 0.6.2", "1.1 of 2.7 MB · 42 %", the step "Download"); only the third step is the keypad's own. The name stays out of the title, the banner and the steps' titles: "From Harbor House · 1.1 of 2.7 MB · 42 %" is 77 bytes with a 47-byte name, within the line's 111; the step's line "From Harbor House · 2.7 MB", within its 79. The bar steps to the whole percent at most once a second, with no animation; the title doesn't change while it receives. Before the first bytes: "From Harbor House · Starting to receive it". Stop the update asks the installer or system master code (checked by Harbor House), then the keypad's own question (bl-048-fw-kp-stop-question). The status-bar banner says "Receiving an update" (bl-048-fw-kp-home-receiving).`,
  bar: kpBanner('Receiving an update'),
  body: RECEIVING});

kpUpd({id: 'bl-048-fw-kp-paused', title: 'Keypad: receiving paused, another partition is armed',
  proposal: P2('settings-update-downloading', 'Receiving pauses while anything is armed or busy, so the link stays free for the alarm; the keypad says why and what ends it.'),
  notes: `The relay pauses (FW_STATE flag PAUSED) while Harbor House's status says no or doesn't come (LINK-034), each reason with its own line and paragraph, as today's waiting page (wait_paragraph): "The system is armed" (here another partition; this keypad's own arming closes Settings, as today) / "Disarm everything and it goes on by itself."; "Installer mode is on" / "Leave installer mode on Harbor House and it goes on by itself."; "An alarm is sounding" / "Silence the alarm and it goes on by itself."; "A walk test is running" / "End the walk test and it goes on by itself."; "A panic is being sent" / "It goes on once the panic has been sent."; "The fire or CO alarm still sounds here" / "Silence it here and it goes on by itself."; "Harbor House isn't answering" (no status for 15 s) / "Check that Harbor House is on and on the same Wi-Fi." (97 bytes at most, with a 47-byte name, within today's 111). After 30 minutes paused the run ends: "Update failed · It waited too long · nothing was changed" (bl-048-fw-kp-failed's page, with that reason). The banner names no reason: "Update paused · goes on by itself" (Q14, recommended, decided with the master's waiting banner; "goes on once disarmed" would be wrong for most reasons). Amber while paused; the bar stays where it stopped.`,
  bar: kpBanner('Update paused · goes on by itself', 'clock'),
  body: [
    M.changed(M.row_status({glyph: 'clock', color: 'amber', title: 'Receiving version 0.6.2', detail: 'Paused at 42 % · The system is armed', level: 42})),
    KP_STOP,
    M.changed(M.paragraph('Disarm everything and it goes on by itself.')),
    kpSteps('relay', 0),
  ]});

kpUpd({id: 'bl-048-fw-kp-ready', title: 'Keypad: ready, waiting for its turn',
  proposal: P2('settings-update-waiting', 'A keypad with the update checked waits for its master\'s word, so only one keypad restarts at a time.'),
  notes: `Checked and kept (READY), until Harbor House's INSTALL names this version; then the keypad asks its gate twice (LINK-034: a fresh status from Harbor House, everything disarmed, no panic still being sent, no fire or CO tone kept here) and restarts: "Restarting with version 0.6.2", then today's self-test page (its self-test needs Harbor House: UI, touch and the link). Kept up to 24 h; thrown away then, on Stop here or at Harbor House, on an order for another version, or on removal (LINK-035). The line under the title: "Installs when its turn comes" and the reason "Waiting for Harbor House" (OTA-026's "Waiting for the alarm panel", Q10), 91 bytes with a 47-byte name, within 111. The banner: "Update waiting · installs when its turn comes" (46 bytes, no name), today's "Update waiting · installs once ..." pattern and the row's words: "Waiting", not "Ready", as today's ("Ready" asks for Install; this one installs by itself).`,
  bar: kpBanner('Update waiting · installs when its turn comes', 'clock'),
  body: [
    M.changed(M.row_status({glyph: 'clock', color: 'amber', title: 'Waiting to install version 0.6.2', detail: 'Installs when its turn comes · Waiting for Harbor House'})),
    KP_STOP,
    M.changed(M.paragraph('This keypad restarts when its turn comes, once everything is disarmed. It takes about a minute.')),
    kpSteps('relay', 2),
  ]});

kpUpd({id: 'bl-048-fw-kp-stop-question', title: 'Keypad: Stop the update?',
  proposal: P2('settings-update-stop-question', 'A keypad has no Install to start it again: its Stop question says who can.'),
  notes: `Today's question (settings-update-stop-question) on a keypad, after the installer's or the system master's code (checked by Harbor House). Its note says who starts it again, not today's "You can install 0.6.1 again later; it downloads it again.", as the keypad has no Install (LINK-029): "The keypad keeps version 0.6.1." and "Harbor House can start it again with Update keypads." (87 bytes with a 47-byte name, within the sheet's 95 for a sentence). Stop it ends the run (OTA-027); Harbor House's row says "Stopped at the keypad" (bl-048-fw-kp-stopped). Keep updating closes the question; it closes by itself, as today, when the run ends or the restart begins.`,
  bar: kpBanner('Receiving an update'),
  body: RECEIVING,
  over: changedNote(M.confirm_dialog({title: 'Stop the update?', ok: 'Stop it', cancel: 'Keep updating',
    note: 'The keypad keeps version 0.6.1. Harbor House can start it again with Update keypads.'}))});

kpUpd({id: 'bl-048-fw-kp-stopped', title: 'Keypad: stopped at the keypad',
  proposal: P2('settings-update-stopped', 'Stopped at a keypad, only the master starts it again: the keypad says where.'),
  notes: `Stop at the keypad (OTA-027; the installer's or the system master's code, checked by Harbor House). Harbor House's row says "Stopped at the keypad" and waits for Update keypads there. No Install and no Check again here: the keypad never takes another version than its master's. The result stays until the next order, as the failed and rolled-back ones.`,
  tags: {},
  body: [
    M.row_status({glyph: 'circle-x', color: 'gray', title: 'Update stopped', detail: 'Stopped by Alex · you have 0.6.1, nothing was changed'}),
    M.changed(M.paragraph('Harbor House\'s installer or system master can start it again there, with Update keypads.')),
  ]});

kpUpd({id: 'bl-048-fw-kp-failed', title: 'Keypad: update failed, its tries used up',
  proposal: P2('settings-update-failed', 'A failed update on a keypad leaves nothing to tap there: the page says where and when it can be tried again.'),
  notes: `The run ended with nothing changed, here with no data from Harbor House for a minute (STALLED 24), the keypad's third failed run in 24 hours (LINK-035), as Hall keypad's row says on bl-048-fw-master-ended: the reason in the words of the master's row (others: "It waited too long" after 30 minutes paused, "It arrived damaged" ...), then "nothing was changed". No Install and no Check again (LINK-029), and no About tag: nothing is offered. The paragraph says what happens next, as the keypad knows it: while tries remain (a transfer reason, fewer than 3 failed runs in 24 h) "Harbor House tries again within a few minutes."; after the third, as drawn, Harbor House's row says "3 tries failed today, try again tomorrow" (bl-048-fw-master-ended) and Update keypads is refused until then (BUDGET); for an image the keypad refused (30-49), "This version can't be installed through the link: update this keypad with a cable." The result stays until the next order.`,
  tags: {},
  body: [
    M.row_status({glyph: 'circle-x', color: 'red', title: 'Update failed', detail: 'No data for a minute · nothing was changed'}),
    M.changed(M.paragraph('3 tries failed today. Harbor House\'s installer or system master can try again tomorrow, with Update keypads.')),
  ]});

kpUpd({id: 'bl-048-fw-kp-rolled-back', title: 'Keypad: rolled back, the self-test failed',
  proposal: P2('settings-update-rolled-back', 'A keypad back on its old version has no Install to offer: the page says who can try again.'),
  notes: `The keypad's self-test failed (UI, touch or the link) and it went back by itself, as any panel (OTA-005); Harbor House holds the keypads of its model (bl-048-fw-master-canary). The About row and the Settings tag say "Rolled back" in amber, as today. No Install and no Check again (LINK-029): Update keypads on Harbor House tries this keypad first again (LINK-030). The result stays until the next order.`,
  tags: {about: {text: 'Rolled back', tone: 'amber'}},
  body: [
    M.row_status({glyph: 'triangle-alert', color: 'amber', title: 'Version 0.6.2 failed its self-test', detail: 'Back on 0.6.1 by itself'}),
    KP_TRY_AGAIN,
  ]});

// ---- a keypad's Home while it updates ----
const KP_READY = {tone: 'green', glyph: 'shield-check', headline: 'Disarmed', detail: 'Ready to arm', note: 'All zones closed'};
M.screen({id: 'bl-048-fw-kp-home-receiving', area: 'Keypad', title: 'Keypad: Home, the update banner', shot: null,
  proposal: P2('kp-home-ready', 'Whoever passes a keypad that is updating knows it will restart, as on the alarm panel.'),
  notes: `Today's update banner (home-update-banner) on a keypad, steady and blue, its words changing only at a new step, never naming Harbor House (a long name would cut the banner short): "Downloading an update" (it fetches itself), "Receiving an update", "Checking the download", "Update paused · goes on by itself", "Update waiting · installs when its turn comes", "Restarting to install the update", "Self-test after the update", "Stopping the update"; amber "Going back to version 0.6.1 · restarting". New for a keypad: the receiving, paused and waiting words (each at most 63 bytes, as today's). It gives way to an alarm as today. A tap opens Software update (with the code). Nothing covers Home, the pad or Panic.`,
  render: () => kpFrame({bar: {banner: {glyph: 'download', tone: 'blue', text: 'Receiving an update', goTo: 'bl-048-fw-kp-receiving'}},
    content: M.home(KP_READY, M.panel_arm())}),
});

M.screen({id: 'bl-048-fw-kp-home-armed', area: 'Keypad', title: 'Keypad: Home armed, the update paused', shot: null,
  proposal: P2('home-update-armed', 'While armed the keypad only tells: the update pauses and goes on by itself once everything is disarmed.'),
  notes: `Armed, Settings is closed as today; the banner tells. A keypad that was receiving pauses ("Update paused · goes on by itself"); one ready to install keeps "Update waiting · installs when its turn comes" (Harbor House gives no INSTALL while anything is armed, and the keypad's gate refuses it anyway). A keypad downloading its own image goes on. Harbor House's installer or system master can stop it there.`,
  render: () => kpFrame({tone: 'rose', rail: {locked: true}, bar: {banner: {glyph: 'clock', tone: 'blue', text: 'Update paused · goes on by itself'}, locked: true},
    content: M.home({tone: 'rose', glyph: 'lock', headline: 'Armed Away', detail: 'The whole house is protected', note: 'Since 7:40 AM · By Alex'},
      M.code_pad({title: 'Disarm', tone: 'green', glyph: 'lock-open', close: false}))}),
});

// ================================ C. A new signing key, at the keypad (LINK-036) ================================
const NEW_KEY = M.row_value({title: 'example.net', value: '8C41 0E9D 52A7 B3F6'});
const KEY_STATUS = M.row_status({glyph: 'shield-alert', color: 'amber', title: 'Version 0.7.0 is signed with a new key', detail: 'This keypad doesn\'t know this key yet'});
const keyBody = (hold) => [
  M.changed(KEY_STATUS),
  M.changed(M.paragraph('Software signed with it can change everything this keypad does. Accept it only if Harbor House moved to it.', 'amber')),
  M.section('The new key'),
  M.changed(NEW_KEY),
  M.changed(M.paragraph('Check that it matches the key on Harbor House: Installer > Updates > Update server.')),
  M.changed(hold),
  M.section(''),
  KP_STOP,
];
const KEY_BANNER = kpBanner('Update waiting · accept the new key', 'key-round', 'amber');

kpUpd({id: 'bl-048-fw-kp-key', title: 'Keypad: a new signing key, before the installer code',
  proposal: P2(null, 'A master moved to another signing key would otherwise leave every keypad needing its console or a cable (decision 1).'),
  notes: `LINK-036: the order names 0.7.0's key; the keypad trusts neither it nor an accepted one, so it stops before downloading (KEY_WAIT) and shows the key, as Harbor House's own warning did (installer-update-server-warning): its server, its fingerprint (the first 16 hex digits in groups of four, OTA-020) and where to compare it. Harbor House's row says "Accept the new key at keypad 2" (bl-048-fw-master-key). Opened here with the system master's code, the hold first asks for the installer's (next screens). Q7: when Software update was opened with the installer code in this Settings visit, the hold alone (recommended); the system master's code never accepts a key. Q9: the status-bar banner "Update waiting · accept the new key" in amber (recommended), so the installer at the keypad finds this page; a tap opens it, with the code. Stop the update ends the run (Harbor House's row: "Stopped at the keypad").`,
  bar: KEY_BANNER,
  body: keyBody(M.row_hold({text: 'Enter the installer code'}))});

kpUpd({id: 'bl-048-fw-kp-key-wrong', title: 'Keypad: accepting the key, a master code instead of the installer\'s',
  proposal: P2(null, 'Only the installer\'s code accepts a key: another person\'s code is refused with why; a code no one has counts toward the keypad\'s lockout, as anywhere on it.'),
  notes: `The pad in the page's place (as Users' and kp-standalone's), titled for what it does. Harbor House checks the code (CMD op 2) as an installer code only, by the rule built for every code (eng_auth), as on kp-standalone and the Installer pad: a valid master or user code is refused with "That isn't the installer code" (drawn) and is not counted (a valid code resets the count); a code no one has shows "Wrong code, try again" and counts toward this keypad's own lockout: after 5, "Keypad locked. Try again in 4:59" (the countdown once a second, bl-048-fw-kp-key-locked), on Home's pad too; Harbor House's own keypad isn't affected (LINK-048). So a system master who opened Software update with their own code and tries it here never locks the keypad. The X goes back to the page; nothing changes.`,
  bar: KEY_BANNER,
  pad: M.code_pad({title: 'Accept the new key', tone: 'amber', glyph: 'key-round', hint: 'That isn\'t the installer code', error: true, width: 428})});

kpUpd({id: 'bl-048-fw-kp-key-locked', title: 'Keypad: accepting the key, locked after 5 wrong codes',
  proposal: P2(null, 'The keypad\'s own lockout applies here as on Home\'s pad.'),
  notes: `The same lockout as pad-locked-out, counted by Harbor House for this keypad: 5 codes no one has (a valid master or user code isn't counted, bl-048-fw-kp-key-wrong). The hint counts down once a second; the pad takes no code until it ends.`,
  bar: KEY_BANNER,
  pad: M.code_pad({title: 'Accept the new key', tone: 'amber', glyph: 'key-round', hint: 'Keypad locked. Try again in 4:59', error: true, width: 428})});

kpUpd({id: 'bl-048-fw-kp-key-hold', title: 'Keypad: the installer code taken, holding to accept',
  proposal: P2(null, 'The hold is the confirmation, as for Harbor House\'s own server change: nothing changes until it completes.'),
  notes: `After the installer's code the row becomes the hold (2 s, here half way); letting go early changes nothing. Then the keypad keeps example.net and this key as its trust record (ota/trust), as a server change does on a master, and goes on with Harbor House's order.`,
  bar: KEY_BANNER,
  body: keyBody(M.row_hold({text: 'Hold to accept this key', held: 0.5}))});

kpUpd({id: 'bl-048-fw-kp-key-accepted', title: 'Keypad: the key accepted, the update goes on',
  proposal: P2(null, 'After the hold the keypad says what it did and the run goes on by itself.'),
  notes: `The toast "New key accepted", once; the page is the run again: Harbor House orders it anew, so it receives 0.7.0 as any update (or downloads it, a keypad that reaches the update server), "From Harbor House · Starting to receive it" until the first bytes. Harbor House's row moves from "Accept the new key at keypad 2" to the run's words. Later updates signed with this key need no question; going back to a key the keypad already trusts needs none either. Q8 (bl-048-fw-master-activity): logged on Harbor House, "Keypad 2 accepted a new signing key".`,
  bar: kpBanner('Receiving an update'),
  toast: 'New key accepted',
  body: [
    M.row_status({glyph: 'download', color: 'blue', title: 'Receiving version 0.7.0', detail: 'From Harbor House · Starting to receive it', level: 0}),
    KP_STOP,
    M.paragraph('The alarm keeps working while it receives. You can stop the update until the keypad restarts.'),
    kpSteps('relay', 0, '0.6.2'),
  ]});

kpUpd({id: 'bl-048-fw-kp-key-factory', title: 'Keypad: a new key refused, factory installer code in use',
  proposal: P2(null, 'With the public factory installer code anyone could accept a key: refused, as on the master (OTA-021).'),
  notes: `While Harbor House's latest STATUS has the installer bit of default_codes set (ALARM_DEFAULT_CODE_INSTALLER, as ui_update.c tests it on the master), the hold is dimmed with what to do, as installer-update-server-factory-code on the master. A factory master code alone does not dim it. The run waits in KEY_WAIT; Stop still works. The factory-codes banner gives way to the update banner (today's order).`,
  bar: KEY_BANNER,
  body: keyBody(M.row_link({glyph: 'key-round', color: 'gray', title: 'Accept the new key', detail: 'Change the installer code on Harbor House first: the factory one is public', disabled: true}))});

M.screen({id: 'bl-048-fw-kp-key-home', area: 'Keypad', title: 'Keypad: Home, waiting for the new key', shot: null,
  proposal: P2('kp-home-ready', 'Harbor House says "Accept the new key at keypad 2": the keypad leads the installer to the page.'),
  notes: `Q9 (recommended): steady amber banner while the keypad waits for the key, a tap opens Software update (with the code). It gives way to an alarm, arming and installer-mode banners as today's update banner does.`,
  render: () => kpFrame({bar: {banner: {glyph: 'key-round', tone: 'amber', text: 'Update waiting · accept the new key', goTo: 'bl-048-fw-kp-key'}},
    content: M.home(KP_READY, M.panel_arm())}),
});
