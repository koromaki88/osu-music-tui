# Playback / Last.fm soak results — 2026-10-25

## Outcome

The accelerated synthetic test reproduced gradual RSS growth in the Last.fm
worker/persistence path with and without real HTTPS. Most of the growth was
allocator-reported free heap rather than a comparable rise in in-use memory.
Growth slowed near the bounded 4,096-receipt limit. This supports allocator
retention and bounded state as plausible explanations for the reported overnight
75 MB to 84 MB increase; it does not establish the cause of that individual run
or prove the absence of all leaks.

No production fixes or packages were made. `malloc_trim` was used only in the
disposable harness as a diagnostic, not added to the application.

## Measurements

Each ordinary run performed 4,500 replacements through real WAV/MP3/Ogg decoders
with SDL dummy output. Each Last.fm run also processed 4,500 synthetic eligible
sessions and 9,000 successful Now Playing/scrobble requests. Real HTTPS used a
generated trusted certificate through an isolated loopback CONNECT fixture;
hostname/certificate verification stayed enabled. No real Last.fm, user account,
library, game or OBS was used. The fixture recorded 9,000 HTTPS requests, zero errors.

All values below are **MiB**, not decimal MB. These harness RSS baselines are not
the full-player/library baseline and must not be compared directly with btop's
reported 75 MB.

| Mode | RSS at cycle 100 | RSS at 4,500 | In-use heap at 100 / 4,500 | Free heap at 100 / 4,500 |
|---|---:|---:|---:|---:|
| Audio only | 54.680 | 55.027 | 0.761 / 1.074 | 1.051 / 1.098 |
| Audio + fake Last.fm transport | 56.535 | 61.793 | 1.045 / 1.656 | 1.158 / 5.766 |
| Audio + real local HTTPS Last.fm | 59.121 | 64.195 | 1.502 / 2.120 | 1.232 / 5.763 |

From cycle 100 to 4,500, HTTPS RSS grew **5.074 MiB** while arena in-use grew
**0.619 MiB** and arena free grew **4.531 MiB**. Fake transport showed a similar
pattern: **5.258 MiB** RSS, **0.611 MiB** in-use and **4.608 MiB** free growth.
Therefore repeated TLS requests were not necessary to reproduce most gradual
growth; HTTPS added a baseline premium of roughly 2.4–2.6 MiB in this harness.

Between cycles 4,100 and 4,500, fake-transport RSS rose **0.066 MiB** and HTTPS RSS
rose **0.297 MiB**. This short post-cap interval suggests leveling off, but is not
a multi-night proof of a permanent plateau. Codec alternation also causes in-use
measurement oscillation; the small rises should not be treated as leak slopes.

| Mode | RSS after settling (backend still alive) | After worker/backend destruction | After diagnostic trim |
|---|---:|---:|---:|
| Audio only | 55.027 | 55.215 | 53.977 |
| Fake Last.fm | 61.793 | 61.656 | 55.824 |
| HTTPS Last.fm | 64.195 | 63.680 | 58.258 |

After destruction, trimming returned another **5.832 MiB** for fake transport and
**5.422 MiB** for HTTPS without restarting the process. In-use heap was unchanged
by trim. This is direct evidence of reclaimable allocator retention in these runs,
not a claim that all retained memory is reclaimable or that no live allocations leak.

## Code explanation

`LastfmQueue::finish` copies the bounded receipt deque before applying a
transactional save, caps it at 4,096, then replaces the old deque. `save` constructs
and frees a json-c tree and serialized string for the pending queue plus receipts
on each save. Enqueue also saves the full document. These temporary allocations
grow as receipts fill; after freeing them, glibc can keep their memory resident
for reuse. The experiment's rising free heap is consistent with that mechanism.
Receipts persist across restarts, so their growth alone cannot explain every
fresh-process baseline difference; startup loading, later saves and allocator layout
can have different high-water marks even with the same persisted receipt count.

The live receipt set is intentional duplicate suppression, not unlimited playback
history. No unbounded request log is retained by this test fixture. Neither this
review nor these successful cycles found evidence of runaway queue accumulation.

## Sanitizers and limits

Separate ASan/UBSan/LeakSanitizer harness runs passed 200 fake-transport and 200
real local-HTTPS cycles (400 HTTPS requests), with no reported sanitizer diagnostics
and normal exit. FFmpeg emitted expected MP3/Vorbis timestamp warnings in saved
stderr; they were not leak reports. Sanitizer measurements are not used for the
ordinary RSS table.

This was accelerated session/decoder replacement, not overnight real-time playback.
It excludes the full TUI/library, long songs, production responses/retries, library
refresh, disconnected networking, Discord and other environmental differences.
`mallinfo2` in-use is allocator accounting, not exact application-owned allocations;
RSS includes shared/code pages and other mappings. Samples do not capture peaks.
No heaptrack/Valgrind was available. A repeat with real-world conditions or a longer
post-cap soak is warranted if memory continues rising instead of settling.

## Evidence

- Method/build/reproduction: `tools/memory-soak/README.md`.
- Ordinary CSV samples: `artifacts/memory-soak/round1/{off,fake,https}.csv`.
- Ordinary fixture totals and decoder stderr: `round1/fixture.json`, `*.stderr`.
- Sanitizer runs: `artifacts/memory-soak/sanitizers/`.
- All synthetic credentials/certificates/audio were in disposable private directories
  under `/tmp/opencode` and were removed by the runner; only measurement evidence remains.

Recommended next optimization, if requested: reduce Last.fm persistence's transient
full-document allocations while preserving atomic saves, receipt limits, retry and
duplicate-suppression semantics. Do not mask a suspected leak with periodic trims
or automatic process restarts.
