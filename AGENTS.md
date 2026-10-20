# Project workflow

Follow [DEVELOPMENT.md](DEVELOPMENT.md) for the development workflow and release checklist.
Keep the README focused on users and basic developer setup; maintain process policy in
DEVELOPMENT.md instead.

## Packaging policy

- Do not build AppImages or Arch packages after ordinary features, fixes, refactors,
  documentation updates, or successful native tests. Use `make test` and relevant
  synthetic tests for routine validation.
- Package when the user explicitly requests a package/release, approves a release
  milestone, or asks to validate packaging, installation, portability, or a packaged
  regression. A completed feature or milestone alone is not approval to package.
- Packaging scripts, bundled runtimes, dependency versions/ABIs, indexer discovery,
  and install-layout changes are reasons to recommend targeted package validation.
  If that validation was not requested, explain the need and ask before building.
- Build only the affected/requested format. Do not rebuild both formats by default.
  Prefer `make test-appimage` or `make test-arch-package` for a validated candidate;
  these already build their packages, so do not run a separate package build first.
- Before a release candidate, run native tests and any relevant synthetic indexer
  tests, review version/package revision and release notes, then run the chosen
  package validation. Keep displayed output, saved logs, and failure status intact.
- Report whether packages were rebuilt and which validation passed. Existing artifacts
  are not evidence that current source changes are packaged. Never describe a failed
  or unvalidated candidate as a verified release.
- Never launch osu! or scan the real library as part of packaging validation. Use
  synthetic fixtures. Do not install packages, publish, commit, tag, push, prune global
  Docker data, or remove old artifacts without separate authorization.
- Public distribution remains blocked on application-license selection and dependency/
  source-compliance review; a local package build is not publication approval.
