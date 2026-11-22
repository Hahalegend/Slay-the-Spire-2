/* so_util.c -- utils to load and hook .so modules
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * Loads Android ARM64 shared objects into a reserved memory range and applies
 * the relocation types used by libsotn.so.
 */

#include <switch.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <elf.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "error.h"
#include "app_paths.h"

static Handle s_own_proc_handle = INVALID_HANDLE;
static int s_own_proc_probed = 0;
static int s_codemem_probed = 0;
static int s_codemem_ok = 0;

static void sts2_proc_handle_recv_thread(void *arg) {
  Handle session = (Handle)(uintptr_t)arg;
  void *base = armGetTls();
  hipcMakeRequestInline(base);

  s32 idx = 0;
  Result rc = svcReplyAndReceive(&idx, &session, 1, INVALID_HANDLE, UINT64_MAX);
  if (R_SUCCEEDED(rc)) {
    HipcParsedRequest r = hipcParseRequest(base);
    if (r.meta.num_copy_handles == 1)
      s_own_proc_handle = r.data.copy_handles[0];
  }
  svcCloseHandle(session);
}

Handle sts2_own_process_handle(void) {
  Handle h = envGetOwnProcessHandle();
  if (h != INVALID_HANDLE)
    return h;
  if (s_own_proc_probed)
    return s_own_proc_handle;
  s_own_proc_probed = 1;

  // Standalone NSO path (same IPC technique as nx-hbloader): copy
  // CUR_PROCESS_HANDLE across an in-process session to obtain a real handle
  // accepted by svcMapProcessCodeMemory / svcSetProcessMemoryPermission.
  Handle server_handle = INVALID_HANDLE, client_handle = INVALID_HANDLE;
  if (R_FAILED(svcCreateSession(&server_handle, &client_handle, 0, 0)) ||
      server_handle == INVALID_HANDLE || client_handle == INVALID_HANDLE) {
    if (server_handle != INVALID_HANDLE) svcCloseHandle(server_handle);
    if (client_handle != INVALID_HANDLE) svcCloseHandle(client_handle);
    return INVALID_HANDLE;
  }

  Thread t;
  if (R_FAILED(threadCreate(&t, &sts2_proc_handle_recv_thread,
                            (void *)(uintptr_t)server_handle, NULL, 0x1000, 0x20, 0))) {
    svcCloseHandle(server_handle);
    svcCloseHandle(client_handle);
    return INVALID_HANDLE;
  }
  if (R_SUCCEEDED(threadStart(&t))) {
    hipcMakeRequestInline(armGetTls(),
      .num_copy_handles = 1,
    ).copy_handles[0] = CUR_PROCESS_HANDLE;
    svcSendSyncRequest(client_handle);
    svcCloseHandle(client_handle);
    threadWaitForExit(&t);
  } else {
    svcCloseHandle(server_handle);
    svcCloseHandle(client_handle);
  }
  threadClose(&t);
  return s_own_proc_handle;
}

int sts2_codemem_available(void) {
  if (s_codemem_probed)
    return s_codemem_ok;
  s_codemem_probed = 1;

  void *probe = memalign(0x1000, 0x1000);
  if (!probe)
    return 0;
  Handle cm = INVALID_HANDLE;
  Result rc = svcCreateCodeMemory(&cm, probe, 0x1000);
  if (R_SUCCEEDED(rc) && cm != INVALID_HANDLE) {
    s_codemem_ok = 1;
    svcCloseHandle(cm);
  }
  MemoryInfo mi;
  u32 pi;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)probe)) &&
      (mi.perm & Perm_Rw) == Perm_Rw) {
    free(probe);
  }
  return s_codemem_ok;
}

// not in devkitA64's elf.h
#ifndef DT_RELR
#define DT_RELR    36
#define DT_RELRSZ  35
#define DT_RELRENT 37
#endif
#define DT_ANDROID_RELR    0x6fffe000
#define DT_ANDROID_RELRSZ  0x6fffe001
#define DT_ANDROID_RELRENT 0x6fffe003

