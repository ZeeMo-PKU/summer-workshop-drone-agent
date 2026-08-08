# Teammate code archive: 2026-08-07 server snapshot

This directory is a read-only reference snapshot of the teammate-owned source
files in `/opt/iking/match_agent/` on `root@10.8.82.81`. It is not part of the
active build and does not replace the stable repository source.

## Contents

- `current/`: the server-side `*.cpp`, `*.hpp`, and project `README.md` files
  present at snapshot time.
- `SHA256SUMS`: hashes of every archived source file under `current/`.

## Deliberate exclusions

- ARM64 binaries, build outputs, logs, captures, caches, and process files.
- API key files, environment files, credentials, and unrelated server data.
- The `codex/` subdirectory and the separate teammate handoff document.

A credential literal was found in `current/recognize_image.hpp`. The public
archive replaces only that literal with an empty fallback and keeps the
environment-variable path intact. `SHA256SUMS` therefore describes the
redacted public archive, not the byte-identical server file. The server files
and processes were not changed as part of the archival redaction.

The current server `match.cpp` differs from the stable repository baseline and
still defaults to an executing mode. Treat it as historical input for review,
not as a safe executable entry point.
