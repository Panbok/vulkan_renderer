// The exposure kernel declares VkrExposureState, which the fullscreen utility
// root points at, so it precedes the resource declarations.
#include "../../shared/exposure_kernel.slangh"
// The bloom kernel reuses the exposure kernel's Rec. 709 luminance for its
// firefly weight, so it must follow it here and in the MSL concatenation list.
#include "../../shared/bloom_kernel.slangh"
#include "../../shared/gtao_kernel.slangh"
#include "../../shared/ggx_kernel.slangh"
#include "../../shared/normal_map_kernel.slangh"
#include "../../shared/transmission_kernel.slangh"
// VkrShL2Packed is referenced by the IBL projection kernel and by deferred
// lighting, so it precedes both the resource declarations and their consumers.
#include "../../shared/sh_l2_kernel.slangh"
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
#include "post/bloom.slang"
#include "post/gtao.slang"
#include "post/default.slang"
#include "text/default.slang"
#include "ui/default.slang"
