#!/usr/bin/env python3
import json
import os
import socket
import subprocess
import sys
import time
from typing import Any, Dict, List
from urllib.request import urlopen


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BRIDGE = os.path.join(ROOT, "ros1_bridge", "host_bridge_node.py")
HOST = "127.0.0.1"
PORT = int(os.getenv("BRIDGE_LOOPBACK_PORT", "19091"))
DEBUG_PORT = int(os.getenv("BRIDGE_LOOPBACK_DEBUG_PORT", "19092"))


class JsonLineClient:
    def __init__(self, host: str, port: int) -> None:
        self.sock = socket.create_connection((host, port), timeout=1.0)
        self.sock.settimeout(1.0)
        self.buffer = b""

    def close(self) -> None:
        self.sock.close()

    def send(self, payload: Dict[str, Any]) -> None:
        line = json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n"
        self.sock.sendall(line.encode("utf-8"))

    def recv(self, timeout: float = 1.0) -> Dict[str, Any]:
        deadline = time.time() + timeout
        while b"\n" not in self.buffer:
            remaining = deadline - time.time()
            if remaining <= 0:
                raise TimeoutError("timed out waiting for JSON line")
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout as exc:
                raise TimeoutError("timed out waiting for JSON line") from exc
            if not chunk:
                raise RuntimeError("connection closed")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line.decode("utf-8"))

    def recv_until(self, msg_type: str, timeout: float = 1.0) -> Dict[str, Any]:
        deadline = time.time() + timeout
        seen: List[str] = []
        while time.time() < deadline:
            msg = self.recv(max(0.01, deadline - time.time()))
            seen.append(str(msg.get("type")))
            if msg.get("type") == msg_type:
                return msg
        raise AssertionError(f"expected {msg_type}, seen {seen}")

    def recv_status(self, status: str, timeout: float = 1.0) -> Dict[str, Any]:
        deadline = time.time() + timeout
        seen: List[str] = []
        while time.time() < deadline:
            msg = self.recv(max(0.01, deadline - time.time()))
            seen.append(f"{msg.get('type')}:{msg.get('operator_input_status')}")
            if msg.get("type") == "system_status" and msg.get("operator_input_status") == status:
                return msg
        raise AssertionError(f"expected system_status {status}, seen {seen}")


def now_ms() -> int:
    return int(time.time() * 1000)


