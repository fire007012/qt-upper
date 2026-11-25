#!/usr/bin/env bash
set -euo pipefail
host_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
default_runtime_dir="${ROBOT24_RUNTIME_DIR:-$HOME/.local/share/robot24}"
if [[ -n "${HOSTCOMPUTER_MANAGEMENT_CONFIG:-}" ]]; then
    host_config="$HOSTCOMPUTER_MANAGEMENT_CONFIG"
elif [[ -f "/etc/robot24/manager.json" ]]; then
    host_config="/etc/robot24/manager.json"
else
    host_config="$default_runtime_dir/manager.json"
fi
if [[ -n "${HOSTCOMPUTER_BINARY:-}" ]]; then
    host_binary="$HOSTCOMPUTER_BINARY"
elif [[ -x "$host_root/build/hostcomputer" ]]; then
    host_binary="$host_root/build/hostcomputer"
else
    host_binary="$host_root/build-linked/hostcomputer"
fi
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
if [[ ! -f "$host_config" ]]; then
    echo "未找到下位机管理配置：$host_config；请先启动下位机 run_manager.sh 或设置 HOSTCOMPUTER_MANAGEMENT_CONFIG。" >&2
    exit 1
fi
if [[ ! -x "$host_binary" ]]; then
    echo "未找到可执行的上位机程序：$host_binary；请设置 HOSTCOMPUTER_BINARY。" >&2
    exit 1
fi
echo '同机连接入口：读取本机管理配置；模拟选 mock，实机选 hardware。跨主机才使用 --vehicle 下位机IP。' >&2
export HOSTCOMPUTER_MANAGEMENT_CONFIG="$host_config"
exec "$host_binary" "$@"
