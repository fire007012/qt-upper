#!/usr/bin/env python3
import argparse
import json
import os
import signal
import socket
import sys
import threading
from typing import Any, Dict, List, Optional, Tuple

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from bridge_core import BridgeCore, BridgeRuntime
from bridge_protocol import DEFAULT_WATCHDOG_MS, MAX_FRAME_BYTES, PROTOCOL_VERSION, now_ms
from bridge_protocol import json_line
from debug_ui import DebugHttpServer
from debug_events import EventSink, RingBufferEventSink
from output_adapters import DryRunOutput, OutputAdapter, RosOutput
from video_manager_ipc import (
    DEFAULT_VIDEO_MANAGER_HOST,
    DEFAULT_VIDEO_MANAGER_PORT,
    VideoManagerClient,
    VideoManagerGateway,
)

DEFAULT_CAMERA_CONFIG = os.path.join(os.path.dirname(__file__), "video_sources.local.yaml")
DEFAULT_BRIDGE_CONFIG = os.path.join(os.path.dirname(__file__), "bridge_control.yaml")
DEFAULT_FLIPPER_JOINT_NAMES = [
    "left_front_arm_joint",
    "right_front_arm_joint",
    "left_rear_arm_joint",
    "right_rear_arm_joint",
]
DEFAULT_BASE_LINEAR_LEVELS = {1: 0.2, 2: 0.4, 3: 0.55, 4: 0.7, 5: 0.8}
DEFAULT_BASE_ANGULAR_LEVELS = {1: 0.4, 2: 0.8, 3: 1.0, 4: 1.25, 5: 1.5}
DEFAULT_ARM_LINEAR_LEVELS = {1: 0.04, 2: 0.08, 3: 0.10, 4: 0.125, 5: 0.15}
DEFAULT_ARM_ANGULAR_LEVELS = {1: 0.2, 2: 0.4, 3: 0.55, 4: 0.7, 5: 0.8}
DEFAULT_GRIPPER_RATE_LEVELS = {1: 0.015, 2: 0.03, 3: 0.04, 4: 0.05, 5: 0.06}
DEFAULT_FLIPPER_VELOCITY_LEVELS = {1: 0.2, 2: 0.4, 3: 0.55, 4: 0.7, 5: 0.8}
LEVEL_CONFIG_KEYS = {
    "base-linear": "base_linear_levels",
    "base-angular": "base_angular_levels",
    "arm-linear": "arm_linear_levels",
    "arm-angular": "arm_angular_levels",
    "gripper-rate": "gripper_rate_levels",
    "flipper-velocity": "flipper_velocity_levels",
}


def add_level_args(
    parser: argparse.ArgumentParser,
    prefix: str,
    help_name: str,
    defaults: Dict[int, float],
) -> None:
    for level in range(1, 6):
        parser.add_argument(
            f"--{prefix}-level-{level}",
            type=float,
            default=float(defaults[level]),
            help=f"{help_name} speed/rate for level {level}",
        )


def collect_level_args(
    args: argparse.Namespace,
    parser: argparse.ArgumentParser,
    prefix: str,
    config: Optional[Dict[str, Any]] = None,
) -> Optional[Dict[int, float]]:
    values: Dict[int, float] = {}
    attr_prefix = prefix.replace("-", "_")
    config_levels = {}
    if config:
        config_key = LEVEL_CONFIG_KEYS.get(prefix, "")
        raw_config_levels = config.get(config_key, {})
        if raw_config_levels is not None:
            if not isinstance(raw_config_levels, dict):
                raise ValueError(f"{config_key} must be a mapping of speed levels")
            config_levels = {
                int(level): float(value)
                for level, value in raw_config_levels.items()
            }
    for level in range(1, 6):
        attr_name = f"{attr_prefix}_level_{level}"
        cli_value = getattr(args, attr_name, None)
        parser_default = parser.get_default(attr_name)
        config_value = config_levels.get(level)
        if cli_value is None:
            if config_value is not None:
                values[level] = config_value
            continue
        if config_value is not None and cli_value == parser_default:
            values[level] = config_value
            continue
        values[level] = float(cli_value)
    return values or None


