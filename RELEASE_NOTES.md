## 0.16.3

- Parallax: reuse terrain meshes when LOD and visibility decisions are unchanged. Enabled by default for supported installations.
- EVE: correct the slope used by wet-surface lighting, preventing false wet reflections on distant trees.
- Scatterer: remove dark dashed artefacts near the atmospheric horizon.
- New options appear under their respective mod tabs. Unsupported shader variants keep their original code; changing these options requires restarting KSP. Existing configs keep their choices: enable the new options in-game.

Performance: one matched render-route A/B measured 28.26 → 28.97 FPS (+2.5%), with 80.2% of mesh checks avoiding a rebuild. A successful native reuse check took 0.00147 ms versus 0.03209 ms for rebuilding; preparing its certificate cost 0.01228 ms per rebuilt mesh. The horizon correction added 0.0092 ms to the captured GPU pass. The wet-surface correction was not timed separately. These are local measurements, not guaranteed gains.
