#!/usr/bin/env python3
import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUILD_DIR = ROOT / "build" / "Desktop_Qt_6_7_3-Debug"


def executable_name() -> str:
    return "hostcomputer.exe" if platform.system().lower().startswith("win") else "hostcomputer"


def ffmpeg_name() -> str:
    return "ffmpeg.exe" if platform.system().lower().startswith("win") else "ffmpeg"


def find_ffmpeg(app_dir: Path, explicit_path: str = ""):
    candidates = []
    if explicit_path:
        candidates.append(Path(explicit_path))
    env_path = os.environ.get("HOSTCOMPUTER_FFMPEG_PATH", "").strip()
    if env_path:
        candidates.append(Path(env_path))
    candidates.append(app_dir / ffmpeg_name())

    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate

    path_hit = shutil.which(ffmpeg_name()) or shutil.which("ffmpeg")
    return Path(path_hit) if path_hit else None


def run_version(binary: Path) -> str:
    result = subprocess.run(
        [str(binary), "-version"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=5,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stdout.strip())
    return result.stdout.splitlines()[0] if result.stdout else str(binary)


def main() -> int:
    parser = argparse.ArgumentParser(description="Check hostcomputer runtime bundle")
    parser.add_argument(
        "--app-dir",
        default=str(DEFAULT_BUILD_DIR),
        help="directory containing hostcomputer executable",
    )
    parser.add_argument(
        "--ffmpeg",
        default="",
        help="optional explicit ffmpeg path to validate",
    )
    parser.add_argument(
        "--require-bundled-ffmpeg",
        action="store_true",
        help="fail unless ffmpeg is next to the hostcomputer executable",
    )
    args = parser.parse_args()

    app_dir = Path(args.app_dir).resolve()
    app = app_dir / executable_name()
    if not app.is_file():
        print(f"FAIL: hostcomputer executable not found: {app}", file=sys.stderr)
        return 1

    ffmpeg = find_ffmpeg(app_dir, args.ffmpeg)
    if not ffmpeg:
        print(
            "FAIL: ffmpeg not found. Put ffmpeg next to hostcomputer, "
            "set HOSTCOMPUTER_FFMPEG_PATH, or add it to PATH.",
            file=sys.stderr,
        )
        return 1

    bundled = ffmpeg.resolve().parent == app_dir
    if args.require_bundled_ffmpeg and not bundled:
        print(f"FAIL: ffmpeg is not bundled in app dir: {ffmpeg}", file=sys.stderr)
        return 1

    try:
        version_line = run_version(ffmpeg)
    except Exception as exc:
        print(f"FAIL: cannot run ffmpeg: {ffmpeg}: {exc}", file=sys.stderr)
        return 1

    print(f"hostcomputer: {app}")
    print(f"ffmpeg: {ffmpeg}")
    print(f"ffmpeg_bundled: {'yes' if bundled else 'no'}")
    print(f"ffmpeg_version: {version_line}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
