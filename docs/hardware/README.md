# docs/hardware

Vendor documentation for the boards this project runs on. It isn't ours, so the repository
carries only this list of where to get it. Everything else in this folder is git-ignored:
save the vendor's pages and datasheets here so the schematics and pin tables are readable
offline while you work on the firmware (and so an AI assistant working in the repository can
answer pin questions without a network). Read them, never edit them; nothing in the build
depends on them.

| Board or part | Where to get it | Suggested local copy |
|---|---|---|
| Elecrow CrowPanel Advanced 7" (ESP32-P4), board rev V1.1: schematics, pinout, display and touch details | [Elecrow wiki](https://wiki.elecrow.com/CrowPanel_Advanced_7inch_ESP32-P4_HMI_AI_Display_1024x600_IPS_Touch_Screen_with_WiFi6_Compatible_with_ArduinoLVGL.html) (save the page with its images) | `CrowPanel7inScreen/WikiPage.html` |
| Elecrow CrowPanel Advanced 10.1" (ESP32-P4), board rev V1.2: schematic (Eagle and PDF), ESP-IDF lessons | [Elecrow's repository for the 10.1"](https://github.com/Elecrow-RD/CrowPanel-Advanced-10.1inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen) (`Eagle_SCH&PCB/1.2/`, `example/V1.2/idf-code/`) | `CrowPanel10in/` |
| Elecrow ESP32-H2 wireless module for the CrowPanel Advanced (V1.1): pinout, USB, buttons | Elecrow's datasheet for the module, linked from the wiki page above | `Wireless_Module-ESP32-H2_Datasheet.pdf` |

The feature documents cite these copies by the names in the last column (docs/features/06,
docs/features/25 and the research notes in `docs/research/`).
