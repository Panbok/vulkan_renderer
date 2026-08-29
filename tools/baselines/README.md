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
build/tools/vkr_harness baseline propose \
  --from build/_artifacts/snapshot/<run-id> \
  --actor '<actor>' \
  --reason '<reason>'
```

Review the emitted `plan.json` and `entries.ndjson`. Accept only with the exact
plan digest printed by the proposal command:

```sh
build/tools/vkr_harness baseline accept \
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

## Current Bistro authorities

The accepted tree intentionally retains exactly two Bistro roots, each with one
generation and the same fourteen cameras plus deterministic system-font,
bitmap, and MTSDF text:

- `local.offscreen/smoke.bistro.vulkan.text.snapshot` — legacy Vulkan 1.2
- `local.offscreen/smoke.bistro.metal.text.snapshot` — Metal 4

The case manifests pin their backend and reject a conflicting environment
request. They cannot be used with `--cross-backend`; each root is compared only
with later runs of the same backend.
