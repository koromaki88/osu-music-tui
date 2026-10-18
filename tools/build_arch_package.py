"""Build a native pacman package from an allowlisted working-tree snapshot."""

import argparse
import hashlib
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import tempfile


ROOT = Path(__file__).resolve().parent.parent
NAME = "osu-music-player-0.1.0"


def source_files(root):
    patterns = ("Makefile", "README.md", "src/*.cpp", "include/osu_player/*.hpp",
                "tests/*.cpp", "tests/*.hpp", "tests/*.py", "tests/fixtures/*.ndjson",
                "tests/osu-indexer-tests/*.cs", "tests/osu-indexer-tests/*.csproj",
                "tests/osu-indexer-tests/packages.lock.json",
                "tools/osu-indexer/*.cs", "tools/osu-indexer/*.csproj",
                "tools/osu-indexer/packages*.lock.json", "tools/*appimage.py",
                "packaging/osu-player.desktop", "packaging/osu-player.svg",
                "packaging/arch/LICENSE-NOTICE", "packaging/arch/PKGBUILD",
                "tools/build_arch_package.py", "tools/install_dotnet.py",
                "tools/run_appimage_build.py", "packaging/Dockerfile")
    return sorted({path for pattern in patterns for path in root.glob(pattern)
                   if path.is_file() and not path.is_symlink()})


def snapshot(root, destination):
    archive = destination / f"{NAME}.tar.gz"
    with tarfile.open(archive, "w:gz") as output:
        for path in source_files(root):
            output.add(path, arcname=str(Path(NAME) / path.relative_to(root)), recursive=False)
    with archive.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    recipe = (root / "packaging/arch/PKGBUILD").read_text()
    (destination / "PKGBUILD").write_text(recipe.replace("@SOURCE_SHA256@", digest))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/arch")
    parser.add_argument("--dotnet", default="dotnet")
    parser.add_argument("--nodeps", action="store_true",
                        help="skip makepkg dependency checks (controlled developer testing only)")
    options = parser.parse_args()
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        parser.error("this native package builder supports Linux x86-64 only")
    if not shutil.which("makepkg"):
        parser.error("makepkg is required; run on Arch Linux with base-devel installed")
    if os.getuid() == 0:
        parser.error("makepkg must run as an unprivileged user, not root")
    options.output = options.output.resolve()
    options.output.mkdir(parents=True, exist_ok=True)
    located_dotnet = shutil.which(options.dotnet)
    if not located_dotnet:
        parser.error(".NET 10 SDK is required; install dotnet-sdk-10.0 or pass --dotnet EXECUTABLE")
    dotnet = str(Path(located_dotnet).resolve())
    try:
        with tempfile.TemporaryDirectory(prefix="build-", dir=options.output) as temporary:
            directory = Path(temporary)
            snapshot(ROOT, directory)
            environment = dict(os.environ, DOTNET=dotnet, DOTNET_CLI_TELEMETRY_OPTOUT="1",
                               PKGDEST=str(directory), SRCDEST=str(directory),
                               BUILDDIR=str(directory), LOGDEST=str(directory))
            subprocess.run(["makepkg", "--cleanbuild", "--force", *(["--nodeps"] if options.nodeps else [])],
                           cwd=directory, env=environment, check=True)
            packages = list(directory.glob("*.pkg.tar.zst"))
            if not packages:
                raise RuntimeError("makepkg did not produce a .pkg.tar.zst package")
            for package in packages:
                target = options.output / package.name
                package.replace(target)
                with target.open("rb") as stream:
                    digest = hashlib.file_digest(stream, "sha256").hexdigest()
                target.with_suffix(target.suffix + ".sha256").write_text(f"{digest}  {target.name}\n")
                print(f"Arch package: {target} ({target.stat().st_size / 1024**2:.1f} MiB)", flush=True)
            for name in ("PKGBUILD", f"{NAME}.tar.gz"):
                shutil.copy2(directory / name, options.output / name)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Arch package build failed: {error}\n")


if __name__ == "__main__":
    main()
