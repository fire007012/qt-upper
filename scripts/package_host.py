#!/usr/bin/env python3
"""Build a native Qt6/QML runtime bundle with version/hash evidence; no deployment."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile

from host_paths import ROOT, external_output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--qt-prefix', required=True, type=Path)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--ffmpeg', required=True, type=Path)
    parser.add_argument('--ffmpeg-license', required=True, type=Path)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--evidence', action='append', default=[], metavar='NAME=JSON',
                        help='include explicitly selected JSON validation evidence in the bundle')
    args = parser.parse_args()
    try:
        build, output = external_output(args.build_dir), external_output(args.output_dir)
        if output.exists():
            raise ValueError('choose a new output directory; existing bundles are never overwritten')
        if Path(str(output) + '.zip').exists():
            raise ValueError('archive already exists; choose a new output directory')
        if build == output or build in output.parents or output in build.parents:
            raise ValueError('build and output directories must not overlap')
        qt = args.qt_prefix.resolve(strict=True)
        ffmpeg, license_file = args.ffmpeg.resolve(strict=True), args.ffmpeg_license.resolve(strict=True)
        if not ffmpeg.is_file() or not license_file.is_file() or args.jobs < 1:
            raise ValueError('FFmpeg and license must be files; jobs must be positive')
        evidence = {}
        for item in args.evidence:
            label, separator, filename = item.partition('=')
            if not separator or not re.fullmatch(r'[A-Za-z0-9_-]+', label) or label in evidence:
                raise ValueError('evidence must have unique simple NAME=JSON labels')
            source = Path(filename).resolve(strict=True)
            if source.stat().st_size > 10 * 1024 * 1024:
                raise ValueError('evidence JSON exceeds 10 MiB')
            json.loads(source.read_text(encoding='utf-8'))
            evidence[label] = source
    except (OSError, ValueError) as error:
        parser.error(str(error))
    output.mkdir(parents=True)
    log_path = output / 'package.log'
    def run(command, **kwargs):
        with log_path.open('a') as log:
            log.write(json.dumps([str(arg) for arg in command]) + '\n'); log.flush()
            subprocess.run([str(arg) for arg in command], stdout=log, stderr=subprocess.STDOUT, check=True, **kwargs)
    try:
        run([args.cmake, '-S', ROOT, '-B', build, '-DCMAKE_BUILD_TYPE=Release',
             '-DCMAKE_PREFIX_PATH=' + str(qt), '-DHOST_DEPLOY_RUNTIME=ON',
             '-DCMAKE_INSTALL_PREFIX=' + str(output), '-DHOST_FFMPEG_EXECUTABLE=' + str(ffmpeg),
             '-DHOST_FFMPEG_LICENSE=' + str(license_file)])
        run([args.cmake, '--build', build, '--config', 'Release', '-j', args.jobs])
        run([args.cmake, '--install', build, '--config', 'Release'])
        app_dir = output if platform.system() == 'Windows' else output / 'bin'
        binary = app_dir / ('hostcomputer.exe' if platform.system() == 'Windows' else 'hostcomputer')
        run([sys.executable, '-B', ROOT / 'scripts/check_host_runtime.py', '--app-dir', app_dir, '--require-bundled-ffmpeg'])
        # Remove development Qt overrides before testing the installed layout.
        env = {k:v for k,v in os.environ.items() if k not in (
            'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QML_IMPORT_PATH', 'QML2_IMPORT_PATH',
            'LD_LIBRARY_PATH', 'HOSTCOMPUTER_FFMPEG_PATH')}
        env['PATH'] = os.pathsep.join(entry for entry in env.get('PATH', '').split(os.pathsep)
                                     if entry and qt != Path(entry).resolve() and qt not in Path(entry).resolve().parents)
        env['QT_QPA_PLATFORM'] = 'offscreen'
        run([binary, '--check-ui-runtime'], env=env, cwd=output, timeout=30)
        run([sys.executable, '-B', ROOT / 'scripts/check_host_release.py', '--binary', binary,
             '--output', output / 'local-runtime-check.json'], env=env, timeout=30)
        # Copy to an unrelated path: an install-prefix-dependent bundle must fail
        # here, before generating a release manifest or archive.
        with tempfile.TemporaryDirectory(prefix='host-relocation-') as folder:
            relocated = Path(folder) / 'bundle'
            shutil.copytree(output, relocated, symlinks=True)
            moved_binary = relocated / binary.relative_to(output)
            run([moved_binary, '--check-ui-runtime'], env=env, cwd=relocated, timeout=30)
            if platform.system() == 'Linux':
                linked = subprocess.check_output(['ldd', str(moved_binary)], env=env, text=True, timeout=10)
                with log_path.open('a') as log:
                    log.write(linked)
                for line in linked.splitlines():
                    if '=> not found' in line:
                        raise RuntimeError('relocated bundle has unresolved libraries')
                    if 'libQt6' in line and '=>' in line:
                        library = Path(line.split('=>', 1)[1].strip().split()[0]).resolve()
                        if relocated not in library.parents:
                            raise RuntimeError('relocated bundle loaded Qt from outside its package')
        manifest = dict(format_version=1, generated_at=datetime.now(timezone.utc).isoformat(),
                        platform=platform.platform(), scope='native_host_runtime_bundle',
                        compatibility=json.loads((ROOT / 'deploy/compatibility.json').read_text()),
                        platform_plugin_qml_creation_validated=True, local_relocation_validated=True, rendering_validated=False,
                        clean_machine_validated=False, windows_validated=False,
                        physical_acceptance=False, files={})
        if evidence:
            (output / 'validation').mkdir()
            for label, source in evidence.items():
                shutil.copyfile(source, output / 'validation' / (label + '.json'))
        manifest['source_commit'] = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
        manifest['source_dirty'] = bool(subprocess.check_output(['git', '-C', str(ROOT), 'status', '--porcelain']))
        for file in sorted(output.rglob('*')):
            if file.is_file():
                digest = hashlib.sha256()
                with file.open('rb') as stream:
                    for block in iter(lambda: stream.read(1024 * 1024), b''):
                        digest.update(block)
                manifest['files'][file.relative_to(output).as_posix()] = digest.hexdigest()
        (output / 'release-manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
        archive = shutil.make_archive(str(output), 'zip', root_dir=output.parent, base_dir=output.name)
        print(json.dumps(dict(bundle=str(output), archive=archive, passed=True, scope=manifest['scope'])))
    except (OSError, subprocess.SubprocessError, RuntimeError) as error:
        print(f'Package failed: {error}; evidence: {log_path}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
