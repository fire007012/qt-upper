#!/usr/bin/env python3
"""Read-only connection card for a manager running on this computer."""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import subprocess
import sys


def command(args):
    try:
        result = subprocess.run(args, capture_output=True, text=True, timeout=3,
                                check=False)
        return result.stdout if result.returncode == 0 else None
    except (OSError, subprocess.SubprocessError):
        return None


def lan_addresses():
    output = command(["ip", "-j", "-4", "addr", "show", "up"])
    if output is None:
        return []
    try:
        interfaces = json.loads(output)
        addresses = []
        for interface in interfaces:
            for info in interface.get("addr_info", []):
                address = ipaddress.ip_address(info.get("local", ""))
                if address.version == 4 and not (address.is_loopback or address.is_link_local
                                                or address.is_unspecified):
                    item = (interface["ifname"], str(address))
                    if item not in addresses:
                        addresses.append(item)
        return addresses
    except (ValueError, KeyError, TypeError, AttributeError):
        return []


def listeners(port):
    output = command(["ss", "-H", "-ltn"])
    if output is None:
        return None
    result = []
    for line in output.splitlines():
        fields = line.split()
        if len(fields) >= 4 and fields[3].rsplit(":", 1)[-1] == str(port):
            result.append(fields[3])
    return result


def connection_card(config_path, hide_token=False, local=False):
    # Never dump the configuration: it may contain credentials or private paths.
    try:
        config_path = config_path.resolve(strict=True)
        if config_path.stat().st_size > 65536:
            raise ValueError
        config = json.loads(config_path.read_text(encoding="utf-8"))
        if not isinstance(config, dict):
            raise ValueError
        port = config.get("port", 9091)
        bind = config.get("bind", "127.0.0.1")
        token_path = config_path.parent / config["token_file"]
        if (not isinstance(bind, str) or not bind.strip()
                or type(port) is not int or not 1 <= port <= 65535):
            raise ValueError
        if token_path.stat().st_size > 4096:
            raise ValueError
        token = token_path.read_text(encoding="utf-8").strip()
        if len(token) < 32 or any(not char.isprintable() for char in token):
            raise ValueError
    except (OSError, ValueError, TypeError, KeyError):
        print("读取失败：请检查管理配置的 bind、port、token_file，以及令牌文件的读取权限。",
              file=sys.stderr)
        print("令牌至少 32 字符；相对 token_file 按管理配置文件所在目录解析，与终端当前目录无关。",
              file=sys.stderr)
        return 1

    print("上位机连接设置（请在下位机运行本脚本）")
    print(f"配置文件：{config_path}")
    print(f"管理端口：{port}（填写管理端口，不是控制端口）")
    print(f"认证令牌：{'[已隐藏]' if hide_token else token}")
    print(f"管理监听配置：{bind}")

    try:
        address = ipaddress.ip_address(bind)
    except ValueError:
        address = None
    loopback = bind.lower() == "localhost" or (address is not None and address.is_loopback)
    if loopback:
        print(f"下位机地址：{bind}（仅供同一台电脑上的上位机使用）")
        print("远程不可用：当前管理服务仅本机监听，另一台上位机不能通过车辆 IP 连接。")
        print("需要下位机部署端配置局域网监听及可访问的控制端点，再受控重启；本脚本不修改配置。")
    elif bind == "0.0.0.0" and local:
        print("下位机地址：127.0.0.1（上位机和下位机运行在同一台主机）")
    elif bind == "0.0.0.0":
        addresses = lan_addresses()
        if addresses:
            print("下位机地址候选（选择与上位机网络互通的网卡 IP）：")
            for interface, host in addresses:
                print(f"  {host}  网卡={interface}")
        else:
            print("下位机地址：未取得 IPv4 地址，请运行 hostname -I 或 ip -4 addr 核对。")
        print("0.0.0.0 是监听地址，不能填入上位机。候选 IP 不代表网络已连通。")
    elif address is not None and address.version == 4 and not address.is_unspecified:
        print(f"下位机地址：{bind}（以管理配置为准，请确认该地址属于当前下位机且网络互通）")
    else:
        print("下位机地址：无法自动确认此监听配置的远程地址，请由部署端核对。")

    listening = listeners(port)
    if listening is None:
        print("端口监听：无法检查（ss 命令不可用或执行失败）。")
    elif listening:
        print("检测到端口监听：" + ", ".join(listening))
        print("监听只证明端口占用；请确认是使用上述配置启动的管理服务。")
    else:
        print("端口监听：未发现，请确认 run_manager.sh 已成功启动且未退出。")
    print("连接后仍须核对机器人身份及 hardware/mock 档案；这些参数不会把模拟切为实机。")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="只读显示下位机管理服务的上位机连接参数；不连接或启动服务。")
    parser.add_argument("--config", type=Path,
                        default=Path(os.environ.get("HOSTCOMPUTER_MANAGEMENT_CONFIG")
                                     or str(Path.home() / "ros2_ws_car/linked-runtime/manager.json")),
                        help="与 run_manager.sh 实际使用的管理 JSON 相同")
    parser.add_argument("--hide-token", action="store_true", help="隐藏令牌，便于分享排查输出")
    parser.add_argument("--local", action="store_true", help="同机连接；监听允许时显示 127.0.0.1")
    args = parser.parse_args(argv)
    return connection_card(args.config, args.hide_token, args.local)


if __name__ == "__main__":
    sys.exit(main())