static so_module *so_list = NULL;

void hook_arm64(uintptr_t addr, uintptr_t dst) {
  if (addr == 0)
    return;
  uint32_t *hook = (uint32_t *)addr;
  hook[0] = 0x58000051u; // LDR X17, #0x8
  hook[1] = 0xd61f0220u; // BR X17
  *(uint64_t *)(hook + 2) = dst;
}

void so_flush_caches(so_module *mod) {
  armDCacheFlush(mod->load_virtbase, mod->load_size);
  armICacheInvalidate(mod->load_virtbase, mod->load_size);
}

void so_free_temp(so_module *mod) {
  free(mod->so_base);
  mod->so_base = NULL;
}

void so_finalize(so_module *mod) {
  Result rc = 0;
  Handle proc = sts2_own_process_handle();

  // Build per-page RX vs RW map first (shared by both backends).
  const size_t num_pages = mod->load_size / 0x1000;
  uint8_t *is_x_page = calloc(num_pages, 1);
  if (!is_x_page) fatal_error("Error: out of memory in so_finalize");

  for (int i = 0; i < mod->phnum; i++) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type != PT_LOAD || (p->p_flags & PF_X) != PF_X)
      continue;
    const size_t first = p->p_vaddr / 0x1000;
    const size_t last = (ALIGN_MEM(p->p_vaddr + p->p_memsz, 0x1000) / 0x1000) - 1;
    for (size_t pg = first; pg <= last && pg < num_pages; pg++)
      is_x_page[pg] = 1;
  }

  if (proc != INVALID_HANDLE) {
    // Primary real-Switch path: map entire module as ProcessCodeMemory and set
    // RX segments first (W -> X is forbidden on code memory), then RW.
    rc = svcMapProcessCodeMemory(proc, (u64)mod->load_virtbase, (u64)mod->load_base, mod->load_size);
    if (R_FAILED(rc)) fatal_error("Error: svcMapProcessCodeMemory failed:\n%08x", rc);

    for (int want_x = 1; want_x >= 0; want_x--) {
      size_t pg = 0;
      while (pg < num_pages) {
        if (is_x_page[pg] != want_x) {
          pg++;
          continue;
        }
        size_t run_end = pg;
        while (run_end < num_pages && is_x_page[run_end] == want_x)
          run_end++;
        const u64 addr = (u64)mod->load_virtbase + pg * 0x1000;
        const u64 size = (run_end - pg) * 0x1000;
        rc = svcSetProcessMemoryPermission(proc, addr, size, want_x ? Perm_Rx : Perm_Rw);
        if (R_FAILED(rc)) fatal_error("Error: could not map %u bytes of %s memory at %p:\n%08x", (u32)size, want_x ? "RX" : "RW", (void *)addr, rc);
        pg = run_end;
      }
    }
  } else {
    // Fallback path for emulators (yuzu/ruzu/Ryujinx) where svcCreateSession is
    // stubbed: map each contiguous RX/RW page run into the exact same reserved
    // AliasCode virtual range via svcCreateCodeMemory + svcControlCodeMemory
    // (0x4B/0x4C). Both CodeOut (MapOwner/RW) and GeneratedCode (MapSlave/RX)
    // share AliasCode space, preserving all ELF ADRP/PC-relative offsets.
    mod->cm_count = 0;
    size_t pg = 0;
    while (pg < num_pages) {
      const uint8_t want_x = is_x_page[pg];
      size_t run_end = pg;
      while (run_end < num_pages && is_x_page[run_end] == want_x)
        run_end++;

      const size_t run_off  = pg * 0x1000;
      const size_t run_size = (run_end - pg) * 0x1000;
      void *src = (void *)((uintptr_t)mod->load_base + run_off);
      void *dst = (void *)((uintptr_t)mod->load_virtbase + run_off);

      // svcCreateCodeMemory zeroes/0xFF-fills backing pages on creation, so
      // snapshot the already-relocated bytes before locking the source pages.
      void *backup = malloc(run_size);
      if (!backup) fatal_error("Error: OOM backing up %u KB in so_finalize", (unsigned)(run_size >> 10));
      memcpy(backup, src, run_size);

      Handle cm = INVALID_HANDLE;
      rc = svcCreateCodeMemory(&cm, src, run_size);
      if (R_FAILED(rc)) fatal_error("Error: svcCreateCodeMemory(%u KB) failed:\n%08x", (unsigned)(run_size >> 10), rc);

      if (!want_x) {
        rc = svcControlCodeMemory(cm, CodeMapOperation_MapOwner, dst, run_size, Perm_Rw);
        if (R_FAILED(rc)) fatal_error("Error: svcControlCodeMemory(MapOwner RW, %p, %u KB) failed:\n%08x", dst, (unsigned)(run_size >> 10), rc);
        memcpy(dst, backup, run_size);
        free(backup);
      } else {
        virtmemLock();
        void *tmp_rw = virtmemFindCodeMemory(run_size, 0x1000);
        VirtmemReservation *tmp_rv = virtmemAddReservation(tmp_rw, run_size);
        virtmemUnlock();

        rc = svcControlCodeMemory(cm, CodeMapOperation_MapOwner, tmp_rw, run_size, Perm_Rw);
        if (R_FAILED(rc)) fatal_error("Error: svcControlCodeMemory(MapOwner tmp, %p, %u KB) failed:\n%08x", tmp_rw, (unsigned)(run_size >> 10), rc);

        memcpy(tmp_rw, backup, run_size);
        free(backup);
        armDCacheFlush(tmp_rw, run_size);

        rc = svcControlCodeMemory(cm, CodeMapOperation_UnmapOwner, tmp_rw, run_size, Perm_None);
        if (R_FAILED(rc)) fatal_error("Error: svcControlCodeMemory(UnmapOwner tmp) failed:\n%08x", rc);

        virtmemLock();
        virtmemRemoveReservation(tmp_rv);
        virtmemUnlock();

        rc = svcControlCodeMemory(cm, CodeMapOperation_MapSlave, dst, run_size, Perm_Rx);
        if (R_FAILED(rc)) fatal_error("Error: svcControlCodeMemory(MapSlave RX, %p, %u KB) failed:\n%08x", dst, (unsigned)(run_size >> 10), rc);
      }

      if (mod->cm_count < SO_MAX_CM_RUNS) {
        int idx = mod->cm_count++;
        mod->cm_handles[idx] = cm;
        mod->cm_addrs[idx] = dst;
        mod->cm_sizes[idx] = run_size;
        mod->cm_is_x[idx] = want_x;
      }
      pg = run_end;
    }
  }

  // Re-point the symbol tables from load_base to load_virtbase (load_base is
  // now inaccessible under both ProcessCodeMemory and CodeMemory).
  if (mod->syms)
    mod->syms = (Elf64_Sym *)((uintptr_t)mod->load_virtbase +
                              ((uintptr_t)mod->syms - (uintptr_t)mod->load_base));
  if (mod->dynstrtab)
    mod->dynstrtab = (char *)((uintptr_t)mod->load_virtbase +
                              ((uintptr_t)mod->dynstrtab - (uintptr_t)mod->load_base));
  mod->finalized = 1;

  free(is_x_page);
}

