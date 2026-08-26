/* vulkan_shim.c -- present Mesa's Switch Vulkan as the Android one.
 *
 * The engine is libgodot_android.so, an Android build. Its Vulkan path wants
 * VK_KHR_android_surface and vkCreateAndroidSurfaceKHR(ANativeWindow *).
 *
 * Mesa 26.2.0's Horizon build exports VK_NN_vi_surface and
 * vkCreateViSurfaceNN(NWindow *) instead -- Nintendo's own Switch surface
 * extension, backed by a real WSI over libnx NWindow (wsi_switch_*, not
 * headless).
 *
 * The two create structs are the same shape:
 *
 *   VkAndroidSurfaceCreateInfoKHR { sType; pNext; flags; ANativeWindow *window; }
 *   VkViSurfaceCreateInfoNN       { sType; pNext; flags; void      *window; }
 *
 * so the whole gap is one function and one extension name. Everything below is
 * that translation, in both directions:
 *
 *   enumerate    VI -> Android    so the engine's extension check passes
 *   create       Android -> VI    so Mesa accepts what the engine asked for
 *   surface      Android -> VI    substituting nwindowGetDefault()
 *
 * Both directions are required. Reporting the Android name without translating
 * it back at vkCreateInstance would fail with
 * VK_ERROR_EXTENSION_NOT_PRESENT -- Mesa has never heard of it.
 *
 * Mesa is linked STATICALLY into the .nro, so "loading libvulkan.so" is a
 * dlopen_fake sentinel and dlsym resolves to symbols already in the binary.
 *
 * MIT license; see LICENSE. */

#include <string.h>
#include <stdlib.h>
#include <switch.h>

// Both platform blocks.
//
// VI_NN is what Mesa actually implements here; ANDROID_KHR is what the engine
// asks for, and its header is needed only for the struct layout we translate
// from -- nothing Android-specific is linked.
//
// Safe to enable off-Android: vulkan_android.h in this SDK includes NOTHING and
// forward-declares `struct ANativeWindow;` itself, so it does not drag in
// <android/native_window.h>. Checked rather than assumed, because a platform
// header that pulls in an NDK tree would fail confusingly here.
#define VK_USE_PLATFORM_VI_NN
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include "config.h"
#include "spirv_patch.h"
#include "vk_validate.h"
#include "util.h"
#include "vulkan_shim.h"
#include "call_once_shim.h"

// No extern declarations for Mesa's entry points.
//
// vulkan.h declares all of them itself (VK_NO_PROTOTYPES is not defined), with
// VKAPI_ATTR/VKAPI_CALL decoration. Hand-writing plain `extern` versions was
// redundant and risked a signature mismatch the compiler would only complain
// about on one of the two -- the linker would then be resolving something
// subtly different from what the header promised.
//
// VK_KHR_ANDROID_SURFACE_EXTENSION_NAME likewise comes from vulkan_android.h,
// so the local #ifndef fallback is gone too: one definition, from the SDK.

