import os
import shutil
import shlex
import socket
import subprocess
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from bridge_protocol import now_ms
from debug_events import EventSink, NullEventSink

try:
    import yaml
except ImportError:  # pragma: no cover - exercised only on minimal systems
    yaml = None


class VideoConfigError(ValueError):
    pass


DEFAULT_LOG_DIR = "/tmp/rtsp_logs"
DEFAULT_STARTUP_CHECK_SECONDS = 1.5
DEFAULT_DEVICE_TIMEOUT_SECONDS = 5.0
DEFAULT_RESTART_BACKOFF_SECONDS = 5.0


@dataclass
class RtspConfig:
    host: str = "127.0.0.1"
    publish_host: str = "127.0.0.1"
    port: int = 8554
    server: str = "mediamtx"
    transport: str = "tcp"

    def public_url(self, path: str) -> str:
        return f"rtsp://{self.host}:{self.port}/{path}"

    def publish_url(self, path: str) -> str:
        return f"rtsp://{self.publish_host}:{self.port}/{path}"


@dataclass
class DirectVideoSource:
    camera_id: int
    name: str
    device: str
    rtsp_path: str
    source_id: str = ""
    runner: str = "ffmpeg"
    input_format: str = "mjpeg"
    input_width: int = 1280
    input_height: int = 720
    width: int = 1280
    height: int = 720
    fps: int = 30
    codec: str = "h264"
    bitrate_kbps: int = 2500
    enabled: bool = True
    slot_hint: Optional[int] = None
    ffmpeg_path: str = "ffmpeg"
    vf: str = ""
    crop_aspect: str = ""
    video_filter: str = ""
    gop: int = 0
    keyint_min: int = 0
    x264_params: str = "repeat-headers=1"
    extra_input_args: List[str] = field(default_factory=list)
    extra_output_args: List[str] = field(default_factory=list)

    def __post_init__(self) -> None:
        if not self.source_id:
            self.source_id = f"camera_{self.camera_id}"
        if self.slot_hint is None:
            self.slot_hint = self.camera_id
        self.input_format = normalize_v4l2_input_format(self.input_format)

    def output_size(self) -> Tuple[int, int]:
        if self.width and self.height:
            return self.width, self.height
        crop = crop_dimensions_for_aspect(self.input_width, self.input_height, self.crop_aspect)
        if crop is None:
            return self.input_width, self.input_height
        return crop[0], crop[1]

    def camera_info(
        self,
        rtsp: RtspConfig,
        online: bool = False,
        last_error: str = "",
    ) -> Dict[str, Any]:
        output_width, output_height = self.output_size()
        return {
            "camera_id": self.camera_id,
            "source_id": self.source_id,
            "slot_hint": self.slot_hint,
            "name": self.name,
            "kind": "direct",
            "online": online,
            "rtsp_url": rtsp.public_url(self.rtsp_path) if online else "",
            "rtsp_transport": rtsp.transport,
            "codec": self.codec,
            "width": output_width,
            "height": output_height,
            "source_width": self.input_width,
            "source_height": self.input_height,
            "fps": self.fps,
            "bitrate_kbps": self.bitrate_kbps if online else 0,
            "profile": "low_latency",
            "vf": self.vf,
            "crop_aspect": self.crop_aspect,
            "video_filter": self.video_filter,
            "last_error": last_error,
        }


@dataclass
class VideoConfig:
    rtsp: RtspConfig
    direct_sources: List[DirectVideoSource]

    def camera_infos(self) -> List[Dict[str, Any]]:
        return [source.camera_info(self.rtsp, online=False) for source in self.direct_sources]


