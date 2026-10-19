#!/usr/bin/env python3
"""link_fw_sim.py - keypad updates through the panel link, on this PC (docs/features/11 "Updates
through the link"; host/link_sim, link_fw.h).

Builds link_sim (as tools/link_sim.ps1 does), starts a virtual master and a virtual keypad on
127.0.0.1 with fresh records, pairs them, and runs the scenarios one after the other on the same
two processes (each one's versions carry on into the next):

  together  the master installs 1.5.0; the keypad fetches it from its own update server, restarts
            with the master and stays on trial until the master's self-test passes
  relay     the keypad's update server can't be reached (no internet): the master relays the image
            it downloaded, then they restart together
  armed     the master is armed away during the download: the relay pauses in the exit time and
            goes on while armed; nothing restarts until the disarm, then at once
  back      the master's self-test fails: it goes back, and the keypad on trial goes back with it
  copy      a second keypad of another model and with no internet is paired: the master fetches a
            copy of its next version for that model and relays it, the first keypad fetches its
            own, and all three restart together

Each scenario prints the rows over time - the master's "fw: keypad ..." lines, the keypad's "fw:"
lines and the events - with the seconds since it began, and checks where both end.

    python tools/link_fw_sim.py                 # all five (about 5 minutes)
    python tools/link_fw_sim.py --only relay    # one (pairing first)
    python tools/link_fw_sim.py -v              # every line of both
Exit code 0 when every scenario ended as it should.
"""
import argparse
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MASTER_PORT = 6156  # not 6056: a panel or another link_sim on this PC keeps its port


class Node:
    """One link_sim process: its lines are read on a thread, each with the time it came."""

    def __init__(self, name, args, verbose):
        self.name = name
        self.verbose = verbose
        self.lines = queue.Queue()
        self.log = []
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, bufsize=1,
                                     encoding='utf-8', errors='replace')
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            line = line.rstrip('\r\n')
            self.lines.put((time.monotonic(), line))

    def send(self, cmd):
        if self.verbose:
            print(f'           {self.name} < {cmd}')
        self.proc.stdin.write(cmd + '\n')
        self.proc.stdin.flush()

    def stop(self):
        try:
            self.send('quit')
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


class Run:
    def __init__(self, master, keypad, verbose):
        self.m = master
        self.k = keypad
        self.nodes = [master, keypad]
        self.verbose = verbose
        self.t0 = time.monotonic()

    def show(self, node, at, line):
        shown = self.verbose or line.startswith(('fw:', 'event:')) or 'link: ' in line
        if node.name == 'master' and line.startswith('event') and 'keypad' not in line \
                and not self.verbose:
            shown = 'Firmware' in line or 'firmware' in line
        if shown:
            print(f'  {at - self.t0:6.1f} s  {node.name:7}  {line}')

    def pump(self, until, want=None):
        """Lines of both until the time `until`, or until want (node, regex) matches: the match."""
        while True:
            got = False
            for node in self.nodes:
                try:
                    at, line = node.lines.get_nowait()
                except queue.Empty:
                    continue
                got = True
                node.log.append(line)
                self.show(node, at, line)
                if want is not None and want[0] is node:
                    found = re.search(want[1], line)
                    if found:
                        return found
            for node in self.nodes:
                if node.proc.poll() is not None and node.lines.empty():
                    raise RuntimeError(f'{node.name} stopped')
            if time.monotonic() >= until:
                return None
            if not got:
                time.sleep(0.02)

    def wait(self, node, pattern, timeout, what):
        found = self.pump(time.monotonic() + timeout, (node, pattern))
        if found is None:
            raise RuntimeError(f'{what}: not seen within {timeout} s ({node.name}: /{pattern}/)')
        return found

    def idle(self, seconds):
        self.pump(time.monotonic() + seconds)

    def scenario(self, title):
        self.t0 = time.monotonic()
        print(f'\n=== {title}')


def build(build_dir):
    env_script = ROOT / 'tools' / 'host_env.ps1'
    if os.name == 'nt' and shutil.which('pwsh'):
        cmd = ['pwsh', '-NoProfile', '-Command',
               f". '{env_script}'; cmake -S '{ROOT / 'host' / 'link_sim'}' -B '{build_dir}' -G Ninja "
               f"-DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc; if ($LASTEXITCODE -ne 0) "
               f"{{ exit $LASTEXITCODE }}; cmake --build '{build_dir}' --target link_sim; "
               f"exit $LASTEXITCODE"]
    else:
        cmd = ['sh', '-c', f"cmake -S '{ROOT / 'host' / 'link_sim'}' -B '{build_dir}' -G Ninja "
               f"-DCMAKE_BUILD_TYPE=Release && cmake --build '{build_dir}' --target link_sim"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-3000:], r.stderr[-3000:])
        raise SystemExit('link_sim did not build')


