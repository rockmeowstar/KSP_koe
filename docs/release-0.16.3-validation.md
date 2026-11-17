# 0.16.3 validation

## Parallax

- 13 Rust tests, including visibility and LOD boundaries, float extremes, output-buffer bounds, invalidation and nonfinite culling.
- 168 original-CIL cases: 213,097 vertices and 1,214,907 indices, exact output.
- 672 synthetic certificate queries: 663 accepted, including 495 moving queries; every accepted result matched the original CIL.
- 16 captured meshes: 8,518 vertices and 46,725 indices, byte parity. Sequential replay accepted all four unchanged outputs, with zero false positives.
- Production support checks accept the installed API and equivalent code with a changed MVID, and reject missing or changed APIs.
- Options tests cover 78 unique keys, defaults, existing configurations, dependencies and recovery.

One coordinate-route render A/B, KOE-S, frame generation off, focus 100%, master/session volume 2%. PhysX/FAR dynamics were disabled for pose replay. These are render-fixture results, not normal-flight physics results.

| Recorded route | Without reuse | With reuse | Change |
|---|---:|---:|---:|
| 0–15 s | 25.898 FPS | 26.483 FPS | +2.26% |
| 15–30 s | 27.297 FPS | 27.827 FPS | +1.94% |
| 30–60 s | 28.051 FPS | 28.850 FPS | +2.85% |
| 60–112.14 s | 29.249 FPS | 29.991 FPS | +2.54% |
| Frames / active seconds | 28.260 FPS | 28.968 FPS | +2.51% |

Periodic route counters: 3,593 accepted checks out of 4,482 (80.2%); approximately 4,373 mesh builds versus 957. Both runs retained 56 parts, completed a subsequent reload, and reported no topology failures or budget skips. One ordered pair does not eliminate cache/order effects or prove repeatability. The production adapter removes diagnostic timing dictionaries and file writes; its packaging/integration checks are separate from these candidate FPS measurements.

Native-only microbenchmark means of 16 per-snapshot medians: rebuild/merge/copy 0.03209 ms; successful reuse 0.00147 ms; certificate preparation 0.01228 ms per rebuilt mesh. This is not a whole-game speedup.

## Visual corrections

Wet-surface lighting uses geometric slope instead of the perturbed material normal for its slope decision. The horizon correction reconstructs finite-segment atmospheric radiance around the LUT seam. Both are deliberate visual bug corrections, not pixel-identical optimizations.

The same recipes reproduce the GPU-validated target DXBC byte-for-byte, reject every truncated recipe and corrupt/unsupported input, and replace both original shaders through the production D3D11 creation hook in an isolated process. D3D11 accepts both replacement shaders. Unsupported source hashes retain the original shader. No external mod shader bundles are redistributed or modified on disk.

Captured GPU replay for the horizon pass: five-sample medians after warm-up, 0.166912 → 0.176128 ms (+0.009216 ms). Wet-surface cost was not separately timed. Recipe decoding and hashing happen at shader creation, not on each frame. The live bug-fix visuals were inspected before packaging; the new package is not a new controlled whole-flight FPS result.

Local evidence: `KerbalOverEngineering/artifacts/parallax-topology-20261117/recorded-pair-20261117-222044` and `recorded-pair-20261117-222510`; `KerbalOverEngineering/Reports/parallax-certified-reuse-20261117.md`; `KerbalOverEngineering/Reports/horizon-finite-segment-fix-20261117.md`.
