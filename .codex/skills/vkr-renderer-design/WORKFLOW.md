# Renderer audit and migration

Use `vkr-task-workflow` for task state and user questions. Use `compress-codebase`
when the requested deliverable is a file-by-file compression audit. A focused
change does not need this full workflow.

1. Bound the affected call path and read its current architecture status and
   ADRs. Record the behavior, ownership, and completion invariants to preserve.
2. Map each in-scope file to its actual callers and owner. Record allocation,
   waits, locks, strings, and checks by frequency: load, frame, pass, draw.
   Identify duplicated state and representations. A reviewed file may stay as is.
3. Ask immediately when a finding requires a new architectural choice. Present
   the tradeoff and recommendation in the conversation. Do not defer a required
   decision into a closing list or substitute a design document for the question.
4. Choose one slice and its observable failure or cost. Reproduce it through the
   smallest suitable harness case or tool. Use `vkr-validation` for correctness
   gates and `vkr-performance` for timing. Add CPU tests only with a named
   independent oracle and a demonstrated coverage advantage.
5. Fix the owning representation or producer, then remove code it makes
   redundant. Keep state authority, resource retirement, and cache changes in
   separate slices when they need different evidence.
6. Rerun the selected case against the changed code. Inspect report status,
   assertions, native diagnostics, and relevant output. For optimization,
   compare matching configurations and work volume. Iterate on failures; widen
   the run only when the result leaves a specific invariant untested.
7. Record the final command, configuration, digest, compared values, and remaining
   evidence limits. Follow `vkr-harness` to publish required portable captures
   before deleting local run trees.

Evidence depends on the finding. Pass timings can isolate GPU work; upload and
slot waits can expose serialization; allocator live/committed totals and handle
counts can expose lifetime growth. Collect the metric that can disprove the
hypothesis, rather than every available metric.

A pre-existing failing baseline or missing native backend changes what the run
proves. Investigate available evidence and describe that limit when it appears.
Do not treat a CPU pass or a changed workload as replacement performance or
cross-backend evidence.
