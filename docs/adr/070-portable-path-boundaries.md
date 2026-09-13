---
status: implemented
updated: 2026-09-13
authority: adr
---
# ADR-070: Portable path boundaries

## Status

Accepted. The owners and regression gates below are implemented. Native macOS,
network-share access and an interactive Windows scene-selection check remain
separate evidence gates; lexical UNC tests do not establish share access.

## Context

Windows scene creation, reopening and rebuilding exposed different path rules
at adjacent boundaries. Narrow process arguments disagreed with UTF-8 APIs;
native separators leaked into portable JSON; filename parsers applied shell or
URI semantics to ordinary paths. Fixing individual call sites did not cover the
full import and reload lifecycle.

## Decision

| Value | Contract and owner |
|---|---|
| Host path | UTF-8 at C interfaces, `Path` in Python. The filesystem owner converts to native encoding/root syntax immediately before I/O. C++ uses the same native adapter for STL file operations. |
| Managed reference | UTF-8 owner-relative path with `/` separators and nonempty raw segments. Reject roots, drives, `.`, `..`, backslashes and NUL before filesystem normalization. C and Python consume the same conformance corpus. |
| Resource reference | Explicit `./` or `../` resolves against the owning file; bare references retain legacy repository-root meaning. Native root classification precedes query and dot processing. |
| glTF URI | Decode URI path escapes exactly once at the import boundary. Data URIs remain separate. Python managed imports reject remote/query/fragment dependencies. |
| OBJ/MTL filename | Parse supported quotes and comments without shell escapes or URL decoding. Native Windows separators remain separators; literal percent characters remain filename bytes. |

`vkr_asset_path_managed_valid` and Python `validate_managed_path` own raw managed
grammar. `managed_reference` owns Python serialization. Physical containment
remains the project resolver's responsibility, including symlink/junction escape
and component boundaries. Managed names do not inherit permissive resource-path
dot traversal. Values are never globally lowercased or slash-replaced.

The resource resolver protects UNC server/share roots, recognizes extended
Windows drive/UNC roots before interpreting `?`, and rejects Windows drive-relative
and device paths. POSIX literal backslashes are preserved. Asset-reference
buffers are bounded to 32767 bytes; native conversion separately checks its
32768-wide-character storage. Existing editor and JSON capacities still apply:
long-path support does not remove input-size limits or authorize truncation.

The Windows `VKR_MAIN` adapter converts CRT wide arguments to UTF-8 for app,
editor, harness and shipping cooker entry points. Startup storage is owned until
the application returns and uses CRT allocation before project allocators exist.
`file_fopen` supplies UTF-8/extended-path access to stdio consumers and closes via
ordinary `fclose`. Cgltf file callbacks and KTX stdio bridges reuse this boundary;
vendor implementations are not patched. Process working-directory and redirected
log paths use the filesystem's native conversion too.

Windows process working directories have a separate `CreateProcessW` length
limit. For a deep directory the launcher uses its existing short-name spelling
when available, preserving directory identity. A volume without a usable short
name cannot launch with that deep working directory; the operation fails rather
than silently changing the directory. This restriction does not apply to the
extended source, output and redirected-log paths supported by the I/O owner.

The project store resolves selected scene paths for the editor's request writer.
Missing-manifest deletion requests retain their distinct checked-join behavior.
Request open/serialization failures produce a visible message rather than a
silent return before Bakery starts.

## Publication and compatibility

Directory publication is distinct from file replacement. Python
`publish_directory` uses no-replace rename on Windows and exclusive destination
reservation followed by replacement on POSIX. Transaction owners acquire cleanup
ownership only after publication succeeds. Existing conflict, cancellation and
manifest-fingerprint checks remain in force; publication never starts by deleting
another destination.

Compatibility migration changes only known historical `source` fields in asset
and import records. It repairs legacy Windows separators and then applies the
same raw grammar and containment checks. Unknown strings, display names, URI
payloads, source bytes and old artifacts remain unchanged. Prepare/reopen repairs
the loaded view without rewriting the document; a successful mutating transaction
or copied import publishes normalized metadata under the existing conflict checks.
No document version bump is needed for this repair.

## Regression gates

The normal CPU test wrappers run the source boundary guard and Python path
conformance check before the C suite. The guard rejects new narrow file calls in
shipping consumers and native serialization of managed references; behavioral
tests remain the correctness oracle.

```sh
python tools/checks/check_path_boundaries.py
python tools/checks/check_path_contract.py
./build_test.sh
python tools/checks/check_native_paths.py --cooker build_release/tools/vkr_mesh_cooker --hdr build_release/tools/vkr_hdr_cube_packer --packer build_release/tools/vkr_vkt_packer
python tools/checks/check_path_lifecycle.py --mesh-cooker build_release/tools/vkr_mesh_cooker --texture-packer build_release/tools/vkr_vkt_packer
```

Use `.bat` and `.exe` counterparts on Windows. Native-path fixtures cover Unicode
arguments, external glTF buffers/images and deep paths. The lifecycle check uses
fresh job processes for create, prepare, rebuild, reimport, relocation and prepare,
checking stable asset identity, old revisions, saved edits and portable metadata.
C tests exercise the same scene-path resolver used by request generation; this is
not a substitute for the pending interactive UI gate. Synthetic assets isolate
filesystem behavior without a GPU; renderer activation uses Bistro separately.

The Windows process test checks actual child-directory identity and redirected
log bytes. On volumes without short names it verifies explicit long-cwd rejection,
reports that activation gate unavailable, and still checks deep log paths with a
supported working directory. This is not a successful long-cwd activation result.

## Consequences and alternatives

There are separate host, managed and format-reference boundaries, rather than a
single normalizer that erases meaning. C and Python share test data without a new
runtime dependency between languages. Existing filesystem ownership is extended
instead of adding a second platform I/O stack. New consumers must use these
owners; narrower path limits must fail explicitly before publication.

Relying on the Windows active code page would leave behavior machine-dependent.
Replacing every backslash would corrupt POSIX filenames and unrelated metadata.
Accepting all normalized strings would erase the managed-reference grammar.

## Revisit when

New remote URI schemes, arbitrary device paths, externally shared mutable
workspaces or a different runtime resource-reference syntax are required.

## Implementation

- [Filesystem API](../../lib/src/filesystem/filesystem.h), [C++ native bridge](../../lib/src/filesystem/vkr_filesystem_cpp.h), [startup](../../lib/src/platform/vkr_entry.h)
- [Asset resolver](../../lib/src/filesystem/vkr_asset_path.c), [project store](../../editor/src/editor_project_store.c), [project jobs](../../tools/editor_project_jobs.py)
- [Cgltf adapter](../../tools/assets/cgltf_impl.c), [KTX adapter](../../tools/assets/vkr_ktx_file.h)
- [Shared managed corpus](../../tests/fixtures/paths/managed.json), [native lexical corpus](../../tests/fixtures/paths/native.json), [C tests](../../tests/src/asset_path_test.c)
