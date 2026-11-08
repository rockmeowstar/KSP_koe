# Shadow draw batching

The adapter combines compatible Unity shadow draws into cached geometry packets. A Rust queue packs position/normal/index data; object matrices remain on the GPU and are copied into a constant-buffer bank. The CPU does not read mapped GPU upload memory.

Only the reviewed shader bytecodes, input layout, static geometry and buffer ranges are accepted. Unknown shaders, dynamic geometry, active queries and unsupported state use the original draws. State changes flush the pending batch. Singleton geometry uses its original draw and is excluded from later batching attempts.

Original geometry bindings remain owned while queued. If packet construction fails, queued original draws replay with their saved GPU matrices; an already submitted batch is never drawn twice. Scene changes request cache release on the render thread. CPU mirrors belong to their D3D resources and use a bounded memory budget; GPU packet budgets also account for available adapter memory.

`Rendering.ShadowDrawBatching` controls the adapter independently. Windows x64, D3D11, feature level 11 and partial constant-buffer updates are required. RenderDoc capture uses the original renderer. The implementation reduces accepted draw submissions; this is not a claim of proportional FPS improvement.

Build the native DLL with `Build-Native.ps1 -Rustc <rustc>`. `-TestFailures` exposes fault injection for GPU tests and is excluded from the production build. The banked vertex shader is a reviewed build asset; upstream game binaries are not included in source.
