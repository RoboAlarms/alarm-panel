#!/usr/bin/env python3
"""Mock-ups of the Z-Wave screens (docs/features/36 screens.md), drawn as SVG in the style of the
7" UI (1024x600: rail, status bar, Settings column, page). These are plans for review, not
the real UI: the real screens come from the LVGL Pro layout and the PC preview
(tools/ui_preview.ps1) once they are built. No dependencies beyond the standard library.

Run: python docs/features/36-Z-Wave-Sensors-ZAC93/screens/make_mockups.py
"""
from pathlib import Path
from xml.sax.saxutils import escape

OUT = Path(__file__).resolve().parent
W, H = 1024, 600
C = {
    "bg": "#0a0e13", "rail": "#0c1117", "row": "#131a22", "rowsel": "#1a2330", "line": "#222c38",
    "text": "#e8edf2", "gray": "#8b98a5", "dim": "#5c6773",
    "blue": "#5aa0ff", "amber": "#f5b93b", "green": "#34d399", "red": "#f87171",
    "violet": "#a78bfa", "teal": "#2dd4bf", "new": "#5aa0ff",
}
FONT = "Inter, 'Segoe UI', Arial, sans-serif"
PX, PW = 444, 556  # the page column


class Svg:
    def __init__(self):
        self.parts = [f'<rect width="{W}" height="{H}" fill="{C["bg"]}"/>']

    def add(self, s):
        self.parts.append(s)

    def text(self, x, y, s, size=18, color="text", weight=400, anchor="start", mono=False):
        fam = "Consolas, 'Courier New', monospace" if mono else FONT
        self.add(f'<text x="{x}" y="{y}" font-family="{fam}" font-size="{size}" '
                 f'font-weight="{weight}" fill="{C.get(color, color)}" text-anchor="{anchor}">'
                 f'{escape(s)}</text>')

    def rect(self, x, y, w, h, fill, r=14, stroke=None, sw=1.5, opacity=1.0):
        st = f' stroke="{C.get(stroke, stroke)}" stroke-width="{sw}"' if stroke else ""
        self.add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" '
                 f'fill="{C.get(fill, fill)}" fill-opacity="{opacity}"{st}/>')

    def circle(self, x, y, r, fill, opacity=1.0, stroke=None):
        st = f' stroke="{C.get(stroke, stroke)}" stroke-width="1.5"' if stroke else ""
        self.add(f'<circle cx="{x}" cy="{y}" r="{r}" fill="{C.get(fill, fill)}" '
                 f'fill-opacity="{opacity}"{st}/>')

    def icon(self, x, y, glyph, color, r=20):
        self.circle(x, y, r, color, 0.16)
        self.text(x, y + 6, glyph, 17, color, 700, "middle")

    def save(self, name):
        body = "\n".join(self.parts)
        (OUT / f"{name}.svg").write_bytes(  # bytes: LF line ends on every OS
            (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" '
             f'height="{H}">\n{body}\n</svg>\n').encode("utf-8"))


def chrome(s: Svg, pill="Installer · Arming off · Tap to leave", pill_color="blue",
           selected="Installer", settings=True):
    s.rect(0, 0, 96, H, "rail", 0)
    s.rect(28, 22, 40, 40, "blue", 10, opacity=0.2)
    s.text(48, 50, "R", 26, "blue", 800, "middle")
    for i, (g, label) in enumerate([("◇", "Home"), ("▦", "Zones"), ("↺", "Activity"),
                                    ("△", "Troubles"), ("♪", "Chime")]):
        y = 110 + i * 76
        s.text(48, y, g, 24, "gray", 400, "middle")
        s.text(48, y + 26, label, 14, "gray", 400, "middle")
    s.text(48, 548, "▲", 22, "red", 400, "middle")
    s.text(48, 572, "Panic", 14, "red", 600, "middle")
    # status bar
    pw = 10 + len(pill) * 10
    s.rect(120, 12, pw, 40, pill_color, 20, opacity=0.14)
    s.text(140, 39, pill, 19, pill_color, 600)
    s.text(842, 43, "12:52", 28, "text", 600)
    s.text(920, 43, "AM", 15, "gray", 500)
    s.circle(978, 32, 22, "row")
    s.text(978, 39, "⚙", 18, "gray", 400, "middle")
    if settings:
        s.text(120, 90, "Settings", 26, "text", 600)
        items = [("☼", "Display and sound", "Brightness and volume", "blue"),
                 ("☺", "Users", "Codes and who can do what", "violet"),
                 ("≋", "Wi-Fi", "Network and signal", "blue"),
                 ("◷", "Date and time", "Time zone, 24-hour clock", "green"),
                 ("⌂", "Integrations", "Smart home and notifications", "violet"),
                 ("≡", "Event log", "Everything the panel did", "green"),
                 ("✎", "Installer", "Zones, delays, codes, reset", "amber"),
                 ("i", "About", "Storage and system", "red")]
        for i, (g, t, sub, col) in enumerate(items):
            y = 110 + i * 60
            if t == selected:
                s.rect(120, y, 300, 56, "rowsel", 14)
            s.icon(152, y + 28, g, col, 18)
            s.text(184, y + 25, t, 19, "text", 600)
            s.text(184, y + 45, sub, 14, "gray")


