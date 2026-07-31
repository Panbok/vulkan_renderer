# Compression and N+1 principles

## Casey Muratori: compression-oriented programming

Primary sources:

- [Semantic Compression](https://caseymuratori.com/blog_0015)
- [Complexity and Granularity](https://caseymuratori.com/blog_0016)
- [Defining a Single Enumerant](https://caseymuratori.com/blog_0017)

Apply these ideas:

- Start with concrete working cases and compress only after repetition is
  visible.
- Require at least two real uses before extracting reusable machinery.
- Optimize total lifetime human work — writing, debugging, changing, adapting,
  and operating the code — not physical line count.
- Give frequent domain operations stable names so the source mirrors the
  renderer's language.
- Keep unique code local instead of routing it through an abstraction built for
  a different repetition.
- Build high-level operations by progressively bundling lower-level ones, and
  keep the lower granularity available so exceptional work does not require a
  wholesale rewrite.
- Prefer procedural locality for operations that change together. Do not scatter
  one operation across a table of function pointers merely to organize by nouns.
- When usage code is missing, write representative usage first instead of
  guessing the interface.

Compression is semantic, not minification. A shorter implementation is worse
when it hides ordering, ownership, synchronization, or failure invariants.

## Ryan Fleury: codepaths, batches, and constraints

Primary sources:

- [The Codepath Combinatoric Explosion](https://www.dgtlgrove.com/p/the-codepath-combinatoric-explosion)
- [The Easiest Way To Handle Errors Is To Not Have Them](https://www.dgtlgrove.com/p/the-easiest-way-to-handle-errors)
- [Multi-Threading & Mutation](https://www.dgtlgrove.com/p/multi-threading-and-mutation)
- [Emergence and Composition](https://www.dgtlgrove.com/p/emergence-and-composition)

Apply these ideas:

- Every independent branch/state combination adds execution possibilities, test
  burden, and failure surface. Collapse cases into common exercised codepaths.
- Errors are data and cases, not a separate metaphysical category. Let
  recoverable cases flow through shared paths; eliminate impossible cases
  structurally.
- Operate on batches for both simpler ownership and better hardware behavior.
  Allocate batch lifetime together where possible.
- Treat concurrency, lifetime, and determinism as shaping constraints early
  enough to avoid accidental mutation and later rewrites.
- Preserve multiple useful granularities. A low-level representation and a
  higher-level domain interpretation coexist; one does not erase the other.

## The project N+1 rule

N+1 here is a design test, not the database-query problem and not attributed to
either author above:

> Given N supported pass kinds, resource states, texture formats, light kinds,
> or material slots, adding one more should extend a table, tagged variant,
> descriptor, or registration point without rewriting N unrelated callers.

Use it at stable module interfaces and data descriptions. Reject it when it
would:

- add indirection or branching inside a measured per-draw/per-dispatch loop;
- create a seam with only one real adapter;
- replace a small explicit switch or table with a deep indirection ladder;
- permit invalid combinations that every consumer must then validate;
- obscure GPU ownership, ordering, or completion requirements.

Good N+1 examples in this codebase: the JSON render-graph pass declarations and
named executor registry, `.shadercfg` manifests, typed feature/usage flags,
`VkrTextureDescription`-style descriptors, and the pass-executor table. Poor
examples: one function-pointer indirection per draw, speculative plugin points,
and `void *` extension bags.

## Performance is a correctness property

This project inverts the usual "correctness first, optimize later" ordering. A
renderer that produces the right pixels too slowly has not produced the right
result. That does not license unproven optimization:

- An optimization without a same-configuration Release measurement is a guess.
- An optimization that weakens a lifetime, ownership, or completion invariant is
  a bug that happens to be fast.
- "It should be faster" is not evidence. See `vkr-performance` for what counts.

## Review questions

1. What concrete repetition is being compressed?
2. Which codepath/state combinations disappear?
3. Does the interface expose one source of truth?
4. Can a high-level operation still be replaced by a few lower-level ones?
5. What does the N+1 addition edit, and what stays untouched?
6. Are ownership, ordering, and completion requirements explicit?
7. Is the abstraction outside the hot loop, or supported by measurement?
