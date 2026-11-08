#!/usr/bin/env python3
"""Read-only optional contract test of the supplied manager implementation.

All state/config/logs are temporary; the injected backend never starts ROS or
hardware. The reference source is imported with bytecode generation disabled.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys
import tempfile
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manager', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    args = parser.parse_args()
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(args.manager.resolve().parent))
    spec = importlib.util.spec_from_file_location('reference_manager', args.manager.resolve())
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    class MockBackend:
        def __init__(self):
            self.started = []
            self.stopped = []
        def validate(self, profile):
            pass
        def preflight(self, profile):
            return dict(passed=True, checks=[])
        def required(self, profile):
            return ['core', 'bridge']
        def modules(self, profile):
            return self.required(profile)
        def start(self, name, profile, operation_id):
            self.started.append(name)
            return dict(module=name, pid=0, identity=['mock', '0'])
        def health(self, profile):
            return dict(modules={name: dict(ok=True) for name in self.required(profile)},
                        runtime=dict(state='Uninitialized'), observed_at=time.time())
        def halt(self, profile):
            self.stopped.append('halt')
        def stop(self, resource):
            self.stopped.append(resource['module'])

    with tempfile.TemporaryDirectory(prefix='host-manager-contract-') as tmp:
        root = Path(tmp)
        profiles = root / 'profiles'
        profiles.mkdir()
        (profiles / 'mock.json').write_text(json.dumps(dict(
            profile_id='mock', config_revision='revision-A', mode='mock', build_version='contract-test',
            video=False, video_required=False, required_modules=['core', 'bridge'], optional_modules=[],
            devices=[], startup_timeout=1)))
        token = secrets.token_urlsafe(32)
        (root / 'token').write_text(token)
        (root / 'token').chmod(0o600)
        (root / 'config.json').write_text(json.dumps(dict(
            config_root=str(profiles), state_dir=str(root / 'state'), token_file=str(root / 'token'),
            robot_id='robot24', build_version='contract-test', bind='127.0.0.1', port=0,
            control_endpoint=dict(host='127.0.0.1', port=9090, protocol_version=1))))
        backend = MockBackend()
        manager = module.Manager(root / 'config.json', backend)
        server = module.Server(manager)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            env = dict(os.environ, HOST_MANAGER_PROBE_TOKEN=token,
                       HOST_MANAGER_PROBE_PORT=str(server.server_address[1]),
                       HOST_MANAGER_PROBE_EXPECTED_ENDPOINTS=str(
                           2 if manager.config['control_endpoint']['protocol_version'] in (1, 2) else 0))
            result = subprocess.run([str(args.probe.resolve())], env=env, timeout=35, check=False)
            if result.returncode:
                raise RuntimeError(f'Qt management probe failed: {result.returncode}')
            assert backend.started == ['core', 'bridge'] * 2, backend.started
            assert backend.stopped == ['halt', 'bridge', 'core'] * 2, backend.stopped
            assert not manager.data['resources']
            print('reference manager + Qt RuntimeService: PASS (injected backend, no ROS/hardware)')
        finally:
            manager.closed.set()
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)
            manager.lock_file.close()


if __name__ == '__main__':
    main()
