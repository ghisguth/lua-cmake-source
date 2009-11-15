/*
** Machine code management.
** Copyright (C) 2005-2009 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_mcode_c
#define LUA_CORE

#include "lj_obj.h"

#if LJ_HASJIT

#include "lj_gc.h"
#include "lj_jit.h"
#include "lj_mcode.h"
#include "lj_trace.h"
#include "lj_dispatch.h"

/* -- OS-specific functions ----------------------------------------------- */

#if defined(LUA_USE_WIN)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define MCPROT_RW	PAGE_READWRITE
#define MCPROT_RX	PAGE_EXECUTE_READ
#define MCPROT_RWX	PAGE_EXECUTE_READWRITE

static LJ_AINLINE void *mcode_alloc(jit_State *J, size_t sz, DWORD prot)
{
  void *p = VirtualAlloc(NULL, sz, MEM_RESERVE|MEM_COMMIT|MEM_TOP_DOWN, prot);
  if (!p)
    lj_trace_err(J, LJ_TRERR_MCODEAL);
  return p;
}

static LJ_AINLINE void mcode_free(jit_State *J, void *p, size_t sz)
{
  UNUSED(J); UNUSED(sz);
  VirtualFree(p, 0, MEM_RELEASE);
}

static LJ_AINLINE void mcode_setprot(void *p, size_t sz, DWORD prot)
{
  DWORD oprot;
  VirtualProtect(p, sz, prot, &oprot);
}

#elif defined(LUA_USE_POSIX)

#include <sys/mman.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS	MAP_ANON
#endif

#define MCPROT_RW	(PROT_READ|PROT_WRITE)
#define MCPROT_RX	(PROT_READ|PROT_EXEC)
#define MCPROT_RWX	(PROT_READ|PROT_WRITE|PROT_EXEC)

