# FidelityFX SDK 1.1.4 dependency

VKR obtains only `sdk/` from the official FidelityFX SDK repository at configure
time. The sparse checkout pins tag `v1.1.4` to commit
`c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`.

The build enables the Vulkan FSR 3.1.4 upscaler component only. It excludes
frame interpolation, optical flow, samples and media. The generated SDK source
tree lives in the build directory and is never committed.

`fidelityfx-sdk-1.1.4-luma-history-format.patch` carries the compatibility
changes required for the embedded upscaler-only build. It changes the Vulkan
GLSL luma history storage image from `rgba8` to `rgba16f`; the component
creates that image as `R16G16B16A16_FLOAT`. It forces the backend's
FP32/wave32 capability path because VKR does not enable `shaderFloat16` or
subgroup-size control. It accepts the parent-provided `FFX_PLATFORM_NAME` so
Ninja does not receive Visual Studio's platform setting, and fixes the shader
output-variable export that otherwise leaves a literal missing Ninja input.
MSVC-only Debug flags are conditional on the selected compiler frontend.
Finally, it clears the frame-generation swapchain callback under
`VKR_FSR_UPSCALER_ONLY`; the embedding also excludes the frame-interpolation
swapchain sources. No frame-generation or presentation hook enters the binary.

`VkrVulkanFsrSdk` is the sole owner of the SDK context, SDK scratch bytes, and
the three FSR upscaler shared images (dilated depth, dilated motion, and
reconstructed previous nearest depth). It creates those images from
`ffxFsr3UpscalerGetSharedResourceDescriptions` in a dedicated
`FFX_EFFECT_SHAREDRESOURCES` backend context. The bridge object uses the
caller's `VkrAllocator`. Its 2-context SDK scratch allocation uses a separate
context-lifetime `Arena`, reserved and committed to the exact SDK scratch byte
count plus the arena header/alignment; this avoids consuming the bounded graph
allocator. Creation logs the requested, reserved, and committed bytes. The
SDK backend owns its Vulkan images. The caller imports only HDR color,
forward-Z color depth, motion, optional reactive/composition masks, and output.
It records into the supplied graphics command buffer without submitting or
waiting, and destruction/recreation requires caller-proven device idleness.

The shared images are not graph resources and have one persistent instance.
For each dispatch the bridge obtains an `FfxResource` from the shared backend
context, then the upscaler context imports it as a dynamic resource. Its
unregister step transitions that dynamic import back to its initial UAV state
and advances the SDK's four-frame view ring. Sequential dispatches are safe on
the caller's single ordered graphics queue. The caller's completion-proven
three-entry dispatch retirement ring prevents reuse of SDK dynamic views before
their recorded GPU work completes; a cancelled recording recreates the bridge
before the slot is reused.
