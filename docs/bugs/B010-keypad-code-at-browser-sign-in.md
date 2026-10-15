# B010 · A keypad code typed at the browser sign-in burns the wrong-sign-in count

**Status:** fixed in code (2026-09-24); installed on the second panel by network update
(ELF `4d77ec45...`), not on the first; the message not yet seen on a panel. Found in use: the
installer could not sign in to the Panel Manager on either panel.

## Symptom
The sign-in was tried with the Panel ID as the user name and the **installer keypad code** as
the password. Every attempt said "That user name and password don't match a sign-in set on this
panel", and after five the page said "Too many tries. Wait a moment before trying again" (the
sign-in gate: one minute, doubling to 15). The same happened on the second, freshly flashed
panel while trying to enroll a workstation (which needs the installer, not a master).

## Cause
Not a fault in the check: since `dd63dda` keypad codes do not sign in to the Panel Manager (a
code seen over a shoulder must not open the panel remotely, docs/features/31). Each person signs in
with a **browser password** of 12-64 characters set on the panel's screen - the installer in
Settings > Installer > Allow browser sign-in (user name: the Panel ID), a master in Settings >
Users > their name. Neither panel had an installer browser password yet, so no password could
match.

What made it a defect: the page only said so in small print, the failure message did not
mention codes at all, and every code typed counted as a wrong sign-in - so the natural first
attempt locked the installer out for up to 15 minutes on each panel.

## Fix
- `components/services/panel_web/src/panel_web.c` (`login`): a password shorter than
  `WEB_PASSWORD_MIN` (12) is refused with `400 password_too_short` **before** the sign-in gate,
  and is not counted. Every stored browser password is at least 12 characters (`web_login.c`
  checks it when one is set; temporary ones are 19), so such a guess can never match and tells
  a guesser nothing the length rule doesn't. The 2-second spacing between attempts still applies.
- `components/services/panel_web/web/panel.js`: the same check before sending (no request, no count), one
  message for both - "Keypad codes don't sign in here. Use the browser password set on the
  panel's screen (at least 12 characters): the installer's is in Settings > Installer > Allow
  browser sign-in, a master's in Settings > Users > their name." - the wrong-password message
  now mentions codes too, and the note under the form names both places.
- `components/services/panel_web/web/mock-panel.cjs` answers as the panel; `components/services/panel_web/test/panel_pages.test.cjs`
  checks a 7-digit code is refused as `password_too_short`.

## Verify
- PC: `node --test components/services/panel_web/test/panel_pages.test.cjs` (6/6 on 2026-09-24).
- Panel, after the update: sign in with the Panel ID and the installer code: the page explains,
  and "Too many tries" never follows however many times a code is typed. A wrong password of 12+
  characters still counts (five, then a minute).

## Not changed
Keypad codes still do not sign in. If the installer code should open the Panel
Manager again, that reverses a security decision (docs/features/31) and is the maintainers' to make.