int so_load(so_module *mod, const char *filename, void *base, size_t max_size) {
  int res = 0;

  memset(mod, 0, sizeof(*mod));
  strncpy(mod->name, filename, sizeof(mod->name) - 1);

  char real_path[512];
  FILE *fd = fopen(app_paths_real_path(filename, real_path, sizeof(real_path)), "rb");
  if (fd == NULL)
    return -1;

  fseek(fd, 0, SEEK_END);
  mod->so_size = ftell(fd);
  fseek(fd, 0, SEEK_SET);

  mod->so_base = malloc(mod->so_size);
  if (!mod->so_base) {
    fclose(fd);
    return -2;
  }

  fread(mod->so_base, mod->so_size, 1, fd);
  fclose(fd);

  if (memcmp(mod->so_base, ELFMAG, SELFMAG) != 0) {
    res = -1;
    goto err_free_so;
  }

  mod->elf_hdr = (Elf64_Ehdr *)mod->so_base;
  mod->prog_hdr = (Elf64_Phdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_phoff);
  mod->sec_hdr = (Elf64_Shdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_shoff);
  mod->shstrtab = (char *)((uintptr_t)mod->so_base + mod->sec_hdr[mod->elf_hdr->e_shstrndx].sh_offset);

  if (mod->elf_hdr->e_phnum > SO_MAX_SEGMENTS * 2) {
    debugPrintf("so_load: %s has too many program headers (%d)\n", filename, mod->elf_hdr->e_phnum);
    res = -4;
    goto err_free_so;
  }

  // save a pristine copy of the phdrs for dl_iterate_phdr
  mod->phnum = mod->elf_hdr->e_phnum;
  memcpy(mod->phdr, mod->prog_hdr, mod->phnum * sizeof(Elf64_Phdr));

  // total size of the LOAD zone = highest vaddr+memsz of all LOAD segments
  mod->load_size = 0;
  for (int i = 0; i < mod->elf_hdr->e_phnum; i++) {
    if (mod->prog_hdr[i].p_type == PT_LOAD) {
      const size_t seg_end = mod->prog_hdr[i].p_vaddr + mod->prog_hdr[i].p_memsz;
      if (seg_end > mod->load_size)
        mod->load_size = seg_end;
    }
  }

  // align total size to page size
  mod->load_size = ALIGN_MEM(mod->load_size, 0x1000);
  if (mod->load_size > max_size) {
    res = -3;
    goto err_free_so;
  }

  mod->load_base = base;
  if (!mod->load_base) goto err_free_so;
  memset(mod->load_base, 0, mod->load_size);

  // reserve virtual memory space for the entire LOAD zone
  virtmemLock();
  mod->load_virtbase = virtmemFindCodeMemory(mod->load_size, 0x1000);
  mod->load_memrv = virtmemAddReservation(mod->load_virtbase, mod->load_size);
  virtmemUnlock();

  // copy segments to where they belong, then fix up the runtime phdrs to
  // point into the virtual address space
  for (int i = 0; i < mod->elf_hdr->e_phnum; i++) {
    Elf64_Phdr *p = &mod->prog_hdr[i];
    if (p->p_type == PT_LOAD) {
      memcpy((void *)((uintptr_t)mod->load_base + p->p_vaddr),
             (void *)((uintptr_t)mod->so_base + p->p_offset),
             p->p_filesz);
    }
    p->p_vaddr += (Elf64_Addr)mod->load_virtbase;
  }

  mod->syms = NULL;
  mod->dynstrtab = NULL;

  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".dynsym") == 0) {
      mod->syms = (Elf64_Sym *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      mod->num_syms = mod->sec_hdr[i].sh_size / sizeof(Elf64_Sym);
    } else if (strcmp(sh_name, ".dynstr") == 0) {
      mod->dynstrtab = (char *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
    }
  }

  if (mod->syms == NULL || mod->dynstrtab == NULL) {
    res = -2;
    goto err_free_load;
  }

  // register in the global module list (append, load order matters for resolution)
  mod->next = NULL;
  if (!so_list) {
    so_list = mod;
  } else {
    so_module *m = so_list;
    while (m->next) m = m->next;
    m->next = mod;
  }

  return 0;

err_free_load:
  virtmemLock();
  virtmemRemoveReservation(mod->load_memrv);
  virtmemUnlock();
err_free_so:
  free(mod->so_base);
  mod->so_base = NULL;

  return res;
}

