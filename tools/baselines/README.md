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
comparison between two hosts. Metal and Vulkan now run different pipeline
classes (ADR-087), and an unpinned case's workload fingerprint names the
class its host's backend runs, so a Metal and a Vulkan run of one case no
longer share a workload and cannot be paired. The first machine runs the snapshot, proposes and
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

None. The Metal desktop witnesses (`smoke.sh_ibl.single_probe.snapshot` and
the Bistro local-shadow street and indoor captures) were removed with the
Metal desktop pipeline (ADR-087, 2026-10-06); their cases now run on Vulkan.

## Current Bistro authorities

The accepted tree retains one Bistro root with one current generation, its
fourteen cameras plus deterministic system-font, bitmap, and MTSDF text:

- `local.offscreen/smoke.bistro.vulkan.text.snapshot` — legacy Vulkan 1.2

Its case manifest pins the backend and rejects a conflicting environment
request. It cannot be used with `--cross-backend`; the root is compared only
with runs whose workload, policy, and environment fingerprints match. The
legacy Vulkan 1.2 generation is a retained historical reference; it does not
validate the current Vulkan 1.4 packet implementation. The Metal 4 root
`smoke.bistro.metal.text.snapshot` recorded the desktop pipeline and was
removed with it; a tiled-pipeline Bistro authority has not been accepted.
