#!/usr/bin/env python3
"""Real supplied bridge + Qt v2 contract, read-only source and injected no-device output.

Explicit opt-in, never installed by default CTest. No ROS imports or hardware
initialization. Journals, profiles and Qt settings are outside the source tree.
"""
import argparse
from collections import Counter, deque
import importlib.util
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time


def scenario(args, case):
    sys.dont_write_bytecode = True
    source = args.bridge.resolve()
    sys.path.insert(0, str(source.parent))
    spec = importlib.util.spec_from_file_location('host_reference_bridge', source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    from output_adapters import DryRunOutput

    class Capture(DryRunOutput):
        def __init__(self):
            super().__init__()
            self.calls = Counter()
            self.nonzero = threading.Event()
            self.outputs = deque(maxlen=10000)
            self.release = threading.Event()
            groups = ('base', 'arm', 'flippers', 'gripper')
            self.runtime_state = dict(groups={g: 'Running' for g in groups}, diagnostics=dict(
                readable=True, global_inhibit=False, owner_fresh=True,
                groups={g: dict(state=2, ready=True, blockers=[]) for g in groups}))
            self.runtime_received = time.monotonic()
        def publish_twist(self, value):
            self.outputs.append((time.monotonic(), value.linear_x, value.source))
            if value.linear_x:
                self.nonzero.set()
        def publish_servo(self, value):
            pass
        def publish_flipper(self, value):
            pass
        def publish_gripper(self, value):
            pass
        def call_lifecycle(self, command):
            self.calls[command] += 1
            if case == 'slowBackendAndIndependentEmergency' and command == 'init':
                self.release.wait(30)
            return True, 0, 'fixture only'

    with tempfile.TemporaryDirectory(prefix='host-control-contract-') as folder:
        output = Capture()
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        server = module.HostBridgeServer('127.0.0.1', port, output, watchdog_ms=500,
                                         control_state_dir=str(Path(folder) / 'control'))
        server.core.robot_id = 'robot24'
        original = server.control.handle
        writes = []
        dropped = False
        emergency_times = []
        def handle(connection, request):
            nonlocal dropped
            if request.get('type') == 'system_command':
                writes.append((request.get('request_id'), request.get('command'), request.get('params')))
            if request.get('type') == 'emergency_stop':
                emergency_times.append(time.monotonic())
            response = original(connection, request)
            if (case == 'acceptedResponseLost' and request.get('type') == 'system_command'
                    and response['ok'] and not dropped):
                dropped = True
                # Accepted intent is durable; drop only the response, never replay work.
                time.sleep(.05)
                connection.close()
            return response
        server.control.handle = handle
        done = threading.Event()
        epoch_changed = False
        def observations():
            nonlocal epoch_changed
            while not done.wait(.02):
                output.runtime_received = time.monotonic()
                if case == 'epochAndEmergency' and output.nonzero.is_set() and not epoch_changed:
                    epoch_changed = True
                    server.control.runtime_changed('host fixture actual epoch change')
        observer = threading.Thread(target=observations, daemon=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        observer.start()
        thread.start()
        try:
            deadline = time.monotonic() + 3
            while True:
                try:
                    with socket.create_connection(('127.0.0.1', port), .1):
                        break
                except OSError:
                    if time.monotonic() > deadline:
                        raise RuntimeError('reference bridge failed to listen')
                    time.sleep(.02)
            env = dict(os.environ, HOST_CONTROL_PORT=str(port), HOST_CONTROL_CYCLES=str(args.cycles),
                       QTEST_FUNCTION_TIMEOUT=str(max(600000, args.cycles * 5000)))
            subprocess.run([str(args.probe.resolve()), case], env=env, cwd=folder, timeout=max(70, args.cycles * 5), check=True)
            if case == 'acceptedResponseLost':
                assert len(writes) == 2 and writes[0] == writes[1], writes
                assert output.calls['init'] == 1, output.calls
                assert len(server.control.operations) == 1
            if case in ('epochAndEmergency', 'slowBackendAndIndependentEmergency'):
                assert output.nonzero.is_set(), 'client never caused a simulated nonzero output'
                assert emergency_times
                stop_at = emergency_times[-1]
                zeros = [t for t, x, _ in output.outputs if not x and t >= stop_at]
                assert zeros and min(zeros) - stop_at < .5, 'no timely simulated zero output'
                assert not [x for t, x, _ in output.outputs if x and t > stop_at + .1], 'motion after emergency'
            if case == 'multipleClientsAndInputWatchdog':
                assert output.nonzero.is_set()
                last_motion = max(t for t, x, _ in output.outputs if x)
                zeros = [t for t, x, _ in output.outputs if not x and t >= last_motion]
                assert zeros and min(zeros) - last_motion < .6, 'watchdog failed to zero simulated output'
                assert server.control.counts.get('OWNER_BUSY', 0) >= 1
            if case.startswith('repeated'):
                assert server.control.counts.get('outputs', 0) >= args.cycles, 'not every cycle produced simulated motion'
            report = dict(case=case, passed=True, no_ros=True, physical_acceptance=False,
                          commands=dict(output.calls), outputs_retained=len(output.outputs),
                          counts=dict(server.control.counts), reconnect_cycles=args.cycles if case.startswith('repeated') else None)
            print(json.dumps(report), flush=True)
        finally:
            done.set(); output.release.set(); server.stop_event.set()
            with server.client_lock:
                for client in list(server.active_clients):
                    client.close()
            thread.join(3); observer.join(1)
            server.control.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bridge', required=True, type=Path)
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--cycles', type=int, default=100)
    parser.add_argument('--case', choices=['all', 'epochAndEmergency', 'acceptedResponseLost',
                                          'slowBackendAndIndependentEmergency', 'repeatedReconnectsNeverRestoreAuthorization', 'multipleClientsAndInputWatchdog'], default='all')
    args = parser.parse_args()
    if not 1 <= args.cycles <= 10000:
        parser.error('cycles must be between 1 and 10000')
    cases = ['epochAndEmergency', 'acceptedResponseLost', 'slowBackendAndIndependentEmergency',
             'repeatedReconnectsNeverRestoreAuthorization', 'multipleClientsAndInputWatchdog'] if args.case == 'all' else [args.case]
    for case in cases:
        scenario(args, case)


if __name__ == '__main__':
    main()
