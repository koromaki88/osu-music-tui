"""Unit tests for quick/full verification, independent of .NET and real game data."""

import argparse
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from indexer_integration import fingerprint, positive_timeout, validate_records


class VerificationTests(unittest.TestCase):
    def test_quick_does_not_open_audio(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "client.realm").write_bytes(b"database")
            audio = root / "files" / "a" / "aa" / ("a" * 64)
            audio.parent.mkdir(parents=True)
            audio.write_bytes(b"audio")
            original_open = Path.open

            def guarded_open(path, *args, **kwargs):
                if path == audio:
                    self.fail("quick verification must not read audio content")
                return original_open(path, *args, **kwargs)

            with patch.object(Path, "open", guarded_open):
                before = fingerprint(root)
            self.assertNotIn(str(audio.relative_to(root)), before)
            (root / "client.realm").write_bytes(b"changed database")
            self.assertNotEqual(before, fingerprint(root))

    def test_full_covers_nested_audio(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "files").mkdir()
            audio = root / "files" / "audio"
            audio.write_bytes(b"audio")
            before = fingerprint(root, "full")
            audio.write_bytes(b"other")
            self.assertNotEqual(before, fingerprint(root, "full"))

    @unittest.skipUnless(hasattr(os, "mkfifo"), "requires Unix notification FIFOs")
    def test_notification_pipe_is_never_read(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            os.mkfifo(root / "client.realm.note")
            for mode in ("quick", "full"):
                self.assertIsNone(fingerprint(root, mode)["client.realm.note"][-1])

    def test_large_export_is_processed_incrementally(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            content_hash = "a" * 64
            audio = root / "files" / "a" / "aa" / content_hash
            audio.parent.mkdir(parents=True)
            audio.touch()
            track = json.dumps({"type": "track", "audio_hash": content_hash,
                                "audio_path": str(audio)}) + "\n"
            lines = iter(['{"type":"header","schema_version":1}\n'] + [track] * 10_001)
            self.assertEqual(validate_records(lines, root), 10_001)

    def test_invalid_header_and_timeout(self):
        with self.assertRaises(RuntimeError):
            validate_records(io.StringIO("{}\n"), Path("/unused"))
        self.assertEqual(positive_timeout("600"), 600)
        for value in ("0", "-1", "nan", "inf"):
            with self.assertRaises(argparse.ArgumentTypeError):
                positive_timeout(value)


if __name__ == "__main__":
    unittest.main()
