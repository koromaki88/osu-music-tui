"""Build/export the Debian-baseline image with Docker or Podman (no buildx needed)."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import uuid


ROOT = Path(__file__).resolve().parent.parent
FILES = ("osu-player-x86_64.AppImage", "osu-player-x86_64.AppImage.sha256",
         "osu-player-x86_64.AppImage.manifest.json")
TOOLCHAIN = "osu-player-appimage-toolchain:net10-debian12"
SMOKE_IMAGE = "debian:bookworm-slim@sha256:a4672c0cb26fbdde88e38fa2dfb6c681942306680e41e4378b28770b6e79ee91"


def source_snapshot(root, destination):
    # Explicit allowlist, independent of Docker ignore semantics. Never mount the
    # working tree itself: it may contain SDKs, artifacts, or the game executable.
    patterns = ("Makefile", "VERSION", "README.md", "DEVELOPMENT.md", "CHANGELOG.md", "docs/*.md", "src/*.cpp", "src/*.hpp", "include/osu_player/*.hpp",
                "tests/*.py", "tests/*.cpp", "tests/*.hpp", "tests/fixtures/*.ndjson",
                "tests/osu-indexer-tests/*.cs", "tests/osu-indexer-tests/*.csproj",
                "tests/osu-indexer-tests/packages.lock.json", "tools/package_appimage.py",
                 "tools/build_appimage.py", "tools/build_arch_package.py", "tools/install_dotnet.py",
                 "tools/check_ca_bundle.py",
                "tools/run_appimage_build.py",
                "tools/osu-indexer/*.cs", "tools/osu-indexer/*.csproj",
                "tools/osu-indexer/packages*.lock.json", "packaging/AppRun", "packaging/Dockerfile", "packaging/*.md",
                "packaging/*.svg", "packaging/*.desktop", "packaging/arch/PKGBUILD",
                "packaging/arch/LICENSE-NOTICE")
    for path in sorted({path for pattern in patterns for path in root.glob(pattern)}):
        if path.is_file() and not path.is_symlink():
            target = destination / path.relative_to(root)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)


def run_container(engine, image, mounts, command):
    name = "osu-player-build-" + uuid.uuid4().hex
    arguments = [engine, "run", "--rm", "--name", name, "--platform", "linux/amd64",
                 "--user", f"{os.getuid()}:{os.getgid()}", "--workdir", "/tmp"]
    if Path(engine).name == "podman":
        arguments += ["--userns=keep-id"]
    for host, guest, mode in mounts:
        arguments += ["--volume", f"{host}:{guest}:{mode}"]
    try:
        subprocess.run([*arguments, image, *command], check=True)
    finally:
        # --rm handles successful exits. Explicit cleanup also covers build
        # failures and interruption; the generated name belongs only to this run.
        subprocess.run([engine, "rm", "--force", name], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def build(engine, output):
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="appimage-build-", dir=output) as directory:
        work = Path(directory)
        context = work / "toolchain"
        (context / "tools").mkdir(parents=True)
        shutil.copy2(ROOT / "packaging/Dockerfile", context / "Dockerfile")
        shutil.copy2(ROOT / "tools/install_dotnet.py", context / "tools/install_dotnet.py")
        # Only two stable toolchain files enter Docker's layer cache. Source
        # edits cannot create new cached compilation/test/package layers.
        subprocess.run([engine, "build", "--force-rm", "--platform", "linux/amd64",
                        "--tag", TOOLCHAIN, str(context)], check=True)
        source = work / "source"
        source_snapshot(ROOT, source)
        staging = work / "output"
        staging.mkdir()
        run_container(engine, TOOLCHAIN, [(source, "/input", "ro"), (staging, "/output", "rw")],
                      ["python3", "/input/tools/run_appimage_build.py", "--output", "/output"])
        # No SDK or application libraries are installed in the smoke image.
        run_container(engine, SMOKE_IMAGE, [(staging, "/input", "ro")], ["sh", "-eu", "-c", """
cd "$(mktemp -d)"
/input/osu-player-x86_64.AppImage --appimage-extract-and-run --help
/input/osu-player-x86_64.AppImage --appimage-extract > /dev/null
LD_LIBRARY_PATH="$PWD/squashfs-root/usr/lib" \
  ./squashfs-root/usr/lib/osu-player/indexer/osu-indexer --osu-dir /input/fixture > library.ndjson
./squashfs-root/AppRun library library.ndjson
# Check fallback selection on this deliberately CA-less minimal image, without
# replacing anything in the candidate or making any external network request.
mkdir -p trust-check/usr/bin trust-check/usr/share/osu-player
cp squashfs-root/AppRun trust-check/AppRun
cp squashfs-root/usr/share/osu-player/ca-certificates.crt trust-check/usr/share/osu-player/
cat > trust-check/usr/bin/osu-player <<'CHECK'
#!/bin/sh
test "$SSL_CERT_FILE" = "$APPDIR/usr/share/osu-player/ca-certificates.crt"
test -s "$SSL_CERT_FILE"
CHECK
chmod +x trust-check/usr/bin/osu-player
SSL_CERT_FILE= ./trust-check/AppRun
"""])
        with (staging / FILES[0]).open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        if (staging / FILES[1]).read_text().split()[0] != digest:
            raise RuntimeError("exported AppImage checksum does not match")
        manifest = json.loads((staging / FILES[2]).read_text())
        if manifest["architecture"] != "x86_64" or tuple(map(int, manifest["glibc_minimum"].split("."))) > (2, 36):
            raise RuntimeError("container artifact does not meet the declared x86-64/glibc 2.36 baseline")
        for name in FILES:
            (staging / name).replace(output / name)
        print(f"Standalone AppImage: {output / FILES[0]} (glibc >= {manifest['glibc_minimum']})", flush=True)


def cleanup(engine):
    # No global prune: remove only the exact image created by this project.
    subprocess.run([engine, "image", "rm", TOOLCHAIN], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", default="docker")
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/dist")
    parser.add_argument("--clean-toolchain", action="store_true",
                        help="remove only this project's reusable toolchain image")
    options = parser.parse_args()
    try:
        if options.clean_toolchain:
            cleanup(options.engine)
        else:
            build(options.engine, options.output.resolve())
    except KeyboardInterrupt:
        parser.exit(130, "AppImage build interrupted; disposable containers cleaned up\n")
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Container AppImage build failed: {error}\n")


def interrupted(signum, frame):
    raise KeyboardInterrupt


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, interrupted)
    main()
