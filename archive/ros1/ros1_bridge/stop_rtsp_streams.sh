#!/usr/bin/env bash
set -euo pipefail

pkill -f "^ffmpeg .*rtsp://127.0.0.1:8554" || true
pkill -f "^ffmpeg .*rtsp://.*:8554" || true
