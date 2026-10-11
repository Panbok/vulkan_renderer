#include "fuzz_common.h"

/* The replay driver of a fuzzer built without libFuzzer: runs every file
 * named on the command line through LLVMFuzzerTestOneInput once. It finds
 * nothing new; it reproduces a crash file or replays a corpus under the
 * sanitizers. Arguments starting with '-' are libFuzzer flags and are
 * ignored, so one command line serves both builds. */

static bool8_t fuzz_replay_file(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) {
    fprintf(stderr, "cannot open %s\n", path);
    return false_v;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return false_v;
  }
  const long length = ftell(file);
  if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return false_v;
  }
  uint8_t *bytes = malloc(length > 0 ? (size_t)length : 1u);
  if (!bytes) {
    fclose(file);
    return false_v;
  }
  const size_t read = fread(bytes, 1u, (size_t)length, file);
  fclose(file);
  if (read != (size_t)length) {
    free(bytes);
    return false_v;
  }
  (void)LLVMFuzzerTestOneInput(bytes, (size_t)length);
  free(bytes);
  return true_v;
}

int main(int argc, char **argv) {
  (void)LLVMFuzzerInitialize(&argc, &argv);
  uint32_t replayed = 0u;
  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] == '-') {
      continue;
    }
    if (!fuzz_replay_file(argv[i])) {
      return 1;
    }
    replayed += 1u;
  }
  fprintf(stderr, "replayed %u inputs\n", replayed);
  return 0;
}
