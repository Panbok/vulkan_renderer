# RenderDoc in-application API header

`vendor/renderdoc/renderdoc_app.h` is the unmodified in-application API
header from [RenderDoc](https://renderdoc.org) v1.45 (MIT, notice at the top
of the file; API version 1.7.0), copied from the RenderDoc install.

The Vulkan backend ([`vkr_vulkan_gpu_capture.c`](../renderer/src/vulkan/vkr_vulkan_gpu_capture.c))
uses it to drive frame captures for `debug.gpu_capture` when RenderDoc's
Vulkan layer is in the process. Nothing links against RenderDoc: the backend
asks the already loaded `renderdoc.dll` for `RENDERDOC_GetAPI` and requests
API 1.6.0, so older RenderDoc installs work too.
