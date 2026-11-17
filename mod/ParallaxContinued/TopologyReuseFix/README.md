# Certified terrain mesh reuse

`ParallaxContinued.CertifiedMeshReuse` defaults on for supported Windows x64 installations. Changing it requires a restart. Existing user configurations are preserved.

The Rust kernel caches shared subdivision vertices and LOD classifications. A certificate records every comparison needed to retain the existing mesh. It skips traversal, copies and mesh submission only when those decisions remain unchanged. Source refresh, cleanup, changed thresholds and forced dispatch invalidate reuse. Pending work must complete before its context is released. Unknown APIs, failed native work and exhausted cache reservations retain the original Parallax path.

Native binaries are embedded in the managed module and extracted to a content-addressed cache. No trial marker, periodic telemetry file or direct-output experiment is used. Compatibility checks use method semantics and field layouts rather than assembly MVIDs. The native source retains diagnostic test/export helpers; the production adapter calls only the accepted merge/copy and fast-certificate path.

Build with `Build-Native.ps1 -Rustc <rustc.exe>`. Run `tests/Test-ParallaxTopology.ps1 -GamePath <KSP> -Rustc <rustc.exe>`. Captured CIL/mesh comparisons and the flight A/B are documented in `docs/release-0.16.3-validation.md`.