def page(s: Svg, title, sub=None, back=True, title_color="text"):
    x = PX
    if back:
        s.circle(PX + 22, 94, 22, "row")
        s.text(PX + 22, 101, "‹", 26, "text", 600, "middle")
        x = PX + 56
    s.text(x, 92, title, 27, title_color, 600)
    if sub:
        s.text(x, 120, sub, 18, "gray")


def row(s: Svg, y, glyph, color, title, sub=None, value=None, chevron=True, h=64,
        value_color="gray", title_color="text", highlight=None, sub_color="gray"):
    s.rect(PX, y, PW, h, "row", 14, stroke=highlight)
    s.icon(PX + 40, y + h / 2, glyph, color)
    ty = y + (h / 2 + 7 if not sub else h / 2 - 3)
    s.text(PX + 70, ty, title, 20, title_color, 600)
    if sub:
        s.text(PX + 70, ty + 21, sub, 15, sub_color)
    if value:
        s.text(PX + PW - (44 if chevron else 18), y + h / 2 + 7, value, 19, value_color, 600, "end")
    if chevron:
        s.text(PX + PW - 20, y + h / 2 + 8, "›", 24, "dim", 400, "middle")


def toggle_row(s: Svg, y, title, sub, on=True, h=64):
    s.rect(PX, y, PW, h, "row", 14)
    s.text(PX + 18, y + h / 2 - 3, title, 20, "text", 600)
    s.text(PX + 18, y + h / 2 + 18, sub, 15, "gray")
    s.rect(PX + PW - 84, y + h / 2 - 17, 64, 34, "green" if on else "line", 17)
    s.circle(PX + PW - (38 if on else 66), y + h / 2, 13, "text")


def para(s: Svg, y, lines, color="gray", size=17, x=PX + 4, gap=24):
    for i, ln in enumerate(lines):
        s.text(x, y + i * gap, ln, size, color)


def section(s: Svg, y, label):
    s.text(PX + 4, y, label, 15, "gray", 500)


def button(s: Svg, y, label, color, glyph=None, h=56, x=PX, w=PW, filled=False):
    s.rect(x, y, w, h, color, 14, stroke=color, opacity=0.18 if filled else 0.07)
    s.text(x + w / 2, y + h / 2 + 7, (glyph + "  " if glyph else "") + label, 20, color, 600, "middle")


def step(s: Svg, y, state, title, sub=None):
    glyph, color = {"done": ("✓", "green"), "now": ("•", "blue"), "todo": ("·", "dim"),
                    "fail": ("✕", "red"), "warn": ("!", "amber")}[state]
    s.icon(PX + 22, y, glyph, color, 16)
    s.text(PX + 50, y + 6, title, 19, "text" if state != "todo" else "dim", 600)
    if sub:
        s.text(PX + 50, y + 27, sub, 15, "gray")


