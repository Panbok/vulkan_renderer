---
status: partial
updated: 2026-10-18
authority: adr
---

# ADR-123: Asset streaming from a depot

## Status

Accepted (partial). Step 1 of phase 6 of the
[network protocol proposal](../proposals/network-protocol.md#asset-streaming-use-case-4b)
is implemented: a program reads a scene from a depot server without a full
download, every file arriving when a loader first asks for it, and cooked
platform files travel on a side branch per GPU family. Progressive mips and
LOD ranges, staging fetched bytes straight into GPU memory, a sparse working
copy for editing, macOS and a run between two computers stay in the
proposal.

## Context

The depot ([ADR-105](105-network-transport-and-asset-depot.md)) moved whole
commits: a clone of Bistro fetched 2.7 GB before the first frame. Loaders
already read every asset through the platform file calls, which consult
the VFS first ([vkr_vfs.h](../../lib/src/filesystem/vkr_vfs.h)), and the
editor's workspace overlay already serves `assets/` reads from another
directory. Cooked textures (`.vkt`) hold BC7 and BC5 blocks on Windows and
ASTC on Apple GPUs; the project's history leaves them out
([ADR-110](110-project-sharing-in-the-editor.md)) and every receiver cooks
its own.

Owner decision of 2026-10-18: cooked platform files travel on a side branch
per GPU family, outside the project's history.

## Decision

### Remote content directory

[vkr_depot_remote.h](../../tools/depot/vkr_depot_remote.h) makes a local
directory that a depot server fills on demand:

- Opening connects, resolves a branch (default `main`) to its newest commit
  and fetches only its trees
  (`vkr_depot_client_fetch_trees`, [vkr_depot_client.h](../../tools/depot/vkr_depot_client.h)),
  which list every file with its size and blob. Files of the cooked side
  branch for this computer join the list unless the main commit lists the
  same path.
- `vkr_depot_remote_fetch` looks a path up in that sorted list. An unlisted
  path answers false at once. A listed file is queued; the caller waits
  until it is written below the directory, or answers false when its
  download failed.
- One worker thread owns the connection. It takes every queued request at
  once and fetches their blobs in one pipelined transfer
  (`vkr_depot_client_fetch_files`), then writes each file through a
  temporary and a rename. Between transfers nothing pumps the connection,
  so the transport's idle timeout may close it; a failed transfer
  reconnects once and tries again.
- Fetched objects stay in a store inside the directory
  (`.vkrdepot-stream`). A file below the directory with its listed size and
  its blob in that store counts as present, so a second run fetches only
  what changed.

### VFS remote

`vkr_vfs_set_remote(directory, fetch, context)` installs such a directory
for the whole process ([vkr_vfs.c](../../lib/src/filesystem/vkr_vfs.c)).
Before a read-only open, map, existence check, stat or `fopen` of a path
below it reaches the disk, and before the workspace overlay looks for its
copy there, the file seam (`fs_vfs_remote`,
[vkr_filesystem_internal.h](../../lib/src/platform/vkr_filesystem_internal.h))
passes the path, relative and `/`-separated, to `fetch`. Writes never ask.
Loader workers block in the fetch, so a scene's files download in the
parallel batches its loaders ask for. The lib knows no depot: the program
installs the callback, as it installs the content decompressor.

### Cooked side branch

`<branch>.cooked.bc` or `<branch>.cooked.astc`
(`vkr_depot_cooked_branch`, [vkr_depot_sync.h](../../tools/depot/vkr_depot_sync.h))
holds the working copy's untracked `.vkt` files and nothing else:

- `vkr_depot cooked [DIR]` hashes them into the store first
  (`vkr_depot_cooked_snapshot`, without a connection, since hashing
  hundreds of megabytes outlasts the idle timeout), then commits them on
  top of the side branch's newest commit with the message `Cooked for
  <base>` and moves the branch by compare-and-swap
  (`vkr_depot_publish_cooked`). The same files as the branch holds publish
  nothing. The project's branch and the working copy's index stay as they
  were.
- A streaming client lists the side branch for its own family only, so a
  Windows computer never receives ASTC blocks.

### Editor

`vkr_editor --stream <address> <server key> <directory>` (with
`--stream-branch <name>` and `--stream-no-cooked`) opens a remote content
directory during the UI's initialization, before the scene loads, and makes
it both the VFS remote and the workspace overlay
(`vkr_editor_depot_stream`, [editor_depot.c](../../editor/src/editor_depot.c)).
Every `assets/` read the branch lists then comes from the server, even
where this checkout holds the same file. The `stream.status` operation
answers the commits, the files listed, fetched, present and failed, the
bytes stored, the time loaders waited and the time of the last file.
`vkr_depot stream <address> <key> <directory> <path>` fetches one file the
same way, for a quick check.

## Consequences

- A scene opens after its trees and the files its first loads read arrive,
  not after the whole commit.
- Loader workers wait on the network. Their wait adds up across threads
  and does not stall the frame, but a slow link delays each texture's first
  draw, as loading from a slow disk does.
- A path the branch does not list reads from the local disk as before.
- A listed file is written whole before any reader sees it; a texture
  arrives at full resolution or not at all until progressive mips exist.
- The cooked side branch grows by one commit per publish, each holding
  every cooked file of that working copy; unchanged files share their
  chunks.
- `VKR_VFS_RECORD` ended a Windows process at startup:
  `setvbuf(..., _IOLBF, 0)` is an invalid parameter to the Windows C
  runtime. The record stream is unbuffered now; each record is one
  `fwrite`.

## Alternatives considered

- **A shared Bakery cache by action key** (declined 2026-10-18): finer
  reuse, but a client must resolve recipes to compute keys, and the
  [asset build proposal](../proposals/asset-build-system.md) keeps a
  network cache out of scope.
- **Cooking on the receiver**: no cooked file travels, but Bistro's first
  full-detail view waits for about 500 cooks on every new computer.
- **A remote mount inside the lib**: the lib would link the transport and
  the depot; a callback keeps it free of both.
- **Prefetching the whole commit in the background**: simpler, but it
  fetches files no view reads and holds the first frame behind the
  download.

## Evidence

Windows 10, Ryzen 5 2600, RX 6700 XT, clang, 2026-10-18.

- `vulkan_renderer_tester --suite filesystem` (`build_debug`) passes
  `test_vfs_remote_directory`: a read below the directory asks the remote
  before the disk, an unlisted file never appears, a path outside never
  asks, and the overlay asks for its copy there.
- `--suite depot` passes `test_depot_transfer`, whose sparse fetch brings
  the commit and its two trees (3 objects) and no file, then one file that
  equals the original, and `test_depot_remote` over a loopback service: the
  cooked `.vkt` publishes to `main.cooked.bc` and publishes nothing the
  second time, a remote lists 4 files and holds none, a VFS read fetches
  the cooked file with its bytes, four threads asking at once all get
  theirs, and a second remote finds the 4 files present and fetches none.
- `python tools/checks/check_asset_stream.py --editor
  build_release/editor/vkr_editor.exe --depot
  build_release/tools/depot/vkr_depot.exe`
  ([check_asset_stream.py](../../tools/checks/check_asset_stream.py)),
  Release, loopback: a local Bistro run read 782 files below `assets/`
  (3,252 MB, 511 of them cooked textures). Committed to `main` and
  published to `main.cooked.bc` (1,318 MB uploaded), they streamed into an
  empty directory: the branch listed 783 files in 113 ms, the scene was
  ready 23.4 s after the stream opened and the last file arrived at 74.6 s;
  775 files (1,348 MB stored) were fetched, none failed, and every one
  equals the local file byte for byte. The capture after full detail
  differs from a local load's by a mean of 0.715 of 255 (5.9% of pixels by
  more than 2), where two local loads differ by 0.568 (4.2%). Passes.
- Before `setvbuf` changed, every Windows editor started with
  `VKR_VFS_RECORD` exited before its first log line; it records 1,512
  reads of a Bistro load now.
- Not measured: a local load's time to the same points, a link slower
  than loopback, macOS and the ASTC side branch, two computers.

## Revisit when

A texture's first draw must not wait for its whole file (progressive mips),
fetched bytes should land in GPU staging memory directly, a link is slow
enough that loaders' waits show in frame pacing, or editing needs a sparse
working copy.
