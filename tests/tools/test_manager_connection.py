"""Connection-card CLI regressions using disposable configs; no robot or ROS."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "scripts/show_manager_connection.py"


class ManagerConnectionTest(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.root = Path(self.folder.name)
        self.token = "fixture-connection-token-" + "x" * 32
        self.token_file = self.root / "token"
        self.token_file.write_text(self.token + "\n")
        self.config_file = self.root / "manager.json"
        self.config = {"bind": "127.0.0.1", "port": 19091,
                       "token_file": str(self.token_file),
                       "control_endpoint": {"host": "192.0.2.99", "port": 19090}}
        self.helper("ss", "LISTEN 0 128 127.0.0.1:19091 0.0.0.0:*\n")
        self.helper("ip", json.dumps([
            {"ifname": "lo", "addr_info": [{"local": "127.0.0.1"}]},
            {"ifname": "eth0", "addr_info": [{"local": "192.0.2.24"}]},
            {"ifname": "wlan0", "addr_info": [{"local": "198.51.100.24"}]}]))

    def helper(self, name, output):
        path = self.root / name
        path.write_text(f"#!{sys.executable}\nprint({output!r})\n")
        path.chmod(0o700)

    def run_card(self, *args):
        self.config_file.write_text(json.dumps(self.config))
        before = self.token_file.read_bytes()
        result = subprocess.run([sys.executable, "-B", str(SCRIPT), "--config",
                                 str(self.config_file), *args], capture_output=True,
                                text=True, cwd=self.root,
                                env=dict(os.environ, PATH=str(self.root)), timeout=10)
        self.assertEqual(self.token_file.read_bytes(), before)
        self.assertEqual(json.loads(self.config_file.read_text()), self.config)
        return result

    def test_loopback_is_never_advertised_as_remote_vehicle_address(self):
        result = self.run_card()
        self.assertEqual(result.returncode, 0)
        self.assertIn("远程不可用", result.stdout)
        self.assertIn("管理端口：19091", result.stdout)
        self.assertIn(self.token, result.stdout)
        self.assertIn("检测到端口监听", result.stdout)
        self.assertNotIn("192.0.2.99", result.stdout)
        self.assertNotIn("19090", result.stdout)

    def test_wildcard_lists_real_interfaces_instead_of_bind_address(self):
        self.config["bind"] = "0.0.0.0"
        result = self.run_card()
        self.assertEqual(result.returncode, 0)
        self.assertIn("192.0.2.24  网卡=eth0", result.stdout)
        self.assertIn("198.51.100.24  网卡=wlan0", result.stdout)
        self.assertNotIn("网卡=lo", result.stdout)
        self.assertIn("不能填入上位机", result.stdout)

    def test_hide_token_never_outputs_secret(self):
        result = self.run_card("--hide-token")
        self.assertEqual(result.returncode, 0)
        self.assertIn("认证令牌：[已隐藏]", result.stdout)
        self.assertNotIn(self.token, result.stdout + result.stderr)

    def test_local_card_uses_loopback_for_wildcard_listener(self):
        self.config["bind"] = "0.0.0.0"
        result = self.run_card("--local", "--hide-token")
        self.assertEqual(result.returncode, 0)
        self.assertIn("下位机地址：127.0.0.1", result.stdout)
        self.assertNotIn("地址候选", result.stdout)
        self.assertNotIn("192.0.2.24", result.stdout)

    def test_missing_network_tools_do_not_claim_service_is_running(self):
        (self.root / "ss").unlink()
        (self.root / "ip").unlink()
        self.config["bind"] = "0.0.0.0"
        result = self.run_card()
        self.assertEqual(result.returncode, 0)
        self.assertIn("无法检查", result.stdout)
        self.assertIn("未取得 IPv4 地址", result.stdout)

    def test_not_listening_is_distinguished_from_unknown(self):
        self.helper("ss", "LISTEN 0 128 0.0.0.0:19090 0.0.0.0:*\n")
        self.assertIn("端口监听：未发现", self.run_card().stdout)

    def test_invalid_config_and_token_fail_without_leaking_credentials(self):
        for port in (True, 0, 65536, "19091"):
            with self.subTest(port=port):
                self.config["port"] = port
                result = self.run_card()
                self.assertEqual(result.returncode, 1)
                self.assertNotIn(self.token, result.stdout + result.stderr)
        self.config["port"] = 19091
        self.token_file.write_text("too-short")
        self.assertEqual(self.run_card().returncode, 1)

    def test_missing_token_file_is_reported_without_traceback(self):
        self.config["token_file"] = str(self.root / "missing-token")
        result = self.run_card()
        self.assertEqual(result.returncode, 1)
        self.assertNotIn("Traceback", result.stderr)

    def test_relative_token_uses_config_directory_even_with_cwd_decoy(self):
        deployment = self.root / "deployment"
        deployment.mkdir()
        self.config_file = deployment / "manager.json"
        self.token_file = deployment / "manager.token"
        self.token_file.write_text(self.token)
        decoy = "wrong-cwd-token-" + "z" * 32
        (self.root / "manager.token").write_text(decoy)
        self.config["token_file"] = "manager.token"
        result = self.run_card()
        self.assertEqual(result.returncode, 0)
        self.assertIn(self.token, result.stdout)
        self.assertNotIn(decoy, result.stdout + result.stderr)

    def test_symlink_config_uses_target_directory_for_token(self):
        deployment = self.root / "deployment"
        deployment.mkdir()
        self.token_file = deployment / "manager.token"
        self.token_file.write_text(self.token)
        self.config_file.symlink_to(deployment / "manager.json")
        self.config["token_file"] = "manager.token"
        result = self.run_card("--hide-token")
        self.assertEqual(result.returncode, 0)
        self.assertNotIn(self.token, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
