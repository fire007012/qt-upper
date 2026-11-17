#!/usr/bin/env bash
set -euo pipefail
host_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
host_config="${HOSTCOMPUTER_MANAGEMENT_CONFIG:-$host_root/../ros2_ws_car/linked-runtime/manager.json}"
host_binary="${HOSTCOMPUTER_BINARY:-$host_root/build-linked/hostcomputer}"
if [[ "${1:-}" == "--vehicle" ]]; then
    if [[ -z "${2:-}" || "$2" == -* ]]; then
        echo '用法：run_linked.sh --vehicle 下位机IP；用于跨主机连接，运行环境在界面选择。' >&2
        exit 2
    fi
    export HOSTCOMPUTER_MANAGEMENT_HOST="$2"
    unset HOSTCOMPUTER_MANAGEMENT_CONFIG
    shift 2
    if [[ ! -x "$host_binary" ]]; then
        echo '未找到上位机构建，请设置 HOSTCOMPUTER_BINARY。' >&2
        exit 1
    fi
    echo '跨主机连接入口：请核对管理端口和令牌，再选择 mock 或 hardware 档案。' >&2
    exec "$host_binary" "$@"
fi
if [[ ! -f "$host_config" || ! -x "$host_binary" ]]; then
    echo '未找到联动配置或上位机构建，请查看 docs/上下位机启动与控制说明.md' >&2
    exit 1
fi
echo '同机连接入口：读取本机管理配置；模拟选 mock，实机选 hardware。跨主机才使用 --vehicle 下位机IP。' >&2
export HOSTCOMPUTER_MANAGEMENT_CONFIG="$host_config"
exec "$host_binary" "$@"
