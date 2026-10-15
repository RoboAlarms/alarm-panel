"""Explicit read-only load acceptance on an authorized enrolled development panel.
Run alongside serial 'display stress 20' and 'crypto_test 100' on a dark build.
"""
import argparse
import json
from pathlib import Path
import sys
import time
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import panelctl as p

a=argparse.ArgumentParser(description=__doc__); a.add_argument("--device",required=True); a.add_argument("--seconds",type=int,default=35)
a.add_argument("--max-crypto-faults",type=int,default=0,help="Explicit allowance for the serial crypto diagnostic's deliberate injections")
args=a.parse_args(); c=p.Client(args.device); before=c.info()
assert before["idle"] and before["display"]["forced_off"], "Use an idle dark build"
deadline=time.monotonic()+args.seconds; frames=0; skipped=0; smallest=before["heap_internal"]
while time.monotonic()<deadline:
    status=c.info(); assert status["boot"]==before["boot"], "Unexpected reboot"
    assert status["health_stale"]==0 and status["crypto_faults"]<=args.max_crypto_faults
    smallest=min(smallest,status["heap_internal"])
    try: raw=c.request("GET","frame")
    except RuntimeError as exc:
        if "Device 503" not in str(exc): raise
        skipped+=1; time.sleep(.1); continue  # Optional capture yields to local rendering.
    assert len(raw)==status["capture_width"]*status["capture_height"]*2
    frames+=1
after=c.info(); c.close()
assert after["display"]["guard_trips"]==before["display"]["guard_trips"]
assert after["display"]["late"]==before["display"]["late"]
assert frames>5, "Capture made insufficient progress"
print(json.dumps({"frames_received":frames,"busy_frames_skipped":skipped,"lowest_sampled_internal":smallest,"before":before,"after":after},indent=2))