// find a dynamic tag in the file image; returns 0 if missing
static Elf64_Xword so_dynamic_tag(so_module *mod, Elf64_Sxword tag) {
  for (int i = 0; i < mod->phnum; i++) {
    if (mod->phdr[i].p_type == PT_DYNAMIC) {
      const Elf64_Dyn *dyn = (const Elf64_Dyn *)((uintptr_t)mod->so_base + mod->phdr[i].p_offset);
      for (; dyn->d_tag != DT_NULL; dyn++)
        if (dyn->d_tag == tag)
          return dyn->d_un.d_val;
    }
  }
  return 0;
}

// RELR/ANDROID_RELR packed relative relocations;
// each location gets the load base added to the value stored in the file
static void so_process_relr(so_module *mod, const Elf64_Xword *relr, size_t relrsz) {
  uintptr_t where = 0;
  const size_t count = relrsz / sizeof(Elf64_Xword);
  for (size_t i = 0; i < count; i++) {
    const Elf64_Xword entry = relr[i];
    if ((entry & 1) == 0) {
      // even entry: address of the next relocation
      where = (uintptr_t)entry;
      *(uint64_t *)((uintptr_t)mod->load_base + where) += (uint64_t)mod->load_virtbase;
      where += 8;
    } else {
      // odd entry: bitmap for the next 63 words
      for (int bit = 1; bit < 64; bit++) {
        if (entry & (1ull << bit))
          *(uint64_t *)((uintptr_t)mod->load_base + where + (bit - 1) * 8) += (uint64_t)mod->load_virtbase;
      }
      where += 63 * 8;
    }
  }
}

