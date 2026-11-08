import json
import socket
import threading
from typing import Any, Dict, List, Optional, Tuple

from debug_events import EventSink, NullEventSink


DEFAULT_VIDEO_MANAGER_HOST = "127.0.0.1"
DEFAULT_VIDEO_MANAGER_PORT = 18081
MAX_FRAME_BYTES = 1024 * 1024


class VideoManagerClientError(RuntimeError):
    pass


class VideoManagerClient:
    def __init__(
        self,
        host: str = DEFAULT_VIDEO_MANAGER_HOST,
        port: int = DEFAULT_VIDEO_MANAGER_PORT,
        timeout: float = 1.0,
    ) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        self._seq = 0
        self._lock = threading.Lock()

    def ping(self) -> Dict[str, Any]:
        return self.request({"type": "ping"})

    def list_cameras(self) -> List[Dict[str, Any]]:
        response = self.request({"type": "list_cameras"})
        if not bool(response.get("ok", False)):
            raise VideoManagerClientError(
                str(response.get("message", "video manager list_cameras failed"))
            )
        cameras = response.get("cameras", [])
        if not isinstance(cameras, list):
            raise VideoManagerClientError("video manager returned invalid cameras field")
        return [camera for camera in cameras if isinstance(camera, dict)]

    def diagnostics(self) -> Dict[str, Any]:
        response = self.request({"type": "diagnostics"})
        if not bool(response.get("ok", False)):
            raise VideoManagerClientError(
                str(response.get("message", "video manager diagnostics failed"))
            )
        return response

    def stream_request(self, camera_id: int, action: str) -> Dict[str, Any]:
        return self.request({
            "type": "stream_request",
            "camera_id": camera_id,
            "action": action,
        })

    def request(self, payload: Dict[str, Any]) -> Dict[str, Any]:
        with self._lock:
            self._seq += 1
            seq = self._seq
        frame = dict(payload)
        frame["seq"] = int(frame.get("seq", seq) or seq)
        line = json.dumps(frame, ensure_ascii=False, separators=(",", ":")) + "\n"
        try:
            with socket.create_connection((self.host, self.port), timeout=self.timeout) as sock:
                sock.settimeout(self.timeout)
                sock.sendall(line.encode("utf-8"))
                response = _recv_json_line(sock, self.timeout)
        except OSError as exc:
            raise VideoManagerClientError(
                f"cannot reach video manager at {self.host}:{self.port}: {exc}"
            ) from exc
        if not isinstance(response, dict):
            raise VideoManagerClientError("video manager returned a non-object JSON frame")
        return response


class VideoManagerGateway:
    def __init__(
        self,
        client: VideoManagerClient,
        events: Optional[EventSink] = None,
    ) -> None:
        self.client = client
        self.events = events or NullEventSink()
        self._lock = threading.RLock()
        self._cameras: List[Dict[str, Any]] = []
        self._last_error = ""

    def camera_infos(self) -> List[Dict[str, Any]]:
        with self._lock:
            return [dict(camera) for camera in self._cameras]

    def last_error(self) -> str:
        return self._last_error

    def refresh(self) -> List[Dict[str, Any]]:
        try:
            cameras = self.client.list_cameras()
        except VideoManagerClientError as exc:
            self._last_error = str(exc)
            self.events.emit("video", "video manager refresh failed", level="warning", data={
                "error": self._last_error,
            })
            return []

        with self._lock:
            before = {_camera_key(camera): _stable_json(camera) for camera in self._cameras}
            self._cameras = [dict(camera) for camera in cameras]
            changed = [
                dict(camera)
                for camera in self._cameras
                if before.get(_camera_key(camera)) != _stable_json(camera)
            ]
        self._last_error = ""
        if changed:
            self.events.emit("video", "video manager camera state changed", data={
                "count": len(changed),
            })
        return changed

    def stream_request(self, params: Dict[str, Any]) -> Tuple[bool, int, str, Optional[Dict[str, Any]]]:
        try:
            camera_id = int(params.get("camera_id"))
        except (TypeError, ValueError):
            return False, 2401, "camera_id must be an integer", None

        action = str(params.get("action", "")).strip().lower()
        if action not in ("start", "stop", "restart"):
            return False, 2402, f"unsupported camera stream action: {action}", None

        try:
            response = self.client.stream_request(camera_id, action)
        except VideoManagerClientError as exc:
            return False, 2400, str(exc), None

        ok = bool(response.get("ok", False))
        code = int(response.get("code", 0 if ok else 2400) or 0)
        message = str(response.get("message", "ok" if ok else "video manager request failed"))
        camera = response.get("camera")
        if isinstance(camera, dict):
            with self._lock:
                self._upsert_camera_locked(camera)
            return ok, code, message, dict(camera)
        return ok, code, message, None

    def _upsert_camera_locked(self, camera: Dict[str, Any]) -> None:
        key = _camera_key(camera)
        for index, existing in enumerate(self._cameras):
            if _camera_key(existing) == key:
                self._cameras[index] = dict(camera)
                return
        self._cameras.append(dict(camera))


def _recv_json_line(sock: socket.socket, timeout: float) -> Dict[str, Any]:
    buffer = b""
    sock.settimeout(timeout)
    while b"\n" not in buffer:
        chunk = sock.recv(4096)
        if not chunk:
            raise VideoManagerClientError("video manager closed the connection")
        buffer += chunk
        if len(buffer) > MAX_FRAME_BYTES:
            raise VideoManagerClientError("video manager response exceeds max frame size")
    line, _ = buffer.split(b"\n", 1)
    try:
        payload = json.loads(line.decode("utf-8"))
    except Exception as exc:
        raise VideoManagerClientError(f"invalid JSON from video manager: {exc}") from exc
    if not isinstance(payload, dict):
        raise VideoManagerClientError("video manager response must be a JSON object")
    return payload


def _camera_key(camera: Dict[str, Any]) -> str:
    if "camera_id" in camera:
        return f"id:{camera.get('camera_id')}"
    return f"source:{camera.get('source_id', camera.get('name', 'unknown'))}"


def _stable_json(value: Dict[str, Any]) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
