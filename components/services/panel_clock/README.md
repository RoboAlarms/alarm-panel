# panel_clock

**Tier:** services (ESP-IDF) · **Features:** [35 Battery-backed clock](../../../docs/features/35-Battery-Backed-Clock/README.md) ·
**Tests:** the rules are host-tested in [`rtc_clock`](../../core/rtc_clock/test/); the service on a
panel (docs/features/35 "Validation")

The battery-backed clock module. Most panels have none, and for them nothing here changes
anything: start-up probes the board's I2C connector once, finds nothing, logs one line and
starts no task. With a DS3231 there:

- **Start-up** (`panel_clock_start`, from `panel_app` right after the board's power, before the
  event log and the engine): one read of the register file; a module that kept a valid time sets
  the system clock (`settimeofday`), so the first event of the start-up carries the time and no
  "clock not set" trouble is raised; the control and status registers are set up for the coin
  cell (oscillator on battery, no square wave, 32 kHz output off).
- **The `clock` task** (3.5 KB internal stack, priority 3): after every time-server sync
  (`net_wifi_time_syncs()`) and after a person sets the time, writes the module at the system
  clock's next whole second (writing the seconds register restarts the module's second, so the
  two tick together) and clears the oscillator-stop flag; a minute after start-up or a write,
  then hourly, reads the module on its second tick (polling the seconds register every 2 ms)
  and lets `clock_keeper` decide: nothing, slew the system clock (`adjtime`), or put the module
  back.
- **By hand** (`panel_clock_set`, from the touchscreen and the Panel Manager through
  `ui_port_esp`, and `rtc set` at the console): only while a module answers; sets the system
  clock at once, the module at its next second, and clears "clock not set"
  (`net_wifi_clock_set()`).

## Public API
| Header | What it offers |
|---|---|
| `include/panel_clock.h` | `panel_clock_place_rules()`, `panel_clock_stored_place()` / `panel_clock_stored_custom()` (`rtc place`, NVS `clock`/`place`, RTC-013; `rtc place custom`, `clock`/`c_sda` `c_scl`, RTC-016: the app opens the bus of the chosen place), `panel_clock_start(bus, where, place)`, `panel_clock_get()` (`panel_clock_status_t`), `panel_clock_set(epoch_ms, who)`, `panel_clock_console_register()` (`rtc`, `rtc set <epoch s>`, `rtc check`, `rtc place <place>|default`, `rtc place custom sda=<g> scl=<g>`: replies start "place custom from the next restart (`restart`)" or "not custom: ", read back as "custom sda=.. scl=.." before the `rtc module=` line) |

## Depends on
- Project components: `rtc_clock`, `rtc_ds3231`, `platform_esp` (`net_wifi`), `board_info` (`board_custom_parse`)
- ESP-IDF: `esp_driver_i2c`, `console`, `esp_timer`

## Options
`CONFIG_ALARM_FEATURE_RTC` (`components/services/alarm_features`, on for the CrowPanel, off for
the E32R40T whose I2C header pins are zone terminals). Off, the sources compile to stubs: `get`
says not built, `set` answers `ESP_ERR_NOT_SUPPORTED`, and `rtc` says "not in this firmware".

## Rules
- Never waits on the module for longer than one bounded transaction (50 ms), and never touches
  the bus from the alarm or LVGL task.
- The system clock is changed only here and by SNTP; the keeper's rules say which wins.
- `rtc`'s first two output lines are read by the Flasher
  (`tools/flasher/roboflash/clock_test.py`): change them together.
