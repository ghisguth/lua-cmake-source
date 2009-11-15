/*
** JIT library.
** Copyright (C) 2005-2009 Mike Pall. See Copyright Notice in luajit.h
*/

#define lib_jit_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_arch.h"
#include "lj_obj.h"
#include "lj_err.h"
#include "lj_str.h"
#include "lj_tab.h"
#if LJ_HASJIT
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_iropt.h"
#endif
#include "lj_dispatch.h"
#include "lj_vm.h"
#include "lj_vmevent.h"
#include "lj_lib.h"

#include "luajit.h"

/* -- jit.* functions ----------------------------------------------------- */

#define LJLIB_MODULE_jit

static int setjitmode(lua_State *L, int mode)
{
  int idx = 0;
  if (L->base == L->top || tvisnil(L->base)) {  /* jit.on/off/flush([nil]) */
    mode |= LUAJIT_MODE_ENGINE;
  } else {
    /* jit.on/off/flush(func|proto, nil|true|false) */
    if (tvisfunc(L->base) || tvisproto(L->base))
      idx = 1;
    else if (!tvistrue(L->base))  /* jit.on/off/flush(true, nil|true|false) */
      goto err;
    if (L->base+1 < L->top && tvisbool(L->base+1))
      mode |= boolV(L->base+1) ? LUAJIT_MODE_ALLFUNC : LUAJIT_MODE_ALLSUBFUNC;
    else
      mode |= LUAJIT_MODE_FUNC;
  }
  if (luaJIT_setmode(L, idx, mode) != 1) {
  err:
#if LJ_HASJIT
    lj_err_arg(L, 1, LJ_ERR_NOLFUNC);
#else
    lj_err_caller(L, LJ_ERR_NOJIT);
#endif
  }
  return 0;
}

LJLIB_CF(jit_on)
{
  return setjitmode(L, LUAJIT_MODE_ON);
}

LJLIB_CF(jit_off)
{
  return setjitmode(L, LUAJIT_MODE_OFF);
}

LJLIB_CF(jit_flush)
{
#if LJ_HASJIT
  if (L->base < L->top && (tvisnum(L->base) || tvisstr(L->base))) {
    int traceno = lj_lib_checkint(L, 1);
    setboolV(L->top-1,
	     luaJIT_setmode(L, traceno, LUAJIT_MODE_FLUSH|LUAJIT_MODE_TRACE));
    return 1;
  }
#endif
  return setjitmode(L, LUAJIT_MODE_FLUSH);
}

#if LJ_HASJIT
/* Push a string for every flag bit that is set. */
static void flagbits_to_strings(lua_State *L, uint32_t flags, uint32_t base,
				const char *str)
{
  for (; *str; base <<= 1, str += 1+*str)
    if (flags & base)
      setstrV(L, L->top++, lj_str_new(L, str+1, *(uint8_t *)str));
}
#endif

LJLIB_CF(jit_status)
{
#if LJ_HASJIT
  jit_State *J = L2J(L);
  L->top = L->base;
  setboolV(L->top++, (J->flags & JIT_F_ON) ? 1 : 0);
  flagbits_to_strings(L, J->flags, JIT_F_CPU_FIRST, JIT_F_CPUSTRING);
  flagbits_to_strings(L, J->flags, JIT_F_OPT_FIRST, JIT_F_OPTSTRING);
  return L->top - L->base;
#else
  setboolV(L->top++, 0);
  return 1;
#endif
}

LJLIB_CF(jit_attach)
{
#ifdef LUAJIT_DISABLE_VMEVENT
  luaL_error(L, "vmevent API disabled");
#else
  GCfunc *fn = lj_lib_checkfunc(L, 1);
  GCstr *s = lj_lib_optstr(L, 2);
  luaL_findtable(L, LUA_REGISTRYINDEX, LJ_VMEVENTS_REGKEY, LJ_VMEVENTS_HSIZE);
  if (s) {  /* Attach to given event. */
    lua_pushvalue(L, 1);
    lua_rawseti(L, -2, VMEVENT_HASHIDX(s->hash));
    G(L)->vmevmask = VMEVENT_NOCACHE;  /* Invalidate cache. */
  } else {  /* Detach if no event given. */
    setnilV(L->top++);
    while (lua_next(L, -2)) {
      L->top--;
      if (tvisfunc(L->top) && funcV(L->top) == fn) {
	setnilV(lj_tab_set(L, tabV(L->top-2), L->top-1));
      }
    }
  }
#endif
  return 0;
}

