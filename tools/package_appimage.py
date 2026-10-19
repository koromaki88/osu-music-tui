"""Build a relocatable Linux x86-64 AppImage; never reads a user's osu! library."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile
import urllib.request


ROOT = Path(__file__).resolve().parent.parent
TOOL_URL = "https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage"
TOOL_SHA256 = "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
RUNTIME_VERSION = "10.0.13"
GLIBC_LIBRARIES = {
    "libc.so.6", "libm.so.6", "libmvec.so.1", "libdl.so.2", "libpthread.so.0",
    "librt.so.1", "libresolv.so.2", "libutil.so.1", "libanl.so.1",
}


def run(arguments, **kwargs):
    return subprocess.run([str(argument) for argument in arguments], check=True, **kwargs)


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def fetch_tool():
    directory = ROOT / ".tools/appimage"
    directory.mkdir(parents=True, exist_ok=True)
    tool = directory / "appimagetool-1.9.1-x86_64.AppImage"
    if not tool.exists():
        print("Downloading pinned appimagetool 1.9.1...", flush=True)
        with tempfile.TemporaryDirectory(prefix="download-", dir=directory) as temporary:
            staging = Path(temporary) / "appimagetool"
            with urllib.request.urlopen(TOOL_URL, timeout=120) as response, staging.open("wb") as output:
                shutil.copyfileobj(response, output)
            if sha256(staging) != TOOL_SHA256:
                raise RuntimeError("appimagetool checksum mismatch; refusing to execute download")
            staging.replace(tool)
    if sha256(tool) != TOOL_SHA256:
        raise RuntimeError(f"appimagetool checksum mismatch: {tool}")
    tool.chmod(0o755)
    return tool


def is_elf(path):
    with path.open("rb") as stream:
        return stream.read(4) == b"\x7fELF"


def excluded_library(name):
    return name in GLIBC_LIBRARIES or name.startswith(("ld-linux", "libnss_"))


def dependencies(path):
    # Only the player we built, trusted .NET/Realm files, and system libraries
    # enter ldd. Never use this routine on untrusted downloaded/user media.
    result = subprocess.run(["ldd", str(path)], text=True, capture_output=True,
                            env=dict(os.environ, LC_ALL="C", LD_LIBRARY_PATH="", LD_PRELOAD=""))
    output = result.stdout + result.stderr
    if "=> not found" in output:
        raise RuntimeError(f"unresolved build dependency for {path}:\n{output}")
    if result.returncode and not any(text in output for text in ("not a dynamic executable", "statically linked")):
        raise RuntimeError(f"ldd failed for {path}:\n{output}")
    found = {}
    for line in output.splitlines():
        match = re.match(r"\s*(\S+) => (/\S+) \(", line)
        if match:
            name, location = match.groups()
            if "/" in name:
                if excluded_library(Path(name).name):
                    continue  # ldd can print the loader's absolute path as its name.
                raise RuntimeError(f"cannot relocate an absolute/path-based native dependency: {name}")
            if name.startswith(("libasan.", "libubsan.", "libtsan.")):
                raise RuntimeError("refusing to package a sanitizer build; use make release")
            if not excluded_library(name):
                found[name] = Path(location).resolve()
    return found


def dynamic_audio_libraries():
    # SDL dlopens its backends. Recent Arch SDL2 is sdl2-compat and also dlopens
    # SDL3, so its absence would not show in the player's ordinary ldd output.
    result = run(["ldconfig", "-p"], text=True, capture_output=True)
    available = {}
    for line in result.stdout.splitlines():
        match = re.match(r"\s*(\S+) .* => (/.+)", line)
        if match:
            name, path = match.groups()
            available.setdefault(name, Path(path).resolve())
    return {name: available[name] for name in (
        "libSDL3.so.0", "libpulse.so.0", "libasound.so.2", "libpipewire-0.3.so.0",
        "libssl.so.3", "libcrypto.so.3",
    ) if name in available}


def package_owner(path):
    if shutil.which("pacman"):
        result = subprocess.run(["pacman", "-Qoq", str(path)], capture_output=True, text=True)
        if result.returncode == 0:
            name = result.stdout.strip().splitlines()[0]
            version = run(["pacman", "-Q", name], capture_output=True, text=True).stdout.strip()
            return name, version, Path("/usr/share/licenses") / name
    elif shutil.which("dpkg-query"):
        result = subprocess.run(["dpkg-query", "-S", str(path)], capture_output=True, text=True)
        if result.returncode == 0:
            name = result.stdout.split(": ", 1)[0]
            version = run(["dpkg-query", "-W", "-f=${Package} ${Version}", name],
                          capture_output=True, text=True).stdout.strip()
            return name, version, Path("/usr/share/doc") / name.split(":")[0] / "copyright"
    return None, None, None


def bundle_libraries(appdir, player, indexer):
    seeds = [player, *[path for path in indexer.iterdir() if path.is_file() and is_elf(path)]]
    libraries = dynamic_audio_libraries()
    for seed in seeds:
        libraries.update(dependencies(seed))
    pending = list(libraries.items())
    visited = set()
    while pending:
        name, source = pending.pop()
        if name in visited:
            continue
        visited.add(name)
        for dependency, location in dependencies(source).items():
            if dependency in libraries and libraries[dependency] != location:
                raise RuntimeError(f"conflicting native dependency: {dependency}")
            if dependency not in libraries:
                libraries[dependency] = location
                pending.append((dependency, location))
    libdir = appdir / "usr/lib"
    licenses = appdir / "usr/share/licenses"
    licenses.mkdir(parents=True)
    manifest = []
    copied_licenses = set()
    for name, source in sorted(libraries.items()):
        shutil.copy2(source, libdir / name)
        owner, version, license_path = package_owner(source)
        if owner and owner not in copied_licenses and license_path.exists():
            target = licenses / owner
            if license_path.is_dir():
                shutil.copytree(license_path, target, symlinks=False)
            else:
                target.mkdir()
                shutil.copy2(license_path, target / "copyright")
            copied_licenses.add(owner)
        manifest.append({"soname": name, "source": str(source), "sha256": sha256(source),
                         "package": version, "license_copied": owner in copied_licenses})
    for source, name in (("/usr/share/licenses/common", "common"),
                         ("/usr/share/common-licenses", "common-debian")):
        common = Path(source)
        if common.exists():
            shutil.copytree(common, licenses / name, symlinks=False)
    return manifest


def copy_notices(appdir, publish):
    destination = appdir / "usr/share/licenses/dotnet-and-nuget"
    destination.mkdir(parents=True)
    for path in publish.iterdir():
        if any(word in path.name.lower() for word in ("license", "notice")):
            shutil.copy2(path, destination / path.name)
    cache = Path(os.environ.get("NUGET_PACKAGES", str(Path.home() / ".nuget/packages")))
    lock = json.loads((ROOT / "tools/osu-indexer/packages.portable.lock.json").read_text())
    packages = {}
    for group in lock["dependencies"].values():
        for name, details in group.items():
            packages[name.lower()] = details["resolved"]
    packages["microsoft.netcore.app.runtime.linux-x64"] = RUNTIME_VERSION
    for name, version in packages.items():
        source = cache / name / version
        if not source.exists():
            continue
        for path in source.iterdir():
            if path.is_file() and (path.suffix == ".nuspec" or
                                  any(word in path.name.lower() for word in ("license", "notice", "copying"))):
                target = destination / f"{name}-{version}"
                target.mkdir(exist_ok=True)
                shutil.copy2(path, target / path.name)
    shutil.copy2(ROOT / "packaging/THIRD_PARTY.md", appdir / "usr/share/licenses/THIRD_PARTY.md")


def glibc_requirement(appdir):
    versions = set()
    for folder in (appdir / "usr/bin", appdir / "usr/lib"):
        for path in folder.rglob("*"):
            if path.is_file() and is_elf(path):
                output = run(["readelf", "--version-info", path], capture_output=True, text=True).stdout
                versions.update(re.findall(r"\bGLIBC_(\d+\.\d+(?:\.\d+)?)\b", output))
    return max(versions, key=lambda value: tuple(map(int, value.split("."))))


def build(options):
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        raise RuntimeError("this initial AppImage builder supports native Linux x86-64 only")
    for name in ("ldd", "ldconfig", "readelf", "file", "desktop-file-validate"):
        if not shutil.which(name):
            raise RuntimeError(f"missing packaging dependency: {name}")
    player = Path(options.player_binary).resolve()
    dotnet = shutil.which(options.dotnet) or str(Path(options.dotnet).resolve())
    output = Path(options.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    tool_image = fetch_tool()
    with tempfile.TemporaryDirectory(prefix="appimage-build-", dir=output.parent) as directory:
        work = Path(directory)
        appdir = work / "osu-player.AppDir"
        (appdir / "usr/bin").mkdir(parents=True)
        (appdir / "usr/lib").mkdir()
        shutil.copy2(player, appdir / "usr/bin/osu-player")
        print("Publishing self-contained .NET indexer...", flush=True)
        project = ROOT / "tools/osu-indexer"
        properties = ["--runtime", "linux-x64", "--property:SelfContained=true",
                      f"--property:RuntimeFrameworkVersion={RUNTIME_VERSION}",
                      "--property:NuGetLockFilePath=packages.portable.lock.json"]
        run([dotnet, "restore", project, "--locked-mode", *properties], cwd=ROOT)
        indexer = appdir / "usr/lib/osu-player/indexer"
        run([dotnet, "publish", project, "--configuration", "Release", "--no-restore",
             "--output", indexer, *properties, "--property:PublishTrimmed=false",
             "--property:PublishSingleFile=false", "--property:InvariantGlobalization=true",
             "--property:DebugType=None", "--property:DebugSymbols=false"], cwd=ROOT)
        # Optional .NET LTTng tracing needs a separate host liblttng ABI. It is
        # unrelated to indexing and isn't part of this standalone distribution.
        (indexer / "libcoreclrtraceptprovider.so").unlink(missing_ok=True)
        libraries = bundle_libraries(appdir, player, indexer)
        print(f"Bundled {len(libraries)} native libraries (excluding glibc/loader).", flush=True)
        for source in (Path("/lib/terminfo"), Path("/usr/share/terminfo")):
            if source.is_dir():
                shutil.copytree(source, appdir / "usr/share/terminfo", dirs_exist_ok=True, symlinks=False)
        for name in ("AppRun", "osu-player.desktop", "osu-player.svg"):
            shutil.copy2(ROOT / "packaging" / name, appdir / name)
        (appdir / "AppRun").chmod(0o755)
        copy_notices(appdir, indexer)
        certificates = appdir / "usr/share/osu-player/ca-certificates.crt"
        certificates.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2("/etc/ssl/certs/ca-certificates.crt", certificates)
        manifest = {"version": "0.4.0", "architecture": "x86_64", "glibc_minimum": glibc_requirement(appdir),
                    "ca_bundle_sha256": sha256(certificates),
                    "dotnet_runtime": RUNTIME_VERSION, "appimagetool_sha256": TOOL_SHA256,
                    "libraries": libraries,
                    "host_requirements": ["Linux x86-64", "compatible glibc and /bin/sh",
                                          "audio services/drivers", "UTF-8 locale"]}
        manifest_text = json.dumps(manifest, indent=2) + "\n"
        info = appdir / "usr/share/osu-player"
        info.mkdir(parents=True, exist_ok=True)
        (info / "build-manifest.json").write_text(manifest_text)
        shutil.copy2(ROOT / "README.md", info / "README.md")
        for name in ("CHANGELOG.md", "DEVELOPMENT.md"):
            shutil.copy2(ROOT / name, info / name)
        # Reuse the runtime in the checksum-pinned tool image. Otherwise
        # appimagetool downloads an unpinned continuous runtime during each build.
        offset = int(run([tool_image, "--appimage-offset"], capture_output=True, text=True).stdout)
        runtime = work / "runtime-x86_64"
        with tool_image.open("rb") as stream:
            runtime.write_bytes(stream.read(offset))
        run([tool_image, "--appimage-extract"], cwd=work, stdout=subprocess.DEVNULL)
        packed = work / "osu-player-x86_64.AppImage"
        run([work / "squashfs-root/AppRun", "--no-appstream", "--runtime-file", runtime,
             "--mksquashfs-opt", "-processors", "--mksquashfs-opt", "2", appdir, packed],
              env=dict(os.environ, ARCH="x86_64", VERSION="0.4.0"))
        packed.chmod(0o755)
        os.replace(packed, output)
        output.with_suffix(output.suffix + ".sha256").write_text(f"{sha256(output)}  {output.name}\n")
        output.with_suffix(output.suffix + ".manifest.json").write_text(manifest_text)
        print(f"Built {output} ({output.stat().st_size / 1024**2:.1f} MiB); requires glibc >= {manifest['glibc_minimum']}.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player-binary", default="artifacts/release/osu-player")
    parser.add_argument("--dotnet", default="dotnet")
    parser.add_argument("--output", default="artifacts/dist/osu-player-x86_64.AppImage")
    options = parser.parse_args()
    try:
        build(options)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"AppImage build failed: {error}\n")


if __name__ == "__main__":
    main()