// VKAPI_ATTR ... VKAPI_CALL on every shim below, matching how vulkan_core.h
// declares the functions they stand in for. Both macros are empty on aarch64
// (vk_platform.h picks the default convention), so this changes nothing today
// -- but these function pointers are handed to the ENGINE, and a shim whose
// convention silently disagreed with the declaration would be the kind of bug
// that only appears on a different target.
//
// ---------------------------------------------------------------------------
// GLSL type cache
//
// Mesa's GLSL type system allocates from a global linear context that must be
// set up before any type is created. In this build it never is, on the Vulkan
// path.
//
// Evidence from the linked sts2.elf, not from reading Mesa's source:
//
//   * glsl_type_singleton_init_or_ref exists at 0x2a7640 and has exactly four
//     callers -- nouveau_screen_init, zink_internal_create_screen,
//     _mesa_glsl_builtin_functions_init_or_ref, and one_time_init. Every one
//     is a GL-stack path.
//   * vk_instance_init is NOT among them. Disassembling it shows 26 direct
//     calls -- mtx_init, dispatch tables, debug options -- and no GLSL init,
//     even though .rela.text.vk_instance_init in libvulkan.a's vk_instance.c.o
//     carries two R_AARCH64_CALL26 relocations to that symbol.
//
// RESOLVED BY MESA 26.2.1. The archive shipped THREE definitions of
// vk_instance_init, at sizes 0x490, 0x484 and 0x490 -- the odd 0x484 one being
// libvulkan_lite_instance.a.p/vk_instance.c.o, and 0x484 is exactly the size
// of the copy that got linked. 26.2.1 removes that member; the two that remain
// are both 0x490 and both call the GLSL init.
//
// The call below is KEPT anyway. It is refcounted (init_or_ref / decref), so an
// extra reference costs nothing but a cache that is never freed at exit, on a
// console that is exiting. Removing it and being wrong brings back a crash that
// took a session to find, and the check is not one the wrapper can make at
// runtime.
//
// What was unresolved at the time, and what is certain
// is the consequence: nothing on the Vulkan path initialises the cache, so
// glsl_interface_type reads a NULL linear context and dies on the first shader
// containing a uniform or storage block:
//
//   spirv_to_nir -> vtn_handle_type -> glsl_interface_type
//     -> linear_zalloc_child      ldp w1, w6, [x0, #4], x0 == NULL
//
// So we call it, exactly as the GL drivers in this same Mesa do. It is
// refcounted (init_or_ref / decref), so an extra reference is harmless -- it
// only ensures the count never reaches zero, which is what we want anyway
// since the engine keeps its device for the whole session.
// ---------------------------------------------------------------------------

extern void glsl_type_singleton_init_or_ref(void);

// ---------------------------------------------------------------------------
// surface
// ---------------------------------------------------------------------------

