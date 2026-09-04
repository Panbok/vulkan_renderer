# Compression and interface rules

## Compress concrete code

Extract shared machinery only after inspecting at least two production uses
with the same semantics and lifetime. A local helper may name one complex
operation without becoming a reuse framework. Start with a `static` function;
promote it only when a real caller needs the boundary.

Keep the change if it removes duplicated facts, branches, synchronization work,
or caller knowledge. Reject a shorter version that hides ownership, error
propagation, or GPU ordering. Prefer functions and typed data over macros when
either can express the operation without repeated evaluation or hidden state.

Keep unique operations local. Shared high-level helpers should retain useful
lower-level operations when actual callers need them. Do not expose internals
solely for hypothetical future flexibility.

## N+1 interface check

For an existing family of passes, formats, resource states, or material slots,
trace the edits needed to add one member. Extend its owning table, tagged
record, descriptor, or registration point. Unrelated callers should not change.

Use a small explicit switch when it expresses the variation directly. Reject a
new abstraction when it introduces per-draw dispatch, invalid combinations,
`void *` extension data, or an adapter with no real consumer. Metal and Vulkan
already provide two concrete callers for the coarse selected-implementation
boundary and shared GPU cores.

## Review evidence

For each extraction, identify the real callers, the fact or branch removed, the
remaining owner, and how failure and completion propagate. For each removed
check, identify the cold producer that now guarantees its condition. For a
performance claim, supply the matched measurement required by `vkr-performance`.

These rules adapt Casey Muratori's semantic compression and Ryan Fleury's
codepath/batch design ideas. The local N+1 check is a project rule. Historical
reading is optional and is not required context for applying this specification:

- [Semantic Compression](https://caseymuratori.com/blog_0015)
- [Complexity and Granularity](https://caseymuratori.com/blog_0016)
- [The Codepath Combinatoric Explosion](https://www.dgtlgrove.com/p/the-codepath-combinatoric-explosion)
