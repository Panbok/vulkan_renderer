#pragma once

#include "defines.h"

bool32_t run_harness_tests(void);

/* Child mode of the process-lock test: takes `lock_name`, starts a
   background process and exits holding the lock. */
int harness_lock_owner_test_child(const char *lock_name,
                                  const char *lock_directory);
