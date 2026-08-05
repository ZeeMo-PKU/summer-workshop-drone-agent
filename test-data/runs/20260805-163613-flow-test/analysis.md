# Run analysis: 20260805-163613

This was the first complete isolated single-round simulation run after the
event-order, hover-duration, camera-lifetime, and network fixes.

## Verified sequence

- Live preflight confirmed `CFG_FLIGHTSIM=1`, `STANDBY`, unarmed, SDK mode,
  altitude zero, both camera capabilities, and no competing controller.
- Scene B telemetry remained within tolerance for 3000 ms.
- The question arrived before `SCENE_TRIGGER_SUCCEEDED`, was cached once, and
  was consumed after the trigger.
- A fresh `captures/scene_B.jpg` was saved and its frame pool was closed.
- The program flew to physical zone 2, called `gimbalDownSet`, saved a fresh
  `captures/answer_layout.jpg`, and closed that frame pool.
- `gripperOpen` was accepted once, the start zone was reached, and
  `returnToHome` ended in `STANDBY`.
- The referee reported `ANSWER_CORRECT`.

## Important limitation

This success does not validate vision correctness. The physical front camera
showed the real lab while the referee question existed only in the virtual
scene. The model returned `B`, while the simulator oracle said the semantic
answer was `A`. The randomized layout happened to place semantic answer `A`
at physical zone 2, so the fixed `B -> zone 2` mapping scored correctly by
coincidence.

The next perception change must independently determine the semantic answer
and the physical A/B/C layout, then compose those results. Until then, this run
validates the flight, event, camera, gripper, return, and archival path only.

The server-generated `SHA256SUMS` was verified before this analysis file was
added locally.
