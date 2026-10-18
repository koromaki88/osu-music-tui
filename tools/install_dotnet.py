"""Install the pinned, checksum-verified .NET 10 SDK into a private directory."""

import argparse
import hashlib
from pathlib import Path
import platform
import shutil
import tarfile
import tempfile
import urllib.request


SDK_VERSION = "10.0.402"
SDK_SHA512 = "1904a98dda2db34ba78ba3c4fb1b224455329af91c341d83547843c1839e112b04ae699ee92fa2449a8f62249196a938723078c0c60b9f65c88539d5687729c2"
SDK_URL = f"https://builds.dotnet.microsoft.com/dotnet/Sdk/{SDK_VERSION}/dotnet-sdk-{SDK_VERSION}-linux-x64.tar.gz"


def extract_sdk(archive, destination):
    # Debian 12's Python 3.11 lacks tarfile's newer extraction filter. This SDK
    # contains only directories/regular files: reject links and special entries,
    # and validate all destinations before extracting anything on either Python.
    root = destination.resolve()
    with tarfile.open(archive) as package:
        members = package.getmembers()
        for member in members:
            if not (member.isfile() or member.isdir()) or Path(member.name).is_absolute() or \
                    not (root / member.name).resolve().is_relative_to(root):
                raise RuntimeError(f"unsafe SDK archive entry: {member.name}")
        filters = {"filter": "data"} if hasattr(tarfile, "data_filter") else {}
        package.extractall(root, members=members, **filters)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, default=Path(".tools/dotnet10"))
    options = parser.parse_args()
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        parser.error("this SDK bootstrap supports Linux x86-64 only")
    destination = options.destination.resolve()
    if destination.exists() and any(destination.iterdir()):
        parser.error("destination must be empty; existing SDKs are never overwritten")
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="dotnet-download-", dir=destination.parent) as temporary:
        archive = Path(temporary) / "sdk.tar.gz"
        with urllib.request.urlopen(SDK_URL, timeout=120) as response, archive.open("wb") as stream:
            shutil.copyfileobj(response, stream)
        with archive.open("rb") as stream:
            if hashlib.file_digest(stream, "sha512").hexdigest() != SDK_SHA512:
                raise RuntimeError("SDK checksum mismatch; refusing to extract download")
        extract_sdk(archive, destination)
    print(f"Installed .NET SDK {SDK_VERSION} in {destination}")


if __name__ == "__main__":
    main()
