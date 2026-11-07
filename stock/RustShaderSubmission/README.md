# Rust shader submission

This module replaces Unity's D3D11 shader override preparation and constant-buffer application. It covers all seven active shader stages and applies to stock and mod shaders, including EVE. Unity still owns the GPU resources, draw calls and driver interface.

`Rendering.RustShaderSubmission` selects the module. The catalog defaults to disabled; the development installation explicitly enables it. Frame generation is independent and remains disabled.

The Rust pass builds reusable property indexes on native worker threads, preserving the first matching property and Unity's parameter order. Each lookup checks the current names and type ranges against its owned snapshot; values, offsets and resource handles are read again on every submission. A pending index uses the original-order SIMD search until its worker finishes. GPU and Unity calls stay on the render thread.

The pool derives its worker count from available logical processors, with the render thread participating in bulk copies. The index-cache budget follows detected CPU cache capacity and available RAM. Startup calibration selects a bulk-copy threshold only when every measured parallel sample beats the fastest serial sample. Small copies stay local. Copy jobs write disjoint reserved ranges and complete before resource callbacks. Thread-local scratch storage replaces temporary per-stage allocations. The module serializes scalar, vector and matrix overrides, inline and external constant buffers, textures and structured buffers.

Unknown shader implementations use their original application method. Unsupported layouts use Unity's original preparation or application path. No GPU command is issued by the custom application path before its constant-buffer validation completes. Installation checks the machine-code identities containing the private layouts and device offsets; a mismatch prevents activation. The supported target is the verified KSP 1.12.5 Windows x64 D3D11 image. Other engines and platforms retain Unity's original path.

The differential tests execute the actual Unity routines in an isolated process. Only allocation and GPU resource boundaries are mocked. They compare packed data, constant-buffer bytes, dirty flags, cursor positions, resource arguments and command order. Four unused padding bytes in each external-buffer record are excluded from byte comparisons. These tests do not establish pixel identity for every possible game scene.

Run `tests/Test-RustSubmission.ps1` with `-GamePath`, `-Rustc` and optionally `-NativeDll` to test the production adapter. `Build-Native.ps1` builds the Rust library and adapter; normal package builds validate the resulting source/binary manifest. Rust and the C++ build tools are build dependencies only. The installed module contains its native binary.

The diagnostic build supports shadow comparisons and timed scopes; production excludes those exports and counters. Timed scopes are separate from FPS measurements. Synthetic preparation benchmarks and the historical render-thread profile are not evidence of an equal FPS gain.
