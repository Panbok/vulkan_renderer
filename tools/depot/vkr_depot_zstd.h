#pragma once

#include "vkr_depot_store.h"

/* The zstd codec (vkr_depot_zstd.c); the caller links zstd. */
const VkrDepotCodec *vkr_depot_zstd_codec(void);