// vkCreateAndroidSurfaceKHR, implemented over VK_NN_vi_surface.
//
// The ANativeWindow the engine passes is the wrapper's fake one (jni_fake.c);
// it is not something Mesa can present to. The real target is libnx's default
// NWindow, which is what wsi_switch drives.
static VKAPI_ATTR VkResult VKAPI_CALL
shim_vkCreateAndroidSurfaceKHR(VkInstance instance,
                               const VkAndroidSurfaceCreateInfoKHR *info,
                               const VkAllocationCallbacks *alloc,
                               VkSurfaceKHR *out) {
  NWindow *win = nwindowGetDefault();
  if (!win) {
    debugPrintf("[vk] nwindowGetDefault() returned NULL -- no surface\n");
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  VkViSurfaceCreateInfoNN vi = {
    .sType  = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN,
    .pNext  = info ? info->pNext : NULL,
    .flags  = 0,
    .window = win,
  };

  const VkResult r = vkCreateViSurfaceNN(instance, &vi, alloc, out);
  debugPrintf("[vk] CreateAndroidSurface -> CreateViSurfaceNN(nwindow=%p) = %d\n",
              (void *)win, (int)r);
  return r;
}

// ---------------------------------------------------------------------------
// extension name translation
// ---------------------------------------------------------------------------

static VKAPI_ATTR VkResult VKAPI_CALL
shim_vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count,
                                            VkExtensionProperties *props) {
  const VkResult r = vkEnumerateInstanceExtensionProperties(layer, count, props);
  if (r != VK_SUCCESS && r != VK_INCOMPLETE) return r;
  if (!props || !count) return r;

  // Rewrite VI -> Android in place. The engine looks for the Android name and
  // will refuse to use Vulkan without it.
  for (uint32_t i = 0; i < *count; i++) {
    if (!strcmp(props[i].extensionName, VK_NN_VI_SURFACE_EXTENSION_NAME)) {
      strncpy(props[i].extensionName, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
              sizeof(props[i].extensionName) - 1);
      props[i].extensionName[sizeof(props[i].extensionName) - 1] = '\0';
      debugPrintf("[vk] reported VK_NN_vi_surface as VK_KHR_android_surface\n");
    }
  }
  return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL
shim_vkCreateInstance(const VkInstanceCreateInfo *info,
                      const VkAllocationCallbacks *alloc, VkInstance *out) {
  if (!info) return vkCreateInstance(info, alloc, out);

  // Translate back: Mesa has never heard of VK_KHR_android_surface, and
  // passing it through is VK_ERROR_EXTENSION_NOT_PRESENT.
  VkInstanceCreateInfo patched = *info;
  const char **names = NULL;
  int swapped = 0;

  if (info->enabledExtensionCount && info->ppEnabledExtensionNames) {
    names = calloc(info->enabledExtensionCount, sizeof(*names));
    if (!names) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
      const char *n = info->ppEnabledExtensionNames[i];
      if (n && !strcmp(n, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) {
        names[i] = VK_NN_VI_SURFACE_EXTENSION_NAME;
        swapped = 1;
      } else {
        names[i] = n;
      }
    }
    patched.ppEnabledExtensionNames = names;
  }

  if (swapped)
    debugPrintf("[vk] CreateInstance: VK_KHR_android_surface -> VK_NN_vi_surface\n");

  const VkResult r = vkCreateInstance(&patched, alloc, out);
  free(names);

  debugPrintf("[vk] vkCreateInstance -> %d (%u extensions)\n",
              (int)r, (unsigned)info->enabledExtensionCount);
  return r;
}

// ---------------------------------------------------------------------------
// format support logging
//
// STATE.md records that every texture was re-encoded to ASTC 8x8 to fit a
// 3.2 GB console -- ~7 GB of desktop-export RGBA8/BPTC/DXT down to ~444 MB.
// That was validated on GLES3. Vulkan is a different driver path, and whether
// NVK reports ASTC as sampleable on GM20B is a RUNTIME device query: the
// static archive contains the format tables, which proves the code exists, not
// that this chip advertises it.
//
// Tegra X1 has ASTC LDR in hardware, so the expected answer is yes. Rather
// than rely on that, this logs what NVK actually says the first time the
// engine asks. If ASTC comes back without SAMPLED_IMAGE, every re-encoded
// texture fails and the symptom would be missing or black art -- easy to
// mistake for a renderer bug.
// ---------------------------------------------------------------------------

static void log_format_support(VkFormat fmt, const VkFormatProperties *p) {
  if (!config.vk_verbose) {
    // Unconditionally interesting: the compressed formats this port depends on.
    if (!(fmt >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && fmt <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) &&
        !(fmt >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && fmt <= VK_FORMAT_BC7_SRGB_BLOCK))
      return;
  }
  const int sampled = (p->optimalTilingFeatures &
                       VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
  debugPrintf("[vk] format %d: optimal=0x%08x%s\n", (int)fmt,
              (unsigned)p->optimalTilingFeatures,
              sampled ? "  SAMPLED ok" : "  <- NOT SAMPLEABLE");
}

// Resolved in shim_vkGetInstanceProcAddr, where a REAL instance is in hand.
//
// The first version resolved it here with VK_NULL_HANDLE, which the spec says
// returns only the four GLOBAL commands -- vkEnumerateInstanceVersion,
// ...ExtensionProperties, ...LayerProperties and vkCreateInstance.
// vkGetPhysicalDeviceFormatProperties is INSTANCE-level, so the lookup gave
// NULL, and the `if (!real) return;` below returned WITHOUT WRITING *out.
//
// Godot then read uninitialised stack as format capabilities and concluded
// R8_UNORM was not sampleable -- a format every driver supports. From there:
// texture_create failed, the uniform set was null, and the engine faulted in
// compute_list_bind_uniform_set.
//
// A diagnostic that silently corrupts the thing it was added to observe. The
// out-parameter was the whole contract and it went unwritten, which is the
// "silent promise" shape this port keeps rediscovering -- this time in code I
// added to look for exactly that.
static PFN_vkGetPhysicalDeviceFormatProperties g_real_get_fmt_props;

static VKAPI_ATTR void VKAPI_CALL
shim_vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice dev, VkFormat fmt,
                                         VkFormatProperties *out) {
  // Never reachable with a null real: GetInstanceProcAddr below hands back the
  // REAL pointer rather than this wrapper when resolution fails, so a lookup
  // problem costs logging and nothing else. Belt and braces, because the cost
  // of being wrong here is a corrupted out-parameter.
  if (!g_real_get_fmt_props) {
    if (out) memset(out, 0, sizeof(*out));
    return;
  }
  g_real_get_fmt_props(dev, fmt, out);
  if (out) log_format_support(fmt, out);
}

// ---------------------------------------------------------------------------
// proc address
// ---------------------------------------------------------------------------

// Every substitution above has to be visible HERE as well, not only through
// ---------------------------------------------------------------------------
// Compute dispatch interception
//
// WHY: the GPU channel dies on a particle compute dispatch, and every layer
// above this one has been tried:
//
//   - gpu_particles 0 suppressed set_Emitting/Restart at the C# boundary --
//     the announce line fired, the suppression ran, the channel still died.
//     Godot dispatches particle compute (the clear/setup pass on first draw)
//     with no C# call involved, so no managed patch can gate it.
//   - NVK_DEBUG=zero_memory zeroed every GPU allocation -- still died. The
//     fault is NOT data-dependent: it is the compiled code, NAK's Maxwell
//     output for this shader.
//
// The dispatch itself is the last place the CPU touches the work before the
// GPU hangs on it, so this is where the lever goes. Two functions:
//
//   dispatch_log   first 32 dispatches and every 512th after, with group
//                  counts -- evidence of HOW MUCH compute a 2D card game
//                  actually issues, which decides whether skipping all of it
//                  is safe or reckless.
//   skip_compute   drop every vkCmdDispatch/DispatchIndirect on the floor.
//                  Blunt by design: there is no reliable way to identify "the
//                  particle pipeline" from here (release SPIR-V carries no
//                  names), and the hypothesis worth testing first is that a 2D
//                  canvas game issues no compute the player would miss.
//
// Both off/quiet by default; config.txt turns them on.
// ---------------------------------------------------------------------------
static PFN_vkGetDeviceProcAddr  g_real_gdpa;
static PFN_vkCmdDispatch        g_real_cmd_dispatch;
static PFN_vkCmdDispatchIndirect g_real_cmd_dispatch_indirect;
static uint32_t g_dispatch_n, g_dispatch_skipped;

static VKAPI_ATTR void VKAPI_CALL
shim_vkCmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
  const uint32_t n = __atomic_add_fetch(&g_dispatch_n, 1, __ATOMIC_RELAXED);
  if (config.dispatch_log && (n <= 32 || (n & 511u) == 0))
    debugPrintf("[vk] compute dispatch #%u: %ux%ux%u groups%s\n", n, x, y, z,
                config.skip_compute ? "  SKIPPED" : "");
  if (config.skip_compute) { g_dispatch_skipped++; return; }
  g_real_cmd_dispatch(cb, x, y, z);
}

