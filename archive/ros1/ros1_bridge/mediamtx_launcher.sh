#!/usr/bin/env bash
set -euo pipefail

MEDIAMTX_BIN="mediamtx"
MEDIAMTX_CONFIG=""
RTSP_PORT="8554"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bin)
      MEDIAMTX_BIN="${2:-mediamtx}"
      shift 2
      ;;
    --config)
      MEDIAMTX_CONFIG="${2:-}"
      shift 2
      ;;
    --port)
      RTSP_PORT="${2:-8554}"
      shift 2
      ;;
    *:=*)
      shift
      ;;
    *)
      echo "unknown mediamtx launcher argument: $1" >&2
      exit 2
      ;;
  esac
done

port_listening() {
  if command -v ss >/dev/null 2>&1; then
    ss -lnt | awk -v port=":${RTSP_PORT}" '
      NR > 1 && substr($4, length($4) - length(port) + 1) == port { found = 1 }
      END { exit found ? 0 : 1 }
    '
    return
  fi
  python3 - "$RTSP_PORT" <<'PY'
import socket
import sys
port = int(sys.argv[1])
try:
    with socket.create_connection(("127.0.0.1", port), timeout=0.2):
        pass
except OSError:
    sys.exit(1)
PY
}

if port_listening; then
  echo "[mediamtx-launcher] RTSP port ${RTSP_PORT} already listening; using external MediaMTX"
  trap 'exit 0' INT TERM
  while true; do
    sleep 3600 &
    wait "$!" || true
  done
fi

if ! command -v "$MEDIAMTX_BIN" >/dev/null 2>&1; then
  echo "[mediamtx-launcher] mediamtx executable not found: ${MEDIAMTX_BIN}" >&2
  exit 127
fi

echo "[mediamtx-launcher] starting ${MEDIAMTX_BIN}"
if [[ -n "$MEDIAMTX_CONFIG" ]]; then
  exec "$MEDIAMTX_BIN" "$MEDIAMTX_CONFIG"
fi
exec "$MEDIAMTX_BIN"
