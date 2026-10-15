# B029: the Home Assistant link dropped every 95 s while the house was quiet

**Status: fixed in the integration (`RoboAlarms/ha-addon` 7b6671a, not released yet) and proven on
board 3 with a Home Assistant in Docker, 2026-10-06.** Found by the bench run in
docs/features/23 validation.md, part 2. The panel's firmware is unchanged; protocol.md now says
which silence each side measures.

## Symptom
With nothing happening in the house, the panel closed the link and Home Assistant reconnected 5 s
later, over and over: every entity unavailable for a moment, every device on the panel's Smart
Home greyed with "Your devices can't be reached right now" for a moment, about every 95 s.

## Evidence
The panel's console, 90 s after Home Assistant connected:

```
I (07:31:01.824) ha_link: silent for 90 s: dropping the connection
```

Home Assistant's log (`custom_components.roboalarms` at debug) the same minute: a message from
the panel every 10 s, then the drop, then the reconnect.

```
07:30:41.096 DEBUG ... Manually updated roboalarms Home data
07:30:51.117 DEBUG ... Manually updated roboalarms Home data
07:31:01.140 ERROR ... Error requesting roboalarms Home data: connection closed
07:31:06.841 DEBUG ... Manually updated roboalarms Home data
```

Two drops in the first four minutes after pairing (07:29:25 and 07:31:01).

## Cause
protocol.md said "the client sends `ping` every 30 s of silence" and "either side may drop a
connection silent for 90 s". Each side read silence as its own side's:
- the panel (`ha_link.c`, `IDLE_LIMIT_MS`) counts what it receives: right for finding a client
  that is gone;
- the integration (`coordinator.py` `_async_listen`) pinged only after 30 s in which it had heard
  nothing. The panel sends an update every 10 s (Home Assistant's log above), so it always heard
  something and never pinged, and while no entity it follows changed it sent nothing at all.

The panel heard nothing for 90 s and closed the link. The integration's tests never caught it:
their fake panel sends nothing on its own.

## Fix
- The integration pings after 30 s in which it has *sent* nothing, as well as after 30 s in which
  it has heard nothing; 90 s without hearing anything is still a dead link. `PanelClient.send`
  keeps the time a frame last went out whole (`last_sent`).
- Two tests in `tests/test_init.py`: a fake panel talking every 0.05 s still gets pings (it got 1
  in 1.5 s before the fix, at 0.2 s keepalives), and a quiet one is pinged on one connection.
  165 tests and Ruff pass.
- protocol.md: "the client sends `ping` after 30 s in which it has sent nothing, and after 30 s in
  which it has heard nothing".

## On the panel
Board 3 (v1 silicon, firmware of `bl-168-automations` at cdae263 with the `zone bind ha` console
command), Home Assistant 2026.9.4 in Docker on another subnet, added by address: with the fix,
no drop in the 31 minutes that followed but the two the run caused (the panel's link switched
off, the panel flashed), where the old integration had dropped five times in eight minutes, every
96 s (07:29:25, 07:31:01, 07:32:37, 07:34:13, 07:35:49).

## Left
- Release the integration fix (`RoboAlarms/ha-addon` 7b6671a): every installed copy of
  v0.1.0 drops a quiet house's link every 95 s.
- The integration ignored a `ping` from the panel (protocol.md lists `ping`/`pong` both ways):
  it answers `pong` now, in the same commit (BL-173).
