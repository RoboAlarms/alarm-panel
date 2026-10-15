"""Check an already-built release artifact; no device access or provisioning."""
import argparse
from pathlib import Path
from panelctl import image_info


def check(build, config):
    settings = config.read_text(encoding="utf-8")
    assert "CONFIG_ALARM_BUILD_RELEASE=y" in settings, "Not a release configuration"
    for flag in ("ALARM_BUILD_DEV", "ALARM_SERVICE_CONSOLE",
                 "ALARM_CRYPTO_DIAGNOSTICS", "HA_LINK_DIAGNOSTICS", "BOARD_BACKLIGHT_FORCE_OFF"):
        assert f"CONFIG_{flag}=y" not in settings, f"Forbidden release flag: {flag}"
    link_map = (build / "alarm_crowpanel_p4-release.map").read_text(encoding="utf-8")
    for symbol in ("panel_web_start", "dev_remote_set_allowed"):
        assert symbol in link_map, f"Missing runtime access control: {symbol}"
    data = (build / "alarm_crowpanel_p4-release.bin").read_bytes()
    assert image_info(data)["project"] == "alarm_crowpanel_p4-release"
    assert b"/api/v1/login" in data, "Missing panel-hosted manager"
    assert b"/dev/v1/speaker" not in data, "Bench silence in a release image"
    assert b"/dev/v1/play" not in data, "Audio bench in a release image"
    print("PASS release profile, physical console exclusion, signed identity and runtime access controls")


if __name__ == "__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build",type=Path,required=True)
    p.add_argument("--config",type=Path,required=True)
    args=p.parse_args(); check(args.build,args.config)
