# Renderer harness baselines

Accepted snapshot baselines live below this directory, scoped by profile and
case:

```text
<profile-id>/<case-id>/
  current.json
  generations/<generation-sha256-without-prefix>/...
```

Generation directories are immutable and content-addressed. `current.json` is
the only mutable file; the harness replaces it atomically after every source
artifact has been rehashed and copied successfully.

Do not copy files here manually. Create a no-mutation proposal first:

```sh
build_release/tools/vkr_harness baseline propose \
  --from build/_artifacts/snapshot/<run-id> \
  --actor '<actor>' \
  --reason '<reason>'
```

Review the emitted `plan.json` and `entries.ndjson`. Accept only with the exact
plan digest printed by the proposal command:

```sh
build_release/tools/vkr_harness baseline accept \
  --plan build/_artifacts/baseline/<proposal-id>/plan.json \
  --confirm-sha256 sha256:<plan-digest>
```

Acceptance records the actor, reason, source report and summary digests,
previous generation, accepted generation, and acceptance timestamp in
`current.json`. Ordinary `profile`, `snapshot`, `autotest`, and `compare`
commands never mutate this directory.

## Cross-machine parity

An accepted generation is also the portable witness for a backend-neutral
Metal/Vulkan comparison. The first machine runs the snapshot, proposes and
accepts the reviewed generation, then commits and pushes both `current.json`
and the generation directory. The second machine pulls those files and runs:

```sh
build_release/tools/vkr_harness snapshot \
  --case tools/cases/smoke/<backend-neutral-case>.case.json \
  --profile tools/profiles/local-offscreen.json \
  --cross-backend
```

The generation retains the source `report.json`, `capture-summary.bin`, every
canonical capture, its metadata, child capture reports, and distinct previews.
The comparison verifies each payload digest before decoding it. Cross-backend
mode requires the same workload and policy fingerprints, an unpinned
`renderer.backend`, and a different environment fingerprint. Ordinary snapshot
and compare commands still require all three fingerprints to match.

Do not delete the first machine's snapshot while its proposal is pending. Once
acceptance has copied and rehashed every listed file, the tracked generation no
longer depends on `build/_artifacts` and the run tree may be removed.

## Current portable witnesses

- `local.offscreen/smoke.sh_ibl.single_probe.snapshot`, Metal 4 on Apple M1 Pro:
  generation
  `sha256:91df6c781f37abe8109c776e81808c68aad99f587c1ef1c9ed09a61ff6cf0f72`,
  source report
  `sha256:3021860c9dec0c0836d5b2341f38eccb0d5f8b087b8f71946c9c5140e29884f6`.
  Its fixture now uses a constant global source instead of the removed image
  sky (ADR-058). The Windows Vulkan `--cross-backend` pairing passed for the
  prior generation `sha256:7492b640…` and must be repeated for this one.

## Current Bistro authorities

The accepted tree intentionally retains exactly two Bistro roots, each with one
current generation and the same fourteen cameras plus deterministic
system-font, bitmap, and MTSDF text:

- `local.offscreen/smoke.bistro.vulkan.text.snapshot` — legacy Vulkan 1.2
- `local.offscreen/smoke.bistro.metal.text.snapshot` — Metal 4; generation
  `sha256:ac640a40415ddc187055988fb660e007623c4c068869eb4f640f58782be494db`,
  source report
  `sha256:a94eb5142656ac7189ca94801f2d364608048af606ce009adbad3d92578becc8`,
  accepted 2026-10-05 at `20eb4355` with Bistro's 0.05 m lamp source radii
  (ADR-019) and its editor override file in the scene content digest. A run
  with every shadow cascade redrawn each frame matches it, and a fresh run
  passes against it.

The case manifests pin their backend and reject a conflicting environment
request. They cannot be used with `--cross-backend`; each root is compared only
with runs whose workload, policy, and environment fingerprints match. The
legacy Vulkan 1.2 generation is a retained historical reference; it does not
validate the current Vulkan 1.4 packet implementation.
