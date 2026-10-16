#pragma once

#include "core/vkr_codec.h"

/* The zstd decoder `.vkpak` version 2 compressed chunks decode through
 * (filesystem/vkr_vfs.h). The runtime links zstd with its KTX reader; the
 * foundation library links no codec, so every process that mounts archives
 * installs this before vkr_vfs_mount_startup(). */

/** Installs the zstd decoder into the content mounts. */
void vkr_content_codec_install(void);

/** The same zstd decoder as a codec with no compressor: recordings read
 * through it (vkr_recording_format.h); writing needs the full zstd the
 * editor carries. */
const VkrCodec *vkr_content_codec_zstd_decoder(void);
