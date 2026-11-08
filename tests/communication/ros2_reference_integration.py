#!/usr/bin/env python3
"""Qt -> supplied bridge/RosOutput -> actual ROS2 DDS -> independent mock runtime.

Explicit read-only reference source; isolated localhost domain and no hardware.
Source the disposable mock interface overlay before invoking this runner.
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
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from host_paths import external_output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bridge', required=True, type=Path)
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--domain-id', type=int, default=196)
    args = parser.parse_args()
    if not 101 <= args.domain_id <= 200:
        parser.error('use an isolated mock domain between 101 and 200')
    try:
        output_dir = external_output(args.output_dir)
        if output_dir.exists():
            raise ValueError('choose a new evidence directory')
        bridge = args.bridge.resolve(strict=True)
        probe = args.probe.resolve(strict=True)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    output_dir.mkdir(parents=True)
    # Set isolation and artifact paths before loading any ROS library.
    os.environ.update(ROS_DOMAIN_ID=str(args.domain_id), ROS_LOCALHOST_ONLY='1',
                      ROS_HOME=str(output_dir / 'ros-home'), ROS_LOG_DIR=str(output_dir / 'ros-logs'),
                      PYTHONDONTWRITEBYTECODE='1')
    sys.dont_write_bytecode = True
    import rclpy
    from rclpy.node import Node
    from rclpy.executors import MultiThreadedExecutor
    from rclpy.callback_groups import ReentrantCallbackGroup
    from geometry_msgs.msg import Twist
    from std_msgs.msg import String
    from std_srvs.srv import Trigger

    sys.path.insert(0, str(bridge.parent))
    spec = importlib.util.spec_from_file_location('host_ros2_reference_bridge', bridge)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    from output_adapters import RosOutput

    class MockTransportOutput(RosOutput):
        # Keep the actual adapter/ROS service implementation; only isolate names.
        def lifecycle_service_name(self, command):
            return '/host_test_runtime/' + command

    adapter = MockTransportOutput('host_test_bridge', '/host_test/cmd_vel', '/host_test/servo',
                                  '/host_test/gripper', '/host_test/flipper', '/host_test/profile', '/host_test_runtime')
    node = Node('host_test_runtime')
    executor = MultiThreadedExecutor(num_threads=4); executor.add_node(node)
    callbacks = ReentrantCallbackGroup()
    outputs, call_times = deque(maxlen=4096), deque(maxlen=64)
    calls = Counter()
    def init(request, response):
        calls['init'] += 1; call_times.append(('init', time.monotonic()))
        time.sleep(3)
        response.success = True; response.message = 'mock ROS2 init completed'; return response
    def halt(request, response):
        calls['halt'] += 1; call_times.append(('halt', time.monotonic()))
        response.success = True; response.message = 'mock ROS2 halt completed'; return response
    node.create_service(Trigger, '/host_test_runtime/init', init, callback_group=callbacks)
    node.create_service(Trigger, '/host_test_runtime/halt', halt, callback_group=callbacks)
    node.create_subscription(Twist, '/host_test/cmd_vel', lambda msg: outputs.append((time.monotonic(), msg.linear.x)),
                             100, callback_group=callbacks)
    publisher = node.create_publisher(String, '/robot_runtime/state', 1)
    groups = ('base', 'arm', 'flippers', 'gripper')
    # Deliberate mock permission, never inferred from missing hardware.
    state = dict(groups={group:'Running' for group in groups}, input_epochs={group:1 for group in groups},
                 diagnostics=dict(readable=True, owner_fresh=True, global_inhibit=False,
                    groups={group:dict(state=2, ready=True, blockers=[]) for group in groups}))
    node.create_timer(.05, lambda: publisher.publish(String(data=json.dumps(state))), callback_group=callbacks)
    spin = threading.Thread(target=executor.spin, daemon=True); spin.start()
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0)); port = reservation.getsockname()[1]
    server = module.HostBridgeServer('127.0.0.1', port, adapter, watchdog_ms=500,
                                     control_state_dir=str(output_dir / 'control-journal'))
    server.core.robot_id = 'robot24'; adapter.runtime_reset_callback = server.control.runtime_changed
    emergencies = []
    original = server.control.handle
    def handle(connection, request):
        if request.get('type') == 'emergency_stop':
            emergencies.append(time.monotonic())
        return original(connection, request)
    server.control.handle = handle
    serving = threading.Thread(target=server.serve_forever, daemon=True); serving.start()
    report = dict(format_version=1, scope='qt_actual_ros2_mock_transport', passed=False,
                  domain_id=args.domain_id, localhost_only=True, physical_acceptance=False,
                  hardware_initialized=False)
    try:
        deadline = time.monotonic() + 15
        while not server.control.permission():
            if time.monotonic() >= deadline:
                raise RuntimeError('mock ROS2 runtime state did not reach the real adapter')
            time.sleep(.05)
        with (output_dir / 'qt-probe.log').open('w') as log:
            subprocess.run([str(probe), 'ros2RuntimeAndEmergency'],
                           env=dict(os.environ, HOST_CONTROL_PORT=str(port)), cwd=output_dir,
                           stdout=log, stderr=subprocess.STDOUT, timeout=40, check=True)
        assert calls['init'] == 1, calls
        assert emergencies and any(value > 0 for _, value in outputs), 'no ROS2 motion/emergency observation'
        emergency_at = emergencies[-1]
        zero_at = min(at for at, value in outputs if value == 0 and at >= emergency_at)
        halt_at = min(at for command, at in call_times if command == 'halt' and at >= emergency_at)
        assert zero_at - emergency_at < .5, 'ROS2 zero command delayed'
        assert halt_at - emergency_at < 1, 'ROS2 halt delayed by ordinary service'
        assert not any(value != 0 for at, value in outputs if at > emergency_at + .1), 'ROS2 motion after emergency'
        report.update(passed=True, lifecycle_calls=dict(calls), ros_output_samples=len(outputs),
                      emergency_to_ros_zero_seconds=zero_at-emergency_at,
                      emergency_to_ros_halt_seconds=halt_at-emergency_at,
                      ros2_distro=os.environ.get('ROS_DISTRO'))
    finally:
        server.stop_event.set()
        with server.client_lock:
            for client in list(server.active_clients):
                client.close()
        serving.join(3); server.control.close()
        executor.shutdown(); spin.join(3); node.destroy_node(); adapter.close()
        (output_dir / 'summary.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
        print(json.dumps(report, ensure_ascii=False), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
