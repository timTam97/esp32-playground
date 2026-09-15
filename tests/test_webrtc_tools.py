"""Recovery must reject damaged or incomplete snapshots before touching a board."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("webrtc_tools", ROOT / "tools/webrtc.py")
tools = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tools)


class RecoverySnapshotTests(unittest.TestCase):
    def snapshot(self, directory, size=4 * 1024 * 1024):
        path = Path(directory) / "recovery.bin"
        path.write_bytes(b"\xff" * size)
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        path.with_suffix(".sha256").write_text(f"{digest}  recovery.bin\n")
        return path

    def test_complete_snapshot_with_matching_digest_is_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.snapshot(directory)
            self.assertEqual(tools.verify_backup(path), path.resolve())

    def test_changed_byte_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.snapshot(directory)
            with path.open("r+b") as output:
                output.seek(0x10000)
                output.write(b"\x00")
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                tools.verify_backup(path)

    def test_partial_image_is_rejected_even_with_matching_digest(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.snapshot(directory, 1024 * 1024)
            with self.assertRaisesRegex(ValueError, "complete 4 MiB"):
                tools.verify_backup(path)

    def test_missing_digest_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.snapshot(directory)
            path.with_suffix(".sha256").unlink()
            with self.assertRaises(FileNotFoundError):
                tools.verify_backup(path)


if __name__ == "__main__":
    unittest.main()
