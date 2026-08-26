/* egl_shim.h -- dlopen/dlsym -> mesa EGL/GLES bridge. MIT license; see LICENSE. */

#ifndef __EGL_SHIM_H__
#define __EGL_SHIM_H__

void *dlopen_fake(const char *filename, int flag);
void *dlsym_fake(void *handle, const char *symbol);
int dlclose_fake(void *handle);
char *dlerror_fake(void);

// Distinct sentinel handles so dlsym_fake can tell the callers apart. Values
// are arbitrary non-NULL pointers that are never dereferenced.
// dlopen(NULL): "the main program and everything loaded into it". Godot uses
// it for the handle it hands to GodotSharp's DllImport resolver.
#define MAIN_PROG_DL_HANDLE ((void *)0x4d41494e)  /* 'MAIN' */

// libvulkan.so. Mesa is linked statically, so this is a sentinel like the
// others rather than a module handle.
#define VULKAN_DL_HANDLE ((void *)0x56554C4B)  /* 'VULK' */

#define FMOD_DL_HANDLE   ((void *)0x464d4f44)  /* 'FMOD' */
#define SPINE_DL_HANDLE  ((void *)0x53504e45)  /* 'SPNE' */
#define AAUDIO_DL_HANDLE ((void *)0x41415544)  /* 'AAUD' */

// Names one of dlopen_fake's handles, or NULL if it is not one of ours.
// A description rather than a bool: the sentinels are not equivalent, and only
// FAKE_DL_HANDLE indicates a name dlopen_fake failed to recognise. See
// egl_shim.c.
const char *sts2_describe_dl_handle(const void *h);

#endif
