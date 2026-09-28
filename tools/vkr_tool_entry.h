#pragma once

/* Every cooker is linked into vkr_bakery and keeps its original command line
 * as one named entry. `vkr_bakery tool <name> ...` passes UTF-8 arguments
 * (converted by vkr_bakery's own VKR_MAIN) with argv[0] naming the tool. */
#if defined(__cplusplus)
#define VKR_TOOL_ENTRY(name) extern "C" int name(int argc, char **argv)
#else
#define VKR_TOOL_ENTRY(name) int name(int argc, char **argv)
#endif
