# B005 · A signed-in master could guess other codes through the Panel Manager

**Status (2026-09-22): committed and pushed as `8367725`; fixed in code, built for every profile, not yet flashed.** Found by
code review; no evidence anyone used it.

## Symptom

A master signed in to the browser Panel Manager could find the installer code (or any other
user's code) by trying codes against `people` (change a code): the reply said
`not_authorized` for a code that belonged to someone else and `invalid_code` for one that
belonged to nobody, and nothing slowed the attempts down. The engine's five-wrong-codes
lockout did not help: the whole web service is one source to the engine, and a source's
failure count is reset by any correct code, so four guesses followed by the master's own code
never tripped it. At `login`, a real but non-administrator code was refused with a different
message from a wrong one, so anyone on the network could tell standard codes from wrong
guesses (at the 2 s per attempt the login limiter allows).

## Root cause

Two answers where one was needed, and no failure counting of the web service's own.

## Fix (`panel_web.c`, `web_extra.c`)

- `people` answers a code that belongs to someone else exactly as it answers no code at all;
  `login` refuses a non-administrator code with the same 401 `invalid_code` as a wrong code
  (a full session table is a 503 instead).
- A code gate shared by every request that carries a code (login, people, sensor bindings,
  installer code, factory reset): five refused codes hold every such request for a minute,
  doubling per further refusal up to 15 minutes; a correct code does not reset it; an hour
  without a refusal forgets it.
- Enrolling a developer certificate (which signs in as the master and updates firmware for
  as long as it exists) and installing older firmware (`X-Allow-Older`) need the installer,
  who alone can switch developer mode on.

Hardening found in the same review, fixed with it:

- A refused request that left its announced body unread made the single HTTPS worker read
  and discard that body 32 bytes at a time for as long as the sender cared to keep sending:
  such refusals now close the connection (`error()` returns `ESP_FAIL` after the reply).
- A 3 s pause on the line aborted a firmware upload or a JSON body: TLS receive timeouts are
  retried until the request's own deadline (5 min for uploads, 10 s for bodies).
- The session cookie was read from a 256-byte copy of the whole Cookie header; a longer
  header made every request after sign-in a 401 (`httpd_req_get_cookie_val` now).
- 64-character WPA2 keys were refused; a query string on an API URL changed the action;
  a raw NUL byte in a JSON string bypassed the `\u0000` check; the continuity token was
  compared with `strcmp`; a proof that could not be computed (memory, crypto) signed the
  user out instead of failing the one request; the configuration copy with the code hashes
  was freed unwiped on one error path; `A.input` could be called when absent.

Found in the code audit of 2026-09-24 (committed with it, not yet tried on hardware): applying installer programming
(`POST alarm`) had been left outside the gate, and it and `installer-code` still told another
user's code (`not_authorized`) from a wrong one (`invalid_code`). Both now answer
`invalid_code` for either (as `mock-panel.cjs` already did), `alarm` goes through the gate, and
`installer-code` no longer counts a right code refused because the panel is busy as a wrong
guess. Only an installer session reaches these requests.

## No codes in the browser (BL-135, 2026-10-02)

The guessing is now closed at its root: no browser request carries a keypad code any more.
Programming (`alarm`), sensor bindings, `people` and the installer and master codes run as the
signed-in person, whose slot and authority the engine reads again on every change
(docs/features/01 CORE-USR-028, CORE-PROG-011), so there is no code to try and no answer that
could tell one code from another. The code gate (`web_code_gate_open/failed`) is gone with
them. What still asks for a secret asks for the person's own password: the changes that let a
computer in or can't be undone (remote access, enrolling a workstation, a backup's download or
restore, factory reset) need it again unless it was typed in the last 10 minutes, and a wrong
one counts with the sign-in limiter (five free, then a minute doubling to 15). Keypad codes
stay where there is a keypad: the touchscreen, keypads and the remote keypad, under the
engine's own lockout.

## Verify on hardware

Sign in as the installer in the browser and wait 10 minutes; on Access and maintenance turn a
remote switch with a wrong password six times: the answers are `wrong_password` and, from the
sixth, 429 for a minute, and a sign-in on another browser waits the same minute. A right
password afterwards goes through and the next guarded change within 10 minutes asks nothing.
No request in the browser's network log carries a `code`, and `events` shows the People and
programming changes "By <name> · Panel Manager".
