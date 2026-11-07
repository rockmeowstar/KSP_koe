# FAR native renderer preparation

`FAR.RustRendererFilter` replaces the renderer-layer predicate inside FAR's `VehicleExposure.ResetParts`. It is enabled by default when the dependency and native-binding checks pass, and remains independently switchable. It preserves FAR's reset, renderer initialization, output-list order, material-property writes and exposure setup.

The C# bridge snapshots native Component pointers from live managed Renderer references. A small Rust library reads the owning GameObject and layer through a reviewed Unity engine binding, returning a byte mask. The bridge applies that mask with stable `List.RemoveAll` compaction. No Unity work moves to worker threads. Null or destroyed wrappers use the original predicate; invalid native metadata falls back before modifying the list.

The binding authenticates the installed engine and FAR implementation. Field offsets are decoded from reviewed engine accessor instructions. Other engine/FAR implementations keep their original path. An owned Unity fixture compares four renderer types, all 32 layers, active/inactive objects and destroyed wrappers before patch installation. Harmony patches on the predicate or relevant API accessors prevent native filtering at the next reset boundary. Native entry never invokes managed callbacks.

The native library is embedded in the managed module and extracted by KOE's existing hash-checked loader. Current native support is Windows x64; Windows KSP under Proton remains untested for this feature. Native Linux, ARM and other engine builds fall back. No graphics-vendor requirement applies.

`KOE_FAR_RUST_FILTER` can select a development library. `KOE_FAR_RUST_AUDIT=1` evaluates the original predicate alongside every native result, retaining the original behavior and disabling the fast path on mismatch. Audit timing is not a performance result.

Local validation: 65 Unity fixture cases; 58,037 live predicate comparisons with zero differences; 1,420 bridge/kernel/IL checks; Rust kernel tests. One matched diagnostic start measured 8.095 → 3.671 ms in filtering and 30.951 → 27.578 ms in FAR reset. Whole-stage time did not improve in that pair; no whole-flight FPS gain is claimed. Broader visual and physics acceptance remains separate.

The installed-package check measured 2.867 ms in filtering, 25.166 ms in reset and 154.454 ms in activation, against the prior 8.095 / 30.951 / 166.332 ms control. This is one instrumented ignition sample per comparison, not a complete-flight benchmark.

## Property setup

`FAR.RustRendererSetup` is a separate, independently switchable batch for the `SetupRenderers` call inside reset. It defaults on for supported installations. It calls the same reviewed native `ShaderPropertySheet::SetVector` and `RendererScripting::SetPropertyBlock` functions for every renderer, retaining color-write/property-copy order and count. FAR's existing object-color calculation runs unchanged. Its reference-comparer sets receive the same renderer references in the same order after the native batch. Only built-in renderer classes and FAR's actual reference comparers are admitted. Null blocks, destroyed renderers, derived lists, custom comparers or conflicting Harmony patches use the original method.

Both native paths require the main thread. The property binding additionally verifies the live engine function bodies before use. Fifteen owned Unity cases check five renderer types and three colors, duplicate renderer writes, bitwise color equality, unchanged input color, and preservation of float, vector, matrix and texture properties. The Rust kernel tests also verify write ordering and rejection before any writes for invalid batches.

With equal call-site timers, property setup measured 10.367 → 7.687 ms, FAR reset 22.378 → 18.513 ms, and activation 157.364 → 140.874 ms. The candidate processed 5,937 batches / 49,536 renderers without fallback. The earlier attempt that fell back on every list is not counted as a performance result. `KOE_FAR_RUST_SETUP=1` enables a development experiment independently of the normal option. No full-flight FPS or general pixel-parity result is claimed from these short tests.

The final normal-config installation check measured 9.195 ms in setup, 21.592 ms in FAR reset and 153.754 ms in activation. Thus the local method improved again, but the whole-stage difference against its 157.364 ms control was only 2.3%, below the user's acceptance threshold for overall timing. Both measurements are retained; the earlier 10.5% whole-stage reduction is not a stable performance guarantee. Both native flags are enabled on the development installation and, following acceptance, default on in generated configs when supported. Existing manual settings remain intact.
