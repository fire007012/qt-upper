#!/usr/bin/env python3
"""Collect host build/runtime evidence without deploying or starting the GUI."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    protected = (root / 'ros2_ws_car').resolve()
    if output == protected or protected in output.parents:
        parser.error('output must be outside the read-only reference tree')
    binary = args.binary.resolve(strict=True)
    compatibility = json.loads((root / 'deploy/compatibility.json').read_text())
    version = subprocess.run([str(binary), '--version'], check=True, text=True,
                             capture_output=True, timeout=10).stdout.strip()
    if not version.startswith(compatibility['host_build_version'] + ' Qt 6.'):
        parser.error('binary version does not match the Qt6 host compatibility manifest')
    dependencies = None
    checks = []
    if platform.system() == 'Linux':
        result = subprocess.run(['ldd', str(binary)], text=True, capture_output=True, timeout=10)
        dependencies = result.stdout
        checks.append(dict(name='linked_libraries', passed=result.returncode == 0 and 'not found' not in result.stdout))
    else:
        checks.append(dict(name='linked_libraries', passed=False, reason='run platform packaging checks separately'))
    ffmpeg = shutil.which('ffmpeg')
    checks.append(dict(name='ffmpeg_on_path', passed=ffmpeg is not None))
    report = dict(format_version=1, version=version, platform=platform.platform(),
                  binary=str(binary), binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  compatibility=compatibility, checks=checks, linked_libraries=dependencies,
                  ffmpeg_path=ffmpeg, scope='host_local_runtime_only',
                  gui_plugins_validated=False, clean_machine_validated=False,
                  physical_acceptance=False, stability_24h_validated=False)
    # Atomic replace; failures leave existing evidence intact.
    with tempfile.NamedTemporaryFile(mode='w', dir=output.parent, prefix='.host-release-', delete=False) as file:
        temporary = Path(file.name)
        json.dump(report, file, ensure_ascii=False, indent=2)
        file.write('\n')
    try:
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)
    print(json.dumps(dict(output=str(output), passed=all(c['passed'] for c in checks),
                          scope=report['scope']), ensure_ascii=False))
    return 0 if all(c['passed'] for c in checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
