# Ground telemetry guard regression

- Recorded at: `2026-08-06 12:06:21 +0800`
- Scope: build, unit tests, source deployment hashes, live read-only telemetry, and launcher concurrency guard.
- Flight commands sent: none.
- Flight acceptance: not performed.

## Result

- Server build completed successfully.
- CTest passed 3/3 tests.
- Local and deployed hashes matched for every changed source/document file.
- The launcher refused to start because teammate controller PID `44459` was still resident as `/opt/iking/match_agent/match`.
- Live SDK telemetry reported `STANDBY`, `armed=false`, `sdk_mode=true`, and altitude `0`.
- The referee monitor reported `preparing`, not running, connected, and the drone at the start-zone ground position.

## Defect covered

The resident controller previously treated `STANDBY` alone as sufficient before a later round and after return-to-home. The patched version also requires fresh, valid telemetry, `armed=false`, SDK control, finite altitude, and absolute altitude no greater than `0.10 m`.

This is a no-flight regression record. A full trajectory, capture, recognition, drop, answer, and landing test remains pending until the other controller exits.