static VKAPI_ATTR void VKAPI_CALL
shim_vkCmdDispatchIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off) {
  const uint32_t n = __atomic_add_fetch(&g_dispatch_n, 1, __ATOMIC_RELAXED);
  if (config.dispatch_log && (n <= 32 || (n & 511u) == 0))
    debugPrintf("[vk] compute dispatch #%u: INDIRECT%s\n", n,
                config.skip_compute ? "  SKIPPED" : "");
  if (config.skip_compute) { g_dispatch_skipped++; return; }
  g_real_cmd_dispatch_indirect(cb, buf, off);
}

// NOTE: the descriptor/pipeline/shader-module hooks are NOT here. They were,
// and on hardware they reported zero of everything and never announced
// themselves -- Godot never calls vkGetDeviceProcAddr, because Mesa is linked
// statically into the NRO and the calls bind directly at link time. They now
// live in vk_validate.c behind --wrap, which intercepts at that same step.
//
// The dispatch hooks below stay, because they were verified to resolve.
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
shim_vkGetDeviceProcAddr(VkDevice device, const char *name) {
  if (!name) return NULL;

  // Interception only when the real pointer resolves, same rule as the
  // format-props hook: the fallback is always the truth, never a NULL that
  // used to be a function.
  if (!strcmp(name, "vkCmdDispatch")) {
    if (!g_real_cmd_dispatch)
      g_real_cmd_dispatch = (PFN_vkCmdDispatch)vkGetDeviceProcAddr(device, name);
    if (g_real_cmd_dispatch) {
      // Once, loudly. A run with skip_compute=1 and zero dispatch lines is
      // AMBIGUOUS without this: "no compute was issued" and "the hook never
      // engaged" read identically in the log, and they send the
      // investigation in opposite directions. One line here makes every
      // future skip_compute run interpretable.
      static int announced;
      if (!announced) {
        announced = 1;
        debugPrintf("[vk] vkCmdDispatch HOOKED (skip_compute=%d)\n",
                    config.skip_compute);
      }
      return (PFN_vkVoidFunction)shim_vkCmdDispatch;
    }
    debugPrintf("[vk] vkCmdDispatch did NOT resolve -- dispatch hook INACTIVE\n");
  }
  if (!strcmp(name, "vkCmdDispatchIndirect")) {
    if (!g_real_cmd_dispatch_indirect)
      g_real_cmd_dispatch_indirect =
          (PFN_vkCmdDispatchIndirect)vkGetDeviceProcAddr(device, name);
    if (g_real_cmd_dispatch_indirect)
      return (PFN_vkVoidFunction)shim_vkCmdDispatchIndirect;
  }
  if (!strcmp(name, "vkGetDeviceProcAddr"))
    return (PFN_vkVoidFunction)shim_vkGetDeviceProcAddr;

  // Same four, for callers that ask by name rather than by dlsym. Legal:
  // vkGetInstanceProcAddr may return device-level commands.
  {
    void *ov = vulkan_shim_device_override(name);
    if (ov) return (PFN_vkVoidFunction)ov;
  }

  return vkGetDeviceProcAddr(device, name);
}