int so_relocate(so_module *mod) {
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.dyn") == 0 || strcmp(sh_name, ".rela.plt") == 0) {
      Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      for (int j = 0; j < mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela); j++) {
        uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
        Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];

        int type = ELF64_R_TYPE(rels[j].r_info);
        switch (type) {
          case R_AARCH64_ABS64:
            if (sym->st_shndx == SHN_UNDEF) {
              // Imported absolute relocations (notably RTTI/type_info vtable
              // references) must be resolved after all modules are loaded.
              *ptr = rels[j].r_addend;
            } else {
              *ptr = (uintptr_t)mod->load_virtbase + sym->st_value + rels[j].r_addend;
            }
            break;

          case R_AARCH64_RELATIVE:
            // sometimes the value of r_addend is also at *ptr
            *ptr = (uintptr_t)mod->load_virtbase + rels[j].r_addend;
            break;

          case R_AARCH64_GLOB_DAT:
          case R_AARCH64_JUMP_SLOT:
          {
            if (sym->st_shndx != SHN_UNDEF)
              *ptr = (uintptr_t)mod->load_virtbase + sym->st_value + rels[j].r_addend;
            break;
          }

          default:
            fatal_error("Error: unknown relocation type:\n%x\n", type);
            break;
        }
      }
    }
  }

  // packed relative relocations, if present (lld emits these for newer libs)
  Elf64_Xword relr_off = so_dynamic_tag(mod, DT_RELR);
  Elf64_Xword relr_size = so_dynamic_tag(mod, DT_RELRSZ);
  if (!relr_off) {
    relr_off = so_dynamic_tag(mod, DT_ANDROID_RELR);
    relr_size = so_dynamic_tag(mod, DT_ANDROID_RELRSZ);
  }
  if (relr_off && relr_size) {
    so_process_relr(mod, (const Elf64_Xword *)((uintptr_t)mod->load_base + relr_off), relr_size);
  }


  return 0;
}

