#pragma once

/* The zstd decoder `.vkpak` version 2 compressed chunks decode through
 * (filesystem/vkr_vfs.h). The runtime links zstd with its KTX reader; the
 * foundation library links no codec, so every process that mounts archives
 * installs this before vkr_vfs_mount_startup(). */

/** Installs the zstd decoder into the content mounts. */
void vkr_content_codec_install(void);
