"""Temporary loopback UI test transport. NOT a product server or deployment tool.

Uses an exact panel certificate pin, forwards only read-only API operations plus
session login/logout and remote pointer events. All alarm/configuration mutations
are blocked here. The shipped firmware serves the same assets directly over HTTPS.
"""
import argparse
import http.server
import json
from pathlib import Path
import threading
import time
from urllib.parse import urlsplit
from panel_web_hil import Panel,Client

def run(args):
    panel=Panel(args.host,args.pin)
    lock=threading.Lock()
    assets=Path(__file__).resolve().parents[2]/'components'/'services'/'panel_web'/'web'
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self,*_):pass
        def do_GET(self):self.handle_request()
        def do_POST(self):self.handle_request()
        def handle_request(self):
            allowed_origin='http://127.0.0.1:'+str(self.server.server_port)
            if self.headers.get('Host')!='127.0.0.1:'+str(self.server.server_port):return self.send_error(403)
            if self.headers.get('Sec-Fetch-Site')=='cross-site':return self.send_error(403)
            if self.command=='POST' and self.headers.get('Origin')!=allowed_origin:return self.send_error(403)
            path=urlsplit(self.path).path
            files={'/':'panel.html','/panel.css':'panel.css','/panel.js':'panel.js','/panel-pages.js':'panel-pages.js','/panel-keypad.js':'panel-keypad.js'}
            if path in files and self.command=='GET':
                data=(assets/files[path]).read_bytes();self.send_response(200)
                self.send_header('Content-Type','text/html' if path=='/' else 'text/css' if path.endswith('.css') else 'application/javascript')
                self.send_header('Cache-Control','no-store');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data);return
            if not path.startswith('/api/v1/'):return self.send_error(404)
            if self.command=='POST' and path not in ('/api/v1/login','/api/v1/logout','/api/v1/control','/api/v1/sources/search'):return self.send_error(403,'Read-only UI test transport')
            length=int(self.headers.get('Content-Length','0'))
            if length>49152:return self.send_error(413)
            data=json.loads(self.rfile.read(length)) if length else None
            headers={'Cookie':self.headers.get('Cookie',''),'X-Panel-CSRF':self.headers.get('X-Panel-CSRF','')}
            with lock:
                try:
                    # Keep TLS alive for realistic frame pacing; failures below reset it.
                    status,body,result=panel.request(path,data,self.command,headers)
                except Exception:
                    panel.close();return self.send_error(502,'Panel connection interrupted')
            self.send_response(status)
            for key,value in result.items():
                if key.lower() not in ('content-length','connection','transfer-encoding'):
                    if key.lower()=='set-cookie':value=value.replace('; Secure','')
                    self.send_header(key,value)
            self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
    client=Client(args.device,args.host)
    grant=json.loads(client.request('POST','browser-grant',b''))['grant'];client.close()
    url='http://127.0.0.1:'+str(server.server_port)+'/#grant='+grant
    Path(args.url_file).write_text(url,encoding='utf-8')
    print('Read-only panel browser test transport on loopback port',server.server_port,flush=True)
    threading.Thread(target=lambda:(time.sleep(1800),server.shutdown()),daemon=True).start()
    try:server.serve_forever()
    finally:panel.close();server.server_close()
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--host',required=True);p.add_argument('--pin',required=True);p.add_argument('--device',required=True);p.add_argument('--url-file',default='build/web-browser-test-url.txt');run(p.parse_args())
