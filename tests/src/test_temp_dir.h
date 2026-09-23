#pragma once

#include "defines.h"

#if !defined(_WIN32)
/**
 * Creates a directory from a mkdtemp template (ending in XXXXXX) and
 * registers it for removal when the tester exits. Removal also runs when a
 * failed assertion aborts the process, so a failing test leaves no fixture
 * behind.
 */
bool8_t vkr_test_temp_dir_create(char *template_path);

/** Recursively removes `path`; a missing path counts as removed. */
bool8_t vkr_test_remove_tree(const char *path);
#endif
