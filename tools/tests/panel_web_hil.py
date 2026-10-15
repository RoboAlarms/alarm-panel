"""Read/negative validation of Panel Manager on an explicitly selected enrolled panel.

An exact TLS certificate fingerprint obtained through the physical console is required.
Never guesses a PIN, arms the alarm, edits users, or changes network settings.
"""
import argparse
import hashlib
import hmac
import http.client
import json
from pathlib import Path
import ssl
import sys
import time
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from panelctl import Client

class Panel:
    def __init__(self,host,pin):
        self.host,self.pin=host,pin
        self.cookie=self.csrf=''
        self.conn=None
    def connect(self):
        ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.minimum_version=ssl.TLSVersion.TLSv1_2
        ctx.check_hostname=False
        ctx.verify_mode=ssl.CERT_NONE # exact, out-of-band identity pin below replaces CA verification
        conn=http.client.HTTPSConnection(self.host,443,context=ctx,timeout=15)
        conn.connect()
        if not hmac.compare_digest(hashlib.sha256(conn.sock.getpeercert(True)).hexdigest(),self.pin):
            conn.close();raise ssl.SSLError('Panel TLS identity mismatch')
        self.conn=conn
    def request(self,path,data=None,method=None,headers=None):
        if self.conn is None:self.connect()
        h={'Origin':'https://'+self.host,'Cookie':self.cookie,'X-Panel-CSRF':self.csrf}
        if data is not None:h['Content-Type']='application/json';data=json.dumps(data)
        if headers:h.update(headers)
        self.conn.request(method or ('POST' if data is not None else 'GET'),path,data,h)
        r=self.conn.getresponse();payload=r.read();cookie=r.getheader('Set-Cookie')
        if cookie:self.cookie=cookie.split(';')[0]
        return r.status,payload,dict(r.getheaders())
    def login_enrolled(self,device):
        client=Client(device,self.host)
        grant=json.loads(client.request('POST','browser-grant',b''))['grant'];client.close()
        status,body,_=self.request('/api/v1/login',{'grant':grant})
        assert status==200,(status,body)
        self.csrf=json.loads(body)['csrf']
    def get(self,path):
        status,payload,_=self.request('/api/v1/'+path)
        assert status==200,(path,status,payload[:200])
        return json.loads(payload)
    def close(self):
        if self.conn:self.conn.close();self.conn=None

def run(args):
    out=Path(args.output);out.mkdir(parents=True,exist_ok=True)
    panel=Panel(args.host,args.pin)
    status,body,headers=panel.request('/')
    assert status==200 and b'/panel.js' in body
    assert headers.get('X-Frame-Options')=='DENY'
    assert panel.request('/api/v1/status')[0]==401
    assert panel.request('/api/v1/login',{'code':''},headers={'Origin':'https://attacker.invalid'})[0]==403
    panel.login_enrolled(args.device)
    st=panel.get('status');assert st['panel_id'] and st['capabilities']['width']==1024
    # Since the per-path access table (2026-09-24) a grant session has its user's authority:
    # installer-only pages (alarm) answer 403 unless that user is the installer.
    refused=[]
    for path in ['alarm','people','history','time','updates','config/display','config/network']:
        status,payload,_=panel.request('/api/v1/'+path)
        if status==403 and path=='alarm' and st.get('authority')!='installer':
            refused.append(path);continue
        assert status==200,(path,status,payload[:200])
        data=json.loads(payload)
        text=json.dumps(data)
        assert 'code_hash' not in text and 'code_salt' not in text
    for provider in st['capabilities']['integrations']:
        if provider=='home_assistant':panel.get(provider);continue
        data=panel.get('config/'+provider)
        for field in data['fields']:
            if field['type']=='password':assert field['name'] not in data['values']
    assert panel.request('/api/v1/config/display',{'values':{'brightness':101}})[0]==400
    assert panel.request('/api/v1/config/display',{'values':{'saver_after_ms':1}})[0]==400
    assert panel.request('/api/v1/config/display',{'values':{}},headers={'X-Panel-CSRF':'bad'})[0]==401
    assert panel.request('/api/v1/config/display',{'values':{}},headers={'Origin':'https://attacker.invalid'})[0]==401
    assert panel.request('/api/v1/remote',{'web':False,'developer':False})[0]==403
    sizes=[]
    for i in range(args.frames):
        status,data,_=panel.request('/api/v1/frame')
        if status==503:time.sleep(1);continue
        assert status==200,(status,data[:100])
        assert data[:2]==b'\xff\xd8' and data[-2:]==b'\xff\xd9'
        sizes.append(len(data))
        if i==0:(out/'full-resolution.jpg').write_bytes(data)
        time.sleep(1)
    assert sizes,'No frames captured'
    after=panel.get('status')
    assert after['health']['display']['late']==0 and after['health']['display']['guard_trips']==0
    assert after['health']['stale']==0
    panel.request('/api/v1/logout',{})
    assert panel.request('/api/v1/status')[0]==401
    panel.close()
    result={'panel':st['panel_id'],'firmware':st['firmware'],'frames':len(sizes),'jpeg_bytes_min':min(sizes),'jpeg_bytes_max':max(sizes),'health':after['health'],'checks':'TLS pin, embedded assets, auth, CSRF, role denial, secret redaction, range validation, JPEG, logout','refused_for_this_authority':refused}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host',required=True);p.add_argument('--pin',required=True);p.add_argument('--device',required=True)
    p.add_argument('--output',default='build/web-hil');p.add_argument('--frames',type=int,default=20)
    run(p.parse_args())
