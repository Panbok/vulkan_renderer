---
status: proposed
updated: 2026-10-05
authority: proposal
---
# Container audit and Array/Vector merge

Review of the generic containers in `lib/src/containers/` at commit
`526665c8`. It lists the defects with evidence, the fix for each one and
a design that merges `Array` and `Vector` into one type. No container code
has changed yet.

## Scope and method

- Code: [`array.h`](../../lib/src/containers/array.h),
  [`vector.h`](../../lib/src/containers/vector.h),
  [`queue.h`](../../lib/src/containers/queue.h),
  [`vkr_hashtable.h`](../../lib/src/containers/vkr_hashtable.h),
  [`vkr_freelist.c`](../../lib/src/containers/vkr_freelist.c),
  [`bitset.c`](../../lib/src/containers/bitset.c),
  [`vkr_sort.c`](../../lib/src/containers/vkr_sort.c) and the production
  callers of each.
- Evidence: source reading and two Python simulations that reproduce the
  hash table's FNV-1a hash, `hash % capacity` index, linear probing,
  128-probe limit, tombstones and growth at three quarters of capacity.
- Not covered: the string library (`str.c`), builds, CPU tests and timings.
  Line numbers below refer to `526665c8`.

## Severity

| Level | Meaning |
|---|---|
| P1 | Wrong result or failure in a supported path, now or at a reachable size |
| P2 | Cost or fragility in a supported path |
| P3 | Limit, contract gap or cleanup to record |

## Hash table findings

| # | Level | Finding | Evidence | Fix |
|---|---|---|---|---|
| H1 | P1 | An insert fails while the table has free capacity. `vkr_hash_table_insert_internal_##name` stops after `VKR_HASH_TABLE_MAX_PROBES` (128) probes and returns false (line 89). Growth happens only when `size` reaches three quarters of capacity (line 261), so the failure does not trigger growth. | Simulation with keys like `assets/textures/bistro/<8 letters>_<n>.ktx2`, starting at capacity 16, five trials per size. The longest probe distance was 36–75 at 1,000 keys, 62–112 at 10,000 keys and 94–156 at 100,000 keys. Four of five trials at 100,000 keys need more than 127 probes and fail. | Grow on probe overflow, or remove the limit because the load factor bounds probe length. |
| H2 | P2 | Tombstones are never cleared. Remove marks a tombstone (line 192). Only growth rehashes, and growth counts live keys only. A table with churn loses all empty slots, so each miss walks the full probe limit. | `request_by_key` in [`vkr_resource_system.c`](../../runtime/src/renderer/systems/vkr_resource_system.c) inserts one unique key per request (line 1279), looks keys up (line 1191) and removes them at completion (line 606), under the resource-system mutex. Simulation at capacity 128 with at most 16 requests in flight: 85 empty slots after 50 requests, 17 after 200 and 0 after 500. After that, each new request walks 128 slots for the lookup and 128 for the insert. | Count tombstones. Rehash at the same capacity when live keys plus tombstones reach the threshold. |
| H3 | P2 | The low *k* bits of FNV-1a depend only on the low *k* bits of each input byte. Capacities start at 16 and double, so `hash % capacity` reads only those bits. | `mat_a`, `mat_q`, `mat_A` and `mat_Q` all map to slot 5 at capacity 16. | Apply a 64-bit finalizer (for example fmix64) before indexing. |
| H4 | P2 | Each hash and each probe step does a 64-bit division (lines 38, 63, 83, 197, 221 and 246). | Source. | Keep capacities at powers of two and mask with `capacity - 1`. |
| H5 | P2 | Entries store no hash, so each occupied slot on a probe path costs `strcmp`. `vkr_hash_table_get_string8_##name` calls `vkr_string8_equals_cstr`, which runs `strlen` on each candidate. | Source. | Store the 64-bit hash in the entry and compare it before the key. Rehash then reuses stored hashes. |
| H6 | P3 | An update of an existing key keeps the first key pointer (line 76). A caller that inserts a new key copy and frees the old copy leaves a dangling key. | Source. The header says keys are borrowed but not which copy the table keeps. | State in the header that the first inserted key stays borrowed until removal. |
| H7 | P3 | Keys are strings only. `vkr_text_font_find_legacy_glyph` in [`vkr_text.c`](../../runtime/src/core/vkr_text.c) formats each codepoint as decimal text to find a glyph (line 356). | Source. Cooked fonts use `codepoint_map` and avoid this path. | Add an integer-key table variant, or move the legacy glyph index to a sorted codepoint array. |

H1 is latent today. The texture, material, geometry, mesh asset, camera and
font systems and the glyph indices create tables at twice their maximum
count, so their load stays at or below one half. The tables that grow to
three quarters, such as the render graph name indices and `request_by_key`,
hold few live keys.

## Other container findings

