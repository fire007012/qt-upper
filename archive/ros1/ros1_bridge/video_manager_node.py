#!/usr/bin/env python3
import argparse
import json
import os
import signal
import socket
import sys
import threading
import time
from typing import Any, Dict, Optional

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from video_manager import VideoConfigError, VideoManager, load_video_config
from video_manager_ipc import DEFAULT_VIDEO_MANAGER_HOST, DEFAULT_VIDEO_MANAGER_PORT, MAX_FRAME_BYTES


DEFAULT_CONFIG = os.path.join(os.path.dirname(__file__), "video_sources.local.yaml")


class VideoManagerNode:
    def __init__(
        self,
        config_path: str,
        host: str,
        port: int,
        dry_run: bool = False,
        autostart: bool = False,
        poll_sec: float = 1.0,
        rtsp_public_host: str = "",
        rtsp_publish_host: str = "",
        rtsp_port: int = 0,
        log_dir: str = "/tmp/rtsp_logs",
        startup_check_sec: float = 1.5,
        device_timeout_sec: float = 5.0,
        restart_backoff_sec: float = 5.0,
    ) -> None:
        self.config_path = config_path
        self.host = host
        self.port = port
        self.dry_run = dry_run
        self.autostart = autostart
        self.poll_sec = max(0.1, poll_sec)
        self.rtsp_public_host = rtsp_public_host
        self.rtsp_publish_host = rtsp_publish_host
        self.rtsp_port = rtsp_port
        self.log_dir = log_dir
        self.startup_check_sec = startup_check_sec
        self.device_timeout_sec = device_timeout_sec
        self.restart_backoff_sec = restart_backoff_sec
        self.stop_event = threading.Event()
        self._manager_lock = threading.RLock()
        self.manager = self._load_manager(self.config_path)

    def _load_manager(self, config_path: str) -> VideoManager:
        config = load_video_config(config_path)
        if self.rtsp_public_host:
            config.rtsp.host = self.rtsp_public_host
        if self.rtsp_publish_host:
            config.rtsp.publish_host = self.rtsp_publish_host
        if self.rtsp_port:
            config.rtsp.port = self.rtsp_port
        return VideoManager(
            config,
            dry_run=self.dry_run,
            log_dir=self.log_dir,
            startup_check_sec=self.startup_check_sec,
            device_timeout_sec=self.device_timeout_sec,
            restart_backoff_sec=self.restart_backoff_sec,
        )

    def serve_forever(self) -> None:
        threading.Thread(target=self._monitor_loop, daemon=True).start()
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
                server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                server.bind((self.host, self.port))
                server.listen(8)
                server.settimeout(0.5)
                print(f"[video-manager] listening on {self.host}:{self.port}", flush=True)
                if self.autostart:
                    threading.Thread(target=self._autostart_streams, daemon=True).start()
                while not self.stop_event.is_set():
                    try:
                        conn, addr = server.accept()
                    except socket.timeout:
                        continue
                    except OSError:
                        break
                    threading.Thread(target=self._handle_client, args=(conn, addr), daemon=True).start()
        finally:
            with self._manager_lock:
                stopped = self.manager.stop_all()
            print(f"[video-manager] stopped {len(stopped)} stream(s)", flush=True)

    def stop(self) -> None:
        self.stop_event.set()

    def _autostart_streams(self) -> None:
        started = self.manager.start_enabled()
        print(f"[video-manager] autostarted {len(started)} stream(s)", flush=True)

    def _monitor_loop(self) -> None:
        while not self.stop_event.wait(self.poll_sec):
            with self._manager_lock:
                changed = self.manager.refresh(auto_restart=True)
            for camera in changed:
                print(
                    "[video-manager] camera state changed "
                    f"camera_id={camera.get('camera_id')} online={camera.get('online')}",
                    flush=True,
                )

    def _handle_client(self, conn: socket.socket, addr: Any) -> None:
        del addr
        with conn:
            conn.settimeout(2.0)
            buffer = b""
            while not self.stop_event.is_set():
                try:
                    chunk = conn.recv(4096)
                except socket.timeout:
                    continue
                except OSError:
                    return
                if not chunk:
                    return
                buffer += chunk
                if len(buffer) > MAX_FRAME_BYTES and b"\n" not in buffer:
                    _send_json(conn, _error_response({}, 2001, "request exceeds max frame size"))
                    return
                while b"\n" in buffer:
                    line, buffer = buffer.split(b"\n", 1)
                    if not line.strip():
                        continue
                    response = self._handle_line(line)
                    _send_json(conn, response)

    def _handle_line(self, line: bytes) -> Dict[str, Any]:
        try:
            request = json.loads(line.decode("utf-8"))
            if not isinstance(request, dict):
                raise ValueError("JSON frame is not an object")
        except Exception as exc:
            return _error_response({}, 2100, f"invalid JSON: {exc}")
        return self._handle_request(request)

    def _handle_request(self, request: Dict[str, Any]) -> Dict[str, Any]:
        msg_type = str(request.get("type", "")).strip()
        seq = int(request.get("seq", 0) or 0)
        try:
            if msg_type == "ping":
                return _ok_response(seq, "pong")
            if msg_type == "list_cameras":
                with self._manager_lock:
                    self.manager.refresh()
                    cameras = self.manager.camera_infos()
                response = _ok_response(seq, "ok")
                response["type"] = "camera_list"
                response["cameras"] = cameras
                return response
            if msg_type == "diagnostics":
                return self._handle_diagnostics(request)
            if msg_type == "stream_request":
                return self._handle_stream_request(request)
            if msg_type == "reload_config":
                return self._handle_reload_config(request)
            if msg_type == "stop_all":
                with self._manager_lock:
                    cameras = self.manager.stop_all()
                response = _ok_response(seq, "all streams stopped")
                response["type"] = "stop_all_response"
                response["cameras"] = cameras
                return response
            return _error_response(request, 2101, f"unsupported request type: {msg_type}")
        except KeyError as exc:
            return _error_response(request, 2403, str(exc).strip("'"))
        except Exception as exc:
            return _error_response(request, 2500, str(exc))

    def _handle_stream_request(self, request: Dict[str, Any]) -> Dict[str, Any]:
        seq = int(request.get("seq", 0) or 0)
        try:
            camera_id = int(request.get("camera_id"))
        except (TypeError, ValueError):
            return _error_response(request, 2401, "camera_id must be an integer")
        action = str(request.get("action", "")).strip().lower()
        with self._manager_lock:
            if action == "start":
                camera = self.manager.start(camera_id)
            elif action == "stop":
                camera = self.manager.stop(camera_id)
            elif action == "restart":
                camera = self.manager.restart(camera_id)
            else:
                return _error_response(request, 2402, f"unsupported stream action: {action}")
        ok = action == "stop" or bool(camera.get("online", False))
        message = f"camera stream {action} accepted" if ok else str(camera.get("last_error", "stream failed"))
        response = _ok_response(seq, message) if ok else _error_response(request, 2501, message)
        response["type"] = "stream_response"
        response["camera"] = camera
        return response

    def _handle_diagnostics(self, request: Dict[str, Any]) -> Dict[str, Any]:
        seq = int(request.get("seq", 0) or 0)
        with self._manager_lock:
            self.manager.refresh()
            cameras = self.manager.camera_infos()
            config = self.manager.config
        response = _ok_response(seq, "ok")
        response["type"] = "diagnostics"
        response["config_path"] = self.config_path
        response["dry_run"] = self.dry_run
        response["autostart"] = self.autostart
        response["log_dir"] = self.log_dir
        response["rtsp"] = {
            "public_host": config.rtsp.host,
            "publish_host": config.rtsp.publish_host,
            "port": config.rtsp.port,
            "transport": config.rtsp.transport,
        }
        response["mediamtx_listening"] = _port_listening(config.rtsp.port)
        response["cameras"] = cameras
        response["summary"] = {
            "total": len(cameras),
            "online": sum(1 for camera in cameras if camera.get("online")),
            "desired_online": sum(1 for camera in cameras if camera.get("desired_online")),
            "errors": sum(1 for camera in cameras if camera.get("last_error")),
        }
        return response

    def _handle_reload_config(self, request: Dict[str, Any]) -> Dict[str, Any]:
        seq = int(request.get("seq", 0) or 0)
        new_path = str(request.get("config_path") or self.config_path)
        with self._manager_lock:
            new_manager = self._load_manager(new_path)
            old_manager = self.manager
            old_manager.stop_all()
            self.config_path = new_path
            self.manager = new_manager
            if self.autostart:
                self.manager.start_enabled()
            cameras = self.manager.camera_infos()
        response = _ok_response(seq, "video config reloaded")
        response["type"] = "reload_config_response"
        response["cameras"] = cameras
        return response


