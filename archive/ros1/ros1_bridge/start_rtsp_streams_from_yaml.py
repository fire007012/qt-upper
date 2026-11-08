#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

import yaml


DEFAULT_CONFIG = str(Path(__file__).with_name("video_sources.local.yaml"))
DEFAULT_LOG_DIR = "/tmp/rtsp_logs"
DEFAULT_STARTUP_CHECK_SECONDS = 1.5
DEFAULT_STARTUP_DELAY_SECONDS = 0.5
DEFAULT_DEVICE_TIMEOUT_SECONDS = 5.0


def is_rtsp_listening(port: int) -> bool:
    result = subprocess.run(
        ["ss", "-lnt"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    return f":{port} " in result.stdout


def log_tail(log_path: Path, max_bytes: int = 4096) -> str:
    if not log_path.exists():
        return ""
    with open(log_path, "rb") as f:
        try:
            f.seek(-max_bytes, os.SEEK_END)
        except OSError:
            f.seek(0)
        return f.read().decode("utf-8", errors="replace").replace("\r", "\n").strip()


def stop_existing_publishers(rtsp_port: int, rtsp_paths: list[str]) -> None:
    for rtsp_path in rtsp_paths:
        pattern = f"^ffmpeg .*rtsp://.*:{rtsp_port}/{rtsp_path}"
        subprocess.run(["pkill", "-f", pattern], check=False)


def is_device_busy(device: str) -> bool:
    return subprocess.run(
        ["fuser", "-s", device],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    ).returncode == 0


def wait_for_device(device: str, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not is_device_busy(device):
            return True
        time.sleep(0.2)
    return not is_device_busy(device)


def build_ffmpeg_cmd(source: dict, rtsp_host: str, rtsp_port: int) -> list[str]:
    input_width = int(source.get("input_width", source.get("width", 1280)))
    input_height = int(source.get("input_height", source.get("height", 720)))
    bitrate = int(source.get("bitrate_kbps", 2500))
    rtsp_path = str(source.get("rtsp_path") or source.get("source_id")).strip("/")
    url = f"rtsp://{rtsp_host}:{rtsp_port}/{rtsp_path}"

    cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "info",
        "-f",
        "v4l2",
        "-input_format",
        str(source.get("input_format", "mjpeg")),
        "-video_size",
        f"{input_width}x{input_height}",
        "-framerate",
        str(int(source.get("fps", 30))),
        "-i",
        str(source["device"]),
    ]

    vf = str(source.get("vf", "")).strip()
    if vf:
        cmd += ["-vf", vf]

    cmd += [
        "-c:v",
        "libx264",
        "-preset",
        "ultrafast",
        "-tune",
        "zerolatency",
        "-g",
        str(int(source.get("gop", source.get("fps", 30)))),
        "-keyint_min",
        str(int(source.get("keyint_min", source.get("gop", source.get("fps", 30))))),
        "-sc_threshold",
        "0",
        "-x264-params",
        str(source.get("x264_params", "repeat-headers=1")),
        "-b:v",
        f"{bitrate}k",
        "-pix_fmt",
        "yuv420p",
        "-f",
        "rtsp",
        "-rtsp_transport",
        "tcp",
        url,
    ]
    return cmd


def load_enabled_sources(config_path: str) -> tuple[dict, list[dict]]:
    with open(config_path, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f) or {}
    rtsp = data.get("rtsp", {}) if isinstance(data.get("rtsp", {}), dict) else {}
    sources = [
        source for source in data.get("direct_sources", [])
        if isinstance(source, dict) and bool(source.get("enabled", True))
    ]
    return rtsp, sources


def run_once(args: argparse.Namespace) -> int:
    rtsp, sources = load_enabled_sources(args.config)
    rtsp_host = args.rtsp_host or str(rtsp.get("host", "127.0.0.1"))
    rtsp_port = int(args.rtsp_port or rtsp.get("port", 8554))
    rtsp_paths = [
        str(source.get("rtsp_path") or source.get("source_id")).strip("/")
        for source in sources
    ]

    if not is_rtsp_listening(rtsp_port):
        print(f"MediaMTX is not listening on port {rtsp_port}", file=sys.stderr)
        return 1

    log_dir = Path(args.log_dir)
    log_dir.mkdir(parents=True, exist_ok=True)
    procs: list[subprocess.Popen] = []

    if not args.keep_existing:
        stop_existing_publishers(rtsp_port, rtsp_paths)
        time.sleep(0.5)

    for source in sources:
        device = str(source.get("device", ""))
        source_id = str(source.get("source_id") or source.get("rtsp_path") or device)
        if not os.path.exists(device):
            print(f"missing device for {source_id}: {device}", file=sys.stderr)
            return 1

        cmd = build_ffmpeg_cmd(source, rtsp_host, rtsp_port)
        rtsp_path = str(source.get("rtsp_path") or source_id).strip("/")
        log_path = log_dir / f"{rtsp_path}.log"
        if not wait_for_device(device, args.device_timeout):
            print(f"device busy for {source_id}: {device}", file=sys.stderr)
            subprocess.run(["fuser", "-v", device], check=False)
            return 1

        log = open(log_path, "w", encoding="utf-8")
        proc = subprocess.Popen(
            cmd,
            stdin=subprocess.DEVNULL,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=not args.foreground,
        )
        log.close()
        procs.append(proc)
        print(f"started {rtsp_path}: pid={proc.pid} log={log_path}")
        time.sleep(args.startup_check)
        code = proc.poll()
        if code is not None:
            print(f"{rtsp_path} exited during startup: code={code}", file=sys.stderr)
            tail = log_tail(log_path)
            if tail:
                print(f"===== {log_path} =====", file=sys.stderr)
                print(tail, file=sys.stderr)
            for running_proc in procs:
                if running_proc.poll() is None:
                    running_proc.terminate()
            return code or 1
        if args.startup_delay > 0:
            time.sleep(args.startup_delay)

    if not args.foreground:
        print(f"started {len(procs)} RTSP publisher(s)")
        return 0

    def stop_all(signum=None, frame=None) -> None:
        for proc in procs:
            if proc.poll() is None:
                proc.terminate()
        deadline = time.time() + 3.0
        for proc in procs:
            if proc.poll() is None:
                timeout = max(0.0, deadline - time.time())
                try:
                    proc.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    proc.kill()

    signal.signal(signal.SIGINT, stop_all)
    signal.signal(signal.SIGTERM, stop_all)

    try:
        while True:
            for proc in procs:
                code = proc.poll()
                if code is not None:
                    stop_all()
                    print(f"ffmpeg exited: pid={proc.pid} code={code}", file=sys.stderr)
                    return code or 1
            time.sleep(2)
    finally:
        stop_all()


def main() -> int:
    parser = argparse.ArgumentParser(description="Start RTSP ffmpeg publishers from video_sources YAML")
    parser.add_argument("--config", default=DEFAULT_CONFIG)
    parser.add_argument("--rtsp-host", default="")
    parser.add_argument("--rtsp-port", type=int, default=0)
    parser.add_argument("--log-dir", default=DEFAULT_LOG_DIR)
    parser.add_argument("--foreground", action="store_true")
    parser.add_argument("--keep-existing", action="store_true")
    parser.add_argument("--startup-check", type=float, default=DEFAULT_STARTUP_CHECK_SECONDS)
    parser.add_argument("--startup-delay", type=float, default=DEFAULT_STARTUP_DELAY_SECONDS)
    parser.add_argument("--device-timeout", type=float, default=DEFAULT_DEVICE_TIMEOUT_SECONDS)
    return run_once(parser.parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
