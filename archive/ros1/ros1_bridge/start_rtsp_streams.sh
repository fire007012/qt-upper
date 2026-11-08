#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

exec "${SCRIPT_DIR}/start_rtsp_streams_from_yaml.py" \
  --config "${VIDEO_SOURCES_CONFIG:-${SCRIPT_DIR}/video_sources.local.yaml}" \
  --log-dir "${LOG_DIR:-/tmp/rtsp_logs}"