| # | Level | Finding | Fix |
|---|---|---|---|
| C1 | P3 | `Vector` allocates with `vkr_allocator_alloc` and `vkr_allocator_realloc` (`vector.h` lines 33 and 57), which give the allocator's default maximum alignment. `Array` uses `AlignOf(type)`. A type with larger alignment is misaligned in a `Vector`. | Use the aligned allocate, reallocate and free calls with `AlignOf(type)`. |
| C2 | P3 | Growth copies the old capacity, not `length`. On an arena, reallocation leaves the old block in place, so the arena holds about twice the final size. | Copy `length` elements. Reserve exact sizes before filling arena-backed vectors. |
| C3 | P3 | `queue_enqueue_##name` and `queue_dequeue_##name` use `%` on each call (`queue.h` lines 114 and 134). | Increment the index and reset it to zero at `capacity`. |
| C4 | P3 | `bitset8_create()` has no C11 prototype (`bitset.h` line 42). The header says set, clear and toggle accept OR'd flags, but `bitset.c` asserts one bit. | Declare `(void)`. Make the header and the assertions agree. |
| C5 | P3 | `vkr_freelist_resize` has no node-memory size parameter. It sizes nodes from the tracked address space (`vkr_freelist.c` line 315), so the page-aligned tail that `vkr_dmemory` passes stays unused and `nodes_allocated_size` records the smaller size. | Take `new_memory_size`, as `vkr_freelist_grow_nodes` does. |
| C6 | P3 | `array_create_##name` returns the all-zero record for length 0, so a caller cannot tell an empty result from an allocation failure. | The merged type below permits capacity 0 as an empty, valid record. |

`vkr_sort` and the remaining `Array`, `Vector`, `Queue` and freelist paths
are correct. `vkr_sort` wraps `qsort`, which is not stable; the header already
requires a total order.

## Array and Vector merge

### Baseline

| Property | `Array` | `Vector` |
|---|---|---|
| Fields | allocator, length, data | allocator, capacity, length, data |
| Length at creation | Equals the requested count | 0 |
| Growth | None | Doubles on push; exact on reserve |
| Element pointers | Stable for the lifetime, because no growth API exists | Invalidated by growth |
| Alignment | `AlignOf(type)` | Allocator default (C1) |
| Default capacity | None | `DEFAULT_VECTOR_CAPACITY` (16) |

Outside the container headers, `array_create_` appears 55 times and
`vector_create_` 33 times, and several owners start a `Vector` lazily with only
`.allocator` set. Five project element types have both instantiations:
`VkrFontGlyph`, `VkrFontKerning`, `VkrMtsdfGlyph`,
`VkrMeshLoaderSubmeshRange` and `VkrBitmapFontPage`.

`Array` has two kinds of user:

- **Loaded data of known size.** Font loaders build glyphs and kernings in
  a `Vector` on a temporary allocator, then copy them into an exact `Array`
  on the result allocator. Mesh decode sizes its arrays from counts in the
  cooked header.
- **Fixed slot tables.** The texture, material, geometry, mesh, camera and
  font systems size an `Array` to a configured maximum at startup. Getters
  such as `vkr_material_system_get_by_handle` return `&array.data[i]`. These
  owners depend on the storage never moving.

### Proposed contract

One type, `Array_##name`, with fields allocator, length, capacity and data:

1. `array_create_##name(allocator, capacity)` allocates `capacity` elements
   with `AlignOf(type)` and sets length 0. Capacity 0 returns a valid, empty
   record that allocates on first growth. The default capacity of 16 goes away.
2. `array_create_filled_##name(allocator, length)` allocates and zeroes
   `length` elements and sets length and capacity to `length`. Slot tables and
   loaded data use it.
3. `array_push_##name` doubles capacity when full. `array_reserve_##name`
   grows to an exact capacity. Both preserve contents on failure and return
   `bool8_t`.
4. Get, set, pop, pop-at, find, clear and destroy keep their current
   `Vector` semantics. Destroy frees with the same alignment.
5. A successful push or reserve can move the storage. A slot-table owner
   allocates once with `array_create_filled_##name` and does not call push or
   reserve afterwards. The owner's header states this rule.

The change ports every caller in one commit and deletes `vector.h`, with no
adapter layer.

### Cost and risk

- Pointer stability for slot tables becomes an owner rule. A future push on
  a slot table moves the storage, and the compiler cannot detect that.
- Each former `Array` record grows by 8 bytes for `capacity`.
- The port touches the runtime, renderer, tools and tests together.

### Alternatives considered

- **Keep two types.** This keeps the type-level stability guarantee for slot
  tables. It also keeps two APIs for one layout and the duplicate
  instantiations.
- **A fixed flag on the merged type.** A flag-driven mode adds a branch to
  each growth call. `AGENTS.md` prefers typed tables over flag-driven helpers.
- **Inline small-buffer storage.** Constructors return records by value. A
  record that holds its own buffer would leave `data` pointing into the old
  copy.
- **A caller-provided initial buffer** that spills to the allocator on
  overflow. This is safe in C. It is deferred until a hot path needs bounded
  stack storage that can grow.

## Order of work

1. Hash table H1 to H5 as one correctness change. H6 is a header comment in
   the same change.
2. C1 to C5 in the existing owners.
3. The `Array` and `Vector` merge, which includes C6.
4. H7 when the legacy font path is next changed.

## Acceptance evidence

| Item | Check |
|---|---|
| H1 | A CPU test inserts 200 keys that share one home slot under the current hash and checks that every insert and lookup succeeds. The test fails at `526665c8`. |
| H2 | Matched capture-free Release before and after measurements of resource request submission during a Bistro load. |
| H3 to H5 | Review of the new hash and index code. Timings, if claimed, follow the H2 rule. |
| C1 | A CPU test stores a type with `_Alignas(64)` and checks the data alignment after creation and after growth. |
| Merge | Repository build wrappers and the full CPU suite on Windows with Vulkan and on macOS with Metal, and a Bistro load in the editor. |