def dialog(s: Svg, x, y, w, h, title, sub=None):
    s.rect(0, 0, W, H, "#000000", 0, opacity=0.55)
    s.rect(x, y, w, h, "#10161d", 26, stroke="line")
    s.text(x + 26, y + 50, title, 27, "text", 600)
    if sub:
        s.text(x + 26, y + 80, sub, 18, "gray")
    s.circle(x + w - 46, y + 50, 22, "row")
    s.text(x + w - 46, y + 57, "✕", 16, "gray", 400, "middle")


def keypad(s: Svg, x0, y0, bw=78, bh=58, gap=10):
    keys = ["1", "2", "3", "4", "5", "6", "7", "8", "9", "⌫", "0", "OK"]
    for i, k in enumerate(keys):
        cx, cy = x0 + (i % 3) * (bw + gap), y0 + (i // 3) * (bh + gap)
        col = "green" if k == "OK" else "text"
        s.rect(cx, cy, bw, bh, "row", 14, stroke="green" if k == "OK" else None)
        s.text(cx + bw / 2, cy + bh / 2 + 9, k, 24, col, 600, "middle")


# ---- the screens ---------------------------------------------------------------------------

def s01():
    s = Svg(); chrome(s); page(s, "Installer", "Installer mode · arming is off until you leave", back=False)
    row(s, 136, "≡", "blue", "System options", "Delays, arming, codes, power")
    row(s, 208, "▯", "green", "Zones", "Sensors, types, names", "9 in use")
    row(s, 280, "Z", "new", "Z-Wave", "Sensors on the panel's Z-Wave radio", "5 devices", highlight="new")
    row(s, 352, "⌂", "violet", "System name", "Shown on the screen and in the event log", "Home")
    row(s, 424, "▤", "violet", "Partitions", "Areas that arm separately", "1 in use")
    row(s, 496, "▣", "blue", "Keypads and devices", "Supervised keypads and expanders", "0 in use")
    s.save("01_installer_menu")


def s02():
    s = Svg(); chrome(s); page(s, "Z-Wave", "Sensors paired to the panel's Z-Wave radio")
    row(s, 136, "Z", "green", "Radio · Working", "United States · 5 devices · all heard")
    row(s, 208, "+", "blue", "Add a device", "Pair a sensor with the panel")
    row(s, 280, "−", "gray", "Remove a device", "Also resets a sensor from another system")
    section(s, 370, "Devices")
    row(s, 382, "▯", "green", "Front door", "Zooz ZSE41 · Secure · Battery 92 %", "Zone 3")
    row(s, 454, "◉", "green", "Hall motion", "Zooz ZSE18 · Secure · Battery 80 %", "Zone 5")
    row(s, 526, "≈", "amber", "Zooz ZSE43 tilt/shock", "Not encrypted · Battery 100 %", "Not in a zone",
        sub_color="amber")
    s.save("02_zwave_network")


def s03():
    s = Svg(); chrome(s); page(s, "Z-Wave", "Sensors paired to the panel's Z-Wave radio")
    row(s, 136, "▯", "green", "Basement leak", "Zooz ZSE42 · Secure · Asleep", "Zone 8")
    row(s, 208, "◌", "amber", "Garage motion", "Zooz ZSE18 · Not heard for 2 h 30 min", "Zone 9",
        sub_color="amber")
    section(s, 298, "Network")
    row(s, 310, "⌖", "blue", "Region", "Match your radio", "United States")
    row(s, 382, "◷", "blue", "Check-in for new devices", "How often battery sensors report in", "Every hour")
    row(s, 454, "⟳", "blue", "Rebuild routes", "Refresh how signals reach the panel")
    row(s, 526, "!", "red", "Reset the Z-Wave network", "Every device must be added again",
        title_color="red")
    s.save("03_zwave_network_more")


def s04():
    s = Svg(); chrome(s); page(s, "Add a Z-Wave device", "Listening · 52 s left")
    para(s, 160, ["Put the device in pairing mode now. Most sensors:",
                  "press their Z-Wave button 3 times quickly. Keep it",
                  "within a few metres of the panel."], "text", 18, gap=26)
    step(s, 268, "now", "Listening for the device")
    step(s, 318, "todo", "Connecting")
    step(s, 368, "todo", "Securing")
    step(s, 418, "todo", "Reading what it can do")
    button(s, 470, "Keep listening", "blue", w=270)
    button(s, 470, "Stop", "gray", x=PX + 286, w=270)
    para(s, 560, ["The countdown changes once a second; nothing blinks."], "dim", 14)
    s.save("04_add_listening")


def s05():
    s = Svg(); chrome(s); page(s, "Secure connection", "Zooz ZSE41 asks for its reports to be encrypted")
    toggle_row(s, 140, "Authenticated", "Needs the 5-digit PIN printed on it", True)
    toggle_row(s, 212, "Unauthenticated", "Encrypted, no PIN needed", True)
    para(s, 312, ["Allow what it asks for. Added without encryption, its",
                  "signal could be copied and it can't guard fire or CO.",
                  "4 minutes to decide."], "gray", 17)
    button(s, 400, "Allow", "green", "✓", filled=True)
    button(s, 468, "Add it without encryption", "red", "✕")
    s.save("05_add_security")


def s06():
    s = Svg(); chrome(s); page(s, "Enter its PIN", "The first 5 digits of the key on its label or box")
    for i, d in enumerate("5699"):
        s.rect(PX + i * 58, 146, 50, 64, "row", 12, stroke="blue")
        s.text(PX + i * 58 + 25, 190, d, 30, "text", 600, "middle")
    s.rect(PX + 4 * 58, 146, 50, 64, "row", 12, stroke="line")
    para(s, 256, ["The rest of the key should read:"], "gray", 16)
    para(s, 284, ["56189-31613-49588-54107", "25026-60644-13623"], "text", 19, gap=28)
    para(s, 364, ["If it doesn't match the label,", "this is not your device: stop."], "amber", 16)
    para(s, 432, ["3 min 40 s left"], "gray", 16)
    keypad(s, 744, 140)
    s.save("06_add_pin")


def s07():
    s = Svg(); chrome(s); page(s, "Adding Zooz ZSE41", "Keep the device close to the panel")
    step(s, 160, "done", "Connected", "Device 7")
    step(s, 224, "done", "Secured", "S2 Authenticated")
    step(s, 288, "now", "Reading what it can do", "6 of 12 questions answered")
    step(s, 352, "todo", "Ready to use")
    s.rect(PX, 400, PW, 92, "amber", 14, stroke="amber", opacity=0.08)
    para(s, 432, ["It went to sleep before it finished.", "Wake it: click its Z-Wave button 4 times quickly."],
         "amber", 18, x=PX + 18, gap=28)
    button(s, 510, "Stop and finish later", "gray")
    s.save("07_add_progress")


def s08():
    s = Svg(); chrome(s); page(s, "Added", "Zooz ZSE41 open/close sensor · Secure", title_color="green")
    para(s, 160, ["Encrypted: its signal can't be copied. Checks in every hour."], "green", 17)
    section(s, 204, "Use it for")
    row(s, 216, "▯", "green", "Door or window contact", "Right now: Closed", "New zone")
    row(s, 288, "▯", "blue", "Door or window contact", "For an existing zone", "Pick")
    para(s, 390, ["Tamper and low battery come with the zone:", "nothing else to set up."], "gray", 16)
    button(s, 470, "Not now: keep it without a zone", "gray")
    s.save("08_add_done")


def s09():
    s = Svg(); chrome(s, settings=True)
    dialog(s, 212, 24, 600, 560, "Where is the sensor?", "How the new zone's sensor is connected")
    items = [("⏚", "Wired contact", "A contact wired to the panel's terminals", "blue"),
             ("⌂", "Home Assistant sensor", "Any sensor Home Assistant knows", "blue"),
             ("Z", "Z-Wave sensor", "Paired to the panel's Z-Wave radio", "new"),
             ("◎", "Zigbee sensor", "Paired straight to the panel's own radio", "blue"),
             ("◎", "433 MHz sensor", "Learned by opening and closing it", "blue"),
             ("✎", "Simulator", "No sensor: the service console opens and closes it", "gray")]
    for i, (g, t, sub, col) in enumerate(items):
        y = 130 + i * 72
        if t == "Z-Wave sensor":
            s.rect(236, y - 6, 552, 66, "rowsel", 14, stroke="new")
        s.icon(270, y + 26, g, col)
        s.text(302, y + 20, t, 21, "text", 600)
        s.text(302, y + 44, sub, 15, "gray")
    s.save("09_source_picker")


def s10():
    s = Svg(); chrome(s); page(s, "Pick the sensor", "Z-Wave sensor")
    row(s, 136, "+", "blue", "Add a new Z-Wave device", "Pair it now, then pick its input")
    row(s, 208, "≈", "amber", "Zooz ZSE43 tilt/shock", "tilt · Just moved", "Tilted", value_color="amber")
    row(s, 280, "≈", "blue", "Zooz ZSE43 tilt/shock", "shock", "Still")
    row(s, 352, "▯", "dim", "Front door", "Zooz ZSE41 · door contact · In use: Zone 3", "Closed", chevron=False)
    row(s, 424, "≈", "dim", "Basement leak", "Zooz ZSE42 · water · In use: Zone 8", "Dry", chevron=False)
    para(s, 520, ["Open or move a sensor: it moves to the top and turns amber."], "gray", 16)
    s.save("10_zwave_pick_sensor")


def s11():
    s = Svg(); chrome(s); page(s, "Zone 12", "Zone 12 · saved when you go back")
    row(s, 136, "⌂", "blue", "Name", None, "Side door")
    row(s, 208, "◈", "blue", "Type", None, "Perimeter (03)")
    row(s, 280, "▯", "green", "Sensor", None, "Door")
    row(s, 352, "Z", "blue", "Source", None, "Z-Wave · Zooz ZSE41")
    row(s, 424, "≡", "blue", "Z-Wave device", "Secure · Battery 92 % · Heard 2 min ago")
    s.text(PX + 4, 530, "Right now", 20, "gray")
    s.text(PX + PW - 4, 530, "Closed", 20, "text", 700, "end")
    para(s, 566, ["Works without Wi-Fi. Encrypted. Checks in every hour."], "green", 15)
    s.save("11_zone_page")


def s12():
    s = Svg(); chrome(s); page(s, "Front door", "Zooz ZSE41 open/close sensor · device 7")
    row(s, 136, "⚿", "green", "Security", None, "Secure (S2 Authenticated)", chevron=False)
    row(s, 208, "▮", "green", "Battery", None, "92 %", chevron=False)
    row(s, 280, "◷", "green", "Last heard", "Signal good", "2 min ago", chevron=False)
    row(s, 352, "↻", "blue", "Checks in", "Longer is kinder to its battery", "Every hour")
    row(s, 424, "▯", "blue", "Used by", None, "Zone 3 · Front door")
    row(s, 496, "−", "red", "Remove this device", "Pair it again to remove it", title_color="red")
    s.save("12_device_page")


def s13():
    s = Svg(); chrome(s); page(s, "Garage motion", "Zooz ZSE18 · not heard for 2 h 30 min")
    s.rect(PX, 140, PW, 92, "amber", 14, stroke="amber", opacity=0.08)
    para(s, 172, ["It missed its check-ins. Change its battery or bring it", "closer, then wake it: hold its button 5 seconds."],
         "amber", 17, x=PX + 18, gap=26)
    row(s, 248, "⟳", "blue", "Rebuild its routes", "Waits for it to wake up")
    row(s, 320, "⇄", "blue", "Replace it", "A new sensor takes over its zones")
    row(s, 392, "✕", "red", "Remove it: it's broken or gone", "Only if it can't be removed the normal way",
        title_color="red")
    para(s, 490, ["A sleeping sensor never answers a check: removing one", "that is only asleep stops it working."], "gray", 15)
    s.save("13_device_unreachable")


def s14():
    s = Svg(); chrome(s); page(s, "Remove a Z-Wave device", "Listening · 41 s left")
    para(s, 160, ["Put the device in pairing mode, the same way as", "adding it. This also resets a sensor that belonged",
                  "to another system, so it can be added here."], "text", 18, gap=26)
    button(s, 290, "Stop", "gray")
    section(s, 386, "When it's done")
    s.rect(PX, 398, PW, 110, "green", 14, stroke="green", opacity=0.07)
    step(s, 430, "done", "Removed: Front door (Zooz ZSE41)", "Zone 3 now needs a new sensor: it shows CHECK")
    para(s, 490, ["or: Removed a device from another system: it can be added now."], "gray", 15, x=PX + 50)
    s.save("14_remove")


def s15():
    s = Svg(); chrome(s); page(s, "Region", "Must match where the radio was bought")
    for i, (t, sel) in enumerate([("United States with Long Range", True), ("United States", False)]):
        y = 136 + i * 72
        s.rect(PX, y, PW, 64, "row", 14, stroke="blue" if sel else None)
        s.text(PX + 20, y + 40, t, 20, "text", 600)
        if sel:
            s.text(PX + PW - 24, y + 41, "✓", 22, "blue", 700, "end")
    para(s, 300, ["This radio offers only these two. Devices added in", "another region stop working until they are added again."],
         "amber", 17)
    para(s, 380, ["Changing it restarts the Z-Wave radio (a few seconds)."], "gray", 16)
    s.save("15_region")


def s16():
    s = Svg(); chrome(s); page(s, "Z-Wave", "Sensors paired to the panel's Z-Wave radio")
    row(s, 136, "Z", "green", "Radio · Working", "United States · 5 devices")
    dialog(s, 232, 130, 560, 340, "Reset the Z-Wave network?")
    para(s, 230, ["All 5 devices lose the panel. Each one must be reset",
                  "and added again, and their zones show CHECK until then."], "text", 17, x=258, gap=26)
    button(s, 320, "Reset the network", "red", "!", x=258, w=508)
    button(s, 388, "Cancel", "gray", x=258, w=508)
    s.save("16_reset_confirm")


def s17():
    s = Svg(); chrome(s); page(s, "Added without encryption", "Zooz ZSE43 tilt/shock", title_color="amber")
    para(s, 162, ["The secure connection didn't finish: the PIN was", "wrong, or it took too long. The sensor works, but its",
                  "signal can be copied and it can't guard fire or CO."], "amber", 17, gap=26)
    row(s, 262, "⟳", "blue", "Remove it and try again", "Pair it once to remove it, then add it")
    row(s, 334, "✓", "gray", "Keep it this way", "Its zones will say it isn't encrypted")
    s.save("17_add_insecure")


def s18():
    s = Svg(); chrome(s); page(s, "Waiting to join", "Sensors registered by their QR code")
    row(s, 136, "Z", "blue", "Porch motion", "Zooz ZSE70 · Long Range · power it up", "Waiting")
    row(s, 208, "Z", "amber", "Shed door", "Could not secure it 5 times: check its code", "Stopped",
        value_color="amber", sub_color="amber")
    row(s, 280, "+", "blue", "Register a sensor", "Type its 40-digit key, or scan it with a phone")
    para(s, 380, ["A registered sensor joins by itself when it is powered,",
                  "secured with S2: no buttons to press. Scan QR codes",
                  "with a phone in the Panel Manager (Z-Wave page)."], "gray", 16)
    s.save("18_smartstart")


def s19():
    s = Svg(); chrome(s); page(s, "Z-Wave radio", "What the panel knows about its radio")
    row(s, 136, "Z", "green", "State", None, "Working", chevron=False)
    row(s, 208, "i", "blue", "Radio software", None, "2.30 · Z-Wave 7.24.2", chevron=False)
    row(s, 280, "⌖", "blue", "Region", None, "United States", chevron=False)
    row(s, 352, "≋", "blue", "Background noise", "Highest channel right now", "-94 dBm", chevron=False)
    row(s, 424, "⇄", "blue", "Restart the radio", "Takes a few seconds; zones keep their state")
    para(s, 526, ["Radios older than 2.20 get: \"Update the Z-Wave radio's software\"."], "gray", 15)
    s.save("19_radio_details")


if __name__ == "__main__":
    for fn in [s01, s02, s03, s04, s05, s06, s07, s08, s09, s10, s11, s12, s13, s14, s15, s16,
               s17, s18, s19]:
        fn()
    print("wrote", len(list(OUT.glob("*.svg"))), "SVG files to", OUT)