def wait_for_port(timeout: float = 3.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((HOST, PORT), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"bridge did not open {HOST}:{PORT}")


def start_bridge() -> subprocess.Popen:
    proc = subprocess.Popen(
        [
            sys.executable,
            BRIDGE,
            "--dry-run",
            "--host",
            HOST,
            "--port",
            str(PORT),
            "--watchdog-ms",
            "150",
            "--debug-ui",
            "--debug-host",
            HOST,
            "--debug-port",
            str(DEBUG_PORT),
            "--service-command",
            "set_control_mode=dry-run:/control/{mode}",
            "--service-command",
            "emergency_stop=dry-run:/safety/emergency_stop",
        ],
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    wait_for_port()
    return proc


def get_json(path: str) -> Dict[str, Any]:
    with urlopen(f"http://{HOST}:{DEBUG_PORT}{path}", timeout=1.0) as response:
        return json.loads(response.read().decode("utf-8"))


def wait_for_debug_event(category: str, message: str, timeout: float = 1.0) -> Dict[str, Any]:
    deadline = time.time() + timeout
    while time.time() < deadline:
        events = get_json("/api/events?limit=200")["events"]
        for event in events:
            if event.get("category") == category and event.get("message") == message:
                return event
        time.sleep(0.05)
    raise AssertionError(f"missing debug event {category}:{message}")


def stop_bridge(proc: subprocess.Popen) -> None:
    proc.terminate()
    try:
        proc.wait(timeout=1.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def main() -> None:
    proc = start_bridge()
    try:
        client = JsonLineClient(HOST, PORT)
        try:
            hello = client.recv_until("hello")
            assert hello["protocol_version"] == 1
            assert hello["bridge_name"] == "host_bridge_node"

            capabilities = client.recv_until("capabilities")
            assert "operator_input" in capabilities["supports"]
            assert "watchdog" in capabilities["supports"]

            client.send({
                "type": "heartbeat",
                "protocol_version": 1,
                "seq": 1,
                "timestamp_ms": now_ms(),
            })
            heartbeat_ack = client.recv_until("heartbeat_ack")
            assert heartbeat_ack["seq"] == 1

            client.send({
                "type": "sync_request",
                "protocol_version": 1,
                "seq": 2,
                "timestamp_ms": now_ms(),
                "params": {"reason": "bridge_loopback"},
            })
            snapshot = client.recv_until("system_snapshot")
            assert snapshot["seq"] == 2
            assert snapshot["emergency"]["active"] is False

            client.send({
                "type": "camera_list_request",
                "protocol_version": 1,
                "seq": 3,
                "timestamp_ms": now_ms(),
            })
            cameras = client.recv_until("camera_list_response")
            assert cameras["seq"] == 3
            assert len(cameras["cameras"]) >= 1

            client.send({
                "type": "operator_input",
                "protocol_version": 1,
                "seq": 4,
                "timestamp_ms": now_ms(),
                "ttl_ms": 500,
                "mode": "vehicle",
                "keyboard": {"pressed_keys": ["w", "d"]},
                "gamepad": {"connected": False, "buttons": {}, "axes": {}},
            })
            accepted = client.recv_status("accepted")
            assert accepted["last_operator_input_seq"] == 4
            assert accepted["control_mode"] == "vehicle"
            assert accepted["speed_level"] == 2
            assert accepted["cmd_vel"]["linear_x"] > 0
            assert accepted["cmd_vel"]["angular_z"] < 0

            client.send({
                "type": "operator_input",
                "protocol_version": 1,
                "seq": 5,
                "timestamp_ms": now_ms(),
                "ttl_ms": 500,
                "mode": "vehicle",
                "keyboard": {"pressed_keys": ["8"]},
                "gamepad": {"connected": False, "buttons": {}, "axes": {}},
            })
            speed_status = client.recv_status("accepted")
            assert speed_status["last_operator_input_seq"] == 5
            assert speed_status["speed_level"] == 3

            client.send({
                "type": "operator_input",
                "protocol_version": 1,
                "seq": 6,
                "timestamp_ms": now_ms(),
                "ttl_ms": 500,
                "mode": "vehicle",
                "keyboard": {"pressed_keys": ["1"]},
                "gamepad": {"connected": False, "buttons": {}, "axes": {}},
            })
            arm_mode = client.recv_status("accepted")
            assert arm_mode["last_operator_input_seq"] == 6
            assert arm_mode["control_mode"] == "arm"
            assert arm_mode["cmd_vel"]["linear_x"] == 0.0
            assert arm_mode["servo"]["frame_id"] == "catch_camera"

            client.send({
                "type": "operator_input",
                "protocol_version": 1,
                "seq": 7,
                "timestamp_ms": now_ms(),
                "ttl_ms": 500,
                "mode": "vehicle",
                "keyboard": {"pressed_keys": ["w", "d", "u", "i", "q", "f"]},
                "gamepad": {"connected": False, "buttons": {}, "axes": {}},
            })
            arm_move = client.recv_status("accepted")
            assert arm_move["last_operator_input_seq"] == 7
            assert arm_move["control_mode"] == "arm"
            assert arm_move["cmd_vel"]["linear_x"] == 0.0
            assert arm_move["servo"]["linear_x"] > 0
            assert arm_move["servo"]["linear_y"] < 0
            assert arm_move["servo"]["linear_z"] > 0
            assert arm_move["servo"]["angular_x"] > 0
            assert arm_move["servo"]["angular_y"] < 0

            client.send({
                "type": "operator_input",
                "protocol_version": 1,
                "seq": 8,
                "timestamp_ms": now_ms(),
                "ttl_ms": 500,
                "mode": "vehicle",
                "keyboard": {"pressed_keys": ["1"]},
                "gamepad": {"connected": False, "buttons": {}, "axes": {}},
            })
            vehicle_mode = client.recv_status("accepted")
            assert vehicle_mode["last_operator_input_seq"] == 8
            assert vehicle_mode["control_mode"] == "vehicle"

            client.send({
                "type": "operator_input",
                "protocol_version": 1,
                "seq": 9,
                "timestamp_ms": now_ms(),
                "ttl_ms": 500,
                "mode": "vehicle",
                "keyboard": {"pressed_keys": ["w", "d"]},
                "gamepad": {"connected": False, "buttons": {}, "axes": {}},
            })
            vehicle_move = client.recv_status("accepted")
            assert vehicle_move["last_operator_input_seq"] == 9
            assert vehicle_move["control_mode"] == "vehicle"
            assert abs(vehicle_move["cmd_vel"]["linear_x"] - 0.55) < 1e-6
            assert abs(vehicle_move["cmd_vel"]["angular_z"] + 1.0) < 1e-6

            client.send({
                "type": "operator_input",
                "protocol_version": 1,
                "seq": 10,
                "timestamp_ms": now_ms(),
                "ttl_ms": 500,
                "mode": "vehicle",
                "keyboard": {"pressed_keys": ["space"]},
                "gamepad": {"connected": False, "buttons": {}, "axes": {}},
            })
            emergency_key = client.recv_status("emergency_key")
            assert emergency_key["last_operator_input_seq"] == 10
            assert emergency_key["emergency_active"] is True
            emergency_from_space = client.recv_until("emergency_state")
            assert emergency_from_space["active"] is True
            assert emergency_from_space["source"] == "keyboard_space"

            client.send({
                "type": "clear_emergency",
                "protocol_version": 1,
                "seq": 11,
                "timestamp_ms": now_ms(),
            })
            clear_ack = client.recv_until("ack")
            assert clear_ack["seq"] == 11
            assert clear_ack["ack_type"] == "clear_emergency"
            cleared = client.recv_until("emergency_state")
            assert cleared["active"] is False

            client.send({
                "type": "operator_input",
                "protocol_version": 1,
                "seq": 12,
                "timestamp_ms": now_ms(),
                "ttl_ms": 500,
                "mode": "vehicle",
                "keyboard": {"pressed_keys": ["w", "d"]},
                "gamepad": {"connected": False, "buttons": {}, "axes": {}},
            })
            resumed = client.recv_status("accepted")
            assert resumed["last_operator_input_seq"] == 12
            assert resumed["emergency_active"] is False

            watchdog = client.recv_status("watchdog_timeout", timeout=1.0)
            assert watchdog["watchdog_active"] is True
            assert watchdog["cmd_vel"]["linear_x"] == 0.0
            assert watchdog["cmd_vel"]["angular_z"] == 0.0

            client.send({
                "type": "system_command",
                "protocol_version": 1,
                "seq": 13,
                "timestamp_ms": now_ms(),
                "command": "enable",
                "params": {},
            })
            lifecycle_ack = client.recv_until("ack")
            assert lifecycle_ack["seq"] == 13
            assert lifecycle_ack["ack_type"] == "system_command"
            assert lifecycle_ack["ok"] is True
            service_result = client.recv_until("service_call_result")
            assert service_result["seq"] == 13
            assert service_result["request_type"] == "system_command"
            assert service_result["command"] == "enable"
            assert service_result["service"] == "dry-run:lifecycle/enable"
            assert service_result["ok"] is True

            client.send({
                "type": "system_command",
                "protocol_version": 1,
                "seq": 14,
                "timestamp_ms": now_ms(),
                "command": "set_control_mode",
                "params": {"mode": "arm"},
            })
            mode_ack = client.recv_until("ack")
            assert mode_ack["seq"] == 14
            assert mode_ack["ack_type"] == "system_command"
            assert mode_ack["ok"] is True
            mode_service = client.recv_until("service_call_result")
            assert mode_service["seq"] == 14
            assert mode_service["command"] == "set_control_mode"
            assert mode_service["service"] == "dry-run:/control/arm"
            assert mode_service["ok"] is True

            client.send({
                "type": "system_command",
                "protocol_version": 1,
                "seq": 115,
                "timestamp_ms": now_ms(),
                "command": "request_arm_named_targets",
                "params": {},
            })
            targets_ack = client.recv_until("ack")
            assert targets_ack["seq"] == 115
            assert targets_ack["ack_type"] == "system_command"
            assert targets_ack["ok"] is True
            targets = client.recv_until("arm_named_targets")
            assert targets["seq"] == 115
            assert [item["name"] for item in targets["targets"]][:2] == ["home", "ready"]

            client.send({
                "type": "system_command",
                "protocol_version": 1,
                "seq": 116,
                "timestamp_ms": now_ms(),
                "command": "move_arm_named_target",
                "params": {"target": "home"},
            })
            move_ack = client.recv_until("ack")
            assert move_ack["seq"] == 116
            assert move_ack["ack_type"] == "system_command"
            assert move_ack["ok"] is True
            move_result = client.recv_until("service_call_result")
            assert move_result["seq"] == 116
            assert move_result["command"] == "move_arm_named_target"
            assert move_result["service"] == "moveit:arm/home"
            assert move_result["ok"] is True

            client.send({
                "type": "emergency_stop",
                "protocol_version": 1,
                "seq": 15,
                "timestamp_ms": now_ms(),
                "params": {"source": "bridge_loopback"},
            })
            ack = client.recv_until("ack")
            assert ack["seq"] == 15
            assert ack["ack_type"] == "emergency_stop"
            assert ack["ok"] is True
            emergency_service = client.recv_until("service_call_result")
            assert emergency_service["seq"] == 15
            assert emergency_service["command"] == "emergency_stop"
            assert emergency_service["service"] == "dry-run:/safety/emergency_stop"
            assert emergency_service["ok"] is True
            emergency = client.recv_until("emergency_state")
            assert emergency["active"] is True

            client.send({
                "type": "operator_input",
                "protocol_version": 1,
                "seq": 16,
                "timestamp_ms": now_ms(),
                "ttl_ms": 500,
                "mode": "vehicle",
                "keyboard": {"pressed_keys": ["w"]},
                "gamepad": {"connected": False, "buttons": {}, "axes": {}},
            })
            ignored = client.recv_status("ignored_emergency")
            assert ignored["cmd_vel"]["linear_x"] == 0.0

            state = get_json("/api/state")["state"]
            assert state["emergency_active"] is True
            assert state["last_operator_seq"] == 16
            assert state["last_twist"]["linear_x"] == 0.0

            cameras_api = get_json("/api/cameras")
            assert len(cameras_api["cameras"]) >= 1

            accepted_event = wait_for_debug_event("operator_input", "operator input accepted")
            assert accepted_event["data"]["seq"] == 4
            watchdog_event = wait_for_debug_event("watchdog", "operator input watchdog timeout")
            assert watchdog_event["level"] == "warning"
            emergency_event = wait_for_debug_event("emergency", "emergency stop accepted")
            assert emergency_event["data"]["source"] == "bridge_loopback"
        finally:
            client.close()
    finally:
        stop_bridge(proc)

    print("bridge loopback: PASS")


if __name__ == "__main__":
    main()
