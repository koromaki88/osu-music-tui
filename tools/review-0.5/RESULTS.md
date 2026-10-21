# Focused pre-0.5 collection review

This directory contains the one-off synthetic benchmark source and its recorded
results. It is not part of the player build or release. Remove this directory as a
unit when the evidence is no longer needed; production changes do not depend on it.
Raw local binaries, fixture JSON and logs live in `artifacts/pre-0.5-review/`.
No real library, osu! process, personal configuration, or live service was used.

## Method and results

`collections_bench.cpp` creates 50,000 synthetic long-metadata tracks, loads a
one-member favorite list, then measures three isolated operations. The before run
used the source before this review; both binaries used `g++ -O2 -DNDEBUG`.
Allocation counts/bytes come from replaceable C++ `new`/`new[]`, not C allocations,
RSS, peak live memory, or audio/backend memory. Each row's result was unchanged.

| Operation | Before allocations / bytes | After allocations / bytes | Before / after elapsed ms |
|---|---:|---:|---:|
| 10,000 favorite-marker checks | 10,000 / 670,000 | 0 / 0 | 0.385 / 0.177 |
| Resolve one reference against 50,000 tracks | 100,003 / 6,764,573 | 3 / 72 | 15.666 / 3.159 |
| 20 searches within a three-track scope | 2,000,360 / 146,652,080 | 140 / 7,600 | 234.205 / 0.014 |

These are single local timing samples, not general speedup or retained-RAM
guarantees. The allocation/work reductions are the reason for keeping the changes.
Resolution still scans the library: the improvement is bounded scratch storage and
avoiding full-library string-key construction, not constant-time lookup. Full-library
search is unchanged. Small scoped searches now do work proportional to scope size.

Reproduce the after run from the repository root (use a new fixture directory):

```sh
g++ -O2 -DNDEBUG -std=c++20 -pthread -Iinclude \
  $(pkg-config --cflags json-c) tools/review-0.5/collections_bench.cpp \
  src/collections.cpp src/collections_file.cpp src/collections_model.cpp \
  src/library.cpp src/library_view.cpp src/track.cpp src/storage_path.cpp \
  $(pkg-config --libs json-c) -o /tmp/opencode/collections-bench
/tmp/opencode/collections-bench /tmp/opencode/collections-bench-fixture
```

The fixture is intentionally synthetic and isolated. The executable writes its
`private/lists.json`; never point it at an actual configuration directory.

## Organization and compatibility

The collection worker, model/validation and file handling now have separate source
files. Panel content and pure playlist input routing are separated from the ncurses
frontend. Capacity limits are centralized in `collections.hpp`. The Make and CMake
source lists include the extracted modules. Storage schema, controls, queue snapshot
semantics and missing-reference retention are unchanged.

Duplicate adds/boundary reorders skip file replacement when serialized bytes are
unchanged, while still checking for external changes. UTF-8 validation on direct
edits replaces the old serialize-and-reparse validation pass; regression tests cover
overlong encodings, surrogates, truncation and values above Unicode's limit.

Release version and packages are unchanged. This review is not release validation
or permission to publish; the normal release checklist still applies.

Validation: `make -j4 test` passed, including synthetic collections/TUI/refresh,
dummy audio and fake integration services. Separate ASan/UBSan builds of the
collections and TUI model/panel tests passed. `git diff --check` passed. CMake
configuration/build could not be checked because CMake is not installed here;
no indexer behavior changed and no packages were rebuilt.
