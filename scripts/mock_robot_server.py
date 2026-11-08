#!/usr/bin/env python3
"""Standalone TCP/JSON control-protocol fixture; no ROS runtime or hardware.

Only advertises the subset implemented here. This is not a ROS2 bridge or a
management service; use the Qt communication regressions for fault coverage.
"""
import json
import os
import socket
import threading
import time

HOST = os.getenv("MOCK_HOST", "127.0.0.1")
PORT = int(os.getenv("MOCK_PORT", "9090"))
DROP_INTERVAL_SEC = float(os.getenv("MOCK_DROP_INTERVAL_SEC", "15"))
ACK_MODE = os.getenv("MOCK_ACK_MODE", "ok").lower()
ACK_DELAY_SEC = float(os.getenv("MOCK_ACK_DELAY_SEC", "0"))
CRITICAL_COMMANDS = {
    "emergency_stop",
    "clear_emergency",
    "motor_init",
    "motor_enable",
    "motor_disable",
    "set_control_mode",
    "home_arm",
    "start_task",
    "stop_task",
    "executor_action",
    "set_param",
}
CAMERAS = [
    {
        "camera_id": 0,
        "name": "front",
        "online": True,
        "rtsp_url": "rtsp://127.0.0.1:8554/cam0",
        "codec": "h264",
        "width": 1280,
        "height": 720,
        "fps": 25,
        "bitrate_kbps": 2500,
    },
    {
        "camera_id": 1,
        "name": "rear",
        "online": False,
        "rtsp_url": "",
        "codec": "h264",
        "width": 1280,
        "height": 720,
        "fps": 25,
        "bitrate_kbps": 0,
    },
    {
        "camera_id": 5,
        "name": "back",
        "online": False,
        "rtsp_url": "",
        "codec": "h264",
        "width": 1280,
        "height": 720,
        "fps": 25,
        "bitrate_kbps": 0,
    },
]


def now_ms():
    return int(time.time() * 1000)


def send_json(conn, payload):
    conn.sendall((json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8"))


def make_ack(msg, ok=True, code=0, message="ok"):
    return {
        "type": "ack",
        "protocol_version": 1,
        "ack_type": msg.get("type", "unknown"),
        "seq": msg.get("seq", 0),
        "ok": ok,
        "code": code,
        "message": message,
        "timestamp_ms": now_ms(),
    }


def make_protocol_error(msg, code=1001, message=None):
    mtype = msg.get("type", "unknown")
    return {
        "type": "protocol_error",
        "protocol_version": 1,
        "seq": msg.get("seq", 0),
        "code": code,
        "message": message or f"unsupported message type: {mtype}",
        "timestamp_ms": now_ms(),
    }


def make_hello():
    return {
        "type": "hello",
        "protocol_version": 1,
        "seq": 0,
        "timestamp_ms": now_ms(),
        "bridge_name": "mock_robot_bridge",
        "bridge_version": "mock-1.0",
        "robot_id": "mock_robot",
    }


def make_capabilities():
    return {
        "type": "capabilities",
        "protocol_version": 1,
        "seq": 0,
        "timestamp_ms": now_ms(),
        "supports": [
            "operator_input",
            "heartbeat_ack",
            "critical_ack",
            "sync_request",
            "system_snapshot",
            "camera_list_request",
            "camera_list_response",
        ],
        "max_frame_bytes": 1048576,
        "cameras": CAMERAS,
    }


def make_system_snapshot(msg):
    return {
        "type": "system_snapshot",
        "protocol_version": 1,
        "seq": msg.get("seq", 0),
        "timestamp_ms": now_ms(),
        "control_mode": "vehicle",
        "emergency": {
            "active": False,
            "source": "",
        },
        "motor": {
            "initialized": True,
            "enabled": True,
        },
        "modules": {
            "base": "online",
            "arm": "online",
            "camera": "degraded",
        },
        "last_error": {
            "code": 0,
            "message": "",
        },
    }


def make_camera_list_response(msg):
    return {
        "type": "camera_list_response",
        "protocol_version": 1,
        "seq": msg.get("seq", 0),
        "timestamp_ms": now_ms(),
        "cameras": CAMERAS,
    }


def maybe_send_ack(conn, msg, message="ok"):
    if ACK_MODE == "drop":
        print(f"[mock] drop ack for seq={msg.get('seq', 0)} type={msg.get('type', 'unknown')}")
        return

    if ACK_DELAY_SEC > 0:
        time.sleep(ACK_DELAY_SEC)

    if ACK_MODE == "fail":
        send_json(conn, make_ack(msg, ok=False, code=500, message="mock ack failure"))
        return

    send_json(conn, make_ack(msg, message=message))


def client_worker(conn, addr):
    print(f"[mock] client connected: {addr}")
    conn.settimeout(1.0)
    started = time.time()
    buffer = b""
    send_json(conn, make_hello())
    send_json(conn, make_capabilities())
    try:
        while True:
            if DROP_INTERVAL_SEC > 0 and time.time() - started > DROP_INTERVAL_SEC:
                print("[mock] force drop connection for reconnect test")
                conn.close()
                return

            try:
                data = conn.recv(4096)
            except socket.timeout:
                continue

            if not data:
                return

            buffer += data
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                if not line.strip():
                    continue
                try:
                    msg = json.loads(line.decode("utf-8", errors="ignore"))
                except Exception:
                    continue

                mtype = msg.get("type", "unknown")
                if mtype == "heartbeat":
                    send_json(conn, {
                        "type": "heartbeat_ack",
                        "protocol_version": 1,
                        "seq": msg.get("seq", 0),
                        "timestamp_ms": now_ms(),
                        "server_time_ms": now_ms(),
                    })
                elif mtype == "operator_input":
                    send_json(conn, {
                        "type": "system_status",
                        "mock": True,
                        "last_operator_input_seq": msg.get("seq", 0),
                        "timestamp_ms": now_ms(),
                    })
                elif mtype in ("sync_request", "system_snapshot_request"):
                    send_json(conn, make_system_snapshot(msg))
                elif mtype == "camera_list_request":
                    send_json(conn, make_camera_list_response(msg))
                elif mtype in CRITICAL_COMMANDS:
                    maybe_send_ack(conn, msg)
                elif mtype == "system_command":
                    maybe_send_ack(conn, msg, message=f"system command {msg.get('command', '')} accepted")
                else:
                    send_json(conn, make_protocol_error(msg))
    finally:
        try:
            conn.close()
        except Exception:
            pass
        print(f"[mock] client disconnected: {addr}")


def main():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((HOST, PORT))
        s.listen(5)
        print(f"[mock] TCP/JSON mock server listening on {HOST}:{PORT}")
        while True:
            conn, addr = s.accept()
            t = threading.Thread(target=client_worker, args=(conn, addr), daemon=True)
            t.start()


if __name__ == "__main__":
    main()