def pair_one(run, k):
    run.m.send('pair')
    run.wait(run.m, r'pairing open', 5, 'pairing open')
    k.send(f'pair 127.0.0.1:{MASTER_PORT}')
    mc = run.wait(run.m, r'pairing code (\d{3} \d{3})', 20, 'the master\'s code')
    kc = run.wait(k, r'pairing code (\d{3} \d{3})', 20, 'the keypad\'s code')
    if mc.group(1) != kc.group(1):
        raise RuntimeError(f'the codes differ: {mc.group(1)} / {kc.group(1)}')
    run.m.send('allow')
    k.send('yes')
    run.wait(k, r'link: online', 30, 'the keypad online')


def pair(run):
    run.scenario('pairing (the master in installer mode, the keypad asks it)')
    pair_one(run, run.k)
    run.idle(3)


def together(run):
    run.scenario('together: the master installs 1.5.0, the keypad fetches it itself')
    run.k.send('fw server on')
    run.m.send('fw install 1.5.0')
    run.wait(run.k, r'fw: downloading 1\.5\.0', 20, 'the keypad downloading')
    run.wait(run.m, r'fw: keypad 1 on 1\.4\.0: ready', 30, 'the keypad READY')
    run.wait(run.m, r'restarts into 1\.5\.0', 30, 'the master restarting')
    run.wait(run.k, r'fw: installing 1\.5\.0: restarting', 15, 'the keypad restarting with it')
    run.wait(run.k, r'runs 1\.5\.0 on its self-test, on trial', 15, 'the keypad on 1.5.0')
    run.wait(run.m, r'self-test passed: 1\.5\.0 confirmed', 60, 'the master confirmed')
    run.wait(run.k, r'self-test passed \(its master runs it\): 1\.5\.0', 60,
             'the keypad confirmed with it')
    run.wait(run.m, r'event: keypad 1 updated', 30, 'Activity: keypad updated')
    run.idle(2)


def relay(run):
    run.scenario('relay: no internet at the keypad, the master relays 1.6.0')
    run.k.send('fw server off')
    run.m.send('fw install 1.6.0')
    run.wait(run.k, r"FETCH 1\.6\.0: its update server can't be reached", 30, 'its FETCH failing')
    run.wait(run.k, r'fw: RELAY 1\.6\.0 from its master', 15, 'the relay ordered')
    run.wait(run.m, r'fw: keypad 1 on 1\.5\.0: receiving from this panel', 15, 'receiving')
    run.wait(run.m, r'fw: keypad 1 on 1\.5\.0: ready', 60, 'READY after the relay')
    run.wait(run.k, r'fw: installing 1\.6\.0: restarting', 30, 'the keypad restarting with it')
    run.wait(run.k, r'self-test passed \(its master runs it\): 1\.6\.0', 90, 'both on 1.6.0')
    run.idle(2)


def armed(run):
    run.scenario('armed: armed away during the download, disarmed later: the restart at once')
    run.m.send('fw install 1.7.0')
    run.wait(run.m, r'this panel downloading 1\.7\.0', 10, 'the master downloading')
    run.m.send('arm away 123456')
    run.wait(run.m, r'arm away: ok', 5, 'armed away')
    run.wait(run.k, r'paused: not_quiet', 20, 'the relay paused in the exit time')
    run.wait(run.k, r'fw: receiving 1\.7\.0 from its master \d+ %$', 70,
             'the relay going on, armed (after the 45 s exit time)')
    run.wait(run.k, r'fw: 1\.7\.0 ready', 60, 'the keypad READY while armed')
    run.idle(6)
    if any('restarts into 1.7.0' in l for l in run.m.log[-200:]):
        raise RuntimeError('the master restarted while armed')
    print('  (armed: nothing restarts)')
    run.m.send('disarm 123456')
    run.wait(run.m, r'disarm: ok', 5, 'disarmed')
    run.wait(run.m, r'restarts into 1\.7\.0', 10, 'the master restarting right after the disarm')
    run.wait(run.k, r'fw: installing 1\.7\.0: restarting', 15, 'the keypad restarting with it')
    run.wait(run.k, r'self-test passed \(its master runs it\): 1\.7\.0', 90, 'both on 1.7.0')
    run.idle(2)


