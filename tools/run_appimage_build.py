"""Container-side disposable build; sources arrive via a read-only snapshot mount."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    source = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="osu-appimage-build-") as temporary:
        work = Path(temporary)
        root = work / "source"
        shutil.copytree(source, root)
        home = work / "home"
        home.mkdir()
        environment = dict(os.environ, HOME=str(home), DOTNET_CLI_TELEMETRY_OPTOUT="1",
                           DOTNET_SKIP_FIRST_TIME_EXPERIENCE="1")

        def run(arguments):
            subprocess.run(arguments, cwd=root, env=environment, check=True)

        run(["make", "-j2", "test"])
        run(["make", "test-indexer", "DOTNET=dotnet"])
        run(["dotnet", "run", "--project", "tests/osu-indexer-tests", "--configuration", "Release",
             "--no-build", "--no-restore", "--", "--fixture-output", str(options.output / "fixture")])
        run(["make", "appimage-local", "DOTNET=dotnet"])
        run(["python3", "tests/appimage_integration.py", "--dotnet", "dotnet"])
        for path in (root / "artifacts/dist").iterdir():
            if path.is_file():
                shutil.copy2(path, options.output / path.name)


if __name__ == "__main__":
    main()
