---
status: partial
updated: 2026-10-21
authority: adr
---

# ADR-123: Asset streaming from a depot

## Status

Accepted (partial). Steps 1 and 2 of phase 6 of the
[network protocol proposal](../proposals/network-protocol.md#asset-streaming-use-case-4b)
are implemented: a program reads a scene from a depot server without a full
download, every file arriving when a loader first asks for it; cooked
platform files travel on a side branch per GPU family; and a material's
large texture first draws from the smallest mips at the start of its file,
then from the whole file once a frame draws it. A sparse working copy holds
on disk only the files a reader opened, and commits and pulls without the
others. A queued download whose texture no frame draws any more is
cancelled. The editor's Get can make a sparse working copy, whose project
opens and Submits with Bakery processes that read the copy themselves. Mesh
LOD ranges, priority from the view, staging fetched bytes straight into GPU
memory and a run between two computers stay in the proposal.

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

[vkr_depot_remote.h](../../tools/depot/vkr_depot_remote.h) (its connection
allocates from memory of its own while its worker pumps it) makes a local
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

### Parts of files

A blob lists its chunks with their sizes, so the chunks that hold a byte
range are known before any of them arrive
(`vkr_depot_blob_range`, [vkr_depot_object.h](../../tools/depot/vkr_depot_object.h)):

- `vkr_depot_remote_read(remote, path, offset, size, destination)` copies a
  range of a listed file. A present file reads from the disk. Otherwise
  `vkr_depot_client_fetch_ranges`
  ([vkr_depot_client.h](../../tools/depot/vkr_depot_client.h)) fetches the
  blob alone, then only the chunks the store lacks that hold the range, and
  `vkr_depot_read_blob_range`
  ([vkr_depot_workspace.h](../../tools/depot/vkr_depot_workspace.h)) copies
  the bytes out of them. The file is not written.
- `vkr_depot_remote_pending` answers whether a listed file is not yet whole
  under the directory, without the network. `vkr_depot_remote_prefetch`
  queues a whole file and returns at once, and `vkr_depot_remote_cancel`
  drops a queued prefetch whose download has not started; a file a fetch
  asked for, or one in a transfer, goes on. A cancelled file may be
  prefetched again, so the prefetch queue compacts when it fills.
  `vkr_depot_remote_arrivals` counts the files written whole, without the
  lock.
- The worker serves every waiting read in one transfer first, then every
  waiting fetch, then prefetches in transfers of at most 16 MiB of listed
  files, so a read or fetch waits behind at most that much background work.
  A prefetched file that a loader then asks for moves to the fetch queue.
- A whole-file transfer passes every blob, including one the store holds: a
  read may have brought it with only some of its chunks. The chunks a read
  brought do not travel again.

### VFS remote

`vkr_vfs_set_remote(directory, calls, context)` installs such a directory
for the whole process ([vkr_vfs.c](../../lib/src/filesystem/vkr_vfs.c)).
`VkrVfsRemoteCalls` ([vkr_vfs.h](../../lib/src/filesystem/vkr_vfs.h)) holds
`fetch`, `pending`, `read`, `prefetch`, `cancel`, `list` and `arrivals`;
`vkr_depot_remote_vfs_calls()` returns the remote's. Before a read-only
open, map, existence check, stat or `fopen` of a path below the directory
reaches the disk, and before the workspace overlay looks for its copy
there, the file seam (`fs_vfs_remote`,
[vkr_filesystem_internal.h](../../lib/src/platform/vkr_filesystem_internal.h))
passes the path, relative and `/`-separated with each run of separators as
one, to `fetch`. Writes never ask.
Loader workers block in the fetch, so a scene's files download in the
parallel batches its loaders ask for. `vkr_vfs_remote_pending`,
`vkr_vfs_remote_read`, `vkr_vfs_remote_prefetch` and
`vkr_vfs_remote_cancel` resolve a path as a read does (archives, then the
overlay's copy, then the path) and call the others without fetching; `vkr_vfs_remote_arrivals` returns the
remote's count. The lib knows no depot: the program installs the calls, as
it installs the content decompressor.

Folders and names answer for files the remote has not written, without
fetching them. `file_list_directory` adds a folder's unwritten files and
subfolders from `list` to the disk's names, each once, and lists a folder
that holds only such files. `file_exists` is true for that folder
(`vkr_vfs_remote_directory`). `file_path_resolve` of an unwritten file or
folder resolves its parent folder and appends the name
(`fs_vfs_remote_resolve`), so a caller that resolves a manifest before it
reads it, as project discovery does, finds the file.

### Progressive mips

KTX2 stores a texture's levels from the smallest up, after a header, level
index and key/value data of under 1 KB, so the start of a `.vkt` holds a
complete chain of small mips:

- The material system's first load of a texture outside a replacement adds
  `&preview=1` to its request
  ([vkr_material_system.c](../../runtime/src/renderer/systems/vkr_material_system.c)).
- With that flag, a `.vkt` the remote holds but has not written whole and
  that is larger than 1 MiB loads from its first 256 KiB alone
  (`vkr_texture_decode_ktx2_prefix`,
  [vkr_texture_system.c](../../runtime/src/renderer/systems/vkr_texture_system.c)):
  KTX-Software parses the header, level index, format descriptor and
  key/value data without image data, and the levels within the load limit
  that end in the prefix upload. Only single-layer 2D chains of native
  blocks without supercompression take this path; everything else, and any
  failed read, reads the whole file as before. A Bistro texture of 4096
  texels in ASTC 6x6 previews at 512 texels, one in BC7 at 256. The load's
  metric takes such a file's size from the remote's list
  (`vkr_resource_system_metric_source_bytes`,
  [vkr_resource_system.c](../../runtime/src/renderer/systems/vkr_resource_system.c)),
  since its stat would wait for the whole file before the preview.
- The result reports the whole stored chain (`limit_source_extent` and
  `limit_source_mip_levels`), so the load-limit reload of
  [ADR-012](012-texture-compression-pipeline.md) sees that a load at the
  same limit would hold more mips. It is a preview
  (`VkrTexture.preview`) only when it holds fewer mips than that load would;
  a limit the prefix already covers loads complete. A preview registers
  under its request name; any other result registers under the name without
  `preview`, so it shares the texture of the plain request.
- That reload follows ADR-012's order: drawn materials first, at most 32
  outstanding, and a texture no frame draws keeps its preview until one
  does. A preview's reload records the texture it replaces and waits in the
  queue without a load slot until `vkr_texture_system_full_file_ready`
  finds the file whole; its first check queues the file's prefetch, and
  later checks run only after the remote wrote another file. Only drawn
  previews therefore download whole, and the reload then reads the local
  file without waiting.
- A waiting reload whose material no frame drew for 120 draw epochs ends
  and cancels its queued download (`vkr_texture_system_cancel_full_file`).
  A frame draws every material of a loaded, visible world source, wherever
  the camera looks, so hidden or unloaded geometry ends a download's
  interest and a turned camera does not. ADR-012's deferral queues it again
  once a frame draws the material. A material's unload, and a reload the
  limit no longer needs, cancel it too.
- Reload requests ask for a preview too, and only a reload that grows a
  preview waits for its file: one the limit shrinks loads complete from the
  preview's mips without the download.
- `VkrTextureSystem.preview_count` counts live previews; the evaluator
  reads it as `stats.preview_textures`
  ([ADR-075](075-editor-cmd-bar-and-evaluator.md)).

### Sparse working copy

A sparse working copy tracks every file of its base but holds on disk only
the files it fetched
([vkr_depot_workspace.h](../../tools/depot/vkr_depot_workspace.h)):

- `vkr_depot clone ... --sparse` (`VkrDepotCloneConfig.sparse`) fetches
  the commit and its trees and writes only the root `.vkrdepotignore`,
  whose rules every snapshot needs. Each other index entry carries
  `VKR_DEPOT_MTIME_UNFETCHED` with its blob. Such a working copy saves
  state version 2 with a sparse flag, which earlier programs refuse
  instead of reading every unfetched file as deleted; other working copies
  keep version 1.
- A remote content directory opened on the working copy
  (`VkrDepotRemoteConfig.workspace`) lists its unfetched files at its base
  and the cooked side branch's files it does not track, keeps their objects
  in the working copy's store and takes its server, key and branch from the
  working copy's state. It counts any file on disk as present and never
  writes over one, and it appends `<blob> <path>` to `.vkrdepot/fetched`
  for each file it writes.
- A snapshot keeps an unfetched entry whose file is not on disk as
  unchanged, with its blob, unless the fetched log names the path with that
  blob: that file arrived and was deleted since, which is a deletion. A
  file that arrived is walked and hashed like any other, and status records
  its time when it is unchanged. A commit or pull clears the log once the
  new index holds what it said.
- A pull (`vkr_depot_fetch_checkout`) of a sparse working copy fetches the
  commit's trees and only the blobs its checkout writes: files on disk that
  the commit changed, and the root ignore rules. The checkout leaves every
  other file of the target unfetched with the target's blob.
- `vkr_depot status` reports how many of the tracked files are not
  fetched, and `vkr_depot commit` the objects and bytes it uploaded
  (`VkrDepotPendingCommit.uploaded`).

### Cooked side branch

`<branch>.cooked.bc` or `<branch>.cooked.astc`
(`vkr_depot_cooked_branch`, [vkr_depot_sync.h](../../tools/depot/vkr_depot_sync.h))
holds the working copy's untracked `.vkt` files and nothing else:

- `vkr_depot cooked [DIR]` hashes them into the store first
  (`vkr_depot_cooked_snapshot`, without a connection, since hashing
  hundreds of megabytes outlasts the idle timeout), then commits them on
  top of the side branch's newest commit with the message `Cooked for
  <full id of the working copy's base>` and moves the branch by
  compare-and-swap (`vkr_depot_publish_cooked`). The same files as the
  branch holds publish nothing. The project's branch and the working
  copy's index stay as they were.
- `.vkrdepot/cooked` caches the cooked files' sizes, times and blobs, so a
  cooked snapshot hashes only files that changed since the last snapshot
  or fetch.
- A streaming client lists the side branch for its own family only, so a
  Windows computer never receives ASTC blocks.

### Project sharing

Get and Get latest ([ADR-110](110-project-sharing-in-the-editor.md)) fetch
the cooked side branch for this computer after the checkout and before
the cook (`vkr_depot_fetch_cooked`), so the cook makes only what it lacks:

- A file already here stays, and a path the index tracks is never
  written.
- A `<source>.vkt` is taken only when its source's blob in this working
  copy equals the source's blob at the commit it was cooked for; a changed
  source cooks here instead. Other cooked files, such as a build's
  textures, belong to a build revision and are taken where missing, as
  `recook_textures` treats them.
- After a Get or Get latest that cooked anything, and after every Submit,
  the editor publishes its cooked files to the side branch; the cooked
  cache keeps that to the files that changed. Neither failure fails the
  step: a missing cooked file only cooks here.
- `depot.status` reports the textures the last Get cooked, fetched and
  failed to cook.

### Sparse project sharing

The Get dialog's **Fetch files when opened** (`get.sparse`, the
`depot.clone` operation's `sparse`) makes the copy a sparse working copy
(`vkr_editor_depot_clone`, [editor_depot.c](../../editor/src/editor_depot.c)):

- The Get writes only the ignore rules: it fetches no cooked side branch
  and cooks nothing, since every file arrives when a reader opens it. It
  opens the first project its index tracks.
- The editor holds one remote content directory on the open workspace
  when that workspace is a sparse working copy
  (`vkr_editor_depot_workspace_sparse`). It attaches the remote before the
  workspace opens, since the project folders themselves arrive when read,
  goes back to the previous workspace's remote when a switch does not
  happen, and closes it with the workspace. The remote lists the cooked
  side branch, so the copy's cooked textures come from it as they are
  read.
- The editor sets `VKR_DEPOT_SPARSE_WORKSPACE` to the copy's folder for
  the processes it starts. A Bakery process with that variable opens its
  own remote on the copy and installs it as its VFS remote
  (`bakery_sparse_open`,
  [vkr_bakery_main.c](../../tools/bakery/vkr_bakery_main.c)). Its file
  tests (`vkr_bakery_stat`,
  [vkr_bakery_os.c](../../tools/bakery/vkr_bakery_os.c)) answer an
  unwritten file from the remote's list, with its listed size, and its
  listings add unwritten names; reads fetch. A scene's preparation checks
  that every artifact exists and so fetches only the files it reads.
  The remote's own presence test stays native
  (`vkr_bakery_stat_native`).
- A sparse checkout creates the folder of every file it leaves unfetched,
  so a native folder test, such as the workspace's real-path check, finds
  it.
- Get latest on a sparse copy pulls and reloads, and skips the cooked
  fetch, the cook, the recook and the publish; Submit commits and skips
  the cooked publish, since a copy that holds some cooked files would
  publish a side branch without the others.
- `stream.status` reports the editor's remote on a sparse copy with
  `"sparse": true`.

### Editor

`vkr_editor --stream <address> <server key> <directory>` (with
`--stream-branch <name>` and `--stream-no-cooked`) opens a remote content
directory during the UI's initialization, before the scene loads, and makes
it both the VFS remote and the workspace overlay
(`vkr_editor_depot_stream`, [editor_depot.c](../../editor/src/editor_depot.c)).
Every `assets/` read the branch lists then comes from the server, even
where this checkout holds the same file. The `stream.status` operation
answers the commits, the files listed, fetched, present and failed, the
bytes stored, the reads of parts of files with the bytes they brought, the
files prefetched and the prefetches cancelled, the time loaders waited and
the times of the last file and the last read.
`vkr_editor --stream-workspace <directory>` opens such a directory on a
sparse working copy instead, so a scene opens from the copy and the editor's
writes through the overlay, such as a new material, land in it.
`vkr_depot stream <address> <key> <directory> <path>` fetches one file the
same way, for a quick check.

## Consequences

- A scene opens after its trees and the files its first loads read arrive,
  not after the whole commit.
- Loader workers wait on the network. Their wait adds up across threads
  and does not stall the frame, but a slow link delays each texture's first
  draw, as loading from a slow disk does.
- A path the branch does not list reads from the local disk as before.
- A listed file is written whole before any reader of the file sees it; a
  material's large texture draws from its preview until its file is whole
  and a frame draws it. A texture no frame draws for 120 draw epochs before
  its file arrives lets its queued download go; one already in a transfer
  finishes.
- A preview is a second texture and publication for the same slot; the
  reload releases it through the texture system's retirement when the whole
  chain binds.
- On loopback, previews cover every texture about 21 s sooner and full
  detail arrives 3 to 6 s later than whole files alone (Evidence): the
  reads go first, and at most 32 reloads, so 32 prefetched files, wait at
  once.
- Listings and existence tests through the lib and Bakery's file calls see
  a sparse copy's unfetched files; a native call outside them, in a tool
  without the remote, sees only the files on disk.
- One process has one VFS remote, so an editor holds one sparse working
  copy open at a time.
- The scene sidecar's save does not ask the overlay
  (`vkr_vfs_overlay_write_path`): it writes beside the scene it loaded, so
  an edited repository scene saves into the repository, not the working
  copy. Material saves do ask it.
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
- **A reload per level, smallest first**: each level a new texture and
  publication; two steps give the first view most of the gain.
- **Prefetching every previewed file at once**: downloads textures no frame
  draws; the reload gate keeps downloads to drawn materials in ADR-012's
  order.
- **Upgrading a preview in place behind the same handle**: no second
  texture, but the publication would have to swap a resource under live
  materials and retire the old one; the reload path already does both.
- **Telling a fetched file from an unfetched one by its blob in the
  store**: no log, but a read of part of a file brings its blob without
  writing it, and two paths share one blob, so a missing file would read as
  deleted.
- **A sparse checkout through the remote**: one writer for every file, but
  pull and commit would need the editor running; the fetched log lets the
  command line and the editor share the working copy.
- **The editor fetching each scene's files before its first Bakery job**
  (instead of Bakery processes reading the copy): Bakery would stay
  unchanged, but the editor would have to know a scene's files before its
  preparation computes them, and would fetch every file a job tests
  whether or not the job reads it.

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
- 2026-10-18, Get with the cooked side branch, `build_debug` and
  `build_release`, Windows:
  - `--suite depot` passes `test_depot_remote`'s Get part: a member's
    cooked fetch writes the texture with the author's bytes and caches it,
    a second fetch finds it present and writes nothing, and after the
    author changes its source without publishing, the fetch counts it
    stale and writes nothing, so the cook makes it.
  - Two editor Gets (`depot.clone`) of a copy of this computer's depot
    store (the cross-machine project, 802 MB) over loopback, each from an
    empty home: the first took 85 s, cooked one folder and published 5
    cooked textures to `main.cooked.bc`; the second took 90 s, took the
    same 5 from the side branch and cooked none. That project holds few
    cooked textures, so the times say nothing about Bistro's 511. In both,
    one scene's `recook_textures` failed with "Re-cook scene does not match
    project membership", from the unchanged re-cook step.
  - On the LAN: `windows-d` (`D:\vkr-depot\store`) holds `main`
    `74c285f3ea6d` and `main.cooked.bc` `2650088eb830`, published from its
    working copy with `vkr_depot cooked`. A Release editor's Get from
    `192.168.3.2:7320` into an empty home took 85 s, took the 5 cooked
    textures from the side branch and cooked none.
  - Mac M1, Release at `feab49e5`, two Gets from `windows-d` over Wi-Fi,
    each into an empty home: the first (108 s) fetched nothing, cooked the
    5 textures and published them to `main.cooked.astc`; the second (82 s)
    took all 5 from it and cooked none. Its 5 `.vkt` files are KTX2 format
    166 (ASTC 6x6 sRGB) with the first Get's SHA-256 hashes; none came
    from `main.cooked.bc`. The Mac's mirror then copied
    `main.cooked.astc` into its own store.
  - Every Get of that project counted one failure: its `project.json`
    lists the Bistro scene, which the depot does not share, and
    `recook_textures` for a missing scene fails. Get now skips a listed
    scene whose `scene.json` the working copy lacks; a Windows Get from
    `windows-d` then took the 5 BC textures and reported no failure.
  - That run found the depot reading `VKR_DEPOT_IDENTITY`, the user name
    and the host name with `getenv`, which on Windows gives the active
    code page: a key under a non-ASCII user folder failed with "no depot
    identity key". They are read as UTF-8 now
    (`vkr_platform_environment_variable`).

- 2026-10-21, progressive mips, Mac M1 Pro (16 GB), Metal, loopback:
  - `vulkan_renderer_tester` (`build_debug`, ASan and UBSan): `--suite
    depot` passes `test_depot_remote_ranges`. A read of the first 200,000
    bytes of a 3 MB file brings its blob and the chunks that hold them
    (within 16 KiB of those chunks' sizes, under half the file), returns the
    file's bytes and writes no file. A read past the end or of an unlisted
    path fails. A prefetch then writes the whole file, equal to the
    original, and stores less than the file minus the chunks the reads
    brought. A read of the present file does not go to the server.
    `--suite filesystem` passes `test_vfs_remote_directory`, whose pending
    file reads in part and queues without arriving. `--suite vkt` passes
    `test_texture_ktx2_remote_preview`: a `preview=1` load of a pending
    1.4 MB BC7 chain of 1024 texels reads only its first 256 KiB, holds
    levels 2 to 10 and reports 1024 texels in 11 levels; at a 128-texel
    limit the prefix loads complete and is no preview; without the flag, or
    once the file arrived, the whole chain loads. `--suite material`,
    `texture`, `mesh` and `scene_edit` pass.
  - `python3 tools/checks/check_asset_stream.py --editor
    build_release/editor/vkr_editor --depot
    build_release/tools/depot/vkr_depot`, Release, passes three times, the
    last with this change on top of `e3952013`. The local run read 782
    files (1,964 MB, 511 of them ASTC textures; `main.cooked.astc` uploaded
    897 MB). Streamed: 775 files fetched, none failed, every one equal to
    the local file; 457 to 460 reads brought 83.3 MB, 370 files were
    prefetched and 846.1 MB more was stored. From the stream's opening: the
    scene was ready at 7.3 to 8.2 s; from 11.3 to 12.3 s only the 32
    waiting reloads were pending, so every texture had at least its preview
    (at most 367 previews at once); no texture was pending from 36.5 to
    39.7 s, and no preview was left at the capture. The captures differ
    from a local load's by 0.107, 0.299 and 0.308 of 255, where two local
    loads differ by 0.335, 0.345 and 0.229.
  - The same check with the binaries of `590d704c`, before progressive
    mips (a detached worktree with the same assets): the scene was ready at
    7.0 s, 496 textures were still pending at 13.1 s, and none from 33.3 s;
    it stored 929.4 MB, the sum of the streamed run's reads and fetches, so
    no chunk travels twice. Previews put every texture on screen about 21 s
    sooner and full detail arrives 3 to 6 s later, in one run before and
    three after. A local load is ready 5.4 to 8.0 s after it begins, with
    every texture about 2 s later.
  - The first run with previews failed its capture comparison, 0.556
    against 0.278: the local runs captured 30 s after their scene and the
    streamed run about 100 s after, and Bistro's final colour drifts while
    it runs. One local run's captures 30 s and 100 s after its scene differ
    by 0.455, and the streamed capture differs from a local capture at
    100 s by 0.421. Every run of the check now captures `--settle` (120) s
    after its scene.
  - Not measured: BC previews on Windows and the desktop pipeline, two
    computers, a link slower than loopback.
- 2026-10-21, sparse working copy, Mac M1 Pro, Metal, loopback:
  - `--suite depot` (`build_debug`, ASan and UBSan) passes
    `test_depot_sparse`. A sparse clone of five files writes only
    `.vkrdepotignore` and saves state version 2 (a plain clone keeps 1), and
    status shows no change. A remote on the copy lists the four unfetched
    files, keeps a file written over an unfetched one and fetches the
    others. After an edit, the deletion of a fetched file and an addition,
    status lists exactly those, and the file never fetched stays unchanged.
    The commit uploads at most 9 objects and keeps the unfetched file's
    blob; a plain clone of it holds every file's content. A pull of another
    user's commit rewrites the fetched file it changed and leaves the file
    never fetched and a new one unfetched, and the next reads bring both
    with the new content.
  - `python3 tools/checks/check_sparse_workspace.py --editor
    build_release/editor/vkr_editor --depot
    build_release/tools/depot/vkr_depot`
    ([check_sparse_workspace.py](../../tools/checks/check_sparse_workspace.py))
    passes. `main` held the 271 files a local Bistro run read, the ignore
    rules and the 36 files of `assets/characters` and `assets/models/alps`,
    which Bistro never opens (308 files), with the cooked textures on
    `main.cooked.astc`. The sparse clone held one file, 307 of 308 not
    fetched. The editor opened Bistro with `--stream-workspace`: the scene
    was ready 8.1 s after the stream opened and no texture was pending from
    38.4 s; 775 files arrived (459 previews read), none failed. One
    `material.create` copied a Bistro material into the working copy.
    Status then listed that addition alone; 266 of 308 files were on disk,
    every one a file the local run read and none of the 36. The commit
    uploaded 4 objects (659 bytes), a second sparse clone lists 309 files,
    and the material read back from the depot equals the working copy's.
    The capture differs from a local load's by 0.279 of 255, where two
    local loads differ by 0.323.
  - Not measured: a sparse working copy on Windows or between two
    computers.
- 2026-10-21, cancelled downloads, Mac M1 Pro, Metal, loopback:
  - `--suite depot` passes `test_depot_remote_cancel` three times in a row:
    of three prefetched 6 MB files, the third, cancelled while the first
    transfer ran, is not written and stays pending, the other two arrive, a
    fetch of the third still brings it with the author's bytes, and
    cancelling an arrived file counts nothing. `--suite filesystem` passes
    with `vkr_vfs_remote_cancel` reaching the remote; `material`, `vkt`,
    `texture` and `mesh` pass.
  - `python3 tools/checks/check_asset_stream.py --editor
    build_release/editor/vkr_editor --depot
    build_release/tools/depot/vkr_depot --hide-after 3 --settle 60`
    passes. The streamed editor hid `Bistro Lights` 12.8 s after the stream
    opened, when 389 files had arrived and no whole file was queued yet. The
    reloads of the textures drawn until then queued 32 downloads; 30 were
    cancelled once their materials went 120 draw epochs undrawn, the last
    file arrived at 13.3 s, and 407 of 783 files arrived in all, with
    65.7 MB stored against 846.1 MB when Bistro stays in view. Every file
    written equals the local one, none failed.
  - A first attempt moved the camera 20 km away instead and cancelled
    nothing: a frame draws the materials of every visible world source,
    wherever the camera looks.
- 2026-10-21, sparse project sharing, Mac M1 Pro, Metal, loopback:
  - `--suite filesystem` (`build_debug`) passes `test_vfs_remote_directory`
    with folders that hold only unwritten files: they exist, list their
    files once beside the disk's names, resolve below the directory's
    resolved path, and a path that doubles a separator finds its file, all
    without a fetch. `--suite depot` passes the remote's listing and the
    Bakery hooks: `vkr_bakery_stat` gives an unwritten file its listed size
    where the native stat finds nothing, its listing names the file, and
    neither downloads. `metrics`, `resource_async_state`, `texture_vkt` and
    `texture_lifetime` pass.
  - `python3 tools/checks/check_sparse_project.py --editor
    build_release/editor/vkr_editor --depot
    build_release/tools/depot/vkr_depot --bakery
    build_release/tools/bakery/vkr_bakery`
    ([check_sparse_project.py](../../tools/checks/check_sparse_project.py))
    passes. Bakery's `share_project` shared the repository's Testbed project
    trimmed to Bistro (618 tracked files, 509 cooked textures on
    `main.cooked.astc`, the World's cloud wind set to zero so the captures
    do not depend on run time). A second editor with a home of its own Got
    it with `depot.clone` and `sparse` from the shared project, with no scene
    open, and opened Bistro by name: 8.3 s against 4.2 s from the shared
    folder (Bakery's preparation 4.8 s against 0.5 s). 536 previews (80.9
    MB) were read and 782 files written, none failed, the last 86.8 s after
    the remote opened.
    After one new object, a save and a Submit, 343 of the 618 tracked files
    were never fetched (source images, glTF sources and import records), the
    Submit carried Bistro's new edits revision and `scene.json` alone and
    added 2,430 bytes to the server, no change remained, and a second
    sparse clone lists 618 files. With dynamic resolution off in every run,
    the capture differs from a local load's by 0.306 of 255, where two
    local loads differ by 0.250.
  - Defects the check found and fixed: project discovery resolved
    `project.json` with `realpath`, which failed for an unfetched file; the
    World's path doubles a separator (`<project>//world.scene.json`), so
    the remote did not list it and the editor wrote a blank World that a
    Submit would publish; the load metric's stat downloaded each texture
    whole before its preview; and missing project folders made a switch
    to the copy retry every frame.
  - An editor that opens Bistro before the Get reuses the first open's
    materials, which the material system names by file name, so the check
    starts the receiving editor with no scene open. The switch to another
    workspace with Bistro's textures resident also rejects their sampler
    updates; neither depends on the copy being sparse.
  - Not measured: Windows, two computers, a link slower than loopback.

## Revisit when

[Texture streaming](../proposals/texture-streaming.md) computes the mips
each texture needs (its required mips then replace the fixed 256 KiB
preview and the drawn-first order), mesh LOD ranges need the same
treatment, fetched bytes should land in GPU staging memory directly,
texture demand follows the camera instead of the world's visible sources,
a link is slow enough that
loaders' waits show in frame pacing, or a tool that reads a sparse copy
outside the lib and Bakery's file calls joins project sharing.