LJLIB_PUSH(top-4) LJLIB_SET(arch)
LJLIB_PUSH(top-3) LJLIB_SET(version_num)
LJLIB_PUSH(top-2) LJLIB_SET(version)

#include "lj_libdef.h"

/* -- jit.util.* functions ------------------------------------------------ */

#define LJLIB_MODULE_jit_util

/* -- Reflection API for Lua functions ------------------------------------ */

/* Return prototype of first argument (Lua function or prototype object) */
static GCproto *check_Lproto(lua_State *L, int nolua)
{
  TValue *o = L->base;
  if (L->top > o) {
    if (tvisproto(o)) {
      return protoV(o);
    } else if (tvisfunc(o)) {
      if (isluafunc(funcV(o)))
	return funcproto(funcV(o));
      else if (nolua)
	return NULL;
    }
  }
  lj_err_argt(L, 1, LUA_TFUNCTION);
  return NULL;  /* unreachable */
}

static void setintfield(lua_State *L, GCtab *t, const char *name, int32_t val)
{
  setintV(lj_tab_setstr(L, t, lj_str_newz(L, name)), val);
}

/* local info = jit.util.funcinfo(func [,pc]) */
LJLIB_CF(jit_util_funcinfo)
{
  GCproto *pt = check_Lproto(L, 1);
  if (pt) {
    BCPos pc = (BCPos)lj_lib_optint(L, 2, 0);
    GCtab *t;
    lua_createtable(L, 0, 16);  /* Increment hash size if fields are added. */
    t = tabV(L->top-1);
    setintfield(L, t, "linedefined", pt->linedefined);
    setintfield(L, t, "lastlinedefined", pt->lastlinedefined);
    setintfield(L, t, "stackslots", pt->framesize);
    setintfield(L, t, "params", pt->numparams);
    setintfield(L, t, "bytecodes", (int32_t)pt->sizebc);
    setintfield(L, t, "gcconsts", (int32_t)pt->sizekgc);
    setintfield(L, t, "nconsts", (int32_t)pt->sizekn);
    setintfield(L, t, "upvalues", (int32_t)pt->sizeuv);
    if (pc > 0)
      setintfield(L, t, "currentline", pt->lineinfo ? pt->lineinfo[pc-1] : 0);
    lua_pushboolean(L, (pt->flags & PROTO_IS_VARARG));
    lua_setfield(L, -2, "isvararg");
    setstrV(L, L->top++, pt->chunkname);
    lua_setfield(L, -2, "source");
    lj_err_pushloc(L, pt, pc);
    lua_setfield(L, -2, "loc");
  } else {
    GCfunc *fn = funcV(L->base);
    GCtab *t;
    lua_createtable(L, 0, 2);  /* Increment hash size if fields are added. */
    t = tabV(L->top-1);
    setintfield(L, t, "ffid", fn->c.ffid);
    setintfield(L, t, "upvalues", fn->c.nupvalues);
  }
  return 1;
}

/* local ins, m = jit.util.funcbc(func, pc) */
LJLIB_CF(jit_util_funcbc)
{
  GCproto *pt = check_Lproto(L, 0);
  BCPos pc = (BCPos)lj_lib_checkint(L, 2) - 1;
  if (pc < pt->sizebc) {
    BCIns ins = pt->bc[pc];
    BCOp op = bc_op(ins);
    lua_assert(op < BC__MAX);
    setintV(L->top, ins);
    setintV(L->top+1, lj_bc_mode[op]);
    L->top += 2;
    return 2;
  }
  return 0;
}

/* local k = jit.util.funck(func, idx) */
LJLIB_CF(jit_util_funck)
{
  GCproto *pt = check_Lproto(L, 0);
  MSize idx = (MSize)lj_lib_checkint(L, 2);
  if ((int32_t)idx >= 0) {
    if (idx < pt->sizekn) {
      setnumV(L->top-1, pt->k.n[idx]);
      return 1;
    }
  } else {
    if (~idx < pt->sizekgc) {
      GCobj *gc = gcref(pt->k.gc[idx]);
      setgcV(L, L->top-1, &gc->gch, ~gc->gch.gct);
      return 1;
    }
  }
  return 0;
}

