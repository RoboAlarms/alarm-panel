# V1.1 native battery sensing

The battery helper **is readable by the ESP32-P4**. This was verified on the development
board on 2026-09-20 without a battery, with USB2.0 powered and UART0 connected to the PC.

## Sources and wiring

- [Elecrow V1.1 schematic](https://github.com/Elecrow-RD/CrowPanel-Advanced-7inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen/blob/master/Eagle_SCH%26PCB/1.1/ESP32-P4%20Display%207.0%20inch%20V1.1.sch):
  U14 STC8H1K08 P1.5/P1.4 reach I2C1 SCL/SDA through R188/R189 (0 ohms), sharing
  GPIO46/45 with touch. R164=39k and R186=100k divide VBAT into U14 ADC10.
- [Elecrow 7-inch factory source](https://github.com/Elecrow-RD/CrowPanel-Advanced-7inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen/blob/master/factory_sourcecode/V1.0/ESP32-P4-Adcance-brookesia_phone_inch7.zip):
  `components/espressif__esp32_p4_function_ev_board/bsp_stc8h1kxx.{c,h}` defines address
  0x2f and the battery bank. Our driver reads that bank only; it does not copy the vendor's
  deep-sleep behavior, flash the helper, or write its GPIO/PWM controls.

| Registers | Meaning |
|---|---|
| 0x00–0x03 | ADC millivolts, little-endian uint32 |
| 0x04–0x07 | Converted battery millivolts, little-endian uint32 |
| 0x08 | Estimated percentage |
| 0x09 | Idle=0, charging=1, full=2, no-charge=3, error=4 |
| 0x0a | LED state, 0..4 |

## Empty-connector finding

Repeated real reads returned `e0 0c 00 00 e5 11 00 00 64 02 02`:
ADC 3296 mV, calculated battery 4581 mV, 100%, full, green LED.
The connector has no battery. R17 also links the battery node to the external input through
5.1k; charger status and a high voltage are not proof of an attached cell.

The driver therefore rejects this as a plausible battery. The UI stays red/crossed-out
and never displays the bogus percentage. It does not claim a definitive insertion switch:
an out-of-range cell and an empty connector cannot be distinguished by this protocol.

## Runtime policy and limits

- Sample on the existing health task, with 25 ms per transaction; the UI and alarm task
  read cached data. Failed or >10 s old data is unavailable. No new task/stack.
- Validate all fields, divider consistency, percentage and 2000..4350 mV cell plausibility.
  Percentage comes from the helper and is an estimate, not measured capacity/runtime.
- Qualify presence/charging/low state for 5 seconds. Low at <=3500 mV, restored at >=3700 mV.
- Charging/full confirms external input. No-charge does **not** prove AC loss. This driver
  intentionally does not synthesize an AC-loss trouble from that ambiguous state.
- A plausible cell without confirmed external input conservatively enables a 20% backlight
  cap and suppresses cosmetic key/chime sounds. Both entering and leaving savings require
  15 seconds of stable evidence. The user's brightness is restored; saver-off stays off.
  Failure retains existing savings. Essential alarm/entry/exit/code feedback remains active.
- No shutdown/deep sleep is initiated. Independent AC-loss detection requires a confirmed
  input-power signal. Battery insertion, charging, discharge and runtime require a battery
  acceptance test; none is available yet (BL-038). Both USB supplies must be accounted for
  during it.

Research/probe logs and downloaded vendor material are git-ignored under
`build/battery-research/`. No vendor archive or helper firmware is redistributed here.
