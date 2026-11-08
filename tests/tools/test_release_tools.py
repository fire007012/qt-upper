"""Host artifact boundaries and executable discovery; no network or ROS."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from host_paths import external_output
from check_host_runtime import find_ffmpeg
from profile_migration_report import compare_profiles


class ReleaseToolsTest(unittest.TestCase):
    def test_migration_never_fills_missing_device_values(self):
        report = compare_profiles(dict(profile_id='mock', startup_timeout=50, motor_direction=-1),
            dict(profile_id='mock', startup_timeout=90, video=True, video_required=False), 'current-revision')
        self.assertEqual(report['proposed_patches'], [dict(profile_id='mock', field='startup_timeout', value=50)])
        self.assertIn('video_required', report['missing_fields'])
        self.assertIn('motor_direction', report['unmapped_differences'])
        self.assertFalse(report['applied'])

    def test_migration_rejects_invalid_policy_and_profile_identity(self):
        report = compare_profiles(dict(profile_id='mock', startup_timeout=True, video_required=True),
            dict(profile_id='mock', startup_timeout=90, video=False, video_required=False))
        self.assertEqual(report['invalid_fields'], ['startup_timeout', 'video_required'])
        self.assertEqual(report['proposed_patches'], [])
        report = compare_profiles(dict(profile_id='mock', startup_timeout=10**1000, video_required=False),
            dict(profile_id='mock', startup_timeout=90, video_required=False))
        self.assertEqual(report['invalid_fields'], ['startup_timeout'])
        with self.assertRaises(ValueError):
            compare_profiles(dict(profile_id='a'), dict(profile_id='b'))

    def test_reference_and_source_ancestors_are_rejected(self):
        for path in (ROOT, ROOT.parent, ROOT / 'ros2_ws_car/new-build', ROOT / 'archive/new-build'):
            with self.subTest(path=path), self.assertRaises(ValueError):
                external_output(path)

    def test_symlink_into_reference_is_rejected_before_creation(self):
        with tempfile.TemporaryDirectory() as folder:
            link = Path(folder) / 'reference'
            try:
                link.symlink_to(ROOT / 'ros2_ws_car', target_is_directory=True)
            except OSError:
                self.skipTest('symlink creation unavailable')
            with self.assertRaises(ValueError):
                external_output(link / 'new-output')

    def test_missing_ffmpeg_is_none_instead_of_current_directory(self):
        with tempfile.TemporaryDirectory() as folder, patch.dict(os.environ, {}, clear=True), patch('shutil.which', return_value=None):
            self.assertIsNone(find_ffmpeg(Path(folder)))

    def test_bundle_ffmpeg_is_selected_without_path(self):
        with tempfile.TemporaryDirectory() as folder, patch.dict(os.environ, {}, clear=True), patch('shutil.which', return_value=None):
            binary = Path(folder) / ('ffmpeg.exe' if sys.platform == 'win32' else 'ffmpeg')
            binary.write_text('fixture'); binary.chmod(0o755)
            self.assertEqual(find_ffmpeg(Path(folder)), binary)

    def test_package_invalid_output_never_creates_it(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / 'not-created'
            result = subprocess.run([sys.executable, '-B', str(ROOT / 'scripts/package_host.py'),
                '--qt-prefix', folder, '--build-dir', str(ROOT / 'ros2_ws_car/no-build'),
                '--output-dir', str(output), '--ffmpeg', sys.executable, '--ffmpeg-license', __file__],
                text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('outside the source tree', result.stderr)
            self.assertFalse(output.exists())

    def test_evidence_label_cannot_escape_bundle(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / 'not-created'
            result = subprocess.run([sys.executable, '-B', str(ROOT / 'scripts/package_host.py'),
                '--qt-prefix', folder, '--build-dir', str(Path(folder) / 'build'),
                '--output-dir', str(output), '--ffmpeg', sys.executable, '--ffmpeg-license', __file__,
                '--evidence', '../escape=' + __file__], text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('simple NAME=JSON', result.stderr)
            self.assertFalse(output.exists())



if __name__ == '__main__':
    unittest.main()