// dlsym: Godot resolves most Vulkan entry points with vkGetInstanceProcAddr,
// so a shim that only answered dlsym would be bypassed for exactly the
// functions that matter.
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
shim_vkGetInstanceProcAddr(VkInstance instance, const char *name) {
  if (!name) return NULL;

  // SHOW HOW THE ENGINE RESOLVES THINGS.
  //
  // Three separate attempts to hook vkCreateBuffer failed -- first behind
  // vkGetDeviceProcAddr, then behind --wrap, then behind an override that a
  // count=1 string replace dropped into the WRONG function -- and each cost a
  // hardware run, because "buffers tracked 0" looks the same whether the hook
  // is misplaced or the engine simply never asks.
  //
  // Every one of those rounds would have been answered by printing the names
  // as they are requested. Guessing the resolution path three times cost more
  // than logging it once.
  if (config.vk_verbose) {
    static uint32_t seen;
    if (seen < 64)
      debugPrintf("[vk] GIPA #%u: %s\n", seen, name);
    seen++;
  }

  // Ours, if any. Before every specific case below, so there is one answer per
  // name no matter which path asks.
  {
    void *ov = vulkan_shim_device_override(name);
    if (ov) return (PFN_vkVoidFunction)ov;
  }

  if (!strcmp(name, "vkCreateAndroidSurfaceKHR"))
    return (PFN_vkVoidFunction)shim_vkCreateAndroidSurfaceKHR;
  if (!strcmp(name, "vkEnumerateInstanceExtensionProperties"))
    return (PFN_vkVoidFunction)shim_vkEnumerateInstanceExtensionProperties;
  if (!strcmp(name, "vkCreateInstance"))
    return (PFN_vkVoidFunction)shim_vkCreateInstance;
  if (!strcmp(name, "vkGetInstanceProcAddr"))
    return (PFN_vkVoidFunction)shim_vkGetInstanceProcAddr;
  if (!strcmp(name, "vkGetDeviceProcAddr"))
    return (PFN_vkVoidFunction)shim_vkGetDeviceProcAddr;
  if (!strcmp(name, "vkGetPhysicalDeviceFormatProperties")) {
    // Resolve through the instance we were GIVEN. Interception only happens
    // if that succeeds; otherwise hand back the real pointer untouched, so
    // logging is the only thing that can be lost.
    if (!g_real_get_fmt_props)
      g_real_get_fmt_props = (PFN_vkGetPhysicalDeviceFormatProperties)
                               vkGetInstanceProcAddr(instance, name);
    if (g_real_get_fmt_props)
      return (PFN_vkVoidFunction)shim_vkGetPhysicalDeviceFormatProperties;
    debugPrintf("[vk] could not resolve %s; not intercepting\n", name);
    return vkGetInstanceProcAddr(instance, name);
  }

  PFN_vkVoidFunction p = vkGetInstanceProcAddr(instance, name);
  if (!p && config.vk_verbose)
    debugPrintf("[vk] GetInstanceProcAddr(\"%s\") -> NULL\n", name);
  return p;
}

