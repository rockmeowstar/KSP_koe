# Reuse unchanged screen-copy commands

Feature: `Scatterer.ScreenCopyReuse`, enabled by default after explicit user acceptance. An isolated full-cycle retest on 2026-11-08 confirmed 4,353 skipped command rebuilds; FPS changes ranged from -1.7% to +1.9% across stages. The earlier two-candidate 15% flight slowdown was not reproduced by this feature alone. This is not a demonstrated all-stage FPS gain.

In the reviewed Scatterer build, `hdrEnabled` is never assigned. On an HDR camera the comparison with `targetCamera.allowHDR` therefore causes `Reinit` every time screen copying is enabled. `Reinit` still selects its texture format using the unchanged stored field, so it rebuilds the same commands.

This adapter memoizes successful command construction by owner, camera identity, command-buffer identity, stored HDR field and reflection mode. It skips only an unchanged, detached command list. Actual screen-copy requests and their attachment/execution are preserved. The existing stored HDR field and upstream texture format are deliberately unchanged. Relative render-target dimensions remain `-1`, so they continue to follow the camera at execution time.

Destruction disposes the owned command buffer and removes the exact owner from its camera registry. The cache uses a conditional weak table. An assembly SHA256/layout gate leaves unreviewed builds unchanged.

Validation: real Harmony patch on an isolated reproduction of the reviewed callback; 1,000 requested frames, command builds 1,001 → 1, identical command sequences, mode/buffer invalidation and idempotent ownership cleanup. A Unity/KSP menu fixture using the actual Scatterer component passed 1,000 requests, 1,000 skipped rebuilds, stable native buffer identity/size and final native-handle/registry cleanup. No test camera was rendered. This is not a game-image or FPS acceptance claim.
