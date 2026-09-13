---
status: proposed
updated: 2026-09-13
authority: proposal
---

# Portable path acceptance gates

The path owners, migration and regression fixtures are implemented in
[ADR-070](../adr/070-portable-path-boundaries.md). This proposal retains only
checks that require an unavailable native environment or interactive editor.

## Remaining evidence

1. Run the CPU wrapper, native path fixture and fresh-process project lifecycle
   on macOS. Verify literal POSIX filename characters, symlink containment,
   publication collision and Unicode source/workspace paths. A Windows pass and
   source inspection do not establish native macOS behavior.
2. Run the native fixture and lifecycle from a writable Windows network share,
   using ordinary and extended UNC roots. Verify identity, process working
   directory, redirected logs and exclusive publication. Lexical root tests do
   not establish access to a real share.
3. In the Windows editor, select a managed scene after restarting the process,
   then rebuild it from Content. Use a Unicode workspace with deep paths and
   verify both successful activation and a visible error for an invalid request
   path. The C project-store test exercises the request resolver but does not
   exercise a UI click or the complete event path. Any scene-based renderer
   activation check must use Bistro.

## Acceptance

Use the commands owned by ADR-070 with native tool suffixes. Retain exact
configuration, commands, outputs and unavailable gates in the task evidence.
Preserve stable asset IDs, authored edits and old revisions throughout the
lifecycle. Investigate any failing boundary at its owner; do not weaken raw
managed-reference grammar or substitute a shorter path to obtain a pass.

Remove this proposal once these gates pass and record the resulting native
coverage in the accepted ADR. No additional path abstraction or document
version change is proposed.