/* local name = jit.util.funcuvname(func, idx) */
LJLIB_CF(jit_util_funcuvname)
{
  GCproto *pt = check_Lproto(L, 0);
  uint32_t idx = (uint32_t)lj_lib_checkint(L, 2);
  if (idx < pt->sizeuvname) {
    setstrV(L, L->top-1, pt->uvname[idx]);
    return 1;
  }
  return 0;
}

/* -- Reflection API for traces ------------------------------------------- */

#if LJ_HASJIT

/* Check trace argument. Must not throw for non-existent trace numbers. */
static Trace *jit_checktrace(lua_State *L)
{
  TraceNo tr = (TraceNo)lj_lib_checkint(L, 1);
  jit_State *J = L2J(L);
  if (tr > 0 && tr < J->sizetrace)
    return J->trace[tr];
  return NULL;
}

/* local info = jit.util.traceinfo(tr) */
LJLIB_CF(jit_util_traceinfo)
{
  Trace *T = jit_checktrace(L);
  if (T) {
    GCtab *t;
    lua_createtable(L, 0, 4);  /* Increment hash size if fields are added. */
    t = tabV(L->top-1);
    setintfield(L, t, "nins", (int32_t)T->nins - REF_BIAS - 1);
    setintfield(L, t, "nk", REF_BIAS - (int32_t)T->nk);
    setintfield(L, t, "link", T->link);
    setintfield(L, t, "nexit", T->nsnap);
    /* There are many more fields. Add them only when needed. */
    return 1;
  }
  return 0;
}

/* local m, ot, op1, op2, prev = jit.util.traceir(tr, idx) */
LJLIB_CF(jit_util_traceir)
{
  Trace *T = jit_checktrace(L);
  IRRef ref = (IRRef)lj_lib_checkint(L, 2) + REF_BIAS;
  if (T && ref >= REF_BIAS && ref < T->nins) {
    IRIns *ir = &T->ir[ref];
    int32_t m = lj_ir_mode[ir->o];
    setintV(L->top-2, m);
    setintV(L->top-1, ir->ot);
    setintV(L->top++, (int32_t)ir->op1 - (irm_op1(m)==IRMref ? REF_BIAS : 0));
    setintV(L->top++, (int32_t)ir->op2 - (irm_op2(m)==IRMref ? REF_BIAS : 0));
    setintV(L->top++, ir->prev);
    return 5;
  }
  return 0;
}

/* local k, t [, slot] = jit.util.tracek(tr, idx) */
LJLIB_CF(jit_util_tracek)
{
  Trace *T = jit_checktrace(L);
  IRRef ref = (IRRef)lj_lib_checkint(L, 2) + REF_BIAS;
  if (T && ref >= T->nk && ref < REF_BIAS) {
    IRIns *ir = &T->ir[ref];
    int32_t slot = -1;
    if (ir->o == IR_KSLOT) {
      slot = ir->op2;
      ir = &T->ir[ir->op1];
    }
    lj_ir_kvalue(L, L->top-2, ir);
    setintV(L->top-1, (int32_t)irt_type(ir->t));
    if (slot == -1)
      return 2;
    setintV(L->top++, slot);
    return 3;
  }
  return 0;
}

/* local snap = jit.util.tracesnap(tr, sn) */
LJLIB_CF(jit_util_tracesnap)
{
  Trace *T = jit_checktrace(L);
  SnapNo sn = (SnapNo)lj_lib_checkint(L, 2);
  if (T && sn < T->nsnap) {
    SnapShot *snap = &T->snap[sn];
    IRRef2 *map = &T->snapmap[snap->mapofs];
    BCReg s, nslots = snap->nslots;
    GCtab *t;
    lua_createtable(L, nslots ? (int)nslots : 1, 0);
    t = tabV(L->top-1);
    setintV(lj_tab_setint(L, t, 0), (int32_t)snap->ref - REF_BIAS);
    for (s = 0; s < nslots; s++) {
      TValue *o = lj_tab_setint(L, t, (int32_t)(s+1));
      IRRef ref = snap_ref(map[s]);
      if (ref)
	setintV(o, (int32_t)ref - REF_BIAS);
      else
	setboolV(o, 0);
    }
    return 1;
  }
  return 0;
}

