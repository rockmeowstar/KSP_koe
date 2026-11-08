# Cloud pass fusion

`EVE.CloudPassFusion` combines a reviewed producer/consumer pair into one GPU draw. It requires `EVE.CloudShaderPacking`, EVE, D3D11 and feature level 11. Other programs use their original draws.

The existing cloud compiler reports the bytecode of shaders it actually creates. The Rust compiler joins the two programs, separates temporary registers and resource slots, and preserves the first output in an additional render target for later readers. It compiles from the game's loaded shaders: no captured EVE program is distributed. Exact bytecode fingerprints limit support to the pair validated with byte comparisons; assembly MVIDs are not used.

The adapter verifies the actual resources, float32 render targets, vertex program bytecode, geometry and used vertex bindings. It ignores debug annotations and bindings that the vertex shader never reads. Depth/stencil targets, linked shader classes, stream output, predication and unsupported geometry prevent fusion. The absence of a depth target makes depth-state flags irrelevant.

Pixel constants are copied on the GPU. A deferred command list retains the original producer while the adapter examines the next draw. Conflicting resource access, another draw, queries, presentation or incompatible state replays the original command list and restores the incoming state. A successful fusion preserves both outputs and restores all modified bindings. Scene transitions request cache release on the render thread.

Validation is separate: Rust register/ABI tests, synthetic GPU fallback tests, captured producer/consumer/final-frame byte comparisons, and live counters/FPS. A reduction in GPU passes is not a claim of proportional whole-frame acceleration.
# Shared hooks with cloud-map packing

CloudMapPacking and CloudPassFusion share the map cache's physical D3D11 hooks. Fusion stages an outer boundary callback and forwards into the existing map-processing body; the driver call remains single. Publication happens only after complete fusion initialization. A failed initialization cancels unpublished callbacks and removes only fusion-owned physical hooks. Render calls use an atomic callback load without a registry lock.

The installed production DLL's former duplicate-hook failure was reproduced by the native GPU fixture. The fixed DLL passed the map cache byte oracle both alone and with fusion enabled, including uploads, Map/Unmap copies, invalidation, budget rejection and cleanup. A complete flight then demonstrated paired-map bindings alongside 1,217 fused draws. These checks establish coexistence, not a universal FPS improvement or whole-scene pixel parity. CloudMapPacking remains opt-in following the measured post-fuel slowdown.
