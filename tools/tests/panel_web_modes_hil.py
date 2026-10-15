"""Verify runtime listener shutdown and session revocation, with explicit USB recovery."""
import argparse
import json
from pathlib import Path
import socket
import time
import serial
from panel_web_hil import Panel, Client

def run(args):
    client = Client(args.device, args.host)
    before = client.info()
    assert before['idle'] and not before['probation']
    client.close()
    panel = Panel(args.host, args.pin)
    panel.login_enrolled(args.device)
    state = panel.get('status')
    assert state['web'] and state['developer'], 'This test restores an initially enabled development panel only'
    panel.close()
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = port.rts = False
    port.port = args.port
    port.open()
    def command(text):
        port.write((text + '\r').encode())
        until = time.monotonic() + 3
        while time.monotonic() < until:
            port.read(4096)
    def reachable(number):
        try:
            with socket.create_connection((args.host, number), timeout=1):
                return True
        except OSError:
            return False
    def wait_port(number, desired):
        until = time.monotonic() + 20
        while time.monotonic() < until:
            if desired:
                try:
                    if number == 443:
                        panel.close()
                        assert panel.request('/')[0] == 200
                        panel.close()
                    else:
                        client = Client(args.device, args.host)
                        try:
                            client.info()
                        finally:
                            client.close()
                    return
                except OSError:
                    panel.close()
            elif not reachable(number):
                return
            time.sleep(1)
        raise AssertionError(f'Port {number} did not become reachable={desired}')
    try:
        command('web developer off')
        wait_port(6055, False)
        wait_port(443, True)
        assert panel.request('/')[0] == 200
        assert panel.request('/api/v1/status')[0] == 401, 'Previous session survived mode change'
        panel.close()
        print('Developer off: developer listener closed, household HTTPS remained, old session revoked', flush=True)
        command('web off')
        wait_port(443, False)
        wait_port(6055, False)
        print('Web off: both network listeners closed', flush=True)
    finally:
        command('web developer on')
        port.close()
    wait_port(443, True)
    wait_port(6055, True)
    panel.close()
    assert panel.request('/api/v1/status')[0] == 401
    panel.login_enrolled(args.device)
    after = panel.get('status')
    assert after['web'] and after['developer'] and after['health']['stale'] == 0
    panel.request('/api/v1/logout', {})
    panel.close()
    Path(args.output).write_text(json.dumps({'passed': True, 'restored': True, 'health': after['health']}, indent=2) + '\n')
    print('Runtime mode shutdown, session revocation and restoration PASSED', flush=True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('host', 'pin', 'device', 'port', 'output'):
        parser.add_argument('--' + name, required=True)
    run(parser.parse_args())
