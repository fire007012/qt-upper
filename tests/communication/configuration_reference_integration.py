#!/usr/bin/env python3
"""Read-only manager contract test; isolated injected backend, no ROS or devices.

Imports only the explicitly supplied manager source. All state, profiles and Qt
settings live in temporary directories outside the reference workspace.
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manager', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    args = parser.parse_args()
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(args.manager.resolve().parent))
    spec = importlib.util.spec_from_file_location('config_reference_manager', args.manager.resolve())
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    class NoDevice:
        def validate(self, profile):
            assert 1 <= profile.get('startup_timeout', 90) <= 180
        def preflight(self, profile):
            return dict(passed=True, checks=[])

    with tempfile.TemporaryDirectory(prefix='host-configuration-contract-') as folder:
        root = Path(folder)
        (root / 'profiles').mkdir()
        (root / 'profiles/mock.json').write_text(json.dumps(dict(
            profile_id='mock', config_revision='deployment-revision', mode='mock',
            video=False, startup_timeout=90, build_version='host-contract')))
        token = secrets.token_urlsafe(32)
        (root / 'token').write_text(token)
        (root / 'token').chmod(0o600)
        config = root / 'manager.json'
        config.write_text(json.dumps(dict(
            enable_config_transactions=True, robot_id='robot24',
            config_root=str(root / 'profiles'), state_dir=str(root / 'state'),
            token_file=str(root / 'token'), build_version='host-contract',
            bind='127.0.0.1', port=0,
            control_endpoint=dict(host='127.0.0.1', port=0, protocol_version=2))))
        manager = module.Manager(config, NoDevice())
        original = manager.dispatch
        writes = []
        dropped = False

        def dispatch(request):
            nonlocal dropped
            method = request.get('method')
            if method in ('start', 'stop'):
                raise AssertionError('configuration client must not start/stop implicitly')
            if method in ('config.candidate', 'config.apply', 'config.rollback'):
                assert request['request_id'].startswith('config-')
                writes.append((method, request['request_id'], request['params']))
            if method == 'config.apply':
                revision = request['params']['candidate_revision']
                profiles = manager.config_transactions._read(revision)
                fail = profiles['mock']['startup_timeout'] == 110
                def hook(phase):
                    if fail and phase == 'after_switch':
                        raise RuntimeError('host fixture injected failure after activation')
                manager.config_transactions.hook = hook
            elif method == 'config.rollback':
                manager.config_transactions.hook = lambda phase: None
            result = original(request)
            if method == 'config.apply' and not dropped:
                dropped = True
                raise OSError('host fixture dropped accepted response')
            return result

        manager.dispatch = dispatch
        server = module.Server(manager)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            env = dict(os.environ, HOST_CONFIG_PORT=str(server.server_address[1]), HOST_CONFIG_TOKEN=token)
            subprocess.run([str(args.probe.resolve())], env=env, timeout=30, check=True)
            applies = [(rid, params) for method, rid, params in writes if method == 'config.apply']
            assert len(applies) == 3 and applies[0] == applies[1] and applies[2][0] != applies[0][0], applies
            assert len(manager.config_transactions.data['operations']) == 3
            assert manager.data['software_state'] == 'stopped' and not manager.data['resources']
            print('PASS: real manager + Qt configuration service; same-ID recovery, '
                  'readback, rollback after failure, explicit rollback, audit; no ROS/hardware')
        finally:
            if manager.config_transactions.worker:
                manager.config_transactions.worker.join(3)
            manager.closed.set()
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)
            manager.lock_file.close()


if __name__ == '__main__':
    main()