@dataclass
class DirectSourceRuntime:
    source: DirectVideoSource
    online: bool = False
    desired_online: bool = False
    last_error: str = ""
    process: Optional[subprocess.Popen] = None
    started_at_ms: int = 0
    stopped_at_ms: int = 0
    restart_count: int = 0
    last_exit_code: Optional[int] = None
    command: List[str] = field(default_factory=list)
    log_path: str = ""
    next_restart_at_ms: int = 0

    def camera_info(self, rtsp: RtspConfig) -> Dict[str, Any]:
        info = self.source.camera_info(rtsp, online=self.online, last_error=self.last_error)
        info["pid"] = self.process.pid if self.process and self.process.poll() is None else 0
        info["started_at_ms"] = self.started_at_ms
        info["stopped_at_ms"] = self.stopped_at_ms
        info["restart_count"] = self.restart_count
        info["last_exit_code"] = self.last_exit_code
        info["desired_online"] = self.desired_online
        info["command"] = " ".join(shlex.quote(part) for part in self.command)
        info["log_path"] = self.log_path
        info["next_restart_at_ms"] = self.next_restart_at_ms
        return info


class VideoManager:
    def __init__(
        self,
        config: VideoConfig,
        dry_run: bool = False,
        events: Optional[EventSink] = None,
        log_dir: str = DEFAULT_LOG_DIR,
        startup_check_sec: float = DEFAULT_STARTUP_CHECK_SECONDS,
        device_timeout_sec: float = DEFAULT_DEVICE_TIMEOUT_SECONDS,
        restart_backoff_sec: float = DEFAULT_RESTART_BACKOFF_SECONDS,
    ) -> None:
        self.config = config
        self.dry_run = dry_run
        self.events = events or NullEventSink()
        self.log_dir = Path(log_dir)
        self.startup_check_sec = max(0.0, startup_check_sec)
        self.device_timeout_sec = max(0.0, device_timeout_sec)
        self.restart_backoff_sec = max(0.0, restart_backoff_sec)
        self._lock = threading.RLock()
        self._runtime: Dict[int, DirectSourceRuntime] = {
            source.camera_id: DirectSourceRuntime(source=source)
            for source in config.direct_sources
        }

    @classmethod
    def from_config_path(
        cls,
        path: str,
        dry_run: bool = False,
        events: Optional[EventSink] = None,
        log_dir: str = DEFAULT_LOG_DIR,
        startup_check_sec: float = DEFAULT_STARTUP_CHECK_SECONDS,
        device_timeout_sec: float = DEFAULT_DEVICE_TIMEOUT_SECONDS,
        restart_backoff_sec: float = DEFAULT_RESTART_BACKOFF_SECONDS,
    ) -> "VideoManager":
        return cls(
            load_video_config(path),
            dry_run=dry_run,
            events=events,
            log_dir=log_dir,
            startup_check_sec=startup_check_sec,
            device_timeout_sec=device_timeout_sec,
            restart_backoff_sec=restart_backoff_sec,
        )

    def start_enabled(self) -> List[Dict[str, Any]]:
        changed: List[Dict[str, Any]] = []
        for source in self.config.direct_sources:
            if source.enabled:
                changed.append(self.start(source.camera_id))
        return changed

    def stop_all(self) -> List[Dict[str, Any]]:
        changed: List[Dict[str, Any]] = []
        for camera_id in list(self._runtime.keys()):
            changed.append(self.stop(camera_id))
        return changed

    def start(self, camera_id: int) -> Dict[str, Any]:
        with self._lock:
            runtime = self._get_runtime(camera_id)
            runtime.desired_online = True
            self._refresh_runtime_locked(runtime)
            if runtime.online:
                return runtime.camera_info(self.config.rtsp)

            command = self.build_ffmpeg_command(runtime.source)
            runtime.command = command
            runtime.last_error = ""
            runtime.last_exit_code = None
            runtime.started_at_ms = now_ms()
            runtime.stopped_at_ms = 0

            if self.dry_run:
                runtime.online = True
                runtime.process = None
                runtime.next_restart_at_ms = 0
                self.events.emit("video", "dry-run stream started", data={
                    "camera_id": camera_id,
                    "command": runtime.camera_info(self.config.rtsp)["command"],
                })
                return runtime.camera_info(self.config.rtsp)

            if not is_rtsp_listening(self.config.rtsp.port):
                self._mark_start_failed(
                    runtime,
                    f"MediaMTX is not listening on port {self.config.rtsp.port}",
                )
                return runtime.camera_info(self.config.rtsp)

            device = runtime.source.device
            if not os.path.exists(device):
                self._mark_start_failed(runtime, f"missing device: {device}")
                return runtime.camera_info(self.config.rtsp)

            if not wait_for_device(device, self.device_timeout_sec):
                self._mark_start_failed(runtime, f"device busy: {device}")
                return runtime.camera_info(self.config.rtsp)

            try:
                self.log_dir.mkdir(parents=True, exist_ok=True)
                runtime.log_path = str(self.log_dir / f"{runtime.source.rtsp_path}.log")
                with open(runtime.log_path, "w", encoding="utf-8") as log:
                    runtime.process = subprocess.Popen(
                        command,
                        stdin=subprocess.DEVNULL,
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        start_new_session=True,
                    )
                runtime.online = True
                runtime.next_restart_at_ms = 0
                self.events.emit("video", "stream process started", data={
                    "camera_id": camera_id,
                    "pid": runtime.process.pid,
                    "log_path": runtime.log_path,
                })
                if self.startup_check_sec > 0:
                    time.sleep(self.startup_check_sec)
                    exit_code = runtime.process.poll()
                    if exit_code is not None:
                        runtime.online = False
                        runtime.last_exit_code = exit_code
                        runtime.process = None
                        runtime.stopped_at_ms = now_ms()
                        runtime.last_error = self._startup_exit_error(runtime, exit_code)
                        runtime.next_restart_at_ms = self._next_restart_ms()
                        self.events.emit("video", "stream exited during startup", level="error", data={
                            "camera_id": camera_id,
                            "exit_code": exit_code,
                            "log_path": runtime.log_path,
                        })
            except OSError as exc:
                self._mark_start_failed(runtime, str(exc))
                self.events.emit("video", "stream process failed to start", level="error", data={
                    "camera_id": camera_id,
                    "error": str(exc),
                })

            return runtime.camera_info(self.config.rtsp)

    def stop(self, camera_id: int) -> Dict[str, Any]:
        with self._lock:
            runtime = self._get_runtime(camera_id)
            runtime.desired_online = False
            process = runtime.process
            if process and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2.0)
            runtime.process = None
            runtime.online = False
            runtime.stopped_at_ms = now_ms()
            runtime.next_restart_at_ms = 0
            runtime.last_exit_code = process.returncode if process else runtime.last_exit_code
            self.events.emit("video", "stream stopped", data={"camera_id": camera_id})
            return runtime.camera_info(self.config.rtsp)

    def restart(self, camera_id: int) -> Dict[str, Any]:
        with self._lock:
            runtime = self._get_runtime(camera_id)
            runtime.restart_count += 1
        self.stop(camera_id)
        return self.start(camera_id)

    def camera_infos(self) -> List[Dict[str, Any]]:
        with self._lock:
            return [
                self._runtime[source.camera_id].camera_info(self.config.rtsp)
                for source in self.config.direct_sources
            ]

    def refresh(self, auto_restart: bool = False) -> List[Dict[str, Any]]:
        changed: List[Dict[str, Any]] = []
        with self._lock:
            for runtime in self._runtime.values():
                before = runtime.online
                self._refresh_runtime_locked(runtime)
                if auto_restart and runtime.desired_online and not runtime.online:
                    if runtime.next_restart_at_ms and now_ms() < runtime.next_restart_at_ms:
                        if runtime.online != before:
                            changed.append(runtime.camera_info(self.config.rtsp))
                        continue
                    runtime.restart_count += 1
                    self.events.emit("video", "stream auto-restarting", data={
                        "camera_id": runtime.source.camera_id,
                    })
                    changed.append(self.start(runtime.source.camera_id))
                    continue
                if runtime.online != before:
                    changed.append(runtime.camera_info(self.config.rtsp))
        return changed

    def build_ffmpeg_command(self, source: DirectVideoSource) -> List[str]:
        output_url = self.config.rtsp.publish_url(source.rtsp_path)
        command = [
            source.ffmpeg_path,
            "-hide_banner",
            "-loglevel",
            "warning",
            "-f",
            "v4l2",
        ]
        if source.input_format:
            command += ["-input_format", normalize_v4l2_input_format(source.input_format)]
        command += [
            "-video_size",
            f"{source.input_width}x{source.input_height}",
            "-framerate",
            str(source.fps),
        ]
        command += list(source.extra_input_args)
        command += ["-i", source.device, "-an"]
        filters = self._ffmpeg_video_filters(source)
        if filters:
            command += ["-vf", ",".join(filters)]
        command += self._ffmpeg_codec_args(source)
        if self.config.rtsp.transport:
            command += ["-rtsp_transport", self.config.rtsp.transport]
        command += list(source.extra_output_args)
        command += ["-f", "rtsp", output_url]
        return command

    def _ffmpeg_video_filters(self, source: DirectVideoSource) -> List[str]:
        filters: List[str] = []
        if source.vf:
            filters.append(source.vf)
            return filters
        crop_filter = crop_filter_for_aspect(
            source.input_width,
            source.input_height,
            source.crop_aspect,
        )
        if crop_filter:
            filters.append(crop_filter)
        if source.video_filter:
            filters.append(source.video_filter)
        return filters

    def _ffmpeg_codec_args(self, source: DirectVideoSource) -> List[str]:
        codec = source.codec.lower()
        if codec in ("copy", "passthrough"):
            return ["-c:v", "copy"]
        if codec in ("h264", "libx264"):
            return [
                "-c:v",
                "libx264",
                "-pix_fmt",
                "yuv420p",
                "-profile:v",
                "baseline",
                "-preset",
                "ultrafast",
                "-tune",
                "zerolatency",
                "-g",
                str(source.gop or max(source.fps, 1)),
                "-keyint_min",
                str(source.keyint_min or source.gop or max(source.fps, 1)),
                "-sc_threshold",
                "0",
                "-x264-params",
                source.x264_params,
                "-b:v",
                f"{source.bitrate_kbps}k",
            ]
        if codec in ("mjpeg", "mjpg"):
            return ["-c:v", "mjpeg", "-q:v", "5"]
        return ["-c:v", source.codec, "-b:v", f"{source.bitrate_kbps}k"]

    def _refresh_runtime_locked(self, runtime: DirectSourceRuntime) -> None:
        process = runtime.process
        if self.dry_run or not process:
            return
        exit_code = process.poll()
        if exit_code is None:
            runtime.online = True
            return
        runtime.online = False
        runtime.last_exit_code = exit_code
        runtime.process = None
        runtime.stopped_at_ms = now_ms()
        runtime.last_error = f"ffmpeg exited with code {exit_code}"
        runtime.next_restart_at_ms = self._next_restart_ms()
        self.events.emit("video", "stream process exited", level="warning", data={
            "camera_id": runtime.source.camera_id,
            "exit_code": exit_code,
        })

    def _mark_start_failed(self, runtime: DirectSourceRuntime, message: str) -> None:
        runtime.online = False
        runtime.process = None
        runtime.stopped_at_ms = now_ms()
        runtime.last_error = message
        runtime.next_restart_at_ms = self._next_restart_ms()

    def _startup_exit_error(self, runtime: DirectSourceRuntime, exit_code: int) -> str:
        tail = log_tail(Path(runtime.log_path)) if runtime.log_path else ""
        if not tail:
            return f"ffmpeg exited during startup with code {exit_code}"
        return f"ffmpeg exited during startup with code {exit_code}: {tail}"

    def _next_restart_ms(self) -> int:
        if self.restart_backoff_sec <= 0:
            return 0
        return now_ms() + int(self.restart_backoff_sec * 1000)

    def _get_runtime(self, camera_id: int) -> DirectSourceRuntime:
        if camera_id not in self._runtime:
            raise KeyError(f"unknown camera_id: {camera_id}")
        return self._runtime[camera_id]


