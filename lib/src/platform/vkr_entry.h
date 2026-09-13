#pragma once

/* CRT startup owns the wide arguments. Converted UTF-8 storage is owned by this
 * cold startup adapter until the application returns, before any VKR allocator
 * exists. No argument view survives that return. */
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static int vkr_entry_run(int argc, wchar_t **wide_argv,
                         int (*entry)(int, char **)) {
  char **argv = (char **)calloc((size_t)argc + 1u, sizeof(char *));
  int result = EXIT_FAILURE;
  if (!argv) {
    return result;
  }
  for (int i = 0; i < argc; ++i) {
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i],
                                    -1, NULL, 0, NULL, NULL);
    if (bytes <= 0) {
      goto cleanup;
    }
    argv[i] = (char *)malloc((size_t)bytes);
    if (!argv[i] ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1,
                             argv[i], bytes, NULL, NULL)) {
      goto cleanup;
    }
  }
  result = entry(argc, argv);
cleanup:
  for (int i = 0; i < argc; ++i) {
    free(argv[i]);
  }
  free(argv);
  return result;
}

#define VKR_MAIN(argc, argv)                                                   \
  static int vkr_entry_main(int argc, char **argv);                            \
  int wmain(int argc, wchar_t **argv) {                                        \
    return vkr_entry_run(argc, argv, vkr_entry_main);                          \
  }                                                                            \
  static int vkr_entry_main(int argc, char **argv)
#else
#define VKR_MAIN(argc, argv) int main(int argc, char **argv)
#endif
