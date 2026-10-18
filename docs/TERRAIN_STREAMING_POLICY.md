# Terrain and world-streaming policy

This is the acceptance contract for item 14.6. The first release streams terrain chunks only; gameplay-entity streaming is a separate phase and must not be inferred from `PersistentComponent::StreamingRadius`.

## Authority and persistence

- A versioned `.wlterrain` asset is the immutable authored source of truth. It records a stable `WorldId`, schema version, generator ID and version, seed, world origin, sample spacing, chunk resolution, height/noise parameters, material asset IDs, and slope/height splat rules.
- Generated height samples, LOD meshes, collision caches, and renderer resources are derived data. Do not serialize them into `.wastescene` or `.wlsave`.
- Runtime edits live in versioned world-state sidecars outside the authored asset tree, keyed by `(WorldId, signed int64 chunkX, signed int64 chunkZ)`. Store sparse deltas from the deterministic base height lattice and explicit material overrides; never persist LOD-specific data.
- Every sidecar records the terrain schema and a fingerprint of the base asset/generator. Reject an unsupported schema or mismatched fingerprint without mutating the active world. Migrations must be explicit.
- Persist a dirty chunk before eviction. Use temporary-file plus atomic-replace writes and validate checksums on load. A failed write keeps the chunk dirty and resident, defers additional loads when necessary, and reports backpressure; it never drops edits to meet a budget.
- Keep terrain sidecars separate from the entity snapshot. A save game may identify the terrain overlay revision, but does not embed terrain samples or meshes.

## Deterministic chunk identity and generation

- Use 64x64 base cells per chunk (65x65 boundary-inclusive height samples) for the initial asset profile. LOD is a transient mesh/cache key, never persistent identity.
- Convert world positions to signed global sample-lattice coordinates with mathematical floor, including for negative coordinates. Derive chunk coordinates from integer lattice indices, not rounded floating-point camera positions.
- Generate samples from `(generator ID, generator version, seed, global lattice X, global lattice Z)`. Neighboring chunks obtain shared border samples from the same global lattice function, so borders match exactly.
- Make out-of-bounds and invalid-sample behavior explicit in the API. Splat weights must be finite, non-negative, and normalized to one; material selection derives from authored slope/height rules.

## Initial LOD and residency profile

- Measure Chebyshev distance in chunk coordinates. LOD0 is resident through distance 2; LOD1 covers distances 3–6; LOD2 covers distances 7–12; no terrain chunk is resident beyond distance 12.
- Add one-chunk enter/exit hysteresis at each ring boundary to prevent camera movement from thrashing requests. Hysteresis affects residency only; it does not change chunk identity or persisted data.
- Start with configurable 256 MiB CPU and 256 MiB GPU residency ceilings. Count staging buffers and in-flight allocations against the relevant ceiling. Default to at most 2 generation/read jobs and 4 main-thread mesh/collision/resource commits per frame; expose these limits for tuning only after measurements.
- Workers perform file I/O and deterministic CPU generation only. ECS/Scene mutation, physics-body changes, and renderer-resource creation/destruction/commit stay on the main thread and obey the per-frame commit budget.
- Pin chunks needed by the active player/physics region and all dirty chunks. Clean LOD meshes and collision caches are disposable and rebuildable. If all eviction candidates are pinned, defer new loads and report backpressure instead of exceeding memory caps or losing data.

## Scope and gates

1. Ship CPU-only terrain sampling, deterministic chunk identity, splat evaluation, and asset validation first. These have no renderer or Scene dependency and must be covered by native tests.
2. Add sidecar persistence and failure-safe dirty-state handling next; test round trips, corrupt files, wrong world IDs/fingerprints, and failed atomic replacement.
3. Add CPU LOD generation/stitching after persistence. Test that all LODs use the same base terrain and that adjacent transitions share edge heights.
4. Add the camera-driven terrain streamer only after the prior gates pass. Test ring selection, hysteresis, duplicate suppression, stale async completion rejection, bounded queues/bytes/uploads, and dirty-chunk recovery after unload/reload.
5. Do not claim terrain is walkable until a tested terrain collision representation is integrated with Scene/physics. SceneSerializer currently does not persist `PersistentComponent::StreamingRadius`; do not use that field for terrain policy or entity eviction until its authored semantics have explicit serialization and tests.
6. Gameplay-entity streaming requires a durable UUID-keyed store for entities absent from the live registry. It remains out of scope for the first terrain-only streamer until that store and its recovery tests exist.
