"""Native packaging snapshot checks: no pacman, downloads, or game access."""

import hashlib
import importlib.util
from pathlib import Path
import tarfile
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "build_arch_package", Path(__file__).resolve().parent.parent / "tools/build_arch_package.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class ArchPackagingTests(unittest.TestCase):
    def test_snapshot_only_includes_sources_and_locks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "source"
            output = Path(directory) / "output"
            output.mkdir()
            files = {"Makefile": "all:", "src/main.cpp": "// source",
                     "tools/osu-indexer/packages.arch.lock.json": "{}",
                     "packaging/arch/PKGBUILD": "sha256sums=('@SOURCE_SHA256@')",
                     ".tools/dotnet/dotnet": "sdk", "osu.AppImage": "game",
                     "artifacts/private/client.realm": "private", "tests/__pycache__/x.pyc": "cache",
                     "tests/osu-indexer-tests/bin/Release/app.dll": "binary"}
            for name, content in files.items():
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content)
            (root / "src/private.cpp").symlink_to(root / "artifacts/private/client.realm")
            builder.snapshot(root, output)
            archive = output / f"{builder.NAME}.tar.gz"
            with tarfile.open(archive) as packaged:
                names = set(packaged.getnames())
            self.assertEqual(names, {f"{builder.NAME}/{name}" for name in (
                "Makefile", "src/main.cpp", "tools/osu-indexer/packages.arch.lock.json",
                "packaging/arch/PKGBUILD")})
            with archive.open("rb") as stream:
                digest = hashlib.file_digest(stream, "sha256").hexdigest()
            self.assertEqual((output / "PKGBUILD").read_text(), f"sha256sums=('{digest}')")


if __name__ == "__main__":
    unittest.main()
