#pragma once

#include "defines.h"

/**
 * Runs the hardware video codec tests (lib/src/platform/vkr_video.h); a
 * machine without a hardware H.264 codec skips them.
 * @return true if all tests passed, false if one or more tests failed
 */
bool32_t run_video_tests(void);
