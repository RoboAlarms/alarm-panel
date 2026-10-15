"""Explicit real-device acceptance (never part of automatic unit tests).

Run only with owner authorization on an idle enrolled development panel.
--rollback-image stages/boots that signed dev app, withholds confirmation and
requires return to the original ELF within four minutes. No alarm commands.
"""
import argparse
import hashlib
import http.client
import json
from pathlib import Path
import secrets
import ssl
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import panelctl as p


def rejected(call, status="409"):
    try: call()
    except RuntimeError as exc:
        assert status in str(exc), str(exc)
        return
    raise AssertionError("Forbidden operation unexpectedly succeeded")


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device",required=True)
    parser.add_argument("--rollback-image")
    args=parser.parse_args()
    c=p.Client(args.device)
    status=c.info()
    assert status["idle"] and not status["probation"], "Panel must be idle and outside probation"
    boot=status["boot"]
    # TLS without the enrolled client certificate must not serve even status.
    ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname=False; ctx.verify_mode=ssl.CERT_NONE
    unauth=http.client.HTTPSConnection(c.host,c.device["port"],context=ctx,timeout=8)
    try:
        unauth.request("GET","/dev/v1/info")
        response=unauth.getresponse()
        assert response.status==403, "Unauthenticated status was exposed"
    except (OSError,http.client.HTTPException): pass
    finally: unauth.close()
    print("PASS missing client certificate rejected",flush=True)
    lease=c.request("POST","control",boot+" acquire").decode()
    assert len(lease)==32
    rejected(lambda:c.request("POST","control",boot+" acquire"))
    rejected(lambda:c.request("POST","control",boot+" "+"0"*32+" input 1 0 0 0"))
    c.request("POST","control",f"{boot} {lease} input 1 0 0 0")
    rejected(lambda:c.request("POST","control",f"{boot} {lease} input 1 0 0 0"))
    time.sleep(2.5)
    rejected(lambda:c.request("POST","control",f"{boot} {lease} input 2 0 0 0"))
    print("PASS second controller, wrong lease, replay, expired lease rejected",flush=True)
    lease=c.request("POST","control",boot+" acquire").decode()
    c.request("POST","control",f"{boot} {lease} release")
    print(c.request("GET","diagnostics").decode(),flush=True)
    data=Path(args.rollback_image).read_bytes() if args.rollback_image else None
    if data:
        candidate=p.image_info(data)
        assert candidate["elf"]!=status["elf"], "Rollback test needs a different candidate ELF"
        op=secrets.token_hex(16)
        headers={"X-Operation":op,"X-Image-SHA256":hashlib.sha256(data).hexdigest(),"X-Allow-Older":"yes"}
        print("Uploading rollback candidate",flush=True)
        staged=json.loads(c.request("PUT","upload",data,headers))
        assert staged["staged_elf"]==candidate["elf"]
        c.request("POST","install",op)
        deadline=time.monotonic()+250; saw_candidate=False
        while time.monotonic()<deadline:
            time.sleep(2)
            try: current=c.info()
            except (OSError,RuntimeError,http.client.HTTPException): continue
            if current["boot"]==boot: continue
            if current["elf"]==candidate["elf"]:
                assert current["probation"] and current["confirm_required"], "Candidate accepted without confirmation"
                if not saw_candidate: print("Candidate reached; deliberately withholding confirmation",flush=True)
                saw_candidate=True
            elif current["elf"]==status["elf"]:
                assert saw_candidate and not current["probation"]
                print("PASS unattended rollback restored the known-good image and remote access",flush=True)
                print(json.dumps(current,indent=2),flush=True)
                break
            else: raise AssertionError("Unexpected firmware boot")
        else: raise AssertionError("Candidate failed to roll back on deadline")
    c.close()


if __name__=="__main__": main()
