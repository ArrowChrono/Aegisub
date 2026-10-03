#!/usr/bin/env python3
"""Read-only planning tests; synthetic ELF headers are never loaded or executed."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("stage_runtimes", Path(__file__).with_name("stage-runtimes.py"))
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)


class RuntimePlanTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.output = self.base / "output"
        self.source = self.base / "provider.so"
        self.source.write_bytes(b"\x7fELF\x02\x01" + bytes(10) + b"\x03\x00\x3e\x00")
        self.manifest = {"schema_version": 1, "runtimes": [{
            "source": "provider.so", "destination": "bin/runtimes/provider.so",
            "aliases": ["bin/runtimes/provider.so.1"]}]}

    def test_validation_does_not_create_output(self):
        entries = stage.plan(self.base, self.output, self.manifest)
        self.assertEqual(len(entries), 1)
        self.assertFalse(self.output.exists())

    def test_rejects_non_elf(self):
        self.source.write_text("not a library")
        with self.assertRaises(ValueError):
            stage.plan(self.base, None, self.manifest)

    def test_rejects_wrong_architecture(self):
        header = bytearray(self.source.read_bytes())
        header[18:20] = b"\xb7\x00"  # AArch64
        self.source.write_bytes(header)
        with self.assertRaises(ValueError):
            stage.plan(self.base, None, self.manifest)

    def test_rejects_duplicate_destination(self):
        self.manifest["runtimes"].append(self.manifest["runtimes"][0])
        with self.assertRaises(ValueError):
            stage.plan(self.base, self.output, self.manifest)
        self.assertFalse(self.output.exists())

    def test_rejects_overwrite(self):
        target = self.output / "bin/runtimes/provider.so"
        target.parent.mkdir(parents=True)
        target.write_text("keep")
        with self.assertRaises(FileExistsError):
            stage.plan(self.base, self.output, self.manifest)
        self.assertEqual(target.read_text(), "keep")

    def test_rejects_symlink_escape(self):
        self.output.mkdir()
        (self.output / "bin").symlink_to(self.base, target_is_directory=True)
        with self.assertRaises(ValueError):
            stage.plan(self.base, self.output, self.manifest)

    def test_rejects_unconfined_paths(self):
        for value in ["../provider.so", "/tmp/provider.so", ""]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                stage.relative_path(value)

    def test_rejects_unexpected_destination(self):
        self.manifest["runtimes"][0]["destination"] = "bin/aegisub"
        with self.assertRaises(ValueError):
            stage.plan(self.base, self.output, self.manifest)


if __name__ == "__main__":
    unittest.main()