static LJ_AINLINE void *mcode_alloc(jit_State *J, size_t sz, int prot)
{
  void *p = mmap(NULL, sz, prot, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    lj_trace_err(J, LJ_TRERR_MCODEAL);
  return p;
}

static LJ_AINLINE void mcode_free(jit_State *J, void *p, size_t sz)
{
  UNUSED(J);
  munmap(p, sz);
}

static LJ_AINLINE void mcode_setprot(void *p, size_t sz, int prot)
{
  mprotect(p, sz, prot);
}

#else

/* Fallback allocator. This will fail if memory is not executable by default. */
#define LUAJIT_UNPROTECT_MCODE
#define MCPROT_RW	0
#define MCPROT_RX	0
#define MCPROT_RWX	0

static LJ_AINLINE void *mcode_alloc(jit_State *J, size_t sz, int prot)
{
  UNUSED(prot);
  return lj_mem_new(J->L, sz);
}

static LJ_AINLINE void mcode_free(jit_State *J, void *p, size_t sz)
{
  lj_mem_free(J2G(J), p, sz);
}

#define mcode_setprot(p, sz, prot)	UNUSED(p)

#endif

/* -- MCode area management ----------------------------------------------- */

/* Define this ONLY if the page protection twiddling becomes a bottleneck. */
#ifdef LUAJIT_UNPROTECT_MCODE

/* It's generally considered to be a potential security risk to have
** pages with simultaneous write *and* execute access in a process.
**
** Do not even think about using this mode for server processes or
** apps handling untrusted external data (such as a browser).
**
** The security risk is not in LuaJIT itself -- but if an adversary finds
** any *other* flaw in your C application logic, then any RWX memory page
** simplifies writing an exploit considerably.
*/
#define MCPROT_GEN	MCPROT_RWX
#define MCPROT_RUN	MCPROT_RWX

#else

/* This is the default behaviour and much safer:
**
** Most of the time the memory pages holding machine code are executable,
** but NONE of them is writable.
**
** The current memory area is marked read-write (but NOT executable) only
** during the short time window while the assembler generates machine code.
*/
#define MCPROT_GEN	MCPROT_RW
#define MCPROT_RUN	MCPROT_RX

#endif

/* Change protection of MCode area. */
static void mcode_protect(jit_State *J, int prot)
{
#ifdef LUAJIT_UNPROTECT_MCODE
  UNUSED(J); UNUSED(prot);
#else
  if (J->mcprot != prot) {
    mcode_setprot(J->mcarea, J->szmcarea, prot);
    J->mcprot = prot;
  }
#endif
}

/* Linked list of MCode areas. */
typedef struct MCLink {
  MCode *next;		/* Next area. */
  size_t size;		/* Size of current area. */
} MCLink;

/* Allocate a new MCode area. */
static void mcode_allocarea(jit_State *J)
{
  MCode *oldarea = J->mcarea;
  size_t sz = (size_t)J->param[JIT_P_sizemcode] << 10;
  sz = (sz + LJ_PAGESIZE-1) & ~(size_t)(LJ_PAGESIZE - 1);
  J->mcarea = (MCode *)mcode_alloc(J, sz, MCPROT_GEN);
  J->szmcarea = sz;
  J->mcprot = MCPROT_GEN;
  J->mctop = (MCode *)((char *)J->mcarea + J->szmcarea);
  J->mcbot = (MCode *)((char *)J->mcarea + sizeof(MCLink));
  ((MCLink *)J->mcarea)->next = oldarea;
  ((MCLink *)J->mcarea)->size = sz;
  J->szallmcarea += sz;
}

/* Free all MCode areas. */
void lj_mcode_free(jit_State *J)
{
  MCode *mc = J->mcarea;
  J->mcarea = NULL;
  J->szallmcarea = 0;
  while (mc) {
    MCode *next = ((MCLink *)mc)->next;
    mcode_free(J, mc, ((MCLink *)mc)->size);
    mc = next;
  }
}

/* -- MCode transactions -------------------------------------------------- */

/* Reserve the remainder of the current MCode area. */
MCode *lj_mcode_reserve(jit_State *J, MCode **lim)
{
  if (!J->mcarea)
    mcode_allocarea(J);
  else
    mcode_protect(J, MCPROT_GEN);
  *lim = J->mcbot;
  return J->mctop;
}

/* Commit the top part of the current MCode area. */
void lj_mcode_commit(jit_State *J, MCode *top)
{
  J->mctop = top;
  mcode_protect(J, MCPROT_RUN);
}

/* Abort the reservation. */
void lj_mcode_abort(jit_State *J)
{
  mcode_protect(J, MCPROT_RUN);
}

/* Set/reset protection to allow patching of MCode areas. */
MCode *lj_mcode_patch(jit_State *J, MCode *ptr, int finish)
{
#ifdef LUAJIT_UNPROTECT_MCODE
  UNUSED(J); UNUSED(ptr); UNUSED(finish);
  return NULL;
#else
  if (finish) {
    if (J->mcarea == ptr)
      mcode_protect(J, MCPROT_RUN);
    else
      mcode_setprot(ptr, ((MCLink *)ptr)->size, MCPROT_RUN);
    return NULL;
  } else {
    MCode *mc = J->mcarea;
    /* Try current area first to use the protection cache. */
    if (ptr >= mc && ptr < mc + J->szmcarea) {
      mcode_protect(J, MCPROT_GEN);
      return mc;
    }
    /* Otherwise search through the list of MCode areas. */
    for (;;) {
      mc = ((MCLink *)mc)->next;
      lua_assert(mc != NULL);
      if (ptr >= mc && ptr < mc + ((MCLink *)mc)->size) {
	mcode_setprot(mc, ((MCLink *)mc)->size, MCPROT_GEN);
	return mc;
      }
    }
  }
#endif
}

/* Limit of MCode reservation reached. */
void lj_mcode_limiterr(jit_State *J, size_t need)
{
  size_t sizemcode, maxmcode;
  lj_mcode_abort(J);
  sizemcode = (size_t)J->param[JIT_P_sizemcode] << 10;
  sizemcode = (sizemcode + LJ_PAGESIZE-1) & ~(size_t)(LJ_PAGESIZE - 1);
  maxmcode = (size_t)J->param[JIT_P_maxmcode] << 10;
  if ((size_t)need > sizemcode)
    lj_trace_err(J, LJ_TRERR_MCODEOV);  /* Too long for any area. */
  if (J->szallmcarea + sizemcode > maxmcode)
    lj_trace_err(J, LJ_TRERR_MCODEAL);
  mcode_allocarea(J);
  lj_trace_err(J, LJ_TRERR_MCODELM);  /* Retry with new area. */
}

#endif
