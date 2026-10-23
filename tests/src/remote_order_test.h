#pragma once

#include "defines.h"

/**
 * Runs the view streaming client's frame order tests
 * (editor/src/editor_remote_order.h): the reorder window, gaps, key frame
 * requests and refusals.
 * @return true if all tests passed, false if one or more tests failed
 */
bool32_t run_remote_order_tests(void);