// ---------------------------------------------------------------------------
// dlsym surface
// ---------------------------------------------------------------------------

typedef struct { const char *name; void *fn; } VkEntry;

// Only the entry points the loader is expected to fetch by name. Everything
// else goes through vkGetInstanceProcAddr, which is how Vulkan is meant to
// work and how Godot actually does it.
static const VkEntry k_entries[] = {
  { "vkGetInstanceProcAddr",                   (void *)&shim_vkGetInstanceProcAddr },
  { "vkCreateInstance",                        (void *)&shim_vkCreateInstance },
  { "vkEnumerateInstanceExtensionProperties",  (void *)&shim_vkEnumerateInstanceExtensionProperties },
  { "vkCreateAndroidSurfaceKHR",               (void *)&shim_vkCreateAndroidSurfaceKHR },

  // The four vk_validate watches. They belong HERE, in the table Godot's
  // dlsym actually reads, not behind vkGetDeviceProcAddr -- which Godot never
  // calls, and not behind --wrap alone, which only redirects call sites inside
  // the NRO's own link. Godot is a dlopen'd .so; its imports come through
  // vulkan_shim_find and vkGetInstanceProcAddr.
  //
  // These are the __wrap_ functions, so --wrap still provides the __real_
  // side: one wrapper per function, resolved by whichever path asks, and no
  // possibility of a wrapper calling itself.
  // (the four vk_validate entries are served by vulkan_shim_device_override
  //  below, which every resolution path consults first -- listing them twice
  //  would let the table and the helper drift apart)
};

// One place that decides whether a name is one of ours, used by every
// resolution path so they cannot disagree.
void *vulkan_shim_device_override(const char *name) {
  if (!config.vk_validate && !config.patch_loop_bounds) return NULL;
  if (config.vk_validate) {
    if (!strcmp(name, "vkCreateBuffer"))         return (void *)&__wrap_vkCreateBuffer;
    if (!strcmp(name, "vkUpdateDescriptorSets")) return (void *)&__wrap_vkUpdateDescriptorSets;
    if (!strcmp(name, "vkCmdBindPipeline"))      return (void *)&__wrap_vkCmdBindPipeline;
  }
  if (config.patch_loop_bounds && !strcmp(name, "vkCreateShaderModule"))
    return (void *)&__wrap_vkCreateShaderModule;
  return NULL;
}

void *vulkan_shim_find(const char *symbol) {
  if (!symbol) return NULL;

  {
    void *ov = vulkan_shim_device_override(symbol);
    if (ov) return ov;
  }

  for (size_t i = 0; i < sizeof(k_entries) / sizeof(k_entries[0]); i++)
    if (!strcmp(symbol, k_entries[i].name)) return k_entries[i].fn;

  // Anything else that is genuinely exported by the static Mesa: hand it over
  // through the real loader, with a null instance, which is valid for the
  // global commands.
  if (!strncmp(symbol, "vk", 2)) {
    void *p = (void *)vkGetInstanceProcAddr(VK_NULL_HANDLE, symbol);
    if (p) return p;
  }

  if (config.vk_verbose)
    debugPrintf("[vk] dlsym(\"%s\") -> NULL\n", symbol);
  return NULL;
}

