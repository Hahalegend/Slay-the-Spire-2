/* vulkan_shim.h -- Android-shaped Vulkan over the Switch VI surface.
 * See vulkan_shim.c.  MIT license; see LICENSE. */

#ifndef __VULKAN_SHIM_H__
#define __VULKAN_SHIM_H__

// dlsym on the fake libvulkan.so handle. Returns the shimmed entry point for
// the four the loader fetches by name, and defers everything else to Mesa's
// real vkGetInstanceProcAddr.
void *vulkan_shim_find(const char *symbol);
void *vulkan_shim_device_override(const char *name);

// 1 if Mesa came up and reports both VK_KHR_surface and VK_NN_vi_surface.
// Called once before the engine starts, so a missing driver is a log line
// here rather than a failure deep inside the renderer.
int vulkan_shim_probe(void);

#endif
