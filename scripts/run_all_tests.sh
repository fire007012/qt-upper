#!/usr/bin/env bash
# Current host-side checks only. ROS2/hardware acceptance is a separate workflow.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CMAKE_BIN="${CMAKE_BIN:-cmake}"
CTEST_BIN="${CTEST_BIN:-$(dirname "$(command -v "$CMAKE_BIN")")/ctest}"
BUILD_DIR="${BUILD_DIR:-/tmp/hostcomputer-build-smoke}"
COMMUNICATION_BUILD_DIR="${COMMUNICATION_BUILD_DIR:-/tmp/hostcomputer-communication-tests}"
LOG_DIR="${LOG_DIR:-/tmp/hostcomputer-test-logs}"
BUILD_JOBS="${BUILD_JOBS:-4}"
HOST_BUILD_WIDGET_TESTS="${HOST_BUILD_WIDGET_TESTS:-OFF}"
REFERENCE_DIR="$(realpath -m "$ROOT/ros2_ws_car")"
for output_dir in "$BUILD_DIR" "$COMMUNICATION_BUILD_DIR" "$LOG_DIR"; do
    resolved_dir="$(realpath -m "$output_dir")"
    case "$resolved_dir" in
        "$REFERENCE_DIR"|"$REFERENCE_DIR"/*|"$ROOT"|"$ROOT/archive"|"$ROOT/archive"/*)
            echo "拒绝将构建、日志或缓存写入源码根目录、只读参考目录或历史归档: $output_dir" >&2
            exit 1 ;;
    esac
done
mkdir -p "$LOG_DIR"
REPORT="$LOG_DIR/run_all_$(date +%Y%m%d_%H%M%S).txt"
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$REPORT"; }

log "1) Qt application build (optional process smoke via RUN_APP_SMOKE=1)"
CMAKE_BIN="$CMAKE_BIN" BUILD_DIR="$BUILD_DIR" LOG_DIR="$LOG_DIR" BUILD_JOBS="$BUILD_JOBS" \
    bash "$ROOT/scripts/smoke_test.sh" | tee -a "$REPORT"

log "2) Independent RobotTcpClient / management regressions"
cmake_args=(-S "$ROOT/tests/communication" -B "$COMMUNICATION_BUILD_DIR"
    -DCMAKE_BUILD_TYPE=Debug -DHOST_BUILD_WIDGET_TESTS="$HOST_BUILD_WIDGET_TESTS")
if [[ -n "${CMAKE_PREFIX_PATH:-}" ]]; then
    cmake_args+=(-DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH")
fi
"$CMAKE_BIN" "${cmake_args[@]}" | tee -a "$REPORT"
"$CMAKE_BIN" --build "$COMMUNICATION_BUILD_DIR" -j "$BUILD_JOBS" | tee -a "$REPORT"
"$CTEST_BIN" --test-dir "$COMMUNICATION_BUILD_DIR" --output-on-failure -j 3 | tee -a "$REPORT"

log "3) Independent TCP/JSON mock protocol smoke"
python3 -B "$ROOT/scripts/protocol_smoke_test.py" | tee -a "$REPORT"
log "PASS: host build and mock tests completed; ROS2/hardware acceptance remains separate. report=$REPORT"
