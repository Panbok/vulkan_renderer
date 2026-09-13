#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include "filesystem/filesystem.h"
#ifdef __cplusplus
}
#endif
#include <ktx.h>

/* KTX named-file APIs use the CRT locale. Keep stream ownership here and hand
 * the codec an already-open UTF-8/long-path stream. */
static inline KTX_error_code vkr_ktx_read_file(const char *path,
                                               ktxTextureCreateFlags flags,
                                               ktxTexture2 **texture) {
  FILE *stream = file_fopen(path, "rb");
  if (!stream) {
    return KTX_FILE_OPEN_FAILED;
  }
  KTX_error_code result =
      ktxTexture2_CreateFromStdioStream(stream, flags, texture);
  int closed = fclose(stream);
  if (result == KTX_SUCCESS && closed) {
    if (*texture) {
      ktxTexture_Destroy(ktxTexture(*texture));
      *texture = NULL;
    }
    return KTX_FILE_READ_ERROR;
  }
  return result;
}

static inline KTX_error_code vkr_ktx_write_file(ktxTexture *texture,
                                                const char *path) {
  FILE *stream = file_fopen(path, "wb");
  if (!stream) {
    return KTX_FILE_OPEN_FAILED;
  }
  KTX_error_code result = ktxTexture_WriteToStdioStream(texture, stream);
  int closed = fclose(stream);
  return result == KTX_SUCCESS && closed ? KTX_FILE_WRITE_ERROR : result;
}
