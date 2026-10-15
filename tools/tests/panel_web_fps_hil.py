"""Measure bounded full-resolution capture on an explicitly selected panel.

Read-only; no input, codes, arming or configuration writes. Run alongside the
physical console's dark display stress for higher-rate capture acceptance.
"""
import argparse,json,statistics,time
from pathlib import Path
from panel_web_hil import Panel

def run(args):
    p=Panel(args.host,args.pin);p.login_enrolled(args.device)
    before=p.get('status');times=[];captures=[];sizes=[];responses={};start=time.monotonic()
    while time.monotonic()-start<args.seconds:
        t=time.monotonic();status,body,headers=p.request('/api/v1/frame')
        responses[status]=responses.get(status,0)+1
        if status==200:
            assert body[:2]==b'\xff\xd8' and body[-2:]==b'\xff\xd9'
            times.append((time.monotonic()-t)*1000);sizes.append(len(body))
            timing=headers.get('Server-Timing','')
            if timing.startswith('capture;dur='):captures.append(float(timing.split('=')[1]))
        else:assert status in (429,503),(status,body)
        time.sleep(max(.01,args.interval-(time.monotonic()-t)))
    elapsed=time.monotonic()-start;after=p.get('status')
    assert after['health']['stale']==0
    assert after['health']['display']['late']==before['health']['display']['late']==0
    assert after['health']['display']['guard_trips']==before['health']['display']['guard_trips']==0
    assert len(times)>0
    result={'frames':len(times),'seconds':elapsed,'fps':len(times)/elapsed,'responses':responses,
            'response_ms_median':statistics.median(times),'response_ms_p95':sorted(times)[int((len(times)-1)*.95)],
            'capture_ms_median':statistics.median(captures) if captures else None,
            'jpeg_bytes_median':statistics.median(sizes),'health':after['health']}
    p.request('/api/v1/logout',{});p.close()
    Path(args.output).write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps(result,indent=2))
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('host','pin','device','output'):parser.add_argument('--'+name,required=True)
    parser.add_argument('--seconds',type=float,default=25);parser.add_argument('--interval',type=float,default=.105)
    run(parser.parse_args())
