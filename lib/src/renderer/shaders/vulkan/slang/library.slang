// The exposure kernel declares VkrExposureState, which the fullscreen utility
// root points at, so it precedes the resource declarations.
#include "../../shared/exposure_kernel.slangh"
#include "common/resources.slangh"
#include "../../shared/shadow_kernel.slangh"
#include "ibl/common.slangh"
#include "common/vertex.slangh"
#include "post/tonemap.slangh"

#include "ibl/default.slang"
#include "world/default.slang"
#include "world/deferred.slang"
#include "picking/default.slang"
#include "common/fullscreen.slang"
#include "post/exposure.slang"
#include "post/default.slang"
#include "text/default.slang"
