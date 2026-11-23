"""执行正式构建脚本的权限收尾代码，校验失败状态及受控输出范围。"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class BuildOutputPermissionsTest(unittest.TestCase):
    """在独立临时目录回放真实 shell 收尾函数，不启动 Docker 或编译。"""

    def test_output_permissions_and_exit_status(self):
        """成功、构建失败、所有权恢复失败均保留正确状态和文件权限。"""
        source = (Path(__file__).resolve().parents[2] / "build-watcher.sh").read_text()
        start = source.index("restore_output_owner() {")
        end = source.index("trap restore_output_owner EXIT", start)
        cleanup = source[start:end] + "trap restore_output_owner EXIT\n"
        for code, fail_chown in ((0, False), (7, False), (0, True)):
            with self.subTest(code=code, fail_chown=fail_chown), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                outside = root / "unrelated"
                outside.write_text("unchanged")
                outside.chmod(0o644)
                script = "umask 077\n" + cleanup
                if fail_chown:
                    script += "chown() { return 1; }\n"
                script += "mkdir build\nprintf data > build/image.bin\nprintf config > sdkconfig\n"
                script += f"exit {code}\n"
                environment = dict(os.environ, WATCHER_BUILD_UID=str(os.getuid()), WATCHER_BUILD_GID=str(os.getgid()))
                result = subprocess.run(["bash", "-c", script], cwd=root, env=environment,
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 1 if fail_chown else code)
                self.assertEqual((root / "build").stat().st_mode & 0o777, 0o700)
                for output in (root / "build/image.bin", root / "sdkconfig"):
                    self.assertEqual(output.stat().st_mode & 0o777, 0o600)
                    self.assertEqual(output.stat().st_uid, os.getuid())
                self.assertFalse((root / "sdkconfig.old").exists())
                self.assertEqual(outside.stat().st_mode & 0o777, 0o644)
                self.assertEqual(outside.read_text(), "unchanged")


if __name__ == "__main__":
    unittest.main()