def load_video_config(path: str) -> VideoConfig:
    if yaml is None:
        raise VideoConfigError("PyYAML is required to load video source configuration")
    if not path:
        raise VideoConfigError("video config path is empty")
    if not os.path.exists(path):
        raise VideoConfigError(f"video config not found: {path}")

    with open(path, "r", encoding="utf-8") as handle:
        data = yaml.safe_load(handle) or {}
    if not isinstance(data, dict):
        raise VideoConfigError("video config root must be a mapping")
    return parse_video_config(data)


def normalize_v4l2_input_format(input_format: str) -> str:
    normalized = input_format.strip()
    key = normalized.lower()
    aliases = {
        "mjpeg": "mjpeg",
        "mjpg": "mjpeg",
        "mjpe": "mjpeg",
        "jpeg": "mjpeg",
        "yuyv": "yuyv422",
        "yuyv422": "yuyv422",
        "yuy2": "yuyv422",
        "h264": "h264",
        "h265": "hevc",
        "hevc": "hevc",
    }
    return aliases.get(key, key)


def is_rtsp_listening(port: int) -> bool:
    if not shutil.which("ss"):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return True
        except OSError:
            return False
    result = subprocess.run(
        ["ss", "-lnt"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    return f":{port} " in result.stdout


def log_tail(log_path: Path, max_bytes: int = 4096) -> str:
    if not log_path.exists():
        return ""
    with open(log_path, "rb") as handle:
        try:
            handle.seek(-max_bytes, os.SEEK_END)
        except OSError:
            handle.seek(0)
        return handle.read().decode("utf-8", errors="replace").replace("\r", "\n").strip()


def is_device_busy(device: str) -> bool:
    if not shutil.which("fuser"):
        return False
    return subprocess.run(
        ["fuser", "-s", device],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    ).returncode == 0


def wait_for_device(device: str, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not is_device_busy(device):
            return True
        time.sleep(0.2)
    return not is_device_busy(device)


def parse_video_config(data: Dict[str, Any]) -> VideoConfig:
    rtsp_data = data.get("rtsp", {}) or {}
    if not isinstance(rtsp_data, dict):
        raise VideoConfigError("rtsp must be a mapping")

    host_default = rtsp_data.get("host", "127.0.0.1")
    host = _required_str(rtsp_data, "public_host", default=host_default)
    publish_host = _required_str(rtsp_data, "publish_host", default="127.0.0.1")
    rtsp = RtspConfig(
        host=host,
        publish_host=publish_host,
        port=_positive_int(rtsp_data, "port", default=8554),
        server=_required_str(rtsp_data, "server", default="mediamtx"),
        transport=_rtsp_transport(rtsp_data),
    )

    source_items = data.get("direct_sources", []) or []
    if not isinstance(source_items, list):
        raise VideoConfigError("direct_sources must be a list")

    sources: List[DirectVideoSource] = []
    for index, item in enumerate(source_items):
        if not isinstance(item, dict):
            raise VideoConfigError(f"direct_sources[{index}] must be a mapping")
        if not _bool_value(item, "enabled", default=True):
            continue
        sources.append(parse_direct_source(item, index))
    _validate_unique_camera_ids(sources)
    _validate_unique_rtsp_paths(sources)
    return VideoConfig(rtsp=rtsp, direct_sources=sources)


def parse_direct_source(item: Any, index: int) -> DirectVideoSource:
    if not isinstance(item, dict):
        raise VideoConfigError(f"direct_sources[{index}] must be a mapping")

    camera_id = _positive_int(item, "camera_id")
    if camera_id > 5:
        raise VideoConfigError(f"direct_sources[{index}].camera_id must be between 0 and 5")

    base_width = _positive_int(item, "width", default=1280)
    base_height = _positive_int(item, "height", default=720)
    input_width = _positive_int(item, "input_width", default=base_width)
    input_height = _positive_int(item, "input_height", default=base_height)
    output_width = _positive_int(item, "output_width", default=base_width)
    output_height = _positive_int(item, "output_height", default=base_height)
    vf = _optional_str(item, "vf")
    crop_aspect = _optional_aspect(item, "crop_aspect")
    explicit_output_size = (
        "input_width" in item
        or "input_height" in item
        or "output_width" in item
        or "output_height" in item
        or bool(vf)
    )
    if crop_aspect and not explicit_output_size:
        crop = crop_dimensions_for_aspect(input_width, input_height, crop_aspect)
        if crop is not None:
            output_width, output_height = crop[0], crop[1]

    source = DirectVideoSource(
        camera_id=camera_id,
        source_id=_required_str(item, "source_id", default=f"camera_{camera_id}"),
        slot_hint=_optional_int(item, "slot_hint"),
        name=_required_str(item, "name", default=f"Camera {camera_id}"),
        runner=_required_str(item, "runner", default="ffmpeg"),
        device=_required_str(item, "device"),
        input_format=_required_str(item, "input_format", default="mjpeg"),
        input_width=input_width,
        input_height=input_height,
        width=output_width,
        height=output_height,
        fps=_positive_int(item, "fps", default=30),
        codec=_required_str(item, "codec", default="h264"),
        bitrate_kbps=_positive_int(item, "bitrate_kbps", default=2500),
        rtsp_path=_required_str(item, "rtsp_path"),
        enabled=_bool_value(item, "enabled", default=True),
        ffmpeg_path=_required_str(item, "ffmpeg_path", default="ffmpeg"),
        vf=vf,
        crop_aspect=crop_aspect,
        video_filter=_optional_str(item, "video_filter"),
        gop=_positive_int(item, "gop", default=0),
        keyint_min=_positive_int(item, "keyint_min", default=0),
        x264_params=_required_str(item, "x264_params", default="repeat-headers=1"),
        extra_input_args=_string_list(item, "extra_input_args"),
        extra_output_args=_string_list(item, "extra_output_args"),
    )

    if source.runner != "ffmpeg":
        raise VideoConfigError(
            f"direct_sources[{index}].runner={source.runner!r} is not supported yet"
        )
    if "/" in source.rtsp_path.strip("/"):
        raise VideoConfigError(f"direct_sources[{index}].rtsp_path must be a single path segment")
    if source.codec.lower() in ("copy", "passthrough") and (
        source.vf or source.crop_aspect or source.video_filter
    ):
        raise VideoConfigError(
            f"direct_sources[{index}] cannot use codec={source.codec!r} with video filters"
        )
    return source


def _validate_unique_camera_ids(sources: List[DirectVideoSource]) -> None:
    seen: Dict[int, str] = {}
    for source in sources:
        if source.camera_id in seen:
            raise VideoConfigError(
                f"duplicate camera_id {source.camera_id}: {seen[source.camera_id]} and {source.name}"
            )
        seen[source.camera_id] = source.name


def _validate_unique_rtsp_paths(sources: List[DirectVideoSource]) -> None:
    seen: Dict[str, str] = {}
    for source in sources:
        if source.rtsp_path in seen:
            raise VideoConfigError(
                f"duplicate rtsp_path {source.rtsp_path}: {seen[source.rtsp_path]} and {source.name}"
            )
        seen[source.rtsp_path] = source.name


def _required_str(data: Dict[str, Any], key: str, default: Optional[str] = None) -> str:
    value = data.get(key, default)
    if value is None:
        raise VideoConfigError(f"{key} is required")
    if not isinstance(value, str) or not value.strip():
        raise VideoConfigError(f"{key} must be a non-empty string")
    return value.strip()


def _optional_str(data: Dict[str, Any], key: str) -> str:
    value = data.get(key, "")
    if value is None:
        return ""
    if not isinstance(value, str):
        raise VideoConfigError(f"{key} must be a string")
    return value.strip()


def _optional_aspect(data: Dict[str, Any], key: str) -> str:
    aspect = _optional_str(data, key)
    if not aspect:
        return ""
    parse_aspect_ratio(aspect)
    return aspect


def parse_aspect_ratio(aspect: str) -> float:
    normalized = aspect.strip().lower().replace("：", ":")
    if ":" in normalized:
        left, right = normalized.split(":", 1)
    elif "/" in normalized:
        left, right = normalized.split("/", 1)
    else:
        raise VideoConfigError("crop_aspect must be formatted like 4:3 or 16:9")
    try:
        width = float(left)
        height = float(right)
    except ValueError as exc:
        raise VideoConfigError("crop_aspect contains non-numeric values") from exc
    if width <= 0.0 or height <= 0.0:
        raise VideoConfigError("crop_aspect values must be positive")
    return width / height


def crop_filter_for_aspect(width: int, height: int, aspect: str) -> str:
    crop = crop_dimensions_for_aspect(width, height, aspect)
    if crop is None:
        return ""
    crop_w, crop_h, x, y = crop
    return f"crop={crop_w}:{crop_h}:{x}:{y}"


def crop_dimensions_for_aspect(width: int, height: int, aspect: str) -> Optional[Tuple[int, int, int, int]]:
    if not aspect:
        return None

    target = parse_aspect_ratio(aspect)
    current = width / height
    if abs(current - target) < 0.001:
        return None

    if current > target:
        crop_h = height
        crop_w = int(crop_h * target)
    else:
        crop_w = width
        crop_h = int(crop_w / target)

    crop_w = max(2, crop_w - crop_w % 2)
    crop_h = max(2, crop_h - crop_h % 2)
    x = max(0, (width - crop_w) // 2)
    y = max(0, (height - crop_h) // 2)
    x -= x % 2
    y -= y % 2
    return crop_w, crop_h, x, y


def _rtsp_transport(data: Dict[str, Any]) -> str:
    transport = _required_str(data, "transport", default="tcp").lower()
    if transport not in ("tcp", "udp"):
        raise VideoConfigError("rtsp.transport must be tcp or udp")
    return transport


def _positive_int(data: Dict[str, Any], key: str, default: Optional[int] = None) -> int:
    value = data.get(key, default)
    if value is None:
        raise VideoConfigError(f"{key} is required")
    if isinstance(value, bool):
        raise VideoConfigError(f"{key} must be an integer")
    try:
        parsed = int(value)
    except (TypeError, ValueError) as exc:
        raise VideoConfigError(f"{key} must be an integer") from exc
    if parsed < 0:
        raise VideoConfigError(f"{key} must be >= 0")
    return parsed


def _optional_int(data: Dict[str, Any], key: str) -> Optional[int]:
    if key not in data or data[key] is None:
        return None
    return _positive_int(data, key)


def _bool_value(data: Dict[str, Any], key: str, default: bool) -> bool:
    value = data.get(key, default)
    if not isinstance(value, bool):
        raise VideoConfigError(f"{key} must be true or false")
    return value


def _string_list(data: Dict[str, Any], key: str) -> List[str]:
    value = data.get(key, []) or []
    if not isinstance(value, list):
        raise VideoConfigError(f"{key} must be a list of strings")
    result: List[str] = []
    for item in value:
        if not isinstance(item, str) or not item:
            raise VideoConfigError(f"{key} must contain only non-empty strings")
        result.append(item)
    return result
