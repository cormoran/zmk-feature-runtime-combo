import os
import platform
import shutil
import subprocess
import unittest
from pathlib import Path

from dataclasses import dataclass

THIS_DIR = Path(__file__).parent.resolve()


def run_west(args: list[str]) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env.setdefault("CMAKE_BUILD_PARALLEL_LEVEL", "1")
    env.setdefault("J", "1")
    return subprocess.run(
        ["west", *args],
        capture_output=True,
        text=True,
        cwd=THIS_DIR,
        env=env,
    )


@dataclass
class NotFound:
    text: str


@dataclass
class ConfigAndDeviceTree:
    # Expected rows in .config
    config: list[str | NotFound]
    # Expected rows in devicetree_generated.h
    device: list[str | NotFound]


class WestCommandsTests(unittest.TestCase):
    WEST_TOPDIR: Path
    BUILD_DIR: Path

    @classmethod
    def setUpClass(cls):
        cls.WEST_TOPDIR = Path(run_west(["topdir"]).stdout.strip())
        cls.BUILD_DIR = THIS_DIR / "build"

    @unittest.skipUnless(
        platform.system() == "Linux", "zmk-test is only supported on Linux"
    )
    def test_zmk_test(self):
        for test_case in ("test", "studio"):
            test_build_dir = self.BUILD_DIR / THIS_DIR.name / test_case
            shutil.rmtree(test_build_dir, ignore_errors=True)

            result = run_west(
                [
                    "zmk-test",
                    f"tests/{test_case}",
                    "-m",
                    ".",
                    "-d",
                    str(test_build_dir),
                ]
            )
            output = (
                result.stdout
                + result.stderr
                + self._zmk_test_logs(test_build_dir, test_case)
            )
            self.assertEqual(result.returncode, 0, output)
            self.assertIn(f"PASS: {test_case}", result.stdout, output)
            self.assertNotIn("FAILED: ", result.stdout, output)

    def test_zmk_build(self):
        self._test_zmk_build(
            {
                "runtime_combo_board_feature_disabled": ConfigAndDeviceTree(
                    config=[
                        'CONFIG_ZMK_KEYBOARD_NAME="Module Test"',
                        "CONFIG_ZMK_USB=y",
                        "CONFIG_ZMK_BLE=y",
                        "# CONFIG_ZMK_RUNTIME_COMBO is not set",
                    ],
                    device=[
                        "DT_COMPAT_HAS_OKAY_zmk_keymap",
                    ],
                ),
                "runtime_combo_board_with_rpc": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_ZMK_RUNTIME_COMBO=y",
                        "CONFIG_ZMK_RUNTIME_COMBO_STUDIO_RPC=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y",
                    ],
                    device=[],
                ),
                "runtime_combo_board_without_rpc": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_RUNTIME_COMBO=y",
                        "# CONFIG_ZMK_STUDIO is not set",
                        NotFound("CONFIG_ZMK_RUNTIME_COMBO_STUDIO_RPC"),
                    ],
                    device=[],
                ),
                "custom_settings_board": ConfigAndDeviceTree(
                    config=[
                        # Verify that zmk-feature-custom-settings is present and enabled
                        "zmk-feature-custom-settings",
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_ZMK_RUNTIME_COMBO=y",
                        "CONFIG_ZMK_RUNTIME_COMBO_STUDIO_RPC=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y",
                        "CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=128",
                        "CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=2048",
                    ],
                    device=[],
                ),
            }
        )

    def _test_zmk_build(
        self, artifacts_and_expected_build_params: dict[str, ConfigAndDeviceTree]
    ):

        for artifact in artifacts_and_expected_build_params.keys():
            shutil.rmtree(self.BUILD_DIR / artifact, ignore_errors=True)

        for artifact, entries in artifacts_and_expected_build_params.items():
            zephyr_cache_dir = self.BUILD_DIR / ".zephyr-cache" / artifact

            result = run_west(
                [
                    "zmk-build",
                    "tests/zmk-config",
                    "-q",
                    "-af",
                    f"^{artifact}$",
                    "-d",
                    str(self.BUILD_DIR),
                    "--cmake-args",
                    (
                        f"-DUSER_CACHE_DIR={zephyr_cache_dir} "
                        f"-DZEPHYR_TOOLCHAIN_CAPABILITY_CACHE_DIR={zephyr_cache_dir / 'ToolchainCapabilityDatabase'}"
                    ),
                ]
            )
            self.assertEqual(
                result.returncode,
                0,
                f"{artifact} build failed\n{result.stdout}{result.stderr}",
            )

            artifact_dir = self.BUILD_DIR / artifact / "zephyr"
            config_path = artifact_dir / ".config"
            device_tree_path = (
                artifact_dir
                / "include"
                / "generated"
                / "zephyr"
                / "devicetree_generated.h"
            )
            self._test_strings_in_file(
                config_path, entries.config, f"{artifact} config"
            )
            if entries.device:
                self._test_strings_in_file(
                    device_tree_path, entries.device, f"{artifact} device tree"
                )
            self.assertTrue(
                (artifact_dir / "zmk.uf2").exists(),
                f"{artifact} zmk.uf2 is missing in {artifact_dir}",
            )

    def _test_strings_in_file(
        self, file_path: Path, expected_strings: list[str | NotFound], hint: str
    ):
        self.assertTrue(file_path.exists(), f"{hint}: {file_path} is missing")
        file_text = file_path.read_text()

        for expected in expected_strings:
            if isinstance(expected, NotFound):
                if expected.text in file_text:
                    self.fail(
                        f"{hint}: {expected.text} found in {file_path}, but it should not be present"
                    )
            else:
                if expected not in file_text:
                    self.fail(f"{hint}: {expected} not found in {file_path}")

    def _zmk_test_logs(self, build_dir: Path, test_case: str) -> str:
        log_dir = build_dir / "tests" / test_case
        logs = []
        for log_name in ("build.log", "keycode_events_full.log", "keycode_events.log"):
            log_path = log_dir / log_name
            if log_path.exists():
                logs.append(f"\n--- {log_path} ---\n{log_path.read_text()}")
        return "".join(logs)


if __name__ == "__main__":
    unittest.main()
