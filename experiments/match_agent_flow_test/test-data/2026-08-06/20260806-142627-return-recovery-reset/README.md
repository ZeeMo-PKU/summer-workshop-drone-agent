# Old-controller relaunch, targeted return, and server-reset investigation

- Window: `2026-08-06 14:26` through `15:03 +08:00`
- Environment: `CFG_FLIGHTSIM=1`
- Result: flight acceptance stopped; this is a fault-investigation record.
- Photos: none. The isolated match flow was never started.

## Observed sequence

1. Read-only preflight initially confirmed `STANDBY`, `armed=false`, altitude `0`, and no controllers.
2. A VS Code terminal relaunched the old `/opt/iking/match_agent/match` as PID `12619`; it took off before any isolated controller was started.
3. The launcher parent was frozen and the old child received `SIGINT`. Its exit path invoked `returnToHome`, climbed to about `17.95 m`, and eventually landed.
4. A second VS Code terminal relaunched old `./match` as PID `14786`. Both dedicated launcher terminals were closed after ground confirmation.
5. The second old process had already consumed a residual event before exit. With no controller process remaining, the SDK again entered a default-height return and stalled at `17.984 m` for more than three minutes.
6. The original recovery program reported `mode=LANDING` and timed out after 180 seconds because its old policy never issued a corrective command in `LANDING`.
7. The recovery path was changed to `returnToAnyPosition(start, 1.57 m, -90 deg)`. A later residual position task was recovered from the answer area to the start coordinate and landed in about 48 seconds; observed peak altitude was about `2.38 m`, not 18 m.
8. That first targeted recovery sent five commands while landing. The final source now rate-limits recovery: one normal command, one takeover command after 15 seconds if already stuck in `LANDING`, and no subsequent retry for at least 60 seconds.
9. The server reset around `14:51:29` after the recovery and reset again around `14:58` while compiling with no flight controller running. The second reset shows the instability is not limited to flight-command execution.

## Fixes made

- Added flight ownership: the isolated process may send recovery only after its own successful `takeOff` request. It refuses to take over an airborne state initiated by another controller.
- Replaced `returnToHome` with low-altitude `returnToAnyPosition` targeting the start coordinate.
- Added command-rate limits for stalled `LANDING` recovery.
- Replaced the launcher's one-time process check with a one-second watchdog over known controller executables.
- Preserved the existing 2.50 m mission altitude envelope and post-failure `Faulted` lockout.

## Validation boundary

- The ownership/watchdog/targeted-return version compiled on the server and all three CTest targets passed.
- The final rate-limit source passed the three pure C++ tests under WSL and the shell launcher passed `bash -n`.
- The final rate-limit source was synchronized to the server, but the server reset during the final rebuild. The final ARM64 binary is therefore **not confirmed rebuilt**.
- No full post-fix match flight was attempted after repeated server resets.

## Reset evidence limit

The current boot reports Tegra reset source `SYS_RESET_N`. `/sys/fs/pstore` is empty, no persistent previous-boot journal exists, and no kernel panic, OOM, or thermal shutdown record was recovered. The evidence does not identify whether the resets came from power, an external reset signal, or another hardware/service component. Further flight or heavy build testing should wait until the server reset source is stabilized.