// Which module owns this address, if any. Walks the same so_list every other
// lookup uses, so it automatically covers every module the port loads --
// including ones added later, which a hand-maintained registry would miss.
// Used by the crash handler to turn a raw PC into <module>+0x<offset>.
// Head of the loaded-module list, for callers that want to enumerate rather
// than look up (the crash handler prints a module map; bases move every boot,
// so an offset is only meaningful next to the base it came from).
so_module *so_get_list(void) { return so_list; }

so_module *so_find_module_by_addr(const void *addr) {
  const uintptr_t a = (uintptr_t)addr;
  for (so_module *m = so_list; m; m = m->next) {
    const uintptr_t base = (uintptr_t)m->load_virtbase;
    if (base && a >= base && a < base + m->load_size)
      return m;
  }
  return NULL;
}

// look up an exported (defined) symbol in a single module;
// returns its virtual (runtime) address or 0
static uintptr_t so_lookup_export(so_module *mod, const char *name) {
  // Both must be live. Before so_finalize they point into load_base; after it
  // they are rebased onto load_virtbase, because MapProcessCodeMemory makes
  // load_base inaccessible. If either is ever null here, something skipped
  // that rebase and the next line would be a Data Abort with no explanation.
  if (!mod->syms || !mod->dynstrtab || mod->num_syms <= 0)
    return 0;

  for (int i = 0; i < mod->num_syms; i++) {
    if (mod->syms[i].st_shndx == SHN_UNDEF)
      continue;
    if (ELF64_ST_BIND(mod->syms[i].st_info) == STB_LOCAL)
      continue;
    const char *sname = mod->dynstrtab + mod->syms[i].st_name;
    if (sname[0] == name[0] && strcmp(sname, name) == 0)
      return (uintptr_t)mod->load_virtbase + mod->syms[i].st_value;
  }
  return 0;
}

static uintptr_t so_resolve_symbol(so_module *mod, DynLibFunction *funcs, int num_funcs, const char *name) {
  // Prefer Switch shims over exports from other loaded modules.
  for (int k = 0; k < num_funcs; k++) {
    if (strcmp(name, funcs[k].symbol) == 0)
      return funcs[k].func;
  }

  for (so_module *m = so_list; m; m = m->next) {
    if (m == mod)
      continue;
    const uintptr_t addr = so_lookup_export(m, name);
    if (addr)
      return addr;
  }

  return 0;
}

int so_resolve(so_module *mod, DynLibFunction *funcs, int num_funcs, int taint_missing_imports) {
  int missing = 0;
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.dyn") == 0 || strcmp(sh_name, ".rela.plt") == 0) {
      Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      for (int j = 0; j < mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela); j++) {
        uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
        Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];

        int type = ELF64_R_TYPE(rels[j].r_info);
        switch (type) {
          case R_AARCH64_ABS64:
          case R_AARCH64_GLOB_DAT:
          case R_AARCH64_JUMP_SLOT:
          {
            if (sym->st_shndx == SHN_UNDEF) {
              char *name = mod->dynstrtab + sym->st_name;
              uintptr_t addr = so_resolve_symbol(mod, funcs, num_funcs, name);
              if (addr) {
                *ptr = addr + rels[j].r_addend;
              } else if (ELF64_ST_BIND(sym->st_info) == STB_WEAK) {
                // weak undefined (e.g. _ZTH* thread-local inits): the ABI says
                // resolve to NULL; callers null-check before using them
                *ptr = 0;
              } else {
                missing++;
                debugPrintf("%s: unresolved import: %s\n", mod->name, name);
                // Poison unresolved imports when requested.
                if (taint_missing_imports)
                  *ptr = rels[j].r_offset;
              }
            }

            break;
          }

          default:
            break;
        }
      }
    }
  }

  if (missing)
    debugPrintf("%s: %d unresolved imports\n", mod->name, missing);

  return 0;
}

void so_execute_init_array(so_module *mod) {
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".init_array") == 0) {
      int (** init_array)() = (void *)((uintptr_t)mod->load_virtbase + mod->sec_hdr[i].sh_addr);
      for (int j = 0; j < mod->sec_hdr[i].sh_size / 8; j++) {
        if (init_array[j] != 0)
          init_array[j]();
      }
    }
  }
}