def back(run):
    run.scenario('back: the master\'s self-test fails, the keypad on trial goes back with it')
    run.k.send('fw server on')
    run.m.send('fw bad')
    run.m.send('fw install 1.8.0')
    run.wait(run.m, r'restarts into 1\.8\.0', 60, 'the master restarting into 1.8.0')
    run.wait(run.k, r'runs 1\.8\.0 on its self-test, on trial', 20, 'the keypad on trial')
    run.wait(run.m, r'self-test failed \(made to fail\): it goes back to 1\.7\.0', 60,
             'the master failing')
    run.wait(run.m, r'this panel runs 1\.7\.0', 15, 'the master back on 1.7.0')
    run.wait(run.k, r'self-test failed \(its master went back', 60, 'the keypad going back')
    run.wait(run.k, r'this keypad runs 1\.7\.0', 15, 'the keypad back on 1.7.0')
    run.wait(run.m, r'event: keypad 1 update rolled back', 60, 'Activity: rolled back')
    run.m.send('fw')
    run.idle(2)


def copy(run):
    run.scenario('copy: a keypad of another model, no internet: the master fetches a copy for it')
    runs = re.findall(r'this panel runs (\d+\.\d+\.\d+)', '\n'.join(run.m.log))
    version = runs[-1] if runs else '1.4.0'  # the master's: the new keypad comes on it
    k2 = Node('kp10', [str(run.exe), 'keypad', '--bind', '127.0.0.1', '--master',
                       f'127.0.0.1:{MASTER_PORT}', '--state', str(run.tmp / 'kp10.plink'), '--name',
                       'Kitchen keypad', '--id', '2b3c4d', '--model', 'pc_panel10', '--version',
                       version], run.verbose)
    run.nodes.append(k2)
    pair_one(run, k2)
    run.wait(run.m, r'fw: keypad 2 on [\d.]+: up to date', 30, 'keypad 2 up to date')
    k2.send('fw server off')
    run.k.send('fw server on')
    run.m.send('fw install 1.9.0')
    run.wait(run.m, r'fetching a copy of 1\.9\.0 for model pc_panel10', 30, 'the copy asked for')
    run.wait(run.m, r'the copy of 1\.9\.0 for model pc_panel10 is in', 15, 'the copy in')
    run.wait(k2, r'fw: RELAY 1\.9\.0 from its master', 15, 'keypad 2 relayed the copy')
    run.wait(run.m, r'restarts into 1\.9\.0', 60, 'the master restarting')
    run.wait(run.k, r'self-test passed \(its master runs it\): 1\.9\.0', 90, 'keypad 1 on 1.9.0')
    run.wait(k2, r'self-test passed \(its master runs it\): 1\.9\.0', 90, 'keypad 2 on 1.9.0')
    run.m.send('fw')
    run.idle(2)


SCENARIOS = {'together': together, 'relay': relay, 'armed': armed, 'back': back, 'copy': copy}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--only', choices=sorted(SCENARIOS), help='run this scenario only')
    ap.add_argument('--build-dir', default=str(ROOT / 'build' / 'link_sim'))
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('-v', '--verbose', action='store_true')
    a = ap.parse_args()
    build_dir = Path(a.build_dir)
    if not a.no_build:
        build(build_dir)
    exe = build_dir / ('link_sim.exe' if os.name == 'nt' else 'link_sim')
    tmp = Path(tempfile.mkdtemp(prefix='link_fw_sim_'))
    master = Node('master', [str(exe), 'master', '--bind', '127.0.0.1', '--port',
                             str(MASTER_PORT), '--state', str(tmp / 'master.plink'),
                             '--name', 'Harbor House', '--version', '1.4.0'], a.verbose)
    keypad = Node('keypad', [str(exe), 'keypad', '--bind', '127.0.0.1', '--master',
                             f'127.0.0.1:{MASTER_PORT}', '--state', str(tmp / 'keypad.plink'),
                             '--name', 'Hall keypad', '--version', '1.4.0'], a.verbose)
    run = Run(master, keypad, a.verbose)
    run.exe = exe
    run.tmp = tmp
    failed = []
    try:
        pair(run)
        for name, fn in SCENARIOS.items():
            if a.only is not None and name != a.only:
                continue
            try:
                fn(run)
                print(f'--- {name}: OK')
            except RuntimeError as e:
                print(f'--- {name}: FAILED - {e}')
                failed.append(name)
                break  # the next one starts from where this one should have ended
    except RuntimeError as e:
        print(f'--- pairing: FAILED - {e}')
        failed.append('pairing')
    finally:
        for node in run.nodes:
            node.stop()
        shutil.rmtree(tmp, ignore_errors=True)
    print('\nlink_fw_sim: ' + ('OK' if not failed else 'FAILED: ' + ', '.join(failed)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
