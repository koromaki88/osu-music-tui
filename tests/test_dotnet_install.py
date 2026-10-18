"""Bootstrap safety checks; no network access or SDK execution."""

import importlib.util
import io
from pathlib import Path
import tempfile
import tarfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "install_dotnet", Path(__file__).resolve().parent.parent / "tools/install_dotnet.py")
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class DotnetInstallTests(unittest.TestCase):
    def test_archive_paths_and_links_are_rejected(self):
        for name, kind in (("../outside", tarfile.REGTYPE), ("/absolute", tarfile.REGTYPE),
                           ("link", tarfile.SYMTYPE)):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                destination = root / "sdk"
                destination.mkdir()
                archive = root / "test.tar"
                with tarfile.open(archive, "w") as output:
                    entry = tarfile.TarInfo(name)
                    entry.type = kind
                    entry.linkname = "../outside" if kind == tarfile.SYMTYPE else ""
                    output.addfile(entry)
                with self.assertRaisesRegex(RuntimeError, "unsafe SDK archive"):
                    installer.extract_sdk(archive, destination)
                self.assertEqual(list(destination.iterdir()), [])

    def test_wrong_checksum_never_extracts(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "sdk"
            with patch("sys.argv", ["install_dotnet", "--destination", str(destination)]), \
                    patch.object(installer.urllib.request, "urlopen", return_value=io.BytesIO(b"untrusted")), \
                    patch.object(installer.tarfile, "open") as extract:
                with self.assertRaisesRegex(RuntimeError, "checksum mismatch"):
                    installer.main()
                extract.assert_not_called()
                self.assertEqual(list(destination.iterdir()), [])

    def test_existing_sdk_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary)
            host = destination / "dotnet"
            host.write_text("existing SDK")
            with patch("sys.argv", ["install_dotnet", "--destination", str(destination)]), \
                    patch.object(installer.urllib.request, "urlopen") as download, \
                    patch("sys.stderr", new_callable=io.StringIO):
                with self.assertRaises(SystemExit):
                    installer.main()
                download.assert_not_called()
            self.assertEqual(host.read_text(), "existing SDK")


if __name__ == "__main__":
    unittest.main()
