#!/usr/bin/env bash
set -euo pipefail

COMMAND="${1:-status}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PID_DIR="${PID_DIR:-/tmp/robot_video_stack}"
LOG_DIR="${LOG_DIR:-/tmp/rtsp_logs}"
VIDEO_SOURCES_CONFIG="${VIDEO_SOURCES_CONFIG:-${SCRIPT_DIR}/video_sources.local.yaml}"
VIDEO_MANAGER_HOST="${VIDEO_MANAGER_HOST:-127.0.0.1}"
VIDEO_MANAGER_PORT="${VIDEO_MANAGER_PORT:-18081}"
BRIDGE_HOST="${BRIDGE_HOST:-0.0.0.0}"
BRIDGE_PORT="${BRIDGE_PORT:-9090}"
DEBUG_HOST="${DEBUG_HOST:-0.0.0.0}"
DEBUG_PORT="${DEBUG_PORT:-18080}"
RTSP_PORT="${RTSP_PORT:-8554}"
BRIDGE_MODE="${BRIDGE_MODE:---ros}"

port_listening() {
  local host="$1"
  local port="$2"
  if command -v ss >/dev/null 2>&1; then
    ss -lnt | awk -v port=":${port}" '
      NR > 1 && substr($4, length($4) - length(port) + 1) == port { found = 1 }
      END { exit found ? 0 : 1 }
    '
    return
  fi
  python3 - "$host" "$port" <<'PY'
import socket
import sys
host = sys.argv[1]
port = int(sys.argv[2])
try:
    with socket.create_connection((host, port), timeout=0.2):
        pass
except OSError:
    sys.exit(1)
PY
}

wait_for_port() {
  local name="$1"
  local host="$2"
  local port="$3"
  local timeout="${4:-8}"
  local deadline=$((SECONDS + timeout))
  until port_listening "$host" "$port"; do
    if (( SECONDS >= deadline )); then
      echo "ERROR: ${name} did not open ${host}:${port}" >&2
      return 1
    fi
    sleep 0.2
  done
}

pid_alive() {
  local pid_file="$1"
  [[ -s "$pid_file" ]] && kill -0 "$(cat "$pid_file")" 2>/dev/null
}

start_process() {
  local name="$1"
  local pid_file="$PID_DIR/${name}.pid"
  local log_file="$LOG_DIR/${name}.log"
  shift
  if pid_alive "$pid_file"; then
    echo "${name} already running pid=$(cat "$pid_file")"
    return 0
  fi
  rm -f "$pid_file"
  nohup "$@" >>"$log_file" 2>&1 &
  local pid=$!
  echo "$pid" >"$pid_file"
  echo "started ${name}: pid=${pid} log=${log_file}"
}

stop_process() {
  local name="$1"
  local pid_file="$PID_DIR/${name}.pid"
  if [[ ! -s "$pid_file" ]]; then
    echo "${name}: no pid file"
    return 0
  fi

  local pid
  pid="$(cat "$pid_file")"
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "${name}: pid ${pid} is not running"
    rm -f "$pid_file"
    return 0
  fi

  echo "stopping ${name}: pid=${pid}"
  kill "$pid" 2>/dev/null || true
  for _ in $(seq 1 30); do
    if ! kill -0 "$pid" 2>/dev/null; then
      rm -f "$pid_file"
      echo "${name}: stopped"
      return 0
    fi
    sleep 0.2
  done

  echo "${name}: forcing stop"
  kill -9 "$pid" 2>/dev/null || true
  rm -f "$pid_file"
}

ensure_mediamtx() {
  if [[ "${SKIP_MEDIAMTX_START:-0}" != "1" ]] && command -v systemctl >/dev/null 2>&1; then
    if systemctl list-unit-files mediamtx.service >/dev/null 2>&1; then
      systemctl start mediamtx >/dev/null 2>&1 || true
    fi
  fi
  wait_for_port "MediaMTX" "127.0.0.1" "$RTSP_PORT" "${MEDIAMTX_WAIT_SEC:-8}" || {
    echo "MediaMTX is required before starting video streams." >&2
    echo "Recent MediaMTX logs:" >&2
    journalctl -u mediamtx --since '5 minutes ago' --no-pager 2>/dev/null | tail -80 >&2 || true
    exit 1
  }
}

ensure_ros_master() {
  if [[ "$BRIDGE_MODE" == "--dry-run" || "${SKIP_ROS_MASTER_CHECK:-0}" == "1" ]]; then
    return 0
  fi
  python3 - <<'PY'
import os
import socket
import sys
from urllib.parse import urlparse

uri = os.environ.get("ROS_MASTER_URI", "http://localhost:11311")
parsed = urlparse(uri)
host = parsed.hostname or "localhost"
port = parsed.port or 11311
try:
    with socket.create_connection((host, port), timeout=0.5):
        pass
except OSError as exc:
    print(f"ERROR: ROS master is not reachable at {uri}: {exc}", file=sys.stderr)
    print("Start roscore or set ROS_SETUP/ROS_MASTER_URI before video_stack.sh start.", file=sys.stderr)
    sys.exit(1)
PY
}

show_pid() {
  local name="$1"
  local pid_file="$PID_DIR/${name}.pid"
  if pid_alive "$pid_file"; then
    echo "${name}: running pid=$(cat "$pid_file")"
  else
    echo "${name}: stopped"
  fi
}

show_port() {
  local name="$1"
  local host="$2"
  local port="$3"
  if port_listening "$host" "$port"; then
    echo "${name}: listening on ${host}:${port}"
  else
    echo "${name}: not listening on ${host}:${port}"
  fi
}

