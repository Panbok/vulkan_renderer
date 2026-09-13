#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#undef CGLTF_IMPLEMENTATION
#include "assets/vkr_cgltf.h"
#include "filesystem/filesystem.h"
#include <errno.h>

static cgltf_result vkr_cgltf_read(const cgltf_memory_options *memory,
                                   const cgltf_file_options *options,
                                   const char *path, cgltf_size *size,
                                   void **data) {
  (void)options;
  FILE *file = file_fopen(path, "rb");
  if (!file) {
    return errno == ENOENT ? cgltf_result_file_not_found
                           : cgltf_result_io_error;
  }
  cgltf_size bytes = size ? *size : 0;
  if (!bytes) {
#if defined(_WIN32)
    int seek_result = _fseeki64(file, 0, SEEK_END);
    int64_t length = _ftelli64(file);
    int rewind_result = _fseeki64(file, 0, SEEK_SET);
#else
    int seek_result = fseeko(file, 0, SEEK_END);
    int64_t length = ftello(file);
    int rewind_result = fseeko(file, 0, SEEK_SET);
#endif
    if (seek_result || length < 0 || rewind_result ||
        (uint64_t)length > SIZE_MAX) {
      fclose(file);
      return cgltf_result_io_error;
    }
    bytes = (cgltf_size)length;
  }
  void *(*allocate)(void *, cgltf_size) =
      memory->alloc_func ? memory->alloc_func : cgltf_default_alloc;
  void (*release)(void *, void *) =
      memory->free_func ? memory->free_func : cgltf_default_free;
  void *buffer = allocate(memory->user_data, bytes ? bytes : 1);
  if (!buffer) {
    fclose(file);
    return cgltf_result_out_of_memory;
  }
  cgltf_size read_size = fread(buffer, 1, bytes, file);
  int close_result = fclose(file);
  if (read_size != bytes || close_result) {
    release(memory->user_data, buffer);
    return cgltf_result_io_error;
  }
  *data = buffer;
  if (size) {
    *size = bytes;
  }
  return cgltf_result_success;
}

cgltf_file_options vkr_cgltf_file_options(void) {
  return (cgltf_file_options){.read = vkr_cgltf_read,
                              .release = cgltf_default_file_release};
}
