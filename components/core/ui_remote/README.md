# ui_remote

**Tier:** core · **Features:** [11](../../../docs/features/11-Panel-Link-Master-Keypad/README.md)
("The master's settings and installer menu on keypads", LINK-057..LINK-065; BL-223) · **Tests:**
`test/` (LINK-NNN)

The touch UI's backend over a byte channel. A keypad's screen runs the alarm panel's own pages
(users, the installer menu and the rest); their `ui_backend_t` entries on the keypad are proxies
that carry each call to the alarm panel, which runs it on its own backend - the one its screen
and its Panel Manager use - under the grant it holds for that keypad. The alarm panel is the only
judge: of the code, of installer mode, of the user's authority.

## Public API
| Header | What |
|---|---|
| `ui_remote.h` | the calls (`ui_remote_call_t`, frozen ids by group) and what each needs (`ui_remote_info`); the alarm panel's side: the grant (`ui_remote_grant_t`, `_user`, `_tick`), what it can serve (`ui_remote_areas`) and `ui_remote_serve`; the keypad's side: `ui_remote_proxy` over a `ui_remote_port_t` |

Each struct a call carries crosses as a record described by a table of its fields (`ur_codec.c`),
appended only; the bytes of every call are frozen in `test/ui_remote_fixture_v1.h`.

## Depends on
`ui_backend` (the entries and their types); privately `alarm_core` (the configuration codec) and
`panel_link` (`plink_wire.h`'s reading and writing, `plink_wipe`).

## Rules
- Never renumber a call or reorder, remove or retype a field of a table: append. A new call goes
  in its group's block of 16, with its line in `ur_serve.c`'s layout comment and its bytes in a
  new frozen fixture.
- A call that holds a code or a password is marked `secret`: the channel wipes its arguments
  after use and keeps only its answer.
- Not a call, ever: a factory reset, updates, developer access, the alarm panel's own network,
  time and Screen and sound (LINK-058).
