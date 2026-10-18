"""Build/export the Debian-baseline image with Docker or Podman (no buildx needed)."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import uuid


ROOT = Path(__file__).resolve().parent.parent
FILES = ("osu-player-x86_64.AppImage", "osu-player-x86_64.AppImage.sha256",
         "osu-player-x86_64.AppImage.manifest.json")


def build(engine, output):
    tag = "osu-player-appimage-build:" + uuid.uuid4().hex
    container = None
    built = False
    try:
        subprocess.run([engine, "build", "--force-rm", "--platform", "linux/amd64", "--file", "packaging/Dockerfile",
                        "--target", "artifact", "--tag", tag, "."], cwd=ROOT, check=True)
        built = True
        # Create but never start the scratch artifact container. This works with
        # classic Docker as well as Podman, without privileged mounts or buildx.
        container = subprocess.check_output([engine, "create", tag, "/not-executed"], text=True).strip()
        output.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="appimage-export-", dir=output) as directory:
            staging = Path(directory)
            for name in FILES:
                subprocess.run([engine, "cp", f"{container}:/{name}", str(staging / name)], check=True)
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
    finally:
        if container:
            subprocess.run([engine, "rm", container], stdout=subprocess.DEVNULL, check=False)
        if built:
            subprocess.run([engine, "image", "rm", tag], stdout=subprocess.DEVNULL, check=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", default="docker")
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/dist")
    options = parser.parse_args()
    try:
        build(options.engine, options.output.resolve())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Container AppImage build failed: {error}\n")


if __name__ == "__main__":
    main()