def _ok_response(seq: int, message: str) -> Dict[str, Any]:
    return {
        "type": "ok",
        "seq": seq,
        "ok": True,
        "code": 0,
        "message": message,
        "timestamp_ms": int(time.time() * 1000),
    }


def _error_response(request: Dict[str, Any], code: int, message: str) -> Dict[str, Any]:
    return {
        "type": "error",
        "seq": int(request.get("seq", 0) or 0),
        "ok": False,
        "code": code,
        "message": message,
        "timestamp_ms": int(time.time() * 1000),
    }


def _send_json(conn: socket.socket, payload: Dict[str, Any]) -> None:
    line = json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n"
    conn.sendall(line.encode("utf-8"))


def _port_listening(port: int) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
            return True
    except OSError:
        return False


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Direct RTSP video lifecycle manager")
    parser.add_argument("--config", default=DEFAULT_CONFIG)
    parser.add_argument("--host", default=DEFAULT_VIDEO_MANAGER_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_VIDEO_MANAGER_PORT)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--autostart", action="store_true")
    parser.add_argument("--poll-sec", type=float, default=1.0)
    parser.add_argument("--rtsp-public-host", default="")
    parser.add_argument("--rtsp-publish-host", default="")
    parser.add_argument("--rtsp-port", type=int, default=0)
    parser.add_argument("--log-dir", default="/tmp/rtsp_logs")
    parser.add_argument("--startup-check-sec", type=float, default=1.5)
    parser.add_argument("--device-timeout-sec", type=float, default=5.0)
    parser.add_argument("--restart-backoff-sec", type=float, default=5.0)
    return parser


def parse_node_args() -> argparse.Namespace:
    return build_arg_parser().parse_args([
        arg for arg in sys.argv[1:] if ":=" not in arg
    ])


def main() -> int:
    args = parse_node_args()
    try:
        node = VideoManagerNode(
            config_path=os.path.expanduser(args.config),
            host=args.host,
            port=args.port,
            dry_run=args.dry_run,
            autostart=args.autostart,
            poll_sec=args.poll_sec,
            rtsp_public_host=args.rtsp_public_host,
            rtsp_publish_host=args.rtsp_publish_host,
            rtsp_port=args.rtsp_port,
            log_dir=args.log_dir,
            startup_check_sec=args.startup_check_sec,
            device_timeout_sec=args.device_timeout_sec,
            restart_backoff_sec=args.restart_backoff_sec,
        )
    except VideoConfigError as exc:
        print(f"[video-manager] failed to load config: {exc}", flush=True)
        return 1

    signal.signal(signal.SIGTERM, lambda signum, frame: node.stop())
    signal.signal(signal.SIGINT, lambda signum, frame: node.stop())
    node.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
