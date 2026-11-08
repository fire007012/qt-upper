#!/usr/bin/env python3
"""Real loopback H264/RTSP server -> production FFmpeg decoder; synthetic frames."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import threading

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from host_paths import external_output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    args = parser.parse_args()
    try:
        output = external_output(args.output_dir)
        if output.exists():
            raise ValueError('choose a new evidence directory')
        probe = args.probe.resolve(strict=True)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    import gi
    gi.require_version('Gst', '1.0'); gi.require_version('GstRtspServer', '1.0')
    from gi.repository import Gst, GstRtspServer, GLib
    Gst.init(None)
    for plugin in ('videotestsrc', 'videoconvert', 'x264enc', 'rtph264pay'):
        if not Gst.ElementFactory.find(plugin):
            parser.error('missing GStreamer element: ' + plugin)
    output.mkdir(parents=True)
    server = GstRtspServer.RTSPServer(); server.set_address('127.0.0.1'); server.set_service('0')
    factory = GstRtspServer.RTSPMediaFactory(); factory.set_shared(True)
    factory.set_launch('( videotestsrc is-live=true pattern=ball ! videoconvert ! '
                       'video/x-raw,width=160,height=120,framerate=15/1 ! '
                       'x264enc tune=zerolatency speed-preset=ultrafast key-int-max=15 ! rtph264pay name=pay0 pt=96 )')
    server.get_mount_points().add_factory('/test', factory)
    source = server.attach(None)
    if not source:
        raise RuntimeError('cannot listen on temporary loopback RTSP port')
    url = f'rtsp://127.0.0.1:{server.get_bound_port()}/test'
    loop = GLib.MainLoop(); serving = threading.Thread(target=loop.run, daemon=True); serving.start()
    report = dict(format_version=1, scope='actual_rtsp_synthetic_video', passed=False,
                  physical_camera_validated=False, desktop_rendering_validated=False)
    try:
        with (output / 'probe.log').open('w') as log:
            subprocess.run([str(probe)], env=dict(os.environ, HOST_TEST_RTSP_URL=url),
                           stdout=log, stderr=subprocess.STDOUT, cwd=output, timeout=60, check=True)
        report.update(passed=True, codec='H264', transport='RTSP/TCP', decoded_frame_size=[160,120],
                      restart_cycles=3, fisheye_filter_validated=True)
    finally:
        loop.quit(); serving.join(3); GLib.source_remove(source)
        (output / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
