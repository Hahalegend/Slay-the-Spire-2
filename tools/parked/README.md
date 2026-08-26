# Parked: started, not finished, deliberately not built

Nothing here is compiled. The Makefile globs `source/*.c` only
(`CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))` with
`SOURCES := source`), so files in this directory are inert.

## `isa_dump.c`

Captures NAK's compiled Maxwell code at pipeline creation, via
`VK_KHR_pipeline_executable_properties` -- which this NVK does implement (15
entry points in `libvulkan.a`, all enums verified against the real
`vulkan_core.h`).

**Why it was written:** the compiled ISA exists nowhere on disk. Both the Mesa
disk cache and the `VkPipelineCache` blob store serialized NIR on this fork,
and NAK re-runs every boot. If the bug had been codegen, this was the only way
to see the generated instructions.

**Why it is parked:** the bug was not codegen. The hang was
`sky.glsl`'s `fog_process()` looping on a `directional_light_count` read from a
misread scene UBO. A disassembly of correct code compiled from correct SPIR-V
would have shown nothing wrong, because nothing was wrong with it.

**What it still needs, if the codegen question ever reopens:**

1. `isa_dump.h` -- never written
2. `config.dump_isa` in `config.h`/`config.c`
3. `vkCreateDevice` interception: chain
   `VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR{ pipelineExecutableInfo = TRUE }`
   into `pNext` and add the extension name to the enabled list (deep-copy the
   create-info; on failure retry without it and warn)
4. `vkCreateComputePipelines` / `vkCreateGraphicsPipelines` hooks that OR in
   `VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR` and call
   `isa_dump_pipeline()` after a successful create
5. `isa_dump_resolve()` on first device-proc use

Left in the tree rather than deleted because (3) and (4) are the fiddly parts
and the verification behind them -- that the extension is present and every
enum resolves -- is already done.