/* local mcode, addr, loop = jit.util.tracemc(tr) */
LJLIB_CF(jit_util_tracemc)
{
  Trace *T = jit_checktrace(L);
  if (T && T->mcode != NULL) {
    setstrV(L, L->top-1, lj_str_new(L, (const char *)T->mcode, T->szmcode));
    setnumV(L->top++, cast_num((intptr_t)T->mcode));
    setintV(L->top++, T->mcloop);
    return 3;
  }
  return 0;
}

/* local addr = jit.util.traceexitstub(idx) */
LJLIB_CF(jit_util_traceexitstub)
{
  ExitNo exitno = (ExitNo)lj_lib_checkint(L, 1);
  jit_State *J = L2J(L);
  if (exitno < EXITSTUBS_PER_GROUP*LJ_MAX_EXITSTUBGR) {
    setnumV(L->top-1, cast_num((intptr_t)exitstub_addr(J, exitno)));
    return 1;
  }
  return 0;
}

#else

static int trace_nojit(lua_State *L)
{
  UNUSED(L);
  return 0;
}
#define lj_cf_jit_util_traceinfo	trace_nojit
#define lj_cf_jit_util_traceir		trace_nojit
#define lj_cf_jit_util_tracek		trace_nojit
#define lj_cf_jit_util_tracesnap	trace_nojit
#define lj_cf_jit_util_tracemc		trace_nojit
#define lj_cf_jit_util_traceexitstub	trace_nojit

#endif

#include "lj_libdef.h"

/* -- jit.opt module ------------------------------------------------------ */

#define LJLIB_MODULE_jit_opt

#if LJ_HASJIT
/* Parse optimization level. */
static int jitopt_level(jit_State *J, const char *str)
{
  if (str[0] >= '0' && str[0] <= '9' && str[1] == '\0') {
    uint32_t flags;
    if (str[0] == '0') flags = JIT_F_OPT_0;
    else if (str[0] == '1') flags = JIT_F_OPT_1;
    else if (str[0] == '2') flags = JIT_F_OPT_2;
    else flags = JIT_F_OPT_3;
    J->flags = (J->flags & ~JIT_F_OPT_MASK) | flags;
    return 1;  /* Ok. */
  }
  return 0;  /* No match. */
}

/* Parse optimization flag. */
static int jitopt_flag(jit_State *J, const char *str)
{
  const char *lst = JIT_F_OPTSTRING;
  uint32_t opt;
  int set = 1;
  if (str[0] == '+') {
    str++;
  } else if (str[0] == '-') {
    str++;
    set = 0;
  } else if (str[0] == 'n' && str[1] == 'o') {
    str += str[2] == '-' ? 3 : 2;
    set = 0;
  }
  for (opt = JIT_F_OPT_FIRST; ; opt <<= 1) {
    size_t len = *(const uint8_t *)lst;
    if (len == 0)
      break;
    if (strncmp(str, lst+1, len) == 0 && str[len] == '\0') {
      if (set) J->flags |= opt; else J->flags &= ~opt;
      return 1;  /* Ok. */
    }
    lst += 1+len;
  }
  return 0;  /* No match. */
}

/* Forward declaration. */
static void jit_init_hotcount(jit_State *J);

/* Parse optimization parameter. */
static int jitopt_param(jit_State *J, const char *str)
{
  const char *lst = JIT_P_STRING;
  int i;
  for (i = 0; i < JIT_P__MAX; i++) {
    size_t len = *(const uint8_t *)lst;
    TValue tv;
    lua_assert(len != 0);
    if (strncmp(str, lst+1, len) == 0 && str[len] == '=' &&
	lj_str_numconv(&str[len+1], &tv)) {
      J->param[i] = lj_num2int(tv.n);
      if (i == JIT_P_hotloop)
	jit_init_hotcount(J);
      return 1;  /* Ok. */
    }
    lst += 1+len;
  }
  return 0;  /* No match. */
}
#endif

