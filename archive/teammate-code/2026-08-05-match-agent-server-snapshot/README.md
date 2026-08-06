# Teammate code archive: 2026-08-05 server snapshot

This directory is a reference snapshot of teammate-owned project artifacts
from `root@10.8.82.81`. It is not part of the active build. Source content is
kept unchanged except for explicit credential redaction required before public
distribution.

## Provenance

- `current/` was copied from `/opt/iking/match_agent/` on 2026-08-05.
- `captures/` was copied from `/opt/iking/match_agent/captures/` on the same
  date. The project owner explicitly approved these test images for the public
  project report.
- `recovered-vscode-history/match_test.cpp` was recovered from
  `/root/.vscode-server/data/User/History/-5cf2f779/wEkd.cpp`. The corresponding
  working-tree file no longer existed at snapshot time, so this is clearly
  marked as a history recovery rather than a current source file.

## Deliberate exclusions

- ARM64 binaries `match` and `test_recognize`.
- The `codex/` subdirectory, which was produced by separate Codex experiments
  and is not teammate-owned source.
- API keys, `.secrets.env`, credentials, build outputs, process files, caches,
  and unrelated server data.
- The separate teammate handoff document, per the group repository policy.

During the final public-repository scan, one historical fallback API key was
found in `current/recognize_image.hpp`. Its literal value was replaced with an
empty string and the adjacent comment was updated; no other source behavior or
content was changed. The exposed credential must be rotated.

`SHA256SUMS` records the public, redacted artifacts. The server originals were
read only and were not modified or stopped during this archive operation.