start_stack() {
  mkdir -p "$PID_DIR" "$LOG_DIR"

  if [[ -n "${ROS_SETUP:-}" && -f "${ROS_SETUP}" ]]; then
    # shellcheck disable=SC1090
    source "${ROS_SETUP}"
  fi

  ensure_mediamtx

  video_manager_cmd=(
    python3 "$SCRIPT_DIR/video_manager_node.py"
    --host "$VIDEO_MANAGER_HOST"
    --port "$VIDEO_MANAGER_PORT"
    --config "$VIDEO_SOURCES_CONFIG"
    --rtsp-port "$RTSP_PORT"
    --log-dir "$LOG_DIR"
    --autostart
  )
  if [[ -n "${RTSP_PUBLIC_HOST:-}" ]]; then
    video_manager_cmd+=(--rtsp-public-host "$RTSP_PUBLIC_HOST")
  fi
  if [[ -n "${RTSP_PUBLISH_HOST:-}" ]]; then
    video_manager_cmd+=(--rtsp-publish-host "$RTSP_PUBLISH_HOST")
  fi
  if [[ "${VIDEO_MANAGER_DRY_RUN:-0}" == "1" ]]; then
    video_manager_cmd+=(--dry-run)
  fi

  start_process video_manager_node "${video_manager_cmd[@]}"
  wait_for_port "video_manager_node" "$VIDEO_MANAGER_HOST" "$VIDEO_MANAGER_PORT" "${VIDEO_MANAGER_WAIT_SEC:-8}"

  ensure_ros_master

  start_process host_bridge_node \
    python3 "$SCRIPT_DIR/host_bridge_node.py" \
      "$BRIDGE_MODE" \
      --host "$BRIDGE_HOST" \
      --port "$BRIDGE_PORT" \
      --no-camera-config \
      --video-manager \
      --video-manager-host "$VIDEO_MANAGER_HOST" \
      --video-manager-port "$VIDEO_MANAGER_PORT" \
      --video-manager-timeout "${VIDEO_MANAGER_TIMEOUT:-5.0}" \
      --debug-ui \
      --debug-host "$DEBUG_HOST" \
      --debug-port "$DEBUG_PORT"

  wait_for_port "host_bridge_node" "127.0.0.1" "$BRIDGE_PORT" "${BRIDGE_WAIT_SEC:-8}"
  status_stack
}

stop_stack() {
  stop_process host_bridge_node
  stop_process video_manager_node

  if [[ "${STOP_MEDIAMTX:-0}" == "1" ]] && command -v systemctl >/dev/null 2>&1; then
    systemctl stop mediamtx >/dev/null 2>&1 || true
  fi
}

status_stack() {
  echo "== processes =="
  show_pid video_manager_node
  show_pid host_bridge_node

  echo
  echo "== ports =="
  show_port MediaMTX 127.0.0.1 "$RTSP_PORT"
  show_port video_manager_node "$VIDEO_MANAGER_HOST" "$VIDEO_MANAGER_PORT"
  show_port host_bridge_node 127.0.0.1 "$BRIDGE_PORT"
  show_port host_bridge_debug 127.0.0.1 "$DEBUG_PORT"

  echo
  echo "== video diagnostics =="
  PYTHONPATH="$SCRIPT_DIR:${PYTHONPATH:-}" python3 - "$VIDEO_MANAGER_HOST" "$VIDEO_MANAGER_PORT" <<'PY'
import sys
from video_manager_ipc import VideoManagerClient, VideoManagerClientError

host = sys.argv[1]
port = int(sys.argv[2])
try:
    diag = VideoManagerClient(host, port, timeout=10.0).diagnostics()
except VideoManagerClientError as exc:
    print(f"video manager unavailable: {exc}")
    sys.exit(0)

summary = diag.get("summary", {})
rtsp = diag.get("rtsp", {})
print(
    "summary: "
    f"online={summary.get('online', 0)}/"
    f"{summary.get('total', 0)} "
    f"desired={summary.get('desired_online', 0)} "
    f"errors={summary.get('errors', 0)} "
    f"mediamtx_listening={diag.get('mediamtx_listening')}"
)
print(
    "rtsp: "
    f"public={rtsp.get('public_host')} "
    f"publish={rtsp.get('publish_host')}:{rtsp.get('port')} "
    f"transport={rtsp.get('transport')}"
)
for camera in diag.get("cameras", []):
    err = camera.get("last_error") or ""
    if len(err) > 180:
        err = err[:177] + "..."
    print(
        "camera "
        f"{camera.get('camera_id')} {camera.get('source_id')} "
        f"online={camera.get('online')} "
        f"desired={camera.get('desired_online')} "
        f"pid={camera.get('pid', 0)} "
        f"log={camera.get('log_path', '')} "
        f"error={err}"
    )
PY

  echo
  echo "== ffmpeg publishers =="
  pgrep -af '^ffmpeg .*rtsp' || true

  echo
  echo "== recent node logs =="
  for log in "$LOG_DIR/video_manager_node.log" "$LOG_DIR/host_bridge_node.log"; do
    if [[ -f "$log" ]]; then
      echo "--- $log ---"
      tail -40 "$log"
    fi
  done
}

case "$COMMAND" in
  start)
    start_stack
    ;;
  stop)
    stop_stack
    ;;
  restart)
    stop_stack
    start_stack
    ;;
  status)
    status_stack
    ;;
  *)
    echo "usage: $0 {start|stop|restart|status}" >&2
    exit 2
    ;;
esac