uintptr_t so_find_addr(so_module *mod, const char *symbol) {
  for (int i = 0; i < mod->num_syms; i++) {
    char *name = mod->dynstrtab + mod->syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)mod->load_base + mod->syms[i].st_value;
  }

  fatal_error("Error: could not find symbol:\n%s\n", symbol);
  return 0;
}

uintptr_t so_find_addr_rx(so_module *mod, const char *symbol) {
  const uintptr_t addr = so_try_find_addr_rx(mod, symbol);
  if (!addr)
    fatal_error("Error: could not find symbol:\n%s\n", symbol);
  return addr;
}

uintptr_t so_try_find_addr_rx(so_module *mod, const char *symbol) {
  for (int i = 0; i < mod->num_syms; i++) {
    char *name = mod->dynstrtab + mod->syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)mod->load_virtbase + mod->syms[i].st_value;
  }
  return 0;
}

DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs, const char *name) {
  for (int i = 0; i < num_funcs; ++i)
    if (!strcmp(funcs[i].symbol, name))
      return &funcs[i];
  return NULL;
}

int so_unload(so_module *mod) {
  if (mod->load_base == NULL)
    return -1;

  if (mod->so_base) {
    // someone forgot to free the temp data
    so_free_temp(mod);
  }

  if (mod->cm_count > 0) {
    for (int i = 0; i < mod->cm_count; i++) {
      svcControlCodeMemory(mod->cm_handles[i],
                           mod->cm_is_x[i] ? CodeMapOperation_UnmapSlave : CodeMapOperation_UnmapOwner,
                           mod->cm_addrs[i], mod->cm_sizes[i], Perm_None);
      svcCloseHandle(mod->cm_handles[i]);
    }
    mod->cm_count = 0;
  } else {
    // remap everything as RW
    for (int i = 0; i < mod->phnum; i++) {
      const Elf64_Phdr *p = &mod->phdr[i];
      if (p->p_type != PT_LOAD || !(p->p_flags & PF_X))
        continue;
      const u64 seg_start = ((u64)mod->load_virtbase + p->p_vaddr) & ~0xFFFull;
      const u64 seg_end = ALIGN_MEM((u64)mod->load_virtbase + p->p_vaddr + p->p_memsz, 0x1000);
      svcSetProcessMemoryPermission(sts2_own_process_handle(), seg_start, seg_end - seg_start, Perm_Rw);
    }
    // unmap everything
    svcUnmapProcessCodeMemory(sts2_own_process_handle(), (u64)mod->load_virtbase, (u64)mod->load_base, mod->load_size);
  }

  // release virtual address range
  virtmemLock();
  virtmemRemoveReservation(mod->load_memrv);
  virtmemUnlock();

  // remove from list
  if (so_list == mod) {
    so_list = mod->next;
  } else {
    for (so_module *m = so_list; m; m = m->next) {
      if (m->next == mod) {
        m->next = mod->next;
        break;
      }
    }
  }

  return 0;
}

// matches the layout bionic/libunwind expect
struct so_dl_phdr_info {
  Elf64_Addr dlpi_addr;
  const char *dlpi_name;
  const Elf64_Phdr *dlpi_phdr;
  Elf64_Half dlpi_phnum;
};

int so_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data) {
  int ret = 0;
  for (so_module *mod = so_list; mod; mod = mod->next) {
    struct so_dl_phdr_info info;
    info.dlpi_addr = (Elf64_Addr)mod->load_virtbase;
    info.dlpi_name = mod->name;
    info.dlpi_phdr = mod->phdr; // link-time vaddrs + dlpi_addr = runtime
    info.dlpi_phnum = mod->phnum;
    ret = callback(&info, sizeof(info), data);
    if (ret)
      break;
  }
  return ret;
}
