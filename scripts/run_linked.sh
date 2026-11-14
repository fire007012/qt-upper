#!/usr/bin/env bash
set -euo pipefail
host_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
host_config="${HOSTCOMPUTER_MANAGEMENT_CONFIG:-$host_root/../ros2_ws_car/linked-runtime/manager.json}"
host_binary="${HOSTCOMPUTER_BINARY:-$host_root/build-linked/hostcomputer}"
if [[ ! -f "$host_config" || ! -x "$host_binary" ]]; then
    echo '未找到联动配置或上位机构建，请查看 docs/上下位机联动排查与启动.md' >&2
    exit 1
fi
export HOSTCOMPUTER_MANAGEMENT_CONFIG="$host_config"
exec "$host_binary" "$@"
