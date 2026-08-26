/* vk_validate.h -- see vk_validate.c. MIT license; see LICENSE. */
#ifndef STS2_VK_VALIDATE_H
#define STS2_VK_VALIDATE_H

#include <stdint.h>
#include <vulkan/vulkan.h>

void vk_validate_note_buffer(VkBuffer b, uint64_t size);
void vk_validate_descriptor_writes(uint32_t n, const VkWriteDescriptorSet *w);
void vk_validate_note_bind(VkPipelineBindPoint bp, VkPipeline p);
void vk_validate_report(const char *why);

/* The --wrap wrappers, exposed by name.
 *
 * Godot is a dlopen'd .so, so its Vulkan calls are resolved by the wrapper's
 * own dlsym (vulkan_shim_find) and by vkGetInstanceProcAddr -- never by the
 * NRO's static link. --wrap alone therefore never saw them, which is why a run
 * with vk_validate=1 reported "buffers tracked 0" and no ACTIVE line.
 *
 * Handing these back as the resolved symbol closes that gap without a second
 * mechanism: --wrap still supplies the __real_ side, so there is exactly one
 * wrapper per function and no recursion. */
VkResult __wrap_vkCreateBuffer(VkDevice, const VkBufferCreateInfo *,
                               const VkAllocationCallbacks *, VkBuffer *);
void     __wrap_vkUpdateDescriptorSets(VkDevice, uint32_t,
                                       const VkWriteDescriptorSet *,
                                       uint32_t, const VkCopyDescriptorSet *);
void     __wrap_vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint,
                                  VkPipeline);
VkResult __wrap_vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo *,
                                     const VkAllocationCallbacks *,
                                     VkShaderModule *);

#endif
