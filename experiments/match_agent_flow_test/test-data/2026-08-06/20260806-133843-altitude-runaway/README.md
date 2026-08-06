# Altitude runaway and repeated-start failure sample

- Run ID: `20260806-133843`
- Mode: isolated simulation execute mode with explicit simulation oracle enabled.
- Result: aborted; this is a pre-fix failure sample, not flight acceptance.
- Photos: none. The flow never reached the trigger-zone capture stage.

## Observed sequence

1. The first `MATCH_STARTED` entered round preflight while grounded.
2. During the one-second gripper-close delay, mode changed from `STANDBY` to `TAKEOFF` without this process issuing `takeOff`; process inspection later identified the old `/opt/iking/match_agent/match` controller as a concurrent controller launched by a VS Code terminal loop. The isolated round correctly cancelled and requested return-to-home.
3. Return-to-home reached `STANDBY` after about 109 seconds and reported grounded telemetry.
4. A second `MATCH_STARTED` with a different request ID was accepted because the old abort path returned the state machine to `Ready`.
5. The second round issued `takeOff(1.57m)` and `setPosition(scene B)`.
6. Altitude crossed 2.5 m at `2026-08-06T05:41:49Z` in the run log and continued rising while horizontal distance remained about 5.27 m.
7. Parsed telemetry contains 396 valid records. Its maximum altitude is `17.97599983215332 m`; the last valid record is armed at `17.923999786376953 m` with flight path `LANDING.BEGIN_RETURN_LAND`.
8. A third `MATCH_STARTED` arrived during the second flight. It was queued while the mission worker was busy.
9. The server reset at approximately `2026-08-06 13:43:41 +0800`. The control process disappeared and post-boot SDK telemetry reported `STANDBY`, `armed=false`, and altitude `0`.

## Defects found

- Failure recovery returned the resident state machine to `Ready`, so a repeated start message could launch a second flight without operator acknowledgement.
- A separate terminal loop repeatedly relaunched the old controller after it exited, so checking only the child PID was insufficient. The user authorized closing that dedicated launcher terminal, and it was terminated after the aircraft had returned to ground.
- Navigation had a target-altitude tolerance but no absolute mission-altitude envelope; it would wait for the full navigation timeout while altitude diverged.
- Direct `returnToHome` can use a high default return profile. Normal delivery should first return to the start zone at mission altitude before requesting landing.
- The monitor temporarily retained the last 18.93 m coordinates while the device was offline. It refreshed to the ground position after reconnection.

## Fix applied after this sample

- Mission navigation is limited to `-0.10 m` through `2.50 m`; values outside the envelope fail immediately.
- A failed round lands into `Faulted`; repeated `MATCH_STARTED` and `NEXT_ROUND_STARTED` are ignored until `MATCH_FINISHED` or process restart.
- Successful delivery now returns to the start zone at 1.57 m before calling `returnToHome`.
- Server build and all three CTest targets passed after the patch. No post-fix flight was attempted in this session.

## Final live-status limit

After the old launcher terminal was closed, the server stopped responding on SSH and the local monitor reported the device offline. The last valid SDK check before that loss of connectivity reported `STANDBY`, `armed=false`, and altitude `0`; this is historical evidence, not a claim about the current live state. No further controller was launched.

## Reset evidence limit

`last -x` classified the previous session as a crash. The new boot reported Tegra reset source `SYS_RESET_N`, `/sys/fs/pstore` was empty, and no prior-boot panic/OOM record was available. The evidence does not establish whether the reset was manual, electrical, or caused by another external component; it must not be attributed to this controller without additional hardware logs.
