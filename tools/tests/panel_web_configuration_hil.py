"""Bounded API and reversible configuration acceptance on an explicitly selected panel.

Never arms, changes codes, sends notifications, formats media, or changes the
actual network profile. The network trial submits the existing profile unchanged.
"""
import argparse
import json
from pathlib import Path
import secrets
import time
from panel_web_hil import Panel

def run(args):
    p = Panel(args.host, args.pin)
    p.login_enrolled(args.device)
    original = p.get('config/display')['values']
    st = p.get('status')
    assert not st['program'] and all(x['state'] == 'disarmed' for x in st['partitions'])
    for path in ('storage', 'sources', 'binding/1', 'network-check', 'wifi-scan', 'notification-status', 'developer/logs', 'zone-status'):
        p.get(path)
    print('All service configuration reads passed', flush=True)
    for raw in ('{"values":{"brightness":1,"brightness":50}}',
                '{"values":{},"values":{"brightness":50}}',
                '{"values":{"brightness":"50"}}',
                '{"values":{"brightness":50.5}}',
                '{"values":{"saver_after_ms":1}}',
                '{"values":{"unknown":0}}',
                '{"values":{"brightness":NaN}}',
                '{"values":{"unknown":"\\u0000"}}',
                '{"values":{}}\0{"values":{"brightness":50}}',
                '{"values":' + '[' * 20 + '0' + ']' * 20 + '}'):
        if p.conn is None:
            p.connect()
        p.conn.request('POST', '/api/v1/config/display', raw, {
            'Origin': 'https://' + args.host, 'Cookie': p.cookie,
            'X-Panel-CSRF': p.csrf, 'Content-Type': 'application/json'})
        response = p.conn.getresponse()
        body = response.read()
        assert response.status == 400, (raw, response.status, body)
    assert p.get('config/display')['values'] == original
    print('Malformed, duplicate, embedded NUL, excessive nesting and range inputs rejected', flush=True)
    try:
        code, body, _ = p.request('/api/v1/config/display', {'values': {'key_click': not original['key_click']}})
        assert code == 200, (code, body)
        assert p.get('config/display')['values']['key_click'] != original['key_click']
    finally:
        code, body, _ = p.request('/api/v1/config/display', {'values': {'key_click': original['key_click']}})
        assert code == 200, (code, body)
    assert p.get('config/display')['values'] == original
    print('Preference save/readback/restore passed', flush=True)
    network = p.get('config/network')['values']
    for confirm in (True, False):
        operation = secrets.token_hex(16)
        try:
            code, body, _ = p.request('/api/v1/config/network', {'values': network, 'operation': operation})
            assert code == 200, (code, body)
        except (OSError, TimeoutError):
            print('Network reconfiguration dropped the old connection; checking the operation after reconnect', flush=True)
        p.close()
        if confirm:
            time.sleep(5)
            code, body, _ = p.request('/api/v1/network-confirm', {'operation': operation})
            assert code == 200, (code, body)
            print('Confirmed unchanged network trial passed', flush=True)
        else:
            print('Waiting for the unchanged network trial to roll back automatically', flush=True)
            time.sleep(125)
            code, _, _ = p.request('/api/v1/network-confirm', {'operation': operation})
            assert code == 409, 'Expired trial was incorrectly accepted'
        assert p.get('config/network')['values'] == network
    after = p.get('status')
    assert after['health']['stale'] == 0
    assert after['health']['display']['late'] == 0 and after['health']['display']['guard_trips'] == 0
    p.request('/api/v1/logout', {})
    p.close()
    Path(args.output).write_text(json.dumps({'passed': True, 'health': after['health'], 'network_restored': True, 'preferences_restored': True}, indent=2) + '\n')
    print('Configuration and timed network rollback acceptance PASSED', flush=True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('host', 'pin', 'device', 'output'):
        parser.add_argument('--' + name, required=True)
    run(parser.parse_args())
