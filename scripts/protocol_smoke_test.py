#!/usr/bin/env python3
import json
import os
import socket
import subprocess
import sys
import time


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SERVER = os.path.join(ROOT, "scripts", "mock_robot_server.py")
HOST = "127.0.0.1"
PORT = int(os.getenv("PROTOCOL_SMOKE_PORT", "19090"))


class JsonLineClient:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=1.0)
        self.sock.settimeout(1.0)
        self.buffer = b""

    def close(self):
        self.sock.close()

    def send(self, payload):
        line = json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n"
        self.sock.sendall(line.encode("utf-8"))

    def recv(self, timeout=1.0):
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

    def recv_until(self, msg_type, timeout=1.0):
        deadline = time.time() + timeout
        seen = []
        while time.time() < deadline:
            msg = self.recv(max(0.01, deadline - time.time()))
            seen.append(msg.get("type"))
            if msg.get("type") == msg_type:
                return msg
        raise AssertionError(f"expected {msg_type}, seen {seen}")


def wait_for_port(timeout=3.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((HOST, PORT), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"mock server did not open {HOST}:{PORT}")


def start_server(ack_mode="ok"):
    env = os.environ.copy()
    env["MOCK_HOST"] = HOST
    env["MOCK_PORT"] = str(PORT)
    env["MOCK_ACK_MODE"] = ack_mode
    env["MOCK_DROP_INTERVAL_SEC"] = "0"
    proc = subprocess.Popen(
        [sys.executable, SERVER],
        cwd=ROOT,
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    wait_for_port()
    return proc


def stop_server(proc):
    proc.terminate()
    try:
        proc.wait(timeout=1.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def connect_client():
    client = JsonLineClient(HOST, PORT)
    hello = client.recv_until("hello")
    assert hello["protocol_version"] == 1
    capabilities = client.recv_until("capabilities")
    assert "operator_input" in capabilities["supports"]
    assert "camera_list_request" in capabilities["supports"]
    return client


def run_ok_case():
    proc = start_server("ok")
    try:
        client = connect_client()
        try:
            client.send({
                "type": "heartbeat",
                "protocol_version": 1,
                "seq": 1,
                "timestamp_ms": 1000,
            })
            heartbeat_ack = client.recv_until("heartbeat_ack")
            assert heartbeat_ack["seq"] == 1

            client.send({
                "type": "operator_input",
                "protocol_version": 1,
                "seq": 2,
                "timestamp_ms": 1001,
                "ttl_ms": 300,
                "mode": "vehicle",
                "keyboard": {"pressed_keys": ["w", "d"]},
                "gamepad": {"connected": False, "buttons": {}, "axes": {}},
            })
            status = client.recv_until("system_status")
            assert status["last_operator_input_seq"] == 2

            client.send({
                "type": "sync_request",
                "protocol_version": 1,
                "seq": 3,
                "timestamp_ms": 1002,
                "params": {"reason": "protocol_smoke"},
            })
            snapshot = client.recv_until("system_snapshot")
            assert snapshot["seq"] == 3
            assert snapshot["control_mode"] == "vehicle"

            client.send({
                "type": "camera_list_request",
                "protocol_version": 1,
                "seq": 4,
                "timestamp_ms": 1003,
            })
            cameras = client.recv_until("camera_list_response")
            assert cameras["seq"] == 4
            assert len(cameras["cameras"]) >= 1
            assert any(camera["camera_id"] == 5 for camera in cameras["cameras"])

            client.send({
                "type": "emergency_stop",
                "protocol_version": 1,
                "seq": 5,
                "timestamp_ms": 1004,
                "params": {"source": "protocol_smoke"},
            })
            ack = client.recv_until("ack")
            assert ack["seq"] == 5
            assert ack["ack_type"] == "emergency_stop"
            assert ack["ok"] is True

            client.send({
                "type": "unsupported_probe",
                "protocol_version": 1,
                "seq": 6,
                "timestamp_ms": 1005,
            })
            error = client.recv_until("protocol_error")
            assert error["seq"] == 6
            assert error["code"] == 1001
        finally:
            client.close()
    finally:
        stop_server(proc)
    print("protocol ok case: PASS")


def run_ack_mode_case(mode, expected_ack):
    proc = start_server(mode)
    try:
        client = connect_client()
        try:
            client.send({
                "type": "emergency_stop",
                "protocol_version": 1,
                "seq": 20,
                "timestamp_ms": 2000,
                "params": {"source": f"protocol_smoke_{mode}"},
            })
            if expected_ack is None:
                try:
                    msg = client.recv(timeout=0.4)
                except TimeoutError:
                    msg = None
                assert msg is None or msg.get("type") != "ack"
            else:
                ack = client.recv_until("ack")
                assert ack["ok"] is expected_ack
        finally:
            client.close()
    finally:
        stop_server(proc)
    print(f"protocol ack mode {mode}: PASS")


def main():
    run_ok_case()
    run_ack_mode_case("fail", False)
    run_ack_mode_case("drop", None)


if __name__ == "__main__":
    main()
