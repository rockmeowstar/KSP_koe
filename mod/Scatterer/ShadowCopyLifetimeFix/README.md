# Shadow-copy resource lifetime

Feature: `Scatterer.ShadowCopyLifetime`, enabled by default under the user's conditional-optimization acceptance rule. The isolated 2026-11-08 full cycle showed no material regression (-0.4% powered-flight FPS, -0.3% post-fuel FPS). No buffer replacements occurred in its measured lifecycle, so those FPS changes are not attributed to cleanup. The matching native fixture verified 100 replaced buffers were released and final owners/registries were cleaned. The earlier two-candidate 15% flight slowdown was not reproduced by either feature alone; their updated combination still needs a new full-cycle measurement.

The reviewed Scatterer build replaces its shadow-copy command buffer after scene changes without disposing the previous buffer. Destruction also leaves a scene-event subscription and static light-to-owner entry. This adapter detaches/disposes the owned buffer, cancels pending owner coroutines, removes the exact owner callback and removes the registry entry only if it still belongs to that owner. It preserves any original destruction exception.

The assembly SHA256 and expected field/method layout are checked before patching. Unreviewed builds keep upstream behavior. No rendering commands, texture formats or refresh rates are changed.

Validation: isolated 100-recreation ownership fixture; repeated cleanup; replacement-owner preservation; compilation against installed game references. A Unity/KSP menu fixture with real Scatterer components and an owned shadow-map texture also passed 100 replacements and final native-handle/registry cleanup. Full-scene transitions and FPS acceptance remain unmeasured.
