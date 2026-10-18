"""Fast packaging safety checks; no downloads, .NET, containers, or real library."""

import importlib.util
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "package_appimage", Path(__file__).resolve().parent.parent / "tools/package_appimage.py")
packaging = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packaging)
builder_spec = importlib.util.spec_from_file_location(
    "build_appimage", Path(__file__).resolve().parent.parent / "tools/build_appimage.py")
builder = importlib.util.module_from_spec(builder_spec)
builder_spec.loader.exec_module(builder)


class PackagingTests(unittest.TestCase):
    def parse(self, output, returncode=0):
        completed = subprocess.CompletedProcess([], returncode, stdout=output, stderr="")
        with patch.object(packaging.subprocess, "run", return_value=completed):
            return packaging.dependencies(Path("/trusted/player"))

    def test_native_libraries_exclude_glibc_and_absolute_loader(self):
        result = self.parse("""\
        libavcodec.so.59 => /usr/lib/libavcodec.so.59 (0x123)
        libc.so.6 => /usr/lib/libc.so.6 (0x456)
        /lib64/ld-linux-x86-64.so.2 => /usr/lib/ld-linux-x86-64.so.2 (0x789)
        """)
        self.assertEqual(set(result), {"libavcodec.so.59"})
        self.assertTrue(all("/" not in name for name in result))

    def test_missing_or_path_based_dependencies_are_fatal(self):
        for output in ("libfoo.so.1 => not found", "/some/libfoo.so => /usr/lib/libfoo.so (0x123)"):
            with self.assertRaises(RuntimeError):
                self.parse(output)

    def test_sanitizer_build_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "sanitizer"):
            self.parse("libasan.so.8 => /usr/lib/libasan.so.8 (0x123)")

    def test_cached_tool_must_match_pinned_checksum(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = root / ".tools/appimage"
            directory.mkdir(parents=True)
            (directory / "appimagetool-1.9.1-x86_64.AppImage").write_bytes(b"not a trusted tool")
            with patch.object(packaging, "ROOT", root), patch.object(packaging.urllib.request, "urlopen") as download:
                with self.assertRaisesRegex(RuntimeError, "checksum mismatch"):
                    packaging.fetch_tool()
                download.assert_not_called()

    def test_container_export_enforces_baseline_and_preserves_previous_artifact(self):
        for glibc, accepted in (("2.36", True), ("2.44", False)):
            with self.subTest(glibc=glibc), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary)
                previous = output / builder.FILES[0]
                previous.write_bytes(b"previous working image")
                payload = b"new image"
                files = {builder.FILES[0]: payload,
                         builder.FILES[1]: hashlib.sha256(payload).hexdigest().encode() + b"  image\n",
                         builder.FILES[2]: json.dumps({"architecture": "x86_64", "glibc_minimum": glibc}).encode()}

                def engine(arguments, **kwargs):
                    if arguments[1] == "cp":
                        destination = Path(arguments[-1])
                        destination.write_bytes(files[destination.name])
                    return subprocess.CompletedProcess(arguments, 0)

                with patch.object(builder.subprocess, "run", side_effect=engine) as commands, \
                        patch.object(builder.subprocess, "check_output", return_value="private-container\n"), \
                        patch("builtins.print"):
                    if accepted:
                        builder.build("docker", output)
                        self.assertEqual(previous.read_bytes(), payload)
                    else:
                        with self.assertRaisesRegex(RuntimeError, "baseline"):
                            builder.build("docker", output)
                        self.assertEqual(previous.read_bytes(), b"previous working image")
                    self.assertTrue(any(call.args[0][1:3] == ["rm", "private-container"]
                                        for call in commands.call_args_list))


if __name__ == "__main__":
    unittest.main()
