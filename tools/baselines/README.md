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