/* jit.opt.start(flags...) */
LJLIB_CF(jit_opt_start)
{
#if LJ_HASJIT
  jit_State *J = L2J(L);
  int nargs = (int)(L->top - L->base);
  if (nargs == 0) {
    J->flags = (J->flags & ~JIT_F_OPT_MASK) | JIT_F_OPT_DEFAULT;
  } else {
    int i;
    for (i = 1; i <= nargs; i++) {
      const char *str = strdata(lj_lib_checkstr(L, i));
      if (!jitopt_level(J, str) &&
	  !jitopt_flag(J, str) &&
	  !jitopt_param(J, str))
	lj_err_callerv(L, LJ_ERR_JITOPT, str);
    }
  }
#else
  lj_err_caller(L, LJ_ERR_NOJIT);
#endif
  return 0;
}

#include "lj_libdef.h"

/* -- JIT compiler initialization ----------------------------------------- */

#if LJ_HASJIT
/* Default values for JIT parameters. */
static const int32_t jit_param_default[JIT_P__MAX+1] = {
#define JIT_PARAMINIT(len, name, value)	(value),
JIT_PARAMDEF(JIT_PARAMINIT)
#undef JIT_PARAMINIT
  0
};

/* Initialize hotcount table. */
static void jit_init_hotcount(jit_State *J)
{
  HotCount start = (HotCount)J->param[JIT_P_hotloop];
  HotCount *hotcount = J2GG(J)->hotcount;
  uint32_t i;
  for (i = 0; i < HOTCOUNT_SIZE; i++)
    hotcount[i] = start;
}
#endif

/* Arch-dependent CPU detection. */
static uint32_t jit_cpudetect(lua_State *L)
{
  uint32_t flags = 0;
#if LJ_TARGET_X86ORX64
  uint32_t vendor[4];
  uint32_t features[4];
  if (lj_vm_cpuid(0, vendor) && lj_vm_cpuid(1, features)) {
#if !LJ_HASJIT
#define JIT_F_CMOV	1
#endif
    flags |= ((features[3] >> 15)&1) * JIT_F_CMOV;
#if LJ_HASJIT
    flags |= ((features[3] >> 26)&1) * JIT_F_SSE2;
    flags |= ((features[2] >> 19)&1) * JIT_F_SSE4_1;
    if (vendor[2] == 0x6c65746e) {  /* Intel. */
      if ((features[0] & 0x0ff00f00) == 0x00000f00)  /* P4. */
	flags |= JIT_F_P4;  /* Currently unused. */
      else if ((features[0] & 0x0fff0ff0) == 0x000106c0)  /* Atom. */
	flags |= JIT_F_LEA_AGU;
    } else if (vendor[2] == 0x444d4163) {  /* AMD. */
      uint32_t fam = (features[0] & 0x0ff00f00);
      if (fam == 0x00000f00)  /* K8. */
	flags |= JIT_F_SPLIT_XMM;
      if (fam >= 0x00000f00)  /* K8, K10. */
	flags |= JIT_F_PREFER_IMUL;
    }
#endif
  }
#ifndef LUAJIT_CPU_NOCMOV
  if (!(flags & JIT_F_CMOV))
    luaL_error(L, "Ancient CPU lacks CMOV support (recompile with -DLUAJIT_CPU_NOCMOV)");
#endif
#if LJ_HASJIT
  if (!(flags & JIT_F_SSE2))
    luaL_error(L, "Sorry, SSE2 CPU support required for this beta release");
#endif
  UNUSED(L);
#else
#error "Missing CPU detection for this architecture"
#endif
  return flags;
}

/* Initialize JIT compiler. */
static void jit_init(lua_State *L)
{
  uint32_t flags = jit_cpudetect(L);
#if LJ_HASJIT
  jit_State *J = L2J(L);
  J->flags = flags | JIT_F_ON | JIT_F_OPT_DEFAULT;
  memcpy(J->param, jit_param_default, sizeof(J->param));
  jit_init_hotcount(J);
  lj_dispatch_update(G(L));
#else
  UNUSED(flags);
#endif
}

LUALIB_API int luaopen_jit(lua_State *L)
{
  lua_pushliteral(L, LJ_ARCH_NAME);
  lua_pushinteger(L, LUAJIT_VERSION_NUM);
  lua_pushliteral(L, LUAJIT_VERSION);
  LJ_LIB_REG(L, jit);
#ifndef LUAJIT_DISABLE_JITUTIL
  LJ_LIB_REG_(L, "jit.util", jit_util);
#endif
  LJ_LIB_REG_(L, "jit.opt", jit_opt);
  L->top -= 2;
  jit_init(L);
  return 1;
}

