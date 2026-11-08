# Launch-site readiness

`Stock.PlanetLaunchReadiness` is enabled by default. It replaces only the stock
post-ForceInitPQS launch-site timer. All planet construction and placement code
continues to run on Unity's main thread with the original inputs and ordering.

The adapter observes successful initial quad construction during ForceInitPQS,
then waits for a physics boundary. It requires the initialized sphere to remain
alive. It retains the original timer for collider-raycast placement, incomplete
initialization, unknown iterator layouts, or conflicting Harmony patches.
Visibility and a live camera target are not terrain-readiness signals during
startup and are deliberately excluded from the predicate.

The timer duration comes from stock IL. No planet names, radii, MVIDs, CPU counts,
or replacement timeout values are fixed in this module. Patches and tracked PQS
references are released at the main menu or on destruction.

Run `tests/Test-PlanetLaunchReadiness.ps1 -GamePath <KSP>` for production compilation
and IL/predicate checks. `KOE_DIAGNOSTICS` enables isolated runtime counters and
fault tracing from the same source; those facilities are excluded from the
production module. A matched pair covered 1,784 meshes at two checkpoints, with
71,360 matching component hashes. An earlier whole-mesh capture differed and the
cause has not been established; later component captures did not reproduce it.
Default activation was explicitly accepted by the user after the complete flight
comparison. It can be disabled independently. This is not a general terrain-parity claim.