def parse_csv_list(value: str) -> List[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def parse_float_csv_list(value: str) -> List[float]:
    return [float(item.strip()) for item in value.split(",") if item.strip()]


def load_bridge_control_config(path: str) -> Dict[str, Any]:
    try:
        import yaml
    except ImportError as exc:
        raise RuntimeError("PyYAML is required to load bridge control config") from exc

    with open(path, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f) or {}
    if not isinstance(data, dict):
        raise ValueError("bridge control config root must be a mapping")
    node_config = data.get("host_bridge_node", data)
    if not isinstance(node_config, dict):
        raise ValueError("bridge control config host_bridge_node must be a mapping")
    return node_config


def resolve_config_value(
    args: argparse.Namespace,
    parser: argparse.ArgumentParser,
    attr: str,
    config: Dict[str, Any],
    default: Any,
) -> Any:
    value = getattr(args, attr)
    parser_default = parser.get_default(attr)
    if value is None:
        return config.get(attr, default)
    if attr in config and value == parser_default:
        return config[attr]
    return value


def resolve_float_config(
    args: argparse.Namespace,
    parser: argparse.ArgumentParser,
    attr: str,
    config: Dict[str, Any],
    default: float,
) -> float:
    return float(resolve_config_value(args, parser, attr, config, default))


def resolve_int_config(
    args: argparse.Namespace,
    parser: argparse.ArgumentParser,
    attr: str,
    config: Dict[str, Any],
    default: int,
) -> int:
    return int(resolve_config_value(args, parser, attr, config, default))


def resolve_str_config(
    args: argparse.Namespace,
    parser: argparse.ArgumentParser,
    attr: str,
    config: Dict[str, Any],
    default: str,
) -> str:
    return str(resolve_config_value(args, parser, attr, config, default))


def resolve_flipper_direction_corrections(
    args: argparse.Namespace,
    parser: argparse.ArgumentParser,
    config: Dict[str, Any],
    joint_names: List[str],
) -> Optional[List[float]]:
    raw_value = resolve_config_value(
        args,
        parser,
        "flipper_direction_corrections",
        config,
        None,
    )
    if raw_value in (None, ""):
        return None
    if isinstance(raw_value, str):
        return parse_float_csv_list(raw_value)
    if isinstance(raw_value, list):
        return [float(value) for value in raw_value]
    if isinstance(raw_value, dict):
        return [float(raw_value.get(name, 1.0)) for name in joint_names]
    raise ValueError("flipper_direction_corrections must be a comma-separated string, list, or mapping")


class HostBridgeServer:
    def __init__(
        self,
        host: str,
        port: int,
        output: OutputAdapter,
        watchdog_ms: int = DEFAULT_WATCHDOG_MS,
        linear_speed: float = 0.8,
        angular_speed: float = 1.5,
        servo_frame: str = "catch_camera",
        gripper_min_position: float = 0.0,
        gripper_max_position: float = 0.044,
        gripper_initial_position: float = 0.022,
        default_speed_level: int = 2,
        base_linear_levels: Optional[Dict[int, float]] = None,
        base_angular_levels: Optional[Dict[int, float]] = None,
        arm_linear_levels: Optional[Dict[int, float]] = None,
        arm_angular_levels: Optional[Dict[int, float]] = None,
        gripper_rate_levels: Optional[Dict[int, float]] = None,
        flipper_velocity_levels: Optional[Dict[int, float]] = None,
        flipper_joint_names: Optional[List[str]] = None,
        flipper_direction_corrections: Optional[List[float]] = None,
        flipper_jog_duration: float = 0.15,
        flipper_target_profile: str = "csv_velocity",
        flipper_profile_retry_sec: float = 2.0,
        gamepad_deadzone_percent: float = 4.0,
        cameras: Optional[List[Dict[str, Any]]] = None,
        video_gateway: Optional[VideoManagerGateway] = None,
        video_poll_sec: float = 1.0,
        joint_runtime_topic: str = "",
        events: Optional[EventSink] = None,
    ) -> None:
        self.host = host
        self.port = port
        self.stop_event = threading.Event()
        self.client_lock = threading.Lock()
        self.active_clients: List["BridgeClient"] = []
        self.events = events or RingBufferEventSink()
        self.video_gateway = video_gateway
        self.video_poll_sec = max(video_poll_sec, 0.1)
        self.joint_runtime_topic = joint_runtime_topic
        self.joint_runtime_subscriber = None
        self.core = BridgeCore(
            output,
            watchdog_ms,
            linear_speed,
            angular_speed,
            servo_frame,
            gripper_min_position,
            gripper_max_position,
            gripper_initial_position,
            default_speed_level,
            base_linear_levels,
            base_angular_levels,
            arm_linear_levels,
            arm_angular_levels,
            gripper_rate_levels,
            flipper_velocity_levels,
            flipper_joint_names,
            flipper_direction_corrections,
            flipper_jog_duration,
            flipper_target_profile,
            flipper_profile_retry_sec,
            gamepad_deadzone_percent,
            cameras,
            self.video_gateway.camera_infos if self.video_gateway else None,
            self.handle_camera_stream_request if self.video_gateway else None,
            self.events,
        )
        self.runtime = BridgeRuntime(self.core, self.broadcast)

    def serve_forever(self) -> None:
        self.start_video_manager()
        self.start_joint_runtime_forwarder()
        self.runtime.start_watchdog()
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
                server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                server.bind((self.host, self.port))
                server.listen(5)
                server.settimeout(0.5)
                print(f"[bridge] listening on {self.host}:{self.port}", flush=True)
                self.events.emit("tcp", "bridge listening", data={"host": self.host, "port": self.port})
                while not self.stop_event.is_set():
                    try:
                        conn, addr = server.accept()
                    except socket.timeout:
                        continue
                    except OSError:
                        break
                    client = BridgeClient(self, conn, addr)
                    with self.client_lock:
                        self.active_clients.append(client)
                    threading.Thread(target=client.run, daemon=True).start()
        finally:
            self.stop_video_manager()

    def start_video_manager(self) -> None:
        if not self.video_gateway:
            return
        changed = self.video_gateway.refresh()
        if self.video_gateway.last_error():
            self.events.emit("video", "external video manager unavailable", level="warning", data={
                "error": self.video_gateway.last_error(),
            })
        else:
            self.events.emit("video", "external video manager connected", data={
                "initial_changed": len(changed),
            })
        threading.Thread(target=self._video_monitor_loop, daemon=True).start()

    def stop_video_manager(self) -> None:
        if not self.video_gateway:
            return
        self.events.emit("video", "external video manager detached")

    def start_joint_runtime_forwarder(self) -> None:
        topic = self.joint_runtime_topic.strip()
        if not topic:
            return
        try:
            import rospy
            from Eyou_ROS1_Master.msg import JointRuntimeStateArray
        except Exception as exc:
            self.events.emit("joint_runtime", "joint runtime forwarder unavailable",
                             level="warning", data={"error": str(exc)})
            return

        def callback(msg: Any) -> None:
            states = []
            for item in getattr(msg, "states", []):
                states.append({
                    "joint_name": str(getattr(item, "joint_name", "")),
                    "backend": str(getattr(item, "backend", "")),
                    "lifecycle_state": str(getattr(item, "lifecycle_state", "")),
                    "online": bool(getattr(item, "online", False)),
                    "enabled": bool(getattr(item, "enabled", False)),
                    "fault": bool(getattr(item, "fault", False)),
                })
            self.broadcast({
                "type": "joint_runtime_states",
                "protocol_version": PROTOCOL_VERSION,
                "seq": 0,
                "timestamp_ms": now_ms(),
                "states": states,
            })

        self.joint_runtime_subscriber = rospy.Subscriber(
            topic, JointRuntimeStateArray, callback, queue_size=1
        )
        self.events.emit("joint_runtime", "joint runtime forwarder started", data={"topic": topic})

    def _video_monitor_loop(self) -> None:
        if not self.video_gateway:
            return
        while not self.stop_event.wait(self.video_poll_sec):
            for camera in self.video_gateway.refresh():
                self.broadcast(self.core.make_camera_info(camera))

    def handle_camera_stream_request(self, params: Dict[str, Any]) -> Tuple[bool, int, str, Optional[Dict[str, Any]]]:
        if not self.video_gateway:
            return False, 2400, "video manager unavailable", None
        ok, code, message, camera = self.video_gateway.stream_request(params)
        self.events.emit("video", "camera stream request handled", data={
            "camera_id": params.get("camera_id"),
            "action": params.get("action"),
            "online": camera.get("online", False) if camera else False,
        })
        return ok, code, message, camera

    def remove_client(self, client: "BridgeClient") -> None:
        with self.client_lock:
            if client in self.active_clients:
                self.active_clients.remove(client)
        self.core.publish_zero("client_disconnected")

    def broadcast(self, payload: Dict[str, Any]) -> None:
        with self.client_lock:
            clients = list(self.active_clients)
        for client in clients:
            client.send_json(payload)


class BridgeClient:
    def __init__(self, server: HostBridgeServer, conn: socket.socket, addr: Tuple[str, int]) -> None:
        self.server = server
        self.conn = conn
        self.addr = addr
        self.send_lock = threading.Lock()
        self.closed = False

    def run(self) -> None:
        print(f"[bridge] client connected: {self.addr}", flush=True)
        self.server.events.emit("tcp", "client connected", data={"addr": f"{self.addr[0]}:{self.addr[1]}"})
        self.server.core.reset_operator_input_session("client_connected")
        self.conn.settimeout(1.0)
        try:
            self.send_json(self.server.core.make_hello())
            self.send_json(self.server.core.make_capabilities())
            self._recv_loop()
        finally:
            self.closed = True
            try:
                self.conn.close()
            except OSError:
                pass
            self.server.remove_client(self)
            self.server.events.emit("tcp", "client disconnected", data={"addr": f"{self.addr[0]}:{self.addr[1]}"})
            print(f"[bridge] client disconnected: {self.addr}", flush=True)

    def send_json(self, payload: Dict[str, Any]) -> None:
        if self.closed:
            return
        try:
            with self.send_lock:
                self.conn.sendall(json_line(payload))
        except OSError:
            self.closed = True

    def _recv_loop(self) -> None:
        buffer = b""
        while not self.server.stop_event.is_set() and not self.closed:
            try:
                chunk = self.conn.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break

            if not chunk:
                break

            buffer += chunk
            if len(buffer) > MAX_FRAME_BYTES and b"\n" not in buffer:
                self.server.events.emit("protocol", "frame exceeds max length", level="error")
                self.send_json(self.server.core.make_protocol_error({}, 2001, "TCP frame exceeds max length"))
                break

            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                if not line.strip():
                    continue
                if len(line) > MAX_FRAME_BYTES:
                    self.server.events.emit("protocol", "line exceeds max length", level="error")
                    self.send_json(self.server.core.make_protocol_error({}, 2001, "TCP frame exceeds max length"))
                    return
                try:
                    msg = json.loads(line.decode("utf-8"))
                    if not isinstance(msg, dict):
                        raise ValueError("JSON frame is not an object")
                except Exception as exc:
                    self.server.events.emit("protocol", "invalid JSON", level="error", data={"error": str(exc)})
                    self.send_json(self.server.core.make_protocol_error({}, 2100, f"invalid JSON: {exc}"))
                    continue

                for response in self.server.core.handle_message(msg):
                    self.send_json(response)


def parse_camera(value: str) -> Dict[str, Any]:
    parts = value.split(",", 2)
    if len(parts) != 3:
        raise argparse.ArgumentTypeError("camera format: id,name,rtsp_url")
    camera_id = int(parts[0])
    name = parts[1]
    rtsp_url = parts[2]
    return {
        "camera_id": camera_id,
        "name": name,
        "online": bool(rtsp_url),
        "rtsp_url": rtsp_url,
        "codec": "h264",
        "width": 1280,
        "height": 720,
        "fps": 25,
        "bitrate_kbps": 2500 if rtsp_url else 0,
    }


def parse_service_command(value: str) -> Tuple[str, str]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("service command must be command=/service/name")
    command, service = value.split("=", 1)
    command = command.strip()
    service = service.strip()
    if not command or not service:
        raise argparse.ArgumentTypeError("service command and service name must be non-empty")
    return command, service


def load_cameras_from_yaml(path: str, publish_host_override: str = "") -> List[Dict[str, Any]]:
    try:
        import yaml
    except ImportError as exc:
        raise RuntimeError("PyYAML is required for --camera-config") from exc

    with open(path, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f) or {}

    rtsp = data.get("rtsp", {}) if isinstance(data.get("rtsp", {}), dict) else {}
    publish_host = (
        publish_host_override
        or rtsp.get("public_host")
        or rtsp.get("host")
        or rtsp.get("publish_host")
        or "127.0.0.1"
    )
    port = int(rtsp.get("port", 8554))

    sources = data.get("direct_sources", [])
    if not isinstance(sources, list):
        raise ValueError("camera config field direct_sources must be a list")

    cameras: List[Dict[str, Any]] = []
    for source in sources:
        if not isinstance(source, dict):
            continue
        if not bool(source.get("enabled", True)):
            continue

        rtsp_path = str(source.get("rtsp_path") or source.get("source_id") or "").strip("/")
        if not rtsp_path:
            raise ValueError("enabled camera source is missing rtsp_path/source_id")

        camera_id = int(source.get("camera_id", len(cameras)))
        name = str(source.get("name") or source.get("source_id") or rtsp_path)
        codec = str(source.get("codec", "h264"))
        width = int(source.get("width", 1280))
        height = int(source.get("height", 720))
        fps = int(source.get("fps", 25))
        bitrate_kbps = int(source.get("bitrate_kbps", 0))

        cameras.append({
            "camera_id": camera_id,
            "name": name,
            "online": True,
            "rtsp_url": f"rtsp://{publish_host}:{port}/{rtsp_path}",
            "codec": codec,
            "width": width,
            "height": height,
            "fps": fps,
            "bitrate_kbps": bitrate_kbps,
        })

    return cameras


def detect_publish_host() -> str:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("8.8.8.8", 80))
            return sock.getsockname()[0]
    except OSError:
        return "127.0.0.1"


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="ROS1 host bridge TCP/JSON node")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=9090)
    parser.add_argument(
        "--bridge-control-config",
        default=DEFAULT_BRIDGE_CONFIG,
        help="YAML config for host_bridge_node control translation",
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--dry-run", action="store_true", help="run without ROS and print bridge outputs")
    mode.add_argument("--ros", action="store_true", help="publish ROS control topics with rospy; default mode")
    parser.add_argument("--cmd-vel-topic", default="/cmd_vel")
    parser.add_argument("--servo-topic", default="/servo_server/delta_twist_cmds")
    parser.add_argument("--servo-frame", default="catch_camera")
    parser.add_argument("--gripper-position-topic", default="/arm_control/gripper_position")
    parser.add_argument(
        "--moveit-group",
        default=None,
        help="MoveIt MoveGroup name used for named arm targets; overrides bridge_control.yaml",
    )
    parser.add_argument("--flipper-jog-topic", default="/flipper_control/jog_cmd")
    parser.add_argument("--flipper-profile-service", default="/flipper_control/set_control_profile")
    parser.add_argument(
        "--hybrid-service-ns",
        default="/hybrid_motor_hw_node",
        help="ROS service namespace for lifecycle and joint mode commands",
    )
    parser.add_argument(
        "--service-command",
        action="append",
        type=parse_service_command,
        default=[],
        metavar="COMMAND=SERVICE",
        help=(
            "map a TCP command to a std_srvs/Trigger service; may be repeated. "
            "SERVICE may use {command}, {mode}, {source}, e.g. "
            "set_control_mode=/robot/set_{mode}_mode"
        ),
    )
    parser.add_argument("--gripper-min-position", type=float, default=0.0)
    parser.add_argument("--gripper-max-position", type=float, default=0.044)
    parser.add_argument("--gripper-initial-position", type=float, default=0.022)
    parser.add_argument("--default-speed-level", type=int, default=2)
    parser.add_argument("--flipper-target-profile", default="csv_velocity")
    parser.add_argument("--flipper-profile-retry-sec", type=float, default=2.0)
    parser.add_argument("--flipper-jog-duration", type=float, default=0.15)
    parser.add_argument(
        "--gamepad-deadzone-percent",
        type=float,
        default=None,
        help="ignore gamepad axes with absolute value below this percentage",
    )
    parser.add_argument(
        "--flipper-joint-names",
        default=",".join(DEFAULT_FLIPPER_JOINT_NAMES),
        help="comma-separated flipper joint names in command order",
    )
    parser.add_argument(
        "--flipper-direction-corrections",
        default=None,
        help="comma-separated flipper direction corrections aligned with --flipper-joint-names",
    )
    parser.add_argument("--watchdog-ms", type=int, default=DEFAULT_WATCHDOG_MS)
    parser.add_argument("--linear-speed", type=float, default=0.8, help="vehicle level-5 linear speed")
    parser.add_argument("--angular-speed", type=float, default=1.5, help="vehicle level-5 angular speed")
    add_level_args(parser, "base-linear", "base linear", DEFAULT_BASE_LINEAR_LEVELS)
    add_level_args(parser, "base-angular", "base angular", DEFAULT_BASE_ANGULAR_LEVELS)
    add_level_args(parser, "arm-linear", "arm linear", DEFAULT_ARM_LINEAR_LEVELS)
    add_level_args(parser, "arm-angular", "arm angular", DEFAULT_ARM_ANGULAR_LEVELS)
    add_level_args(parser, "gripper-rate", "gripper", DEFAULT_GRIPPER_RATE_LEVELS)
    add_level_args(parser, "flipper-velocity", "flipper", DEFAULT_FLIPPER_VELOCITY_LEVELS)
    parser.add_argument("--debug-ui", dest="debug_ui", action="store_true", default=True,
                        help="start read-only debug HTTP UI; enabled by default")
    parser.add_argument("--no-debug-ui", dest="debug_ui", action="store_false",
                        help="disable debug HTTP UI")
    parser.add_argument("--debug-host", default="0.0.0.0")
    parser.add_argument("--debug-port", type=int, default=18080)
    parser.add_argument(
        "--camera-config",
        default=DEFAULT_CAMERA_CONFIG,
        help="load camera list from a video_sources YAML file",
    )
    parser.add_argument(
        "--no-camera-config",
        action="store_true",
        help="disable static camera config loading; useful when --video-manager owns cameras",
    )
    parser.add_argument(
        "--camera-publish-host",
        default="",
        help="override rtsp.publish_host when loading --camera-config",
    )
    parser.add_argument(
        "--camera",
        action="append",
        type=parse_camera,
        help="camera entry as id,name,rtsp_url; may be repeated",
    )
    parser.add_argument(
        "--video-manager",
        action="store_true",
        help="use the standalone video_manager_node for camera list and stream lifecycle",
    )
    parser.add_argument("--video-manager-host", default=DEFAULT_VIDEO_MANAGER_HOST)
    parser.add_argument("--video-manager-port", type=int, default=DEFAULT_VIDEO_MANAGER_PORT)
    parser.add_argument("--video-manager-timeout", type=float, default=1.0)
    parser.add_argument(
        "--video-config",
        default="",
        help="deprecated: start ros1_bridge/video_manager_node.py with --config instead",
    )
    parser.add_argument(
        "--video-dry-run",
        action="store_true",
        help="deprecated: pass --dry-run to video_manager_node.py instead",
    )
    parser.add_argument(
        "--video-autostart",
        action="store_true",
        help="deprecated: pass --autostart to video_manager_node.py instead",
    )
    parser.add_argument("--video-poll-sec", type=float, default=1.0)
    parser.add_argument(
        "--joint-runtime-topic",
        default="/hybrid_motor_hw_node/joint_runtime_states",
        help="ROS topic to forward as TCP joint_runtime_states when --ros is used; empty disables it",
    )
    return parser


def parse_node_args() -> Tuple[argparse.ArgumentParser, argparse.Namespace]:
    parser = build_arg_parser()
    args = parser.parse_args([
        arg for arg in sys.argv[1:] if ":=" not in arg
    ])
    return parser, args


def main() -> None:
    parser, args = parse_node_args()
    events = RingBufferEventSink()
    service_commands = dict(args.service_command or [])
    bridge_control_config = load_bridge_control_config(
        os.path.expanduser(args.bridge_control_config)
    ) if args.bridge_control_config else {}
    gamepad_deadzone_percent = resolve_float_config(
        args,
        parser,
        "gamepad_deadzone_percent",
        bridge_control_config,
        4.0,
    )
    moveit_group = resolve_str_config(
        args,
        parser,
        "moveit_group",
        bridge_control_config,
        "arm",
    )
    linear_speed = resolve_float_config(args, parser, "linear_speed", bridge_control_config, 0.8)
    angular_speed = resolve_float_config(args, parser, "angular_speed", bridge_control_config, 1.5)
    gripper_min_position = resolve_float_config(
        args, parser, "gripper_min_position", bridge_control_config, 0.0
    )
    gripper_max_position = resolve_float_config(
        args, parser, "gripper_max_position", bridge_control_config, 0.044
    )
    gripper_initial_position = resolve_float_config(
        args, parser, "gripper_initial_position", bridge_control_config, 0.022
    )
    default_speed_level = resolve_int_config(
        args, parser, "default_speed_level", bridge_control_config, 2
    )
    flipper_jog_duration = resolve_float_config(
        args, parser, "flipper_jog_duration", bridge_control_config, 0.15
    )
    flipper_profile_retry_sec = resolve_float_config(
        args, parser, "flipper_profile_retry_sec", bridge_control_config, 2.0
    )
    base_linear_levels = collect_level_args(args, parser, "base-linear", bridge_control_config)
    base_angular_levels = collect_level_args(args, parser, "base-angular", bridge_control_config)
    arm_linear_levels = collect_level_args(args, parser, "arm-linear", bridge_control_config)
    arm_angular_levels = collect_level_args(args, parser, "arm-angular", bridge_control_config)
    gripper_rate_levels = collect_level_args(args, parser, "gripper-rate", bridge_control_config)
    flipper_velocity_levels = collect_level_args(
        args, parser, "flipper-velocity", bridge_control_config
    )
    flipper_joint_names = parse_csv_list(
        resolve_str_config(
            args,
            parser,
            "flipper_joint_names",
            bridge_control_config,
            ",".join(DEFAULT_FLIPPER_JOINT_NAMES),
        )
    )
    flipper_direction_corrections = resolve_flipper_direction_corrections(
        args,
        parser,
        bridge_control_config,
        flipper_joint_names,
    )
    flipper_target_profile = resolve_str_config(
        args,
        parser,
        "flipper_target_profile",
        bridge_control_config,
        "csv_velocity",
    )

    cameras = args.camera
    if args.no_camera_config:
        cameras = cameras or []
    elif args.camera_config:
        camera_config = os.path.expanduser(args.camera_config)
        camera_publish_host = args.camera_publish_host or detect_publish_host()
        cameras = load_cameras_from_yaml(camera_config, camera_publish_host)
        if args.camera:
            cameras.extend(args.camera)

    if not args.dry_run:
        output: OutputAdapter = RosOutput(
            "host_bridge_node",
            args.cmd_vel_topic,
            args.servo_topic,
            args.gripper_position_topic,
            args.flipper_jog_topic,
            args.flipper_profile_service,
            args.hybrid_service_ns,
            service_commands,
            moveit_group,
            events,
        )
    else:
        output = DryRunOutput(events, service_commands)

    if args.video_config or args.video_dry_run or args.video_autostart:
        raise SystemExit(
            "--video-config/--video-dry-run/--video-autostart moved to "
            "ros1_bridge/video_manager_node.py; start that node separately and pass "
            "--video-manager to host_bridge_node.py"
        )

    video_gateway: Optional[VideoManagerGateway] = None
    if args.video_manager:
        video_gateway = VideoManagerGateway(
            VideoManagerClient(
                args.video_manager_host,
                args.video_manager_port,
                args.video_manager_timeout,
            ),
            events,
        )
        events.emit("video", "external video manager configured", data={
            "host": args.video_manager_host,
            "port": args.video_manager_port,
        })

    server = HostBridgeServer(
        host=args.host,
        port=args.port,
        output=output,
        watchdog_ms=args.watchdog_ms,
        linear_speed=linear_speed,
        angular_speed=angular_speed,
        servo_frame=args.servo_frame,
        gripper_min_position=gripper_min_position,
        gripper_max_position=gripper_max_position,
        gripper_initial_position=gripper_initial_position,
        default_speed_level=default_speed_level,
        base_linear_levels=base_linear_levels,
        base_angular_levels=base_angular_levels,
        arm_linear_levels=arm_linear_levels,
        arm_angular_levels=arm_angular_levels,
        gripper_rate_levels=gripper_rate_levels,
        flipper_velocity_levels=flipper_velocity_levels,
        flipper_joint_names=flipper_joint_names,
        flipper_direction_corrections=flipper_direction_corrections,
        flipper_jog_duration=flipper_jog_duration,
        flipper_target_profile=flipper_target_profile,
        flipper_profile_retry_sec=flipper_profile_retry_sec,
        gamepad_deadzone_percent=gamepad_deadzone_percent,
        cameras=cameras,
        video_gateway=video_gateway,
        video_poll_sec=args.video_poll_sec,
        joint_runtime_topic=args.joint_runtime_topic if not args.dry_run else "",
        events=events,
    )
    if args.debug_ui:
        DebugHttpServer(args.debug_host, args.debug_port, server.core, events).start()

    def handle_signal(signum: int, frame: Any) -> None:
        del signum, frame
        server.stop_event.set()
        server.runtime.stop_event.set()

    signal.signal(signal.SIGTERM, handle_signal)
    signal.signal(signal.SIGINT, handle_signal)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("[bridge] stopping", flush=True)
        server.stop_event.set()
        server.runtime.stop_event.set()


if __name__ == "__main__":
    main()
