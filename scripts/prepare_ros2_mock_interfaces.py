#!/usr/bin/env python3
"""Build only exact interface types in a disposable mock overlay, never robot code."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from host_paths import external_output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference-src', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    args = parser.parse_args()
    try:
        output = external_output(args.output_dir)
        if output.exists():
            raise ValueError('choose a new fixture output directory')
        source = args.reference_src.resolve(strict=True)
        definitions = [(name, relative, (source / name / relative).read_bytes()) for name, relative in (
            ('robot_hardware', 'srv/SetProfile.srv'), ('arm_control', 'action/ExecuteArmGoal.action'))]
    except (OSError, ValueError) as error:
        parser.error(str(error))
    output.mkdir(parents=True)
    manifest = dict(scope='mock_interface_types_only', production_overlay=False, files={})
    for name, relative, content in definitions:
        package = output / 'src' / name
        target = package / relative; target.parent.mkdir(parents=True)
        target.write_bytes(content)
        manifest['files'][f'{name}/{relative}'] = hashlib.sha256(content).hexdigest()
        dependency = 'find_package(geometry_msgs REQUIRED)\n' if name == 'arm_control' else ''
        rosidl_dependency = ' DEPENDENCIES geometry_msgs' if name == 'arm_control' else ''
        (package / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.16)
project({name})
find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)
{dependency}rosidl_generate_interfaces(${{PROJECT_NAME}} "{relative}"{rosidl_dependency})
ament_export_dependencies(rosidl_default_runtime)
ament_package()
''')
        (package / 'package.xml').write_text(f'''<?xml version="1.0"?>
<package format="3"><name>{name}</name><version>0.0.0</version>
<description>Disposable host test interface fixture; never install on a robot</description>
<maintainer email="host-test@example.invalid">Host test fixture</maintainer><license>Test-only reference definitions</license>
<buildtool_depend>ament_cmake</buildtool_depend><build_depend>rosidl_default_generators</build_depend>
<exec_depend>rosidl_default_runtime</exec_depend>
{'<depend>geometry_msgs</depend>' if name == 'arm_control' else ''}
<member_of_group>rosidl_interface_packages</member_of_group>
<export><build_type>ament_cmake</build_type></export></package>
''')
    (output / 'fixture-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    colcon = shutil.which('colcon')
    if not colcon:
        parser.error('source ROS2 with colcon before preparing mock interfaces')
    subprocess.run([colcon, '--log-base', str(output / 'log'), 'build', '--merge-install',
                    '--base-paths', str(output / 'src'), '--build-base', str(output / 'build'),
                    '--install-base', str(output / 'install'), '--cmake-args', '-DBUILD_TESTING=OFF'],
                   cwd=output, check=True)
    print(json.dumps(dict(scope=manifest['scope'], setup=str(output / 'install/setup.bash'))))


if __name__ == '__main__':
    raise SystemExit(main())
