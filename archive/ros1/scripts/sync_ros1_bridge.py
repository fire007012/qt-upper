#!/usr/bin/env python3

from __future__ import annotations

if __name__ == "__main__":
    raise SystemExit("ROS1同步脚本已退役；ROS2部署由独立下位机工程负责，禁止向ROS2工作区复制旧桥接。")


import os
import shutil
import sys
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent

# Scan the parent folder of this project, for example /home/rera when the
# project is /home/rera/hostcomputer.
SOURCE_SEARCH_ROOT = PROJECT_DIR.parent

# ======================= 必须修改这里 =======================
# 只改下一行：填入目标 ROS 工作空间的 src 目录，例如：
# TARGET_PARENT_DIR = Path("/home/你的用户名/robot24_ws/src")
# 也可以写 Path("~/Robot24_catkin_ws/src")，脚本会自动展开 ~。
# 如果不改，脚本会直接报错，避免误复制到错误位置。
# ============================================================
DEFAULT_TARGET_PARENT_DIR = Path("__CHANGE_ME_ROS_WORKSPACE_SRC__")
TARGET_PARENT_DIR = Path("~/robot24_ws/src")

SKIP_DIR_NAMES = {
    ".cache",
    ".config",
    ".dbus",
    ".git",
    ".local",
    ".nv",
    "__pycache__",
    "build",
    "build-smoke",
    "build-sdl-verify",
    "build_test",
    "devel",
    "install",
    "log",
    "logs",
}


def is_ros1_bridge_dir(path: Path) -> bool:
    return (
        path.is_dir()
        and (path / "package.xml").is_file()
        and (path / "CMakeLists.txt").is_file()
        and (path / "host_bridge_node.py").is_file()
    )


def same_path(left: Path, right: Path) -> bool:
    try:
        return left.resolve() == right.resolve()
    except FileNotFoundError:
        return left.absolute() == right.absolute()


def find_ros1_bridge(target_dir: Path) -> Path:
    project_candidate = PROJECT_DIR / "ros1_bridge"
    if is_ros1_bridge_dir(project_candidate) and not same_path(project_candidate, target_dir):
        return project_candidate

    candidates: list[Path] = []
    for root, dirs, _files in os.walk(SOURCE_SEARCH_ROOT, topdown=True, onerror=lambda _err: None):
        dirs[:] = [
            name
            for name in dirs
            if name not in SKIP_DIR_NAMES and not name.startswith(".")
        ]

        if "ros1_bridge" not in dirs:
            continue

        candidate = Path(root) / "ros1_bridge"
        dirs.remove("ros1_bridge")
        if same_path(candidate, target_dir):
            continue
        if is_ros1_bridge_dir(candidate):
            candidates.append(candidate)

    if not candidates:
        raise FileNotFoundError(f"未在 {SOURCE_SEARCH_ROOT} 下找到 ros1_bridge 源目录")

    candidates.sort(key=lambda path: len(path.parts))
    return candidates[0]


def replace_directory(source_dir: Path, target_dir: Path) -> None:
    if same_path(source_dir, target_dir):
        raise RuntimeError(f"源目录和目标目录相同，拒绝覆盖自身: {source_dir}")

    target_dir.parent.mkdir(parents=True, exist_ok=True)

    if target_dir.exists() or target_dir.is_symlink():
        if target_dir.is_symlink() or target_dir.is_file():
            target_dir.unlink()
        else:
            shutil.rmtree(target_dir)

    shutil.copytree(source_dir, target_dir, symlinks=True)


def normalize_target_parent_dir(path: Path) -> Path:
    expanded = path.expanduser()
    if not expanded.is_absolute():
        raise RuntimeError("TARGET_PARENT_DIR 必须填写绝对路径，或使用 ~/ 开头的家目录路径")
    return expanded.resolve()


def main() -> int:
    try:
        if TARGET_PARENT_DIR == DEFAULT_TARGET_PARENT_DIR:
            raise RuntimeError(
                "目标路径仍是默认占位值，请先打开脚本修改 TARGET_PARENT_DIR"
            )
        target_parent_dir = normalize_target_parent_dir(TARGET_PARENT_DIR)
        target_dir = target_parent_dir / "ros1_bridge"
        source_dir = find_ros1_bridge(target_dir)
        replace_directory(source_dir, target_dir)
    except Exception as exc:
        print(f"同步 ros1_bridge 失败: {exc}", file=sys.stderr)
        return 1

    print(f"已复制: {source_dir}")
    print(f"目标目录: {target_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
