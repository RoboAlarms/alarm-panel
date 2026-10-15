"""Exercise the browser's signed upload protocol on an explicitly selected idle panel."""
import argparse
import hashlib
import json
from pathlib import Path
import time
from panel_web_hil import Panel, Client

def run(args):
    client = Client(args.device, args.host)
    before = client.info()
    client.close()
    print('Selected panel:', before['id'], flush=True)
    panel = Panel(args.host, args.pin)
    panel.login_enrolled(args.device)
    status = panel.get('status')
    assert not status['program'] and all(p['state'] == 'disarmed' for p in status['partitions']), 'Panel must be disarmed'
    assert not panel.get('updates')['confirming'], 'Previous image is still confirming'
    image = Path(args.image).read_bytes()
    panel.conn.sock.settimeout(330)
    # This is the same raw PUT, digest and CSRF used by XMLHttpRequest in the browser.
    panel.conn.request('PUT', '/api/v1/upload', image, {
        'Origin': 'https://' + args.host, 'Cookie': panel.cookie,
        'X-Panel-CSRF': panel.csrf, 'X-Image-SHA256': hashlib.sha256(image).hexdigest(),
        'X-Allow-Older': 'yes', 'Content-Type': 'application/octet-stream'})
    response = panel.conn.getresponse()
    body = response.read()
    assert response.status == 200, (response.status, body)
    staged = json.loads(body)
    print('Signed image staged and verified:', staged['elf'], flush=True)
    code, body, _ = panel.request('/api/v1/upload-install', {'operation': staged['operation']})
    assert code == 200, (code, body)
    panel.close()
    deadline = time.monotonic() + 240
    time.sleep(4)
    while time.monotonic() < deadline:
        try:
            code, body, _ = panel.request('/api/v1/update-confirm', {'operation': staged['operation'], 'elf': staged['elf']})
            if code == 200 and json.loads(body).get('complete'):
                client = Client(args.device, args.host)
                after = client.info()
                client.close()
                result = {'before': before, 'after': after, 'expected_elf': staged['elf'], 'complete': True}
                Path(args.output).write_text(json.dumps(result, indent=2) + '\n')
                print('Browser upload, reboot, expected-image reconnect and probation PASSED', flush=True)
                return
        except Exception as error:
            print('Waiting for panel:', type(error).__name__, flush=True)
        panel.close()
        time.sleep(3)
    raise RuntimeError('Expected firmware did not confirm; inspect authenticated status and rollback')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('host', 'pin', 'device', 'image', 'output'):
        parser.add_argument('--' + name, required=True)
    run(parser.parse_args())