// ---------------------------------------------------------------------------

int vulkan_shim_probe(void) {
  uint32_t n = 0;
  VkResult r = vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
  if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
    debugPrintf("[vk] probe: EnumerateInstanceExtensionProperties -> %d\n", (int)r);
    return 0;
  }
  debugPrintf("[vk] probe: %u instance extensions\n", (unsigned)n);

  VkExtensionProperties *props = calloc(n ? n : 1, sizeof(*props));
  if (!props) return 0;
  int have_vi = 0, have_surface = 0;
  if (vkEnumerateInstanceExtensionProperties(NULL, &n, props) == VK_SUCCESS) {
    for (uint32_t i = 0; i < n; i++) {
      if (!strcmp(props[i].extensionName, VK_NN_VI_SURFACE_EXTENSION_NAME)) have_vi = 1;
      if (!strcmp(props[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME))   have_surface = 1;
      if (config.vk_verbose) debugPrintf("[vk]   %s\n", props[i].extensionName);
    }
  }
  free(props);

  debugPrintf("[vk] probe: VK_KHR_surface=%d VK_NN_vi_surface=%d\n",
              have_surface, have_vi);

  if (!have_surface || !have_vi) return 0;

  // ACTUALLY CREATE AN INSTANCE, then throw it away.
  //
  // Checking that the extensions are advertised is not the same as checking
  // that a driver comes up. On hardware they diverged: the extension list was
  // perfect and vkCreateInstance still returned -3, because a C11 mtx_init was
  // handing Mesa a non-zero success code.
  //
  // The cost of that gap was not a clean fallback. main() had already skipped
  // egl_setup() on the strength of this probe, so when Godot fell back to
  // GLES3 there was no GL context: the texture atlas failed with status 0 and
  // the engine memcpy'd into the null result. A boot crash whose stack had
  // nothing to do with the cause.
  //
  // One real instance answers the question the extension list only implies.
  // It costs a few milliseconds once, and it means a Vulkan that cannot start
  // produces a working GLES3 boot instead of a fault.
  {
    const char *want[2];
    uint32_t nwant = 0;
    want[nwant++] = VK_KHR_SURFACE_EXTENSION_NAME;
    want[nwant++] = VK_NN_VI_SURFACE_EXTENSION_NAME;

    VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "sts2-probe",
      .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
      .enabledExtensionCount = nwant,
      .ppEnabledExtensionNames = want,
    };

    VkInstance probe = VK_NULL_HANDLE;
    const VkResult pr = vkCreateInstance(&ci, NULL, &probe);
    if (pr != VK_SUCCESS) {
      debugPrintf("[vk] probe: vkCreateInstance -> %d; Vulkan is NOT usable.\n"
                  "[vk] Falling back to GLES3, which needs the EGL context that\n"
                  "[vk] main() would otherwise have skipped.\n", (int)pr);
      return 0;
    }
    vkDestroyInstance(probe, NULL);
    debugPrintf("[vk] probe: vkCreateInstance succeeded and was destroyed\n");
  }

  // Initialise the GLSL type cache before the engine can compile anything.
  // See the comment above -- nothing on the Vulkan path does this.
  glsl_type_singleton_init_or_ref();
  debugPrintf("[vk] glsl_type_singleton_init_or_ref() called\n");

  // How many one-time initialisers Mesa has run by now.
  //
  // ZERO here means call_once_shim.c is not overriding the library's
  // call_once, and Mesa's lazily-initialised state -- the GLSL type cache
  // above all -- is still empty. That surfaced as a NULL linear allocator
  // context inside glsl_interface_type, on the first shader containing a
  // uniform or storage block, a long way from the cause.
  call_once_report();

  // Both are required. Without VI there is nothing to present to, and the
  // Android translation above has no target.
  return have_surface && have_vi;
}
