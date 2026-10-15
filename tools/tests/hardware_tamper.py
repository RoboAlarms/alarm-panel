"""Explicit authorized HIL: upload a tampered dev app with its correct transport
hash. Signature validation must reject it without selecting a boot slot.
"""
import argparse
import hashlib
from pathlib import Path
import secrets
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import panelctl as p

a=argparse.ArgumentParser(description=__doc__); a.add_argument("--device",required=True); a.add_argument("--image",type=Path,required=True)
args=a.parse_args(); c=p.Client(args.device); before=c.info()
assert before["idle"] and not before["probation"]
data=bytearray(args.image.read_bytes()); info=p.image_info(data)
assert info["project"].endswith("-dev")
data[len(data)//2]^=1
try:
    c.request("PUT","upload",data,{"X-Image-SHA256":hashlib.sha256(data).hexdigest(),
                                   "X-Operation":secrets.token_hex(16),"X-Allow-Older":"yes"})
except p.DeviceError as exc:
    assert exc.status==400, str(exc)
    print("PASS corrupted application rejected: "+str(exc),flush=True)
else: raise AssertionError("Tampered firmware was staged")
after=c.info(); c.close()
assert after["boot"]==before["boot"] and after["elf"]==before["elf"] and not after["staged_operation"]
print("PASS running application and boot unchanged",flush=True)
