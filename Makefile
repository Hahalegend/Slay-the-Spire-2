#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET		:=	sts2
APP_TITLE	:=	Slay the Spire 2
APP_AUTHOR	:=	ChanseyIsTheBest
APP_VERSION	:=	1.0.0
BUILD		:=	build
SOURCES		:=	source source/thirdparty/astcenc
DATA		:=	data
INCLUDES	:=	source

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS	:=	-g -Wall -O2 -ffunction-sections -fdata-sections \
			$(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__

CXXFLAGS	:= $(CFLAGS)

ASFLAGS	:=	-g $(ARCH)
# libGLESv1_CM and libGLESv2 (mesa) both define the common GL entry points
# (glClear, glTexImage2D, ...) -- they are thin wrappers over the same libglapi
# dispatch, so let the linker take the first definition and still pull the
# GLES1-only (glOrthof, ...) and GLES2-only (glCreateShader, ...) symbols.
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
			-Wl,--allow-multiple-definition -Wl,-Map,$(notdir $*.map) \
			-Wl,--gc-sections \
			-Wl,--wrap,memcpy -Wl,--wrap,memmove -Wl,--wrap,malloc -Wl,--wrap,calloc -Wl,--wrap,memalign -Wl,--wrap,free -Wl,--wrap,realloc \
			-Wl,--wrap,memcpy -Wl,--wrap,memmove -Wl,--wrap,malloc_usable_size \
			-Wl,--wrap,vkCreateBuffer -Wl,--wrap,vkUpdateDescriptorSets \
			-Wl,--wrap,vkCmdBindPipeline -Wl,--wrap,vkCreateShaderModule
#            ^ gpu_arena.c. libdrm_nouveau and mesa are linked INTO the NRO, so
#              their memalign calls never pass through the import table -- that
#              only serves the .so modules we load ourselves. --wrap is the only
#              way to see them. Just these two: nouveau_bo_new uses memalign and
#              free exclusively, and every extra wrap is a chance to break
#              something unrelated to GPU buffers.

# StS2's libgodot_android.so ships BOTH backends -- 242 vk* symbols and a
# libvulkan.so dlopen -- unlike drmariomania_nx's engine, whose original note
# here said "no Vulkan imports found". Vulkan is NOT linked and NOT serviced:
# main() pins --rendering-method gl_compatibility --rendering-driver opengl3,
# and egl_shim.c's dlopen_fake refuses libvulkan.so outright so the failure
# path is Godot's own fallback rather than a null call. See docs/RENDERING.md.
#
# All 125 GL/EGL symbols the engine imports are covered by gl_imports.inc plus
# the EGL entries in imports.c (verified against the shipped binary). It imports
# its
# GL/EGL by name; we resolve those to mesa (GLESv3 + EGL + glapi +
# drm_nouveau) through the import table, same as smwr_nx/smbr_nx. The wrapper
# owns the EGL context. zlib backs the engine's inflate/deflate imports.
# OpenSLES audio reuses audio.c as-is; aaudio_shim.c adds the AAudio surface
# FMOD's Android build probes first (see config.h AUDIO_BACKEND_*).
#
# FMOD/Spine: their Android arm64 .so files are loaded at runtime through
# so_util like the engine, NOT linked here -- nothing to add to LIBS. Their
# imports resolve through the same table (fmod_shim.c).
# libpng: nx_pointer.c decodes an optional <data_root>/cursor.png for the
# on-screen cursor. -lpng must precede -lz (png depends on it).
#
# Vulkan (Mesa 26.2.0 for Horizon: NVK + wsi_switch over NWindow).
#
# STATIC archives, so they link into the .nro -- there is no libvulkan.so to
# dlopen, which is why vulkan_shim.c hands the engine a sentinel handle.
#
# Order is from the shipped pkgconfig/vulkan.pc, not guessed:
#   Libs: -lvulkan     Libs.private: -lnx -lm -lstdc++
#   Requires.private: libelf expat zlib libzstd
#
# The dependency chain below it is Mesa's own layering -- driver, then winsys,
# then the shader compiler, then utilities. Static linking resolves left to
# right, so a library must precede the ones it depends on.
#
# libnak_rs is Rust (NVK's shader assembler); it needs the C++ runtime and
# pthreads, which -lstdc++ and libnx supply.
#
# ~109 MB of archives against a 6 MB .nro. --gc-sections does the work; if the
# result is still unreasonable, that is a real finding and not a build error.
# Fail with an explanation rather than "vulkan/vulkan.h: No such file".
#
# The Mesa package extracts to opt/devkitpro/portlibs/switch/, and in devkitPro's
# MSYS /opt/devkitpro is a MOUNT pointing at C:\devkitPro -- so unzipping it from
# PowerShell to C:\ lands everything in C:\opt\devkitpro\, which nothing reads.
# From the MSYS shell:  cd / && unzip -o <the zip>
VULKAN_HEADER := $(PORTLIBS)/include/vulkan/vulkan.h
ifeq ($(wildcard $(VULKAN_HEADER)),)
  $(warning )
  $(warning Mesa Vulkan headers not found at $(VULKAN_HEADER))
  $(warning Install the Horizon Mesa package from the MSYS shell:)
  $(warning     cd / && unzip -o mesa-26_2_0-switch-unified-horizon-sdk.zip)
  $(warning /opt/devkitpro is a mount for C:\devkitPro -- extracting from)
  $(warning PowerShell to C:\ puts it in C:\opt\devkitpro\ where nothing looks.)
  $(warning )
  $(error Vulkan headers missing -- see above)
endif

# libvulkan.a is a UNIFIED archive: 1845 members, 86 MB, containing NVK, NIR,
# the NAK shader compiler, blake3, mesa_util and the wsi_switch WSI. Verified
# by symbol, not assumed -- nvk_CreateDevice, nir_shader_create,
# nak_compile_shader, _mesa_blake3_compute and vkCreateViSurfaceNN are all
# defined inside it.
#
# So the shipped pkgconfig is literally right:
#     Libs: -lvulkan      Libs.private: -lnx -lm -lstdc++
#
# I first wrote out the whole sub-archive list (-lnvk -lnak -lnir ...), which
# was both redundant and MIS-ORDERED: a link-order simulation showed libnir.a
# needing 69 symbols from libvulkan.a, and libxmlconfig.a needing 10, both of
# which sit earlier in the line. Static linking resolves left to right, so that
# would have failed after a long build. Taking the .pc at its word avoids the
# whole class of problem.
#
# expat and zstd are the only real external deps -- XML_ParserCreate and
# friends for drirc, ZSTD_compress for the shader cache. Confirmed by
# undefined-symbol scan. -lelf appears in Requires.private but NOTHING
# references it, so it is omitted rather than made a build requirement.
# If the link complains:  pacman -S switch-libexpat switch-zstd
# Split deliberately: -lvulkan must come FIRST (see LIBS below), but its
# external dependencies must come LAST, after everything that also needs them.
# libEGL.a alone pulls operator new/delete, __cxa_guard_*, __cxa_pure_virtual
# and 12 XML_/ZSTD_ symbols; static linking is single-pass, so a dependency
# placed before its consumer is simply not there when the consumer is scanned.
VULKAN_LIB  := -lvulkan
VULKAN_DEPS := -lexpat -lzstd -lstdc++

# ORDER MATTERS, and it was wrong.
#
# libEGL.a is not just EGL: 1081 members, including a complete copy of Mesa's
# GLSL compiler -- glsl_interface_type, linear_zalloc_child, ralloc_context,
# glsl_type_singleton_init_or_ref. libvulkan.a (unified, 1845 members) defines
# every one of them too.
#
# The linker pulls archive members on demand, so with libEGL first you get a
# MIX: some objects from libEGL.a, some from libvulkan.a, each carrying its own
# static glsl_type_cache. vk_instance_init initialises one copy;
# glsl_interface_type reads the other and finds a NULL linear context. That is
# the crash at
#   spirv_to_nir -> vtn_handle_type -> glsl_interface_type -> linear_zalloc_child
# with x0 == NULL, on the first shader containing a uniform block.
#
# MarathonRecomp-NX hit this and solved it by not linking libEGL at all,
# stubbing the egl* entry points instead: "the old-Mesa libEGL portlib, whose
# bundled ralloc clashes with the NVK driver".
#
# We still want GLES available as a runtime fallback, so instead libvulkan
# comes FIRST. Every shared Mesa symbol then resolves from the unified archive,
# consistently, and libEGL contributes only the EGL entry points that
# libvulkan does not define. Both are the same Mesa 26.2.0 build, so the
# shared core is the same code -- the problem was never the code, only which
# copy of its static state got used.
# --start-group, because the dependency is CIRCULAR.
#
# libvulkan.a and libEGL.a define 6173 symbols in common -- libEGL carries a
# complete second copy of Mesa's GLSL compiler. And libEGL.a *needs* 12 symbols
# that only libvulkan.a defines (nvk_loaderless_GetInstanceProcAddr,
# vk_AccessFlagBits_to_str, ...), so it was built expecting to sit beside it.
#
# A single-pass scan cannot satisfy that in either order. The group makes the
# linker iterate until nothing further resolves, which is exactly what mutually
# dependent archives need.
#
# libvulkan is listed FIRST inside the group so its copies of the shared
# symbols are the ones pulled: with --allow-multiple-definition, the first
# definition wins and later ones are ignored. That is the whole point --
# NVK and everything it calls must agree on ONE static glsl_type_cache.
# Getting a mix is what produced the NULL linear context in
# glsl_interface_type.
LIBS	:= -lwebp -Wl,--start-group \
			$(VULKAN_LIB) \
			-lGLESv2 -lEGL -lglapi \
			-Wl,--end-group \
			-ldrm_nouveau \
			$(VULKAN_DEPS) \
			-lpng -lz -lnx -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
export LD	:=	$(CXX)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else
		ifneq (,$(findstring icon.jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.jpg
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

ifneq ($(ROMFS),)
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all	:	$(OUTPUT).nro

$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
