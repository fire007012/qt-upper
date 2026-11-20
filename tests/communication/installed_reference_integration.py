#!/usr/bin/env python3
"""Qt management/control -> actual installed ROS stack -> mock hardware.

Explicit opt-in. Sources the supplied local development config, but uses new
ports, state, credentials and a localhost-only DDS domain. Never uses hardware.
"""
import argparse
import json
import os
import re
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from host_paths import external_output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--manager', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--presets', choices=('base','flippers','arm','all','base_flippers'), default='all')
    parser.add_argument('--exercise-arm', action='store_true', help='assert named goal, Servo/gripper motion and neutral stop; arm preset only')
    parser.add_argument('--domain-id', type=int, default=120+secrets.randbelow(100), help='isolated DDS domain (0..232), random by default')
    parser.add_argument('--hold-ms', type=int, default=500)
    parser.add_argument('--initial-emergency', action='store_true')
    parser.add_argument('--fault-unselected', action='store_true')
    args = parser.parse_args()
    if not 0 <= args.domain_id <= 232: parser.error('domain-id must be 0..232')
    root = external_output(args.output_dir)
    config = json.loads(args.config.read_text())
    profiles = [json.loads(p.read_text()) for p in Path(config['config_root']).glob('*.json')]
    if len(profiles) != 1 or profiles[0].get('mode') != 'mock':
        parser.error('requires exactly one explicitly mock profile')
    if not 500 <= args.hold_ms <= 60000: parser.error('hold-ms must be between 500 and 60000')
    if args.exercise_arm and args.presets != 'arm': parser.error('exercise-arm requires arm preset')
    if args.fault_unselected and args.presets != 'base': parser.error('fault-unselected requires base preset')
    selected = dict(base=['base'], flippers=['flippers'], arm=['arm'], all=['flippers','base','arm'], base_flippers=['flippers','base'])[args.presets]
    root.mkdir(parents=True, exist_ok=False)
    (root / 'profiles').mkdir()
    (root / 'profiles/mock.json').write_text(json.dumps(profiles[0]))
    token = root / 'token'
    with open(token, 'x', opener=lambda p, f: os.open(p, f, 0o600)) as stream:
        stream.write(secrets.token_urlsafe(32))
    # Reserve both ports until configuration is ready.
    reservations = [socket.socket(), socket.socket()]
    for s in reservations:
        s.bind(('127.0.0.1', 0))
    manager_port, control_port = [s.getsockname()[1] for s in reservations]
    config.update(bind='127.0.0.1', port=manager_port, bridge_bind='127.0.0.1',
                  config_root=str(root / 'profiles'), state_dir=str(root / 'state'),
                  token_file=str(token), enable_config_transactions=False,
                  control_endpoint=dict(host='127.0.0.1',port=control_port,protocol_version=2))
    (root / 'manager.json').write_text(json.dumps(config))
    if args.initial_emergency:
        (root / 'state/control').mkdir(parents=True)
        (root / 'state/control/emergency.json').write_text(json.dumps(dict(active=True)))
    dds_config=args.manager.resolve().parent/'config/cyclonedds.xml'
    if dds_config.is_file():
        os.environ.setdefault('RMW_IMPLEMENTATION','rmw_cyclonedds_cpp')
        os.environ.setdefault('CYCLONEDDS_URI',dds_config.as_uri())
    os.environ.update(ROS_DOMAIN_ID=str(args.domain_id), ROS_LOCALHOST_ONLY='1',
                      ROS_HOME=str(root / 'ros-home'), ROS_LOG_DIR=str(root / 'ros-logs'),
                      PYTHONDONTWRITEBYTECODE='1')
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(args.manager.resolve().parent))
    from client import request
    import rclpy
    from rclpy.node import Node
    from rclpy.executors import SingleThreadedExecutor
    from sensor_msgs.msg import JointState
    rclpy.init()
    node = Node('host_installed_mock_observer')
    observed = dict(samples=0, wheel_motion=False, arm_motion=False, gripper_motion=False, neutral_stop=False)
    arm_names={'shoulder_yaw_joint','shoulder_pitch_joint','elbow_pitch_joint','wrist_pitch_joint','wrist_yaw_joint','wrist_roll_joint'}
    baseline={}
    stable_since=[None]
    jog_started=[None]
    max_delta={}
    def joints(message):
        observed['samples'] += 1
        observed['wheel_motion'] |= any('wheel' in name and abs(v) > .01
                                       for name, v in zip(message.name, message.velocity))
        if (root/'arm-jog-start').exists():
            if jog_started[0] is None:jog_started[0]=time.monotonic()
            for name, position in zip(message.name,message.position):
                if name in arm_names or name=='left_gripper_finger_joint':
                    baseline.setdefault(name,position)
                    max_delta[name]=max(max_delta.get(name,0),abs(position-baseline[name]))
            observed['arm_motion']=any(max_delta.get(n,0)>1e-4 for n in arm_names)
            observed['gripper_motion']=max_delta.get('left_gripper_finger_joint',0)>1e-4
            if observed['arm_motion'] and observed['gripper_motion'] and not (root/'arm-motion-confirmed').exists():
                observed['motion_latency_sec']=round(time.monotonic()-jog_started[0],3)
                (root/'arm-motion-confirmed').touch()
        if (root/'arm-neutral-start').exists():
            import math
            values=[abs(v) for n,v in zip(message.name,message.velocity) if n in arm_names or n=='left_gripper_finger_joint']
            if len(values)==7 and all(math.isfinite(v) and v<1e-5 for v in values):
                if stable_since[0] is None:stable_since[0]=time.monotonic()
                if time.monotonic()-stable_since[0]>.3:
                    observed['neutral_stop']=True
                    if not (root/'arm-stop-confirmed').exists():(root/'arm-stop-confirmed').touch()
            else:stable_since[0]=None
    node.create_subscription(JointState, '/joint_states', joints, 10)
    executor = SingleThreadedExecutor(); executor.add_node(node)
    spin = threading.Thread(target=executor.spin, daemon=True)
    spin.start()
    for s in reservations:
        s.close()
    report = dict(scope='qt_installed_ros_mock', passed=False, physical_acceptance=False,
                  hardware_initialized=False, domain_id=args.domain_id, middleware=os.environ.get('RMW_IMPLEMENTATION','default'))
    fault_stop = threading.Event()
    def inject_unselected():
        from std_srvs.srv import SetBool
        marker=root/'fault-request'
        while not fault_stop.wait(.05):
            if marker.exists(): break
        if fault_stop.is_set(): return
        try:
            # These two axes share CanDriverSystem with the healthy base. A
            # device fault is not a shared transport fault.
            for axis in ('wrist_pitch_joint','left_gripper_finger_joint'):
                client=node.create_client(SetBool,'/mock/'+axis+'/fault')
                if not client.wait_for_service(timeout_sec=5): raise RuntimeError('missing mock service')
                future=client.call_async(SetBool.Request(data=True))
                deadline=time.monotonic()+10
                while not future.done() and time.monotonic()<deadline and not fault_stop.wait(.01): pass
                if not future.done() or not future.result().success: raise RuntimeError('fault injection not confirmed')
            (root/'fault-result').write_text('ok')
        except Exception as error:
            (root/'fault-result').write_text(str(error))
    fault_thread=threading.Thread(target=inject_unselected,daemon=True) if args.fault_unselected else None
    if fault_thread: fault_thread.start()
    with (root / 'manager.log').open('w') as log:
        manager = subprocess.Popen([sys.executable, '-B', str(args.manager.resolve()),
                                    '--config', str(root / 'manager.json')],
                                   stdout=log, stderr=subprocess.STDOUT, cwd=root)
        def call(method):
            return request('127.0.0.1', manager_port, token.read_text(), method)
        try:
            deadline = time.monotonic() + 15
            while True:
                if manager.poll() is not None:
                    raise RuntimeError('manager exited; see manager.log')
                try:
                    if call('hello')['ok']:
                        break
                except OSError:
                    pass
                if time.monotonic() >= deadline:
                    raise TimeoutError('manager unavailable')
                time.sleep(.1)
            env = dict(os.environ, HOST_INSTALLED_TOKEN_FILE=str(token),
                       HOST_INSTALLED_MANAGER_PORT=str(manager_port), HOST_INSTALLED_PRESETS=json.dumps(selected),
                       HOST_INSTALLED_HOLD_MS=str(args.hold_ms), HOST_INSTALLED_INITIAL_EMERGENCY=str(int(args.initial_emergency)), QTEST_FUNCTION_TIMEOUT='360000')
            if args.exercise_arm: env['HOST_INSTALLED_ARM_ROOT']=str(root)
            else: env.pop('HOST_INSTALLED_ARM_ROOT', None)
            if args.fault_unselected: env['HOST_INSTALLED_FAULT_ROOT']=str(root)
            with (root / 'qt-probe.log').open('w') as probe_log:
                subprocess.run([str(args.probe.resolve())], env=env, cwd=root,
                               stdout=probe_log, stderr=subprocess.STDOUT, timeout=350, check=True)
            state = json.loads((root / 'state/state.json').read_text())
            assert state['software_state'] == 'stopped' and not state['resources'], state
            if 'base' in selected:
                assert observed['wheel_motion'], 'no measured wheel motion from mock hardware'
            if args.exercise_arm:
                assert observed['arm_motion'] and observed['gripper_motion'] and observed['neutral_stop'], observed
            report['joint_max_displacement']=max_delta
            report.update(presets=selected,hold_ms=args.hold_ms,initial_emergency=args.initial_emergency)
            report.update(unselected_faults=args.fault_unselected)
            report.update(passed=True, **observed, resources_remaining=0)
        finally:
            try:
                snapshot = call('snapshot')
                if snapshot['ok'] and snapshot['result']['software_state'] != 'stopped':
                    call('stop')
                    deadline = time.monotonic() + 45
                    while time.monotonic() < deadline:
                        if call('snapshot')['result']['software_state'] == 'stopped':
                            break
                        time.sleep(.2)
                # Only this runner's new mock resources; never an existing deployment.
                from manager import Backend, identity
                state_file = root / 'state/state.json'
                if state_file.exists():
                    state = json.loads(state_file.read_text())
                    backend = Backend(config, root / 'profiles', root / 'state')
                    for resource in reversed(state['resources']):
                        if resource.get('pid') and identity(resource['pid']) == resource.get('identity'):
                            backend.stop(resource)
            finally:
                fault_stop.set()
                if fault_thread: fault_thread.join(6)
                manager.terminate(); manager.wait(timeout=10)
                executor.shutdown(); spin.join(5); node.destroy_node(); rclpy.shutdown()
                crashes=[p.name for p in (root/'state').glob('*.process.log')
                         if re.search(r'exit code -(?:6|11)\b|Segmentation fault|Aborted \(',p.read_text(errors='replace'))]
                report['process_crashes']=crashes
                if crashes:report['passed']=False
                report.update(**observed, joint_max_displacement=max_delta)
                (root / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
                print(json.dumps(report), flush=True)
                if crashes:raise RuntimeError('installed ROS process crashed: '+','.join(crashes))


if __name__ == '__main__':
    main()
