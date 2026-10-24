# Development

[README](README.md) · [Architecture](docs/ARCHITECTURE.md) · [Packaging](docs/PACKAGING.md) · [Release validation](docs/RELEASES.md)

## Routine workflow

1. Make a focused change and add relevant tests.
2. Run `make test`; also run `make test-indexer` when indexer behavior changes.
3. Update the relevant documentation and report validation results. State whether
   packages were rebuilt: existing artifacts may not contain current source changes.

> [!IMPORTANT]
> Use synthetic fixtures, fake services, and dummy audio. Do not launch osu! or scan
> the real library for routine validation. Never modify lazer's database or content.

### Where documentation belongs

| Content | Location |
|---|---|
| User instructions | README and usage guides |
| Workflow and process policy | This document |
| Implementation details | Architecture guide |
| Release history | Changelog |
| Validation evidence | Release record, not feature instructions |

## Tests

| Command | Scope | Log under `artifacts/` |
|---|---|---|
| `make test` | Native tests, Python safety, dummy audio, PTY/refresh, fake Discord, isolated Last.fm HTTPS | `tests.log` |
| `make test-indexer` | Synthetic Realm fixtures | `test-indexer.log` |
| `make test-appimage` | Build and validate AppImage | `test-appimage.log` |
| `make test-arch-package` | Build and validate extracted Arch package | `test-arch-package.log` |
| `make test-integration` | Explicitly authorized real-library verification only | `test-integration.log` |

Targets display stdout/stderr and save logs, preserving failures with `pipefail` and
`tee`. Each run replaces its log; preserve important earlier logs before rerunning.
Override the directory with `TEST_LOG_DIR=/path/to/logs`. Logs/artifacts are gitignored.
`make test` checks packaging helpers but does **not** build packages.

The synthetic OBS overlay test uses the fixed loopback port 7270. Close an enabled
overlay before running the suite; tests fail rather than stop an existing service.

Coverage and test-file locations are described in [Architecture](docs/ARCHITECTURE.md).

### Sanitizers and CMake

Clean before switching Make build modes; these commands remove generated C++ outputs:

```bash
make clean
make SANITIZE=1 test
# Restore ordinary binaries:
make clean
make
```

Alternative CMake build (not yet validated here; CMake/Ninja were unavailable):

```bash
cmake -S . -B build -G Ninja -DOSU_PLAYER_ENABLE_SANITIZERS=ON
cmake --build build
bash -o pipefail -c 'ctest --test-dir build --output-on-failure 2>&1 | tee build/ctest.log'
```

### Real-library verification: opt-in only

Obtain explicit authorization and close lazer first. This is not a routine test:

```bash
make indexer
make test-integration OSU_DIR=/path/to/osu-data
# Expensive: hash the entire source library before and after indexing:
make test-integration OSU_DIR=/path/to/osu-data INTEGRATION_VERIFY=full INTEGRATION_TIMEOUT=900
```

Quick mode checks Realm/config control files, root entries, and exported audio-path
existence—not proof that every audio file stayed unchanged. Full mode reads the
library twice. The default 300-second timeout applies per subprocess, not to hashing
or the entire test. Output shows progress and at most five song titles.

## Packaging policy

> [!IMPORTANT]
> Do not package ordinary features, fixes, refactors, or documentation changes.
> A completed milestone is a reason to propose a release, not permission to build one.

- **Approval:** build when the user requests a package/release, approves a release
  milestone, or asks to validate packaging, installation, portability, or a packaged regression.
- **Recommend validation:** packaging scripts, bundled runtimes, dependencies/ABIs,
  indexer discovery, and install-layout changes warrant targeted checks. Obtain approval first.

Build only the requested/affected format. Prefer `make test-appimage` or
`make test-arch-package`: these already build the package, so do not build it separately.
See [Packaging](docs/PACKAGING.md) for requirements and commands.

### Release checklist

1. Run native and relevant synthetic indexer tests. Never launch osu! or scan the
   real library as part of package validation.
2. Review version/package revision and release notes. An Arch packaging-only rebuild
   needs an appropriate package revision.
3. Run the approved format's package validation. Retain displayed output, saved logs,
   failure status, checksums, and manifests. Preserve old artifacts.
4. Record output paths, coverage, and limitations in [Release validation](docs/RELEASES.md).
   Never call a failed or unvalidated candidate verified.

### Separate approvals

- Packaging does **not** authorize installation, publication, commits, tags, pushes,
  deleting old artifacts, or global Docker cleanup. Each requires separate approval.
- Public distribution remains blocked on application-license selection and
  dependency/source compliance review; see [third-party notices](packaging/THIRD_PARTY.md).
